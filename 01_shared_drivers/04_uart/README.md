# STM32H755 Shared UART Driver (`01_shared_drivers/04_uart`)

Peripheral-First (Plan A), 3-tier shared UART driver for the STM32H755ZI dual-core
microcontroller: USART1, USART2, USART3, UART4, UART5, USART6, UART7, UART8 and
LPUART1. Asynchronous mode only.

Status: Phase F1 is closed. All three driver tiers (BM, LL, HAL), the console,
and live hardware operations were validated on a NUCLEO-H755ZI-Q (silicon Rev V,
`REV_ID = 0x2003`), dual-core (Cortex-M7 at 400 MHz, Cortex-M4 at 200 MHz,
`pclk1 = 100 MHz`), using loopbacks JU1 (PD5 -> 1.0 kΩ -> PD6) and JU2
(PD4 RTS -> PD3 CTS), and Analog Discovery 2 DIO15 (for logic analyzer wire
decoding and error injection). Deviations D5 and D6 were formally approved by
technical leadership on 2026-10-17. Full test descriptions, hardware captures,
and pruned capture records are in [section 9](#9-validation).

---

## 1. Tiers

| Tier | Files | CMake target | Links | ST dependency | Typical use |
| :--- | :--- | :--- | :--- | :--- | :--- |
| **Bare-Metal** | `uart_bm.[c\|h]` | `stm32h7_driver_uart_bm` | `stm32h7_driver_gpio` | CMSIS only (`stm32h7xx.h`) | Console before `SystemClock_Config()`, fault handlers, Cortex-M4 without HAL UART |
| **Low-Level** | `uart_ll.[c\|h]` | `stm32h7_driver_uart_ll` | `stm32h7_driver_gpio` | `stm32h7xx_ll_usart.h`, `stm32h7xx_ll_lpuart.h` inlines | Same as BM, plus interrupt-driven RX into a ring buffer |
| **HAL** | `uart_hal.[c\|h]` | `stm32h7_driver_uart` | `stm32h7_driver_gpio_hal` | ST HAL (`HAL_UART_*`) | The ST-LINK VCP console of CPU1, FreeRTOS / lwIP projects |

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
| USART1..8 and LPUART1 | yes | yes | yes (rejects unknown instances upfront, D6) |
| Baud rate from an injected kernel clock, prescaler search, ±2 % check | yes | yes (effective divider check, U5) | no (HAL computes it, `PRESC` fixed at /1) |
| Oversampling by 16 or 8 (USART only) | yes | yes | yes |
| 7 / 8 / 9-bit words, parity, stop bits | yes (8-bit data max for byte I/O, U7) | yes (8-bit data max for byte I/O, U7) | yes |
| RTS / CTS hardware flow control | yes | yes | yes |
| Hardware FIFO (`FIFOEN`) | yes | yes | no |
| Optional pin muxing and bus clock enable | yes | yes | yes (`InitEx`, returns `HAL_BUSY` on lock timeout, U6) |
| Polling TX / RX without SysTick, bounded by a spin count | yes | yes | no (HAL tick) |
| Interrupt-driven RX into a ring buffer | no | yes | via HAL callbacks (application) |
| Error capture: ORE, FE, NE, PE | yes (`TakeErrors` reads/clears hardware flags, D5) | yes (`TakeErrors` reads/clears hardware flags, D5) | HAL `ErrorCode` |
| ES0445 DMA workaround check (bit 20 of `DMA_SxCR`) | - | - | yes (`UART_HAL_CheckDmaErrata`) |
| Board console from `board_def.h` | - | - | yes (`UART_Console_*`, returns MSP status, U1) |

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
| DMA stream configuration | **application**, with `02_dma` (`dma_bm` / `dma_ll`, which set TRBUFF themselves). The UART driver has no DMA hook: the application sets `CR3.DMAT` / `DMAR` | **application** (`HAL_DMA_Init`, or `DMA_HAL_Init` from `02_dma`), then `UART_HAL_CheckDmaErrata` |
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
> = `0x0100`), because the default MSP hands it to `GPIO_HAL_Init()` (from
> `00_gpio`, which runs inside `gpio_bm`'s critical section; cross-core only if
> the project installs the shared lock hook, otherwise local `PRIMASK`; since F1b).
> Every `*_Pin_t` structure of the driver (`UartBm_Pin_t`, `UartLL_Pin_t`,
> `uart_hal_pin_t`) takes a pin **index** (`8`). Passing `GPIO_PIN_8` to
> `pin_idx` makes `Init` return `false`.

### 3.1 Overriding the console MSP

`uart_hal.c` defines `HAL_UART_MspInit()` / `HAL_UART_MspDeInit()` and forwards
them to two weak hooks. The default hook clocks the console instance and muxes
its pins from the macros above; any other instance is left alone.

Since F1b of `00_gpio`, the default MSP configures the console pins using
`GPIO_HAL_Init()` and releases them using `GPIO_HAL_DeinitPin()`, executing
every register access inside `gpio_bm`'s critical section (cross-core if the
project installs the shared lock hook, or local `PRIMASK` otherwise). Any error
from `GPIO_HAL_Init()` is captured in `s_msp_gpio_status` and propagated by
`UART_Console_Init()`, `UART_HAL_InitEx()`, and `UART_HAL_Init()` (U1), returning
the MSP status (`HAL_BUSY` if the cross-core lock timed out, or `HAL_ERROR` on
any other pin setup failure) rather than silently proceeding with misconfigured
pins. In `UART_HAL_Init()`, if the console MSP fails, `s_msp_gpio_status` is
returned directly to the caller and the handle remains in `HAL_UART_STATE_RESET`
(U1), preventing silent operation with disconnected hardware.

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
| `Init(h, cfg)` | up to 100 000 spins for `TEACK` / `REACK` | `false` on a bad parameter, an unknown instance, a baud rate that cannot be reached within ±2 %, or no acknowledge | Every parameter rejection happens **before** any USART registers are touched, so a refused call leaves a running UART as it was and registers `CR1`..`CR3` clean (U2, U3, U4). BM checks every framing constant; LL checks framing constants, instance validity, and LPUART stop bits upfront (U2, U3). The optional pins and bus clock are set up before peripheral register configuration. |
| `Deinit(h)` | no | `false` if `h` is not initialised | Clears `UE`, `CR1..CR3`. Leaves the bus clock and the pins as they are. |
| `WriteByte(h, b)` | no | `false` if `TXE` / `TXFNF` is not set | Only 8 bits are written; bit 8 of 9-bit data is not accessible (U7). |
| `Write(h, d, n, spin_limit)` | yes, at most `spin_limit` spins **per byte** | bytes written | A short count means a timeout. Operates on 8-bit bytes (U7). |
| `ReadByte(h, b)` | no | `false` if nothing was received | Clears ORE / FE / NE / PE first and records them in `h->error_flags`. Returns low 8 bits (U7). |
| `Flush(h, spin_limit)` | yes | `false` on timeout | Waits for `TC`: the last stop bit has left the pin. Call it before `Deinit`, a baud change or entering a low-power mode. |
| `TakeErrors(h)` | no | `UART_*_ERR_*` mask | Returns and clears accumulated error flags inside a `PRIMASK` critical section. In addition, it reads and clears hardware error flags (`ORE`, `NE`, `FE`, `PE`) directly from the peripheral registers (`ISR` / `ICR`), ensuring pending hardware errors are harvested even if no byte read occurred (approved deviation D5, 2026-10-17). Safe against `UART_LL_IrqService` on the same core. |
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
- **9-bit data limitation (U7).** 9-bit data words cannot be read through `ReadByte`
  or written through `Write` / `WriteByte`: the APIs operate on `uint8_t`, so bit 8
  cannot be written or read. 9-bit framing is intended for 8-bit payloads with parity
  or requires direct register manipulation of `TDR` and `RDR`.

### 4.3 Error flags

`UART_BM_ERR_*` and `UART_LL_ERR_*` have the same values: `ORE = bit 0`,
`NE = bit 1`, `FE = bit 2`, `PE = bit 3`. They are **not** the `USART_ISR`
bit positions (`USART_ISR_ORE` is bit 3). The flags accumulate in the handle
until `TakeErrors()` returns them.

In addition to accumulating flags during byte reads or IRQ handling, `TakeErrors()`
reads active error flags directly from the hardware `USART_ISR` register and
clears them via `USART_ICR` inside a local `PRIMASK` critical section (approved
deviation D5, 2026-10-17). This guarantees that hardware errors do not remain stuck
in peripheral registers even if the application polls error flags without reading
data.

An overrun that is never cleared stops reception. `ReadByte`, `IrqService`, and
`TakeErrors` clear it on every call, so regular polling is enough.

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
| `UART_HAL_Init(huart, cfg)` | original, frozen | 8N1, no flow control, oversampling 16. Rejects non-UART instances upfront with `HAL_ERROR` (approved deviation D6, 2026-10-17). Propagates console MSP status and leaves handle in `RESET` on pin error (U1). |
| `UART_HAL_Write(huart, data, len)` | original, frozen | 100 ms timeout; the status is discarded. |
| `UART_Console_Init(baud)` | original, frozen | Instance from `BOARD_CONSOLE_UART_INSTANCE`; pins through the MSP hook. Returns the MSP status (`HAL_BUSY` on cross-core lock timeout, `HAL_ERROR` on pin failure) if pin initialization fails (U1). |
| `UART_Console_Write(data, len)` | original, frozen | |
| `UART_Console_GetHandle()` | original, frozen | For `HAL_UART_IRQHandler` and debugging. |
| `UART_HAL_MspInit_User()` / `MspDeInit_User()` | original, frozen, weak | See [3.1](#31-overriding-the-console-msp). Default MSP uses `GPIO_HAL_Init` / `GPIO_HAL_DeinitPin` within `gpio_bm`'s critical section (cross-core if hook installed, otherwise local `PRIMASK`). |
| `UART_HAL_InitEx(huart, cfg)` | new | Framing, flow control, oversampling, optional bus clock and pins (pull-up, very high speed). `PRESC` is fixed at /1. Rejects non-UART instances upfront with `HAL_ERROR` (approved deviation D6, 2026-10-17). Returns MSP status (`HAL_BUSY` on lock timeout, `HAL_ERROR` on failure) if pin setup fails (U6). |
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
even one below it (`USARTDIV & ~1U`). The ±2 % check in BM and LL uses the
*effective* divider the hardware will actually apply
(`eff_div = (oversampling == 8) ? (div & ~1U) : div`), rather than the unadjusted
rounded quotient (U5). This ensures small and odd dividers never pass validation
with an unachievable theoretical rate.

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
| 2.20.3 | DMA stream locked on USART transfers | `UART_HAL_CheckDmaErrata()` sets bit 20 of `DMA_SxCR` and reads it back. It returns `HAL_ERROR` if the handle is not a DMA1/DMA2 stream or its request is not a USART/UART request, and `HAL_BUSY` if the stream is enabled (`CR` is write-protected then). ST's `HAL_DMA_Init()` already sets the bit for UART requests on Rev V; the check makes it explicit and verifiable. The shared DMA driver applies the same workaround on its own (`02_dma/README.md` §6): `dma_bm` and `dma_ll` by request ID, `dma_hal` through `DMA_HAL_CheckErrata`. |
| 2.21.1 | DMA stream locked on LPUART transfers | Nothing to do on this device: LPUART1 is served only by BDMA through DMAMUX2 (RM0399 table 130), and BDMA channels have no bit 20. `CheckDmaErrata` rejects BDMA handles. |
| 2.21.2 | LPUART transmitter jitter when `f_ker / baud` is non-integer in [3, 4) | BM and LL skip any prescaler that produces a non-integer ratio in [3, 5). |
| 2.20.5 | NE set while `ONEBIT` is set | The driver never sets `ONEBIT`, so NE keeps its meaning. |
| 2.20.4 | RX corruption when `ABREN` is cleared | The driver never uses auto baud rate. |
| 2.20.2 | Data corruption on a noisy RX line | Hardware: keep the RX line clean or filter it. FE / NE show up in the error flags. |

---

## 9. Validation

Board: NUCLEO-H755ZI-Q (silicon Rev V, `REV_ID = 0x2003`), dual-core (Cortex-M7 at 400 MHz,
Cortex-M4 at 200 MHz, `pclk1 = 100 MHz`). Instrument: Analog Discovery 2 (SN `210321B5D38D`).
Wiring:
- **JU1**: Loopback wire with **1.0 kΩ** series resistor between PD5 (USART2_TX, CN9-6) and PD6 (USART2_RX, CN9-4). The 1.0 kΩ resistor limits contention current to ≤ 3.3 mA during AD2 error injection.
- **JU2**: Direct wire between PD4 (USART2_RTS, CN9-8) and PD3 (USART2_CTS, CN9-10) for hardware flow control.
- **DIO15**: Connected to PD6 (CN9-4) for logic analyzer frame decoding (UT3, UT4, UT6, UT8) and error injection (UT5).
- **DIO14**: Connected to PA8 (MCO1) outputting HSE/10 = 800 kHz, used as external clock reference for baud rate measurement (UT3).

Selftest suite: `03_firmware_templates/prj_01_template_dualcore`, preset `selftest-uart`.
Host evaluation harness: `03_firmware_templates/prj_01_template_dualcore/tools/uart_hw_check.py`.

### 9.1 Phase F1 Test Matrix (UT0–UT10)

Every step in UT0–UT9 runs a negative control in the same hardware session that mutates measured data or hardware conditions through the identical host evaluation function to ensure no test can pass unconditionally:

| Test | Description and Tier | Oracle | Negative Control (same session) | Result |
| :--- | :--- | :--- | :--- | :---: |
| **UT0** | Bench resting state: JU1 (1 kΩ), JU2, DIO15 connected and idle, PD6 high | AD2 reads PD6 high (1) | MCU drives PD5 low via GPIO; AD2 reads 0 | **PASS** |
| **UT1** | Parameter rejection across BM, LL, and HAL: unknown instance, out-of-range framing, unreachable baud rate (U2, U3), and non-UART instance (D6). Before/after register snapshots show 0 register alterations (U4) | Register snapshot comparison | Valid configuration in same table changes snapshot | **PASS** |
| **UT2** | Prescaler and BRR formulas across BM and LL for USART1, USART6, USART2, UART4, UART5, UART7, UART8, and LPUART1. Checks effective divider applied in hardware for OVER8 odd dividers (U5) | Host mathematical formula | Mutated row in host evaluation table evaluates to FAIL | **PASS** |
| **UT3** | Physical wire framing across BM, LL, and HAL (`InitEx`): 8N1, 8E1, 8O1, 7E1, 7N1, 9N1, 1 and 2 stop bits at 9600, 115200, and 921600 baud (OVER8). Measures bit period against MCO1 (DIO14) to ±0.5% | AD2 Logic Analyzer at 100 MS/s or 2 MS/s decodes all frames | 5 mutations per row on measured data: dropped frame, +2·tol timing, bit flip, inverted parity (on parity rows), corrupted stop bit. All evaluate to FAIL (or N/A for parity on parity-none rows) | **PASS** |
| **UT4** | Loopback transmission over JU1 on CM7: BM and LL, FIFO disabled and enabled (`RXFNE`), 7-bit mask, OVER8 and OVER16. All 72/72 bytes verified | Transmitted vs received data match | Corrupted expected byte evaluates to FAIL | **PASS** |
| **UT4_CM4** | Loopback transmission over JU1 on CM4: LL interrupt-driven reception into ring buffer with FIFO enabled. 16/16 bytes verified | Ring buffer data matches sent pattern | Corrupted byte evaluates to FAIL | **PASS** |
| **UT5** | Error injection via AD2 DIO15 into PD6: PE (corrupted parity), FE (stop bit at 0 and BREAK), NE (short pulse at 9/16 bit sample), ORE with and without FIFO. `TakeErrors()` harvests hardware flags directly (D5) | Flag verification via `TakeErrors()` | Clean frame produces no errors | **PASS** |
| **UT6** | RTS/CTS hardware flow control over JU2: 24 bytes sent with flow control enabled without ORE. Without flow control, unread RX overflows and triggers ORE | Frame count in LA and ORE flag assertion | Identical sequence without flow control asserts ORE | **PASS** |
| **UT7** | `UART_HAL_InitEx` on USART2 (CM7, non-console): framing 8E1, pull-up, high-speed pins. Cross-core GPIO lock held by CM4 returns `HAL_BUSY` (U6); lock released returns `HAL_OK` | Returned status and register snapshot | Lock free returns `HAL_OK` | **PASS** |
| **UT8** | `UART_HAL_CheckDmaErrata` on live DMA1 transfer (bit 20 set and verified). Rejection with active stream (`HAL_BUSY`), non-UART request (`HAL_ERROR`), and BDMA handle (`HAL_ERROR`) | `DMA_SxCR` bit 20 read-back and returned status | All 3 rejection cases evaluate to FAIL if accepted | **PASS** |
| **UT9** | U1 verification: `UART_HAL_Init` on console handle with GPIO lock held by CM4 returns `HAL_BUSY`; lock released returns `HAL_OK`. Handle left clean in `RESET` on error | Returned status and PD8/PD9 pin configuration | Negative control on base commit `b41c4a7` returned `HAL_OK` (reproducing defect U1) | **PASS** |
| **UT10** | Regression check: real selftest of `02_firmware_projects/prj_04_adc_dma_bm_eth` runs with JU1 1 kΩ loopback | VCP console output: `[m4_uart_test] jumper=1 bm_init=1,1,1 bm_loop=1,1,1 ll=1,1 ore=1` | Missing jumper reports `jumper=0` | **PASS** |

### 9.2 Validation Evidence and Captures

**Auditor Execution (commit `954c97a` / `2611907`):**
- **Full hardware session (5/5 consecutive runs):**
  - [`uart_audit_hw_2611907_run1.txt`](../../03_firmware_templates/prj_01_template_dualcore/docs/uart_audit_hw_2611907_run1.txt)
  - [`uart_audit_hw_2611907_run2.txt`](../../03_firmware_templates/prj_01_template_dualcore/docs/uart_audit_hw_2611907_run2.txt)
  - [`uart_audit_hw_2611907_run3.txt`](../../03_firmware_templates/prj_01_template_dualcore/docs/uart_audit_hw_2611907_run3.txt)
  - [`uart_audit_hw_2611907_run4.txt`](../../03_firmware_templates/prj_01_template_dualcore/docs/uart_audit_hw_2611907_run4.txt)
  - [`uart_audit_hw_2611907_run5.txt`](../../03_firmware_templates/prj_01_template_dualcore/docs/uart_audit_hw_2611907_run5.txt)
  *All 5 runs:* `FW VERDICT=PASS`, `HOST VERDICT=PASS`, `m4_done_ack=1`, 15/15 UT3 rows PASS, all negative control mutations evaluate to expected FAIL (or N/A).
- **20 Warm Resets:** [`uart_audit_warm_resets_2611907.txt`](../../03_firmware_templates/prj_01_template_dualcore/docs/uart_audit_warm_resets_2611907.txt) — 20/20 resets PASS with matching revision and build ID.
- **UT9 Negative Control Reproduction:** [`uart_audit_ut9_control_2611907-dirty.txt`](../../03_firmware_templates/prj_01_template_dualcore/docs/uart_audit_ut9_control_2611907-dirty.txt) — defect U1 reproduced on `b41c4a7` (`returned 0, expected 2`).
- **UT10 Regression in prj_04:** [`uart_audit_ut10_2611907.txt`](../../02_firmware_projects/prj_04_adc_dma_bm_eth/docs/uart_audit_ut10_2611907.txt) — PASS with JU1 1 kΩ loopback.

**Developer Deliverable Captures (closure at `9229773`):**
- Hardware session run 1: [`uart_dev_hw_9229773_run1.txt`](../../03_firmware_templates/prj_01_template_dualcore/docs/uart_dev_hw_9229773_run1.txt)
- Hardware session run 2: [`uart_dev_hw_9229773_run2.txt`](../../03_firmware_templates/prj_01_template_dualcore/docs/uart_dev_hw_9229773_run2.txt)
- 20 Warm resets: [`uart_dev_warm_resets_9229773.txt`](../../03_firmware_templates/prj_01_template_dualcore/docs/uart_dev_warm_resets_9229773.txt)
- Offline checks T1/T2: [`uart_checks_9229773.txt`](docs/uart_checks_9229773.txt)

### 9.3 Pruned Captures Catalog (Rule 7 of `00_gpio/README.md` §14)

In accordance with rule 7 of `00_gpio/README.md` §14, intermediate development captures, superseded auditor runs prior to `2611907`, and mock outputs have been pruned from the working tree. They can be retrieved from git history at the commit hashes listed below:

| Pruned Capture | Type | Original Commit | Reason for Pruning |
| :--- | :--- | :---: | :--- |
| `01_shared_drivers/04_uart/docs/uart_checks_474f514.txt` | Offline checks T1/T2 | `c42381e` | Superseded by final closure check |
| `01_shared_drivers/04_uart/docs/uart_checks_4db5903.txt` | Offline checks T1/T2 | `12a3020` | Superseded by final closure check |
| `01_shared_drivers/04_uart/docs/uart_checks_76eb6d7.txt` | Offline checks T1/T2 | `01ef495` | Superseded by final closure check |
| `01_shared_drivers/04_uart/docs/uart_checks_9864bc9.txt` | Offline checks T1/T2 | `f34fe82` | Superseded by final closure check |
| `01_shared_drivers/04_uart/docs/uart_checks_9b9fe37.txt` | Offline checks T1/T2 | `adc848d` | Superseded by final closure check |
| `01_shared_drivers/04_uart/docs/uart_checks_a39abd7.txt` | Offline checks T1/T2 | `641ac26` | Superseded by final closure check |
| `01_shared_drivers/04_uart/docs/uart_checks_ab390d3.txt` | Offline checks T1/T2 | `2611907` | Superseded by final closure check `9229773` |
| `01_shared_drivers/04_uart/docs/uart_checks_fdbf306.txt` | Offline checks T1/T2 | `9d2edd2` | Superseded by final closure check |
| `02_firmware_projects/prj_04_adc_dma_bm_eth/docs/uart_audit_ut10_12a3020.txt` | UT10 audit capture | `297e04d` | Superseded by auditor run `2611907` |
| `02_firmware_projects/prj_04_adc_dma_bm_eth/docs/uart_audit_ut10_9d2edd2.txt` | UT10 audit capture | `8d04a51` | Superseded by auditor run `2611907` |
| `02_firmware_projects/prj_04_adc_dma_bm_eth/docs/uart_audit_ut10_adc848d.txt` | UT10 audit capture | `882b5a9` | Superseded by auditor run `2611907` |
| `03_firmware_templates/prj_01_template_dualcore/docs/uart_audit_hw_12a3020_run1.txt` | Audit hardware run 1 | `297e04d` | Superseded by auditor 5/5 runs `2611907` |
| `03_firmware_templates/prj_01_template_dualcore/docs/uart_audit_hw_12a3020_run2.txt` | Audit hardware run 2 | `297e04d` | Superseded by auditor 5/5 runs `2611907` |
| `03_firmware_templates/prj_01_template_dualcore/docs/uart_audit_hw_9d2edd2_run1.txt` | Audit hardware run 1 | `8d04a51` | Superseded by auditor 5/5 runs `2611907` |
| `03_firmware_templates/prj_01_template_dualcore/docs/uart_audit_hw_9d2edd2_run2.txt` | Audit hardware run 2 | `8d04a51` | Superseded by auditor 5/5 runs `2611907` |
| `03_firmware_templates/prj_01_template_dualcore/docs/uart_audit_hw_adc848d_run1.txt` | Audit hardware run 1 | `882b5a9` | Superseded by auditor 5/5 runs `2611907` |
| `03_firmware_templates/prj_01_template_dualcore/docs/uart_audit_hw_adc848d_run2.txt` | Audit hardware run 2 | `882b5a9` | Superseded by auditor 5/5 runs `2611907` |
| `03_firmware_templates/prj_01_template_dualcore/docs/uart_audit_hw_c42381e.txt` | Audit hardware run | `52fef6e` | Superseded by auditor 5/5 runs `2611907` |
| `03_firmware_templates/prj_01_template_dualcore/docs/uart_audit_ut9_control_12a3020-dirty.txt` | UT9 control audit | `297e04d` | Superseded by auditor control `2611907` |
| `03_firmware_templates/prj_01_template_dualcore/docs/uart_audit_ut9_control_9d2edd2-dirty.txt` | UT9 control audit | `8d04a51` | Superseded by auditor control `2611907` |
| `03_firmware_templates/prj_01_template_dualcore/docs/uart_audit_ut9_control_adc848d-dirty.txt` | UT9 control audit | `882b5a9` | Superseded by auditor control `2611907` |
| `03_firmware_templates/prj_01_template_dualcore/docs/uart_audit_warm_resets_12a3020.txt` | Warm resets 20x audit | `297e04d` | Superseded by auditor warm resets `2611907` |
| `03_firmware_templates/prj_01_template_dualcore/docs/uart_audit_warm_resets_9d2edd2.txt` | Warm resets 20x audit | `8d04a51` | Superseded by auditor warm resets `2611907` |
| `03_firmware_templates/prj_01_template_dualcore/docs/uart_audit_warm_resets_adc848d.txt` | Warm resets 20x audit | `882b5a9` | Superseded by auditor warm resets `2611907` |
| `03_firmware_templates/prj_01_template_dualcore/docs/uart_dev_hw_4db5903.txt` | Dev hardware run | `12a3020` | Superseded by final dev runs |
| `03_firmware_templates/prj_01_template_dualcore/docs/uart_dev_hw_9b9fe37.txt` | Dev hardware run | `adc848d` | Superseded by final dev runs |
| `03_firmware_templates/prj_01_template_dualcore/docs/uart_dev_hw_a39abd7.txt` | Dev hardware run | `641ac26` | Superseded by final dev runs |
| `03_firmware_templates/prj_01_template_dualcore/docs/uart_dev_hw_ab390d3.txt` | Dev hardware run (redundant) | `2611907` | Redundant with `_run1.txt` (I-7) |
| `03_firmware_templates/prj_01_template_dualcore/docs/uart_dev_hw_ab390d3_run1.txt` | Dev hardware run 1 | `2611907` | Superseded by final closure runs `9229773` |
| `03_firmware_templates/prj_01_template_dualcore/docs/uart_dev_hw_ab390d3_run2.txt` | Dev hardware run 2 | `2611907` | Superseded by final closure runs `9229773` |
| `03_firmware_templates/prj_01_template_dualcore/docs/uart_dev_hw_fdbf306.txt` | Dev hardware run | `9d2edd2` | Superseded by final dev runs |
| `03_firmware_templates/prj_01_template_dualcore/docs/uart_dev_warm_resets.txt` | Dev warm resets | `641ac26` | Superseded by final dev warm resets |
| `03_firmware_templates/prj_01_template_dualcore/docs/uart_dev_warm_resets_4db5903.txt` | Dev warm resets | `12a3020` | Superseded by final dev warm resets |
| `03_firmware_templates/prj_01_template_dualcore/docs/uart_dev_warm_resets_9b9fe37.txt` | Dev warm resets | `adc848d` | Superseded by final dev warm resets |
| `03_firmware_templates/prj_01_template_dualcore/docs/uart_dev_warm_resets_ab390d3.txt` | Dev warm resets | `2611907` | Superseded by final closure warm resets `9229773` |
| `03_firmware_templates/prj_01_template_dualcore/docs/uart_dev_warm_resets_fdbf306.txt` | Dev warm resets | `9d2edd2` | Superseded by final dev warm resets |
| `03_firmware_templates/prj_01_template_dualcore/docs/uart_mock_474f514.txt` | Mock check | `c42381e` | Superseded by final test suite |
| `03_firmware_templates/prj_01_template_dualcore/docs/uart_mock_593d720.txt` | Mock check | `ae3e620` | Superseded by final test suite |
| `03_firmware_templates/prj_01_template_dualcore/docs/uart_mock_9864bc9.txt` | Mock check | `f34fe82` | Superseded by final test suite |
| `03_firmware_templates/prj_01_template_dualcore/docs/uart_mock_a39abd7.txt` | Mock check | `641ac26` | Superseded by final test suite |

### 9.4 Reproducing the Selftest

#### prj_01 Dual-Core Selftest (UT0–UT9)
1. Fit jumper JU1: PD5 (USART2_TX, CN9-6) through 1.0 kΩ series resistor to PD6 (USART2_RX, CN9-4).
2. Fit jumper JU2: PD4 (USART2_RTS, CN9-8) directly to PD3 (USART2_CTS, CN9-10).
3. Connect AD2 DIO15 to PD6 (CN9-4) and DIO14 to PA8 (MCO1, CN10-4).
4. Build and flash preset `selftest-uart`:
   ```powershell
   . .\tools\env.ps1
   cmake --build 03_firmware_templates/prj_01_template_dualcore/build/selftest-uart
   py 03_firmware_templates/prj_01_template_dualcore/tools/flash.py --preset selftest-uart --sn 0016000C4142501220353451
   ```
5. Run the hardware validation check:
   ```powershell
   py 03_firmware_templates/prj_01_template_dualcore/tools/uart_hw_check.py --out 03_firmware_templates/prj_01_template_dualcore/docs/uart_dev_hw_run.txt
   ```

#### prj_04 Production Selftest Regression (UT10)
1. With JU1 (1 kΩ) in place between PD5 and PD6:
   ```powershell
   . .\tools\env.ps1
   cmake -S 02_firmware_projects/prj_04_adc_dma_bm_eth -B 02_firmware_projects/prj_04_adc_dma_bm_eth/build/selftest-uart --preset selftest-uart
   cmake --build 02_firmware_projects/prj_04_adc_dma_bm_eth/build/selftest-uart
   py 02_firmware_projects/prj_04_adc_dma_bm_eth/tools/flash.py --preset selftest-uart --sn 0016000C4142501220353451
   ```
2. Start the monitor and perform a hardware reset:
   ```powershell
   Start-Process -NoNewWindow py "tools/monitor.py --sn 0016000C4142501220353451 --timeout 10"
   STM32_Programmer_CLI.exe -c port=SWD sn=0016000C4142501220353451 mode=HOTPLUG -hardRst
   ```
3. Expected output line:
   ```text
   [m4_uart_test] jumper=1 bm_init=1,1,1 bm_loop=1,1,1 ll=1,1 ore=1
   ```

---

## 10. Limitations

- **9-bit data restriction (U7):** `ReadByte` returns `uint8_t` and `Write`/`WriteByte` accepts `uint8_t`, so bit 8 cannot be transferred via standard byte APIs. 9-bit data width is supported for 8 data bits with hardware parity; applications requiring 9 raw data bits must access `TDR` and `RDR` directly.
- **Handle re-initialization without deinit (U7):** Calling `Init` on a handle that was previously initialized without calling `Deinit` first may leave the peripheral half-configured if a subsequent parameter check fails. Callers must invoke `Deinit` before re-configuring an active handle.
- **D4 (Measured Hardware Fact on LPUART1):** In `uart_ll.c`, ST LL's `ATOMIC_*` macros (which expand to `LDREX`/`STREX` exclusive load/store sequences) cause the Cortex-M core to hang when executing against LPUART1 in the D3 power/clock domain. This is an observed, measured physical hardware fact on the STM32H755ZI (NUCLEO-H755ZI-Q board), independent of any published errata citation. To prevent core hangs, `uart_ll.c` performs read-modify-write register accesses under a local `PRIMASK` critical section instead of using `ATOMIC_*`.
- **Noise Error (NE) detection:** Noise detection in the receiver hardware uses 3-sample majority voting at bit center. A glitch pulse injected at 9/16 of the bit time reliably triggers `NE` in UT5.
- **LPUART1 live wire unexercised:** LPUART1 was validated on hardware for prescaler search and divider registers (`PRESC` and `BRR` in UT2). Live wire reception and transmission was not exercised on the bench because LPUART1 pins (PB6/PB7, Arduino D1/D0) depend on Nucleo solder bridges (UM2408 SB11, SB96, SB15, SB94) that may connect them to the ST-LINK VCP.
- **Unexercised features:** Synchronous mode, Smartcard (ISO 7816-3), IrDA SIR ENDEC, LIN mode, RS-485 driver enable hardware flow, and multiprocessor communication mode are not supported.
- No DMA helpers in BM and LL. Use `02_dma` for the stream and set `CR3.DMAT` / `DMAR` in the application. `dma_bm` and `dma_ll` set TRBUFF (ES0445 2.20.3) on their own for USART/UART requests, so `enable_trbuff = true` is no longer needed.
- No TX interrupt path in LL; TX is polling only.
- The HAL tier keeps `PRESC` at /1 and does not expose the FIFO.
- `Deinit` does not gate the bus clock or release the pins.

---

## 11. Changing this driver

This driver is in a shared catalogue. `prj_02` and `prj_03` build the HAL
console through the umbrella target, `prj_04` and the dual-core template link
`stm32h7_driver_uart` by name, and `prj_04` also builds BM and LL for its
selftest. Before changing the driver:

1. **The seven original HAL prototypes and every existing BM/LL signature are
   frozen** (`01_shared_drivers/README.md` rule 2). New behaviour is added as `…Ex`
   functions or as fields at the end of a struct, never by changing what already
   exists.
2. **All hardware comes from the caller:** instance, pins, alternate function and
   `kernel_clock_hz` (BM/LL), or `board_def.h` (HAL console). The driver never
   touches the kernel clock mux: it sits in the RCC bank both cores share (§2.1).
3. **Validate before touching the USART registers.** A refused `Init` must leave a
   running UART as it was (§4.1, U2–U4).
4. **BM and LL keep the same function names and semantics**, so that moving a
   module between them only changes the prefix and the constants (§1.1). The one
   documented difference is how word length is counted (§4.2).
5. **No ST global symbol without a way to turn it off.** `HAL_UART_MspInit` is
   suppressed by `UART_HAL_CUSTOM_MSP` (§3.1). Keep it that way for any new
   callback or MSP.
6. **A missing `board_def.h` macro stays a compile-time `#error`**, never a silent
   default.
7. **Before merging:**
   - `.\tools\build_all.ps1 -Clean`, 8/8 with `-Werror`;
   - the `prj_01` `selftest-uart` suite run on the board with JU1 (1 kΩ), JU2, and DIO15;
   - the `prj_04` `selftest-uart` build, run on the board with the JU1 wire;
   - the expected line of §9.4 recorded in `prj_04 docs/boot_capture.txt`.

---

## 12. History

| Milestone | Commits |
| :--- | :--- |
| HAL console centralised in `01_shared_drivers` | `4204cad`, `129c96c`, `ecf01a2` |
| Unified 3-tier driver (BM, LL, HAL) | `dcd9230` |
| Selftest on USART2 behind `UART_DRIVER_SELFTEST`, and its hardware record with the PD5-PD6 wire | `c0ca222`, `a7f700a` |
| README rewritten around what was validated | `b23b99b` |
| USART + DMA exercised by the `02_dma` selftest (T3, T10) | `02_dma/README.md` §13 |
| Default MSP migrated to `GPIO_HAL_Init` / `GPIO_HAL_DeinitPin` under `gpio_bm` (F1b, H3, G13) | `c8001bf` |
| Phase F1 implementation: unified dual-core selftest (UT0–UT9), AD2 test bench, framing decode with MCO1 clock reference | `474f514` … `9827978` |
| Phase F1 audit corrections: 5 measured mutations per UT3 row, DONE handshake race fix, MCO1 duty-cycle coverage, dynamic kernel clock from `RCC_D2CCIP2R` | `4db5903` … `ab390d3` |
| Technical leadership approval of deviations D5 (`TakeErrors` hardware harvesting) and D6 (HAL instance validation) (2026-10-17) | `9304ed5` |
| Phase F1 audit passed: 5/5 auditor board runs at `954c97a`, 20/20 warm resets, UT9 control in worktree, UT10 in prj_04 | `954c97a`, `9304ed5` |
| Phase F1 formal close: address audit findings I-1..I-7, protocol barriers, lock release check, doc updates, capture pruning | `9229773` |
