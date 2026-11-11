/**
 * UI Controller: Updates DOM telemetry cards, harmonic bars, 16-band WPT metrics,
 * and dispatches runtime acquisition commands (`/api/control`).
 */
export class UIController {
  constructor(renderers) {
    this.oscilloscope = renderers.oscilloscope;
    this.spectrum = renderers.spectrum;
    this.scalogram = renderers.scalogram;

    this.connPill = document.getElementById('connPill');
    this.connText = document.getElementById('connText');
    this.signalBanner = document.getElementById('signalBanner');
    this.bannerState = document.getElementById('bannerState');
    this.blockIdEl = document.getElementById('blockId');
    this.fsInfoEl = document.getElementById('fsInfo');

    this.valF0 = document.getElementById('valF0');
    this.subPeak = document.getElementById('subPeak');
    this.valVpp = document.getElementById('valVpp');
    this.barVpp = document.getElementById('barVpp');
    this.valDc = document.getElementById('valDc');
    this.barDc = document.getElementById('barDc');
    this.subDcV = document.getElementById('subDcV');
    this.valSnr = document.getElementById('valSnr');
    this.subNoise = document.getElementById('subNoise');
    this.valThd = document.getElementById('valThd');
    this.subOverrun = document.getElementById('subOverrun');
    this.harmRatio = document.getElementById('harmRatio');
    this.fpsCounter = document.getElementById('fpsCounter');

    this.wptSummary = document.getElementById('wptSummary');
    this.transientBadge = document.getElementById('transientBadge');
    this.wptTransMag = document.getElementById('wptTransMag');
    this.wptCrest = document.getElementById('wptCrest');
    this.wptTransTime = document.getElementById('wptTransTime');
    this.wptEntropy = document.getElementById('wptEntropy');
    this.wptTransCount = document.getElementById('wptTransCount');

    this.btnToggleStream = document.getElementById('btnToggleStream');
    this.btnRefreshSpec = document.getElementById('btnRefreshSpec');
    this.rateBtns = document.querySelectorAll('.rate-btn');

    this.pktCount = 0;
    this.lastFpsTick = Date.now();
    this.isPaused = false;

    this.bindEvents();
    this.startFpsMonitor();
  }

  async sendControl(payload) {
    try {
      await fetch('/api/control', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify(payload)
      });
    } catch (e) {
      console.error('Control API error:', e);
    }
  }

  bindEvents() {
    this.rateBtns.forEach((btn) => {
      btn.addEventListener('click', () => {
        this.rateBtns.forEach((b) => b.classList.remove('active'));
        btn.classList.add('active');
        const fs = parseInt(btn.dataset.fs, 10);
        this.sendControl({ cmd: 'FS', fs_hz: fs });
        this.setPausedState(false);
      });
    });

    if (this.btnToggleStream) {
      this.btnToggleStream.addEventListener('click', () => {
        this.setPausedState(!this.isPaused);
        this.sendControl({ cmd: this.isPaused ? 'STOP' : 'STREAM' });
      });
    }

    if (this.btnRefreshSpec) {
      this.btnRefreshSpec.addEventListener('click', () => {
        this.sendControl({ cmd: 'SPECTRUM' });
      });
    }
  }

  setPausedState(paused) {
    this.isPaused = paused;
    if (this.scalogram) {
      this.scalogram.setPaused(paused);
    }
    if (!this.btnToggleStream) return;

    if (this.isPaused) {
      this.btnToggleStream.textContent = '\u25B6 Resume';
      this.btnToggleStream.classList.remove('active');
      this.btnToggleStream.classList.add('paused');
    } else {
      this.btnToggleStream.textContent = '\u23F8 Pause';
      this.btnToggleStream.classList.remove('paused');
      this.btnToggleStream.classList.add('active');
    }
  }

  setOnline(online) {
    if (!this.connPill || !this.connText) return;
    if (online) {
      this.connPill.classList.remove('offline');
      this.connText.textContent = 'LIVE UDP';
    } else {
      this.connPill.classList.add('offline');
      this.connText.textContent = 'NO SIGNAL';
    }
  }

  startFpsMonitor() {
    setInterval(() => {
      const now = Date.now();
      const elapsed = (now - this.lastFpsTick) / 1000;
      if (elapsed > 0 && this.fpsCounter) {
        const rate = (this.pktCount / elapsed).toFixed(1);
        this.fpsCounter.textContent = `${rate} pkt/s`;
        this.pktCount = 0;
        this.lastFpsTick = now;
      }
    }, 2000);
  }

  updateTelemetry(d) {
    this.setOnline(true);
    this.pktCount++;

    if (this.blockIdEl) this.blockIdEl.textContent = `Block #${d.block_id}`;
    if (this.fsInfoEl) this.fsInfoEl.textContent = `${(d.fs_hz / 1000).toFixed(1)} kSPS | ${d.block_size} pts`;

    if (this.signalBanner && this.bannerState) {
      if (d.detected) {
        this.signalBanner.className = 'banner detected';
        this.bannerState.textContent = `\uD83D\uDFE2 Active Signal Detected (${d.f0_hz.toFixed(1)} Hz)`;
      } else {
        this.signalBanner.className = 'banner noise';
        this.bannerState.textContent = '\u26AA No Periodic Tone (Noise Floor)';
      }
    }

    if (this.valF0) this.valF0.textContent = d.f0_hz.toFixed(1);
    if (this.subPeak) this.subPeak.textContent = `Peak: ${d.f0_mag_mv} mV`;

    if (this.valVpp) this.valVpp.textContent = d.v_pp_mv;
    if (this.barVpp) {
      const vppPct = Math.min(100, (d.v_pp_mv / 3300) * 100);
      this.barVpp.style.width = `${vppPct}%`;
    }

    if (this.valDc) this.valDc.textContent = d.dc_offset_mv;
    if (this.barDc) {
      const dcPct = Math.min(100, (d.dc_offset_mv / 3300) * 100);
      this.barDc.style.width = `${dcPct}%`;
    }
    if (this.subDcV) this.subDcV.textContent = `${(d.dc_offset_mv / 1000).toFixed(2)} V`;

    if (this.valSnr) this.valSnr.textContent = d.snr_db.toFixed(1);
    if (this.subNoise) this.subNoise.textContent = `Floor: ${d.noise_floor_mv} mV`;

    if (this.valThd) this.valThd.textContent = d.thd_pct.toFixed(1);
    if (this.subOverrun) this.subOverrun.textContent = `Overruns: ${d.overrun_count}`;

    // Harmonics H1..H5
    const hMax = Math.max(1, d.harmonics[0] || 1);
    if (this.harmRatio) this.harmRatio.textContent = `H1 = ${d.harmonics[0]} mV`;
    for (let i = 0; i < 5; i++) {
      const val = d.harmonics[i] || 0;
      const pct = Math.max(3, Math.min(100, (val / hMax) * 100));
      const hvalEl = document.getElementById(`hval${i}`);
      const hbarEl = document.getElementById(`hbar${i}`);
      if (hvalEl) hvalEl.textContent = `${val}`;
      if (hbarEl) hbarEl.style.height = `${pct}%`;
    }

    // Update fundamental peak marker on FFT Spectrum
    const df = (d.fs_hz || 25600) / (d.block_size || 4096);
    const peakBinIndex = d.detected ? Math.round(d.f0_hz / df) : 0;
    if (this.spectrum) {
      this.spectrum.setPeakIndex(peakBinIndex);
    }

    // Update Reconstructed Oscilloscope
    if (this.oscilloscope) {
      this.oscilloscope.render(d);
    }

    // Update 4-Level Daubechies-4 (db4) Wavelet Packet Transform (16 Sub-Bands)
    if (d.wpt) {
      const bwHz = ((d.fs_hz || 25600) / 32.0).toFixed(0);
      if (this.wptSummary) {
        this.wptSummary.textContent = `16 Sub-Bands (${bwHz} Hz/band)`;
      }

      const nBands = Math.min(16, (d.wpt.energy_pct || []).length);
      for (let b = 0; b < nBands; b++) {
        const pct = d.wpt.energy_pct[b] || 0;
        const rms = d.wpt.rms_mv[b] || 0;
        const rmsEl = document.getElementById(`wptRms${b}`);
        const pctEl = document.getElementById(`wptPct${b}`);
        const barEl = document.getElementById(`wptBar${b}`);
        if (rmsEl) rmsEl.textContent = `${rms} mV`;
        if (pctEl) pctEl.textContent = `${pct.toFixed(1)}%`;
        if (barEl) barEl.style.width = `${Math.min(100, pct)}%`;
      }

      if (this.scalogram) {
        this.scalogram.setTargetBands(d.wpt.energy_pct);
      }

      if (this.wptTransMag) this.wptTransMag.textContent = `${d.wpt.transient_mag_mv} mV`;
      if (this.wptCrest) this.wptCrest.textContent = d.wpt.crest_hf.toFixed(1);
      if (this.wptTransTime) this.wptTransTime.textContent = `${d.wpt.transient_time_ms.toFixed(2)} ms`;
      if (this.wptEntropy && d.wpt.entropy_norm !== undefined) {
        this.wptEntropy.textContent = d.wpt.entropy_norm.toFixed(3);
      }
      if (this.wptTransCount) this.wptTransCount.textContent = `${d.wpt.transient_count}`;

      if (this.transientBadge) {
        if (d.wpt.transient_alert) {
          this.transientBadge.className = 'transient-badge alert';
          this.transientBadge.innerHTML = `<span>\u26A1 GLITCH @ ${d.wpt.transient_time_ms.toFixed(1)} ms (${d.wpt.transient_mag_mv} mV)</span>`;
        } else {
          this.transientBadge.className = 'transient-badge normal';
          this.transientBadge.innerHTML = '<span>\u2705 NO HF TRANSIENTS</span>';
        }
      }
    }
  }

  updateSpectrum(data) {
    if (data && data.bins && data.bins.length && this.spectrum) {
      this.spectrum.setBins(data.bins);
      this.spectrum.render();
    }
  }
}
