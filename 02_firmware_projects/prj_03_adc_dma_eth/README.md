# prj_03_adc_dma_eth

Dual-Core STM32H755ZI firmware product with high-speed 16-bit ADC1 + circular DMA1 on Cortex-M4 (Domain D2, SRAM2), TIM3 TRGO hardware triggering at 500 kSPS, inter-core telemetry sharing via SRAM4 under HSEM 4, and on-demand UDP telemetry server (:5001) over 10/100 Ethernet (RMII + LAN8742 PHY) on Cortex-M7 with FreeRTOS and lwIP.

Includes host utilities for CLI queries (`tools/query_adc.py`) and a real-time web dashboard (`tools/adc_dashboard.py`).

Built according to the workspace **Plan A 3-Tier Architecture** consuming centralized drivers from `01_shared_drivers/`.

---

## 1. Core Architecture & System Roles

| Core | Image / Placement | Operating Model | Peripheral & Memory Ownership |
| :--- | :--- | :--- | :--- |
| **CPU1 (Cortex-M7)** | `cm7_firmware.elf`<br>Flash Bank 1 (`0x08000000`) | FreeRTOS (Priority grouping 4) | - Power (`PWR`), system clocks (`RCC`, PLL1 @ 400 MHz, PLL2P @ 25 MHz for ADC)<br>- Console VCP (`USART3` via `04_uart`)<br>- Ethernet MAC & DMA (`08_eth` via RMII) + LAN8742A PHY<br>- lwIP TCP/IP stack (IPv4, ICMP, UDP :5001)<br>- Descriptors in D2 SRAM3 (`0x30040000`, non-cacheable)<br>- Releases CPU2 via HSEM 0 |
| **CPU2 (Cortex-M4)** | `cm4_firmware.elf`<br>Flash Bank 2 (`0x08100000`) | Bare-Metal (No RTOS) | - ADC1 (16-bit, BOOST=10, 25 MHz kernel clock) on `PB1`<br>- TIM3 (TRGO on Update Event @ 500,000 Hz)<br>- DMA1 Stream 0 (DMAMUX Request 9, circular mode)<br>- DMA circular buffer in D2 SRAM2 (`0x30020000`, 1024 samples)<br>- Computes statistics (min, max, avg, mV) in super-loop<br>- Publishes telemetry + 512-sample waveform to SRAM4 (`0x38008000`) under HSEM 4 |

---

## 2. Hardware Allocation & Pinout

| Signal / Role | Pin | Nucleo Connector | Description |
| :--- | :--- | :--- | :--- |
| **Analog Input** (default) | `PB1` | CN9 pin 4 (A3) | ADC12_INP5 single-ended (0 to 3.3V). A **fast channel** per DS12923 Table 87 note 4, which is why it carries the 500 kSPS path |
| **Analog Input** (alternative) | `PA3` | CN9 pin 1 (A0) | ADC12_INP15 single-ended. A *slow* channel; selected by setting `BOARD_ADC_USE_FAST_CHANNEL_PB1` to `0` in `Common/Inc/board_def.h` |
| **Ethernet RMII** | `PA1, PA2, PA7, PB13, PC1, PC4, PC5, PG11, PG13` | On-board | 50 MHz reference clock from LAN8742A PHY |
| **VCP UART Console** | `PD8 (TX), PD9 (RX)` | ST-LINK VCP | 115,200 baud, 8N1 |
| **Status LEDs** | `PB0 (Green), PE1 (Yellow), PB14 (Red)` | On-board LD1-3 | Diagnostics and network state |

---

## 3. Driver Architecture Decision: `adc_hal` vs `adc_bm` (Plan §2.C)

The workspace Plan A 3-Tier architecture defines choosing between `*_bm` (pure bare-metal registers) and `*_hal` (HAL encapsulation) on a per-peripheral basis:

- **Why `adc_hal` was chosen**:
  1. **Calibration Complexity**: STM32H7 ADCs require a multi-step calibration sequence (both offset and linearity) before enablement. `HAL_ADCEx_Calibration_Start` robustly handles the internal state transitions, stabilization delays, and factory linearity tables.
  2. **DMAMUX Coupling**: Binding ADC1 to DMA1 Stream 0 requires configuring DMAMUX1 request 9. HAL's DMA layer encapsulates this cleanly during initialization with zero overhead during streaming.
  3. **Zero Overhead in Hot Path**: Once started, acquisition is 100% hardware-driven (TIM3 TRGO &rarr; ADC1 &rarr; DMA1 &rarr; SRAM2) and the driver is not on it at all. The CPU is only woken every 1.024 ms (512 samples, one half-buffer), where `ADC_HAL_ComputeStats` computes integer statistics (zero floating-point) and the super-loop publishes the snapshot. That per-half work has not been measured on hardware; `half_overrun_count` is what reports whether it fits inside the 1.024 ms budget.
  4. **Multi-Instance Scalability**: The driver maintains an instance lookup table (`s_adc_instances[3]`), cleanly paving the way for Phase 2 (ADC3 + VREFINT in Domain D3).

---

## 4. Silicon Errata Mitigations & Safety Guarantees

- **BOOST Mode (RM0399 / adc.txt:4597)**: Configured to `BOOST[1:0] = 10` for 25 MHz `adc_ker_ck`. Applied while `ADSTART=0` and `JADSTART=0`, masking all write-1-to-trigger bits (`ADSTART`, `ADCAL`, `ADSTP`).
- **Injected Context Queue (ES0445 2.10.x)**: `ADC_CFGR_JQDIS` enabled to eliminate queue corruption.
- **FPU Lazy Stacking (ES0445 2.2.2)**: `FPU->FPCCR &= ~FPU_FPCCR_LSPEN_Msk;` executed at CM4 entry to avoid register corruption during interrupt nesting.
- **Strict HSEM Isolation & Overrun Detection**: Hardware semaphores (`SHARED_LOCK`) are **NEVER** taken within interrupt handlers. DMA half-transfer and transfer-complete ISRs only record the index of the completed half, detect consumer overruns (`half_overrun_count`), and flag readiness. `ADC_HAL_GetReadyHalf` uses an atomic critical section (`PRIMASK`) to prevent dropped notifications. Statistical computation and IPC publication occur strictly in the CM4 super-loop.

---

## 4. Network Configuration (`CM7/App/Inc/net_config.h`)

| Parameter | Default Value | Notes |
| :--- | :--- | :--- |
| **Static IP** | `192.168.1.150` | Default static address |
| **Netmask** | `255.255.255.0` | `/24` subnet |
| **Gateway** | `192.168.1.1` | Default gateway |
| **UDP Telemetry Port**| `5001` | ADC query port (`GET` or `JSON` commands) |

---

## 5. Build and Flash Instructions

```powershell
# 1. Load STM32 environment
.\tools\env.ps1

# 2. Configure build presets
cmake --preset debug

# 3. Build both CM7 and CM4 images
cmake --build --preset debug

# 4. Flash both cores to Nucleo-H755ZI-Q
py tools/flash.py --tool cubeprog

# 5. Monitor logs on VCP console
py tools/monitor.py
```

---

## 6. Telemetry & Host Verification

### CLI Query Tool
```powershell
# Query current ADC readings:
py tools/query_adc.py --host 192.168.1.150

# Query as JSON:
py tools/query_adc.py --host 192.168.1.150 --json

# Continuous monitoring at 10 Hz:
py tools/query_adc.py --host 192.168.1.150 --rate 10
```

### Real-Time Web Dashboard
```powershell
py tools/adc_dashboard.py --host 192.168.1.150 --bind 127.0.0.1:8080
```
Open `http://127.0.0.1:8080` in your web browser for live chart visualization and telemetry metrics.

For detailed bring-up and calibration procedures, refer to [docs/ADC_BRINGUP.md](docs/ADC_BRINGUP.md).

### Cold boot

Since `5dd43a2`, `SystemClock_Config()` writes the supply configuration (`HAL_PWREx_ConfigSupply(PWR_DIRECT_SMPS_SUPPLY)`) before raising VOS. Earlier images waited forever for `VOSRDY` after a power-on reset (no console, no link), because `PWR_CR3` is write-once per power-on reset; they only ran when another image had written it since the last power-up. Checked after a real power cycle with nothing else run since (2026-10-12): `RCC_RSR = 0x00FE0000` (power-on), `PWR_CR3 = 0x00010044`, and `net_check.py` with `monitor.py` at the same time pass (ICMP 4/4, UDP 1-1472 B byte-identical, burst 100/100, `rx_nobuf`/`tx_nobuf`/`dma_err` 0, both cores alive). Captures: `docs/x1_cold_boot_swd_5dd43a2.txt`, `docs/x1_cold_boot_t6_netcheck_5dd43a2.txt`.

---

## 7. Cross-Core GPIO Lock Hook (`00_gpio`, G1 Resolution)

To prevent inter-core races on shared GPIO configuration registers (`MODER`, `PUPDR`, `OTYPER`, etc.), `prj_03_adc_dma_eth` installs the cross-core lock hook from `00_gpio` (closing finding G1):

- **Hardware Semaphore & Timeout**: Uses Hardware Semaphore 5 (`BOARD_GPIO_HSEM_ID 5U`) with a 60 µs timeout defined by `SHARED_GPIO_LOCK_US = 60U` in `Common/Inc/shared_memory.h`.
- **Cycle Limits**: Derived as the product of core frequency and lock timeout constant:
  - CM7: `SHARED_GPIO_LOCK_US * 400U` (24,000 cycles at 400 MHz).
  - CM4: `SHARED_GPIO_LOCK_US * 200U` (12,000 cycles at 200 MHz).
- **Telemetry & Retention Metrics in `g_shared`**: Stored at the end of `SharedMemory_t` in Domain D3 SRAM4 (`0x38008000`, 32-byte cache-line aligned):
  - `m7_gpio_lock_fail_count` / `m4_gpio_lock_fail_count`: Lock acquisition timeout counters.
  - `m7_gpio_lock_hold_cycles_max` / `m4_gpio_lock_hold_cycles_max`: Maximum recorded cycles holding the lock.
- **Hook Implementation & Installation**:
  - Implemented in `Common/Src/shared_memory.c` via static functions `shared_gpio_lock()` / `shared_gpio_unlock()` and public registration function `Shared_GpioLockInstall()`.
  - **CM7 Entry Point** (`CM7/Core/Src/main.c`): Installed immediately after `Shared_Init()` and before releasing CPU2 via `HAL_HSEM_Release(HSEM_ID_BOOT_SYNC, 0)`.
  - **CM4 Entry Point** (`CM4/Core/Src/main.c`): Installed immediately after `CPU2_WaitForCPU1()` and `HAL_Init()`, before any peripheral or pin initialization.
  - Driver link: `stm32h7_driver_hsem_bm` linked in both cores strictly for the lock hook (`HSEM_BM_Init`, `HSEM_BM_TimebaseStart`, `HSEM_BM_TakeTimeout`). The CPU2 boot release/wake mechanism remains on `HAL_HSEM` (Decision 1).
- **Writers Protected**:
  - CM4 ADC pin PB1 (`CM4/App/Src/adc_acquisition.c`): Migrated from unprotected `HAL_GPIO_Init` to `GPIO_BM_ConfigPin` protected by `s_shared_lock` (G1 resolved).
  - CM7 Console (PD8/PD9) and Ethernet (PA1, PA2, PA7, PB13, PC1, PC4, PC5, PG11, PG13): Initialized in `app_main()` after CPU2 release; protected by `s_shared_lock` via `GPIO_HAL_Init`.
  - `LED_Init()`: Initialized mono-core in CM7 before CPU2 release (safe without lock).
  - `LED_PanicRed`: Hard fault / panic handler write (accepted exception G14).

### F1c Validation Evidence

- **T1 (`docs/gpio_f1c_t1_e9e98c4.txt`)**: Clean matrix build 8/8 PASS, `selftest` preset PASS, `prj_04` `debug` preset PASS, strict `-Werror` enforced in debug and release with 0 warnings/errors.
- **T2a & T2b (`docs/gpio_f1c_t2_e9e98c4.txt`)**: T2a verified 0 unprotected HAL calls across 4 ELFs (`nm -u` per linked object; control `e6cb75e` fails on both CM4 ELFs). T2b scanned 970 compilation units with 0 raw writes; CMSIS macro autoprueba passed (12/12 writes detected, 0/9 reads ignored); T2b has no tree control in `prj_03`: its control is the self-test, and T2a covers `e6cb75e`.
- **T3 & Control (`docs/gpio_f1c_t3_c9a3d23.txt`, `docs/gpio_f1c_t3_control_c9a3d23.txt`)**: Live SWD verified `s_shared_lock` matches `g_shared_gpio_lock` on both cores (`0x8015B80` on CM7, `0x81052F0` on CM4), `HSEM_R5 = 0x0`, fail counts = 0, and PB1, PB13, PD8, PD9, PB15, PD0 exact across 5 fields. Dedicated control run confirmed `s_shared_lock == NULL` on both cores.
- **T4 (`docs/gpio_f1c_t4_c9a3d23.txt`)**: Measured retention hold cycles: CM7 = 1274 cycles (3.19 µs @ 400 MHz, 18.84× margin vs 24000), CM4 = 363 cycles (1.81 µs @ 200 MHz, 33.06× margin vs 12000), both `>= 10.0x`.
- **T5 (`docs/gpio_f1c_t5_debug_c9a3d23.txt`, `docs/gpio_f1c_t5_release_f733b98.txt`)**: Production regression in debug and release: ICMP 4/4, UDP sweep 1..1472 B, burst 100/100, ADC samples growing with `OVR == 0`, and `[net]` error counters all 0.
- **T6a (`docs/gpio_f1c_t6a_f733b98.txt`)**: 20 consecutive warm resets via SWD with 10/10 T3 checks passing after each reset.
- **T6b (`docs/gpio_f1c_t6b2_rsr_009e997.txt`, `_swd_`, `_t3_`, `_t5_netcheck_`, `_t5_query_`)**: cold boot on the bench: the owner power-cycled the board and the auditor, with no flash or reset since the power-up, read `RCC_RSR = 0x00FE0000` (power-on), ran an independent SWD read, the tool's T3 and T5: all PASS. A first power cycle (`docs/gpio_f1c_t6b_*_009e997.txt`) was voided by a host network problem.
- **T7 (`../prj_04_adc_dma_bm_eth/docs/gpio_f1c_t7_e9e98c4.txt`)**: Compatibility check for `prj_04`: 1951 compilation units clean, 0 illegal writes, control in `07db657` reproduces all 20 findings.
- **Auditor Independent Runs**: SWD (`docs/gpio_f1c_audit_swd_7ba2cf0.txt`), 20 resets (`docs/gpio_f1c_audit_t6a_7ba2cf0.txt`), T2 (`docs/gpio_f1c_audit_t2_7ba2cf0.txt`), T7 (`../prj_04_adc_dma_bm_eth/docs/gpio_f1c_audit_t7_7ba2cf0.txt`), T3 control (`docs/gpio_f1c_audit_t3_control_7ba2cf0.txt`), T3 run (`docs/gpio_f1c_audit_t3_7ba2cf0.txt`), T5 net check (`docs/gpio_f1c_audit_t5_netcheck_7ba2cf0.txt`, `docs/gpio_f1c_audit_t5_query_7ba2cf0.txt`), and audit 2 runs (`docs/gpio_f1c_audit2_t2_7b56d99.txt`, `../prj_04_adc_dma_bm_eth/docs/gpio_f1c_audit2_t7_7b56d99.txt`, `docs/gpio_f1c_audit2_wrongctrl_7b56d99.txt`).

---

## 8. Timer Driver Integration (`03_tim`, Phase F4)

Cortex-M4 consumes `01_shared_drivers/03_tim` through `stm32h7_driver_tim_hal` (tier HAL) and `stm32h7_driver_tim_bm` (tier BM) for ADC conversion triggering and hardware selftests:

- **TIM3 as ADC Trigger via TRGO (`CM4/App/Src/adc_acquisition.c`)**:
  - Replaces legacy direct writes and unprotected HAL calls with `TIM_HAL_InitTimeBase` (APB1 timer clock 200 MHz, PSC=0, ARR=399 -> 500 kHz nominal trigger rate, 50 samples per 10 kHz signal period) and `TIM_HAL_ConfigMaster` (MMS = `010` update event on TRGO, RM0399 §41.4.2).
  - Late counter start uses `TIM_BM_Start(TIM3)` once ADC and DMA circular transfers are fully armed, so that the first TRGO finds the ADC ready.
  - Zero raw writes to TIM registers or unprotected `HAL_TIM_*` calls remain in CM4 outside `01_shared_drivers/03_tim/tim_hal.c`.
- **TIM3 in Hardware Selftests (`CM4/App/Src/adc_selftest_hw.c`)**:
  - `Tim3_Prepare`, `Tim3_Run` and `Tim3_Halt` migrated to `tim_bm` functions (`TIM_BM_EnableClock`, `TIM_BM_InitTimeBase`, `TIM_BM_ConfigMaster`, `TIM_BM_Start`, `TIM_BM_Stop`), enabling clock before stop and verifying `TIM_STATUS_OK` return status.

### F4 Validation Evidence (Auditor Runs in `docs/`)

- **T3 SWD Hotplug Inspection (`docs/tim_f4_audit3_t3_1caa7c3.txt`)**: 32-bit register read matched RM0399 §41 bit-by-bit (`CR1=0x1`, `CR2=0x20`, `SMCR=0`, `DIER=0`, `PSC=0`, `ARR=0x18F`), identical to control `04fb865`.
- **T4 ADC Sampling Rate & Equivalence (`docs/tim_f4_audit4_t4_1893222.txt`)**: Migrated rate 506 125.3 Hz vs control 505 880.0 Hz (delta = 0.049% <= 0.1%), oracle `dSamples / dm4_hb` = 50 032 (migrated) and 50 006 (control) within [49750, 50250], `ovr` = 0, `err` = 0.
- **T5 Production Regression (`docs/tim_f4_audit3_t5_debug_1caa7c3.txt`, `_release_`)**: 3/3 checks PASS on debug and release; `rx_nobuf`, `tx_nobuf`, `dma_err` strictly 0.
- **T6a Warm Resets (`docs/tim_f4_audit4_t6a_1893222.txt`)**: 20/20 PASS with edge-aligned telemetry windows to `m4_hb` increment transitions; oracle within 49 825..50 076 across all cycles; zero overruns or errors.
- **T6b Cold Boot (`docs/tim_f4_audit_t6b_ddc937e.txt`)**: the bench owner power-cycled the board and the auditor, with no flash, reset or halt since the power-up, read `RCC_RSR = 0x00FE0000` (power-on, no software or watchdog flag) and ran T3 (exact), a 60 s T4 (506 226.6 Hz, 50 041 samples/hb, `ovr` and `err` 0) and T5 (3/3, `[net]` counters 0): 12/12 gates. The same run on a warm boot (`docs/tim_f4_audit_t6b_warm_control_ddc937e.txt`) fails the reset-flag gate with `SFT1RSTF` set, its negative control.
- **T8 Hardware Selftests (`docs/tim_f4_audit3_t8_1caa7c3.txt`)**: `HW RESULT PASS mask=31/31` with `HW T4 HAL:1` on migrated and control images, with verified restoration of `prj_03 debug`.
