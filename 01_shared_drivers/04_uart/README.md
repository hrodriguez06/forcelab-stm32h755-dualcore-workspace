# STM32H755 Shared UART Driver (`01_shared_drivers/04_uart`)

Peripheral-First (Plan A), 3-tier shared UART driver for the STM32H755ZI dual-core microcontroller (USART1–8 and LPUART1).

---

## 1. Architecture & Layering Matrix

| Driver Tier | File Suffix | Library Target | Target Core | Typical Use Case | ST Dependencies |
| :--- | :--- | :--- | :--- | :--- | :--- |
| **Bare-Metal** | `*_bm.[c\|h]` | `stm32h7_driver_uart_bm` | **Cortex-M4** / Early **M7** | Pre-clock tree boot logging, fault handlers, hard real-time loops | Pure CMSIS (`stm32h7xx.h`), zero HAL/LL |
| **Low-Level** | `*_ll.[c\|h]` | `stm32h7_driver_uart_ll` | **Cortex-M4** / **M7** | Lightweight telemetry, interrupt-driven RX with circular buffer | ST LL static inlines (`stm32h7xx_ll_*.h`) |
| **HAL** | `*_hal.[c\|h]` | `stm32h7_driver_uart` | **Cortex-M7** | ST-LINK VCP console, FreeRTOS tasks, lwIP diagnostics | ST HAL (`stm32h7xx_hal.h`) |

### Capability Matrix by Mode

| Mode | Bare-Metal (`_bm`) | Low-Level (`_ll`) | HAL (`_hal`) |
| :--- | :---: | :---: | :---: |
| **Polling / Non-blocking** | Yes (`WriteByte`, `ReadByte`) | Yes (`WriteByte`, `ReadByte`) | Yes (`UART_HAL_Write`, `WriteEx`) |
| **Bounded Spin Timeout** | Yes (`spin_limit`) | Yes (`spin_limit`) | No (uses HAL tick / ms) |
| **Zero SysTick / No Interrupts** | Yes (Fault handler safe) | Yes (Pre-RTOS safe) | No (requires SysTick / HAL timebase) |
| **DMA (Bit 20 Errata Check)** | No (manual register setup) | Manual stream setup | Yes (`UART_HAL_CheckDmaErrata`) |

---

## 2. Hardware Agnosticism & Board Definition Contract

Shared drivers must **never** hardcode pins or peripheral instances. All physical hardware mappings are injected via configuration structures or supplied via `board_def.h`.

### Console UART Definitions (`board_def.h`)
Every project defining a console via `uart_hal` must declare the following macros in its `board_def.h`:

```c
#define BOARD_CONSOLE_UART_INSTANCE       USART3
#define BOARD_CONSOLE_TX_PORT             GPIOD
#define BOARD_CONSOLE_TX_PIN              GPIO_PIN_8
#define BOARD_CONSOLE_TX_AF               GPIO_AF7_USART3
#define BOARD_CONSOLE_RX_PORT             GPIOD
#define BOARD_CONSOLE_RX_PIN              GPIO_PIN_9
#define BOARD_CONSOLE_RX_AF               GPIO_AF7_USART3
```

If any macro is missing, `uart_hal.c` emits a compile-time `#error` explaining the requirement.

---

## 3. Usage Examples

### A. Emergency Pre-Boot Console on Cortex-M7 (`_bm`)
Usable before `SystemClock_Config()`, `MPU_Config()`, or `HAL_Init()`:

```c
#include "uart_bm.h"

static UartBm_Handle_t g_dbg_uart;

void EarlyDebug_Init(void)
{
    /* At reset, USART234578SEL default source is HSI (64 MHz) */
    UartBm_Pin_t tx_pin = { .port = GPIOD, .pin_idx = 8, .af = 7 };
    UartBm_Pin_t rx_pin = { .port = GPIOD, .pin_idx = 9, .af = 7 };

    UartBm_Config_t cfg = {
        .instance        = USART3,
        .kernel_clock_hz = 64000000UL, /* 64 MHz HSI */
        .baudrate        = 115200UL,
        .data_bits       = UART_BM_DATA_BITS_8,
        .stop_bits       = UART_BM_STOP_BITS_1,
        .parity          = UART_BM_PARITY_NONE,
        .oversampling_8  = false,
        .fifo_enable     = false,
        .hw_flow         = UART_BM_HW_FLOW_NONE,
        .enable_clock    = true,
        .tx_pin          = &tx_pin,
        .rx_pin          = &rx_pin
    };

    if (UART_BM_Init(&g_dbg_uart, &cfg))
    {
        const char msg[] = "[boot] CPU1 alive before clock tree\r\n";
        UART_BM_Write(&g_dbg_uart, (const uint8_t *)msg, sizeof(msg) - 1, 100000);
    }
}
```

### B. High-Speed Interrupt RX with Circular Buffer on Cortex-M4 (`_ll`)

```c
#include "uart_ll.h"

static UartLL_Handle_t     g_m4_uart;
static uint8_t             g_rx_raw_buf[256];
static UartLL_RingBuffer_t g_rx_ring = {
    .data = g_rx_raw_buf,
    .size = sizeof(g_rx_raw_buf),
    .head = 0,
    .tail = 0
};

void USART2_IRQHandler(void)
{
    UART_LL_IrqService(&g_m4_uart, &g_rx_ring);
}

void M4_Uart_Init(void)
{
    UartLL_Pin_t tx = { .port = GPIOD, .pin_idx = 5, .af = 7 };
    UartLL_Pin_t rx = { .port = GPIOD, .pin_idx = 6, .af = 7 };

    UartLL_Config_t cfg = {
        .instance        = USART2,
        .kernel_clock_hz = 100000000UL, /* 100 MHz APB1 */
        .baudrate        = 921600UL,
        .data_bits       = LL_USART_DATAWIDTH_8B,
        .stop_bits       = LL_USART_STOPBITS_1,
        .parity          = LL_USART_PARITY_NONE,
        .oversampling    = LL_USART_OVERSAMPLING_16,
        .fifo_enable     = true,
        .hw_flow         = LL_USART_HWCONTROL_NONE,
        .enable_clock    = true,
        .tx_pin          = &tx,
        .rx_pin          = &rx
    };

    if (UART_LL_Init(&g_m4_uart, &cfg))
    {
        UART_LL_EnableRxIrq(&g_m4_uart, USART2_IRQn, 5);
    }
}
```

### C. Standard HAL Console Output on Cortex-M7 (`_hal`)

```c
#include "uart_hal.h"

void App_Init(void)
{
    /* Reads instance and pins from board_def.h */
    UART_Console_Init(115200);
    UART_Console_Write((const uint8_t *)"Console ready\r\n", 15);
}
```

---

## 4. Silicon Errata Compliance (ES0445)

* **ES0445 §2.20.3 & §2.21.1 (DMA Stream Lock)**:
  When using DMA1/DMA2 streams with USART peripherals, bit 20 (`DMA_SxCR_TRBUFF`) of the DMA stream control register must be set while the stream is disabled to prevent transfer stalls under concurrent requests.
  *(Note: on STM32H755, LPUART1 is located in domain D3 and routes exclusively to DMAMUX2 / BDMA where BDMA channels do not have bit 20).*
  `UART_HAL_CheckDmaErrata(hdma)` verifies that the target handle is a valid DMA1/DMA2 stream, validates the request is a USART/UART request, confirms the stream is disabled (`EN=0`), and sets/verifies bit 20.
* **ES0445 §2.21.2 (LPUART Low BRR Clock Ratio)**:
  LPUART hardware generates transfer errors if the clock-to-baud ratio $f_{ker} / \text{baud}$ is in $[3, 4]$ unless it is an exact integer. The driver validates this ratio and chooses prescalers avoiding non-integer ratios.
