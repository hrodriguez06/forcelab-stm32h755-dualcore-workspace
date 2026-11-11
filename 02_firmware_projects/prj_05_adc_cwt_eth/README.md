# `prj_05_adc_cwt_eth` — Dual-Core Continuous Wavelet Transform (CWT) & 1.5 V Biased AC Analyzer (`NUCLEO-H755ZI-Q`)

Real-time dual-core time-frequency signal analysis firmware for the **STM32H755ZI-Q**. **CPU2 (Cortex-M4 @ 200 MHz)** runs a deterministic, 100% Bare-Metal (`*_bm`) 500 kSPS acquisition engine on **Arduino pin A3 (`PB1`, `ADC1_INP5`)** for bipolar AC signals biased at a **1.5 V DC offset**, while **CPU1 (Cortex-M7 @ 400 MHz)** removes the 1.5 V DC offset, computes a **24-scale Complex Morlet Continuous Wavelet Transform (CWT)** in a dedicated FreeRTOS DSP task, and streams the 2D scalogram and AC/DC waveform over **UDP Ethernet (`:5001`)**.

---

## 1. System Architecture & Core Partitioning

| Domain / Core | Role | Drivers (`01_shared_drivers`) | Key Memory Regions |
| :--- | :--- | :--- | :--- |
| **CPU2 — Cortex-M4 (200 MHz)** | Pure Bare-Metal 500 kSPS ADC1 + DMA1 + TIM3 TRGO acquisition & integer statistics | `gpio_bm`, `tim_bm`, `dma_bm`, `adc_bm`, `hsem_bm` | Code: `FLASH_CM4` (`0x08100000`)<br>DMA Ping-Pong (`2 × 512` samples): D2 `SRAM2` (`0x30020000`) |
| **IPC — Shared Domain (D3)** | Lock-protected 512-sample window & 1.5 V DC bias telemetry transfer | `hsem_bm` (`HSEM 0` Boot, `HSEM 4` ADC Data, `HSEM 5` GPIO Lock) | D3 `SRAM4` (`.shared_ram` @ `0x38008000`, MPU Non-Cacheable) |
| **CPU1 — Cortex-M7 (400 MHz)** | FreeRTOS + 1.5 V DC Offset Removal + 24-Scale Complex Morlet CWT + lwIP UDP Server | `gpio_hal`, `led_hal`, `uart_hal`, `eth_hal`, `hsem_bm` | Code: `FLASH_CM7` (`0x08000000`), FFT/IFFT inner loop in `ITCM` (`0x00000000`)<br>CWT Filter Bank & Scalograms: D1 `AXI SRAM` (`0x24000000`)<br>ETH Descriptors & Pool: D2 `SRAM3` (`0x30040000`) |

---

## 2. Analog Front-End 1.5 V DC Bias & Offset Removal Model

Because the STM32H755 internal 16-bit SAR ADC (`ADC1_INP5` on `PB1` / Nucleo CN9 pin 4 **A3**) operates over a unipolar $0.0\text{ V}$ to $V_{\text{REF+}} = 3.3\text{ V}$ span, bipolar AC signals ($v_{\text{AC}}(t) \in [-1.5\text{ V}, +1.5\text{ V}]$) are externally summed with a nominal **1.5 V (`1500 mV`) DC offset**:

$$v_{\text{PB1}}(t) = V_{\text{bias}} + v_{\text{AC}}(t), \quad V_{\text{bias,nom}} = 1500\text{ mV} \implies \text{RAW}_{\text{nom}} = \left\lfloor \frac{1500 \times 65535}{3300} \right\rfloor = 29789$$

On **Cortex-M7** (`CM7/App/Src/cwt_processor.c`), before computing the Continuous Wavelet Transform, each 512-sample window is converted to physical AC millivolts with selectable offset suppression:
1. **`CWT_OFFSET_MODE_AUTO_MEAN` (default, `MODE AUTO`)**:
   - Subtracts the exact window mean $\bar{r} = \text{average\_raw}$ (which captures the nominal $1.5\text{ V}$ bias `29789` **plus** any analog front-end thermal/supply drift $\Delta V_{\text{DC}} = \text{measured\_dc\_mv} - 1500\text{ mV}$):
     $$x_{\text{mV}}[n] = \left(r[n] - \bar{r}\right) \cdot \frac{3300.0}{65535.0}$$
   - Eliminates zero-frequency DC energy before FFT convolution, preventing cone-of-influence boundary artifacts at low-frequency wavelet scales.
2. **`CWT_OFFSET_MODE_FIXED_1V5` (`MODE FIXED`)**:
   - Subtracts the fixed nominal 1.5 V digital code (`ADC_NOMINAL_OFFSET_RAW = 29789`):
     $$x_{\text{mV}}[n] = \left(r[n] - 29789\right) \cdot \frac{3300.0}{65535.0}$$

---

## 3. Continuous Wavelet Transform (Complex Morlet)

- **Sampling Rate ($f_s$)**: $500\,000\text{ Hz}$ ($\Delta t = 2.0\text{ }\mu\text{s}$), Window $N = 512$ ($T_w = 1.024\text{ ms}$).
- **Mother Wavelet**: Analytic Complex Morlet ($\omega_0 = 6.0$).
- **Scale Bank**: `CWT_NUM_SCALES = 24` logarithmically spaced center frequencies across 3 decades from $f_{\min} = 100.0\text{ Hz}$ to $f_{\max} = 100.0\text{ kHz}$:
  $$f_m = 100.0 \times \left(\frac{100\,000.0}{100.0}\right)^{m / 23}, \quad m = 0, \dots, 23$$
- **Dual-Rate Fast Convolution Pipeline**:
  - **High-Frequency Band ($3.0\text{ kHz} \le f_m \le 100.0\text{ kHz}$, Scales $12 \dots 23$)**: Evaluated directly on the full-rate $500\text{ kSPS}$ 512-sample window ($\Delta f_{\text{high}} = 976.56\text{ Hz}$).
  - **Low-Frequency Band ($100.0\text{ Hz} \le f_m < 3.0\text{ kHz}$, Scales $0 \dots 11$)**: Evaluated on a $32\times$-decimated sliding horizon buffer ($f_{s,\text{low}} = 15.625\text{ kHz}$, $\Delta f_{\text{low}} = 30.52\text{ Hz}$, $32.768\text{ ms}$ span with symmetric right-edge mirror padding) so low-frequency Morlet wavelets down to $100\text{ Hz}$ are resolved without FFT grid underflow.
  - **Long-Horizon IIR DC Bias Tracker (`AUTO_MEAN`)**: Tracks the $1.5\text{ V}$ DC bias and thermal drift with a $\sim 50\text{ ms}$ exponential moving average ($f_{-3\text{dB}} \approx 3\text{ Hz}$) so $100\text{ Hz}$ AC waveforms are not high-pass attenuated.

---

## 4. UDP Port `:5001` Protocol (`CM7/App/Src/cwt_server.c`)

| Command | Response | Description |
| :--- | :--- | :--- |
| `CWT` or `SCAL` | 25 binary UDP packets ($\le 1184\text{ B}$ each) | Full 2D CWT burst (`chunk 0`: `f_center_hz[24]` + `raw_waveform[512]`; `chunks 1..24`: `uint16_t` scalogram rows) |
| `CWT SNAP` | 1 binary UDP packet (`1184 B`) | Latches latest CWT frame into TX snapshot buffer and returns `chunk 0` |
| `CWT B <start> <count>` | `1..4` binary UDP packets (`1088 B` each) | Returns `<count>` consecutive chunks from the latched snapshot (fits `ETH_TX_DESC_CNT = 4` with zero drops) |
| `WAVE` | 1 binary UDP packet (`1088 B`) | `WaveformHeader_t` (`64 B`) + `512 × uint16_t` raw ADC samples |
| `JSON` | JSON string | Full telemetry (`mv`, `nominal_offset_mv`, `dc_offset_error_mv`, `vpp_mv`, `ac_min_mv`, `ac_max_mv`, `cwt_exec_us`, `cwt_peak_freq_hz`, `cwt_peak_mv`, etc.) |
| `STAT` / `GET` | Plain-text line | Human-readable telemetry summary |
| `MODE AUTO` / `MODE FIXED` | Plain-text ACK | Switches CM7 1.5 V offset removal mode at runtime |
| `PING` | `PONG` | Liveness check |

---

## 5. Build, Flash & Host Tools

```powershell
# 1. Load toolchain environment
. .\tools\env.ps1

# 2. Configure & build Debug and Release presets
cmake --preset debug
cmake --build --preset debug
cmake --preset release
cmake --build --preset release

# 3. Flash both cores (CM7 + CM4)
py tools/flash.py

# 4. Query CWT scalogram and 1.5 V AC/DC telemetry in terminal
py tools/query_cwt.py --host 192.168.1.150 --count 5

# 5. Launch real-time web dashboard (Oscilloscope + 2D Morlet CWT Heatmap)
py tools/cwt_dashboard.py --host 192.168.1.150 --bind 127.0.0.1:8080
```
