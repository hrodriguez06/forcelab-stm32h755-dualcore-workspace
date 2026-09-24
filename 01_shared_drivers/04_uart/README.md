# STM32H755 Shared UART Driver (`01_shared_drivers/04_uart`)

Peripheral-First (Plan A), 3-tier shared UART driver for the STM32H755ZI dual-core
microcontroller: USART1, USART2, USART3, UART4, UART5, USART6, UART7, UART8 and
LPUART1. Asynchronous mode only.

Status: the BM and LL tiers and the HAL console were validated on a
NUCLEO-H755ZI-Q (silicon Rev V, `REV_ID = 0x2003`) on 2026-09-22 with a PD5-PD6
loopback. What that run covers, and what it does not, is in
[section 9](#9-validation).

---

## 1. Tiers

| Tier | Files | CMake target | Links | ST dependency | Typical use |
| :--- | :--- | :--- | :--- | :--- | :--- |
| **Bare-Metal** | `uart_bm.[c\|h]` | `stm32h7_driver_uart_bm` | `stm32h7_driver_gpio` | CMSIS only (`stm32h7xx.h`) | Console before `SystemClock_Config()`, fault handlers, Cortex-M4 without HAL UART |
| **Low-Level** | `uart_ll.[c\|h]` | `stm32h7_driver_uart_ll` | `stm32h7_driver_gpio` | `stm32h7xx_ll_usart.h`, `stm32h7xx_ll_lpuart.h` inlines | Same as BM, plus interrupt-driven RX into a ring buffer |
| **HAL** | `uart_hal.[c\|h]` | `stm32h7_driver_uart` | `stm32h7_driver_gpio` | ST HAL (`HAL_UART_*`) | The ST-LINK VCP console of CPU1, FreeRTOS / lwIP projects |

```cmake
target_link_libraries(cm4_firmware PRIVATE stm32h7_driver_uart_bm)   # or _ll, or stm32h7_driver_uart
```

```c
#include "uart_bm.h"    /* never "04_uart/uart_bm.h": the directory is on the include path */
```

- **BM and LL need nothing from the HAL.** They compile with
  `HAL_UART_MODULE_ENABLED` undefined, which is how every CM4 image in this
  workspace is configured (the console belongs to CPU1). LL uses only the
  `static inline` functions of the LL headers: no LL `.c` file has to be
  compiled and `USE_FULL_LL_DRIVER` is not required.
- **The HAL tier needs `HAL_UART_MODULE_ENABLED`** in the project's
  `stm32h7xx_hal_conf.h`. Without it `uart_hal.h` and `uart_hal.c` compile to
  nothing.
- `stm32h7_driver_uart` (HAL) is part of the `stm32h7_shared_drivers` umbrella
  target; `_bm` and `_ll` are not, and have to be linked by name.

### 1.1 Feature matrix

| Feature | BM | LL | HAL |
| :--- | :---: | :---: | :---: |
| USART1..8 and LPUART1 | yes | yes | yes |
| Baud rate from an injected kernel clock, prescaler search, ±2 % check | yes | yes | no (HAL computes it, `PRESC` fixed at /1) |
| Oversampling by 16 or 8 (USART only) | yes | yes | yes |
| 7 / 8 / 9-bit words, parity, stop bits | yes | yes | yes |
| RTS / CTS hardware flow control | yes | yes | yes |
| Hardware FIFO (`FIFOEN`) | yes | yes | no |
| Optional pin muxing and bus clock enable | yes | yes | yes (`InitEx`) |
| Polling TX / RX without SysTick, bounded by a spin count | yes | yes | no (HAL tick) |
| Interrupt-driven RX into a ring buffer | no | yes | via HAL callbacks (application) |
| Error capture: ORE, FE, NE, PE | yes | yes | HAL `ErrorCode` |
| ES0445 DMA workaround check (bit 20 of `DMA_SxCR`) | - | - | yes (`UART_HAL_CheckDmaErrata`) |
| Board console from `board_def.h` | - | - | yes (`UART_Console_*`) |

The same function names exist in BM and LL (`Init`, `Deinit`, `WriteByte`,
`Write`, `ReadByte`, `Flush`, `TakeErrors`), so moving a module from one tier to
the other only changes the prefix and the configuration constants.

---

## 2. Division of Work: Driver vs Application

Nothing about the board is hardcoded in the driver. Instance, pins, alternate
function and kernel clock frequency all come from the caller.

| Step | BM / LL | HAL |
| :--- | :--- | :--- |
| Kernel clock source (`RCC_D2CCIP2R.USART16SEL`, `USART234578SEL`, `RCC_D3CCIPR.LPUART1SEL`) | **application (CPU1)** | **application (CPU1)** |
| Tell the driver the kernel clock frequency | application, `kernel_clock_hz` | not needed, HAL reads RCC |
| Bus clock enable (`APB1LENR` / `APB2ENR` / `APB4ENR`) | driver if `enable_clock = true` | driver if `enable_clock = true` (`InitEx`), MSP hook otherwise |
| GPIO alternate function | driver if `tx_pin` / `rx_pin` != `NULL` | driver if `tx_pin` / `rx_pin` != `NULL` (`InitEx`), MSP hook otherwise |
| `PRESC`, `BRR`, `CR1..CR3`, `UE`, wait for `TEACK` / `REACK` | driver | HAL (`HAL_UART_Init`) |
| NVIC priority and enable | driver (`UART_LL_EnableRxIrq`) | **application** |
| IRQ vector (`USARTx_IRQHandler`) | **application**, forwarding to `UART_LL_IrqService` | **application**, forwarding to `HAL_UART_IRQHandler` |
| DMA stream configuration | not provided | **application** (`HAL_DMA_Init`), then `UART_HAL_CheckDmaErrata` |
| Completion callbacks (`HAL_UART_TxCpltCallback`, `HAL_UARTEx_RxEventCallback`, ...) | - | **application**. The driver defines none. |

### 2.1 The kernel clock: what to pass in `kernel_clock_hz`

BM and LL compute `BRR` from `kernel_clock_hz`. They never read or write the
kernel clock mux: those fields sit in `RCC_D2CCIP2R` / `RCC_D3CCIPR`, below
offset `0xD0`, which both cores share. Picking the source is a system decision
that belongs to CPU1's clock initialisation.

| Instance | Mux field | Default after reset (RM0399) | Value right after reset | In prj_04 after `SystemClock_Config()` |
| :--- | :--- | :--- | :--- | :--- |
| USART1, USART6 | `USART16SEL` | `rcc_pclk2` | 64 MHz (HSI) | 100 MHz |
| USART2/3, UART4/5/7/8 | `USART234578SEL` | `rcc_pclk1` | 64 MHz (HSI) | 100 MHz |
| LPUART1 | `LPUART1SEL` | `rcc_pclk_d3` (pclk4) | 64 MHz (HSI) | 100 MHz |

A wrong `kernel_clock_hz` is not detected: the driver checks the baud rate it
*computes*, not the one on the wire. If the clock tree changes after `Init`,
call `Init` again with the new frequency.

---

## 3. Board Contract (`board_def.h`)

The HAL console reads its instance and pins from the project's `board_def.h`.
Every project that compiles `uart_hal.c` with `HAL_UART_MODULE_ENABLED` must
define all seven macros; a missing one is a compile-time `#error`, not a silent
fallback to USART3. The four projects that use the driver today (prj_02,
prj_03, prj_04 and the dual-core template) define the Nucleo-144 ST-LINK VCP.
`prj_eth_lwip_node` carries its own copy in `Custom_Drivers/` and is not bound
by this contract.

```c
/* ---- Console UART (Nucleo-144 ST-LINK VCP) ---- */
#define BOARD_CONSOLE_UART_INSTANCE    USART3
#define BOARD_CONSOLE_TX_PORT          GPIOD            /* PD8 - USART3_TX */
#define BOARD_CONSOLE_TX_PIN           GPIO_PIN_8       /* HAL pin MASK    */
#define BOARD_CONSOLE_TX_AF            GPIO_AF7_USART3
#define BOARD_CONSOLE_RX_PORT          GPIOD            /* PD9 - USART3_RX */
#define BOARD_CONSOLE_RX_PIN           GPIO_PIN_9       /* HAL pin MASK    */
#define BOARD_CONSOLE_RX_AF            GPIO_AF7_USART3
```

> **Pin mask vs pin index.** `BOARD_CONSOLE_*_PIN` is a HAL mask (`GPIO_PIN_8`
> = `0x0100`), because the default MSP hands it to `HAL_GPIO_Init()`. Every
> `*_Pin_t` structure of the driver (`UartBm_Pin_t`, `UartLL_Pin_t`,
> `uart_hal_pin_t`) takes a pin **index** (`8`). Passing `GPIO_PIN_8` to
> `pin_idx` makes `Init` return `false`.

### 3.1 Overriding the console MSP

`uart_hal.c` defines `HAL_UART_MspInit()` / `HAL_UART_MspDeInit()` and forwards
them to two weak hooks. The default hook clocks the console instance and muxes
its pins from the macros above; any other instance is left alone.

| Situation | What to do |
| :--- | :--- |
| One console, different wiring | Change the macros in `board_def.h`. |
| One console, special pin setup (open drain, no pull-up, ...) | Define a strong `UART_HAL_MspInit_User()` / `UART_HAL_MspDeInit_User()` in the application. |
| Several UARTs handled by a CubeMX `stm32h7xx_hal_msp.c` | Define `UART_HAL_CUSTOM_MSP`. `uart_hal.c` then defines no `HAL_UART_MspInit`, so there is no duplicate symbol. The seven macros are still required. |
| Extra UARTs next to the console | Use `UART_HAL_InitEx()` with `enable_clock` and pins; it does the clock and the pins itself, before `HAL_UART_Init()`. |

---

## 4. API Reference

### 4.1 BM and LL

| Function | Blocking | Returns | Notes |
| :--- | :--- | :--- | :--- |
| `Init(h, cfg)` | up to 100 000 spins for `TEACK` / `REACK` | `false` on a bad parameter, an unknown instance, a baud rate that cannot be reached within ±2 %, or no acknowledge | Every rejection happens **before** the USART registers are touched, so a refused call leaves a running UART as it was. The optional pins and bus clock are set up first, though. BM checks every framing constant; LL checks only the LPUART stop bits and hands the rest to the LL setters as given. |
| `Deinit(h)` | no | `false` if `h` is not initialised | Clears `UE`, `CR1..CR3`. Leaves the bus clock and the pins as they are. |
| `WriteByte(h, b)` | no | `false` if `TXE` / `TXFNF` is not set | |
| `Write(h, d, n, spin_limit)` | yes, at most `spin_limit` spins **per byte** | bytes written | A short count means a timeout. |
| `ReadByte(h, b)` | no | `false` if nothing was received | Clears ORE / FE / NE / PE first and records them in `h->error_flags`. |
| `Flush(h, spin_limit)` | yes | `false` on timeout | Waits for `TC`: the last stop bit has left the pin. Call it before `Deinit`, a baud change or entering a low-power mode. |
| `TakeErrors(h)` | no | `UART_*_ERR_*` mask | Returns and clears the accumulated flags inside a `PRIMASK` critical section, so it is safe against `UART_LL_IrqService` running on the same core. |
| `UART_LL_EnableRxIrq(h, irqn, prio)` | no | `false` if `h` is not initialised | Enables `RXNEIE` / `RXFNEIE` and `EIE`, sets the NVIC priority and enables the IRQ. |
| `UART_LL_DisableRxIrq(h, irqn)` | no | - | The reverse. |
| `UART_LL_IrqService(h, ring)` | no | - | Call it from the IRQ vector. Records errors, then drains `RDR` / the RX FIFO into `ring`. Bytes that do not fit are dropped. |

**`spin_limit` counts loop iterations, not time.** One iteration is a register
read and a compare: a few core cycles, shorter on the M7 than on the M4. One
character (10 bits) takes `10 / baud` seconds, so a safe per-byte limit is

```text
spin_limit ≈ 20 × f_core / (4 × baud)        e.g. 200 MHz, 9600 baud → ~104 000
```

Pass `0` and `Write` returns `0` without sending anything.

### 4.2 Word length and parity: BM differs from LL and HAL

| You want on the wire | BM (`data_bits`, `parity`) | LL (`data_bits`, `parity`) | HAL (`word_length`, `parity`) |
| :--- | :--- | :--- | :--- |
| 8N1 | `UART_BM_DATA_BITS_8`, `PARITY_NONE` | `LL_USART_DATAWIDTH_8B`, `PARITY_NONE` | `UART_WORDLENGTH_8B`, `PARITY_NONE` |
| 8E1 | `UART_BM_DATA_BITS_8`, `PARITY_EVEN` | `LL_USART_DATAWIDTH_9B`, `PARITY_EVEN` | `UART_WORDLENGTH_9B`, `PARITY_EVEN` |
| 7E1 | `UART_BM_DATA_BITS_7`, `PARITY_EVEN` | `LL_USART_DATAWIDTH_8B`, `PARITY_EVEN` | `UART_WORDLENGTH_8B`, `PARITY_EVEN` |
| 7N1 | `UART_BM_DATA_BITS_7`, `PARITY_NONE` | `LL_USART_DATAWIDTH_7B`, `PARITY_NONE` | `UART_WORDLENGTH_7B`, `PARITY_NONE` |

- **BM counts data bits.** It adds the parity bit itself when it programs
  `M[1:0]`, and `UART_BM_ReadByte` masks the parity bit out of 7-bit data.
  9 data bits plus parity does not exist in hardware and is rejected.
- **LL and HAL count the word length the way ST does: parity included.** The
  field in `UartLL_Config_t` is called `data_bits`, but it takes the
  `LL_USART_DATAWIDTH_*` constants unchanged. `UART_LL_ReadByte` returns the
  raw low byte, parity bit included, if there is one in bit 7.
- 9-bit data cannot be read through `ReadByte` in either tier: the API returns
  `uint8_t`, so bit 8 is lost.

### 4.3 Error flags

`UART_BM_ERR_*` and `UART_LL_ERR_*` have the same values: `ORE = bit 0`,
`NE = bit 1`, `FE = bit 2`, `PE = bit 3`. They are **not** the `USART_ISR`
bit positions (`USART_ISR_ORE` is bit 3). The flags accumulate in the handle
until `TakeErrors()` returns them.

An overrun that is never cleared stops reception. `ReadByte` and `IrqService`
clear it on every call, so reading regularly is enough.

### 4.4 The LL ring buffer

`UartLL_RingBuffer_t` is a single-producer, single-consumer queue: the IRQ
writes (`Put`), one context reads (`Get`, `Count`). It holds `size - 1` bytes:
one slot always stays empty to tell full from empty. Indices are `volatile`
and `__DMB()` orders data and index updates. It is **not** safe with two
readers, or across cores.

```c
static uint8_t             rx_storage[256];
static UartLL_RingBuffer_t rx_ring = { .data = rx_storage, .size = sizeof(rx_storage) };
```

### 4.5 HAL

| Function | Status | Notes |
| :--- | :--- | :--- |
| `UART_HAL_Init(huart, cfg)` | original, frozen | 8N1, no flow control, oversampling 16. |
| `UART_HAL_Write(huart, data, len)` | original, frozen | 100 ms timeout; the status is discarded. |
| `UART_Console_Init(baud)` | original, frozen | Instance from `BOARD_CONSOLE_UART_INSTANCE`; pins through the MSP hook. |
| `UART_Console_Write(data, len)` | original, frozen | |
| `UART_Console_GetHandle()` | original, frozen | For `HAL_UART_IRQHandler` and debugging. |
| `UART_HAL_MspInit_User()` / `MspDeInit_User()` | original, frozen, weak | See [3.1](#31-overriding-the-console-msp). |
| `UART_HAL_InitEx(huart, cfg)` | new | Framing, flow control, oversampling, optional bus clock and pins (pull-up, very high speed). `PRESC` is fixed at /1. |
| `UART_HAL_WriteEx(huart, data, len, timeout_ms)` | new | Returns the `HAL_StatusTypeDef`. |
| `UART_HAL_CheckDmaErrata(hdma)` | new | See [section 8](#8-silicon-errata-es0445). |

**Backward compatibility.** The seven original prototypes are frozen (README of
`01_shared_drivers`, rule 2): new behaviour is added as `...Ex` functions, never
by changing an existing signature.

---

## 5. Usage Examples

### A. Console before the clock tree, Cortex-M7 (BM)

Works before `SystemClock_Config()`, `MPU_Config()` and `HAL_Init()`: no HAL,
no SysTick, no interrupt. prj_04 runs exactly this when built with
`UART_DRIVER_SELFTEST=ON`.

```c
#include "uart_bm.h"

void EarlyBoot_Print(const char *msg, size_t len)
{
    /* Right after reset USART234578SEL = rcc_pclk1, and pclk1 = HSI = 64 MHz. */
    static const UartBm_Pin_t tx = { .port = GPIOD, .pin_idx = 8, .af = 7 };
    static const UartBm_Pin_t rx = { .port = GPIOD, .pin_idx = 9, .af = 7 };

    const UartBm_Config_t cfg = {
        .instance        = USART3,
        .kernel_clock_hz = 64000000UL,
        .baudrate        = 115200UL,
        .data_bits       = UART_BM_DATA_BITS_8,
        .stop_bits       = UART_BM_STOP_BITS_1,
        .parity          = UART_BM_PARITY_NONE,
        .enable_clock    = true,
        .tx_pin          = &tx,
        .rx_pin          = &rx,
    };
    UartBm_Handle_t h;

    if (UART_BM_Init(&h, &cfg))
    {
        (void)UART_BM_Write(&h, (const uint8_t *)msg, len, 100000U);
        (void)UART_BM_Flush(&h, 100000U);   /* before the clock tree changes */
    }
}
```

The HAL console can take the same USART afterwards: `HAL_UART_Init()`
reprograms it from scratch.

### B. Interrupt-driven RX on the Cortex-M4 (LL)

```c
#include "uart_ll.h"

static UartLL_Handle_t     s_uart;
static uint8_t             s_rx_storage[256];
static UartLL_RingBuffer_t s_rx = { .data = s_rx_storage, .size = sizeof(s_rx_storage) };

void USART2_IRQHandler(void)
{
    UART_LL_IrqService(&s_uart, &s_rx);
}

bool Telemetry_Init(void)
{
    static const UartLL_Pin_t tx = { .port = GPIOD, .pin_idx = 5, .af = 7 };
    static const UartLL_Pin_t rx = { .port = GPIOD, .pin_idx = 6, .af = 7 };

    const UartLL_Config_t cfg = {
        .instance        = USART2,
        .kernel_clock_hz = 100000000UL,          /* pclk1 after CPU1's clock setup */
        .baudrate        = 921600UL,
        .data_bits       = LL_USART_DATAWIDTH_8B,
        .stop_bits       = LL_USART_STOPBITS_1,
        .parity          = LL_USART_PARITY_NONE,
        .oversampling    = LL_USART_OVERSAMPLING_8,
        .fifo_enable     = true,
        .hw_flow         = LL_USART_HWCONTROL_NONE,
        .enable_clock    = true,
        .tx_pin          = &tx,
        .rx_pin          = &rx,
    };

    if (!UART_LL_Init(&s_uart, &cfg))
    {
        return false;
    }
    return UART_LL_EnableRxIrq(&s_uart, USART2_IRQn, 5U);
}

void Telemetry_Poll(void)
{
    uint8_t b;
    while (UartLL_RingBuffer_Get(&s_rx, &b))
    {
        /* consume b */
    }
    if (UART_LL_TakeErrors(&s_uart) & UART_LL_ERR_ORE)
    {
        /* bytes were lost before the ISR ran: raise its priority, or read the ring more often */
    }
}
```

### C. The board console (HAL)

```c
#include "uart_hal.h"

void App_Log_Init(void)
{
    (void)UART_Console_Init(UART_CONSOLE_DEFAULT_BAUDRATE);   /* board_def.h */
    UART_Console_Write((const uint8_t *)"console ready\r\n", 15U);
}
```

### D. A second UART next to the console (HAL)

```c
static UART_HandleTypeDef s_huart7;

HAL_StatusTypeDef Modem_Init(void)
{
    static const uart_hal_pin_t tx = { .port = GPIOE, .pin_idx = 8, .af = GPIO_AF7_UART7 };
    static const uart_hal_pin_t rx = { .port = GPIOE, .pin_idx = 7, .af = GPIO_AF7_UART7 };

    const uart_hal_config_ex_t cfg = {
        .instance     = UART7,
        .baudrate     = 115200UL,
        .word_length  = UART_WORDLENGTH_9B,      /* 8 data bits + parity */
        .stop_bits    = UART_STOPBITS_1,
        .parity       = UART_PARITY_EVEN,
        .hw_flow_ctl  = UART_HWCONTROL_NONE,
        .oversampling = UART_OVERSAMPLING_16,
        .enable_clock = true,
        .tx_pin       = &tx,
        .rx_pin       = &rx,
    };
    return UART_HAL_InitEx(&s_huart7, &cfg);
}
```

### E. DMA with the ES0445 workaround (HAL)

```c
/* After HAL_DMA_Init() and __HAL_LINKDMA(), before the first transfer. */
if (UART_HAL_CheckDmaErrata(&hdma_usart3_rx) != HAL_OK) { Error_Handler(); }
if (UART_HAL_CheckDmaErrata(&hdma_usart3_tx) != HAL_OK) { Error_Handler(); }
```

On Cortex-M7 the DMA buffers follow the workspace cache rules: 32-byte aligned,
a multiple of 32 bytes, clean before TX, invalidate after RX followed by
`__DMB()` (ES0445 2.1.5), or placed in the non-cacheable `.dma_buf` section.

---

## 6. Baud Rate Computation (BM and LL)

The driver tries the prescalers in increasing order and keeps the first one
whose divider is in range and whose baud rate is within ±2 % of the request.

| `PRESC[3:0]` | 0 | 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 | 9 | 10 | 11 |
| :--- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| Divider | 1 | 2 | 4 | 6 | 8 | 10 | 12 | 16 | 32 | 64 | 128 | 256 |

| Peripheral | Divider (rounded to nearest) | Valid range | Register |
| :--- | :--- | :--- | :--- |
| USART, OVER16 | `USARTDIV = f_ker_pres / baud` | 16 ≤ `USARTDIV` ≤ 0xFFFF | `BRR = USARTDIV` |
| USART, OVER8 | `USARTDIV = 2 × f_ker_pres / baud` | 16 ≤ `USARTDIV` ≤ 0xFFFF | `BRR[15:4] = USARTDIV[15:4]`, `BRR[3] = 0`, `BRR[2:0] = USARTDIV[3:1]` |
| LPUART1 | `LPUARTDIV = 256 × f_ker_pres / baud` | 0x300 ≤ `BRR` ≤ 0xFFFFF, 3·baud ≤ `f_ker_pres` ≤ 4096·baud | `BRR = LPUARTDIV` |

In OVER8 the hardware drops bit 0 of `USARTDIV`, so an odd divider runs as the
even one below it. The ±2 % check uses the divider the hardware will actually
apply, not the rounded one.

Reference values, recomputed from the formulas above:

| Instance | `f_ker` | Baud | Mode | `PRESC` | `BRR` | Error |
| :--- | :--- | :--- | :--- | :--- | :--- | :--- |
| USART3, early boot | 64 MHz | 115 200 | OVER16 | /1 | 556 (`0x22C`) | −0.08 % |
| USART3, console | 100 MHz | 115 200 | OVER16 | /1 | 868 (`0x364`) | +0.006 % |
| USART2 | 100 MHz | 921 600 | OVER8 | /1 | `0xD4` (divider 216) | +0.47 % |
| LPUART1 | 100 MHz | 1 200 | - | /32 (`0x8`) | 666 667 (`0xA2C2B`) | −0.00005 % |

The HAL tier does not run this search: `HAL_UART_Init()` computes `BRR` with
`PRESC` = /1 and returns `HAL_ERROR` if it is out of range. A low LPUART1 baud
rate at a high kernel clock therefore needs BM or LL.

---

## 7. Dual-Core Rules

- **One core owns an instance.** The driver takes no HSEM and keeps no global
  state; two cores driving the same USART will corrupt each other. In this
  workspace USART3 (console) belongs to CPU1.
- **Enabling the bus clock is safe from either core.** `APB1LENR`, `APB2ENR`
  and `APB4ENR` are banked per core (RCC offset ≥ `0xD0`), so there is no
  read-modify-write race. The peripheral stays clocked while either core has
  its bit set.
- **The kernel clock mux is not the driver's business.** See
  [2.1](#21-the-kernel-clock-what-to-pass-in-kernel_clock_hz).
- **CPU2 must not start before CPU1 has configured the clocks.** CPU2 in prj_04
  waits for `SHARED_MAGIC` *and* the HSEM 0 flag. The magic alone survives a
  warm reset in SRAM4, and a CM4 that trusted it ran on HSI with a 3x fast
  SysTick (commit `9034a5f`). Any `kernel_clock_hz` computed on CPU2 depends on
  that handshake being right.
- **Fault handlers.** BM `Write` and `Flush` use no interrupt and no tick, so
  they work with interrupts masked. `TakeErrors` saves and restores `PRIMASK`,
  so it does not re-enable interrupts inside a handler.

---

## 8. Silicon Errata (ES0445)

| Erratum | Issue | What the driver does |
| :--- | :--- | :--- |
| 2.20.3 | DMA stream locked on USART transfers | `UART_HAL_CheckDmaErrata()` sets bit 20 of `DMA_SxCR` and reads it back. It returns `HAL_ERROR` if the handle is not a DMA1/DMA2 stream or its request is not a USART/UART request, and `HAL_BUSY` if the stream is enabled (`CR` is write-protected then). ST's `HAL_DMA_Init()` already sets the bit for UART requests on Rev V; the check makes it explicit and verifiable. |
| 2.21.1 | DMA stream locked on LPUART transfers | Nothing to do on this device: LPUART1 is served only by BDMA through DMAMUX2 (RM0399 table 130), and BDMA channels have no bit 20. `CheckDmaErrata` rejects BDMA handles. |
| 2.21.2 | LPUART transmitter jitter when `f_ker / baud` is non-integer in [3, 4) | BM and LL skip any prescaler that produces a non-integer ratio in [3, 5). |
| 2.20.5 | NE set while `ONEBIT` is set | The driver never sets `ONEBIT`, so NE keeps its meaning. |
| 2.20.4 | RX corruption when `ABREN` is cleared | The driver never uses auto baud rate. |
| 2.20.2 | Data corruption on a noisy RX line | Hardware: keep the RX line clean or filter it. FE / NE show up in the error flags. |

---

## 9. Validation

Board: NUCLEO-H755ZI-Q, silicon Rev V (`0x2003`), CPU1 at 400 MHz, CPU2 at
200 MHz, pclk1 = 100 MHz. Capture: [`prj_04 docs/boot_capture.txt`](../../02_firmware_projects/prj_04_adc_dma_bm_eth/docs/boot_capture.txt).

| Test | Tier / core | Result |
| :--- | :--- | :--- |
| Console on USART3, 115 200, OVER16, `f_ker` = 64 MHz, before `SystemClock_Config()` | BM, CPU1 | `[boot_bm] ...` line received on the VCP |
| Console on USART3 through `UART_Console_*` for the whole firmware run | HAL, CPU1 | full log received |
| Loopback PD5 → PD6 (USART2), 9 bytes each at 9 600 and 115 200 (OVER16) and 921 600 (OVER8), FIFO off | BM, CPU2 | `bm_loop=1,1,1` |
| Loopback PD5 → PD6 at 921 600 (OVER8), RX through `USART2_IRQn` into the ring buffer | LL, CPU2 | `ll=1,1` |
| Hardware overrun: two bytes sent, none read; ORE seen in `USART_ISR`, cleared by `ReadByte`, returned once by `TakeErrors` | LL, CPU2 | `ore=1` |
| Strict build (`-Wall -Wextra -Wshadow -Wdouble-promotion -Wundef -Werror`) of `uart_bm.c` and `uart_ll.c` for CM4 and CM7 | BM, LL | clean |
| prj_02, prj_03, prj_04 (selftest OFF and ON) and the dual-core template | all | build with 0 warnings |

**Not validated on hardware yet:** LPUART1, RTS/CTS flow control, parity and
7-bit framing on the wire, the RX FIFO path (`fifo_enable = true`),
`UART_HAL_InitEx` on an instance other than the console, and
`UART_HAL_CheckDmaErrata` on a live DMA transfer. The baud-rate code for all of
them is covered by the table in [section 6](#6-baud-rate-computation-bm-and-ll).

### 9.1 Reproducing the selftest

The selftest lives in prj_04 and is off by default. It drives PD5 as an output
while it probes for the cable, so do not enable it on a board where PD5 is
wired to something else.

1. Fit a wire between PD5 (USART2_TX) and PD6 (USART2_RX). Their connector
   positions on the Nucleo-144 are in UM2408
   (`00_docs_stm32h7/docs/02_board_hardware/`).
2. Build with the option on, in its own build directory:
   ```bash
   cmake -S 02_firmware_projects/prj_04_adc_dma_bm_eth -B 02_firmware_projects/prj_04_adc_dma_bm_eth/build/uart_selftest -G Ninja -DCMAKE_TOOLCHAIN_FILE=02_firmware_projects/prj_04_adc_dma_bm_eth/cmake/arm-none-eabi.cmake -DCMAKE_BUILD_TYPE=Debug -DUART_DRIVER_SELFTEST=ON
   ```
   ```bash
   cmake --build 02_firmware_projects/prj_04_adc_dma_bm_eth/build/uart_selftest
   ```
3. Flash both cores with `tools/flash.py --elf <absolute path>` (CM4 image,
   then CM7 image), then watch the console with `tools/monitor.py`.
4. Expected line:
   ```text
   [m4_uart_test] jumper=1 bm_init=1,1,1 bm_loop=1,1,1 ll=1,1 ore=1
   ```
   `jumper=0` means the wire was not detected. The loopback fields then read
   `skip`, and only `Init()` has been exercised.

Flash the normal build afterwards; the selftest is not meant to stay on a board.

---

## 10. Limitations

- No DMA helpers in BM and LL. With `02_dma/dma_bm`, set
  `DmaBm_Config_t.enable_trbuff = true` on USART streams (ES0445 2.20.3).
- No TX interrupt path in LL; TX is polling only.
- `ReadByte` returns 8 bits, so 9-bit data loses bit 8.
- The HAL tier keeps `PRESC` at /1 and does not expose the FIFO.
- `Deinit` does not gate the bus clock or release the pins.
- Synchronous, smartcard, IrDA, LIN, RS-485 driver-enable and multiprocessor
  modes are not supported.
