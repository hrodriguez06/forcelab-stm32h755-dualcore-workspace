/**
 * Main Application Entry Point — STM32H755ZI DSP Web Dashboard
 * Coordinates SSE telemetry & spectrum streams, UI controls, and 60 FPS Canvas renderers.
 */
import { OscilloscopeRenderer } from './oscilloscope.js';
import { SpectrumRenderer } from './spectrum.js';
import { ScalogramRenderer } from './scalogram.js';
import { UIController } from './ui-controller.js';

document.addEventListener('DOMContentLoaded', () => {
  // 1. Instantiate Canvas Renderers
  const oscilloscope = new OscilloscopeRenderer('scopeCanvas', 'scopeContainer', 'scopeDcVal', 'scopeVppVal');
  const spectrum = new SpectrumRenderer('specCanvas', 'specContainer', 'specPeakInfo');
  const scalogram = new ScalogramRenderer('scaloCanvas', 'scaloContainer', 'scaloFpsBadge');

  // 2. Instantiate UI Controller
  const ui = new UIController({
    oscilloscope,
    spectrum,
    scalogram
  });

  // 3. Responsive Canvas Resize Listener
  const resizeAll = () => {
    oscilloscope.resize();
    spectrum.resize();
    scalogram.resize();
  };
  window.addEventListener('resize', resizeAll);
  resizeAll();

  // 4. Start 60 Hz Waterfall Scalogram Animation Loop
  scalogram.start();

  // 5. Connect to Server-Sent Events (SSE) Stream
  const evtSource = new EventSource('/events');

  evtSource.addEventListener('telemetry', (e) => {
    try {
      const data = JSON.parse(e.data);
      ui.updateTelemetry(data);
    } catch (err) {
      console.error('Error parsing SSE telemetry:', err);
    }
  });

  evtSource.addEventListener('spectrum', (e) => {
    try {
      const data = JSON.parse(e.data);
      ui.updateSpectrum(data);
    } catch (err) {
      console.error('Error parsing SSE spectrum:', err);
    }
  });

  evtSource.addEventListener('status', (e) => {
    try {
      const data = JSON.parse(e.data);
      if (!data.connected) {
        ui.setOnline(false);
      }
    } catch (_) {}
  });

  evtSource.onerror = () => {
    ui.setOnline(false);
  };
});
