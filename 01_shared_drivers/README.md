# STM32H755 Shared Drivers Library (`01_shared_drivers`)

Centralized, multi-project driver library for the STM32H755ZI dual-core microcontroller.

## Architecture & Layering Policy
All drivers in this directory follow **Plan A (Peripheral-First Architecture)** and the **3-Tier Driver Abstraction** specified in [`00_docs_stm32h7/DRIVER_ARCHITECTURE_GUIDELINES.md`](../00_docs_stm32h7/DRIVER_ARCHITECTURE_GUIDELINES.md):

* **`*_bm.[c|h]`**: Pure Bare-Metal register manipulation. Zero overhead, deterministic, for Cortex-M4 and boot/fault diagnostics.
* **`*_ll.[c|h]`**: ST Low-Level API inline wrappers (`stm32h7xx_ll_*.h`). High performance, low footprint.
* **`*_hal.[c|h]`**: ST Hardware Abstraction Layer (`stm32h7xx_hal_*.h`). State machines, handle structs, callbacks for Cortex-M7 FreeRTOS / lwIP.

## Directory Layout

The numeric prefix encodes **bring-up order, not the alphabet**. Read the list top
to bottom and you have the initialisation sequence of a board: pins before
anything that uses them, a blinking LED before there is a console, DMA before the
peripherals whose descriptors live in it, and the inter-core semaphore last
because it needs both cores already running.

```text
01_shared_drivers/
├── CMakeLists.txt              # CMake INTERFACE library target (stm32h7_shared_drivers)
├── README.md                   # This document
├── 00_gpio/                    # GPIO configuration & EXTI interrupts - prerequisite of everything below
├── 01_led/                     # Board status and 3-bit boot diagnosis - works before the clock tree
├── 02_dma/                     # DMA1, DMA2, BDMA, MDMA alignment and cache helpers
├── 03_tim/                     # General, Advanced, Basic & Low-Power Timers - time base
├── 04_uart/                    # USART1 - USART8, LPUART1 - console / log, first thing you want talking
├── 05_i2c/                     # I2C1 - I2C4 sensor interfaces
├── 06_spi/                     # SPI1 - SPI6 high-speed serial
├── 07_adc/                     # ADC1, ADC2, ADC3 (ADC sampling, analog watchdogs)
├── 08_eth/                     # Ethernet MAC + LAN8742 PHY - depends on 02_dma (descriptors in SRAM3)
├── 09_fdcan/                   # FDCAN1, FDCAN2 industrial communications
└── 10_hsem/                    # Hardware Semaphore for M7 <-> M4 IPC - needs both cores alive
```

> **Adding a peripheral:** the prefix is a position in the boot sequence, so a new
> driver takes the number of the slot it belongs in, and the ones below it shift
> up. Renumbering is a `git mv` plus one edit in `CMakeLists.txt` - do it, rather
> than parking the newcomer at the end where it lies about when it initialises.

## Rules for AI Assistants & Developers
1. **Never hardcode GPIO pins or peripheral instances** into these drivers. Hardware configurations must be injected via configuration structures.
2. **Never change existing function signatures**. Always maintain backward compatibility.
3. Every driver file **must carry its tier suffix** (`_bm`, `_ll`, `_hal`).
4. **The numeric prefix never appears in an `#include`.** Each directory is on the
   include path, so it stays `#include "uart_hal.h"`, not `"04_uart/uart_hal.h"` -
   renumbering must never touch application code.
