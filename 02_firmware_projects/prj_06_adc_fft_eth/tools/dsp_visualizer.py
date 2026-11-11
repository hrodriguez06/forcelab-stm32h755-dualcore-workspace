#!/usr/bin/env python3
"""
Real-Time UDP DSP Visualizer & CLI Client for STM32H755ZI (`prj_06_adc_fft_eth`)
=================================================================================
Connects to the Cortex-M7 FreeRTOS + lwIP UDP server on port 5002 (default IP
192.168.1.150) to control and inspect the dual-core 4096-point Blackman-Harris
FFT and 4-Level Daubechies-4 (db4) Wavelet Packet Transform (16 sub-bands).

Usage examples:
  python tools/dsp_visualizer.py              # Print STATUS and start live stream
  python tools/dsp_visualizer.py --status     # Print one-shot STATUS summary
  python tools/dsp_visualizer.py --json       # Print one-shot JSON telemetry
  python tools/dsp_visualizer.py --stream     # Live 144-byte binary stream in console
  python tools/dsp_visualizer.py --spectrum   # Fetch 2048-bin segmented FFT spectrum
  python tools/dsp_visualizer.py --wave       # Fetch 512-sample raw ADC waveform
  python tools/dsp_visualizer.py --fs 50000   # Set sampling rate to 50 kSPS
  python tools/dsp_visualizer.py --plot       # Launch live Matplotlib dashboard
"""

from __future__ import annotations

import argparse
import socket
import struct
import sys
import time

DEFAULT_IP = "192.168.1.150"
DEFAULT_PORT = 5002

DSP2_MAGIC = 0x44535032  # 'DSP2'
SPEC_MAGIC = 0x53504543  # 'SPEC'
WAVE_MAGIC = 0x57415645  # 'WAVE'

TELEMETRY_FMT = "<IBBHIIHHHIHHhH10H4HIH16H16HHHIHH"
TELEMETRY_SIZE = 144

SPECTRUM_HDR_FMT = "<IIBBH"
SPECTRUM_HDR_SIZE = 12

WAVE_HDR_FMT = "<IHHIHHIHHHHIIIIIII"
WAVE_HDR_SIZE = 56


def decode_telemetry(pkt: bytes) -> dict | None:
    """Unpack a 144-byte `UDP_DSP_TelemetryPacket_t` frame."""
    if len(pkt) < TELEMETRY_SIZE:
        return None
    fields = struct.unpack(TELEMETRY_FMT, pkt[:TELEMETRY_SIZE])
    if fields[0] != DSP2_MAGIC:
        return None

    return {
        "magic": fields[0],
        "version": fields[1],
        "msg_type": fields[2],
        "seq": fields[3],
        "block_id": fields[4],
        "fs_hz": fields[5],
        "block_size": fields[6],
        "dc_offset_mv": fields[7],
        "v_pp_mv": fields[8],
        "f0_hz": fields[9] / 10.0,
        "f0_mag_mv": fields[10],
        "noise_floor_mv": fields[11],
        "snr_db": fields[12] / 10.0,
        "thd_pct": fields[13] / 10.0,
        "harmonics_mv": list(fields[14:24]),
        "raw_preview": list(fields[24:28]),
        "overrun_count": fields[28],
        "wpt_num_bands": fields[29],
        "wpt_energy_pct": [x / 10.0 for x in fields[30:46]],
        "wpt_rms_mv": list(fields[46:62]),
        "wpt_transient_mag_mv": fields[62],
        "wpt_transient_sample": fields[63],
        "wpt_transient_count": fields[64],
        "wpt_crest_factor": fields[65] / 10.0,
        "wpt_entropy_norm": fields[66] / 1000.0,
    }


def query_ascii(sock: socket.socket, addr: tuple[str, int], cmd: str) -> str:
    """Send an ASCII command and return the text response."""
    sock.sendto(cmd.encode("ascii"), addr)
    deadline = time.monotonic() + 2.0
    while time.monotonic() < deadline:
        data, _ = sock.recvfrom(4096)
        # Skip binary STREAM packets if the board was already streaming
        if len(data) == TELEMETRY_SIZE and struct.unpack_from("<I", data, 0)[0] == DSP2_MAGIC:
            continue
        return data.decode("utf-8", errors="replace")
    raise TimeoutError(f"Timed out waiting for ASCII reply to '{cmd}' from {addr[0]}:{addr[1]}")


def fetch_spectrum(sock: socket.socket, addr: tuple[str, int]) -> tuple[int, list[float]]:
    """Request the 4-chunk 2048-bin FFT spectrum (`SPECTRUM` command)."""
    sock.sendto(b"SPECTRUM", addr)
    bins_mv = [0.0] * 2048
    received_chunks = set()
    block_id = 0

    deadline = time.monotonic() + 2.0
    while len(received_chunks) < 4 and time.monotonic() < deadline:
        pkt, _ = sock.recvfrom(2048)
        if len(pkt) < SPECTRUM_HDR_SIZE:
            continue
        magic, blk_id, chunk_idx, total_chunks, n_bins = struct.unpack(
            SPECTRUM_HDR_FMT, pkt[:SPECTRUM_HDR_SIZE]
        )
        if magic != SPEC_MAGIC:
            continue
        block_id = blk_id
        payload = pkt[SPECTRUM_HDR_SIZE : SPECTRUM_HDR_SIZE + n_bins * 2]
        vals = struct.unpack(f"<{n_bins}H", payload)
        base = chunk_idx * 512
        for i, v in enumerate(vals):
            if base + i < len(bins_mv):
                bins_mv[base + i] = v / 10.0
        received_chunks.add(chunk_idx)

    return block_id, bins_mv


def fetch_wave(sock: socket.socket, addr: tuple[str, int]) -> tuple[dict, list[int]]:
    """Request a 512-sample raw ADC waveform snapshot (`WAVE` command)."""
    sock.sendto(b"WAVE", addr)
    deadline = time.monotonic() + 2.0
    while time.monotonic() < deadline:
        pkt, _ = sock.recvfrom(2048)
        if len(pkt) < WAVE_HDR_SIZE:
            continue
        magic = struct.unpack_from("<I", pkt, 0)[0]
        if magic != WAVE_MAGIC:
            continue
        hdr = struct.unpack(WAVE_HDR_FMT, pkt[:WAVE_HDR_SIZE])
        n_samples = hdr[4]
        samples = list(
            struct.unpack(
                f"<{n_samples}H",
                pkt[WAVE_HDR_SIZE : WAVE_HDR_SIZE + n_samples * 2],
            )
        )
        meta = {
            "sampling_rate_hz": hdr[3],
            "num_samples": n_samples,
            "vref_mv": hdr[5],
            "seq": hdr[6],
            "min_raw": hdr[7],
            "max_raw": hdr[8],
            "avg_raw": hdr[9],
            "channel": hdr[10],
            "millivolts": hdr[11],
        }
        return meta, samples
    raise TimeoutError(f"Timed out waiting for WAVE packet from {addr[0]}:{addr[1]}")


def run_cli_stream(sock: socket.socket, addr: tuple[str, int]) -> None:
    """Subscribe to real-time 144-byte `DSP2` frames and print telemetry."""
    print(f"\nSubscribing to UDP stream at {addr[0]}:{addr[1]} (Press Ctrl+C to stop)...")
    sock.sendto(b"STREAM", addr)
    try:
        while True:
            try:
                pkt, _ = sock.recvfrom(2048)
            except socket.timeout:
                sock.sendto(b"STREAM", addr)
                continue

            if len(pkt) == TELEMETRY_SIZE:
                t = decode_telemetry(pkt)
                if t:
                    top_band = max(
                        range(16), key=lambda b: t["wpt_energy_pct"][b]
                    )
                    print(
                        f"[Blk {t['block_id']:5d}] Fs={t['fs_hz']} Hz | "
                        f"DC={t['dc_offset_mv']:4d} mV Vpp={t['v_pp_mv']:4d} mV | "
                        f"f0={t['f0_hz']:7.1f} Hz ({t['f0_mag_mv']:4d} mV) "
                        f"SNR={t['snr_db']:5.1f} dB THD={t['thd_pct']:5.1f}% | "
                        f"WPT B{top_band}={t['wpt_energy_pct'][top_band]:5.1f}% "
                        f"H={t['wpt_entropy_norm']:.3f} "
                        f"Crest={t['wpt_crest_factor']:.1f} "
                        f"Glitches={t['wpt_transient_count']}"
                    )
            else:
                text = pkt.decode("utf-8", errors="ignore").strip()
                if text:
                    print(f"[Board] {text}")
    except KeyboardInterrupt:
        print("\nStopping stream...")
        sock.sendto(b"STOP", addr)


def run_matplotlib_plot(sock: socket.socket, addr: tuple[str, int]) -> None:
    """Open a live 3-panel Matplotlib dashboard (FFT Spectrum, Harmonics, 16-Band WPT)."""
    try:
        import matplotlib.pyplot as plt
        from matplotlib.animation import FuncAnimation
    except ImportError:
        sys.exit("ERROR: matplotlib is required for --plot (`pip install matplotlib`).")

    sock.sendto(b"STREAM", addr)
    fig, (ax_spec, ax_wpt, ax_harm) = plt.subplots(3, 1, figsize=(11, 8))
    fig.suptitle("STM32H755ZI Real-Time 4096-pt FFT + 16-Band db4 WPT Analyzer")

    last_telemetry: dict | None = None
    last_spec_time = 0.0
    spec_bins = [0.0] * 2048

    def update(_frame):
        nonlocal last_telemetry, last_spec_time, spec_bins
        while True:
            try:
                pkt, _ = sock.recvfrom(2048)
                t = decode_telemetry(pkt)
                if t:
                    last_telemetry = t
            except socket.timeout:
                break

        now = time.monotonic()
        if now - last_spec_time >= 0.5:
            last_spec_time = now
            try:
                _, spec_bins = fetch_spectrum(sock, addr)
            except Exception:
                pass

        if not last_telemetry:
            return

        fs = last_telemetry["fs_hz"] or 25600
        df = fs / 4096.0
        freqs = [i * df for i in range(len(spec_bins))]

        ax_spec.clear()
        ax_spec.plot(freqs, spec_bins, color="#0284c7", linewidth=1.1)
        ax_spec.set_xlim(0, fs / 2.0)
        ax_spec.set_ylabel("Magnitude (mV)")
        ax_spec.set_title(
            f"4096-pt Blackman-Harris FFT | f0 = {last_telemetry['f0_hz']:.1f} Hz "
            f"({last_telemetry['f0_mag_mv']} mV) | SNR = {last_telemetry['snr_db']:.1f} dB | "
            f"THD = {last_telemetry['thd_pct']:.1f}%"
        )
        ax_spec.grid(True, alpha=0.3)

        ax_wpt.clear()
        bands = [f"B{i}" for i in range(16)]
        ax_wpt.bar(bands, last_telemetry["wpt_energy_pct"], color="#8b5cf6")
        ax_wpt.set_ylim(0, 100)
        ax_wpt.set_ylabel("Energy (%)")
        ax_wpt.set_title(
            f"4-Level Daubechies-4 (db4) WPT (16 Sub-Bands) | "
            f"Normalized Entropy = {last_telemetry['wpt_entropy_norm']:.3f} | "
            f"HF Crest = {last_telemetry['wpt_crest_factor']:.1f} | "
            f"Glitches = {last_telemetry['wpt_transient_count']}"
        )
        ax_wpt.grid(True, axis="y", alpha=0.3)

        ax_harm.clear()
        h_labels = [f"H{i}" for i in range(1, 11)]
        ax_harm.bar(h_labels, last_telemetry["harmonics_mv"], color="#10b981")
        ax_harm.set_ylabel("Amplitude (mV)")
        ax_harm.set_title("Harmonic Profile (H1 .. H10)")
        ax_harm.grid(True, axis="y", alpha=0.3)

        fig.tight_layout()

    _anim = FuncAnimation(fig, update, interval=150, cache_frame_data=False)
    plt.show()


def main() -> None:
    parser = argparse.ArgumentParser(
        description="UDP client and visualizer for prj_06_adc_fft_eth (:5002)"
    )
    parser.add_argument("--ip", default=DEFAULT_IP, help=f"Board IP (default: {DEFAULT_IP})")
    parser.add_argument("--port", type=int, default=DEFAULT_PORT, help=f"UDP port (default: {DEFAULT_PORT})")
    parser.add_argument("--status", action="store_true", help="Query human-readable STATUS summary")
    parser.add_argument("--json", action="store_true", help="Query single-line JSON telemetry")
    parser.add_argument("--spectrum", action="store_true", help="Fetch 2048-bin segmented FFT spectrum")
    parser.add_argument("--wave", action="store_true", help="Fetch 512-sample raw ADC waveform snapshot")
    parser.add_argument("--fs", type=int, metavar="HZ", help="Set sampling frequency in Hz (1000..100000)")
    parser.add_argument("--stop", action="store_true", help="Send STOP command")
    parser.add_argument("--stream", action="store_true", help="Stream live 144-byte telemetry frames in console")
    parser.add_argument("--plot", action="store_true", help="Launch live Matplotlib FFT + WPT visualizer")

    args = parser.parse_args()
    addr = (args.ip, args.port)

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.settimeout(1.5)

    try:
        if args.fs is not None:
            print(query_ascii(sock, addr, f"FS {args.fs}").strip())
            return
        if args.stop:
            print(query_ascii(sock, addr, "STOP").strip())
            return
        if args.json:
            print(query_ascii(sock, addr, "JSON").strip())
            return
        if args.wave:
            meta, samples = fetch_wave(sock, addr)
            print(f"WAVE Snapshot: {meta}")
            print(f"First 16 samples (raw 16-bit): {samples[:16]}")
            return
        if args.spectrum:
            blk_id, bins = fetch_spectrum(sock, addr)
            peak_bin = max(range(1, len(bins)), key=lambda i: bins[i])
            print(
                f"Spectrum Block #{blk_id}: 2048 bins received | "
                f"Peak bin={peak_bin} ({bins[peak_bin]:.1f} mV)"
            )
            return
        if args.plot:
            sock.settimeout(0.05)
            run_matplotlib_plot(sock, addr)
            return
        if args.status:
            print(query_ascii(sock, addr, "STATUS").strip())
            return

        # Default when invoked without flags (e.g. "Run Python File" in VS Code):
        # Print the current STATUS summary and enter the live STREAM loop.
        print(query_ascii(sock, addr, "STATUS").strip())
        run_cli_stream(sock, addr)

    except (socket.timeout, TimeoutError):
        sys.exit(
            f"ERROR: No UDP response from {addr[0]}:{addr[1]}.\n"
            f"  1. Ensure prj_06_adc_fft_eth is flashed (`py tools/flash.py`).\n"
            f"  2. Verify Ethernet link with `py tools/net_check.py`."
        )


if __name__ == "__main__":
    main()
