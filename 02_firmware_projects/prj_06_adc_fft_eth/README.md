# `prj_06_adc_fft_eth` — Dual-Core STM32H755ZI Real-Time 4096-Point FFT + 16-Band `db4` WPT + FreeRTOS + Ethernet

Real-time dual-core spectral and time-frequency analyzer for the **NUCLEO-H755ZI-Q** (`MB1363-E01`), built on the monorepo's centralized **`01_shared_drivers/`** library with **FreeRTOS + lwIP (`NO_SYS=0`)** on **Cortex-M7** and a deterministic **Bare-Metal (`*_bm`)** acquisition engine on **Cortex-M4**.

---

## 1. Architectural Overview

| Domain | Core | Operating Mode | Shared Drivers Used | Responsibilities |
| :--- | :--- | :--- | :--- | :--- |
| **D1** (`400 MHz`) | **Cortex-M7 (CPU1)** | **FreeRTOS** + **lwIP (`NO_SYS=0`)** + **CMSIS-DSP** | `gpio_bm`, `gpio_hal`, `led_bm`, `uart_hal`, `eth_hal`, `hsem_bm` | System clock tree (`400 MHz` SYSCLK, `SMPS`, `PLL2P = 25 MHz`), MPU + I/D-Cache, FreeRTOS scheduler (`TIM6` HAL timebase), 4096-pt Blackman-Harris RFFT, 4-Level Daubechies-4 (`db4`) Wavelet Packet Transform (16 sub-bands), and UDP `:5002` telemetry/spectrum server. |
| **D2** (`200 MHz`) | **Cortex-M4 (CPU2)** | **Bare-Metal** (`__WFI()` low-noise sleep between interrupts) | `adc_bm`, `dma_bm`, `tim_bm`, `gpio_bm`, `hsem_bm` | Hardware-timed `TIM3_TRGO` sampling trigger (default `25.6 kSPS`, runtime reconfigurable `1..100 kSPS`), 16-bit `ADC1` conversion on `PB1` (`ADC1_INP5` Fast Channel, Arduino `A3`), circular `DMA1_Stream0` ping-pong (`2 x 4096` samples in `SRAM2`), integer statistics, and lock-protected publication to `SRAM4`. |
| **D3** (`100 MHz`) | **Shared IPC** | Hardware Semaphores (`HSEM 0`, `HSEM 4`, `HSEM 5`) | `hsem_bm` | Non-cacheable `.shared_ram` (`0x38008000`, `32 KB` in `SRAM4`) holding the `SharedMemory_t` structure (`16,704 bytes`, including `raw_buffer[2][4096]`, runtime command handshake, CM4 HardFault trap, and FFT/WPT telemetry). |

---

## 2. DSP Pipeline Specifications

1. **4096-Point Windowed Real FFT (`CM7/App/Src/dsp_engine.c`)**:
   - **Sampling Rate ($f_s$)**: Default `25,600 Hz` (`25.6 kHz`), runtime reconfigurable (`1,000 .. 100,000 Hz`) via the `FS <hz>` UDP command.
   - **Block Size ($N$)**: `4096` samples (`2048` positive-frequency bins, $\Delta f = 6.25\text{ Hz/bin}$ at `25.6 kHz`, window duration $T_{\text{block}} = 160\text{ ms}$).
   - **Windowing**: 4-term **Blackman-Harris** ($a_0 = 0.35875$, $a_1 = 0.48829$, $a_2 = 0.14128$, $a_3 = 0.01168$; $-92\text{ dB}$ sidelobe floor).
   - **Extraction**: CMSIS-DSP `arm_rfft_fast_f32` + `arm_cmplx_mag_f32` in cached D1 AXI SRAM (`.axi_ram`), 3-point parabolic sub-bin interpolation for $f_0$, harmonics `H1..H10`, RMS noise floor, SNR (`dB`), and THD (`%`).

2. **4-Level Daubechies-4 (`db4`) Wavelet Packet Transform (`CM7/App/Src/dsp_wavelet.c`)**:
   - **Full Binary Tree ($J = 4$)**: Decomposes each 4096-sample zero-mean block using orthogonal 8-tap `db4` Quadrature Mirror Filters into $2^4 = 16$ uniform sub-bands (`B0..B15`, `256` coefficients/band, `800 Hz/band` at `25.6 kHz`).
   - **Paley / Gray-Code Frequency Ordering**: Maps binary tree leaf indices to monotonic frequency bands (`WPT_GRAY_MAP[16]`).
   - **Metrics**: Per-band energy share (`%`), RMS voltage (`mV`), spectral energy density ($\text{mV}^2/\text{Hz}$), and normalized Shannon wavelet entropy ($H_{\text{norm}} \in [0, 1]$).
   - **Microsecond HF Transient Detector**: Evaluates the Level-1 High-Pass detail branch (`2048` coefficients, $f_s/4 .. f_s/2$) crest factor (`peak / rms > 4.5` and `peak > 35 mV`) to capture sub-millisecond glitches invisible to the 160 ms FFT average.

---

## 3. Pinout & Hardware Connections (`NUCLEO-H755ZI-Q`)

| Signal | MCU Pin | Connector | Notes |
| :--- | :--- | :--- | :--- |
| **ADC1 Input (Default)** | `PB1` (`ADC1_INP5`, Fast Channel) | **CN9 pin 7 (`A3`)** | Configurable to `PA3` (`ADC1_INP15`, `A0`) by setting `BOARD_ADC_USE_FAST_CHANNEL_PB1 = 0` in `Common/Inc/board_def.h`. |
| **Analog Ground** | `GND` | **CN9 / CN8 `GND`** | Common ground reference ($0.0\text{ V} .. 3.3\text{ V}$ single-ended input range). |
| **VCP Console UART** | `PD8` (`TX`), `PD9` (`RX`) | ST-LINK USB VCP | `USART3` @ `115200 8N1`. |
| **Ethernet RMII** | `PA1, PA2, PA7, PB13, PC1, PC4, PC5, PG11, PG13` | RJ45 (`LAN8742A`, PHY addr `0`) | Static IP `192.168.1.150 / 24`, UDP Echo `:7`, UDP DSP Server `:5002`. |

---

## 4. Build, Flash & Verification

```powershell
# 1. Load the STM32 toolchain environment into the current PowerShell session
. .\tools\env.ps1

# 2. Configure and build Debug or Release preset (builds BOTH CM7 and CM4 ELFs)
cmake --preset debug
cmake --build --preset debug

cmake --preset release
cmake --build --preset release

# 3. Flash both cores (CM4 -> 0x08100000, CM7 -> 0x08000000) and verify option bytes
py tools/flash.py

# 4. Monitor ST-LINK VCP serial logs (115200 8N1)
py tools/monitor.py --timeout 15

# 5. Verify Ethernet ICMP + UDP echo path
py tools/net_check.py
```

---

## 5. UDP `:5002` Protocol & Host Visualization Tools

### Supported UDP Commands (`192.168.1.150:5002`)
- `STREAM` : Subscribe to real-time 144-byte binary telemetry packets (`UDP_DSP_TelemetryPacket_t`, magic `DSP2`) emitted on every completed 4096-sample block.
- `STOP` : Unsubscribe from streaming and pause the CM4 `TIM3` trigger.
- `SPECTRUM` : Request the full 2048-bin FFT magnitude spectrum across 4 segmented UDP packets (`1036 bytes` each, magic `SPEC`, `512` `uint16_t` bins in $0.1\text{ mV}$ units).
- `WAVE` : Request a 56-byte `UDP_DSP_WaveformHeader_t` (`WAVE` magic) + `512` raw 16-bit ADC samples (`1080 bytes`).
- `STATUS` / `STAT` : Human-readable multi-line ASCII summary of FFT, 16-band WPT, and CM4 status.
- `JSON` : Single-line JSON telemetry summary.
- `FS <hz>` : Dynamically reconfigure the CM4 `TIM3` sampling rate (`1000 .. 100000` Hz).
- `PING` : Replies `"PONG\r\n"`.

### Host Tools
- **Node.js Real-Time Web Dashboard (`tools/web_dashboard/`)**:
  ```powershell
  cd tools/web_dashboard
  node server.js --ip 192.168.1.150 --port 5002 --http 8080
  ```
  Open `http://localhost:8080` to view the live 2048-bin FFT spectrum, reconstructed 2-cycle oscilloscope, H1..H5 harmonics, 16-band `db4` WPT bars, 60 FPS 2D waterfall scalogram, and HF transient glitch monitor.
- **Python CLI & Matplotlib Visualizer (`tools/dsp_visualizer.py`)**:
  ```powershell
  py tools/dsp_visualizer.py --status
  py tools/dsp_visualizer.py --json
  py tools/dsp_visualizer.py --stream
  py tools/dsp_visualizer.py --plot
  ```
