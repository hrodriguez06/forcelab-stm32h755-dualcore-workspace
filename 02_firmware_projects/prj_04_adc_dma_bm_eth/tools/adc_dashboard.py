#!/usr/bin/env python
"""
Interactive real-time web dashboard for STM32H755ZI ADC+DMA validation.

Features:
  - 100% Python Standard Library (no pip dependencies, works offline).
  - High-Speed Oscilloscope Mode: Replicates waveforms up to 10 kHz (500 kSPS).
  - Sub-Sample Jitter-Free Software Triggering: Eliminates horizontal dancing ("cero bailarina").
  - Adjustable Timebase / Time Window: 10 us/div (100 us), 20 us/div (200 us), 50 us/div, 100 us/div or slider.
  - Interactive Trigger Controls: AUTO / NORMAL modes, Rising / Falling slope, Auto 50% or manual level slider.
  - Visual Trigger Markers: Horizontal trigger level line (T ▶) and vertical trigger point pointer (▼ T).
  - Real-Time Digital Measurements: Vpp, Vmax, Vmin, Frequency (kHz), Duty Cycle (%), Timebase.
  - Background UDP poller querying board via binary WAVE protocol (1076 bytes).
  - Rolling 60-second voltage trend, DMA health, overrun counter, and dual-core heartbeat.

This script lives inside the project, NOT in <workspace>/tools/, and it has no
shim. It is not a workspace tool: the struct.unpack() format below has to stay
byte-identical with WaveformHeader_t in CM7/App/Src/adc_server.c, field for
field. That is a contract between two files, and it is only reviewable while
both sit in the same tree. See "Where a script belongs" in the workspace README.

Usage:
    py tools/adc_dashboard.py [--host 192.168.1.150] [--port 5001] [--bind 127.0.0.1:8080] [--rate 15]
"""

from __future__ import annotations

import argparse
from collections import deque
import http.server
import json
import socket
import struct
import sys
import threading
import time

WAVE_MAGIC = 0x57415645  # 'WAVE'
WAVE_VERSION = 1

# Byte-identical with WaveformHeader_t in CM7/App/Src/adc_server.c. The two must
# change together; that is why this file lives in the project and not in
# <workspace>/tools/ (see "Where a script belongs" in the workspace README).
#
# Contract: `version` covers the layout of the fields below. Appending new
# fields only grows `header_len` and leaves `version` alone, so a parser that
# locates the samples at `header_len` - as this one does - keeps working against
# newer firmware. Changing or reordering an existing field bumps `version`.
WAVE_HEADER_FMT = "<IHHIHHIHHHHIIIIII"
WAVE_HEADER_LEN = 52
assert struct.calcsize(WAVE_HEADER_FMT) == WAVE_HEADER_LEN

HTML_PAGE = """<!DOCTYPE html>
<html lang="es">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>STM32H755ZI - Osciloscopio Digital DSO & Telemetría ADC</title>
<style>
  :root {
    --bg-color: #0d1117;
    --card-bg: #161b22;
    --border-color: #30363d;
    --text-primary: #e6edf3;
    --text-muted: #8b949e;
    --accent-blue: #58a6ff;
    --accent-green: #3fb950;
    --accent-red: #f85149;
    --accent-yellow: #d29922;
    --scope-trace: #00ffc8;
    --scope-bg: #060e0b;
  }
  * { box-sizing: border-box; margin: 0; padding: 0; }
  body {
    background-color: var(--bg-color);
    color: var(--text-primary);
    font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, "Helvetica Neue", Arial, sans-serif;
    padding: 20px;
  }
  .header {
    display: flex;
    justify-content: space-between;
    align-items: center;
    padding-bottom: 16px;
    border-bottom: 1px solid var(--border-color);
    margin-bottom: 20px;
  }
  .header h1 { font-size: 1.5rem; font-weight: 600; }
  .status-badge {
    display: flex;
    align-items: center;
    gap: 8px;
    padding: 6px 14px;
    border-radius: 20px;
    font-size: 0.875rem;
    font-weight: 600;
  }
  .status-live { background-color: rgba(63, 185, 80, 0.15); color: var(--accent-green); border: 1px solid var(--accent-green); }
  .status-stale { background-color: rgba(248, 81, 73, 0.15); color: var(--accent-red); border: 1px solid var(--accent-red); }
  .indicator-dot {
    width: 10px; height: 10px; border-radius: 50%;
    background-color: currentColor;
    box-shadow: 0 0 8px currentColor;
  }
  
  /* DSO Oscilloscope Screen */
  .scope-container {
    background-color: var(--scope-bg);
    border: 2px solid #238636;
    border-radius: 8px;
    padding: 16px;
    margin-bottom: 20px;
    box-shadow: 0 0 24px rgba(0, 255, 200, 0.08);
  }
  .scope-header {
    display: flex;
    justify-content: space-between;
    align-items: center;
    flex-wrap: wrap;
    gap: 12px;
    margin-bottom: 12px;
    padding-bottom: 10px;
    border-bottom: 1px solid #1c4533;
  }
  .scope-title {
    font-size: 1.05rem;
    font-weight: 700;
    color: #39d353;
    display: flex;
    align-items: center;
    gap: 8px;
    letter-spacing: 0.5px;
  }
  .scope-badge {
    background: rgba(57, 211, 83, 0.15);
    border: 1px solid #2ea043;
    color: #39d353;
    font-size: 0.75rem;
    font-weight: 600;
    padding: 3px 8px;
    border-radius: 4px;
    font-family: monospace;
  }
  .scope-readouts {
    display: flex;
    gap: 12px;
    flex-wrap: wrap;
    font-family: ui-monospace, SFMono-Regular, Consolas, monospace;
    font-size: 0.82rem;
  }
  .readout-item {
    background: #0f241c;
    border: 1px solid #1b4d37;
    border-radius: 4px;
    padding: 4px 10px;
    display: flex;
    gap: 6px;
  }
  .readout-label { color: var(--text-muted); }
  .readout-val { color: var(--scope-trace); font-weight: bold; }
  
  canvas.scope-canvas {
    width: 100%;
    height: 400px;
    display: block;
    border-radius: 4px;
    background: #060e0b;
  }

  /* Interactive DSO Controls Bar */
  .scope-controls {
    display: grid;
    grid-template-columns: repeat(auto-fit, minmax(320px, 1fr));
    gap: 16px;
    background: #0d1e17;
    border: 1px solid #1c4533;
    border-radius: 6px;
    padding: 12px 16px;
    margin-top: 14px;
  }
  .control-group {
    display: flex;
    flex-direction: column;
    gap: 8px;
  }
  .control-title {
    font-size: 0.75rem;
    font-weight: 700;
    color: #8b949e;
    text-transform: uppercase;
    letter-spacing: 0.5px;
    display: flex;
    justify-content: space-between;
  }
  .btn-row {
    display: flex;
    gap: 6px;
    flex-wrap: wrap;
    align-items: center;
  }
  .scope-btn {
    background: #162e24;
    border: 1px solid #23533e;
    color: #e6edf3;
    padding: 5px 12px;
    border-radius: 4px;
    font-size: 0.8rem;
    font-weight: 600;
    cursor: pointer;
    transition: all 0.15s ease;
  }
  .scope-btn:hover {
    background: #1e4233;
    border-color: #39d353;
  }
  .scope-btn.active {
    background: #238636;
    border-color: #39d353;
    color: #ffffff;
    box-shadow: 0 0 8px rgba(57, 211, 83, 0.4);
  }
  .slider-box {
    display: flex;
    align-items: center;
    gap: 10px;
    font-family: monospace;
    font-size: 0.8rem;
  }
  .slider-box input[type="range"] {
    flex: 1;
    accent-color: #39d353;
    cursor: pointer;
  }
  .checkbox-label {
    display: flex;
    align-items: center;
    gap: 6px;
    font-size: 0.8rem;
    cursor: pointer;
    color: #c9d1d9;
    user-select: none;
  }
  .checkbox-label input { accent-color: #39d353; }

  /* Metrics Cards */
  .grid {
    display: grid;
    grid-template-columns: repeat(auto-fit, minmax(200px, 1fr));
    gap: 16px;
    margin-bottom: 20px;
  }
  .card {
    background-color: var(--card-bg);
    border: 1px solid var(--border-color);
    border-radius: 8px;
    padding: 16px;
  }
  .card-title { font-size: 0.8rem; text-transform: uppercase; color: var(--text-muted); margin-bottom: 6px; letter-spacing: 0.5px; }
  .card-value { font-size: 1.8rem; font-weight: 700; color: var(--text-primary); }
  .card-sub { font-size: 0.8rem; color: var(--text-muted); margin-top: 4px; }

  /* 60s Historical Chart */
  .chart-container {
    background-color: var(--card-bg);
    border: 1px solid var(--border-color);
    border-radius: 8px;
    padding: 16px;
    margin-bottom: 20px;
  }
  .chart-header {
    display: flex;
    justify-content: space-between;
    align-items: center;
    margin-bottom: 12px;
  }
  canvas.trend-canvas {
    width: 100%;
    height: 180px;
    display: block;
  }
  .footer-note {
    font-size: 0.8rem;
    color: var(--text-muted);
    text-align: center;
    margin-top: 20px;
  }
</style>
</head>
<body>

  <div class="header">
    <div>
      <h1>STM32H755ZI Osciloscopio Digital & Telemetría</h1>
      <div style="font-size: 0.85rem; color: var(--text-muted); margin-top: 4px;">
        Cortex-M4 (D2, 500 kSPS TIM3 TRGO) &rarr; SRAM4 (HSEM 4) &rarr; Cortex-M7 (FreeRTOS lwIP :5001 UDP)
      </div>
    </div>
    <div id="statusBadge" class="status-badge status-stale">
      <div class="indicator-dot"></div>
      <span id="statusText">Conectando...</span>
    </div>
  </div>

  <!-- Digital Storage Oscilloscope (DSO) Screen -->
  <div class="scope-container">
    <div class="scope-header">
      <div class="scope-title">
        <span>OSCILOSCOPIO DIGITAL (DSO)</span>
        <span class="scope-badge" id="trigStatusBadge">TRIG'D</span>
        <span class="scope-badge" id="scopePin">CH1: PB1 [A3]</span>
      </div>
      <div class="scope-readouts">
        <div class="readout-item"><span class="readout-label">FREQ:</span><span class="readout-val" id="scopeFreq">-- kHz</span></div>
        <div class="readout-item"><span class="readout-label">Vpp:</span><span class="readout-val" id="scopeVpp">-- V</span></div>
        <div class="readout-item"><span class="readout-label">Vmax:</span><span class="readout-val" id="scopeVmax">-- V</span></div>
        <div class="readout-item"><span class="readout-label">Vmin:</span><span class="readout-val" id="scopeVmin">-- V</span></div>
        <div class="readout-item"><span class="readout-label">DUTY:</span><span class="readout-val" id="scopeDuty">-- %</span></div>
        <div class="readout-item"><span class="readout-label">BASE-T:</span><span class="readout-val" id="scopeTb">20 us/div</span></div>
      </div>
    </div>
    
    <canvas id="scopeCanvas" class="scope-canvas"></canvas>

    <!-- DSO Interactive Controls Toolbar -->
    <div class="scope-controls">
      <!-- Timebase / Window Controls -->
      <div class="control-group">
        <div class="control-title">
          <span>Ventana de Tiempo (Escala Horizontal)</span>
          <span id="txtWindowSpan" style="color: #39d353;">200 &mu;s total</span>
        </div>
        <div class="btn-row">
          <button class="scope-btn" onclick="setTimebase(10)">10 &mu;s/div</button>
          <button class="scope-btn active" id="btnTb20" onclick="setTimebase(20)">20 &mu;s/div</button>
          <button class="scope-btn" onclick="setTimebase(50)">50 &mu;s/div</button>
          <button class="scope-btn" onclick="setTimebase(100)">100 &mu;s/div</button>
        </div>
        <div class="slider-box">
          <span style="color: var(--text-muted);">Zoom:</span>
          <input type="range" id="windowSlider" min="40" max="1000" step="10" value="200" oninput="onWindowSliderChange(this.value)">
          <span id="windowSliderVal" style="width: 65px; text-align: right;">200 &mu;s</span>
        </div>
      </div>

      <!-- Trigger Controls -->
      <div class="control-group">
        <div class="control-title">
          <span>Control de Disparo (Trigger Anti-Jitter)</span>
          <span id="txtTrigModeDesc" style="color: #e3b341;">Flanco Ascendente</span>
        </div>
        <div class="btn-row">
          <button class="scope-btn active" id="btnModeAuto" onclick="setTriggerMode('auto')">AUTO</button>
          <button class="scope-btn" id="btnModeNorm" onclick="setTriggerMode('normal')">NORMAL</button>
          <button class="scope-btn" id="btnSlope" onclick="toggleSlope()">FLANCO: &uarr; SUBIDA</button>
          <label class="checkbox-label">
            <input type="checkbox" id="chkAutoLevel" checked onchange="toggleAutoLevel(this.checked)">
            Nivel 50%
          </label>
        </div>
        <div class="slider-box">
          <span style="color: var(--text-muted);">Nivel:</span>
          <input type="range" id="trigLevelSlider" min="100" max="3200" step="20" value="1650" disabled oninput="onTrigSliderChange(this.value)">
          <span id="trigLevelVal" style="width: 65px; text-align: right;">1650 mV</span>
        </div>
      </div>
    </div>
  </div>

  <div class="grid">
    <div class="card">
      <div class="card-title">Tensión Instantánea & Promedio</div>
      <div class="card-value" id="valMv">-- mV</div>
      <div class="card-sub" id="valAvgMinMax">Prom: -- | Mín: -- | Máx: --</div>
    </div>
    <div class="card">
      <div class="card-title">Tasa de Muestreo Real</div>
      <div class="card-value" id="valRate">500,000 SPS</div>
      <div class="card-sub">Periodo Ts = 2.0 &mu;s (50 pts/ciclo @ 10 kHz)</div>
    </div>
    <div class="card">
      <div class="card-title">Pipeline DMA & Telemetría</div>
      <div class="card-value" id="valSamples">0</div>
      <div class="card-sub" id="valDmaCounters">DMA TC: 0 | Err: 0 | Overrun: 0</div>
    </div>
    <div class="card">
      <div class="card-title">Dual-Core Heartbeat</div>
      <div class="card-value" id="valM4Hb">0</div>
      <div class="card-sub" id="valFreshness">CM4 Super-Loop Activo</div>
    </div>
  </div>

  <div class="chart-container">
    <div class="chart-header">
      <div style="font-weight: 600;">Tendencia de Tensión Media (Ventana 60s)</div>
      <div style="font-size: 0.8rem; color: var(--text-muted);">Escala vertical: 0 a 3300 mV</div>
    </div>
    <canvas id="trendCanvas" class="trend-canvas"></canvas>
  </div>

  <div class="footer-note">
    STM32H755ZI Dual-Core &bull; NUCLEO-H755ZI-Q Hardware Validation Bench &bull; Anti-Jitter Sub-Sample Trigger Activo
  </div>

<script>
  // Global Oscilloscope State
  const scopeState = {
    timeWindowUs: 200,      // Total horizontal window in microseconds (default: 200 us = 20 us/div)
    trigMode: 'auto',       // 'auto' or 'normal'
    trigSlope: 'rising',    // 'rising' or 'falling'
    autoLevel: true,        // Automatically track midpoint between Vmin and Vmax
    manualLevelMv: 1650,    // Manual trigger level in mV
    effectiveTrigMv: 1650,  // Active trigger level applied
    trigPosPct: 0.15,       // Trigger point placed at 15% from left (division 1.5)
    lastValidWaveform: null,// Preserved for NORMAL mode hold
    lastValidMeta: null,
    isTriggered: false
  };

  const scopeCanvas = document.getElementById("scopeCanvas");
  const scopeCtx = scopeCanvas.getContext("2d");

  const trendCanvas = document.getElementById("trendCanvas");
  const trendCtx = trendCanvas.getContext("2d");

  function resizeAll() {
    [scopeCanvas, trendCanvas].forEach(c => {
      const rect = c.getBoundingClientRect();
      c.width = rect.width * window.devicePixelRatio;
      c.height = rect.height * window.devicePixelRatio;
      const ctx = c.getContext("2d");
      ctx.resetTransform ? ctx.resetTransform() : ctx.setTransform(1, 0, 0, 1, 0, 0);
      ctx.scale(window.devicePixelRatio, window.devicePixelRatio);
    });
  }
  window.addEventListener("resize", resizeAll);
  resizeAll();

  // Control Handlers
  function setTimebase(tb) {
    scopeState.timeWindowUs = tb * 10; // 10 horizontal divisions
    document.getElementById("windowSlider").value = scopeState.timeWindowUs;
    document.getElementById("windowSliderVal").textContent = scopeState.timeWindowUs + " \u00B5s";
    document.getElementById("txtWindowSpan").textContent = scopeState.timeWindowUs + " \u00B5s total";
    document.getElementById("scopeTb").textContent = tb + " us/div";

    document.querySelectorAll(".control-group:first-child .scope-btn").forEach(b => b.classList.remove("active"));
    const activeBtn = Array.from(document.querySelectorAll(".control-group:first-child .scope-btn")).find(b => b.textContent.startsWith(tb + ""));
    if (activeBtn) activeBtn.classList.add("active");
  }

  function onWindowSliderChange(val) {
    scopeState.timeWindowUs = Number(val);
    document.getElementById("windowSliderVal").textContent = val + " \u00B5s";
    document.getElementById("txtWindowSpan").textContent = val + " \u00B5s total";
    const tbDiv = (scopeState.timeWindowUs / 10).toFixed(1);
    document.getElementById("scopeTb").textContent = tbDiv + " us/div";
    document.querySelectorAll(".control-group:first-child .scope-btn").forEach(b => b.classList.remove("active"));
  }

  function setTriggerMode(mode) {
    scopeState.trigMode = mode;
    document.getElementById("btnModeAuto").classList.toggle("active", mode === 'auto');
    document.getElementById("btnModeNorm").classList.toggle("active", mode === 'normal');
  }

  function toggleSlope() {
    scopeState.trigSlope = (scopeState.trigSlope === 'rising') ? 'falling' : 'rising';
    const btn = document.getElementById("btnSlope");
    if (scopeState.trigSlope === 'rising') {
      btn.innerHTML = "FLANCO: &uarr; SUBIDA";
      document.getElementById("txtTrigModeDesc").textContent = "Flanco Ascendente";
    } else {
      btn.innerHTML = "FLANCO: &darr; BAJADA";
      document.getElementById("txtTrigModeDesc").textContent = "Flanco Descendente";
    }
  }

  function toggleAutoLevel(checked) {
    scopeState.autoLevel = checked;
    document.getElementById("trigLevelSlider").disabled = checked;
    if (checked) {
      document.getElementById("trigLevelVal").textContent = Math.round(scopeState.effectiveTrigMv) + " mV (Auto)";
    } else {
      document.getElementById("trigLevelVal").textContent = scopeState.manualLevelMv + " mV";
    }
  }

  function onTrigSliderChange(val) {
    scopeState.manualLevelMv = Number(val);
    document.getElementById("trigLevelVal").textContent = val + " mV";
  }

  // Sub-Sample Jitter-Free Trigger Search with Hysteresis
  function findSubSampleTrigger(samples, trigLevel, slope) {
    const n = samples.length;
    const hyst = 75; // 75 mV noise-rejection hysteresis band
    const maxSearch = Math.floor(n * 0.70); // Keep 30% tail room

    if (slope === 'rising') {
      let armed = false;
      for (let i = 1; i < maxSearch; i++) {
        if (samples[i - 1] < trigLevel - hyst) {
          armed = true;
        }
        if (armed && samples[i - 1] <= trigLevel && samples[i] > trigLevel) {
          const dy = samples[i] - samples[i - 1];
          const frac = (dy > 0.001) ? (trigLevel - samples[i - 1]) / dy : 0.0;
          return (i - 1) + Math.max(0.0, Math.min(1.0, frac));
        }
      }
    } else {
      let armed = false;
      for (let i = 1; i < maxSearch; i++) {
        if (samples[i - 1] > trigLevel + hyst) {
          armed = true;
        }
        if (armed && samples[i - 1] >= trigLevel && samples[i] < trigLevel) {
          const dy = samples[i - 1] - samples[i];
          const frac = (dy > 0.001) ? (samples[i - 1] - trigLevel) / dy : 0.0;
          return (i - 1) + Math.max(0.0, Math.min(1.0, frac));
        }
      }
    }
    return -1; // Trigger not found
  }

  // Draw DSO Oscilloscope Reticle, Sub-Sample Waveform & Markers
  function drawDSO(rawWaveform, meta) {
    const rect = scopeCanvas.getBoundingClientRect();
    const w = rect.width;
    const h = rect.height;

    scopeCtx.clearRect(0, 0, w, h);

    // Background
    scopeCtx.fillStyle = "#060e0b";
    scopeCtx.fillRect(0, 0, w, h);

    const xDivs = 10;
    const yDivs = 8;
    const padL = 48;
    const padR = 24;
    const padT = 20;
    const padB = 25;
    const plotW = w - padL - padR;
    const plotH = h - padT - padB;

    // Grid lines
    scopeCtx.strokeStyle = "#12261e";
    scopeCtx.lineWidth = 1;
    for (let i = 0; i <= xDivs; i++) {
      const x = padL + (i / xDivs) * plotW;
      scopeCtx.beginPath();
      scopeCtx.moveTo(x, padT);
      scopeCtx.lineTo(x, padT + plotH);
      scopeCtx.stroke();
    }
    for (let j = 0; j <= yDivs; j++) {
      const y = padT + (j / yDivs) * plotH;
      scopeCtx.beginPath();
      scopeCtx.moveTo(padL, y);
      scopeCtx.lineTo(padL + plotW, y);
      scopeCtx.stroke();
    }

    // Center axes (dotted)
    const midX = padL + plotW / 2;
    const midY = padT + plotH / 2;
    scopeCtx.strokeStyle = "#1b4d37";
    scopeCtx.setLineDash([2, 2]);
    scopeCtx.beginPath();
    scopeCtx.moveTo(midX, padT);
    scopeCtx.lineTo(midX, padT + plotH);
    scopeCtx.moveTo(padL, midY);
    scopeCtx.lineTo(padL + plotW, midY);
    scopeCtx.stroke();
    scopeCtx.setLineDash([]);

    // Y-Axis labels (Voltage)
    scopeCtx.fillStyle = "#3fb950";
    scopeCtx.font = "10px monospace";
    scopeCtx.textAlign = "right";
    for (let j = 0; j <= yDivs; j++) {
      const y = padT + (j / yDivs) * plotH;
      const v = (3.3 * (1 - j / yDivs)).toFixed(1);
      scopeCtx.fillText(v + "V", padL - 8, y + 3);
    }

    // X-Axis labels (Time relative to trigger or window)
    scopeCtx.textAlign = "center";
    const windowUs = scopeState.timeWindowUs;
    for (let i = 0; i <= xDivs; i += 2) {
      const x = padL + (i / xDivs) * plotW;
      const t = Math.round((i / xDivs) * windowUs);
      scopeCtx.fillText(t + " us", x, padT + plotH + 18);
    }

    // Determine Trigger Level
    let vMin = 0, vMax = 3300;
    if (rawWaveform && rawWaveform.length > 0) {
      vMin = Math.min(...rawWaveform);
      vMax = Math.max(...rawWaveform);
    }
    if (scopeState.autoLevel) {
      scopeState.effectiveTrigMv = (vMax + vMin) / 2.0;
      if (scopeState.effectiveTrigMv < 100 || scopeState.effectiveTrigMv > 3200) {
        scopeState.effectiveTrigMv = 1650;
      }
      document.getElementById("trigLevelSlider").value = Math.round(scopeState.effectiveTrigMv);
      document.getElementById("trigLevelVal").textContent = Math.round(scopeState.effectiveTrigMv) + " mV (Auto)";
    } else {
      scopeState.effectiveTrigMv = scopeState.manualLevelMv;
    }

    // Search Trigger in the raw continuous buffer
    let trigSampleFrac = -1;
    let activeWave = rawWaveform;

    if (rawWaveform && rawWaveform.length >= 64) {
      trigSampleFrac = findSubSampleTrigger(rawWaveform, scopeState.effectiveTrigMv, scopeState.trigSlope);
    }

    if (trigSampleFrac >= 0) {
      scopeState.isTriggered = true;
      scopeState.lastValidWaveform = rawWaveform;
    } else {
      scopeState.isTriggered = false;
      if (scopeState.trigMode === 'normal' && scopeState.lastValidWaveform) {
        activeWave = scopeState.lastValidWaveform;
        trigSampleFrac = findSubSampleTrigger(activeWave, scopeState.effectiveTrigMv, scopeState.trigSlope);
      }
    }

    // Update Trigger Badge in UI
    const trigBadge = document.getElementById("trigStatusBadge");
    if (scopeState.isTriggered) {
      trigBadge.textContent = "TRIG'D \u2713";
      trigBadge.style.color = "#39d353";
      trigBadge.style.borderColor = "#2ea043";
      trigBadge.style.background = "rgba(57, 211, 83, 0.15)";
    } else if (scopeState.trigMode === 'normal') {
      trigBadge.textContent = "WAIT...";
      trigBadge.style.color = "#d29922";
      trigBadge.style.borderColor = "#9e6a03";
      trigBadge.style.background = "rgba(210, 153, 34, 0.15)";
    } else {
      trigBadge.textContent = "AUTO (FREE)";
      trigBadge.style.color = "#58a6ff";
      trigBadge.style.borderColor = "#1f6feb";
      trigBadge.style.background = "rgba(56, 139, 253, 0.15)";
    }

    // Draw Trigger Level Horizontal Marker (Dotted Line & Arrow)
    const trigY = padT + (1 - Math.max(0, Math.min(3300, scopeState.effectiveTrigMv)) / 3300.0) * plotH;
    scopeCtx.strokeStyle = "#d29922";
    scopeCtx.lineWidth = 1;
    scopeCtx.setLineDash([4, 4]);
    scopeCtx.beginPath();
    scopeCtx.moveTo(padL, trigY);
    scopeCtx.lineTo(padL + plotW, trigY);
    scopeCtx.stroke();
    scopeCtx.setLineDash([]);

    // Trigger arrow T ▶ at right margin
    scopeCtx.fillStyle = "#d29922";
    scopeCtx.font = "bold 9px monospace";
    scopeCtx.textAlign = "left";
    scopeCtx.fillText("T\u25B6", padL + plotW + 2, trigY + 3);

    // Trigger Point Vertical Pointer ▼ T at top
    const trigX = padL + plotW * scopeState.trigPosPct;
    scopeCtx.fillStyle = "#39d353";
    scopeCtx.textAlign = "center";
    scopeCtx.fillText("\u25BC T", trigX, padT - 4);

    if (!activeWave || activeWave.length < 2) return;

    // Sub-Sample Time Window Reconstruction
    const tsUs = 2.0; // 500 kSPS -> 2.0 us per sample
    let tTrigUs = (trigSampleFrac >= 0) ? (trigSampleFrac * tsUs) : 0.0;
    
    // Position the trigger point at trigPosPct across the canvas
    const tLeftUs = tTrigUs - scopeState.trigPosPct * windowUs;

    scopeCtx.save();
    scopeCtx.beginPath();
    scopeCtx.rect(padL, padT, plotW, plotH);
    scopeCtx.clip();

    scopeCtx.strokeStyle = "#00ffc8";
    scopeCtx.lineWidth = 2.2;
    scopeCtx.shadowBlur = 8;
    scopeCtx.shadowColor = "#00ffc8";
    scopeCtx.beginPath();

    const cols = Math.min(plotW, 600);
    const n = activeWave.length;
    let hasStarted = false;

    for (let c = 0; c <= cols; c++) {
      const fracCol = c / cols;
      const tUs = tLeftUs + fracCol * windowUs;
      const sampleU = tUs / tsUs; // Continuous sample index

      if (sampleU >= 0 && sampleU <= (n - 1)) {
        const k = Math.floor(sampleU);
        const alpha = sampleU - k;
        const vNext = (k + 1 < n) ? activeWave[k + 1] : activeWave[k];
        const mv = activeWave[k] * (1.0 - alpha) + vNext * alpha;

        const x = padL + fracCol * plotW;
        const y = padT + (1 - Math.max(0, Math.min(3300, mv)) / 3300.0) * plotH;

        if (!hasStarted) {
          scopeCtx.moveTo(x, y);
          hasStarted = true;
        } else {
          scopeCtx.lineTo(x, y);
        }
      }
    }
    scopeCtx.stroke();
    scopeCtx.restore();
  }

  // Draw 60s Historical Trend Chart
  function drawTrend(samples) {
    const rect = trendCanvas.getBoundingClientRect();
    const w = rect.width;
    const h = rect.height;

    trendCtx.clearRect(0, 0, w, h);

    trendCtx.strokeStyle = "#21262d";
    trendCtx.lineWidth = 1;
    const steps = 4;
    for (let i = 0; i <= steps; i++) {
      const y = (h - 25) * (1 - i / steps) + 12;
      trendCtx.beginPath();
      trendCtx.moveTo(40, y);
      trendCtx.lineTo(w - 10, y);
      trendCtx.stroke();

      const mvLabel = Math.round((3300 * i) / steps);
      trendCtx.fillStyle = "#8b949e";
      trendCtx.font = "10px sans-serif";
      trendCtx.textAlign = "right";
      trendCtx.fillText(mvLabel + "m", 35, y + 3);
    }

    if (!samples || samples.length < 2) return;

    trendCtx.strokeStyle = "#58a6ff";
    trendCtx.lineWidth = 2;
    trendCtx.beginPath();

    const maxLen = 600;
    const startX = 42;
    const plotW = w - 52;

    for (let i = 0; i < samples.length; i++) {
      const s = samples[i];
      const x = startX + (i / (maxLen - 1)) * plotW;
      const mv = Math.max(0, Math.min(3300, s.mv));
      const y = (h - 25) * (1 - mv / 3300.0) + 12;

      if (i === 0) trendCtx.moveTo(x, y);
      else trendCtx.lineTo(x, y);
    }
    trendCtx.stroke();
  }

  async function updateDashboard() {
    try {
      const resp = await fetch("/api/samples");
      if (!resp.ok) return;
      const data = await resp.json();

      const badge = document.getElementById("statusBadge");
      const statusText = document.getElementById("statusText");

      if (data.is_live) {
        badge.className = "status-badge status-live";
        statusText.textContent = "EN VIVO (500 kSPS)";
      } else {
        badge.className = "status-badge status-stale";
        statusText.textContent = "DESCONECTADO (" + data.stale_seconds + "s)";
      }

      if (data.latest) {
        const lat = data.latest;
        document.getElementById("valMv").textContent = (lat.mv || 0) + " mV";
        document.getElementById("valAvgMinMax").textContent =
          "Prom: " + (lat.avg || 0) + " | Mín: " + (lat.min || 0) + " | Máx: " + (lat.max || 0);

        document.getElementById("valSamples").textContent = Number(lat.samples || 0).toLocaleString();
        document.getElementById("valDmaCounters").textContent =
          "DMA TC: " + (lat.tc || 0) + " | Err: " + (lat.err || 0) + " | Overrun: " + (lat.ovr || 0);

        document.getElementById("valM4Hb").textContent = (lat.m4_hb || 0);
        if (lat.ch === 5) {
          document.getElementById("scopePin").textContent = "CH1: PB1 [A3] (Fast)";
        } else {
          document.getElementById("scopePin").textContent = "CH1: PA3 [A0] (Slow)";
        }
      }

      if (data.wave_meta) {
        const wm = data.wave_meta;
        document.getElementById("scopeFreq").textContent = wm.freq_hz ? (wm.freq_hz / 1000).toFixed(2) + " kHz" : "N/A";
        document.getElementById("scopeVpp").textContent = (wm.v_pp / 1000).toFixed(2) + " V";
        document.getElementById("scopeVmax").textContent = (wm.v_max / 1000).toFixed(2) + " V";
        document.getElementById("scopeVmin").textContent = (wm.v_min / 1000).toFixed(2) + " V";
        document.getElementById("scopeDuty").textContent = wm.duty_pct ? wm.duty_pct.toFixed(1) + " %" : "N/A";
      }

      const rateVal = Math.round(data.measured_rate_hz || (data.latest ? data.latest.rate : 500000));
      document.getElementById("valRate").textContent = Number(rateVal).toLocaleString() + " SPS";

      drawDSO(data.waveform, data.wave_meta);
      drawTrend(data.history);
    } catch (e) {
      console.error(e);
    }
  }

  setInterval(updateDashboard, 100);
  updateDashboard();
</script>
</body>
</html>
"""


class TelemetryCollector:
    def __init__(self, host: str, port: int, rate: float = 15.0, ring_size: int = 600):
        self.host = host
        self.port = port
        self.interval = 1.0 / max(rate, 1.0)
        self.history = deque(maxlen=ring_size)
        self.latest = None
        self.waveform = []
        self.wave_meta = {}
        self.last_rx_time = 0.0
        self.running = False
        self.measured_rate_hz = 0.0
        self._prev_samples = None
        self._prev_time = None
        self._lock = threading.Lock()

    def start(self):
        self.running = True
        t = threading.Thread(target=self._poll_loop, daemon=True)
        t.start()

    def _poll_loop(self):
        sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        sock.settimeout(0.5)

        while self.running:
            start_t = time.time()
            try:
                # Drain any late / pending responses in the socket buffer to avoid desync
                while True:
                    try:
                        sock.setblocking(False)
                        sock.recv(4096)
                    except (BlockingIOError, socket.error):
                        break
                sock.setblocking(True)
                sock.settimeout(0.5)

                # Send binary WAVE command
                sock.sendto(b"WAVE", (self.host, self.port))
                data, _ = sock.recvfrom(2048)
                now = time.time()

                if len(data) >= WAVE_HEADER_LEN:
                    (
                        magic,
                        version,
                        header_len,
                        rate_hz,
                        num_samples,
                        vref_mv,
                        seq,
                        min_raw,
                        max_raw,
                        avg_raw,
                        ch,
                        mv,
                        samples,
                        tc,
                        ovr,
                        m4_hb,
                        ts,
                    ) = struct.unpack(WAVE_HEADER_FMT, data[:WAVE_HEADER_LEN])

                    # header_len, not a hardcoded 48: that is what lets firmware
                    # append a field without this poller reading the sample block
                    # from the wrong offset.
                    end = header_len + num_samples * 2
                    if (magic == WAVE_MAGIC and version == WAVE_VERSION
                            and header_len >= WAVE_HEADER_LEN and len(data) >= end):
                        raw_samples = struct.unpack(f"<{num_samples}H", data[header_len:end])
                        # Convert to millivolts in natural chronological order
                        samples_mv = [(s * vref_mv) / 65535.0 for s in raw_samples]

                        v_min = min(samples_mv)
                        v_max = max(samples_mv)
                        v_pp = v_max - v_min
                        v_mid = (v_max + v_min) / 2.0

                        # Calculate frequency and duty cycle via midpoint crossings
                        rising_edges = []
                        for idx in range(1, len(samples_mv)):
                            if samples_mv[idx - 1] < v_mid <= samples_mv[idx]:
                                rising_edges.append(idx)

                        freq_hz = None
                        duty_pct = None
                        if len(rising_edges) >= 2:
                            periods = [rising_edges[k] - rising_edges[k - 1] for k in range(1, len(rising_edges))]
                            avg_p = sum(periods) / len(periods)
                            if avg_p > 0:
                                freq_hz = rate_hz / avg_p
                            p_start = rising_edges[0]
                            p_end = rising_edges[1]
                            high_cnt = sum(1 for s in samples_mv[p_start:p_end] if s >= v_mid)
                            duty_pct = (high_cnt / (p_end - p_start)) * 100.0

                        total_us = (num_samples / rate_hz) * 1e6 if rate_hz > 0 else 1024

                        with self._lock:
                            self.latest = {
                                "ch": ch,
                                "raw": raw_samples[-1],
                                "avg": avg_raw,
                                "min": min_raw,
                                "max": max_raw,
                                "mv": mv,
                                "rate": rate_hz,
                                "seq": seq,
                                "samples": samples,
                                "tc": tc,
                                "ovr": ovr,
                                "m4_hb": m4_hb,
                            }
                            # Send continuous samples in natural chronological order to browser
                            self.waveform = samples_mv
                            self.wave_meta = {
                                "rate_hz": rate_hz,
                                "num_samples": num_samples,
                                "total_us": total_us,
                                "vref_mv": vref_mv,
                                "v_min": v_min,
                                "v_max": v_max,
                                "v_pp": v_pp,
                                "freq_hz": freq_hz,
                                "duty_pct": duty_pct,
                            }
                            self.last_rx_time = now
                            self.history.append({"t": now, "mv": mv, "raw": raw_samples[-1]})

                            if self._prev_samples is not None and self._prev_time is not None:
                                dt = now - self._prev_time
                                ds = samples - self._prev_samples
                                if dt > 0.05:
                                    inst_rate = ds / dt
                                    self.measured_rate_hz = 0.8 * self.measured_rate_hz + 0.2 * inst_rate if self.measured_rate_hz > 0 else inst_rate

                            self._prev_samples = samples
                            self._prev_time = now

            except (socket.timeout, struct.error, OSError):
                pass

            elapsed = time.time() - start_t
            sleep_t = self.interval - elapsed
            if sleep_t > 0:
                time.sleep(sleep_t)

        sock.close()

    def get_snapshot(self):
        now = time.time()
        with self._lock:
            stale_sec = int(now - self.last_rx_time) if self.last_rx_time > 0 else 999
            is_live = (self.last_rx_time > 0) and (now - self.last_rx_time < 2.0)
            return {
                "is_live": is_live,
                "stale_seconds": stale_sec,
                "measured_rate_hz": self.measured_rate_hz,
                "latest": self.latest,
                "waveform": list(self.waveform),
                "wave_meta": dict(self.wave_meta),
                "history": list(self.history),
            }


class DashboardRequestHandler(http.server.BaseHTTPRequestHandler):
    collector: TelemetryCollector = None

    def do_GET(self):
        if self.path in ("/", "/index.html"):
            self.send_response(200)
            self.send_header("Content-Type", "text/html; charset=utf-8")
            self.send_header("Cache-Control", "no-cache")
            self.end_headers()
            self.wfile.write(HTML_PAGE.encode("utf-8"))

        elif self.path == "/api/samples":
            snapshot = self.collector.get_snapshot()
            data = json.dumps(snapshot).encode("utf-8")
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Cache-Control", "no-cache")
            self.end_headers()
            self.wfile.write(data)

        elif self.path == "/api/health":
            snapshot = self.collector.get_snapshot()
            health = {
                "live": snapshot["is_live"],
                "last_seen_sec_ago": snapshot["stale_seconds"],
                "samples_in_ring": len(snapshot["history"]),
            }
            data = json.dumps(health).encode("utf-8")
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Cache-Control", "no-cache")
            self.end_headers()
            self.wfile.write(data)

        else:
            self.send_response(404)
            self.end_headers()

    def log_message(self, format, *args):
        return


def run_dashboard(host: str, port: int, bind_addr: str, rate: float):
    parts = bind_addr.split(":")
    bind_ip = parts[0]
    bind_port = int(parts[1]) if len(parts) > 1 else 8080

    collector = TelemetryCollector(host, port, rate)
    collector.start()

    DashboardRequestHandler.collector = collector
    server = http.server.ThreadingHTTPServer((bind_ip, bind_port), DashboardRequestHandler)

    print(f"============================================================")
    print(f"  STM32H755ZI DSO Live Oscilloscope & Telemetry Dashboard")
    print(f"  Target Board : {host}:{port} (UDP)")
    print(f"  Dashboard UI : http://{bind_ip}:{bind_port}/")
    print(f"  Polling Rate : {rate:.1f} Hz (Binary WAVE stream)")
    print(f"============================================================")
    print(f"Presiona Ctrl+C para detener el servidor.")

    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print("\nServidor detenido.")
    finally:
        collector.running = False
        server.server_close()


def main():
    parser = argparse.ArgumentParser(description="Real-time ADC telemetry & DSO web dashboard.")
    parser.add_argument("--host", default="192.168.1.150", help="Target board IP (default: 192.168.1.150)")
    parser.add_argument("--port", type=int, default=5001, help="Target UDP port (default: 5001)")
    parser.add_argument("--bind", default="127.0.0.1:8080", help="Bind IP:port for web server (default: 127.0.0.1:8080)")
    parser.add_argument("--rate", type=float, default=15.0, help="Polling rate in Hz (default: 15.0)")

    args = parser.parse_args()
    run_dashboard(args.host, args.port, args.bind, args.rate)


if __name__ == "__main__":
    main()
