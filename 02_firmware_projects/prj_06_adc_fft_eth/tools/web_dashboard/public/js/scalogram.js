/**
 * 2D Wavelet Scalogram Waterfall Renderer (16 WPT Sub-Bands, 60 FPS with LERP).
 */
export class ScalogramRenderer {
  constructor(canvasId, containerId, fpsBadgeId, options = {}) {
    this.canvas = document.getElementById(canvasId);
    this.container = document.getElementById(containerId);
    this.fpsBadge = fpsBadgeId ? document.getElementById(fpsBadgeId) : null;
    this.ctx = this.canvas ? this.canvas.getContext('2d') : null;

    this.maxColumns = options.maxColumns || 240; // ~4s history at 60 FPS
    this.scaloHistory = [];
    this.currentInterpBands = new Array(16).fill(0);
    this.targetWptBands = new Array(16).fill(0);

    this.frameCount = 0;
    this.fpsTimer = performance.now();
    this.currentFps = 60;
    this.isPaused = false;
    this.animationId = null;

    this.loop = this.loop.bind(this);
  }

  setPaused(paused) {
    this.isPaused = paused;
  }

  setTargetBands(bands) {
    if (!bands || !bands.length) return;
    for (let i = 0; i < 16; i++) {
      this.targetWptBands[i] = bands[i] || 0;
    }
  }

  getColor(pct) {
    const p = Math.max(0, Math.min(100, pct)) / 100;
    let r;
    let g;
    let b;
    if (p < 0.25) {
      const t = p / 0.25;
      r = 15 + t * (126 - 15);
      g = 23 + t * (34 - 23);
      b = 42 + t * (206 - 42);
    } else if (p < 0.5) {
      const t = (p - 0.25) / 0.25;
      r = 126 + t * (219 - 126);
      g = 34 + t * (39 - 34);
      b = 206 + t * (119 - 206);
    } else if (p < 0.75) {
      const t = (p - 0.5) / 0.25;
      r = 219 + t * (234 - 219);
      g = 39 + t * (88 - 39);
      b = 119 + t * (12 - 119);
    } else if (p < 0.9) {
      const t = (p - 0.75) / 0.15;
      r = 234 + t * (234 - 234);
      g = 88 + t * (179 - 88);
      b = 12 + t * (8 - 12);
    } else {
      const t = (p - 0.9) / 0.1;
      r = 234 + t * (255 - 234);
      g = 179 + t * (255 - 179);
      b = 8 + t * (255 - 8);
    }
    return `rgb(${Math.round(r)}, ${Math.round(g)}, ${Math.round(b)})`;
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

    this.ctx.clearRect(0, 0, w, h);

    this.ctx.fillStyle = '#0b0f19';
    this.ctx.fillRect(0, 0, w, h);

    const labelW = 46;
    const gridW = w - labelW;
    const numBands = 16;
    const rh = h / numBands;

    const nCols = this.scaloHistory.length;
    const colW = gridW / this.maxColumns;

    for (let c = 0; c < nCols; c++) {
      const x = labelW + (this.maxColumns - nCols + c) * colW;
      const entry = this.scaloHistory[c];
      for (let b = 0; b < numBands; b++) {
        const bandIdx = (numBands - 1) - b;
        const val = (entry && entry[bandIdx] !== undefined) ? entry[bandIdx] : 0;
        this.ctx.fillStyle = this.getColor(val);
        this.ctx.fillRect(x, b * rh, colW + 0.6, rh - 0.5);
      }
    }

    this.ctx.strokeStyle = 'rgba(30, 41, 59, 0.6)';
    this.ctx.lineWidth = 1;
    for (let b = 1; b < numBands; b++) {
      this.ctx.beginPath();
      this.ctx.moveTo(0, b * rh);
      this.ctx.lineTo(w, b * rh);
      this.ctx.stroke();
    }

    this.ctx.strokeStyle = '#1e293b';
    this.ctx.beginPath();
    this.ctx.moveTo(labelW, 0);
    this.ctx.lineTo(labelW, h);
    this.ctx.stroke();

    this.ctx.font = '9px monospace';
    for (let b = 0; b < numBands; b++) {
      const bandIdx = (numBands - 1) - b;
      const y = b * rh + rh * 0.75;
      this.ctx.fillStyle = '#cbd5e1';
      this.ctx.fillText(`B${bandIdx}`, 4, y);
      this.ctx.fillStyle = '#64748b';
      this.ctx.font = '7.5px monospace';
      const freqKhz = ((bandIdx + 1) * 0.8).toFixed(1) + 'k';
      this.ctx.fillText(freqKhz, 23, y);
      this.ctx.font = '9px monospace';
    }
  }

  loop(timestamp) {
    if (!this.isPaused) {
      const LERP_FACTOR = 0.15;
      for (let i = 0; i < 16; i++) {
        const target = this.targetWptBands[i] || 0;
        this.currentInterpBands[i] += (target - this.currentInterpBands[i]) * LERP_FACTOR;
      }

      this.scaloHistory.push([...this.currentInterpBands]);
      if (this.scaloHistory.length > this.maxColumns) {
        this.scaloHistory.shift();
      }

      this.render();

      this.frameCount++;
      if (timestamp - this.fpsTimer >= 1000) {
        this.currentFps = this.frameCount;
        this.frameCount = 0;
        this.fpsTimer = timestamp;
        if (this.fpsBadge) {
          this.fpsBadge.textContent = `${this.currentFps} Hz (${this.currentFps} FPS)`;
        }
      }
    }

    this.animationId = requestAnimationFrame(this.loop);
  }

  start() {
    if (!this.animationId) {
      this.animationId = requestAnimationFrame(this.loop);
    }
  }

  stop() {
    if (this.animationId) {
      cancelAnimationFrame(this.animationId);
      this.animationId = null;
    }
  }
}
