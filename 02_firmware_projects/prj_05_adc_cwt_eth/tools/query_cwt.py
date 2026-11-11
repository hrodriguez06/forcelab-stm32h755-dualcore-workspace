#!/usr/bin/env python
"""
Queries real-time CWT scalograms, AC/DC-coupled waveforms, and telemetry from a
running STM32H755ZI dual-core board (prj_05_adc_cwt_eth).

Sends UDP datagrams to port 5001 (NET_CWT_PORT) and parses:
  - CWT / SCAL: Chunked binary 2D Complex Morlet Continuous Wavelet Transform
    scalogram (24 scales x 512 time bins) + 512 raw ADC samples + 1.5 V DC
    offset telemetry.
  - WAVE: Binary 512-sample time-domain waveform packet (1088 bytes).
  - JSON / STAT: Plain-text or JSON telemetry including measured 1.5 V bias,
    DC offset error, peak-to-peak AC swing, and CM7 CWT DSP execution time.

This script lives inside the project, NOT in <workspace>/tools/, and it has no
shim. It decodes this firmware's application protocol - CwtPacketHeader_t and
WaveformHeader_t - both defined in CM7/App/Src/cwt_server.c.

Usage:
    py tools/query_cwt.py [--host 192.168.1.150] [--port 5001] [--count 5]
    py tools/query_cwt.py --wave
    py tools/query_cwt.py --json
    py tools/query_cwt.py --offset-mode auto
"""

from __future__ import annotations

import argparse
import json
import socket
import struct
import sys
import time

DEFAULT_HOST = "192.168.1.150"
DEFAULT_PORT = 5001
DEFAULT_TIMEOUT_S = 2.0

WAVE_MAGIC = 0x57415645  # 'WAVE'
WAVE_VERSION = 1
WAVE_HEADER_FMT = "<IHHIHHIHHHHIIIIIII"
WAVE_HEADER_LEN = 56
assert struct.calcsize(WAVE_HEADER_FMT) == WAVE_HEADER_LEN

CWT_MAGIC = 0x31545743   # 'CWT1' in little-endian
CWT_VERSION = 1
# Byte-identical with CwtPacketHeader_t in CM7/App/Inc/cwt_server.h (64 bytes):
#   uint32 magic, uint16 version, uint16 header_len,
#   uint32 frame_seq, uint16 chunk_idx, uint16 total_chunks,
#   uint32 sampling_rate_hz, uint16 num_scales, uint16 num_time_bins,
#   uint16 scale_offset, uint16 scales_in_chunk,
#   uint16 nominal_offset_mv, uint16 measured_dc_mv, uint16 vpp_mv,
#   uint16 min_raw, uint16 max_raw, uint16 average_raw,
#   uint32 adc_overrun_count, uint32 dma_error_count, uint32 cwt_exec_us,
#   float magnitude_scale,
#   int16 dc_offset_error_mv, uint16 peak_scale_idx,
#   uint32 m4_heartbeat
CWT_HEADER_FMT = "<IHHIHHIHHHHHHHHHHIIIfhHI"
CWT_HEADER_LEN = 64
assert struct.calcsize(CWT_HEADER_FMT) == CWT_HEADER_LEN


def unpack_cwt_header(data: bytes) -> dict:
    fields = struct.unpack(CWT_HEADER_FMT, data[:CWT_HEADER_LEN])
    return {
        "magic": fields[0],
        "version": fields[1],
        "header_len": fields[2],
        "frame_seq": fields[3],
        "chunk_idx": fields[4],
        "total_chunks": fields[5],
        "sampling_rate_hz": fields[6],
        "num_scales": fields[7],
        "num_time_bins": fields[8],
        "scale_offset": fields[9],
        "scales_in_chunk": fields[10],
        "nominal_offset_mv": fields[11],
        "measured_dc_mv": fields[12],
        "vpp_mv": fields[13],
        "min_raw": fields[14],
        "max_raw": fields[15],
        "average_raw": fields[16],
        "adc_overrun_count": fields[17],
        "dma_error_count": fields[18],
        "cwt_exec_us": fields[19],
        "magnitude_scale": fields[20],
        "dc_offset_error_mv": fields[21],
        "offset_mode": fields[22],
        "m4_heartbeat": fields[23],
    }


def fetch_cwt_frame(sock: socket.socket, host: str, port: int) -> dict | None:
    """Fetches a full 2D CWT frame using pipelined 4-chunk batch requests."""
    sock.sendto(b"CWT SNAP", (host, port))
    data, _ = sock.recvfrom(2048)
    if len(data) < CWT_HEADER_LEN:
        return None

    hdr0 = unpack_cwt_header(data)
    if hdr0["magic"] != CWT_MAGIC or hdr0["version"] != CWT_VERSION:
        return None

    total_chunks = hdr0["total_chunks"]
    num_scales = hdr0["num_scales"]
    num_time_bins = hdr0["num_time_bins"]
    h_len = hdr0["header_len"]

    payload0 = data[h_len:]
    freqs_bytes = num_scales * 4
    wave_bytes = num_time_bins * 2
    if len(payload0) < freqs_bytes + wave_bytes:
        return None

    f_center_hz = list(struct.unpack(f"<{num_scales}f", payload0[:freqs_bytes]))
    raw_waveform = list(
        struct.unpack(f"<{num_time_bins}H", payload0[freqs_bytes : freqs_bytes + wave_bytes])
    )

    scalogram: list[list[int] | None] = [None] * num_scales

    # Fetch scale chunks 1..num_scales in batches of 4 to respect ETH_TX_DESC_CNT=4
    batch_size = 4
    chunk_cursor = 1
    while chunk_cursor < total_chunks:
        count = min(batch_size, total_chunks - chunk_cursor)
        cmd = f"CWT B {chunk_cursor} {count}".encode("ascii")
        sock.sendto(cmd, (host, port))
        for _ in range(count):
            pkt, _ = sock.recvfrom(2048)
            if len(pkt) < CWT_HEADER_LEN:
                continue
            chdr = unpack_cwt_header(pkt)
            if chdr["magic"] != CWT_MAGIC:
                continue
            s_idx = chdr["scale_offset"]
            s_cnt = chdr["scales_in_chunk"]
            cpayload = pkt[chdr["header_len"] :]
            row_bytes = num_time_bins * 2
            for s in range(s_cnt):
                if (s_idx + s) < num_scales and len(cpayload) >= (s + 1) * row_bytes:
                    row = list(
                        struct.unpack(
                            f"<{num_time_bins}H",
                            cpayload[s * row_bytes : (s + 1) * row_bytes],
                        )
                    )
                    scalogram[s_idx + s] = row
        chunk_cursor += count

    # Fill any missing row with zeros if a UDP packet dropped
    for s in range(num_scales):
        if scalogram[s] is None:
            scalogram[s] = [0] * num_time_bins

    return {
        "header": hdr0,
        "f_center_hz": f_center_hz,
        "raw_waveform": raw_waveform,
        "scalogram": scalogram,
    }


def print_ascii_cwt_report(frame: dict) -> None:
    hdr = frame["header"]
    freqs = frame["f_center_hz"]
    raw = frame["raw_waveform"]
    scalogram = frame["scalogram"]

    n = hdr["num_time_bins"]
    num_scales = hdr["num_scales"]
    rate_hz = hdr["sampling_rate_hz"]
    mag_scale = hdr["magnitude_scale"]
    nom_mv = hdr["nominal_offset_mv"]
    dc_mv = hdr["measured_dc_mv"]
    dc_err = hdr["dc_offset_error_mv"]
    vpp_mv = hdr["vpp_mv"]
    mode_str = "AUTO_MEAN" if hdr["offset_mode"] == 0 else "FIXED_1V5"

    samples_mv = [(r * 3300.0) / 65535.0 for r in raw]
    ac_mv = [v - dc_mv for v in samples_mv]
    ac_min = min(ac_mv) if ac_mv else 0.0
    ac_max = max(ac_mv) if ac_mv else 0.0

    # Locate peak wavelet energy across (scale, time)
    peak_q = 0
    peak_scale = 0
    peak_time = 0
    for s_idx, row in enumerate(scalogram):
        row_max = max(row)
        if row_max > peak_q:
            peak_q = row_max
            peak_scale = s_idx
            peak_time = row.index(row_max)

    peak_mv = peak_q * mag_scale
    peak_freq_hz = freqs[peak_scale] if peak_scale < len(freqs) else 0.0
    ts_us = (1.0 / rate_hz) * 1e6
    total_us = n * ts_us

    print("=" * 86)
    print(
        f" [CWT SCALOGRAM & AC/DC TELEMETRY] Frame #{hdr['frame_seq']} | "
        f"Fs: {rate_hz:,} SPS | Window: {n} ({total_us:.1f} us)"
    )
    print(
        f" DC Bias: {dc_mv} mV (Nominal: {nom_mv} mV, Err: {dc_err:+d} mV) | "
        f"Offset Mode: {mode_str}"
    )
    print(
        f" AC Swing: [{ac_min:+6.1f}, {ac_max:+6.1f}] mV | Vpp: {vpp_mv} mV | "
        f"CWT DSP Time: {hdr['cwt_exec_us']} us | Ovr: {hdr['adc_overrun_count']}"
    )
    print(
        f" Dominant Wavelet Ridge: Scale #{peak_scale:02d} "
        f"({peak_freq_hz / 1000.0:6.2f} kHz) @ t={peak_time * ts_us:6.1f} us "
        f"| Peak |W|: {peak_mv:6.2f} mV"
    )
    print("=" * 86)

    # 1. Compact AC-coupled waveform trace (8 rows x 64 cols)
    cols = 64
    rows_wave = 9
    grid_w = [[" " for _ in range(cols)] for _ in range(rows_wave)]
    mid_row = rows_wave // 2
    for c in range(cols):
        grid_w[mid_row][c] = "-"  # 1.5V (0 mV AC) reference line

    step = n / cols
    span = max(abs(ac_max), abs(ac_min), 50.0)
    for c in range(cols):
        val = ac_mv[int(c * step)]
        r = int(((span - val) / (2.0 * span)) * (rows_wave - 1))
        r = max(0, min(rows_wave - 1, r))
        grid_w[r][c] = "#"

    print(" [AC-Coupled Waveform around 1.5 V Bias (mV)]")
    for r in range(rows_wave):
        lvl = span - (r / (rows_wave - 1)) * (2.0 * span)
        print(f" {lvl:+7.0f} mV | {''.join(grid_w[r])} |")
    print(" " * 12 + "+" + "-" * cols + "+")

    # 2. 2D CWT Scalogram Heatmap (24 scales x 64 time bins)
    palette = " .:-=+*#%@"
    n_pal = len(palette) - 1
    norm_denom = peak_q if peak_q > 0 else 1

    print("\n [2D Complex Morlet CWT Scalogram (Frequency 100 Hz .. 100 kHz vs. Time)]")
    for s_idx in range(num_scales):
        f_hz = freqs[s_idx]
        f_lbl = f"{f_hz:6.1f} Hz " if f_hz < 1000.0 else f"{f_hz / 1000.0:6.2f} kHz"
        row_data = scalogram[s_idx]
        chars = []
        for c in range(cols):
            t_start = int(c * step)
            t_end = max(t_start + 1, int((c + 1) * step))
            local_max = max(row_data[t_start:t_end])
            p_idx = int((local_max / norm_denom) * n_pal)
            p_idx = max(0, min(n_pal, p_idx))
            chars.append(palette[p_idx])
        marker = "<-- PEAK" if s_idx == peak_scale else ""
        print(f" {f_lbl} | {''.join(chars)} | {marker}")
    print(" " * 13 + "+" + "-" * cols + "+")
    print(f"             0 us" + " " * (cols - 16) + f"{total_us:.0f} us\n")


def print_ascii_wave(data: bytes) -> None:
    hdr = struct.unpack(WAVE_HEADER_FMT, data[:WAVE_HEADER_LEN])
    seq = hdr[3]
    rate_hz = hdr[6]
    num_samples = hdr[7]
    dc_mv = hdr[8]
    vpp_mv = hdr[9]
    nom_mv = hdr[10]
    h_len = hdr[2]
    raw = struct.unpack(f"<{num_samples}H", data[h_len : h_len + num_samples * 2])
    samples_mv = [(r * 3300.0) / 65535.0 for r in raw]
    print(
        f"[WAVE] Seq={seq} | Rate={rate_hz} SPS | Samples={num_samples} | "
        f"DC={dc_mv} mV (Nom {nom_mv} mV) | Vpp={vpp_mv} mV | "
        f"Min={min(samples_mv):.1f} mV | Max={max(samples_mv):.1f} mV"
    )


def run_query(
    host: str,
    port: int,
    count: int,
    mode: str,
    interval: float,
    offset_mode: str | None,
) -> int:
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.settimeout(DEFAULT_TIMEOUT_S)

    try:
        if offset_mode is not None:
            cmd = b"MODE AUTO" if offset_mode == "auto" else b"MODE FIXED"
            sock.sendto(cmd, (host, port))
            resp, _ = sock.recvfrom(1024)
            print(f"[MODE] {resp.decode('utf-8', errors='replace').strip()}")

        i = 0
        while count == 0 or i < count:
            t0 = time.perf_counter()
            if mode == "cwt":
                frame = fetch_cwt_frame(sock, host, port)
                if frame is None:
                    print("ERROR: Invalid or incomplete CWT frame", file=sys.stderr)
                else:
                    print_ascii_cwt_report(frame)
            elif mode == "wave":
                sock.sendto(b"WAVE", (host, port))
                data, _ = sock.recvfrom(2048)
                print_ascii_wave(data)
            elif mode == "json":
                sock.sendto(b"JSON", (host, port))
                data, _ = sock.recvfrom(2048)
                parsed = json.loads(data.decode("utf-8"))
                rtt_ms = (time.perf_counter() - t0) * 1000.0
                print(
                    f"[{i + 1:03d}] ({rtt_ms:5.2f} ms) "
                    + json.dumps(parsed, separators=(", ", ": "))
                )
            else:
                sock.sendto(b"STAT", (host, port))
                data, _ = sock.recvfrom(2048)
                rtt_ms = (time.perf_counter() - t0) * 1000.0
                print(
                    f"[{i + 1:03d}] ({rtt_ms:5.2f} ms) "
                    f"{data.decode('utf-8', errors='replace').strip()}"
                )

            i += 1
            if count == 0 or i < count:
                time.sleep(interval)

    except socket.timeout:
        print(
            f"ERROR: UDP timeout waiting for {host}:{port}. "
            "Check link and IP configuration.",
            file=sys.stderr,
        )
        return 1
    except KeyboardInterrupt:
        print("\nInterrupted by user.")
        return 0
    finally:
        sock.close()

    return 0


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Query CWT scalograms and 1.5 V biased AC/DC telemetry over UDP."
    )
    parser.add_argument("--host", default=DEFAULT_HOST, help=f"Board IP (default: {DEFAULT_HOST})")
    parser.add_argument("--port", type=int, default=DEFAULT_PORT, help=f"UDP port (default: {DEFAULT_PORT})")
    parser.add_argument("--count", type=int, default=5, help="Number of queries (0 = infinite)")
    parser.add_argument("--interval", type=float, default=0.35, help="Interval between queries in seconds")
    parser.add_argument("--json", action="store_true", help="Request JSON telemetry")
    parser.add_argument("--wave", action="store_true", help="Request raw 512-sample WAVE packet")
    parser.add_argument("--stat", action="store_true", help="Request plain-text STAT line")
    parser.add_argument(
        "--offset-mode",
        choices=["auto", "fixed"],
        default=None,
        help="Configure CM7 1.5V DC offset removal mode before querying",
    )
    args = parser.parse_args()

    mode = "cwt"
    if args.wave:
        mode = "wave"
    elif args.json:
        mode = "json"
    elif args.stat:
        mode = "stat"

    return run_query(
        args.host,
        args.port,
        args.count,
        mode,
        args.interval,
        args.offset_mode,
    )


if __name__ == "__main__":
    sys.exit(main())
