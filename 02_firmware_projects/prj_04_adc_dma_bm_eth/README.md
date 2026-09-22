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
| **CM4 FLASH (Debug)** | 18,344 Bytes (1.75%) | **5,792 Bytes (0.55%)** | **-68.4% (-12,552 B)** |
| **CM4 FLASH (Release)** | 16,252 Bytes (1.55%) | **5,144 Bytes (0.49%)** | **-68.3% (-11,108 B)** |
| **Code (.text Debug)** | 18,320 Bytes | **5,768 Bytes** | **Zero HAL bloat** |
| **Code (.text Release)** | 16,228 Bytes | **5,120 Bytes** | **Zero HAL bloat** |
| **Compiler libcalls** | several `__aeabi_*` | **none** | No 64-bit software helpers |
| **Latency / Overhead** | State-machine callbacks | Direct register writes & atomic bitmask tests | Sub-microsecond determinism |

Read off `arm-none-eabi-size` (FLASH = `text` + `data`), not estimated. Regenerate with
`arm-none-eabi-size build/<preset>/CM4/cm4_firmware.elf` after any change that touches
the CM4 image, and update the table rather than leaving it to drift.

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
- **FIFO & Memory-to-Memory**: Optional FIFO with a selectable threshold. RM0399 forbids both direct mode and circular mode in memory-to-memory transfers, so `DMA_BM_InitStream()` forces `DMDIS` for that direction and refuses a circular request outright, instead of leaving the caller to find out from the hardware.
- **Reported Failures**: `DMA_BM_Stop()` returns whether the stream actually went idle, and `InitStream` aborts on a false. Writes to `PAR`, `M0AR` and `NDTR` are ignored while `EN` is set, so a swallowed timeout would have produced a stream silently running its previous configuration.
- **Errata Hook**: `enable_trbuff` exposes `DMA_SxCR` bit 20, mandatory for USART/LPUART with DMA (ES0445 2.20.3).
- **Teardown**: `DMA_BM_DeinitStream()` stops the stream and resets its registers and DMAMUX channel.

### B. ADC Bare-Metal Driver (`07_adc/adc_bm.[ch]`)
- **Regulator & Deep Power-Down**: Clears `CR.DEEPPWD`, activates `CR.ADVREGEN`, and enforces `tADCVREG_STUP` stabilization.
- **Clock & Boost Mode**: Applies `BOOST[1:0]` while conversions are stopped. `ADC_BM_BOOST_AUTO` reads `DBGMCU->IDCODE` and picks the range for the silicon: Rev V indexes the table with `ker_ck / 2` (as ST's own `ADC_ConfigureBoostMode` does), Rev Y is a single bit at 20 MHz. A hardcoded constant is wrong on one of the two.
- **Errata ES0445 Compliance**: Enforces `ADC_CFGR_JQDIS` to disable the injected context queue.
- **Self-Calibration**: Automates offset and linearity calibration (`ADCALLIN`, `ADCAL`) while disabled (`ADEN = 0`).
- **Channel Preselection**: Direct setup of `PCSEL` (mandatory for STM32H7).
- **Fast Integer Statistics**: Zero floating-point in the hot path (`min`, `max`, `average`, `millivolts`), and 32-bit arithmetic throughout - `65535 * 3300` fits, so the CM4 image links no `__aeabi_*` helper at all.
- **Hardware Overrun Detection**: Enables `ADC_IER_OVRIE` and counts `OVR` separately from DMA transfer errors. A counter that can never increment does not read as "no overruns", it reads as a clean run.
- **Input Validation & Clean Start**: Rejects an out-of-range channel (the sampling-time write shifts by `(channel - 10) * 3`, which is undefined behaviour past 19), and clears `CFGR`, `CFGR2`, `PCSEL`, `SQR1..4`, `SMPR1..2`, `DIFSEL`, `IER` and `ISR` on init - enabling the peripheral clock does not reset the ADC, so a warm reset otherwise inherits the previous configuration.
- **Teardown**: `ADC_BM_Deinit()` stops conversions, disables the ADC and returns it to Deep-Power-Down.

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

### Two things that will bite you

**Queries must originate on the board's own subnet.** The UDP services answer nobody
else (section 8). A query that times out from a different subnet is the filter doing its
job, not a fault: the board logs the first refusal with the source address and counts
the rest as `acl_drop` in its `[net]` status line.

**The binary WAVE reply is versioned.** Its header carries `version` and `header_len`,
and both host tools locate the sample block at `header_len` rather than a fixed offset.
That is what lets a field be appended without an older tool quietly drawing header bytes
as signal - which is exactly what the predecessor of `query_adc.py` did. A tool meeting
a firmware whose `version` it does not know refuses the packet and says so. Keep
`WaveformHeader_t` in `CM7/App/Src/adc_server.c` and the `WAVE_*` constants in both
tools in step; the firmware asserts its own header size at compile time.

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
   - Linear calibration takes 16,384 ADC clock cycles (~0.65 ms at 25 MHz). The wait budgets are named constants in `adc_bm.c` (`ADC_BM_CALIBRATION_TIMEOUT_LOOPS`, `ADC_BM_VREG_STUP_LOOPS`, and the enable/disable/stop pair) rather than bare numbers, but they are still loop counts with no relation to the core clock - re-check them if this driver is ever used at a different frequency or optimisation level.
   - Bit modification on `ADC_CR` masks out all Read-to-Set bits (`ADC_CR_BITS_PROPERTY_RS`: `ADCAL`, `ADSTART`, `ADSTP`, `ADDIS`, `ADEN`) to prevent accidental re-triggers and ensure clean `ADEN=1` assertion.

### Measured Performance

Observed on the board, not estimated. The full record - including how each result was
obtained - is in [`docs/ADC_BRINGUP.md`](docs/ADC_BRINGUP.md) section 7.

- **Silicon**: revision `0x2003` (Rev V), from the boot banner. This is the revision
  `ADC_BM_BOOST_AUTO` resolves to the `ker_ck / 2` boost table; the Rev Y branch is
  still unexercised for want of the part.
- **Acquisition Rate**: `samples` climbed 5,208,064 -> 5,463,552 in 0.5 s, about
  511 kSPS, against a nominal 500 kSPS.
- **Overruns & Errors**: `err`, `ovr` and `adc_ovr` all zero across ~5 M samples. The
  third of those now means something: `ADC_IER_OVRIE` is enabled, so a hardware overrun
  would be counted. Before that it could not increment, and reading zero proved nothing.
- **Binary Protocol**: WAVE replies at 1080 bytes with `version=1` and `header_len=56`,
  and `adc_overrun_count` arriving through the dashboard's own `/api/samples`.
- **Source Filter**: verified in both directions - answered on-subnet with `acl_drop`
  flat at zero, refused off-subnet with `tx` frozen while `acl_drop` tracked `rx`.
- **Inter-Core Heartbeat**: advancing on both cores (`[ipc] m7=... m4=... (CPU2 alive)`).

A `ch` field reading nonsense is worth recognising on sight: it means CPU1 and CPU2
disagree about `SharedAdcData_t`, i.e. one core was reflashed and the other was not.
Flash both - `py tools/flash.py --core both`.

---

## 8. Security Considerations (UDP Amplification Risk)

Both diagnostic services - echo on port 7, telemetry and waveform on port 5001 - answer
without authentication, and a 4-byte `WAVE` command returns 1080 bytes: roughly 270x
amplification. Left open, the board is a usable reflector, and the echo service is one
by definition.

They therefore answer only sources on the interface's own subnet, taken from the live
netif so it follows DHCP and the static fallback. `NET_ACL_MODE` in
`CM7/App/Inc/net_config.h` selects the policy; `NET_ACL_ANY` restores the old behaviour
for a debugging session.

**This is not a security boundary.** A source address is trivially spoofed and anything
on the subnet is still answered. What it removes is the board's value as an off-link
amplifier, without a list of hosts to maintain. On an untrusted network keep upstream
firewall rules or VLAN isolation, and add a token check in `adc_server.c` if the
telemetry itself is worth protecting. ICMP is untouched, so `ping` stays the first
reachability check.

See [`docs/ADC_BRINGUP.md`](docs/ADC_BRINGUP.md) section 6 for the configuration detail
and section 7B for how the refuse path was actually proved, which needs more than a
timeout to be convincing.

