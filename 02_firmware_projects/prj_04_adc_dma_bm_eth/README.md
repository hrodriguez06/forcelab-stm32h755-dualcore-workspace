# prj_04_adc_dma_bm_eth

Dual-Core STM32H755ZI firmware product with **pure Bare-Metal (`*_bm`)** high-speed 16-bit ADC1 + circular DMA1 on Cortex-M4 (Domain D2, SRAM2), TIM3 TRGO hardware triggering at 500 kSPS, inter-core telemetry sharing via SRAM4 under HSEM 4, and on-demand UDP telemetry server (:5001) over 10/100 Ethernet (RMII + LAN8742 PHY) on Cortex-M7 with FreeRTOS and lwIP.

Includes host utilities for CLI queries (`tools/query_adc.py`) and a real-time web dashboard (`tools/adc_dashboard.py`).

Built according to the workspace **Plan A 3-Tier Architecture** consuming centralized drivers from `01_shared_drivers/` (`02_dma/dma_bm`, `07_adc/adc_bm`, `00_gpio/gpio_bm`).

---

## 1. Core Architecture & System Roles

| Core | Image / Placement | Operating Model | Peripheral & Memory Ownership |
| :--- | :--- | :--- | :--- |
| **CPU1 (Cortex-M7)** | `cm7_firmware.elf`<br>Flash Bank 1 (`0x08000000`) | FreeRTOS (Priority grouping 4) | - Power (`PWR`), system clocks (`RCC`, PLL1 @ 400 MHz, PLL2P @ 25 MHz for ADC)<br>- Console VCP (`USART3` via `04_uart`)<br>- Ethernet MAC & DMA (`08_eth` via RMII) + LAN8742A PHY<br>- lwIP TCP/IP stack (IPv4, ICMP, UDP :5001)<br>- Descriptors in D2 SRAM3 (`0x30040000`, non-cacheable)<br>- Releases CPU2 via HSEM 0 |
| **CPU2 (Cortex-M4)** | `cm4_firmware.elf`<br>Flash Bank 2 (`0x08100000`) | **Bare-Metal (`*_bm`)** (No RTOS, No HAL ADC/DMA) | - **ADC1 (`adc_bm`)**: 16-bit, BOOST=10, 25 MHz kernel clock on `PB1`<br>- **TIM3**: Bare-metal TRGO on Update Event @ 500,000 Hz<br>- **DMA1 (`dma_bm`)**: Stream 0, DMAMUX Request 9, circular mode<br>- DMA circular buffer in D2 SRAM2 (`0x30020000`, 1024 samples)<br>- Computes statistics (min, max, avg, mV) in super-loop without floats<br>- Publishes telemetry + 512-sample waveform to SRAM4 (`0x38008000`) under HSEM 4 |

---

## 2. Memory Footprint Comparison (Cortex-M4)

By migrating from ST's HAL ADC & DMA to our centralized Bare-Metal drivers (`adc_bm` and `dma_bm`), Cortex-M4 achieves a dramatic reduction in flash footprint:

| Metric | `prj_03_adc_dma_eth` (HAL) | `prj_04_adc_dma_bm_eth` (Bare-Metal) | Improvement |
| :--- | :---: | :---: | :---: |
| **CM4 FLASH (Debug)** | 18,344 Bytes (1.75%) | **6,204 Bytes (0.59%)** | **-66.2% (-12.1 KB)** |
| **CM4 FLASH (Release)** | 16,820 Bytes (1.60%) | **5,620 Bytes (0.54%)** | **-66.6% (-11.2 KB)** |
| **Code (.text Debug)** | 18,320 Bytes | **6,180 Bytes** | **Zero HAL bloat** |
| **Code (.text Release)** | 16,796 Bytes | **5,596 Bytes** | **Zero HAL bloat** |
| **Latency / Overhead** | State-machine callbacks | Direct register writes & atomic bitmask tests | Sub-microsecond determinism |

---

## 3. Hardware Allocation & Pinout

| Signal / Role | Pin | Nucleo Connector | Description |
| :--- | :--- | :--- | :--- |
| **Analog Input** (default) | `PB1` | CN9 pin 4 (A3) | ADC12_INP5 single-ended (0 to 3.3V). Fast channel (DS12923 Table 87 note 4) |
| **Analog Input** (alternative) | `PA3` | CN9 pin 1 (A0) | ADC12_INP15 single-ended (slow channel; via `Common/Inc/board_def.h`) |
| **Ethernet RMII** | `PA1, PA2, PA7, PB13, PC1, PC4, PC5, PG11, PG13` | On-board | 50 MHz reference clock from LAN8742A PHY |
| **VCP UART Console** | `PD8 (TX), PD9 (RX)` | ST-LINK VCP | 115,200 baud, 8N1 |
| **Status LEDs** | `PB0 (Green), PE1 (Yellow), PB14 (Red)` | On-board LD1-3 | Diagnostics and network state |

---

## 4. Bare-Metal Driver Architecture (`01_shared_drivers/`)

### A. DMA Bare-Metal Driver (`02_dma/dma_bm.[ch]`)
- **Controller & Stream Resolution**: Computes controller (`DMA1`/`DMA2`) and DMAMUX1 channel (0..7 for DMA1, 8..15 for DMA2) automatically from stream pointer address.
- **Stream Configuration**: Direct manipulation of `PAR`, `M0AR`, `NDTR`, `FCR` (direct mode) and `CR`.
- **Interrupts & Overruns**: Atomic checking and clearing of status flags (`TC`, `HT`, `TE`, `DME`, `FE`) in `LISR`/`HISR` and `LIFCR`/`HIFCR`.
- **Bus Alignment**: Explicit 32-byte alignment attributes (`DMA_BM_ALIGN_32`) for deterministic bus bursts.

### B. ADC Bare-Metal Driver (`07_adc/adc_bm.[ch]`)
- **Regulator & Deep Power-Down**: Clears `CR.DEEPPWD`, activates `CR.ADVREGEN`, and enforces `tADCVREG_STUP` stabilization.
- **Clock & Boost Mode**: Applies `BOOST[1:0]` for 25 MHz kernel clock while conversions are stopped.
- **Errata ES0445 Compliance**: Enforces `ADC_CFGR_JQDIS` to disable the injected context queue.
- **Self-Calibration**: Automates offset and linearity calibration (`ADCALLIN`, `ADCAL`) while disabled (`ADEN = 0`).
- **Channel Preselection**: Direct setup of `PCSEL` (mandatory for STM32H7).
- **Fast Integer Statistics**: Zero floating-point in hot path (`min`, `max`, `average`, `millivolts`).

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

---

## 7. Hardware Validation & Silicon Bring-Up Lessons (NUCLEO-H755ZI-Q)

During the hardware bring-up of `prj_04_adc_dma_bm_eth` on STM32H755ZI Rev V, the following hardware constraints and lessons learned were identified, addressed, and verified:

1. **SMPS Supply Architecture (Run\* Mode Lockup)**:
   - The NUCLEO-H755ZI-Q powers the core logic directly from the internal SMPS step-down regulator.
   - `HAL_PWREx_ConfigSupply(PWR_DIRECT_SMPS_SUPPLY)` must be invoked in `SystemClock_Config()` before voltage scaling. Without it, the MCU stalls permanently waiting for `PWR_FLAG_VOSRDY`.
2. **Dual-Core Boot Gating & Warm Reset Handshake**:
   - Device 0x450 Rev V leaves CPU2 (Cortex-M4) boot gated by default in Option Bytes (`BCM4=0`).
   - CPU1 explicitly releases CPU2 via `HAL_RCCEx_EnableBootCore(RCC_BOOT_C2)`.
   - On warm resets, SRAM4 (`.shared_ram`) retains values across resets. CPU1 immediately clears `g_shared.magic = 0;` at entry of `main()` and pre-enables CM4 peripheral clocks (DMA1, ADC12, GPIOB, TIM3) before releasing HSEM 0. CPU2 always enters STOP mode waiting for HSEM 0 and `SHARED_MAGIC`, preventing boot races.
3. **STM32H7 ADC1 External Trigger Mapping (`EXTSEL`)**:
   - In STM32H7, `TIM3_TRGO` is mapped to `EXTSEL = 4` (`ADC_CFGR_EXTSEL_2`), defined in `adc_bm.h` as `ADC_BM_TRIG_TIM3_TRGO (0x00000004UL)`. Older families and channels used index 9; using 4 activates continuous conversions at 500 kSPS.
4. **ADC Calibration Duration & Read/Set (`RS`) Bit Safety**:
   - Linear calibration takes 16,384 ADC clock cycles (~0.65 ms at 25 MHz). The bare-metal calibration wait loop was dimensioned accordingly (`10,000,000` cycles).
   - Bit modification on `ADC_CR` masks out all Read-to-Set bits (`ADC_CR_BITS_PROPERTY_RS`: `ADCAL`, `ADSTART`, `ADSTP`, `ADDIS`, `ADEN`) to prevent accidental re-triggers and ensure clean `ADEN=1` assertion.

### Measured Performance
- **Acquisition Rate**: 500,000 SPS (500 kSPS) continuous.
- **Buffers Processed**: >108,000 transfers (>110 million samples).
- **Overruns & Errors**: Zero (`dma_err=0, ovr=0, adc_ovr=0`).
- **Inter-Core Heartbeat**: Actively advancing across both cores (`[ipc] m7=... m4=... (CPU2 alive)`).

---

## 8. Security Considerations (UDP Amplification Risk)

The diagnostic UDP services (Echo on port 7, ADC Telemetry and Waveform on port 5001) respond to incoming datagrams without source IP filtering or authentication:
- **Amplification Risk**: Sending a 4-byte ASCII command (`WAVE`) triggers an immediate 1072-byte binary response containing the complete 512-sample waveform buffer (~268x bandwidth amplification).
- **Deployment Recommendation**: On untrusted or public networks, restrict access using upstream firewall rules, VLAN isolation, or implement source IP allowlisting / HMAC token verification in `adc_server.c`.

