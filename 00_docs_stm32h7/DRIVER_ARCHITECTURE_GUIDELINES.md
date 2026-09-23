# STM32H755 Driver Architecture, Plan A Organization & Portability Guide

> **Target Audience:** AI Coding Assistants (Antigravity, Claude Code) and Embedded Firmware Engineers.  
> **Target Hardware:** STM32H755ZI Dual-Core (Cortex-M7 @ 400/480 MHz + Cortex-M4 @ 200/240 MHz).  
> **Mandatory Rule for AI Assistants:** You **MUST** review and comply with this guide before creating folders, generating peripheral drivers, or modifying shared architectural components.

---

## 1. Executive Summary & Architectural Policy

To guarantee code reusability, strict real-time determinism, and prevent cross-project breakage across multiple firmware targets, all drivers in this workspace strictly follow **Plan A (Peripheral-First Architecture)** combined with a **3-Tier Abstraction Layer**.

### Core Tenets:
1. **Plan A Directory Grouping**: Drivers are organized by **peripheral first** (`04_uart/`, `06_spi/`, `03_tim/`), not by abstraction layer. All implementations and configurations of a peripheral reside together.
2. **Strict Layer Suffixes**: Every source and header file must clearly indicate its abstraction level using explicit suffixes: `*_hal.[c|h]`, `*_ll.[c|h]`, or `*_bm.[c|h]`.
3. **Hardware Agnosticism**: Shared drivers must **never** hardcode physical pins (`PD8`, `PA9`) or fixed peripheral instances (`USART3`). Hardware configurations and pin muxing must be injected by the project application or Board Support Package (BSP).
4. **Non-Destructive API Evolution**: Public function signatures are immutable. New capabilities are introduced through additive extensions and backwards-compatible wrappers.

---

## 2. Directory Layout & Plan A Specification

### A. Location Options

The architecture supports two deployment locations:

1. **Global Shared Drivers (`01_shared_drivers/`) [RECOMMENDED]**:
   Located at the workspace root, accessible to all firmware projects (`prj_eth_lwip_node`, `prj_motor_controller`, etc.). Ideal for long-term scalability and centralized silicon errata propagation.
2. **Project-Local Drivers (`Custom_Drivers/`)**:
   Located inside an individual project directory (`02_firmware_projects/<prj_name>/Custom_Drivers/`) when a standalone, self-contained repository is required.

### B. Canonical Plan A Tree Structure

```text
01_shared_drivers/
├── CMakeLists.txt                      # Central CMake target defining interface library
│
│   # The NN_ prefix is bring-up order, not the alphabet: read top to bottom and
│   # you have a board's initialisation sequence. It never appears in an #include.
│
├── 00_gpio/                            # General-Purpose I/O & EXTI - prerequisite of everything below
│   ├── .gitkeep                        # Preserves empty folder in Git
│   ├── gpio_ll.h  / gpio_ll.c
│   └── gpio_bm.h  / gpio_bm.c
│
├── 01_led/                             # Board status & Diagnostic LEDs
│   └── led_bm.h   / led_bm.c           # Zero-overhead diagnostics, works before the clock tree
│
├── 02_dma/                             # DMA1, DMA2, BDMA, MDMA helpers
│   ├── .gitkeep
│   ├── dma_bm.h   / dma_bm.c
│   └── dma_ll.h   / dma_ll.c
│
├── 03_tim/                             # General, Advanced & Basic Timers - time base
│   ├── .gitkeep
│   ├── tim_hal.h  / tim_hal.c
│   ├── tim_ll.h   / tim_ll.c
│   └── tim_bm.h   / tim_bm.c
│
├── 04_uart/                            # UART / USART / LPUART - console, first thing you want talking
│   ├── .gitkeep
│   ├── uart_hal.h / uart_hal.c         # ST HAL implementation (for M7 / FreeRTOS)
│   ├── uart_ll.h  / uart_ll.c          # ST Low-Level wrapper (fast, low footprint)
│   └── uart_bm.h  / uart_bm.c          # Pure Bare-Metal direct register access
│
├── 05_i2c/                             # I2C peripheral
│   ├── .gitkeep
│   ├── i2c_hal.h  / i2c_hal.c
│   └── i2c_ll.h   / i2c_ll.c
│
├── 06_spi/                             # SPI peripheral
│   ├── .gitkeep
│   ├── spi_hal.h  / spi_hal.c
│   ├── spi_ll.h   / spi_ll.c
│   └── spi_bm.h   / spi_bm.c
│
├── 07_adc/                             # Analog-to-Digital Converter
│   ├── README.md                       # Architecture, BOOST table, errata, examples
│   ├── adc_hal.h  / adc_hal.c          # ST HAL tier (for M7 / multi-instance handles)
│   ├── adc_ll.h   / adc_ll.c           # ST LL tier (zero HAL dependencies, LL inlines)
│   └── adc_bm.h   / adc_bm.c           # Pure Bare-Metal direct register access
│
├── 08_eth/                             # 10/100 Ethernet MAC & PHY - depends on 02_dma
│   ├── eth_hal.h  / eth_hal.c          # HAL MAC driver + ES0445 errata workarounds
│   └── lan8742_phy.h / lan8742_phy.c   # SMI/MDIO PHY transceiver driver
│
├── 09_fdcan/                           # FDCAN1, FDCAN2
│   └── .gitkeep
│
└── 10_hsem/                            # Hardware Semaphore (Dual-Core IPC) - needs both cores alive
    ├── .gitkeep
    └── hsem_bm.h  / hsem_bm.c          # Direct lock/unlock for shared SRAM4
```

> **The numbering is global.** A project-local `Custom_Drivers/` uses the same
> prefixes as `01_shared_drivers/`, so a driver is promoted with a plain move and
> keeps its place in the boot sequence. A project only carries the slots it needs
> - `prj_eth_lwip_node` has `01_led/`, `04_uart/`, `08_eth/` and the gaps are the
> peripherals that product does not drive.

> [!IMPORTANT]
> **Git Tracking Rule:** Git does **not** track empty directories. Whenever creating empty peripheral folders before implementation, always include a `.gitkeep` file or a boilerplate header file with `#pragma once`.

---

## 3. The 3-Tier Driver Abstraction & File Naming

File names **must** be globally unique across the workspace. Never create a file named `uart.c` or `spi.c` without its layer suffix. This prevents object file collisions in CMake/Ninja (`uart.c.obj`) and eliminates ambiguity during GDB debugging.

| Layer | File Suffix | Description & Constraints | Target Core & Typical Use Case |
| :--- | :--- | :--- | :--- |
| **Bare-Metal** | `*_bm.[c\|h]` | **Direct register manipulation** (`PERIPH->REG = VAL;`). Zero function call overhead, no ST library dependencies, atomic bit manipulation where possible. | **Cortex-M4**: Deterministic real-time loops, DSP, sub-microsecond control, pre-MPU/fault diagnostics. |
| **Low-Level** | `*_ll.[c\|h]` | **ST LL inline wrappers** (`stm32h7xx_ll_*.h`). Very lightweight, avoids bulky HAL handle structures and state machines. | **Cortex-M4 or M7**: Standard communication streams (UART, SPI, I2C), PWM generation, timer triggers. |
| **HAL** | `*_hal.[c\|h]` | **ST HAL library** (`stm32h7xx_hal_*.h`). Utilizes handle pointers (`UART_HandleTypeDef`), state machines, callback architectures, and OS hooks. | **Cortex-M7**: Complex protocol stacks (FreeRTOS integration, lwIP Ethernet, USB CDC/HID, SDMMC). |

---

## 4. Essential Peripheral Registry for STM32H755ZI

When scaffolding or implementing drivers for this MCU, prioritize this baseline set:

The list number is the directory prefix, so this registry and the tree above are the
same ordering: implement downwards and every driver finds its dependencies already there.

0. **`00_gpio/` (GPIO & EXTI)**: High-speed pin toggling, external interrupt event handling. Everything below configures its pins through this.
1. **`01_led/` (Diagnostic GPIO)**: Direct register writes (`_bm`). Used for 3-bit start-up failure codes before clocks/MPU and in fault handlers.
2. **`02_dma/` (DMA1, DMA2, BDMA, MDMA)**: Centralized 32-byte alignment assertions, cache flush/invalidate macros, domain mapping.
3. **`03_tim/` (TIM1–TIM17, LPTIM)**: FreeRTOS timebase tick (TIM6), deterministic PWM motor control, capture/compare.
4. **`04_uart/` (USART1–8, LPUART1)**: ST-LINK VCP console (`_hal`), sensor telemetry streams (`_ll`), pre-boot emergency logging (`_bm`). Apply DMA bit 20 workaround (ES0445 2.20.3).
5. **`05_i2c/` (I2C1–I2C4)**: Environmental sensors, PMICs, EEPROMs. BERR interrupt errata compliance (ES0445 2.19.2).
6. **`06_spi/` (SPI1–SPI6)**: Inter-chip communication, IMUs, external ADCs, display drivers.
7. **`07_adc/` (ADC1, ADC2, ADC3)**: Fast multi-channel analog sampling triggered by timers on Cortex-M4.
8. **`08_eth/` (Ethernet MAC + LAN8742 PHY)**: RMII mode, D2 SRAM3 descriptor placement (`0x30040000`), FEP error packet fix (ES0445 2.25.10). Needs `02_dma`.
9. **`09_fdcan/` (FDCAN1, FDCAN2)**: Industrial and automotive high-speed CAN FD communications.
10. **`10_hsem/` (Hardware Semaphore)**: Non-blocking and blocking atomic locks for inter-core IPC in SRAM4 (`0x38008000`). Last: needs both cores running.

---

## 5. Architectural Risk Mitigation Strategies

### Risk 1: Breaking Changes & The Domino Effect
*Problem:* Modifying a function in `01_shared_drivers/06_spi/spi_ll.c` for Project B inadvertently breaks Project A.

#### Mitigations:
1. **Open/Closed Principle in C**: Never alter the signature or return type of an existing public function. If additional parameters (e.g., timeout, DMA flags) are required, create an extended variant:
   ```c
   /* Existing API (Preserved for backwards compatibility) */
   bool SPI_LL_Transmit(SPI_TypeDef *instance, const uint8_t *pData, size_t length);

   /* New Extended API */
   bool SPI_LL_Transmit_Timeout(SPI_TypeDef *instance, const uint8_t *pData, size_t length, uint32_t timeout_ms);
   ```
2. **Semantic Versioning & Git Submodules**: If `01_shared_drivers` is a standalone Git repository, projects link to it via Git Submodule pinned to a specific tag (`v1.0.0`) or commit hash. Upgrades are explicit and opt-in per project.
3. **Multi-Project CI Matrix Build**: Continuous Integration runs automated compilation of all active projects in `02_firmware_projects/` on every commit to `01_shared_drivers/`. Any compilation breakage is flagged immediately before merging.

---

### Risk 2: Hardware Coupling & Pin Hardcoding
*Problem:* Hardcoding pin definitions (`PD8`, `PA9`) or peripheral instances (`USART3`) inside shared drivers renders them unusable across different boards or projects.

#### Mitigations:
1. **Config Struct Dependency Injection**: The shared driver operates solely on a generic handle and configuration struct provided by the caller:
   ```c
   /* In 01_shared_drivers/04_uart/uart_ll.h */
   typedef struct {
       USART_TypeDef *instance;    /* e.g., USART1, USART3, etc. */
       uint32_t       baudrate;    /* 115200, 921600, etc. */
       uint32_t       data_bits;
       uint32_t       stop_bits;
       uint32_t       parity;
   } UartLL_Config_t;

   typedef struct {
       UartLL_Config_t config;
       uint32_t        error_flags;
   } UartLL_Handle_t;

   bool UART_LL_Init(UartLL_Handle_t *handle, const UartLL_Config_t *config);
   bool UART_LL_WriteByte(UartLL_Handle_t *handle, uint8_t byte);
   ```
2. **MSP / BSP Separation of Concerns**:
   * **The Shared Driver**: Controls register bit manipulation (`CR1`, `BRR`, `ISR`), FIFO state, and silicon erratas. It has zero knowledge of GPIO pins.
   * **The Application / BSP / CubeMX (`stm32h7xx_hal_msp.c`)**: Configures RCC peripheral clocks and GPIO alternate function pin multiplexing before calling the shared driver initialization.

---

### Risk 3: Fragile Relative Paths in CMake
*Problem:* Hardcoding `../../01_shared_drivers` breaks if a project is cloned independently, moved, or built inside an isolated CI/CD container.

#### Mitigations:
1. **Configurable Path Variable with Fallback in Project `CMakeLists.txt`**:
   ```cmake
   # Allow CLI override: cmake -B build -DSHARED_DRIVERS_DIR="/custom/path"
   if(NOT DEFINED SHARED_DRIVERS_DIR)
       if(DEFINED ENV{STM32_SHARED_DRIVERS})
           set(SHARED_DRIVERS_DIR "$ENV{STM32_SHARED_DRIVERS}")
       else()
           # Default relative fallback to monorepo workspace structure
           set(SHARED_DRIVERS_DIR "${CMAKE_CURRENT_LIST_DIR}/../../01_shared_drivers")
       endif()
   endif()

   # Early assertion with clear actionable diagnosis
   if(NOT EXISTS "${SHARED_DRIVERS_DIR}/CMakeLists.txt")
       message(FATAL_ERROR 
           "\n[ERROR] Shared drivers not found at: ${SHARED_DRIVERS_DIR}\n"
           "Fix:\n"
           "  1. Verify workspace relative path: ../../01_shared_drivers\n"
           "  2. Or provide CLI argument: cmake -DSHARED_DRIVERS_DIR=<path> ..\n"
           "  3. Or define environment variable STM32_SHARED_DRIVERS\n"
       )
   endif()

   add_subdirectory("${SHARED_DRIVERS_DIR}" "${CMAKE_BINARY_DIR}/shared_drivers")
   ```

---

### Risk 4: Cross-Machine Project Transfer & Build Hygiene
*Problem:* Copying or transferring a project directory between PCs fails due to stale build cache or path mismatches.

#### Mitigations:
1. **The Golden Rule:** **NEVER copy, archive, or commit the `build/` directory.**  
   CMake caches absolute filesystem paths (`C:/Users/hrodriguez/...`). Transferring `build/` to another machine causes immediate and obscure compilation failures.
2. **Transfer Checklist**:
   * Include: `CMakeLists.txt`, `CMakePresets.json`, `cmake/`, `CM7/`, `CM4/`, `Common/`, `tools/`, and `01_shared_drivers/`.
   * Exclude: `build/`, `.ninja_*`, `*.elf`, `*.bin`, `*.hex`, `*.log`.
3. **Clean Build Protocol on New Machine**:
   ```bash
   # 1. Navigate to project root
   cd stm32h755/02_firmware_projects/prj_<name>

   # 2. Configure build cleanly from scratch
   cmake -B build -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/arm-none-eabi.cmake

   # 3. Compile both cores
   ninja -C build

   # 4. Flash target MCU
   python tools/flash.py --tool cubeprog
   ```

---

## 6. Checklist for AI Assistants Before Action

When instructed to create, extend, or refactor firmware drivers:

- [ ] **Check Layout**: Are files placed under `<peripheral>/<peripheral>_{hal,ll,bm}.[ch]` following Plan A?
- [ ] **Check Layer Suffix**: Is the file explicitly suffixed (`_hal`, `_ll`, `_bm`)?
- [ ] **Check Hardware Coupling**: Are peripheral instances and pins passed via config structs rather than hardcoded?
- [ ] **Check D-Cache Alignment**: If Cortex-M7 DMA is involved, is the buffer 32-byte aligned (`__attribute__((aligned(32)))`), multiple of 32 bytes, and backed by `__DMB()`?
- [ ] **Check Silicon Errata**: Has `ES0445` been checked for the target peripheral (e.g., bit 20 on USART DMA, FEP on Ethernet, LSPEN on CM4)?
- [ ] **Check Git Persistence**: Did you include a `.gitkeep` if creating empty scaffolding directories?
