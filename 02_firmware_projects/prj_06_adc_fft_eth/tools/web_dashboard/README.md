# Real-Time Web Dashboard — STM32H755ZI FFT + 16-Band db4 WPT Analyzer

Zero-dependency Node.js UDP-to-SSE bridge and responsive HTML5 Canvas dashboard for `prj_06_adc_fft_eth`.

## Quick Start

Double-click `start_dashboard.bat` (or `tools/start_dashboard.bat`), or run from PowerShell:

```powershell
cd tools/web_dashboard
node server.js --ip 192.168.1.150 --port 5002 --http 8080
```

Then open `http://localhost:8080` in your desktop or mobile browser.

## Features

- **4096-Point Blackman-Harris FFT Spectrum**: Interactive 2048-bin canvas (`0 .. fs/2`, `6.25 Hz/bin` at `25.6 kHz`), parabolic sub-bin fundamental peak marker ($f_0$), SNR, THD, and noise floor metrics.
- **Harmonic Profile (`H1..H5`)**: Live bar chart of harmonic amplitudes.
- **Reconstructed Oscilloscope (`2T` Window)**: Real-time waveform reconstruction using the measured DC offset and `H1..H10` Fourier components.
- **4-Level Daubechies-4 (`db4`) Wavelet Packet Transform (16 Sub-Bands)**:
  - Live energy percentage and RMS voltage across all 16 Gray-code frequency-ordered sub-bands (`B0..B15`).
  - **60 FPS 2D Waterfall Scalogram** with temporal LERP smoothing.
  - **Level-1 HF Transient Glitch Detector**: Monitors crest factor, peak amplitude (`mV`), time offset (`ms`), normalized Shannon entropy, and cumulative glitch count.
- **Dynamic Acquisition Toolbar**: Switch sampling rate on the fly (`10 kHz`, `25.6 kHz`, `50 kHz`, `100 kHz`), pause/resume capture (`STREAM` / `STOP`), or trigger an on-demand 2048-bin spectrum refresh (`SPECTRUM`).
