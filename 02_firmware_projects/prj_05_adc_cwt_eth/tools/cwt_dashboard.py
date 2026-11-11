#!/usr/bin/env python
"""
Interactive real-time web dashboard for STM32H755ZI Dual-Core Continuous
Wavelet Transform (CWT) & 1.5 V DC-Biased AC Signal Analyzer (prj_05_adc_cwt_eth).

Features:
  - 100% Python Standard Library (no pip dependencies, works offline).
  - Time-Domain Oscilloscope:
      * Toggleable between AC-Coupled (+/- 1500 mV around 1.5 V bias) and
        Absolute (0 - 3300 mV with 1500 mV DC bias reference line).
      * Sub-sample software triggering and adjustable timebase.
  - Real-Time 2D Complex Morlet CWT Scalogram Heatmap:
      * 24 log-spaced scales (2.0 kHz to 200.0 kHz) x 512 time bins (1.024 ms).
      * Selectable scientific colormaps (Inferno, Turbo, Viridis) and dB / Linear scaling.
      * Instantaneous wavelet energy ridge and global peak marker.
  - Analog Front-End 1.5 V Bias & Dual-Core Telemetry:
      * Measured DC bias (mV), DC offset drift error (mV), AC Vpp (mV),
        CM7 CWT DSP execution time (us), ADC overrun count, and live toggle
        between AUTO_MEAN and FIXED_1V5 offset removal modes.

This script lives inside the project, NOT in <workspace>/tools/, and it has no
shim because its struct.unpack() format must stay byte-identical with
CwtPacketHeader_t in CM7/App/Src/cwt_server.c.

Usage:
    py tools/cwt_dashboard.py [--host 192.168.1.150] [--port 5001] [--bind 127.0.0.1:8080] [--rate 12]
"""

from __future__ import annotations

import argparse
from collections import deque
import http.server
import json
import math
import os
import socket
import struct
import sys
import threading
import time
import urllib.parse

try:
    import numpy as np
except ImportError:
    np = None

DEFAULT_PHM_CSV = r"c:\Users\hrodriguez\Downloads\c_1_001.csv\c_1_001.csv"

CWT_MAGIC = 0x31545743  # 'CWT1'
CWT_VERSION = 1
CWT_HEADER_FMT = "<IHHIHHIHHHHHHHHHHIIIfhHI"
CWT_HEADER_LEN = 64
assert struct.calcsize(CWT_HEADER_FMT) == CWT_HEADER_LEN

HTML_PAGE = """<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>STM32H755ZI - Dual-Core Morlet CWT Scalogram & 1.5V AC/DC Oscilloscope</title>
<style>
  :root {
    --bg-color: #0b0f17;
    --card-bg: #151b26;
    --border-color: #2b3445;
    --text-primary: #e6edf3;
    --text-muted: #8b949e;
    --accent-cyan: #00ffc8;
    --accent-blue: #58a6ff;
    --accent-green: #3fb950;
    --accent-yellow: #d29922;
    --accent-red: #f85149;
    --accent-purple: #bc8cff;
  }
  * { box-sizing: border-box; margin: 0; padding: 0; }
  body {
    background-color: var(--bg-color);
    color: var(--text-primary);
    font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, sans-serif;
    padding: 18px;
  }
  .header {
    display: flex;
    justify-content: space-between;
    align-items: center;
    padding-bottom: 14px;
    border-bottom: 1px solid var(--border-color);
    margin-bottom: 16px;
    flex-wrap: wrap;
    gap: 12px;
  }
  .header h1 { font-size: 1.35rem; font-weight: 600; }
  .header-sub { font-size: 0.82rem; color: var(--text-muted); margin-top: 3px; }
  .status-badge {
    display: flex;
    align-items: center;
    gap: 8px;
    padding: 6px 14px;
    border-radius: 20px;
    font-size: 0.82rem;
    font-weight: 600;
  }
  .status-live { background: rgba(63,185,80,0.15); color: var(--accent-green); border: 1px solid var(--accent-green); }
  .status-stale { background: rgba(248,81,73,0.15); color: var(--accent-red); border: 1px solid var(--accent-red); }
  .dot { width: 9px; height: 9px; border-radius: 50%; background: currentColor; box-shadow: 0 0 8px currentColor; }

  .kpi-grid {
    display: grid;
    grid-template-columns: repeat(auto-fit, minmax(175px, 1fr));
    gap: 12px;
    margin-bottom: 16px;
  }
  .kpi-card {
    background: var(--card-bg);
    border: 1px solid var(--border-color);
    border-radius: 8px;
    padding: 12px 14px;
  }
  .kpi-label { font-size: 0.72rem; text-transform: uppercase; letter-spacing: 0.6px; color: var(--text-muted); }
  .kpi-val {
    font-size: 1.35rem;
    font-weight: 700;
    margin-top: 4px;
    font-family: "SFMono-Regular", Consolas, monospace;
  }
  .kpi-sub { font-size: 0.75rem; color: var(--text-muted); margin-top: 3px; }

  .vis-grid {
    display: grid;
    grid-template-columns: 1fr;
    gap: 16px;
  }
  .panel {
    background: var(--card-bg);
    border: 1px solid var(--border-color);
    border-radius: 8px;
    padding: 14px;
  }
  .panel-header {
    display: flex;
    justify-content: space-between;
    align-items: center;
    margin-bottom: 10px;
    flex-wrap: wrap;
    gap: 10px;
  }
  .panel-title {
    font-size: 0.92rem;
    font-weight: 600;
    color: var(--accent-cyan);
    letter-spacing: 0.4px;
  }
  .controls {
    display: flex;
    align-items: center;
    gap: 8px;
    flex-wrap: wrap;
    font-size: 0.78rem;
  }
  .btn {
    background: #1f2735;
    color: var(--text-primary);
    border: 1px solid var(--border-color);
    border-radius: 4px;
    padding: 4px 10px;
    cursor: pointer;
    font-size: 0.76rem;
    font-weight: 600;
  }
  .btn.active {
    background: #1f6feb;
    border-color: var(--accent-blue);
    color: #fff;
  }
  canvas {
    width: 100%;
    display: block;
    background: #060a10;
    border: 1px solid #1e2636;
    border-radius: 6px;
  }
  #scopeCanvas { height: 250px; }
  #cwtCanvas { height: 320px; }
</style>
</head>
<body>

<div class="header">
  <div>
    <h1>STM32H755ZI Dual-Core CWT Analyzer &mdash; 4-Flute Cutting Tool Monitor (&le;10,000 RPM)</h1>
    <div class="header-sub">
      CPU2 (Cortex-M4 Bare-Metal): PB1 / A3 (ADC1_INP5) @ 10 kSPS (51.2 ms/win) &rarr; HSEM4 / SRAM4 &rarr;
      CPU1 (Cortex-M7 400 MHz): 1.5V Bias Removal + 24-Scale Complex Morlet CWT (10 Hz &ndash; 4 kHz)
    </div>
  </div>
  <div id="statusBadge" class="status-badge status-stale">
    <span class="dot"></span>
    <span id="statusText">CONNECTING...</span>
  </div>
</div>

<!-- PHM 2010 Dataset, AD2 W1 Injector & Tool Geometry Bar -->
<div class="panel" style="margin-bottom:12px; padding:10px 14px; background:#121926; border:1px solid #2b3a55;">
  <div style="display:flex; align-items:center; justify-content:space-between; flex-wrap:wrap; gap:10px;">
    <div style="display:flex; align-items:center; gap:8px; flex-wrap:wrap;">
      <span style="color:var(--accent-cyan); font-weight:700; font-size:0.82rem;">DATA SOURCE:</span>
      <button class="btn active" id="btnSrcStm32" onclick="setDataSource('stm32')">LIVE STM32 (PB1/A3 @ 10 kSPS)</button>
      <button class="btn" id="btnSrcPhm" onclick="setDataSource('phm')">PHM 2010 Dataset: c_1_001.csv</button>
      <button class="btn" id="btnInjectAd2" onclick="injectPhmToAd2()" style="border-color:#d29922; color:#ffd33d; background:rgba(210,153,34,0.15); font-weight:700;" title="Program Analog Discovery 2 W1 with selected PHM channel + 1.5V DC offset and switch to LIVE STM32">&#9889; Inyectar en AD2 W1 (1.5V)</button>
      <span id="phmInfoBadge" style="font-family:Consolas,monospace; font-size:0.75rem; color:var(--accent-yellow); padding-left:4px;">
        AD2 W1 &rarr; PB1/A3 Activo (1.5V Offset)
      </span>
    </div>
    <div id="phmControlsRow" style="display:flex; align-items:center; gap:6px; flex-wrap:wrap;">
      <span style="font-size:0.75rem; color:var(--text-muted);">Canal PHM / AD2:</span>
      <button class="btn" id="btnPhmCh0" onclick="setPhmChannel(0)" title="Cutting Force X (N)">Fx (N)</button>
      <button class="btn active" id="btnPhmCh1" onclick="setPhmChannel(1)" title="Cutting Force Y (N)">Fy (N)</button>
      <button class="btn" id="btnPhmCh2" onclick="setPhmChannel(2)" title="Cutting Force Z (N)">Fz (N)</button>
      <button class="btn" id="btnPhmCh3" onclick="setPhmChannel(3)" title="Vibration X (g)">Vx (g)</button>
      <button class="btn" id="btnPhmCh4" onclick="setPhmChannel(4)" title="Vibration Y (g)">Vy (g)</button>
      <button class="btn" id="btnPhmCh5" onclick="setPhmChannel(5)" title="Vibration Z (g)">Vz (g)</button>
      <button class="btn" id="btnPhmCh6" onclick="setPhmChannel(6)" title="Acoustic Emission RMS (V)">AE (V)</button>
      <span style="margin-left:4px; font-size:0.75rem; color:var(--text-muted);">Corte:</span>
      <button class="btn" id="btnPhmPlay" onclick="togglePhmPlay()">&#9654; PLAY</button>
      <button class="btn" onclick="seekPhmSec(0.18)" title="Tool entry transient">Entrada (0.18s)</button>
      <button class="btn" onclick="seekPhmSec(1.00)" title="Steady-state full cutting engagement">Estable (1.00s)</button>
      <button class="btn" onclick="seekPhmSec(2.38)" title="Tool exit transient">Salida (2.38s)</button>
      <input type="range" id="phmTimeSlider" min="0" max="250" step="1" value="100" oninput="onPhmSliderInput(this.value)" style="width:110px; accent-color:var(--accent-cyan);">
    </div>
  </div>
  <!-- Row 2: Tool Geometry (Flutes z & Spindle RPM) + Guide Overlays -->
  <div style="display:flex; align-items:center; justify-content:space-between; flex-wrap:wrap; gap:10px; margin-top:8px; padding-top:8px; border-top:1px solid #1f293d;">
    <div style="display:flex; align-items:center; gap:10px; flex-wrap:wrap;">
      <span style="color:var(--accent-purple); font-weight:700; font-size:0.80rem;">HERRAMIENTA DE CORTE:</span>
      <span style="font-size:0.76rem; color:var(--text-muted);">Gavilanes (z):</span>
      <button class="btn" id="btnFlutes3" onclick="setToolFlutes(3)" title="3-Flute Ball-Nose (PHM 2010)">3 Filos</button>
      <button class="btn active" id="btnFlutes4" onclick="setToolFlutes(4)" title="4-Flute End Mill">4 Gavilanes</button>
      <span style="font-size:0.76rem; color:var(--text-muted); margin-left:4px;">Velocidad Husillo (RPM):</span>
      <input type="number" id="inpToolRpm" value="10400" min="600" max="24000" step="100" onchange="onToolRpmChange(this.value)" style="width:76px; background:#0d131d; color:var(--accent-cyan); font-weight:700; border:1px solid var(--border-color); border-radius:4px; padding:2px 6px; font-family:Consolas,monospace; font-size:0.78rem;">
      <button class="btn" onclick="onToolRpmChange(10400)">10,400 RPM (PHM)</button>
      <button class="btn" onclick="onToolRpmChange(10000)">10,000 RPM</button>
      <button class="btn" onclick="onToolRpmChange(6000)">6,000 RPM</button>
      <label style="display:flex; align-items:center; gap:5px; cursor:pointer; margin-left:8px; font-size:0.78rem; color:#e6edf3;">
        <input type="checkbox" id="chkShowGuides" checked onchange="toggleMachiningGuides(this.checked)">
        <span>Mostrar Gu&iacute;as de Revoluci&oacute;n (Rev #) y Gavilanes (G1..G4) + L&iacute;neas 1X/zX Hz</span>
      </label>
    </div>
    <div id="toolFreqSummary" style="font-family:Consolas,monospace; font-size:0.76rem; color:#8b949e;">
      1X Giro: <strong style="color:#d29922;">173.3 Hz</strong> (5.77 ms/rev) | 4X Paso Gavil&aacute;n: <strong style="color:#00ffc8;">693.3 Hz</strong> (1.44 ms/filo)
    </div>
  </div>
</div>

<!-- Automated Machining Diagnostic & Per-Flute Load Strip -->
<div class="panel" style="margin-bottom:14px; padding:10px 14px; background:#0f1622; border:1px solid #233044;">
  <div style="display:flex; align-items:center; justify-content:space-between; flex-wrap:wrap; gap:14px;">
    <div style="display:flex; align-items:center; gap:12px;">
      <span style="font-size:0.78rem; font-weight:700; color:var(--text-muted);">DIAGN&Oacute;STICO AUTOM&Aacute;TICO:</span>
      <span id="diagStatusPill" style="padding:4px 12px; border-radius:6px; font-size:0.80rem; font-weight:700; border:1px solid #2ea043; background:rgba(57,211,83,0.15); color:#39d353;">
        CORTE ESTABLE &check;
      </span>
      <span id="diagDetailText" style="font-size:0.78rem; color:#c9d1d9;">
        Analizando distribuci&oacute;n de carga por gavil&aacute;n y relaci&oacute;n 1X / Paso de Filo...
      </span>
    </div>
    <!-- Per-Flute Load Mini-Bars (G1 .. G4) -->
    <div id="fluteLoadContainer" style="display:flex; align-items:center; gap:12px; font-family:Consolas,monospace; font-size:0.76rem;">
      <span style="color:var(--text-muted);">Carga por Gavil&aacute;n:</span>
      <div id="fluteBarsHtml" style="display:flex; align-items:center; gap:10px;"></div>
      <span id="diagMetricsBadge" style="color:#8b949e; padding-left:6px; border-left:1px solid #2b3445;">
        Runout (1X/zX): -- | Chatter (&gt;1kHz): --%
      </span>
    </div>
  </div>
</div>

<!-- Floating Synchronized Crosshair Tooltip -->
<div id="cursorTooltip" style="display:none; position:fixed; pointer-events:none; z-index:9999; background:rgba(13,19,29,0.94); border:1px solid #58a6ff; border-radius:6px; padding:7px 10px; font-family:Consolas,monospace; font-size:0.75rem; color:#e6edf3; box-shadow:0 4px 14px rgba(0,0,0,0.6); line-height:1.45;"></div>

<div class="kpi-grid">
  <div class="kpi-card">
    <div class="kpi-label">1.5V Analog DC Bias</div>
    <div class="kpi-val" id="kpiDcMv">-- mV</div>
    <div class="kpi-sub" id="kpiDcErr">Nominal: 1500 mV (Err: -- mV)</div>
  </div>
  <div class="kpi-card">
    <div class="kpi-label">AC Peak-to-Peak (Vpp)</div>
    <div class="kpi-val" id="kpiVpp" style="color: var(--accent-cyan);">-- mV</div>
    <div class="kpi-sub" id="kpiAcSwing">AC Swing: [-- , --] mV</div>
  </div>
  <div class="kpi-card">
    <div class="kpi-label">Dominant Wavelet Ridge</div>
    <div class="kpi-val" id="kpiPeakFreq" style="color: var(--accent-purple);">-- kHz</div>
    <div class="kpi-sub" id="kpiPeakMag">Scale #-- | Peak |W|: -- mV</div>
  </div>
  <div class="kpi-card">
    <div class="kpi-label">CM7 CWT DSP Time</div>
    <div class="kpi-val" id="kpiCwtUs" style="color: var(--accent-green);">-- &mu;s</div>
    <div class="kpi-sub" id="kpiFrameSeq">24x512 FFT-Morlet | Seq #--</div>
  </div>
  <div class="kpi-card">
    <div class="kpi-label">Offset Removal Mode</div>
    <div class="kpi-val" id="kpiOffsetMode" style="font-size: 1.05rem;">AUTO_MEAN</div>
    <div class="kpi-sub" style="margin-top: 6px;">
      <button class="btn active" id="btnModeAuto" onclick="setOffsetMode('auto')">AUTO_MEAN</button>
      <button class="btn" id="btnModeFixed" onclick="setOffsetMode('fixed')">FIXED_1V5</button>
    </div>
  </div>
  <div class="kpi-card">
    <div class="kpi-label">DMA &amp; ADC Integrity</div>
    <div class="kpi-val" id="kpiHealth" style="color: var(--accent-green);">OK</div>
    <div class="kpi-sub" id="kpiOvr">Overruns: 0 | DMA Err: 0</div>
  </div>
</div>

<div class="vis-grid">
  <!-- Panel 1: Time-Domain Oscilloscope with Sub-Sample Software Trigger -->
  <div class="panel">
    <div class="panel-header">
      <div style="display:flex; align-items:center; gap:10px;">
        <div class="panel-title">TIME-DOMAIN WAVEFORM (PB1 / A3 @ 10 kSPS, N = 512, 51.2 ms Real Window)</div>
        <span id="trigStatusBadge" style="padding:2px 8px; border-radius:4px; font-size:0.74rem; font-weight:700; border:1px solid #2ea043; background:rgba(57,211,83,0.15); color:#39d353;">TRIG'D &check;</span>
      </div>
      <div class="controls">
        <span>Coupling:</span>
        <button class="btn active" id="btnViewAc" onclick="setScopeMode('ac')">AC (&plusmn;1500 mV)</button>
        <button class="btn" id="btnViewAbs" onclick="setScopeMode('abs')">Abs (0&ndash;3300 mV)</button>
        <span>Timebase:</span>
        <button class="btn" id="btnZoom6000" onclick="setZoomUs(6000)" title="1 Rev @ 10,000 RPM (4 flute passes)">6 ms (1 Rev)</button>
        <button class="btn" id="btnZoom12000" onclick="setZoomUs(12000)" title="2 Revs @ 10,000 RPM (8 flute passes)">12 ms (2 Rev)</button>
        <button class="btn active" id="btnZoom25600" onclick="setZoomUs(25600)" title="4.2 Revs @ 10,000 RPM">25.6 ms</button>
        <button class="btn" id="btnZoom51200" onclick="setZoomUs(51200)" title="Full 512-sample real window (8.5 Revs @ 10,000 RPM)">51.2 ms</button>
        <button class="btn" id="btnZoom250000" onclick="setZoomUs(250000)" title="Macro 250 ms window (~43 Revs @ 10,400 RPM)">250 ms</button>
        <button class="btn" id="btnZoom500000" onclick="setZoomUs(500000)" title="Macro 500 ms window (~87 Revs @ 10,400 RPM)">500 ms</button>
        <button class="btn" id="btnZoom1000000" onclick="setZoomUs(1000000)" title="1.0 Second Macro Window (~173 Revs @ 10,400 RPM)">1.0 s (1000 ms)</button>
      </div>
    </div>
    <!-- Oscilloscope Trigger & Seismic Roll Control Bar -->
    <div class="controls" style="margin-bottom:10px; padding:8px 10px; background:#0d131d; border:1px solid var(--border-color); border-radius:6px; justify-content:space-between;">
      <div style="display:flex; align-items:center; gap:8px; flex-wrap:wrap;">
        <span style="color:#00ffc8; font-weight:700;">DESPLIEGUE:</span>
        <button class="btn active" id="btnModeRoll" onclick="setRollMode(true)" title="Despliegue continuo de derecha a izquierda como sismógrafo / cascada">📈 ROLLO S&Iacute;SMICO (Continuo)</button>
        <button class="btn" id="btnModeScope" onclick="setRollMode(false)" title="Osciloscopio estacionario anclado al disparo (Trigger)">🎯 OSCILOSCOPIO (Fijo)</button>
        <span style="margin-left:4px;">Vel. Rollo:</span>
        <button class="btn" id="btnRollSlow" onclick="setRollSpeed(0.18)">Lento</button>
        <button class="btn active" id="btnRollMed" onclick="setRollSpeed(0.38)">Normal</button>
        <button class="btn" id="btnRollFast" onclick="setRollSpeed(0.80)">R&aacute;pido</button>
        <span style="color:var(--border-color);">|</span>
        <span style="color:var(--accent-yellow); font-weight:600;">TRIGGER:</span>
        <button class="btn active" id="btnTrigAuto" onclick="setTrigMode('auto')">AUTO</button>
        <button class="btn" id="btnTrigNorm" onclick="setTrigMode('normal')">NORMAL</button>
        <button class="btn" id="btnTrigHold" onclick="toggleTrigHold()">RUN</button>
        <button class="btn active" id="btnTrigSlope" onclick="toggleTrigSlope()">SLOPE: &uarr; RISING</button>
      </div>
      <div style="display:flex; align-items:center; gap:10px; flex-wrap:wrap;">
        <label style="display:flex; align-items:center; gap:4px; cursor:pointer;">
          <input type="checkbox" id="chkTrigAutoLvl" checked onchange="toggleAutoTrigLevel(this.checked)">
          <span>Auto 50% Level</span>
        </label>
        <input type="range" id="trigLevelSlider" min="-1500" max="1500" step="10" value="0" disabled oninput="onTrigSliderInput(this.value)" style="width:120px; accent-color:var(--accent-yellow);">
        <span id="trigLevelText" style="font-family:Consolas,monospace; color:var(--accent-yellow); min-width:105px;">0 mV (Auto)</span>
      </div>
    </div>
    <canvas id="scopeCanvas" width="1200" height="250"></canvas>
  </div>

  <!-- Panel 2: High-Definition 2D CWT Scalogram Heatmap + Colorbar -->
  <div class="panel">
    <div class="panel-header">
      <div class="panel-title" id="cwtPanelTitle">CONTINUOUS WAVELET TRANSFORM (CWT) &mdash; HIGH-DEFINITION REAL MORLET SCALOGRAM</div>
      <div class="controls">
        <span>View Mode:</span>
        <button class="btn active" id="btnCwtModeLobes" onclick="setCwtViewMode('hd_lobes')">HD Lobes (Scale 1&ndash;50)</button>
        <button class="btn" id="btnCwtModeHw" onclick="setCwtViewMode('stm32_env')">STM32 24-Scale (10Hz&ndash;4kHz)</button>
        <span>Norm:</span>
        <button class="btn active" id="btnNormL1" onclick="setCwtNorm('l1')">L1 Flat</button>
        <button class="btn" id="btnNormAuto" onclick="setCwtNorm('autosignal')">AUTOSIGNAL (&radic;a)</button>
        <span>Scale:</span>
        <button class="btn active" id="btnScaleLin" onclick="setCwtScale('linear')">Linear</button>
        <button class="btn" id="btnScaleDb" onclick="setCwtScale('db')">Log (dB)</button>
        <span>Colormap:</span>
        <button class="btn active" id="btnMapJet" onclick="setColormap('jet')">Jet (MATLAB)</button>
        <button class="btn" id="btnMapTurbo" onclick="setColormap('turbo')">Turbo</button>
        <button class="btn" id="btnMapInferno" onclick="setColormap('inferno')">Inferno</button>
        <button class="btn" id="btnMapViridis" onclick="setColormap('viridis')">Viridis</button>
        <label style="display:flex; align-items:center; gap:4px; cursor:pointer; margin-left:4px;">
          <input type="checkbox" id="chkShowRidge" onchange="toggleRidgeOverlay(this.checked)">
          <span>Ridge</span>
        </label>
      </div>
    </div>
    <canvas id="cwtCanvas" width="1200" height="340"></canvas>
  </div>

  <!-- Panel 3: AUTOSIGNAL Dominant Frequency & find_high_amplitude_times() Slip Detector -->
  <div class="panel">
    <div class="panel-header">
      <div style="display:flex; align-items:center; gap:12px; flex-wrap:wrap;">
        <div class="panel-title">AUTOSIGNAL ANALYSIS: DOMINANT FREQUENCY &amp; BAND AMPLITUDE PROFILE (find_high_amplitude_times)</div>
        <span id="slipTimeBadge" style="padding:3px 10px; border-radius:4px; font-size:0.78rem; font-weight:700; font-family:Consolas,monospace; border:1px solid #f85149; background:rgba(248,81,73,0.15); color:#ff7b72;">Slip_time (&Delta;t): -- &mu;s</span>
        <span id="slipRangeBadge" style="font-size:0.76rem; font-family:Consolas,monospace; color:var(--text-muted);">Range: [--, --] &mu;s | Pts: 0/512</span>
      </div>
      <div class="controls">
        <span>Band [f<sub>min</sub>, f<sub>max</sub>] (Hz):</span>
        <input type="number" id="inpBandMin" value="40" min="10" max="3900" step="10" onchange="onBandInputChange()" style="width:72px; background:#0d131d; color:var(--text-primary); border:1px solid var(--border-color); border-radius:4px; padding:3px 6px; font-family:Consolas,monospace; font-size:0.76rem;">
        <span>to</span>
        <input type="number" id="inpBandMax" value="1000" min="20" max="4000" step="10" onchange="onBandInputChange()" style="width:78px; background:#0d131d; color:var(--text-primary); border:1px solid var(--border-color); border-radius:4px; padding:3px 6px; font-family:Consolas,monospace; font-size:0.76rem;">
        <button class="btn" onclick="setBandPreset(10, 200)" title="Spindle 1X rotation (600-10,000 RPM)">1X Husillo (10&ndash;200Hz)</button>
        <button class="btn active" id="btnBand1k10k" onclick="setBandPreset(40, 1000)" title="4-Flute Tooth Passing Frequency (TPF)">4X Gavilanes (40&ndash;1kHz)</button>
        <button class="btn" onclick="setBandPreset(500, 4000)" title="Regenerative chatter & harmonics">Chatter (500&ndash;4kHz)</button>
        <button class="btn" onclick="setBandPreset(10, 4000)">Full (10&ndash;4kHz)</button>
        <span>Profile:</span>
        <button class="btn active" id="btnProfMean" onclick="setProfileMode(false)">MEAN</button>
        <button class="btn" id="btnProfMax" onclick="setProfileMode(true)">MAX</button>
        <span>Thr:</span>
        <input type="range" id="thrRatioSlider" min="10" max="95" step="5" value="50" oninput="onThrSliderInput(this.value)" style="width:95px; accent-color:var(--accent-red);">
        <span id="thrRatioText" style="font-family:Consolas,monospace; color:var(--accent-red); min-width:36px;">50%</span>
      </div>
    </div>
    <canvas id="autosignalCanvas" width="1200" height="290"></canvas>
  </div>
</div>

<script>
let scopeMode = 'ac';
let zoomUs = 25600;         // Default 25.6 ms window (~4.2 spindle revs @ 10,000 RPM, 100% real samples!)
let cwtViewMode = 'hd_lobes'; // 'hd_lobes' (Scale 1..50 Real Morlet Lobes) | 'stm32_env' (24-Scale HW Envelope)
let cwtScale = 'linear';
let cwtNorm = 'l1';         // 'l1' (flat mV) | 'autosignal' (sqrt(a) energy weighting)
let showRidgeOverlay = false;
let colormap = 'jet';
let latestState = null;

// AUTOSIGNAL find_high_amplitude_times() state
const autoSigState = {
  freqMin: 40.0,
  freqMax: 1000.0,
  thresholdRatio: 0.50,
  useMax: false
};

// Sub-Sample Oscilloscope Trigger State (prevents horizontal waveform displacement & tail flashing)
const trigState = {
  mode: 'auto',          // 'auto' | 'normal'
  hold: false,           // true = freeze waveform
  slope: 'rising',       // 'rising' | 'falling'
  autoLevel: true,       // true = auto midpoint (50%) trigger level
  manualLevelMv: 0,      // manual threshold in mV
  effectiveLevelMv: 0,   // active threshold in mV
  trigPosPct: 0.15,      // trigger point anchored at 15% from left edge
  isTriggered: false,
  lastValidTrace: null,  // held trace for NORMAL or HOLD mode
  screenBuf: null        // DSO trigger-locked persistence buffer [0..cols] eliminating right-edge flashing
};

// Seismic Roll (Continuous Strip-Chart / Waterfall) State
const rollState = {
  enabled: true,             // Default: unroll continuously right-to-left like a seismograph
  speedScreensPerSec: 0.38,  // Fraction of window scrolled per second (0.18, 0.38, 0.80)
  scrollUs: 0.0,             // Cumulative time scrolled in microseconds
  phaseUs: 0.0,              // Modulo-wrapped phase offset in microseconds (zero phase-jump on packet update)
  lastRafMs: performance.now()
};

function setRollMode(enabled) {
  rollState.enabled = enabled;
  document.getElementById('btnModeRoll').classList.toggle('active', enabled);
  document.getElementById('btnModeScope').classList.toggle('active', !enabled);
  if (!enabled) {
    rollState.scrollUs = 0.0;
    rollState.phaseUs = 0.0;
  }
  trigState.screenBufValid = false;
  if (latestState) renderAll(latestState);
}

function setRollSpeed(spd) {
  rollState.speedScreensPerSec = spd;
  document.getElementById('btnRollSlow').classList.toggle('active', Math.abs(spd - 0.18) < 0.02);
  document.getElementById('btnRollMed').classList.toggle('active', Math.abs(spd - 0.38) < 0.02);
  document.getElementById('btnRollFast').classList.toggle('active', Math.abs(spd - 0.80) < 0.02);
}

function setScopeMode(m) {
  scopeMode = m;
  document.getElementById('btnViewAc').classList.toggle('active', m === 'ac');
  document.getElementById('btnViewAbs').classList.toggle('active', m === 'abs');
  const slider = document.getElementById('trigLevelSlider');
  if (m === 'ac') {
    slider.min = -1500;
    slider.max = 1500;
    if (!trigState.autoLevel) {
      trigState.manualLevelMv = Math.max(-1500, Math.min(1500, trigState.manualLevelMv - 1500));
      slider.value = trigState.manualLevelMv;
    }
  } else {
    slider.min = 0;
    slider.max = 3300;
    if (!trigState.autoLevel) {
      trigState.manualLevelMv = Math.max(0, Math.min(3300, trigState.manualLevelMv + 1500));
      slider.value = trigState.manualLevelMv;
    }
  }
  trigState.lastValidTrace = null;
  trigState.screenBuf = null;
  if (latestState) renderAll(latestState);
}

function setZoomUs(u) {
  zoomUs = u;
  [6000, 12000, 25600, 51200, 250000, 500000, 1000000].forEach(val => {
    const el = document.getElementById('btnZoom' + val);
    if (el) el.classList.toggle('active', val === u);
  });
  trigState.screenBuf = null;
  trigState.displayBuf = null;
  trigState.cwtDisplayGrid = null;
  fetch('/api/phm?win_us=' + u, { method: 'POST' }).catch(() => {});
  if (latestState) renderAll(latestState);
}

function setTrigMode(m) {
  trigState.mode = m;
  document.getElementById('btnTrigAuto').classList.toggle('active', m === 'auto');
  document.getElementById('btnTrigNorm').classList.toggle('active', m === 'normal');
  if (latestState) renderAll(latestState);
}

function toggleTrigHold() {
  trigState.hold = !trigState.hold;
  const btn = document.getElementById('btnTrigHold');
  btn.textContent = trigState.hold ? 'HOLD (FROZEN)' : 'RUN';
  btn.classList.toggle('active', trigState.hold);
  if (latestState) renderAll(latestState);
}

function toggleTrigSlope() {
  trigState.slope = (trigState.slope === 'rising') ? 'falling' : 'rising';
  const btn = document.getElementById('btnTrigSlope');
  btn.innerHTML = (trigState.slope === 'rising') ? 'SLOPE: &uarr; RISING' : 'SLOPE: &darr; FALLING';
  trigState.screenBuf = null;
  if (latestState) renderAll(latestState);
}

function toggleAutoTrigLevel(checked) {
  trigState.autoLevel = checked;
  document.getElementById('trigLevelSlider').disabled = checked;
  if (!checked) {
    trigState.manualLevelMv = Number(document.getElementById('trigLevelSlider').value);
    document.getElementById('trigLevelText').textContent = trigState.manualLevelMv + ' mV';
  }
  if (latestState) renderAll(latestState);
}

function onTrigSliderInput(val) {
  trigState.manualLevelMv = Number(val);
  document.getElementById('trigLevelText').textContent = val + ' mV';
  if (latestState) renderAll(latestState);
}

// Sub-Sample Jitter-Free Trigger Search with Hysteresis & Period Estimation
function findSubSampleTrigger(samples, trigLevel, slope, preTriggerSamples) {
  const n = samples.length;
  if (n < 16) return { trig: -1, period: 0 };
  let vMin = samples[0], vMax = samples[0];
  let iMin = 0, iMax = 0;
  for (let i = 1; i < n; i++) {
    if (samples[i] < vMin) { vMin = samples[i]; iMin = i; }
    if (samples[i] > vMax) { vMax = samples[i]; iMax = i; }
  }
  const vpp = vMax - vMin;
  if (vpp < 8.0) return { trig: -1, period: 0 };

  // Adaptive noise-rejection hysteresis band (8% of Vpp, clamped to [6, 90] mV)
  const hyst = Math.max(6.0, Math.min(90.0, vpp * 0.08));
  const crossings = [];

  if (slope === 'rising') {
    let armed = false;
    for (let i = 1; i < n - 1; i++) {
      if (samples[i - 1] < trigLevel - hyst) {
        armed = true;
      }
      if (armed && samples[i - 1] <= trigLevel && samples[i] > trigLevel) {
        const dy = samples[i] - samples[i - 1];
        const frac = (dy > 1e-4) ? (trigLevel - samples[i - 1]) / dy : 0.0;
        crossings.push((i - 1) + Math.max(0.0, Math.min(1.0, frac)));
        armed = false;
      }
    }
  } else {
    let armed = false;
    for (let i = 1; i < n - 1; i++) {
      if (samples[i - 1] > trigLevel + hyst) {
        armed = true;
      }
      if (armed && samples[i - 1] >= trigLevel && samples[i] < trigLevel) {
        const dy = samples[i - 1] - samples[i];
        const frac = (dy > 1e-4) ? (samples[i - 1] - trigLevel) / dy : 0.0;
        crossings.push((i - 1) + Math.max(0.0, Math.min(1.0, frac)));
        armed = false;
      }
    }
  }

  if (crossings.length === 0) {
    return { trig: -1, period: 0 };
  }

  // Estimate signal period in samples (from multi-crossing spacing or peak-to-trough distance)
  let period = 0;
  if (crossings.length >= 2) {
    period = (crossings[crossings.length - 1] - crossings[0]) / (crossings.length - 1);
  } else {
    const halfP = Math.abs(iMax - iMin);
    if (halfP >= 8) period = 2.0 * halfP;
  }

  // Prefer the first crossing that has at least preTriggerSamples to its left,
  // or fall back to crossings[0] so low-frequency signals still trigger reliably.
  let chosen = crossings[0];
  for (let k = 0; k < crossings.length; k++) {
    if (crossings[k] >= preTriggerSamples && crossings[k] <= n * 0.78) {
      chosen = crossings[k];
      break;
    }
  }
  return { trig: chosen, period: period };
}

function setCwtViewMode(vMode) {
  cwtViewMode = vMode;
  document.getElementById('btnCwtModeLobes').classList.toggle('active', vMode === 'hd_lobes');
  document.getElementById('btnCwtModeHw').classList.toggle('active', vMode === 'stm32_env');
  const titleEl = document.getElementById('cwtPanelTitle');
  if (vMode === 'hd_lobes') {
    titleEl.innerHTML = 'CONTINUOUS WAVELET TRANSFORM (CWT) &mdash; HIGH-DEFINITION REAL MORLET SCALOGRAM (Scale 1&ndash;50)';
  } else {
    titleEl.innerHTML = '2D CONTINUOUS WAVELET TRANSFORM SCALOGRAM (STM32 CM7 24 Log Scales: 10 Hz &ndash; 4 kHz, Cubic-Smoothed)';
  }
  if (latestState) renderAll(latestState);
}

function setCwtScale(s) {
  cwtScale = s;
  document.getElementById('btnScaleLin').classList.toggle('active', s === 'linear');
  document.getElementById('btnScaleDb').classList.toggle('active', s === 'db');
  if (latestState) renderAll(latestState);
}

function setCwtNorm(nMode) {
  cwtNorm = nMode;
  document.getElementById('btnNormL1').classList.toggle('active', nMode === 'l1');
  document.getElementById('btnNormAuto').classList.toggle('active', nMode === 'autosignal');
  if (latestState) renderAll(latestState);
}

function toggleRidgeOverlay(checked) {
  showRidgeOverlay = checked;
  if (latestState) renderAll(latestState);
}

function setColormap(c) {
  colormap = c;
  document.getElementById('btnMapInferno').classList.toggle('active', c === 'inferno');
  document.getElementById('btnMapTurbo').classList.toggle('active', c === 'turbo');
  document.getElementById('btnMapViridis').classList.toggle('active', c === 'viridis');
  document.getElementById('btnMapJet').classList.toggle('active', c === 'jet');
  if (latestState) renderAll(latestState);
}

function onBandInputChange() {
  let fMin = parseFloat(document.getElementById('inpBandMin').value) || 100;
  let fMax = parseFloat(document.getElementById('inpBandMax').value) || 10000;
  if (fMin >= fMax) fMin = Math.max(100, fMax - 100);
  autoSigState.freqMin = fMin;
  autoSigState.freqMax = fMax;
  if (latestState) renderAll(latestState);
}

function setBandPreset(fMin, fMax) {
  autoSigState.freqMin = fMin;
  autoSigState.freqMax = fMax;
  document.getElementById('inpBandMin').value = fMin;
  document.getElementById('inpBandMax').value = fMax;
  if (latestState) renderAll(latestState);
}

function setProfileMode(useMax) {
  autoSigState.useMax = useMax;
  document.getElementById('btnProfMean').classList.toggle('active', !useMax);
  document.getElementById('btnProfMax').classList.toggle('active', useMax);
  if (latestState) renderAll(latestState);
}

function onThrSliderInput(val) {
  autoSigState.thresholdRatio = Number(val) / 100.0;
  document.getElementById('thrRatioText').textContent = val + '%';
  if (latestState) renderAll(latestState);
}

async function setOffsetMode(mode) {
  try {
    await fetch('/api/mode?mode=' + encodeURIComponent(mode), { method: 'POST' });
  } catch (e) {}
}

let phmPlaying = true;
let activePhmChannel = 1;
let activePhmSec = 1.00;

// Machining Tool & Diagnostic Overlay State
const toolConfig = {
  flutes: 4,             // 3 or 4 flutes
  rpm: 10400,            // Spindle speed in RPM
  showGuides: true,      // Show Rev # / Gavilán G1..Gz guides & 1X/zX lines
  cursorFracX: -1,       // Synchronized mouse X fraction [0..1] across all 3 plots
  cursorFracY: -1,       // Mouse Y fraction [0..1] in active canvas
  cursorCanvasId: '',
  lastBaseStep: 1.0,
  lastCwtGrid: null,
  lastCwtMax: 1.0
};

function updateToolSummaryText() {
  const f1x = toolConfig.rpm / 60.0;
  const tRevMs = 1000.0 / f1x;
  const fTpf = f1x * toolConfig.flutes;
  const tFluteMs = tRevMs / toolConfig.flutes;
  const el = document.getElementById('toolFreqSummary');
  if (el) {
    el.innerHTML =
      '1X Giro: <strong style="color:#d29922;">' + f1x.toFixed(1) + ' Hz</strong> (' + tRevMs.toFixed(2) + ' ms/rev) | ' +
      toolConfig.flutes + 'X Paso Gavil&aacute;n: <strong style="color:#00ffc8;">' + fTpf.toFixed(1) + ' Hz</strong> (' + tFluteMs.toFixed(2) + ' ms/filo)';
  }
}

function setToolFlutes(z) {
  toolConfig.flutes = z;
  document.getElementById('btnFlutes3').classList.toggle('active', z === 3);
  document.getElementById('btnFlutes4').classList.toggle('active', z === 4);
  updateToolSummaryText();
  if (latestState) renderAll(latestState);
}

function onToolRpmChange(val) {
  const r = Math.max(300, Math.min(30000, Number(val) || 10400));
  toolConfig.rpm = r;
  document.getElementById('inpToolRpm').value = r;
  updateToolSummaryText();
  if (latestState) renderAll(latestState);
}

function toggleMachiningGuides(checked) {
  toolConfig.showGuides = checked;
  if (latestState) renderAll(latestState);
}

async function injectPhmToAd2() {
  const badge = document.getElementById('phmInfoBadge');
  if (badge) badge.textContent = 'Programando AD2 W1 (1.5V Offset)...';
  try {
    const r = await fetch(
      '/api/ad2_inject?channel=' + activePhmChannel + '&sec=' + activePhmSec.toFixed(2),
      { method: 'POST' }
    );
    if (r.ok) {
      const res = await r.json();
      document.getElementById('btnSrcStm32').classList.add('active');
      document.getElementById('btnSrcPhm').classList.remove('active');
      trigState.screenBuf = null;
      if (badge) {
        badge.textContent = 'AD2 W1 Activo: ' + (res.label || ('Ch ' + activePhmChannel)) +
          ' @ t=' + activePhmSec.toFixed(2) + 's (1.5V Offset) -> Capturando en STM32 PB1';
      }
    }
  } catch (e) {
    if (badge) badge.textContent = 'Error al programar AD2 W1';
  }
}

async function setDataSource(src) {
  document.getElementById('btnSrcStm32').classList.toggle('active', src === 'stm32');
  document.getElementById('btnSrcPhm').classList.toggle('active', src === 'phm');
  trigState.screenBuf = null;
  trigState.lastValidTrace = null;
  try {
    await fetch('/api/phm?source=' + encodeURIComponent(src), { method: 'POST' });
  } catch (e) {}
}

async function setPhmChannel(ch) {
  activePhmChannel = ch;
  for (let i = 0; i < 7; i++) {
    const el = document.getElementById('btnPhmCh' + i);
    if (el) el.classList.toggle('active', i === ch);
  }
  trigState.screenBuf = null;
  trigState.lastValidTrace = null;
  try {
    await fetch('/api/phm?channel=' + ch, { method: 'POST' });
  } catch (e) {}
}

async function togglePhmPlay() {
  phmPlaying = !phmPlaying;
  const btn = document.getElementById('btnPhmPlay');
  btn.innerHTML = phmPlaying ? '&#9654; PLAY' : '&#9208; PAUSE';
  btn.classList.toggle('active', phmPlaying);
  try {
    await fetch('/api/phm?play=' + (phmPlaying ? '1' : '0'), { method: 'POST' });
  } catch (e) {}
}

async function seekPhmSec(sec) {
  activePhmSec = Number(sec);
  document.getElementById('phmTimeSlider').value = Math.round(sec * 100);
  trigState.screenBuf = null;
  try {
    await fetch('/api/phm?sec=' + sec, { method: 'POST' });
  } catch (e) {}
}

async function onPhmSliderInput(val) {
  activePhmSec = Number(val) / 100.0;
  const sec = activePhmSec.toFixed(2);
  try {
    await fetch('/api/phm?sec=' + sec, { method: 'POST' });
  } catch (e) {}
}

function colorRGB(t, mapName) {
  t = Math.max(0, Math.min(1, t));
  if (mapName === 'jet') {
    // Exact 8-stop MATLAB jet(256) colormap:
    // 0.000 -> [0, 0, 128] (dark navy)
    // 0.125 -> [0, 0, 255] (blue)
    // 0.375 -> [0, 255, 255] (cyan)
    // 0.625 -> [255, 255, 0] (yellow)
    // 0.875 -> [255, 0, 0] (red)
    // 1.000 -> [128, 0, 0] (dark crimson)
    const r = Math.round(255 * Math.min(1, Math.max(0, 1.5 - Math.abs(4.0 * t - 3.0))));
    const g = Math.round(255 * Math.min(1, Math.max(0, 1.5 - Math.abs(4.0 * t - 2.0))));
    const b = Math.round(255 * Math.min(1, Math.max(0, 1.5 - Math.abs(4.0 * t - 1.0))));
    return [r, g, b];
  }
  if (mapName === 'turbo') {
    const r = Math.round(255 * Math.min(1, Math.max(0, 1.5 - Math.abs(3.8 * t - 2.8))));
    const g = Math.round(255 * Math.min(1, Math.max(0, Math.sin(t * Math.PI))));
    const b = Math.round(255 * Math.min(1, Math.max(0, 1.5 - Math.abs(3.8 * t - 1.1))));
    return [r, g, b];
  }
  if (mapName === 'viridis') {
    const r = Math.round(255 * Math.pow(t, 1.6));
    const g = Math.round(255 * (0.1 + 0.85 * t));
    const b = Math.round(255 * (0.35 + 0.45 * Math.sin(t * Math.PI)));
    return [r, g, b];
  }
  // Default: Inferno-like perceptually vivid map
  const r = Math.round(255 * Math.min(1, Math.pow(t * 1.25, 1.1)));
  const g = Math.round(255 * Math.max(0, Math.min(1, (t - 0.25) * 1.35)));
  const b = Math.round(255 * (t < 0.5 ? t * 1.4 : Math.max(0.15, (t - 0.5) * 1.8)));
  return [r, g, b];
}

// Catmull-Rom cubic interpolation helper for smooth vertical scale upsampling
function cubicInterpRow(row0, row1, row2, row3, frac, t) {
  const v0 = row0[t], v1 = row1[t], v2 = row2[t], v3 = row3[t];
  const a0 = -0.5 * v0 + 1.5 * v1 - 1.5 * v2 + 0.5 * v3;
  const a1 = v0 - 2.5 * v1 + 2.0 * v2 - 0.5 * v3;
  const a2 = -0.5 * v0 + 0.5 * v2;
  const a3 = v1;
  return Math.max(0.0, ((a0 * frac + a1) * frac + a2) * frac + a3);
}

// Renders the right-hand MATLAB-style Magnitude Colorbar
function drawColorbar(ctx, xBar, yBar, barW, barH, maxVal, unitStr) {
  const cbarImg = ctx.createImageData(barW, barH);
  const cdata = cbarImg.data;
  for (let y = 0; y < barH; y++) {
    const norm = 1.0 - (y / Math.max(1, barH - 1));
    const [r, g, b] = colorRGB(norm, colormap);
    for (let x = 0; x < barW; x++) {
      const idx = (y * barW + x) * 4;
      cdata[idx] = r;
      cdata[idx + 1] = g;
      cdata[idx + 2] = b;
      cdata[idx + 3] = 255;
    }
  }
  ctx.putImageData(cbarImg, xBar, yBar);
  ctx.strokeStyle = '#8b949e';
  ctx.lineWidth = 1;
  ctx.strokeRect(xBar, yBar, barW, barH);

  ctx.fillStyle = '#e6edf3';
  ctx.font = '11px Consolas, monospace';
  const ticks = 4;
  for (let i = 0; i <= ticks; i++) {
    const frac = i / ticks;
    const y = yBar + (1.0 - frac) * barH;
    const val = frac * maxVal;
    ctx.beginPath();
    ctx.moveTo(xBar + barW, y);
    ctx.lineTo(xBar + barW + 4, y);
    ctx.stroke();
    const lbl = (maxVal >= 20) ? Math.round(val).toString() : val.toFixed(1);
    ctx.fillText(lbl, xBar + barW + 7, y + 4);
  }

  ctx.save();
  ctx.translate(xBar + barW + 38, yBar + barH * 0.5);
  ctx.rotate(-Math.PI / 2);
  ctx.textAlign = 'center';
  ctx.fillStyle = '#e6edf3';
  ctx.font = '12px sans-serif';
  ctx.fillText(unitStr || 'Magnitude', 0, 0);
  ctx.restore();
}

function renderScope(st) {
  const canvas = document.getElementById('scopeCanvas');
  const ctx = canvas.getContext('2d');
  const W = canvas.width;
  const H = canvas.height;
  ctx.clearRect(0, 0, W, H);

  const raw = st.raw_waveform || [];
  if (!raw.length) return;

  const padL = 68, padR = 28, padT = 18, padB = 26;
  const plotW = W - padL - padR;
  const plotH = H - padT - padB;

  const dcMv = (st.offset_mode === 0) ? st.measured_dc_mv : st.nominal_offset_mv;
  const samplesMv = raw.map(r => (r * 3300.0) / 65535.0);
  const incomingTrace = (scopeMode === 'ac')
    ? samplesMv.map(v => v - dcMv)
    : samplesMv;

  const yMin = (scopeMode === 'ac') ? -1650 : 0;
  const yMax = (scopeMode === 'ac') ? +1650 : 3300;

  // Determine effective trigger level (Auto 50% midpoint vs Manual slider)
  let vMin = incomingTrace[0], vMax = incomingTrace[0];
  for (let i = 1; i < incomingTrace.length; i++) {
    if (incomingTrace[i] < vMin) vMin = incomingTrace[i];
    if (incomingTrace[i] > vMax) vMax = incomingTrace[i];
  }
  if (trigState.autoLevel) {
    trigState.effectiveLevelMv = (vMax + vMin) * 0.5;
    document.getElementById('trigLevelSlider').value = Math.round(trigState.effectiveLevelMv);
    const sign = (scopeMode === 'ac' && trigState.effectiveLevelMv > 0) ? '+' : '';
    document.getElementById('trigLevelText').textContent =
      sign + Math.round(trigState.effectiveLevelMv) + ' mV (Auto)';
  } else {
    trigState.effectiveLevelMv = trigState.manualLevelMv;
  }

  // Search sub-sample trigger crossing
  const tsUs = 1e6 / (st.sampling_rate_hz || 500000); // 2.0 us/sample
  const preTrigSamples = (trigState.trigPosPct * zoomUs) / tsUs;

  let activeTrace = incomingTrace;
  let trigRes = { trig: -1, period: 0 };

  if (trigState.hold && trigState.lastValidTrace) {
    activeTrace = trigState.lastValidTrace;
    trigRes = findSubSampleTrigger(
      activeTrace, trigState.effectiveLevelMv, trigState.slope, preTrigSamples
    );
    trigState.isTriggered = (trigRes.trig >= 0);
  } else {
    trigRes = findSubSampleTrigger(
      incomingTrace, trigState.effectiveLevelMv, trigState.slope, preTrigSamples
    );
    if (trigRes.trig >= 0) {
      trigState.isTriggered = true;
      trigState.lastValidTrace = incomingTrace.slice();
    } else {
      trigState.isTriggered = false;
      if (trigState.mode === 'normal' && trigState.lastValidTrace) {
        activeTrace = trigState.lastValidTrace;
        trigRes = findSubSampleTrigger(
          activeTrace, trigState.effectiveLevelMv, trigState.slope, preTrigSamples
        );
      }
    }
  }
  const trigSampleFrac = trigRes.trig;
  const estPeriod = trigRes.period;

  // Update Trigger Status Badge
  const badge = document.getElementById('trigStatusBadge');
  if (trigState.hold) {
    badge.textContent = 'HOLD \u23F8';
    badge.style.color = '#bc8cff';
    badge.style.borderColor = '#8957e5';
    badge.style.background = 'rgba(188,140,255,0.15)';
  } else if (trigState.isTriggered) {
    badge.textContent = "TRIG'D \u2713";
    badge.style.color = '#39d353';
    badge.style.borderColor = '#2ea043';
    badge.style.background = 'rgba(57,211,83,0.15)';
  } else if (trigState.mode === 'normal') {
    badge.textContent = 'WAIT...';
    badge.style.color = '#d29922';
    badge.style.borderColor = '#9e6a03';
    badge.style.background = 'rgba(210,153,34,0.15)';
  } else {
    badge.textContent = 'AUTO (FREE)';
    badge.style.color = '#58a6ff';
    badge.style.borderColor = '#1f6feb';
    badge.style.background = 'rgba(56,139,253,0.15)';
  }

  // Grid lines (10 horizontal x 6 vertical divisions)
  ctx.strokeStyle = '#1b2434';
  ctx.lineWidth = 1;
  ctx.fillStyle = '#8b949e';
  ctx.font = '11px Consolas, monospace';
  const ySteps = 6;
  for (let i = 0; i <= ySteps; i++) {
    const y = padT + (i / ySteps) * plotH;
    const v = yMax - (i / ySteps) * (yMax - yMin);
    ctx.beginPath();
    ctx.moveTo(padL, y);
    ctx.lineTo(padL + plotW, y);
    ctx.stroke();
    const lbl = (scopeMode === 'ac' && v > 0 ? '+' : '') + Math.round(v) + ' mV';
    ctx.fillText(lbl, 6, y + 4);
  }
  const xSteps = 8;
  for (let i = 0; i <= xSteps; i++) {
    const x = padL + (i / xSteps) * plotW;
    ctx.beginPath();
    ctx.moveTo(x, padT);
    ctx.lineTo(x, padT + plotH);
    ctx.stroke();
  }

  // Draw 1.5V Bias Reference Line
  const refVal = (scopeMode === 'ac') ? 0.0 : 1500.0;
  const yRef = padT + ((yMax - refVal) / (yMax - yMin)) * plotH;
  ctx.save();
  ctx.strokeStyle = 'rgba(210, 153, 34, 0.55)';
  ctx.setLineDash([6, 4]);
  ctx.beginPath();
  ctx.moveTo(padL, yRef);
  ctx.lineTo(padL + plotW, yRef);
  ctx.stroke();
  ctx.fillStyle = '#d29922';
  ctx.fillText(scopeMode === 'ac' ? '0 mV (1.5V Bias)' : '1500 mV Bias', padL + 8, yRef - 5);
  ctx.restore();

  // Draw Horizontal Trigger Level Line & Right-Margin Arrow (T ▶)
  const clampedTrig = Math.max(yMin, Math.min(yMax, trigState.effectiveLevelMv));
  const yTrig = padT + ((yMax - clampedTrig) / (yMax - yMin)) * plotH;
  ctx.save();
  ctx.strokeStyle = '#39d353';
  ctx.lineWidth = 1;
  ctx.setLineDash([3, 3]);
  ctx.beginPath();
  ctx.moveTo(padL, yTrig);
  ctx.lineTo(padL + plotW, yTrig);
  ctx.stroke();
  ctx.setLineDash([]);
  ctx.fillStyle = '#39d353';
  ctx.font = 'bold 10px Consolas, monospace';
  ctx.fillText('\u25C0T', padL + plotW + 3, yTrig + 3);
  ctx.restore();

  // Draw Vertical Trigger Anchor Pointer (▼ T) at trigPosPct (15% from left)
  const xTrig = padL + plotW * trigState.trigPosPct;
  ctx.save();
  ctx.strokeStyle = 'rgba(57, 211, 83, 0.35)';
  ctx.setLineDash([2, 3]);
  ctx.beginPath();
  ctx.moveTo(xTrig, padT);
  ctx.lineTo(xTrig, padT + plotH);
  ctx.stroke();
  ctx.fillStyle = '#39d353';
  ctx.font = 'bold 10px Consolas, monospace';
  ctx.fillText('\u25BC T', xTrig - 10, padT - 4);
  ctx.restore();

  // Sub-Sample Interpolated Waveform Rendering Aligned to Trigger Point
  // Uses DSO Trigger-Locked Screen Persistence + Periodic Continuation so the
  // right tail never cuts off or flashes when shifted by tLeftUs.
  const tTrigUs = (trigSampleFrac >= 0)
    ? (trigSampleFrac * tsUs)
    : (trigState.trigPosPct * zoomUs);

  const cols = Math.min(plotW, 700);
  const n = activeTrace.length;
  if (!trigState.screenBuf || trigState.screenBuf.length !== cols + 1) {
    trigState.screenBuf = new Float32Array(cols + 1);
    trigState.displayBuf = new Float32Array(cols + 1);
    trigState.screenBufValid = false;
    trigState.smoothPeriod = 0;
  }

  function sampleInterp(idx) {
    const clamped = Math.max(0, Math.min(n - 1, idx));
    const k = Math.floor(clamped);
    const alpha = clamped - k;
    const vNext = (k + 1 < n) ? activeTrace[k + 1] : activeTrace[k];
    return activeTrace[k] * (1.0 - alpha) + vNext * alpha;
  }

  // Smooth the estimated period across frames (EMA) so multi-millisecond windows
  // (2 ms, 4 ms, 8 ms) never breathe or shift horizontally between packets!
  if (estPeriod >= 12) {
    trigState.smoothPeriod = (trigState.smoothPeriod > 0)
      ? (0.90 * trigState.smoothPeriod + 0.10 * estPeriod)
      : estPeriod;
  }
  const P = (trigState.smoothPeriod >= 12) ? trigState.smoothPeriod : 0;

  // Build a C1-seamless 1-period waveform template [0 .. P] anchored at trigSampleFrac
  // so repeating across 2 ms .. 1000 ms or Seismic Roll has zero seam or jump
  const t0 = (trigSampleFrac >= 0) ? trigSampleFrac : 0;
  function periodicSample(sampleU) {
    if (!rollState.enabled && sampleU >= 0 && sampleU <= (n - 1)) {
      return sampleInterp(sampleU);
    }
    if (P >= 12) {
      let phase = (sampleU - t0) % P;
      if (phase < 0) phase += P;
      let idxA = t0 + phase;
      while (idxA > (n - 1)) idxA -= P;
      while (idxA < 0) idxA += P;
      const valA = sampleInterp(idxA);
      // Crossfade near wrap boundary to guarantee C0/C1 continuity
      const edgeFrac = phase / P;
      if (edgeFrac > 0.92 && (t0 + phase - P) >= 0) {
        const w = (edgeFrac - 0.92) / 0.08;
        const valB = sampleInterp(t0 + (phase - P));
        return (1.0 - w) * valA + w * valB;
      }
      return valA;
    }
    // O(1) symmetric mirror reflection for very low frequencies (P > 512)
    const span = Math.max(1, n - 1);
    const twoSpan = 2 * span;
    let mU = ((sampleU % twoSpan) + twoSpan) % twoSpan;
    if (mU > span) mU = twoSpan - mU;
    return sampleInterp(mU);
  }

  // In Seismic Roll Mode, wrap the phase offset modulo the period so there is zero phase jump on new UDP packets
  const periodUs = (P >= 12) ? (P * tsUs) : (2.0 * Math.max(1, n - 1) * tsUs);
  const rollShiftUs = rollState.enabled ? (rollState.scrollUs % periodUs) : 0.0;
  const tLeftUs = tTrigUs - trigState.trigPosPct * zoomUs + rollShiftUs;

  // Update screen buffer: in Seismic Roll Mode use alpha=1.0 for crisp 60 FPS motion; in fixed DSO mode use 0.65
  const emaAlpha = (rollState.enabled || !trigState.screenBufValid) ? 1.0 : 0.65;
  for (let c = 0; c <= cols; c++) {
    const fracCol = c / cols;
    const tUs = tLeftUs + fracCol * zoomUs;
    const sampleU = tUs / tsUs;
    const targetMv = periodicSample(sampleU);
    trigState.screenBuf[c] = trigState.screenBufValid
      ? ((1.0 - emaAlpha) * trigState.screenBuf[c] + emaAlpha * targetMv)
      : targetMv;
    trigState.displayBuf[c] = trigState.screenBuf[c];
  }

  // Also populate extended CWT buffer [-maxPad .. hdCols + maxPad] using real/periodic
  // samples beyond the window edges so the CWT has ZERO Cone-of-Influence edge distortion!
  const hdCols = 220, maxPad = 96;
  const totalPadLen = hdCols + 2 * maxPad;
  if (!trigState.cwtExtBuf || trigState.cwtExtBuf.length !== totalPadLen) {
    trigState.cwtExtBuf = new Float32Array(totalPadLen);
  }
  for (let i = 0; i < totalPadLen; i++) {
    const frac = (i - maxPad) / (hdCols - 1);
    const tUs = tLeftUs + frac * zoomUs;
    const targetMv = periodicSample(tUs / tsUs);
    trigState.cwtExtBuf[i] = trigState.screenBufValid
      ? ((1.0 - emaAlpha) * trigState.cwtExtBuf[i] + emaAlpha * targetMv)
      : targetMv;
  }
  trigState.screenBufValid = true;

  ctx.save();
  ctx.beginPath();
  ctx.rect(padL, padT, plotW, plotH);
  ctx.clip();

  ctx.strokeStyle = '#00ffc8';
  ctx.lineWidth = 2.0;
  ctx.beginPath();

  for (let c = 0; c <= cols; c++) {
    const fracCol = c / cols;
    const mv = trigState.displayBuf[c];
    const x = padL + fracCol * plotW;
    const y = padT + ((yMax - mv) / (yMax - yMin)) * plotH;
    const yc = Math.max(padT, Math.min(padT + plotH, y));
    if (c === 0) ctx.moveTo(x, yc);
    else ctx.lineTo(x, yc);
  }
  ctx.stroke();

  // Draw Revolution (Rev #) & Flute (G1..Gz) Vertical Guides on Oscilloscope
  // In Seismic Roll Mode, the vertical guides march from right to left locked to the wave crests!
  if (toolConfig.showGuides) {
    const tRevUs = (60.0 / Math.max(300, toolConfig.rpm)) * 1e6;
    const totalRevsInWin = zoomUs / tRevUs;
    const revStep = (totalRevsInWin > 60) ? 20 : (totalRevsInWin > 20 ? 5 : 1);
    const tFluteUs = tRevUs / toolConfig.flutes;
    const tTrigWinUs = trigState.trigPosPct * zoomUs;
    const scrollOff = rollState.enabled ? rollState.scrollUs : 0.0;
    const kMin = Math.floor((scrollOff - tTrigWinUs) / tRevUs) - 1;
    const kMax = Math.ceil((scrollOff + zoomUs - tTrigWinUs) / tRevUs) + 1;

    for (let k = kMin; k <= kMax; k++) {
      const revStartUs = tTrigWinUs + k * tRevUs - scrollOff;
      const absRevNum = k + 1;
      if (revStep === 1) {
        // Draw flute sub-divisions (G2..Gz) only in detailed view (<= 51.2 ms)
        for (let g = 1; g < toolConfig.flutes; g++) {
          const fgUs = revStartUs + g * tFluteUs;
          if (fgUs >= 0 && fgUs <= zoomUs) {
            const xg = padL + (fgUs / zoomUs) * plotW;
            ctx.strokeStyle = 'rgba(88, 166, 255, 0.22)';
            ctx.lineWidth = 1;
            ctx.setLineDash([2, 4]);
            ctx.beginPath();
            ctx.moveTo(xg, padT);
            ctx.lineTo(xg, padT + plotH);
            ctx.stroke();
            if (zoomUs <= 26000) {
              ctx.fillStyle = 'rgba(88, 166, 255, 0.75)';
              ctx.font = '9px Consolas, monospace';
              ctx.fillText('G' + (g + 1), xg + 3, padT + plotH - 5);
            }
          }
        }
      }
      // Draw main Revolution boundary (every revStep revolutions)
      if (revStartUs >= 0 && revStartUs <= zoomUs) {
        if (((absRevNum - 1) % revStep + revStep) % revStep === 0 && absRevNum >= 1) {
          const xr = padL + (revStartUs / zoomUs) * plotW;
          ctx.strokeStyle = 'rgba(188, 140, 255, 0.65)';
          ctx.lineWidth = 1.3;
          ctx.setLineDash([5, 3]);
          ctx.beginPath();
          ctx.moveTo(xr, padT);
          ctx.lineTo(xr, padT + plotH);
          ctx.stroke();
          ctx.setLineDash([]);
          ctx.fillStyle = '#d2a8ff';
          ctx.font = 'bold 10px Consolas, monospace';
          ctx.fillText('Rev' + absRevNum + (zoomUs <= 26000 ? ' (G1)' : ''), xr + 4, padT + 12);
        }
      }
    }
  }

  // Draw Seismic Roll Direction Banner at top-right of Scope
  if (rollState.enabled) {
    ctx.save();
    ctx.fillStyle = 'rgba(13, 19, 29, 0.82)';
    ctx.strokeStyle = 'rgba(0, 255, 200, 0.45)';
    ctx.lineWidth = 1;
    ctx.fillRect(padL + plotW - 255, padT + 4, 250, 18);
    ctx.strokeRect(padL + plotW - 255, padT + 4, 250, 18);
    ctx.fillStyle = '#00ffc8';
    ctx.font = 'bold 10px Consolas, monospace';
    ctx.fillText('\u2190 ROLLO S\u00cdSMICO | NUEVO ENTRA POR DER. \u2192', padL + plotW - 248, padT + 16);
    ctx.restore();
  }

  // Draw Synchronized Vertical Crosshair Cursor if active
  if (toolConfig.cursorFracX >= 0 && toolConfig.cursorFracX <= 1) {
    const xc = padL + toolConfig.cursorFracX * plotW;
    ctx.strokeStyle = 'rgba(255, 255, 255, 0.85)';
    ctx.lineWidth = 1.2;
    ctx.setLineDash([3, 3]);
    ctx.beginPath();
    ctx.moveTo(xc, padT);
    ctx.lineTo(xc, padT + plotH);
    ctx.stroke();
  }
  ctx.restore();

  // Compute Per-Flute Load & Automated Diagnosis from Triggered Waveform
  updateFluteDiagnostics(st, cols);

  // Time axis labels (in milliseconds, scrolling continuously in Seismic Roll Mode)
  const baseMs = rollState.enabled ? (rollState.scrollUs / 1000.0) : 0.0;
  for (let i = 0; i <= 8; i++) {
    const x = padL + (i / 8) * plotW;
    const tMs = baseMs + ((i / 8) * zoomUs) / 1000.0;
    const lbl = (tMs >= 1000.0) ? ((tMs / 1000.0).toFixed(2) + ' s') : (tMs.toFixed(1) + ' ms');
    ctx.fillText(lbl, x - 20, H - 6);
  }
}

function updateFluteDiagnostics(st, cols) {
  if (!trigState.displayBuf || cols < 32) return;
  const z = toolConfig.flutes;
  const tRevUs = (60.0 / Math.max(300, toolConfig.rpm)) * 1e6;
  const tTrigWinUs = trigState.trigPosPct * zoomUs;
  const scrollOff = rollState.enabled ? rollState.scrollUs : 0.0;
  const flutePeaks = new Float32Array(z);
  const fluteCounts = new Uint16Array(z);

  let meanVal = 0.0;
  for (let c = 0; c <= cols; c++) meanVal += trigState.displayBuf[c];
  meanVal /= (cols + 1);

  for (let c = 0; c <= cols; c++) {
    const tUs = (c / cols) * zoomUs + scrollOff;
    let relUs = (tUs - tTrigWinUs) % tRevUs;
    if (relUs < 0) relUs += tRevUs;
    const gIdx = Math.min(z - 1, Math.floor((relUs / tRevUs) * z));
    const amp = Math.abs(trigState.displayBuf[c] - meanVal);
    if (amp > flutePeaks[gIdx]) flutePeaks[gIdx] = amp;
    fluteCounts[gIdx]++;
  }

  let maxFluteAmp = 1.0;
  let minFluteAmp = 1e9;
  let dominantFlute = 0;
  for (let g = 0; g < z; g++) {
    if (flutePeaks[g] > maxFluteAmp) {
      maxFluteAmp = flutePeaks[g];
      dominantFlute = g;
    }
    if (flutePeaks[g] < minFluteAmp) minFluteAmp = flutePeaks[g];
  }

  // Render Per-Flute Mini-Bars (G1 .. Gz)
  const barsEl = document.getElementById('fluteBarsHtml');
  if (barsEl) {
    let html = '';
    for (let g = 0; g < z; g++) {
      const pct = Math.round((flutePeaks[g] / maxFluteAmp) * 100);
      const barColor = (pct >= 90) ? '#ff7b72' : (pct >= 65 ? '#39d353' : '#58a6ff');
      html +=
        '<div style="display:flex; align-items:center; gap:4px;">' +
        '<span style="color:#e6edf3; font-weight:700;">G' + (g + 1) + ':</span>' +
        '<div style="width:44px; height:8px; background:#1b2434; border-radius:4px; overflow:hidden;">' +
        '<div style="width:' + pct + '%; height:100%; background:' + barColor + ';"></div></div>' +
        '<span style="color:' + barColor + ';">' + pct + '%</span></div>';
    }
    barsEl.innerHTML = html;
  }

  // Compute 1X Spindle vs zX Tooth-Pass vs High-Freq Chatter (>1000 Hz) energy from CWT scalogram
  const scal = st.scalogram || [];
  const freqs = st.f_center_hz || [];
  let e1x = 1e-3, eTpf = 1e-3, eChatter = 0.0, eTotal = 1e-3;
  const f1x = toolConfig.rpm / 60.0;
  const fTpf = f1x * z;
  if (scal.length && freqs.length) {
    for (let s = 0; s < scal.length; s++) {
      const f = freqs[s] || 0;
      let rowSum = 0.0;
      const row = scal[s];
      for (let t = 0; t < row.length; t += 4) rowSum += row[t];
      eTotal += rowSum;
      if (Math.abs(f - f1x) < f1x * 0.45) e1x += rowSum;
      if (Math.abs(f - fTpf) < fTpf * 0.40) eTpf += rowSum;
      if (f >= 1000.0) eChatter += rowSum;
    }
  }
  const runoutRatio = e1x / Math.max(1e-3, eTpf);
  const chatterPct = Math.min(100, Math.round((eChatter / eTotal) * 100));
  const loadImbalance = (maxFluteAmp - minFluteAmp) / Math.max(1.0, maxFluteAmp);

  const metEl = document.getElementById('diagMetricsBadge');
  if (metEl) {
    metEl.textContent = 'Runout (1X/' + z + 'X): ' + runoutRatio.toFixed(2) + 'x | Chatter (>1kHz): ' + chatterPct + '%';
  }

  const pill = document.getElementById('diagStatusPill');
  const det = document.getElementById('diagDetailText');
  if (pill && det) {
    if (chatterPct > 38) {
      pill.textContent = 'ALERTA: CHATTER / VIBRACI\u00d3N ALTA \u26a0';
      pill.style.color = '#ff7b72';
      pill.style.borderColor = '#f85149';
      pill.style.background = 'rgba(248,81,73,0.18)';
      det.textContent = 'Energ\u00eda de alta frecuencia (>1 kHz) = ' + chatterPct + '%. Posible vibraci\u00f3n regenerativa o impacto severo.';
    } else if (loadImbalance > 0.32 || runoutRatio > 1.35) {
      pill.textContent = 'RUNOUT / ASIMETR\u00cdA EN GAVIL\u00c1N G' + (dominantFlute + 1);
      pill.style.color = '#ffd33d';
      pill.style.borderColor = '#d29922';
      pill.style.background = 'rgba(210,153,34,0.18)';
      det.textContent = 'El Gavil\u00e1n G' + (dominantFlute + 1) + ' toma el 100% del pico mientras el menor carga ' +
        Math.round((minFluteAmp / maxFluteAmp) * 100) + '% (1X/' + z + 'X = ' + runoutRatio.toFixed(2) + 'x). Excentricidad radial normal en fresado.';
    } else {
      pill.textContent = 'CORTE SIM\u00c9TRICO ESTABLE \u2713';
      pill.style.color = '#39d353';
      pill.style.borderColor = '#2ea043';
      pill.style.background = 'rgba(57,211,83,0.15)';
      det.textContent = 'Los ' + z + ' gavilanes presentan carga balanceada (desviaci\u00f3n <30%) y bajo nivel de chatter (' + chatterPct + '%).';
    }
  }
}

function renderCwt(st) {
  const canvas = document.getElementById('cwtCanvas');
  const ctx = canvas.getContext('2d');
  const W = canvas.width;
  const H = canvas.height;
  ctx.clearRect(0, 0, W, H);

  const scal = st.scalogram || [];
  const freqs = st.f_center_hz || [];
  if (!scal.length || !scal[0].length) return;

  const numScales = scal.length;
  const numTime = scal[0].length;
  const padL = 58, padR = 92, padT = 16, padB = 28;
  const plotW = W - padL - padR;
  const plotH = H - padT - padB;
  const magScale = st.magnitude_scale || (1.0 / 65535.0);

  // Build scale weights: L1 flat (1.0) vs AUTOSIGNAL sqrt(a) where a = omega0/(2*pi*f)
  const scaleWeights = new Float32Array(numScales);
  const fRef = freqs[numScales - 1] || 100000.0;
  for (let s = 0; s < numScales; s++) {
    const fHz = Math.max(1.0, freqs[s] || 100.0);
    scaleWeights[s] = (cwtNorm === 'autosignal') ? Math.sqrt(fRef / fHz) : 1.0;
  }

  // Find global weighted peak and per-time-bin dominant scale (AUTOSIGNAL ridge)
  let maxWeighted = 1e-6;
  const domScaleByTime = new Uint16Array(numTime);
  const domAmpByTime = new Float32Array(numTime);

  for (let t = 0; t < numTime; t++) {
    let bestS = 0;
    let bestVal = -1.0;
    for (let s = 0; s < numScales; s++) {
      const wVal = scal[s][t] * magScale * scaleWeights[s];
      if (wVal > bestVal) {
        bestVal = wVal;
        bestS = s;
      }
      if (wVal > maxWeighted) maxWeighted = wVal;
    }
    domScaleByTime[t] = bestS;
    domAmpByTime[t] = bestVal;
  }

  if (cwtViewMode === 'hd_lobes' && trigState.screenBuf && trigState.screenBuf.length > 32) {
    // =========================================================================
    // MODE A: High-Definition Real Morlet CWT (Scale 1..50, 64x220 branchless grid)
    // Precomputes 1D kernel per scale row + mirror-padded signal (< 0.8 ms/frame!)
    // =========================================================================
    const srcBuf = trigState.screenBuf;
    const srcLen = srcBuf.length;
    const hdCols = 220;
    const hdRows = 64; // Scales s = 1 .. 50 sampled across 64 vertical rows

    // Resample triggered waveform to hdCols and remove residual mean
    const sig = new Float32Array(hdCols);
    let mean = 0.0;
    for (let x = 0; x < hdCols; x++) {
      const idx = (x / (hdCols - 1)) * (srcLen - 1);
      const k = Math.floor(idx);
      const a = idx - k;
      const v = (k + 1 < srcLen) ? (srcBuf[k] * (1 - a) + srcBuf[k + 1] * a) : srcBuf[k];
      sig[x] = v;
      mean += v;
    }
    mean /= hdCols;
    let maxAbsSig = 1.0;
    for (let x = 0; x < hdCols; x++) {
      sig[x] -= mean;
      if (Math.abs(sig[x]) > maxAbsSig) maxAbsSig = Math.abs(sig[x]);
    }
    const normFactor = (maxAbsSig > 5.0) ? (3.2 / maxAbsSig) : 0.01;
    for (let x = 0; x < hdCols; x++) {
      sig[x] *= normFactor;
    }

    // Use true continuous extended buffer [-maxPad .. hdCols + maxPad] from renderScope
    // so there is ZERO Cone-of-Influence mirror distortion at 0.0 ms or 25.6 ms!
    const maxPad = 96;
    const sigPad = new Float32Array(hdCols + 2 * maxPad);
    if (trigState.cwtExtBuf && trigState.cwtExtBuf.length === hdCols + 2 * maxPad) {
      for (let i = 0; i < hdCols + 2 * maxPad; i++) {
        sigPad[i] = (trigState.cwtExtBuf[i] - mean) * normFactor;
      }
    } else {
      for (let i = 0; i < hdCols + 2 * maxPad; i++) {
        let xi = Math.max(0, Math.min(hdCols - 1, i - maxPad));
        sigPad[i] = sig[xi];
      }
    }

    // Estimate fundamental period in hdCols so scale s=11 resonates with the fundamental
    let zCross = 0;
    for (let x = 1; x < hdCols; x++) {
      if ((sig[x - 1] < 0 && sig[x] >= 0) || (sig[x - 1] > 0 && sig[x] <= 0)) zCross++;
    }
    const cyclesInWin = Math.max(2.0, zCross * 0.5);
    const periodCols = hdCols / cyclesInWin;
    const omega0 = 5.4;
    const baseStep = (periodCols * omega0) / (2.0 * Math.PI * 11.0);

    const cwtGrid = new Float32Array(hdRows * hdCols);
    const kern = new Float32Array(2 * maxPad + 1);
    let maxMag = 1e-6;

    for (let r = 0; r < hdRows; r++) {
      const scaleVal = 50.0 - (r / (hdRows - 1)) * 49.0; // 50.0 down to 1.0
      const aEff = Math.max(0.6, scaleVal * baseStep);
      const invA = 1.0 / aEff;
      const normA = (cwtNorm === 'autosignal') ? Math.sqrt(scaleVal / 10.0) : (1.0 / Math.sqrt(aEff));
      const topTaper = (scaleVal > 36.0)
        ? Math.exp(-0.5 * Math.pow((scaleVal - 36.0) / 6.5, 2))
        : 1.0;
      const rowScale = normA * topTaper;
      const halfSupport = Math.min(maxPad, Math.ceil(2.6 * aEff));
      const kLen = 2 * halfSupport + 1;

      // Precompute 1D Morlet kernel ONCE for this scale row (instead of hdCols times!)
      for (let k = 0; k < kLen; k++) {
        const u = (k - halfSupport) * invA;
        kern[k] = Math.exp(-0.5 * u * u) * Math.cos(omega0 * u) * rowScale;
      }

      const rowOff = r * hdCols;
      const padBase = maxPad - halfSupport;
      for (let x = 0; x < hdCols; x++) {
        let sumRe = 0.0;
        const pOff = padBase + x;
        for (let k = 0; k < kLen; k++) {
          sumRe += sigPad[pOff + k] * kern[k];
        }
        const mag = Math.abs(sumRe);
        cwtGrid[rowOff + x] = mag;
        if (mag > maxMag) maxMag = mag;
      }
    }

    if (!window._hdCanvas) {
      window._hdCanvas = document.createElement('canvas');
      window._hdCanvas.width = hdCols;
      window._hdCanvas.height = hdRows;
      window._hdCtx = window._hdCanvas.getContext('2d');
      window._hdImg = window._hdCtx.createImageData(hdCols, hdRows);
    }
    const octx = window._hdCtx;
    const img = window._hdImg;
    const data = img.data;

    for (let r = 0; r < hdRows; r++) {
      const rowOff = r * hdCols;
      for (let x = 0; x < hdCols; x++) {
        let norm = cwtGrid[rowOff + x] / maxMag;
        if (cwtScale === 'db') {
          const db = 20.0 * Math.log10(Math.max(1e-4, norm));
          norm = (db + 40.0) / 40.0;
        }
        const [cr, cg, cb] = colorRGB(norm, colormap);
        const idx = (rowOff + x) * 4;
        data[idx] = cr;
        data[idx + 1] = cg;
        data[idx + 2] = cb;
        data[idx + 3] = 255;
      }
    }
    octx.putImageData(img, 0, 0);

    // Enable smooth bilinear filtering on top of the 64x220 scalar grid
    ctx.imageSmoothingEnabled = true;
    ctx.imageSmoothingQuality = 'high';
    ctx.drawImage(window._hdCanvas, padL, padT, plotW, plotH);
    ctx.strokeStyle = '#2b3445';
    ctx.strokeRect(padL, padT, plotW, plotH);

    toolConfig.lastBaseStep = baseStep;
    toolConfig.lastCwtGrid = cwtGrid;
    toolConfig.lastCwtMax = maxMag;

    // Helper: convert Scale [1..50] <-> Physical Frequency (Hz)
    const dtColSec = (zoomUs * 1e-6) / (hdCols - 1);
    function scaleToHz(sVal) {
      if (sVal <= 0.2) return 0;
      return omega0 / (2.0 * Math.PI * (sVal * baseStep) * dtColSec);
    }
    function hzToScale(fHz) {
      if (fHz <= 1.0) return 50.0;
      return omega0 / (2.0 * Math.PI * fHz * baseStep * dtColSec);
    }

    // Draw Revolution Boundaries (Rev #) & Horizontal 1X / zX Frequency Guides on CWT
    if (toolConfig.showGuides) {
      ctx.save();
      ctx.beginPath();
      ctx.rect(padL, padT, plotW, plotH);
      ctx.clip();

      const tRevUs = (60.0 / Math.max(300, toolConfig.rpm)) * 1e6;
      const totalRevsInWin = zoomUs / tRevUs;
      const revStep = (totalRevsInWin > 60) ? 20 : (totalRevsInWin > 20 ? 5 : 1);
      const tTrigWinUs = trigState.trigPosPct * zoomUs;
      const scrollOff = rollState.enabled ? rollState.scrollUs : 0.0;
      const kMin = Math.floor((scrollOff - tTrigWinUs) / tRevUs) - 1;
      const kMax = Math.ceil((scrollOff + zoomUs - tTrigWinUs) / tRevUs) + 1;
      for (let k = kMin; k <= kMax; k++) {
        const revUs = tTrigWinUs + k * tRevUs - scrollOff;
        const absRevNum = k + 1;
        if (revUs >= 0 && revUs <= zoomUs) {
          if (((absRevNum - 1) % revStep + revStep) % revStep === 0 && absRevNum >= 1) {
            const xr = padL + (revUs / zoomUs) * plotW;
            ctx.strokeStyle = 'rgba(255, 255, 255, 0.35)';
            ctx.lineWidth = 1.0;
            ctx.setLineDash([4, 4]);
            ctx.beginPath();
            ctx.moveTo(xr, padT);
            ctx.lineTo(xr, padT + plotH);
            ctx.stroke();
            ctx.fillStyle = 'rgba(255, 255, 255, 0.85)';
            ctx.font = 'bold 9px Consolas, monospace';
            ctx.fillText('R' + absRevNum, xr + 3, padT + 10);
          }
        }
      }

      // Horizontal Guide Line 1: 1X Spindle Rotation Frequency
      const f1x = toolConfig.rpm / 60.0;
      const s1x = hzToScale(f1x);
      if (s1x >= 1.0 && s1x <= 49.5) {
        const y1x = padT + (1.0 - s1x / 50.0) * plotH;
        ctx.strokeStyle = 'rgba(255, 211, 61, 0.85)';
        ctx.lineWidth = 1.4;
        ctx.setLineDash([6, 4]);
        ctx.beginPath();
        ctx.moveTo(padL, y1x);
        ctx.lineTo(padL + plotW, y1x);
        ctx.stroke();
        ctx.fillStyle = '#ffd33d';
        ctx.font = 'bold 10px Consolas, monospace';
        ctx.fillText('1X Husillo (' + f1x.toFixed(0) + ' Hz, S' + s1x.toFixed(0) + ')', padL + 8, Math.max(padT + 12, y1x - 4));
      }

      // Horizontal Guide Line 2: zX Tooth Passing Frequency (Paso de Gavilán)
      const fTpf = f1x * toolConfig.flutes;
      const sTpf = hzToScale(fTpf);
      if (sTpf >= 1.0 && sTpf <= 49.5) {
        const yTpf = padT + (1.0 - sTpf / 50.0) * plotH;
        ctx.strokeStyle = 'rgba(0, 255, 200, 0.85)';
        ctx.lineWidth = 1.4;
        ctx.setLineDash([3, 3]);
        ctx.beginPath();
        ctx.moveTo(padL, yTpf);
        ctx.lineTo(padL + plotW, yTpf);
        ctx.stroke();
        ctx.fillStyle = '#00ffc8';
        ctx.font = 'bold 10px Consolas, monospace';
        ctx.fillText(toolConfig.flutes + 'X Gavil\u00e1n (' + fTpf.toFixed(0) + ' Hz, S' + sTpf.toFixed(0) + ')', padL + 8, Math.min(padT + plotH - 4, yTpf + 12));
      }

      ctx.restore();
    }

    // Draw Synchronized Crosshair on CWT
    if (toolConfig.cursorFracX >= 0 && toolConfig.cursorFracX <= 1) {
      const xc = padL + toolConfig.cursorFracX * plotW;
      ctx.save();
      ctx.strokeStyle = 'rgba(255, 255, 255, 0.9)';
      ctx.lineWidth = 1.2;
      ctx.setLineDash([3, 3]);
      ctx.beginPath();
      ctx.moveTo(xc, padT);
      ctx.lineTo(xc, padT + plotH);
      ctx.stroke();
      if (toolConfig.cursorCanvasId === 'cwtCanvas' && toolConfig.cursorFracY >= 0 && toolConfig.cursorFracY <= 1) {
        const yc = padT + toolConfig.cursorFracY * plotH;
        ctx.beginPath();
        ctx.moveTo(padL, yc);
        ctx.lineTo(padL + plotW, yc);
        ctx.stroke();
      }
      ctx.restore();
    }

    // Y-axis Dual Scale + Hz ticks: 0, 10, 20, 30, 40, 50
    ctx.fillStyle = '#e6edf3';
    ctx.font = '10px Consolas, monospace';
    [0, 10, 20, 30, 40, 50].forEach(sTick => {
      const y = padT + (1.0 - sTick / 50.0) * plotH;
      ctx.beginPath();
      ctx.moveTo(padL - 5, y);
      ctx.lineTo(padL, y);
      ctx.strokeStyle = '#8b949e';
      ctx.stroke();
      if (sTick === 0) {
        ctx.fillText('S0', 6, y + 4);
      } else {
        const fTick = scaleToHz(sTick);
        const fStr = (fTick >= 1000) ? ((fTick / 1000).toFixed(1) + 'k') : (Math.round(fTick) + 'Hz');
        ctx.fillText('S' + sTick + ' ' + fStr, 2, y + 4);
      }
    });

    // X-axis ticks (scrolling physical ms in Seismic Roll Mode)
    const baseMsCwt = rollState.enabled ? (rollState.scrollUs / 1000.0) : 0.0;
    for (let i = 0; i <= 5; i++) {
      const frac = i / 5.0;
      const x = padL + frac * plotW;
      const tMs = baseMsCwt + (frac * zoomUs) / 1000.0;
      ctx.beginPath();
      ctx.moveTo(x, padT + plotH);
      ctx.lineTo(x, padT + plotH + 4);
      ctx.strokeStyle = '#8b949e';
      ctx.stroke();
      const lbl = (tMs >= 1000.0)
        ? ((tMs / 1000.0).toFixed(2) + 's')
        : (tMs.toFixed(1) + 'ms');
      ctx.fillText(frac.toFixed(1) + ' (' + lbl + ')', x - 28, H - 6);
    }

    // Draw Right-Hand Magnitude Colorbar (scaled to ~8.0 or mV)
    const displayMax = (maxAbsSig > 5.0) ? 8.0 : 1.0;
    drawColorbar(ctx, padL + plotW + 16, padT, 16, plotH, displayMax, 'Magnitude');

  } else {
    // =========================================================================
    // MODE B: STM32 CM7 24-Scale Hardware Analytic Morlet with 240-Row Cubic Upsampling
    // =========================================================================
    const upRows = 240;
    const offscreen = document.createElement('canvas');
    offscreen.width = numTime;
    offscreen.height = upRows;
    const octx = offscreen.getContext('2d');
    const img = octx.createImageData(numTime, upRows);
    const data = img.data;

    // Pre-scale the 24 hardware rows (shifted by tShift in Seismic Roll Mode)
    const tsUsHw = 1e6 / (st.sampling_rate_hz || 10000);
    const tShift = rollState.enabled ? (Math.floor(rollState.scrollUs / tsUsHw) % numTime) : 0;
    const scaledRows = [];
    for (let s = 0; s < numScales; s++) {
      const rArr = new Float32Array(numTime);
      const w = scaleWeights[s];
      const srcRow = scal[s];
      for (let t = 0; t < numTime; t++) {
        const tSrc = (t + tShift) % numTime;
        rArr[t] = srcRow[tSrc] * magScale * w;
      }
      scaledRows.push(rArr);
    }

    for (let yr = 0; yr < upRows; yr++) {
      // yr = 0 is top (s = numScales - 1), yr = upRows - 1 is bottom (s = 0)
      const sFloat = (1.0 - yr / (upRows - 1)) * (numScales - 1);
      const s1 = Math.floor(sFloat);
      const frac = sFloat - s1;
      const s0 = Math.max(0, s1 - 1);
      const s2 = Math.min(numScales - 1, s1 + 1);
      const s3 = Math.min(numScales - 1, s1 + 2);
      const r0 = scaledRows[s0], r1 = scaledRows[s1], r2 = scaledRows[s2], r3 = scaledRows[s3];

      for (let t = 0; t < numTime; t++) {
        const val = cubicInterpRow(r0, r1, r2, r3, frac, t);
        let norm = val / maxWeighted;
        if (cwtScale === 'db') {
          const db = 20.0 * Math.log10(Math.max(1e-4, norm));
          norm = (db + 40.0) / 40.0;
        }
        const [cr, cg, cb] = colorRGB(norm, colormap);
        const idx = (yr * numTime + t) * 4;
        data[idx] = cr;
        data[idx + 1] = cg;
        data[idx + 2] = cb;
        data[idx + 3] = 255;
      }
    }
    octx.putImageData(img, 0, 0);

    ctx.imageSmoothingEnabled = true;
    ctx.imageSmoothingQuality = 'high';
    ctx.drawImage(offscreen, padL, padT, plotW, plotH);

    // Frequency labels on Y axis (axis xy: high freq at top, low freq at bottom)
    ctx.fillStyle = '#e6edf3';
    ctx.font = '11px Consolas, monospace';
    for (let s = 0; s < numScales; s += 3) {
      const yRow = (numScales - 1) - s;
      const y = padT + ((yRow + 0.5) / numScales) * plotH;
      const fHz = freqs[s] || 0;
      const lbl = (fHz < 1000.0) ? (fHz.toFixed(0) + 'Hz') : ((fHz / 1000.0).toFixed(1) + 'k');
      ctx.fillText(lbl, 4, y + 4);
    }

    if (showRidgeOverlay) {
      ctx.save();
      ctx.strokeStyle = 'rgba(255, 255, 255, 0.9)';
      ctx.lineWidth = 1.6;
      ctx.beginPath();
      for (let t = 0; t < numTime; t++) {
        const tSrc = (t + tShift) % numTime;
        const s = domScaleByTime[tSrc];
        const yRow = (numScales - 1) - s;
        const x = padL + (t / (numTime - 1)) * plotW;
        const y = padT + ((yRow + 0.5) / numScales) * plotH;
        if (t === 0) ctx.moveTo(x, y);
        else ctx.lineTo(x, y);
      }
      ctx.stroke();
      ctx.restore();
    }

    // Time axis labels (51.2 ms full window)
    const winMs = (numTime * 1000.0) / (st.sampling_rate_hz || 10000);
    const baseMsHw = rollState.enabled ? (rollState.scrollUs / 1000.0) : 0.0;
    for (let i = 0; i <= 8; i++) {
      const x = padL + (i / 8) * plotW;
      const tMs = baseMsHw + (i / 8) * winMs;
      ctx.fillText(tMs.toFixed(1) + ' ms', x - 18, H - 6);
    }

    drawColorbar(ctx, padL + plotW + 16, padT, 16, plotH, maxWeighted, 'Magnitude (mV)');
  }

  // Render Panel 3: AUTOSIGNAL Dominant Frequency & find_high_amplitude_times()
  renderAutosignalAnalysis(st, domScaleByTime, scaleWeights);
}

function renderAutosignalAnalysis(st, domScaleByTime, scaleWeights) {
  const canvas = document.getElementById('autosignalCanvas');
  if (!canvas) return;
  const ctx = canvas.getContext('2d');
  const W = canvas.width;
  const H = canvas.height;
  ctx.clearRect(0, 0, W, H);

  const scal = st.scalogram || [];
  const freqs = st.f_center_hz || [];
  if (!scal.length || !scal[0].length) return;

  const numScales = scal.length;
  const numTime = scal[0].length;
  const magScale = st.magnitude_scale || (1.0 / 65535.0);
  const tsUs = 1e6 / (st.sampling_rate_hz || 10000);
  const tShift = rollState.enabled ? (Math.floor(rollState.scrollUs / tsUs) % numTime) : 0;

  const padL = 68, padR = 24;
  const plotW = W - padL - padR;

  // Subplot 1 (Top): Instantaneous Dominant Frequency vs Time
  const topT = 16, topH = 105;
  // Subplot 2 (Bottom): Band Amplitude Profile & Slip Time Detector
  const botT = 148, botH = 114;

  // ---- 1. Top Subplot: Instantaneous Dominant Frequency (Log Y axis) ----
  const fMinAll = Math.max(10.0, freqs[0] || 100.0);
  const fMaxAll = Math.max(1000.0, freqs[numScales - 1] || 100000.0);
  const logMin = Math.log10(fMinAll);
  const logMax = Math.log10(fMaxAll);

  ctx.strokeStyle = '#1b2434';
  ctx.lineWidth = 1;
  ctx.fillStyle = '#8b949e';
  ctx.font = '10px Consolas, monospace';

  [100, 1000, 10000, 100000].forEach(fTick => {
    const lf = Math.log10(fTick);
    if (lf >= logMin - 0.05 && lf <= logMax + 0.05) {
      const y = topT + (1.0 - (lf - logMin) / (logMax - logMin)) * topH;
      ctx.beginPath();
      ctx.moveTo(padL, y);
      ctx.lineTo(padL + plotW, y);
      ctx.stroke();
      const lbl = fTick < 1000 ? (fTick + 'Hz') : ((fTick / 1000) + 'kHz');
      ctx.fillText(lbl, 6, y + 3);
    }
  });

  ctx.fillStyle = '#ff7b72';
  ctx.font = 'bold 11px sans-serif';
  ctx.fillText('Instantaneous Dominant Frequency (Hz)', padL + 6, topT + 12);

  ctx.strokeStyle = '#ff7b72';
  ctx.lineWidth = 1.8;
  ctx.beginPath();
  for (let t = 0; t < numTime; t++) {
    const tSrc = (t + tShift) % numTime;
    const s = domScaleByTime[tSrc];
    const fHz = Math.max(fMinAll, freqs[s] || fMinAll);
    const lf = Math.log10(fHz);
    const x = padL + (t / (numTime - 1)) * plotW;
    const y = topT + (1.0 - (lf - logMin) / (logMax - logMin)) * topH;
    if (t === 0) ctx.moveTo(x, y);
    else ctx.lineTo(x, y);
  }
  ctx.stroke();

  // ---- 2. Bottom Subplot: find_high_amplitude_times() in [freqMin, freqMax] ----
  const fLow = autoSigState.freqMin;
  const fHigh = autoSigState.freqMax;
  const bandIndices = [];
  for (let s = 0; s < numScales; s++) {
    if (freqs[s] >= fLow && freqs[s] <= fHigh) {
      bandIndices.push(s);
    }
  }
  // Fallback to nearest scale if user selected a very narrow range between two scales
  if (bandIndices.length === 0) {
    let bestS = 0, bestDist = 1e12;
    const fMid = 0.5 * (fLow + fHigh);
    for (let s = 0; s < numScales; s++) {
      const d = Math.abs(freqs[s] - fMid);
      if (d < bestDist) { bestDist = d; bestS = s; }
    }
    bandIndices.push(bestS);
  }

  const profile = new Float32Array(numTime);
  let maxProfile = 1e-6;
  for (let t = 0; t < numTime; t++) {
    const tSrc = (t + tShift) % numTime;
    if (autoSigState.useMax) {
      let m = 0.0;
      for (let k = 0; k < bandIndices.length; k++) {
        const s = bandIndices[k];
        const v = scal[s][tSrc] * magScale * scaleWeights[s];
        if (v > m) m = v;
      }
      profile[t] = m;
    } else {
      let sum = 0.0;
      for (let k = 0; k < bandIndices.length; k++) {
        const s = bandIndices[k];
        sum += scal[s][tSrc] * magScale * scaleWeights[s];
      }
      profile[t] = sum / bandIndices.length;
    }
    if (profile[t] > maxProfile) maxProfile = profile[t];
  }

  const threshold = autoSigState.thresholdRatio * maxProfile;
  const highIndices = [];
  for (let t = 0; t < numTime; t++) {
    if (profile[t] > threshold) {
      highIndices.push(t);
    }
  }

  let slipTimeUs = 0.0;
  let tStartUs = 0.0;
  let tEndUs = 0.0;
  if (highIndices.length > 0) {
    tStartUs = highIndices[0] * tsUs;
    tEndUs = highIndices[highIndices.length - 1] * tsUs;
    slipTimeUs = tEndUs - tStartUs;
  }

  document.getElementById('slipTimeBadge').textContent =
    'Slip_time (\u0394t): ' + slipTimeUs.toFixed(1) + ' \u00b5s (' + (slipTimeUs / 1000.0).toFixed(3) + ' ms)';
  document.getElementById('slipRangeBadge').textContent =
    'Range: [' + tStartUs.toFixed(0) + ', ' + tEndUs.toFixed(0) + '] \u00b5s | Thr: ' +
    threshold.toFixed(1) + ' mV | Pts: ' + highIndices.length + '/' + numTime;

  // Grid for Bottom Subplot
  ctx.strokeStyle = '#1b2434';
  ctx.fillStyle = '#8b949e';
  ctx.font = '10px Consolas, monospace';
  for (let i = 0; i <= 3; i++) {
    const y = botT + (i / 3) * botH;
    const v = maxProfile * (1.0 - i / 3);
    ctx.beginPath();
    ctx.moveTo(padL, y);
    ctx.lineTo(padL + plotW, y);
    ctx.stroke();
    ctx.fillText(v.toFixed(0) + 'mV', 6, y + 3);
  }

  // Highlight Slip_time [tStartUs, tEndUs] shaded span
  if (highIndices.length >= 2) {
    const x0 = padL + (highIndices[0] / (numTime - 1)) * plotW;
    const x1 = padL + (highIndices[highIndices.length - 1] / (numTime - 1)) * plotW;
    ctx.fillStyle = 'rgba(248, 81, 73, 0.12)';
    ctx.fillRect(x0, botT, x1 - x0, botH);
  }

  // Draw Amplitude Profile curve (blue, matching MATLAB `plot(time, amplitude_profile, 'b-')`)
  ctx.strokeStyle = '#58a6ff';
  ctx.lineWidth = 2.0;
  ctx.beginPath();
  for (let t = 0; t < numTime; t++) {
    const x = padL + (t / (numTime - 1)) * plotW;
    const y = botT + (1.0 - profile[t] / maxProfile) * botH;
    if (t === 0) ctx.moveTo(x, y);
    else ctx.lineTo(x, y);
  }
  ctx.stroke();

  // Draw Threshold line (red dashed, matching MATLAB `plot(..., 'r--')`)
  const yThr = botT + (1.0 - autoSigState.thresholdRatio) * botH;
  ctx.save();
  ctx.strokeStyle = '#f85149';
  ctx.lineWidth = 1.5;
  ctx.setLineDash([5, 4]);
  ctx.beginPath();
  ctx.moveTo(padL, yThr);
  ctx.lineTo(padL + plotW, yThr);
  ctx.stroke();
  ctx.restore();

  // Draw High-Amplitude Region markers (red dots)
  ctx.fillStyle = '#f85149';
  const stepMark = Math.max(1, Math.floor(highIndices.length / 64));
  for (let k = 0; k < highIndices.length; k += stepMark) {
    const t = highIndices[k];
    const x = padL + (t / (numTime - 1)) * plotW;
    const y = botT + (1.0 - profile[t] / maxProfile) * botH;
    ctx.beginPath();
    ctx.arc(x, y, 2.5, 0, 2 * Math.PI);
    ctx.fill();
  }

  ctx.fillStyle = '#58a6ff';
  ctx.font = 'bold 11px sans-serif';
  ctx.fillText(
    'Band Amplitude Profile [' + fLow.toFixed(0) + ' Hz .. ' + fHigh.toFixed(0) + ' Hz] & Threshold (' +
    Math.round(autoSigState.thresholdRatio * 100) + '%)',
    padL + 6, botT + 12
  );

  // Draw Synchronized Crosshair on AUTOSIGNAL panel
  if (toolConfig.cursorFracX >= 0 && toolConfig.cursorFracX <= 1) {
    const xc = padL + toolConfig.cursorFracX * plotW;
    ctx.save();
    ctx.strokeStyle = 'rgba(255, 255, 255, 0.85)';
    ctx.lineWidth = 1.2;
    ctx.setLineDash([3, 3]);
    ctx.beginPath();
    ctx.moveTo(xc, topT);
    ctx.lineTo(xc, botT + botH);
    ctx.stroke();
    ctx.restore();
  }

  // Time axis labels (51.2 ms window)
  ctx.fillStyle = '#8b949e';
  ctx.font = '11px Consolas, monospace';
  const winMs = (numTime * 1000.0) / (st.sampling_rate_hz || 10000);
  const baseMsAuto = rollState.enabled ? (rollState.scrollUs / 1000.0) : 0.0;
  for (let i = 0; i <= 8; i++) {
    const x = padL + (i / 8) * plotW;
    const tMs = baseMsAuto + (i / 8) * winMs;
    ctx.fillText(tMs.toFixed(1) + ' ms', x - 18, H - 6);
  }
}

function renderAll(st) {
  const badge = document.getElementById('statusBadge');
  const stTxt = document.getElementById('statusText');
  const btnStm = document.getElementById('btnSrcStm32');
  const btnPhm = document.getElementById('btnSrcPhm');
  if (btnStm && btnPhm) {
    btnStm.classList.toggle('active', !st.phm_active);
    btnPhm.classList.toggle('active', !!st.phm_active);
  }
  if (st.phm_active) {
    badge.className = 'status-badge status-live';
    stTxt.textContent = 'PHM 2010 (c_1_001.csv) | ' + (st.phm_label || 'Fy') + ' @ ' + (st.phm_sec || 0).toFixed(2) + 's';
    const infoEl = document.getElementById('phmInfoBadge');
    if (infoEl) {
      infoEl.textContent = 'Ch: ' + (st.phm_label || 'Fy') + ' | t = ' +
        (st.phm_sec || 0).toFixed(2) + ' s / ' + (st.phm_dur_sec || 2.55).toFixed(2) +
        ' s | Raw Peak: ' + (st.phm_phys_vpp || 0).toFixed(3) + ' ' + (st.phm_unit || 'N');
    }
    const sl = document.getElementById('phmTimeSlider');
    if (sl && document.activeElement !== sl) {
      sl.value = Math.round((st.phm_sec || 0) * 100);
    }
  } else if (st.connected) {
    badge.className = 'status-badge status-live';
    stTxt.textContent = 'LIVE 10 kSPS (51.2 ms) | CWT #' + st.frame_seq;
  } else {
    badge.className = 'status-badge status-stale';
    stTxt.textContent = 'STALE / RECONNECTING';
  }

  document.getElementById('kpiDcMv').textContent = st.measured_dc_mv + ' mV';
  const errSign = st.dc_offset_error_mv >= 0 ? '+' : '';
  document.getElementById('kpiDcErr').textContent =
    'Nominal: ' + st.nominal_offset_mv + ' mV (Err: ' + errSign + st.dc_offset_error_mv + ' mV)';
  document.getElementById('kpiVpp').textContent = st.vpp_mv + ' mV';
  document.getElementById('kpiAcSwing').textContent =
    'AC Swing: [' + st.ac_min_mv.toFixed(0) + ', +' + st.ac_max_mv.toFixed(0) + '] mV';
  const fHz = st.peak_freq_hz || 0;
  const estRpm4Flute = Math.round((fHz / 4.0) * 60.0);
  document.getElementById('kpiPeakFreq').textContent =
    (fHz < 1000.0) ? (fHz.toFixed(1) + ' Hz') : ((fHz / 1000.0).toFixed(2) + ' kHz');
  document.getElementById('kpiPeakMag').textContent =
    '4-Flute ~' + estRpm4Flute + ' RPM | Peak |W|: ' + st.peak_mv.toFixed(1) + ' mV';
  document.getElementById('kpiCwtUs').textContent = st.cwt_exec_us + ' \u00b5s';
  document.getElementById('kpiFrameSeq').textContent =
    '24x512 FFT-Morlet | Seq #' + st.frame_seq;

  const isAuto = st.offset_mode === 0;
  document.getElementById('kpiOffsetMode').textContent = isAuto ? 'AUTO_MEAN' : 'FIXED_1V5';
  document.getElementById('btnModeAuto').classList.toggle('active', isAuto);
  document.getElementById('btnModeFixed').classList.toggle('active', !isAuto);

  const ok = (st.adc_overrun_count === 0 && st.dma_error_count === 0);
  const hEl = document.getElementById('kpiHealth');
  hEl.textContent = ok ? 'ZERO LOSS' : 'OVERRUN';
  hEl.style.color = ok ? 'var(--accent-green)' : 'var(--accent-red)';
  document.getElementById('kpiOvr').textContent =
    'Overruns: ' + st.adc_overrun_count + ' | DMA Err: ' + st.dma_error_count;

  renderScope(st);
  renderCwt(st);
}

let lastRenderedSeq = -1;
let rafPending = false;

function scheduleRender() {
  if (!rafPending) {
    rafPending = true;
    requestAnimationFrame(() => {
      rafPending = false;
      if (latestState) {
        renderAll(latestState);
      }
    });
  }
}

async function pollLoop() {
  try {
    const r = await fetch('/api/state', { cache: 'no-store' });
    if (r.ok) {
      const st = await r.json();
      latestState = st;
      if (st.frame_seq !== lastRenderedSeq || !st.connected) {
        lastRenderedSeq = st.frame_seq;
        if (!rollState.enabled) {
          scheduleRender();
        }
      }
    }
  } catch (e) {}
  setTimeout(pollLoop, 50);
}

// 60 FPS Continuous Seismic Roll Animation Loop
function rollAnimLoop(nowMs) {
  const dtSec = Math.min(0.08, Math.max(0.0, (nowMs - rollState.lastRafMs) / 1000.0));
  rollState.lastRafMs = nowMs;
  if (rollState.enabled && !trigState.hold && latestState) {
    rollState.scrollUs += dtSec * zoomUs * rollState.speedScreensPerSec;
    renderAll(latestState);
  }
  requestAnimationFrame(rollAnimLoop);
}

// Install Synchronized Interactive Crosshair & Diagnostic Tooltip across all 3 Canvases
function setupSynchronizedCrosshair() {
  const tip = document.getElementById('cursorTooltip');
  const configs = [
    { id: 'scopeCanvas', padL: 68, padR: 28, padT: 18, padB: 26 },
    { id: 'cwtCanvas',   padL: 58, padR: 92, padT: 16, padB: 28 },
    { id: 'autosignalCanvas', padL: 68, padR: 24, padT: 16, padB: 28 }
  ];

  configs.forEach(cfg => {
    const canvas = document.getElementById(cfg.id);
    if (!canvas) return;

    canvas.addEventListener('mousemove', (ev) => {
      const rect = canvas.getBoundingClientRect();
      const scaleX = canvas.width / rect.width;
      const scaleY = canvas.height / rect.height;
      const cx = (ev.clientX - rect.left) * scaleX;
      const cy = (ev.clientY - rect.top) * scaleY;
      const plotW = canvas.width - cfg.padL - cfg.padR;
      const plotH = canvas.height - cfg.padT - cfg.padB;

      const fracX = (cx - cfg.padL) / Math.max(1, plotW);
      const fracY = (cy - cfg.padT) / Math.max(1, plotH);
      if (fracX < 0 || fracX > 1 || fracY < 0 || fracY > 1) {
        toolConfig.cursorFracX = -1;
        if (tip) tip.style.display = 'none';
        scheduleRender();
        return;
      }

      toolConfig.cursorFracX = fracX;
      toolConfig.cursorFracY = fracY;
      toolConfig.cursorCanvasId = cfg.id;

      // Compute physical values at cursor (accounting for Seismic Roll offset)
      const scrollOff = rollState.enabled ? rollState.scrollUs : 0.0;
      const tUs = fracX * zoomUs + scrollOff;
      const tMs = tUs / 1000.0;
      const tRevUs = (60.0 / Math.max(300, toolConfig.rpm)) * 1e6;
      const tTrigWinUs = trigState.trigPosPct * zoomUs;
      const revNum = Math.floor((tUs - tTrigWinUs) / tRevUs) + 1;
      let relUs = (tUs - tTrigWinUs) % tRevUs;
      if (relUs < 0) relUs += tRevUs;
      const fluteNum = Math.min(toolConfig.flutes, Math.floor((relUs / tRevUs) * toolConfig.flutes) + 1);

      let mvVal = 0;
      if (trigState.displayBuf && trigState.displayBuf.length > 1) {
        const cIdx = Math.min(trigState.displayBuf.length - 1, Math.max(0, Math.round(fracX * (trigState.displayBuf.length - 1))));
        mvVal = trigState.displayBuf[cIdx];
      }

      const sVal = Math.max(1.0, 50.0 * (1.0 - fracY));
      const dtColSec = (zoomUs * 1e-6) / 219.0;
      const fCursorHz = 5.4 / (2.0 * Math.PI * (sVal * (toolConfig.lastBaseStep || 1.0)) * dtColSec);

      let cwtMag = 0.0;
      if (toolConfig.lastCwtGrid) {
        const rIdx = Math.min(63, Math.max(0, Math.round(fracY * 63)));
        const xIdx = Math.min(219, Math.max(0, Math.round(fracX * 219)));
        cwtMag = (toolConfig.lastCwtGrid[rIdx * 220 + xIdx] / Math.max(1e-6, toolConfig.lastCwtMax)) * 8.0;
      }

      if (tip) {
        tip.style.display = 'block';
        tip.style.left = Math.min(window.innerWidth - 290, ev.clientX + 16) + 'px';
        tip.style.top = Math.max(12, ev.clientY - 76) + 'px';
        tip.innerHTML =
          '<div style="color:#58a6ff; font-weight:700;">t = ' + tMs.toFixed(2) + ' ms &nbsp;|&nbsp; ' +
          '<span style="color:#d2a8ff;">Rev #' + revNum + ' &mdash; Gavil\u00e1n G' + fluteNum + '</span></div>' +
          '<div>Amplitud Onda: <strong style="color:#00ffc8;">' + (mvVal >= 0 ? '+' : '') + Math.round(mvVal) + ' mV</strong></div>' +
          (cfg.id === 'cwtCanvas'
            ? ('<div>Escala: <strong style="color:#ffd33d;">S' + sVal.toFixed(1) + '</strong> &rarr; Frec: <strong style="color:#ffd33d;">' +
               Math.round(fCursorHz) + ' Hz</strong> | Mag: <strong>' + cwtMag.toFixed(2) + '</strong></div>')
            : '');
      }
      if (!rollState.enabled) scheduleRender();
    });

    canvas.addEventListener('mouseleave', () => {
      toolConfig.cursorFracX = -1;
      if (tip) tip.style.display = 'none';
      if (!rollState.enabled) scheduleRender();
    });
  });
}

updateToolSummaryText();
setupSynchronizedCrosshair();
pollLoop();
requestAnimationFrame(rollAnimLoop);
</script>
</body>
</html>
"""


class CwtDashboardState:
    PHM_CHANNELS = [
        ("Fx (Fuerza X)", "N"),
        ("Fy (Fuerza Y)", "N"),
        ("Fz (Fuerza Z)", "N"),
        ("Vx (Vibracion X)", "g"),
        ("Vy (Vibracion Y)", "g"),
        ("Vz (Vibracion Z)", "g"),
        ("AE_rms (Acustica)", "V"),
    ]

    def __init__(self, host: str, port: int, poll_rate_hz: float, csv_path: str = DEFAULT_PHM_CSV) -> None:
        self.host = host
        self.port = port
        self.poll_interval = 1.0 / max(1.0, poll_rate_hz)
        self.lock = threading.Lock()
        self.pending_mode_cmd: bytes | None = None

        # PHM 2010 dataset state (127,399 rows @ 50 kHz -> decimated 5x to 10 kSPS)
        self.phm_data_10k = None
        self.phm_active = False
        self.phm_channel = 1  # Default to Fy (strongest cutting force signal)
        self.phm_playing = True
        self.phm_idx_10k = 10000  # Start at t = 1.00 s (steady-state cutting engagement)
        self.phm_win_us = 25600   # Active timebase window in us (supports up to 1,000,000 us = 1.0 s)
        self.phm_seq = 1
        self.phm_dur_sec = 2.548
        self.f_center_hz = [
            10.0 * ((4000.0 / 10.0) ** (s / 23.0)) for s in range(24)
        ]

        if np is not None and csv_path and os.path.isfile(csv_path):
            try:
                print(f"[phm2010] Loading PHM 2010 dataset from {csv_path} ...", flush=True)
                raw_50k = np.loadtxt(csv_path, delimiter=",", dtype=np.float32)
                # Decimate 5x (50 kHz -> 10 kSPS) via 5-sample boxcar anti-aliasing filter
                n5 = (raw_50k.shape[0] // 5) * 5
                self.phm_data_10k = raw_50k[:n5].reshape(-1, 5, raw_50k.shape[1]).mean(axis=1)
                self.phm_dur_sec = self.phm_data_10k.shape[0] / 10000.0
                self.phm_active = False  # Start in LIVE STM32 mode so user sees AD2 W1 -> PB1/A3 directly!
                print(
                    f"[phm2010] Loaded {raw_50k.shape[0]} samples @ 50 kHz -> "
                    f"{self.phm_data_10k.shape[0]} samples @ 10 kSPS ({self.phm_dur_sec:.2f} s)",
                    flush=True,
                )
            except Exception as exc:
                print(f"[phm2010] Warning: failed to load CSV: {exc}", flush=True)

        self.state: dict = {
            "connected": False,
            "phm_active": self.phm_active,
            "frame_seq": 0,
            "sampling_rate_hz": 10000,
            "nominal_offset_mv": 1500,
            "measured_dc_mv": 1500,
            "dc_offset_error_mv": 0,
            "vpp_mv": 0,
            "ac_min_mv": 0.0,
            "ac_max_mv": 0.0,
            "cwt_exec_us": 0,
            "adc_overrun_count": 0,
            "dma_error_count": 0,
            "offset_mode": 0,
            "m4_heartbeat": 0,
            "peak_scale": 0,
            "peak_time": 0,
            "peak_q": 1,
            "peak_mv": 0.0,
            "peak_freq_hz": 0.0,
            "f_center_hz": self.f_center_hz,
            "raw_waveform": [],
            "scalogram": [],
        }

    def configure_phm(
        self,
        source: str | None = None,
        channel: int | None = None,
        play: bool | None = None,
        sec: float | None = None,
        win_us: int | None = None,
    ) -> None:
        with self.lock:
            if source is not None:
                self.phm_active = (source == "phm") and (self.phm_data_10k is not None)
            if channel is not None and 0 <= channel < 7:
                self.phm_channel = channel
            if play is not None:
                self.phm_playing = play
            if win_us is not None and win_us >= 1000:
                self.phm_win_us = win_us
            if sec is not None and self.phm_data_10k is not None:
                max_idx = max(0, self.phm_data_10k.shape[0] - 513)
                self.phm_idx_10k = max(0, min(max_idx, int(sec * 10000.0)))

    def _step_phm_frame(self) -> None:
        if self.phm_data_10k is None:
            return
        t_start = time.perf_counter()
        with self.lock:
            ch = self.phm_channel
            idx = self.phm_idx_10k
            playing = self.phm_playing
            win_us = self.phm_win_us

        n_total = self.phm_data_10k.shape[0]
        # Number of 10 kSPS samples needed to span max(51.2 ms, win_us)
        span_samples = max(512, int(round((win_us / 1e6) * 10000.0)))
        if idx + span_samples > n_total:
            idx = max(0, min(2000, n_total - span_samples - 1))

        col = self.phm_data_10k[:, ch]
        raw_span = col[idx : idx + span_samples]
        if len(raw_span) < 512:
            return

        # Resample to 512 points if macro window (250 ms, 500 ms, 1.0 s) is selected
        if len(raw_span) == 512:
            win = raw_span
            eff_fs = 10000
        else:
            x_old = np.linspace(0.0, 1.0, len(raw_span), endpoint=False)
            x_new = np.linspace(0.0, 1.0, 512, endpoint=False)
            win = np.interp(x_new, x_old, raw_span).astype(np.float32)
            eff_fs = max(512, int(round(512.0 / (win_us / 1e6))))
        phys_min = float(win.min())
        phys_max = float(win.max())
        phys_vpp = phys_max - phys_min
        phys_mean = float(win.mean())
        ac = win - phys_mean

        # Scale channel dynamics so full-scale cutting force/vibration maps cleanly to +/- 1150 mV around 1500 mV
        ch_fullscale = max(1e-5, float(np.percentile(np.abs(col - col.mean()), 99.2)))
        mv_gain = 1150.0 / ch_fullscale
        ac_mv = np.clip(ac * mv_gain, -1450.0, 1750.0)
        abs_mv = 1500.0 + ac_mv
        raw_u16 = np.clip(np.round((abs_mv / 3300.0) * 65535.0), 0, 65535).astype(np.uint16)

        # Compute 24-scale Analytic Morlet CWT (10 Hz .. 4,000 Hz @ fs = 10 kSPS) via FFT
        n_fft = 512
        fs = 10000.0
        freqs_fft = np.fft.rfftfreq(n_fft, d=1.0 / fs)
        sig_fft = np.fft.rfft(ac_mv)
        mag_matrix = np.zeros((24, n_fft), dtype=np.float32)

        for s_idx, fc in enumerate(self.f_center_hz):
            sigma_f = max(fc / 6.0, 0.75 * (fs / n_fft))
            delta = (freqs_fft - fc) / sigma_f
            psi = 2.0 * np.exp(-0.5 * (delta ** 2))
            psi[0] = 0.0
            # Analytic signal IFFT via full 512-point complex spectrum
            spec_full = np.zeros(n_fft, dtype=np.complex64)
            spec_full[: len(freqs_fft)] = sig_fft * psi
            w_complex = np.fft.ifft(spec_full)
            mag_matrix[s_idx, :] = np.abs(w_complex)

        max_mag = float(mag_matrix.max())
        mag_scale = (max_mag / 65535.0) if max_mag > 1e-3 else (1.0 / 65535.0)
        scal_u16 = np.clip(np.round(mag_matrix / mag_scale), 0, 65535).astype(np.uint16)

        peak_flat = int(np.argmax(scal_u16))
        peak_s, peak_t = divmod(peak_flat, n_fft)
        peak_q = int(scal_u16[peak_s, peak_t])
        cwt_us = int((time.perf_counter() - t_start) * 1e6)
        label, unit = self.PHM_CHANNELS[ch]

        with self.lock:
            if playing:
                self.phm_idx_10k = idx + 128  # Advance 12.8 ms per frame for smooth visual inspection
                if self.phm_idx_10k + 512 >= n_total:
                    self.phm_idx_10k = 1800
            self.phm_seq += 1
            self.state.update(
                {
                    "connected": True,
                    "phm_active": True,
                    "phm_channel": ch,
                    "phm_label": label,
                    "phm_unit": unit,
                    "phm_sec": idx / 10000.0,
                    "phm_dur_sec": self.phm_dur_sec,
                    "phm_phys_vpp": phys_vpp,
                    "frame_seq": self.phm_seq,
                    "sampling_rate_hz": eff_fs,
                    "nominal_offset_mv": 1500,
                    "measured_dc_mv": 1500,
                    "vpp_mv": int(round(float(ac_mv.max() - ac_mv.min()))),
                    "min_raw": int(raw_u16.min()),
                    "max_raw": int(raw_u16.max()),
                    "average_raw": int(raw_u16.mean()),
                    "adc_overrun_count": 0,
                    "dma_error_count": 0,
                    "cwt_exec_us": cwt_us,
                    "magnitude_scale": mag_scale,
                    "dc_offset_error_mv": 0,
                    "offset_mode": 0,
                    "m4_heartbeat": self.phm_seq,
                    "ac_min_mv": float(ac_mv.min()),
                    "ac_max_mv": float(ac_mv.max()),
                    "peak_scale": peak_s,
                    "peak_time": peak_t,
                    "peak_q": max(1, peak_q),
                    "peak_mv": peak_q * mag_scale,
                    "peak_freq_hz": self.f_center_hz[peak_s],
                    "f_center_hz": self.f_center_hz,
                    "raw_waveform": raw_u16.tolist(),
                    "scalogram": scal_u16.tolist(),
                }
            )

    def request_mode_change(self, mode_str: str) -> None:
        with self.lock:
            if mode_str == "fixed":
                self.pending_mode_cmd = b"MODE FIXED"
            else:
                self.pending_mode_cmd = b"MODE AUTO"

    def get_snapshot(self) -> dict:
        with self.lock:
            return dict(self.state)

    def poll_forever(self) -> None:
        sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        sock.settimeout(0.8)
        while True:
            t0 = time.perf_counter()
            with self.lock:
                use_phm = self.phm_active
            if use_phm:
                try:
                    self._step_phm_frame()
                except Exception:
                    pass
                elapsed = time.perf_counter() - t0
                if elapsed < self.poll_interval:
                    time.sleep(self.poll_interval - elapsed)
                continue
            try:
                cmd_to_send = None
                with self.lock:
                    if self.pending_mode_cmd is not None:
                        cmd_to_send = self.pending_mode_cmd
                        self.pending_mode_cmd = None

                if cmd_to_send is not None:
                    sock.sendto(cmd_to_send, (self.host, self.port))
                    sock.recvfrom(1024)

                sock.sendto(b"CWT SNAP", (self.host, self.port))
                pkt0, _ = sock.recvfrom(2048)
                if len(pkt0) >= CWT_HEADER_LEN:
                    fields = struct.unpack(CWT_HEADER_FMT, pkt0[:CWT_HEADER_LEN])
                    if fields[0] == CWT_MAGIC and fields[1] == CWT_VERSION:
                        h_len = fields[2]
                        total_chunks = fields[5]
                        num_scales = fields[7]
                        num_time = fields[8]
                        payload0 = pkt0[h_len:]
                        f_bytes = num_scales * 4
                        w_bytes = num_time * 2
                        f_center_hz = list(struct.unpack(f"<{num_scales}f", payload0[:f_bytes]))
                        raw_wave = list(
                            struct.unpack(f"<{num_time}H", payload0[f_bytes : f_bytes + w_bytes])
                        )

                        scalogram: list[list[int]] = [[0] * num_time for _ in range(num_scales)]
                        cursor = 1
                        while cursor < total_chunks:
                            cnt = min(4, total_chunks - cursor)
                            sock.sendto(f"CWT B {cursor} {cnt}".encode("ascii"), (self.host, self.port))
                            for _ in range(cnt):
                                pk, _ = sock.recvfrom(2048)
                                if len(pk) >= CWT_HEADER_LEN:
                                    cf = struct.unpack(CWT_HEADER_FMT, pk[:CWT_HEADER_LEN])
                                    s_off = cf[9]
                                    if s_off < num_scales:
                                        row_b = pk[cf[2] : cf[2] + num_time * 2]
                                        if len(row_b) == num_time * 2:
                                            scalogram[s_off] = list(struct.unpack(f"<{num_time}H", row_b))
                            cursor += cnt

                        peak_q = 0
                        peak_s = 0
                        peak_t = 0
                        for s_idx, r_vals in enumerate(scalogram):
                            rmax = max(r_vals)
                            if rmax > peak_q:
                                peak_q = rmax
                                peak_s = s_idx
                                peak_t = r_vals.index(rmax)

                        dc_mv = fields[12]
                        mag_scale = fields[20]
                        ac_vals = [((r * 3300.0) / 65535.0) - dc_mv for r in raw_wave]

                        with self.lock:
                            self.state.update(
                                {
                                    "connected": True,
                                    "phm_active": False,
                                    "frame_seq": fields[3],
                                    "sampling_rate_hz": fields[6],
                                    "nominal_offset_mv": fields[11],
                                    "measured_dc_mv": dc_mv,
                                    "vpp_mv": fields[13],
                                    "min_raw": fields[14],
                                    "max_raw": fields[15],
                                    "average_raw": fields[16],
                                    "adc_overrun_count": fields[17],
                                    "dma_error_count": fields[18],
                                    "cwt_exec_us": fields[19],
                                    "magnitude_scale": mag_scale,
                                    "dc_offset_error_mv": fields[21],
                                    "offset_mode": fields[22],
                                    "m4_heartbeat": fields[23],
                                    "ac_min_mv": min(ac_vals) if ac_vals else 0.0,
                                    "ac_max_mv": max(ac_vals) if ac_vals else 0.0,
                                    "peak_scale": peak_s,
                                    "peak_time": peak_t,
                                    "peak_q": max(1, peak_q),
                                    "peak_mv": peak_q * mag_scale,
                                    "peak_freq_hz": f_center_hz[peak_s] if peak_s < len(f_center_hz) else 0.0,
                                    "f_center_hz": f_center_hz,
                                    "raw_waveform": raw_wave,
                                    "scalogram": scalogram,
                                }
                            )
            except Exception:
                with self.lock:
                    self.state["connected"] = False

            elapsed = time.perf_counter() - t0
            if elapsed < self.poll_interval:
                time.sleep(self.poll_interval - elapsed)


def make_handler(shared: CwtDashboardState):
    class Handler(http.server.BaseHTTPRequestHandler):
        def do_GET(self) -> None:
            if self.path.startswith("/api/state"):
                payload = json.dumps(shared.get_snapshot()).encode("utf-8")
                self.send_response(200)
                self.send_header("Content-Type", "application/json")
                self.send_header("Cache-Control", "no-store")
                self.end_headers()
                self.wfile.write(payload)
            else:
                body = HTML_PAGE.encode("utf-8")
                self.send_response(200)
                self.send_header("Content-Type", "text/html; charset=utf-8")
                self.send_header("Cache-Control", "no-store, no-cache, must-revalidate")
                self.end_headers()
                self.wfile.write(body)

        def do_POST(self) -> None:
            if self.path.startswith("/api/mode"):
                parsed = urllib.parse.urlparse(self.path)
                qs = urllib.parse.parse_qs(parsed.query)
                mode = qs.get("mode", ["auto"])[0]
                shared.request_mode_change(mode)
                self.send_response(200)
                self.send_header("Content-Type", "application/json")
                self.end_headers()
                self.wfile.write(b'{"ok":true}')
            elif self.path.startswith("/api/phm"):
                parsed = urllib.parse.urlparse(self.path)
                qs = urllib.parse.parse_qs(parsed.query)
                source = qs.get("source", [None])[0]
                ch_s = qs.get("channel", [None])[0]
                play_s = qs.get("play", [None])[0]
                sec_s = qs.get("sec", [None])[0]
                win_s = qs.get("win_us", [None])[0]
                shared.configure_phm(
                    source=source,
                    channel=int(ch_s) if ch_s is not None else None,
                    play=(play_s == "1") if play_s is not None else None,
                    sec=float(sec_s) if sec_s is not None else None,
                    win_us=int(win_s) if win_s is not None else None,
                )
                self.send_response(200)
                self.send_header("Content-Type", "application/json")
                self.end_headers()
                self.wfile.write(b'{"ok":true}')
            elif self.path.startswith("/api/ad2_inject"):
                parsed = urllib.parse.urlparse(self.path)
                qs = urllib.parse.parse_qs(parsed.query)
                ch = int(qs.get("channel", ["1"])[0])
                sec = float(qs.get("sec", ["1.0"])[0])
                import subprocess
                script_path = os.path.join(os.path.dirname(os.path.abspath(__file__)), "ad2_wavegen_phm.py")
                subprocess.run(
                    [sys.executable, script_path, "--channel", str(ch), "--sec", f"{sec:.2f}"],
                    check=False,
                )
                shared.configure_phm(source="stm32", channel=ch, sec=sec)
                label, _ = shared.PHM_CHANNELS[max(0, min(6, ch))]
                resp = json.dumps({"ok": True, "label": label, "sec": sec}).encode("utf-8")
                self.send_response(200)
                self.send_header("Content-Type", "application/json")
                self.end_headers()
                self.wfile.write(resp)
            else:
                self.send_response(404)
                self.end_headers()

        def log_message(self, format: str, *args) -> None:
            return

    return Handler


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Real-time web dashboard for STM32H755ZI Dual-Core CWT & 1.5V Biased Oscilloscope."
    )
    parser.add_argument("--host", default="192.168.1.150", help="Board IP address (default: 192.168.1.150)")
    parser.add_argument("--port", type=int, default=5001, help="Board UDP port (default: 5001)")
    parser.add_argument("--bind", default="127.0.0.1:8080", help="Local HTTP bind address (default: 127.0.0.1:8080)")
    parser.add_argument("--rate", type=float, default=10.0, help="UDP CWT frame poll rate in Hz (default: 10)")
    args = parser.parse_args()

    bind_host, bind_port_s = args.bind.split(":")
    bind_port = int(bind_port_s)

    shared = CwtDashboardState(args.host, args.port, args.rate)
    poller = threading.Thread(target=shared.poll_forever, daemon=True)
    poller.start()

    server = http.server.ThreadingHTTPServer((bind_host, bind_port), make_handler(shared))
    print(f"CWT Web Dashboard running at http://{bind_host}:{bind_port}")
    print(f"Polling STM32H755ZI CWT UDP server at {args.host}:{args.port} ({args.rate:.1f} Hz)...")
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print("\nShutting down CWT dashboard.")
    finally:
        server.server_close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
