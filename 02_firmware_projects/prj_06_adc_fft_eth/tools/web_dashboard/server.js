#!/usr/bin/env node
/**
 * Real-Time Web Dashboard Server — STM32H755ZI DSP Spectrum Analyzer
 * ==================================================================
 * Zero-dependency Node.js bridge:
 *   1. Communicates with the Nucleo-H755ZI-Q over UDP (:5002).
 *   2. Decodes 144-byte binary telemetry packets (`DSP2`, 4096-pt FFT + 16-band
 *      Daubechies-4 Wavelet Packet Transform) and 4-chunk 2048-bin spectrum
 *      bursts (`SPEC`).
 *   3. Streams real-time JSON updates to desktop/mobile browsers over Server-Sent
 *      Events (SSE `/events`) and exposes HTTP control endpoints (`/api/control`,
 *      `/api/status`).
 *
 * Usage:
 *   node server.js [--ip 192.168.1.150] [--port 5002] [--http 8080]
 */

const dgram = require('dgram');
const http = require('http');
const os = require('os');
const fs = require('fs');
const path = require('path');

// --- CLI / Environment Configuration ---
function getArg(flag, fallback) {
  const idx = process.argv.indexOf(flag);
  if (idx !== -1 && idx + 1 < process.argv.length) {
    return process.argv[idx + 1];
  }
  return fallback;
}

const STM32_IP = getArg('--ip', process.env.STM32_IP || '192.168.1.150');
const STM32_PORT = parseInt(getArg('--port', process.env.STM32_PORT || '5002'), 10);
const HTTP_PORT = parseInt(getArg('--http', process.env.HTTP_PORT || '8080'), 10);

const MAGIC_DSP2 = 0x44535032; // 'DSP2'
const MAGIC_SPEC = 0x53504543; // 'SPEC'
const TELEMETRY_SIZE = 144;
const SPEC_HDR_SIZE = 12;

const PUBLIC_DIR = path.join(__dirname, 'public');

const MIME_TYPES = {
  '.html': 'text/html; charset=utf-8',
  '.css': 'text/css; charset=utf-8',
  '.js': 'application/javascript; charset=utf-8',
  '.json': 'application/json; charset=utf-8',
  '.png': 'image/png',
  '.svg': 'image/svg+xml',
  '.ico': 'image/x-icon'
};

// --- Global State & Connected SSE Clients ---
const sseClients = new Set();
let latestState = {
  connected: false,
  lastUpdateMs: 0,
  telemetry: null,
  spectrum: new Array(2048).fill(0),
  spectrumBlockId: 0
};

let specChunksBuffer = [null, null, null, null];
let specCurrentBlock = -1;

// --- UDP Socket Setup ---
const udpClient = dgram.createSocket('udp4');

function sendStm32Command(cmd) {
  const msg = Buffer.from(cmd, 'ascii');
  udpClient.send(msg, 0, msg.length, STM32_PORT, STM32_IP, (err) => {
    if (err) {
      console.error(`[UDP Error] Failed to send "${cmd}":`, err.message);
    }
  });
}

// --- Decode 144-byte Telemetry Packet (`UDP_DSP_TelemetryPacket_t`) ---
function parseTelemetry(buf) {
  if (buf.length < TELEMETRY_SIZE) return null;
  const magic = buf.readUInt32LE(0);
  if (magic !== MAGIC_DSP2) return null;

  const version = buf.readUInt8(4);
  const msg_type = buf.readUInt8(5);
  const seq = buf.readUInt16LE(6);
  const block_id = buf.readUInt32LE(8);
  const fs_hz = buf.readUInt32LE(12);
  const block_size = buf.readUInt16LE(16);
  const dc_offset_mv = buf.readUInt16LE(18);
  const v_pp_mv = buf.readUInt16LE(20);
  const f0_hz_x10 = buf.readUInt32LE(22);
  const f0_mag_mv = buf.readUInt16LE(26);
  const noise_floor_mv = buf.readUInt16LE(28);
  const snr_db_x10 = buf.readInt16LE(30);
  const thd_pct_x10 = buf.readUInt16LE(32);

  const harmonics = [];
  for (let i = 0; i < 10; i++) {
    harmonics.push(buf.readUInt16LE(34 + i * 2));
  }

  const raw_preview = [];
  for (let i = 0; i < 4; i++) {
    raw_preview.push(buf.readUInt16LE(54 + i * 2));
  }

  const overrun_count = buf.readUInt32LE(62);

  let wpt = null;
  if (buf.length >= 144) {
    const wpt_num_bands = buf.readUInt16LE(66);
    const energy_pct = [];
    const rms_mv = [];
    for (let b = 0; b < 16; b++) {
      energy_pct.push(+(buf.readUInt16LE(68 + b * 2) / 10.0).toFixed(1));
      rms_mv.push(buf.readUInt16LE(100 + b * 2));
    }
    const transient_mag_mv = buf.readUInt16LE(132);
    const transient_sample = buf.readUInt16LE(134);
    const transient_count = buf.readUInt32LE(136);
    const crest_hf = +(buf.readUInt16LE(140) / 10.0).toFixed(1);
    const entropy_norm = +(buf.readUInt16LE(142) / 1000.0).toFixed(3);

    wpt = {
      num_bands: wpt_num_bands,
      energy_pct,
      rms_mv,
      transient_mag_mv,
      transient_sample,
      transient_time_ms: +((transient_sample / (fs_hz || 25600)) * 1000.0).toFixed(2),
      transient_count,
      crest_hf,
      entropy_norm,
      transient_alert: (crest_hf > 4.5 && transient_mag_mv > 35)
    };
  }

  const f0_hz = f0_hz_x10 / 10.0;
  const snr_db = snr_db_x10 / 10.0;
  const thd_pct = Math.min(100.0, thd_pct_x10 / 10.0);

  return {
    version,
    msg_type,
    seq,
    block_id,
    fs_hz,
    block_size,
    dc_offset_mv,
    v_pp_mv,
    f0_hz,
    f0_mag_mv,
    noise_floor_mv,
    snr_db,
    thd_pct,
    harmonics,
    raw_preview,
    overrun_count,
    wpt,
    detected: f0_hz > 0 && f0_mag_mv > 0
  };
}

// --- Decode 1036-byte Spectrum Chunk (`UDP_DSP_SpectrumChunkHeader_t`) ---
function parseSpectrumChunk(buf) {
  if (buf.length < SPEC_HDR_SIZE) return;
  const magic = buf.readUInt32LE(0);
  if (magic !== MAGIC_SPEC) return;

  const blockId = buf.readUInt32LE(4);
  const chunkIdx = buf.readUInt8(8);
  const totalChunks = buf.readUInt8(9);
  const binsInChunk = buf.readUInt16LE(10);

  if (blockId !== specCurrentBlock) {
    specCurrentBlock = blockId;
    specChunksBuffer = new Array(totalChunks).fill(null);
  }

  const chunkData = new Array(binsInChunk);
  for (let i = 0; i < binsInChunk; i++) {
    const offset = SPEC_HDR_SIZE + i * 2;
    if (offset + 2 <= buf.length) {
      chunkData[i] = +(buf.readUInt16LE(offset) / 10.0).toFixed(1);
    } else {
      chunkData[i] = 0;
    }
  }

  specChunksBuffer[chunkIdx] = chunkData;

  if (specChunksBuffer.every((c) => c !== null)) {
    const fullSpec = [].concat(...specChunksBuffer);
    latestState.spectrum = fullSpec;
    latestState.spectrumBlockId = blockId;
    broadcastSSE('spectrum', {
      block_id: blockId,
      bins: fullSpec
    });
  }
}

// --- Broadcast Event to All Connected Browsers via SSE ---
function broadcastSSE(eventType, payload) {
  const msg = `event: ${eventType}\ndata: ${JSON.stringify(payload)}\n\n`;
  for (const res of sseClients) {
    res.write(msg);
  }
}

// --- Incoming UDP Message Handler ---
udpClient.on('message', (msg) => {
  latestState.connected = true;
  latestState.lastUpdateMs = Date.now();

  if (msg.length >= 4) {
    const magic = msg.readUInt32LE(0);
    if (magic === MAGIC_DSP2 && msg.length >= TELEMETRY_SIZE) {
      const tel = parseTelemetry(msg);
      if (tel) {
        latestState.telemetry = tel;
        broadcastSSE('telemetry', tel);
      }
      return;
    }
    if (magic === MAGIC_SPEC) {
      parseSpectrumChunk(msg);
      return;
    }
  }
});

udpClient.on('error', (err) => {
  console.error('[UDP Socket Error]:', err.message);
});

udpClient.bind(0, () => {
  console.log(`[UDP] Listening on ephemeral local port ${udpClient.address().port}`);
  console.log(`[UDP] Subscribing to STM32H755ZI at ${STM32_IP}:${STM32_PORT}...`);
  sendStm32Command('STREAM');
  setTimeout(() => sendStm32Command('SPECTRUM'), 200);
});

let streamPaused = false;

// --- Periodic Keep-Alive & Spectrum Polling ---
setInterval(() => {
  if (!streamPaused) {
    sendStm32Command('SPECTRUM');
  }

  if (Date.now() - latestState.lastUpdateMs > 2500) {
    if (latestState.connected) {
      latestState.connected = false;
      broadcastSSE('status', { connected: false });
    }
    if (!streamPaused) {
      sendStm32Command('STREAM');
    }
  }
}, 500);

// --- HTTP Server (Static Assets + SSE + REST API) ---
const server = http.createServer((req, res) => {
  if (req.url === '/events') {
    res.writeHead(200, {
      'Content-Type': 'text/event-stream',
      'Cache-Control': 'no-cache, no-transform',
      'Connection': 'keep-alive',
      'Access-Control-Allow-Origin': '*'
    });

    if (latestState.telemetry) {
      res.write(`event: telemetry\ndata: ${JSON.stringify(latestState.telemetry)}\n\n`);
    }
    if (latestState.spectrumBlockId > 0) {
      res.write(`event: spectrum\ndata: ${JSON.stringify({ block_id: latestState.spectrumBlockId, bins: latestState.spectrum })}\n\n`);
    }

    sseClients.add(res);
    req.on('close', () => sseClients.delete(res));
    return;
  }

  if (req.url === '/api/status') {
    res.writeHead(200, { 'Content-Type': 'application/json' });
    res.end(JSON.stringify(latestState));
    return;
  }

  if (req.url === '/api/control' && req.method === 'POST') {
    let body = '';
    req.on('data', (chunk) => { body += chunk; });
    req.on('end', () => {
      try {
        const data = JSON.parse(body);
        if (data.cmd === 'STOP') {
          streamPaused = true;
          sendStm32Command('STOP');
        } else if (data.cmd === 'STREAM') {
          streamPaused = false;
          sendStm32Command('STREAM');
        } else if (data.cmd === 'FS' && data.fs_hz) {
          streamPaused = false;
          sendStm32Command(`FS ${parseInt(data.fs_hz, 10)}`);
        } else if (data.cmd === 'SPECTRUM') {
          sendStm32Command('SPECTRUM');
        }
        res.writeHead(200, { 'Content-Type': 'application/json' });
        res.end(JSON.stringify({ ok: true, paused: streamPaused }));
      } catch (e) {
        res.writeHead(400, { 'Content-Type': 'application/json' });
        res.end(JSON.stringify({ ok: false, error: e.message }));
      }
    });
    return;
  }

  const safePath = path.normalize(req.url === '/' ? '/index.html' : req.url).replace(/^(\.\.[\/\\])+/, '');
  const filePath = path.join(PUBLIC_DIR, safePath);

  if (!filePath.startsWith(PUBLIC_DIR)) {
    res.writeHead(403, { 'Content-Type': 'text/plain' });
    res.end('403 Forbidden');
    return;
  }

  fs.readFile(filePath, (err, content) => {
    if (err) {
      res.writeHead(err.code === 'ENOENT' ? 404 : 500, { 'Content-Type': 'text/plain' });
      res.end(err.code === 'ENOENT' ? '404 Not Found' : '500 Internal Server Error');
      return;
    }
    const ext = path.extname(filePath).toLowerCase();
    const contentType = MIME_TYPES[ext] || 'application/octet-stream';
    res.writeHead(200, {
      'Content-Type': contentType,
      'Cache-Control': 'no-cache'
    });
    res.end(content);
  });
});

function getLocalWiFiIPs() {
  const nets = os.networkInterfaces();
  const results = [];
  for (const name of Object.keys(nets)) {
    for (const net of nets[name]) {
      if (net.family === 'IPv4' && !net.internal) {
        results.push({ name, address: net.address });
      }
    }
  }
  return results;
}

server.listen(HTTP_PORT, '0.0.0.0', () => {
  console.log('===================================================================');
  console.log('  STM32H755ZI DSP Web Dashboard (4096-pt FFT + 16-Band db4 WPT)');
  console.log('===================================================================');
  console.log(`  Local browser URL  : http://localhost:${HTTP_PORT}`);
  const ips = getLocalWiFiIPs();
  for (const item of ips) {
    console.log(`  LAN / Mobile URL   : http://${item.address}:${HTTP_PORT}  [${item.name}]`);
  }
  console.log(`  Target Board UDP   : ${STM32_IP}:${STM32_PORT}`);
  console.log('===================================================================');
});
