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
