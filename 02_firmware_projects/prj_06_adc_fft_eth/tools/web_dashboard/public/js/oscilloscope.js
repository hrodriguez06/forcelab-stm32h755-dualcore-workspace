/**
 * Reconstructed Oscilloscope Renderer: Synthesizes 2 cycles of the time-domain
 * waveform from the measured DC offset and H1..H10 Fourier harmonic amplitudes.
 */
export class OscilloscopeRenderer {
  constructor(canvasId, containerId, dcValId, vppValId) {
    this.canvas = document.getElementById(canvasId);
    this.container = document.getElementById(containerId);
    this.ctx = this.canvas ? this.canvas.getContext('2d') : null;
    this.scopeDcVal = dcValId ? document.getElementById(dcValId) : null;
    this.scopeVppVal = vppValId ? document.getElementById(vppValId) : null;
    this.lastWaveformData = null;
  }

  resize() {
    if (!this.canvas || !this.container || !this.ctx) return;
    const dpr = window.devicePixelRatio || 1;
    const rect = this.container.getBoundingClientRect();
    this.canvas.width = rect.width * dpr;
    this.canvas.height = rect.height * dpr;
    this.ctx.setTransform(1, 0, 0, 1, 0, 0);
    this.ctx.scale(dpr, dpr);

    if (this.lastWaveformData) {
      this.render(this.lastWaveformData);
    }
  }

  render(data) {
    if (!this.ctx || !this.container) return;
    this.lastWaveformData = data;

    const rect = this.container.getBoundingClientRect();
    const w = rect.width;
    const h = rect.height;

    this.ctx.clearRect(0, 0, w, h);

    const f0 = data.f0_hz || 0;
    const vdc = (data.dc_offset_mv !== undefined) ? data.dc_offset_mv : 1500;
    const vpp = (data.v_pp_mv !== undefined) ? data.v_pp_mv : 0;
    const harms = data.harmonics || [0, 0, 0, 0, 0];

    if (this.scopeDcVal) this.scopeDcVal.textContent = (vdc / 1000.0).toFixed(2) + ' V';
    if (this.scopeVppVal) this.scopeVppVal.textContent = (vpp / 1000.0).toFixed(2) + ' Vpp';

    // Vertical scale: 0 mV .. 3300 mV (ADC full-scale input range)
    const vMax = 3300.0;
    const toY = (mv) => {
      const norm = Math.max(0, Math.min(1.0, mv / vMax));
      return (h - 16) - (norm * (h - 26));
    };

    // 1. Horizontal voltage grid
    this.ctx.strokeStyle = '#1e293b';
    this.ctx.lineWidth = 1;
    this.ctx.fillStyle = '#64748b';
    this.ctx.font = '10px monospace';

    const vLines = [500, 1000, 1500, 2000, 2500, 3000];
    vLines.forEach((mv) => {
      const y = toY(mv);
      this.ctx.beginPath();
      this.ctx.moveTo(0, y);
      this.ctx.lineTo(w, y);
      this.ctx.stroke();
      this.ctx.fillText((mv / 1000).toFixed(1) + 'V', 6, y - 3);
    });

    // 2. Dashed amber DC offset reference line
    const dcY = toY(vdc);
    this.ctx.save();
    this.ctx.strokeStyle = 'rgba(245, 158, 11, 0.5)';
    this.ctx.setLineDash([4, 4]);
    this.ctx.lineWidth = 1.2;
    this.ctx.beginPath();
    this.ctx.moveTo(0, dcY);
    this.ctx.lineTo(w, dcY);
    this.ctx.stroke();
    this.ctx.fillStyle = 'rgba(245, 158, 11, 0.9)';
    this.ctx.fillText(`Vdc=${(vdc / 1000).toFixed(2)}V`, w - 80, dcY - 4);
    this.ctx.restore();

    // 3. Vertical cycle grid divisions (0 .. 2.0T)
    const tDivs = [0.25, 0.5, 0.75, 1.0, 1.25, 1.5, 1.75, 2.0];
    const periodMs = f0 > 0 ? (1000.0 / f0) : 0;
    tDivs.forEach((frac) => {
      const x = (frac / 2.0) * w;
      this.ctx.beginPath();
      this.ctx.moveTo(x, 0);
      this.ctx.lineTo(x, h - 14);
      this.ctx.stroke();
      if (frac === 1.0 && periodMs > 0) {
        this.ctx.fillText('1T (' + periodMs.toFixed(2) + 'ms)', x - 28, h - 3);
      } else if (frac === 2.0) {
        this.ctx.fillText('2T', x - 18, h - 3);
      }
    });

    // 4. Fourier series synthesis: v(t) = Vdc + SUM(Hn * sin(n * theta))
    const points = 250;
    const waveY = new Float32Array(points);

    for (let i = 0; i < points; i++) {
      const phi = (i / (points - 1)) * (4.0 * Math.PI); // 2 full cycles (0 .. 4pi)
      let v = vdc;

      if (data.detected && f0 > 0) {
        const hLen = Math.min(10, harms.length);
        for (let n = 1; n <= hLen; n++) {
          const hMag = harms[n - 1] || 0;
          v += hMag * Math.sin(n * phi);
        }
      }

      waveY[i] = toY(v);
    }

    // Soft gradient fill under curve
    const grad = this.ctx.createLinearGradient(0, 0, 0, h);
    grad.addColorStop(0, 'rgba(16, 185, 129, 0.2)');
    grad.addColorStop(1, 'rgba(16, 185, 129, 0.0)');

    this.ctx.beginPath();
    this.ctx.moveTo(0, dcY);
    for (let i = 0; i < points; i++) {
      const x = (i / (points - 1)) * w;
      this.ctx.lineTo(x, waveY[i]);
    }
    this.ctx.lineTo(w, dcY);
    this.ctx.fillStyle = grad;
    this.ctx.fill();

    // Waveform stroke
    this.ctx.save();
    this.ctx.shadowColor = 'rgba(16, 185, 129, 0.7)';
    this.ctx.shadowBlur = 5;
    this.ctx.strokeStyle = '#10b981';
    this.ctx.lineWidth = 2.2;
    this.ctx.beginPath();

    for (let i = 0; i < points; i++) {
      const x = (i / (points - 1)) * w;
      if (i === 0) this.ctx.moveTo(x, waveY[i]);
      else this.ctx.lineTo(x, waveY[i]);
    }
    this.ctx.stroke();
    this.ctx.restore();
  }
}
