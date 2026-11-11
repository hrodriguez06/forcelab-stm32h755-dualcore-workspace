/**
 * FFT Magnitude Spectrum Renderer (2048 Bins, 0 .. 12.8 kHz at 25.6 kSPS).
 */
export class SpectrumRenderer {
  constructor(canvasId, containerId, peakInfoId) {
    this.canvas = document.getElementById(canvasId);
    this.container = document.getElementById(containerId);
    this.peakInfoEl = peakInfoId ? document.getElementById(peakInfoId) : null;
    this.ctx = this.canvas ? this.canvas.getContext('2d') : null;

    this.currentBins = new Array(2048).fill(0);
    this.peakBinIndex = 0;
  }

  setBins(bins) {
    this.currentBins = bins;
  }

  setPeakIndex(index) {
    this.peakBinIndex = index;
  }

  resize() {
    if (!this.canvas || !this.container || !this.ctx) return;
    const dpr = window.devicePixelRatio || 1;
    const rect = this.container.getBoundingClientRect();
    this.canvas.width = rect.width * dpr;
    this.canvas.height = rect.height * dpr;
    this.ctx.setTransform(1, 0, 0, 1, 0, 0);
    this.ctx.scale(dpr, dpr);
    this.render();
  }

  render() {
    if (!this.ctx || !this.container) return;

    const rect = this.container.getBoundingClientRect();
    const w = rect.width;
    const h = rect.height;
    const numBins = this.currentBins.length || 2048;
    const df = 12800.0 / numBins;

    this.ctx.clearRect(0, 0, w, h);

    // Frequency grid lines (1k, 3k, 6k, 9k, 12k)
    this.ctx.strokeStyle = '#1e293b';
    this.ctx.lineWidth = 1;
    this.ctx.fillStyle = '#64748b';
    this.ctx.font = '10px monospace';

    const freqs = [1000, 3000, 6000, 9000, 12000];
    freqs.forEach((freq) => {
      const x = (freq / 12800) * w;
      this.ctx.beginPath();
      this.ctx.moveTo(x, 0);
      this.ctx.lineTo(x, h - 14);
      this.ctx.stroke();
      this.ctx.fillText((freq / 1000) + 'k', x - 8, h - 3);
    });

    // Dynamic vertical scaling
    let maxVal = 10;
    for (let i = 0; i < numBins; i++) {
      if (this.currentBins[i] > maxVal) maxVal = this.currentBins[i];
    }
    if (this.peakInfoEl) {
      this.peakInfoEl.textContent = `Peak: ${maxVal.toFixed(1)} mV (${(this.peakBinIndex * df).toFixed(1)} Hz)`;
    }

    // Filled area under spectrum curve
    const grad = this.ctx.createLinearGradient(0, 0, 0, h);
    grad.addColorStop(0, 'rgba(56, 189, 248, 0.4)');
    grad.addColorStop(1, 'rgba(56, 189, 248, 0.0)');

    this.ctx.beginPath();
    this.ctx.moveTo(0, h - 14);

    for (let i = 0; i < numBins; i++) {
      const x = (i / (numBins - 1)) * w;
      const norm = Math.min(1.0, this.currentBins[i] / (maxVal * 1.15));
      const y = (h - 16) - (norm * (h - 24));
      this.ctx.lineTo(x, y);
    }

    this.ctx.lineTo(w, h - 14);
    this.ctx.fillStyle = grad;
    this.ctx.fill();

    // Spectrum trace
    this.ctx.beginPath();
    for (let i = 0; i < numBins; i++) {
      const x = (i / (numBins - 1)) * w;
      const norm = Math.min(1.0, this.currentBins[i] / (maxVal * 1.15));
      const y = (h - 16) - (norm * (h - 24));
      if (i === 0) this.ctx.moveTo(x, y);
      else this.ctx.lineTo(x, y);
    }
    this.ctx.strokeStyle = '#38bdf8';
    this.ctx.lineWidth = 1.8;
    this.ctx.stroke();

    // Fundamental peak marker
    if (this.peakBinIndex > 0 && this.peakBinIndex < numBins) {
      const px = (this.peakBinIndex / (numBins - 1)) * w;
      const pNorm = Math.min(1.0, this.currentBins[this.peakBinIndex] / (maxVal * 1.15));
      const py = (h - 16) - (pNorm * (h - 24));

      this.ctx.fillStyle = '#10b981';
      this.ctx.beginPath();
      this.ctx.arc(px, py, 4, 0, Math.PI * 2);
      this.ctx.fill();
      this.ctx.strokeStyle = '#fff';
      this.ctx.lineWidth = 1.5;
      this.ctx.stroke();
    }
  }
}
