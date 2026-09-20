# STM32H755 Development Guidelines (Claude Code / Antigravity)

## 1. MCU Architecture Overview
- **Device**: STM32H755ZI (Dual-core Arm Cortex-M).
  - **Core 1 (CPU1)**: Cortex-M7 @ up to 480 MHz, with 32 KB I-Cache and 32 KB D-Cache, FPU (Double-precision), MPU.
  - **Core 2 (CPU2)**: Cortex-M4 @ up to 240 MHz, FPU (Single-precision), MPU.
- **Power Domains & Bus Matrix**:
  - **D1 Domain (CD)**: Cortex-M7, AXI SRAM (`0x24000000`, 512 KB), ITCM (`0x00000000`, 64 KB), DTCM (`0x20000000`, 128 KB), MDMA, High-speed peripherals.
  - **D2 Domain (CD)**: Cortex-M4, SRAM1 (`0x30000000`, 128 KB), SRAM2 (`0x30020000`, 128 KB), SRAM3 (`0x30040000`, 32 KB), DMA1, DMA2, Ethernet, USB, standard peripherals.
  - **D3 Domain (SRD)**: SRAM4 (`0x38000000`, 64 KB), BDMA, RTC, I2C4, LPUART, low-power domain.

---

## 2. Mandatory Rules for Code Generation

### A. D-Cache and DMA Coherency (CRITICAL)
Whenever using DMA (DMA1, DMA2, MDMA, BDMA) with Cortex-M7:
1. **Buffer Alignment**: All DMA buffers MUST be aligned to the 32-byte cache line size:
   ```c
   uint8_t tx_buffer[BUFFER_SIZE] __attribute__((aligned(32)));
   ```
2. **Buffer Sizing**: Buffer size MUST be a multiple of 32 bytes to avoid cache line sharing corruption with adjacent variables.
3. **Cache Maintenance**:
   - **Before DMA TX (Memory -> Peripheral)**: Clean the D-Cache:
     ```c
     SCB_CleanDCache_by_Addr((uint32_t*)tx_buffer, BUFFER_SIZE);
     ```
   - **Before/After DMA RX (Peripheral -> Memory)**: Invalidate the D-Cache:
     ```c
     SCB_InvalidateDCache_by_Addr((uint32_t*)rx_buffer, BUFFER_SIZE);
     ```
   - *Alternative*: Place DMA buffers in a dedicated MPU-configured non-cacheable region (e.g. SRAM4 or a specific section of SRAM1/2).
4. **Memory Barrier (ERRATA ES0445 2.1.5)**: a `__DMB()` MUST follow every cache
   maintenance call before the next store to that buffer:
   ```c
   SCB_InvalidateDCache_by_Addr((uint32_t*)rx_buffer, BUFFER_SIZE);
   __DMB();
   ```
5. **Never** configure an MPU region as write-through to solve coherency
   (ERRATA ES0445 2.1.1). Use non-cacheable, or write-back + explicit maintenance.
6. See detailed guide in: `docs/00_core_and_system/cache_mpu_rules.md`.

### B. Memory Domain Placement for DMA Controllers
- **DMA1 / DMA2**: Cannot directly access DTCM or ITCM. Buffers MUST reside in **D2 SRAM1/2/3** (`0x30000000`) or AXI SRAM via bus matrix.
- **BDMA**: Only accesses **D3 SRAM4** (`0x38000000`) and D3 peripherals (e.g., LPUART1, SPI6, I2C4).
- **MDMA**: Can access virtually all memories (AXI SRAM, D2 SRAM, D3 SRAM, TCM, Flash, External memories).
- **Ethernet DMA Descriptors**: MUST reside in D2 SRAM (`0x30000000` or `0x30040000`).
- See detailed memory map in: `STM32H755_Memory_Map.md` and `docs/00_core_and_system/memory_and_domains.md`.

### C. Pinout & Alternate Functions (AF)
- Always check `STM32H755_Pin_Definitions.md` and `STM32H755_Alternate_Functions.md` before generating GPIO / peripheral pin configurations.
- Verify that the chosen pin supports the required AF mode (e.g., USART3_TX on `PD8` AF7, USART3_RX on `PD9` AF7).

### D. Dual-Core Synchronization (HSEM & IPC)
- CPU1 (M7) initializes common clocks (RCC, PLLs) and power supplies before releasing CPU2 (M4).
- Hardware Semaphore (**HSEM**) must be used to protect shared peripheral access and coordinate boot/initialization.
- Shared data between M7 and M4 should reside in shared SRAM (e.g., SRAM3 `0x30040000` or SRAM4 `0x38000000`).
- See guide in: `docs/00_core_and_system/dual_core_boot_ipc.md` and `docs/01_drivers_and_hal/AN5617_dualcore_ipc_openamp.pdf`.

### E. FreeRTOS & RTOS Rules (CRITICAL)
Whenever generating FreeRTOS tasks or ISR code:
1. **NVIC Priority Grouping**: Always set `HAL_NVIC_SetPriorityGrouping(NVIC_PRIORITYGROUP_4)`. All 4 bits must be preemption priority (no subpriorities).
2. **SysCall Interrupt Priority**: Any ISR calling `...FromISR()` FreeRTOS functions MUST have a priority numerically >= `configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY` (e.g. priority 5 to 15).
3. **HAL Timebase Conflict**: Do NOT use SysTick for `HAL_GetTick()` / `HAL_Delay()` when FreeRTOS is active. Use a basic timer (**TIM6** or **TIM7**) as the HAL timebase.
4. Reference guide: `docs/01_drivers_and_hal/FreeRTOS/README.md`.

---

### F. Silicon Errata (MANDATORY CHECK)
Before generating driver init code, consult `docs/00_core_and_system/errata_summary.md`.
Non-negotiable workarounds for this device:
1. **USART / LPUART + DMA**: the DMA stream locks permanently unless bit 20 of
   `DMA_SxCR` is set on that stream (ES0445 2.20.3 / 2.21.1). Apply it after
   `HAL_DMA_Init()` and before enabling the stream.
2. **WWDG**: set `RCC_GCR.WW1RSC` (CPU1) / `WW2RSC` (CPU2) so the watchdog resets the
   whole system. A CPU-scoped watchdog reset locks the AXI domain (ES0445 2.3.1).
3. **480 MHz / VOS0**: unavailable on silicon revisions Y and W. Verify `REV_ID` in
   `DBGMCU_IDC` before generating a 480 MHz clock tree; otherwise target 400 MHz
   with VOS1 (ES0445 2.3.5).
4. **Ethernet**: always RMII, never MII on this package (ES0445 2.3.11). Set
   `ETH_MTLRXQOMR.FEP` if a 10 Mbps link is possible (ES0445 2.25.10).
5. **LPTIM**: disable it by pulsing `LPTIMxRST` in RCC, never by clearing `ENABLE`
   (ES0445 2.17.1).
6. **I2C master**: on a `BERR` interrupt, clear the flag and continue. Do not abort
   the transfer (ES0445 2.19.2).
7. **Cortex-M4 FPU**: clear `LSPEN` in `FPCCR`, or guarantee every ISR has more than
   2 instructions besides the exception return (ES0445 2.2.2).

### G. Memory Layout & Linker
- Ready-made dual-core linker scripts, MPU configuration and the M7/M4 shared-memory
  block live in `docs/00_core_and_system/linker_templates/`.
- Section attributes to use instead of plain globals:
  - `.dma_buf`    -> SRAM1 `0x30000000`, non-cacheable, for DMA1 / DMA2.
  - `.eth_ram`    -> SRAM3 `0x30040000`, non-cacheable, for ETH descriptors.
  - `.d3_ram`     -> SRAM4 `0x38000000`, non-cacheable, for BDMA / D3 peripherals.
  - `.shared_ram` -> SRAM4 `0x38008000`, non-cacheable, for M7 <-> M4 IPC.
  - `.axi_ram`    -> AXI SRAM `0x24000000`, cacheable, for bulk buffers.
  - `.itcm_text`  -> ITCM `0x00000000`, for time-critical functions.
- `MPU_Config()` must run **before** `SCB_EnableDCache()`.

---

## 3. Peripheral Workflow
When implementing or debugging a peripheral:
1. Navigate to the peripheral folder: `docs/peripherals/<peripheral_name>/`.
2. Review the quick guide: `README.md` (pinout patterns, clocks, DMA, and cache considerations).
3. If deep register-level information is needed, use the **extracted RM0399 chapter**
   inside that peripheral folder. Every chapter exists twice:
   - **`.txt`** - plain text, **searchable with `grep`**. Start here to locate a register,
     a bit name or a procedure.
     ```bash
     grep -rn --include='*.txt' 'OVRMOD' docs/peripherals/adc_dac/
     ```
   - **`.pdf`** - same chapter, correct layout. Open it for register bit diagrams,
     figures and wide tables, which do not survive text extraction.
   The HAL/LL API manual, the errata sheet and the datasheet also have `.txt` twins:
   `docs/01_drivers_and_hal/UM2217_stm32h7_hal_ll_manual.txt`,
   `docs/00_core_and_system/ES0445_stm32h755_errata_sheet.txt`,
   `docs/00_core_and_system/DS12923_stm32h755zi_datasheet.txt`.
4. Cross-reference pin multiplexing in `STM32H755_Alternate_Functions.md` and `STM32H755_Pin_Definitions.md`.
5. If using DMA on Cortex-M7, strictly enforce `docs/00_core_and_system/cache_mpu_rules.md` (32-byte alignment and cache flush/invalidation).
6. To check official ST tested code patterns:
   - Official API manual: `docs/01_drivers_and_hal/UM2217_stm32h7_hal_ll_manual.pdf`.
   - Ready-to-run Nucleo board examples: `docs/01_drivers_and_hal/STM32CubeH7/Projects/NUCLEO-H745ZI-Q/Examples/`.
     **Note**: the `NUCLEO-H755ZI-Q/` folder of the Cube package contains only a
     `readme.txt`. `NUCLEO-H745ZI-Q` is the equivalent dual-core board and is the
     valid reference for this project. Do not search under `NUCLEO-H755ZI-Q/`.
     This Cube copy is **pruned**: only `Drivers/`, `Middlewares/` and the
     NUCLEO-H745ZI-Q project remain, and the git history was removed. Details in
     `docs/01_drivers_and_hal/STM32CubeH7/PRUNED.md`.
7. Check `docs/00_core_and_system/errata_summary.md` for the peripheral being
   implemented before finalizing the code.

---

## 4. Driver Architecture, Plan A & Portability Guidelines (MANDATORY)
Before creating directories, adding new peripheral drivers, or modifying shared architecture:
1. **Strictly Consult**: `DRIVER_ARCHITECTURE_GUIDELINES.md` for Plan A (Peripheral-First directory layout: `<driver_root>/<peripheral>/<peripheral>_{hal,ll,bm}.[ch]`).
2. **Never Hardcode Pins**: Shared drivers must receive instances and parameters via configuration structs (Dependency Injection). Pins and clocks are configured in the project/MSP/BSP layer.
3. **Immutable APIs**: Never modify existing public function signatures. Use non-destructive extensions to avoid breaking existing projects.
4. **Git Persistence**: Always add `.gitkeep` files when scaffolding empty peripheral directories.
5. **Build Hygiene**: Never copy or commit the `build/` directory across machines.
