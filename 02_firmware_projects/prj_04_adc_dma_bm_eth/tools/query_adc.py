#!/usr/bin/env python
"""
Queries real-time ADC telemetry from a running STM32H755ZI dual-core board.

Sends UDP datagrams to port 5001 (NET_ADC_PORT) and parses either plain-text
or JSON responses containing ADC1+DMA1 readings collected by Cortex-M4.

This script lives inside the project, NOT in <workspace>/tools/, and it has no
shim. It is not a workspace tool: it decodes this firmware's application
protocol - the JSON key set and the versioned binary WAVE header - both of which
are defined in CM7/App/Src/adc_server.c a few directories away. Move it up to
the workspace and that contract spans two trees, where nothing keeps the two
ends in step. See "Where a script belongs" in the workspace README.

Usage:
    py tools/query_adc.py [--host 192.168.1.150] [--port 5001] [--count 10] [--json]
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

# Byte-identical with WaveformHeader_t in CM7/App/Src/adc_server.c. The two must
# change together; that is why this file lives in the project and not in
# <workspace>/tools/ (see "Where a script belongs" in the workspace README).
#
# Contract: `version` covers the layout of the core fields below. Appending new
# fields only grows `header_len` and leaves `version` alone, so a parser that
# locates the samples at `header_len` - as this one does - keeps working against
# newer firmware. Changing or reordering an existing field bumps `version`.
#
# Conversely, only the core is required: a field appended later is read when
# `header_len` reaches it and reported as absent when it does not, so this tool
# still works against firmware older than itself.
WAVE_CORE_FMT = "<IHHIHHIHHHHIIIIII"   # fields present since version 1
WAVE_CORE_LEN = 52
assert struct.calcsize(WAVE_CORE_FMT) == WAVE_CORE_LEN

WAVE_OFF_ADC_OVR = 52                  # uint32 adc_overrun_count, appended after v1
WAVE_LEN_WITH_ADC_OVR = 56


def print_ascii_oscillogram(samples_mv: list[float], rate_hz: int, seq: int) -> None:
    """Renders a text-based DSO oscillogram in the terminal."""
    if not samples_mv:
        return

    n = len(samples_mv)
    v_min = min(samples_mv)
    v_max = max(samples_mv)
    v_pp = v_max - v_min

    # Calculate frequency by rising edge crossings across the midpoint
    v_mid = (v_max + v_min) / 2.0
    rising_edges = []
    for i in range(1, n):
        if samples_mv[i - 1] < v_mid <= samples_mv[i]:
            rising_edges.append(i)

    freq_str = "N/A"
    duty_str = "N/A"
    if len(rising_edges) >= 2:
        periods = [rising_edges[k] - rising_edges[k - 1] for k in range(1, len(rising_edges))]
        avg_period_samples = sum(periods) / len(periods)
        if avg_period_samples > 0:
            freq_hz = rate_hz / avg_period_samples
            freq_str = f"{freq_hz:8.2f} Hz"

        # Calculate duty cycle on first complete period
        p_start = rising_edges[0]
        p_end = rising_edges[1]
        high_samples = sum(1 for s in samples_mv[p_start:p_end] if s >= v_mid)
        duty_pct = (high_samples / (p_end - p_start)) * 100.0
        duty_str = f"{duty_pct:5.1f} %"

    ts_us = (1.0 / rate_hz) * 1e6
    total_us = n * ts_us

    print("=" * 78)
    print(f" [DSO OSCILLOGRAM] Seq: {seq} | Rate: {rate_hz:,} SPS (Ts: {ts_us:.1f} us) | Samples: {n} ({total_us:.1f} us)")
    print(f" Vmin: {v_min:6.1f} mV | Vmax: {v_max:6.1f} mV | Vpp: {v_pp:6.1f} mV | Freq: {freq_str} | Duty: {duty_str}")
    print("=" * 78)

    # Plot 16 rows x 64 columns
    rows = 14
    cols = 64
    grid = [[" " for _ in range(cols)] for _ in range(rows)]

    # Downsample or window to 64 points
    step = n / cols
    plot_vals = [samples_mv[int(i * step)] for i in range(cols)]

    v_range = v_max - v_min if (v_max - v_min) > 50 else 100.0
    for col, val in enumerate(plot_vals):
        r = int(((v_max - val) / v_range) * (rows - 1))
        r = max(0, min(rows - 1, r))
        grid[r][col] = "#"

    for r in range(rows):
        v_level = v_max - (r / (rows - 1)) * v_range
        line = "".join(grid[r])
        print(f"{v_level:6.0f} mV | {line} |")
    print(" " * 9 + "+" + "-" * cols + "+")
    print(f"         0 us" + " " * (cols - 16) + f"{total_us:.0f} us\n")


def query_adc(host: str, port: int, count: int, mode: str, interval: float) -> int:
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.settimeout(DEFAULT_TIMEOUT_S)

    if mode == "wave":
        cmd = b"WAVE"
    elif mode == "json":
        cmd = b"JSON"
    else:
        cmd = b"GET"

    print(f"[*] Querying ADC telemetry from {host}:{port} (Mode: {mode.upper()})...")

    prev_samples = None
    prev_time = None
    success_count = 0

    i = 0
    while True:
        if count > 0 and i >= count:
            break
        i += 1

        now = time.time()
        try:
            # Drain any stale pending response before sending
            while True:
                try:
                    sock.setblocking(False)
                    sock.recv(4096)
                except (BlockingIOError, socket.error):
                    break
            sock.setblocking(True)
            sock.settimeout(DEFAULT_TIMEOUT_S)

            sock.sendto(cmd, (host, port))
            data, _ = sock.recvfrom(2048)
            success_count += 1

            if mode == "wave":
                err = None
                if len(data) < WAVE_CORE_LEN:
                    err = f"short WAVE packet ({len(data)} B)"
                else:
                    fields = struct.unpack(WAVE_CORE_FMT, data[:WAVE_CORE_LEN])
                    magic, version, header_len, rate_hz, num_samples, vref_mv, seq = fields[:7]
                    end = header_len + num_samples * 2
                    if magic != WAVE_MAGIC:
                        err = f"not a WAVE packet (magic 0x{magic:08X})"
                    elif version != WAVE_VERSION:
                        err = (f"firmware speaks WAVE v{version}, this tool speaks "
                               f"v{WAVE_VERSION} - reflash or update the tool")
                    elif header_len < WAVE_CORE_LEN:
                        err = f"header_len {header_len} below the known {WAVE_CORE_LEN}"
                    elif len(data) < end:
                        err = f"truncated: need {end} B for {num_samples} samples, got {len(data)}"
                    else:
                        raw_samples = struct.unpack(f"<{num_samples}H", data[header_len:end])
                        samples_mv = [(s * vref_mv) / 65535.0 for s in raw_samples]
                        print_ascii_oscillogram(samples_mv, rate_hz, seq)
                if err is not None:
                    print(f"[{i:04d}] ERROR: {err}", file=sys.stderr)

            elif mode == "json":
                text = data.decode("utf-8", errors="replace").strip()
                try:
                    payload = json.loads(text)
                    samples = payload.get("samples", 0)
                    measured_rate = ""
                    if prev_samples is not None and prev_time is not None:
                        dt = now - prev_time
                        ds = samples - prev_samples
                        if dt > 0:
                            measured_rate = f" | Measured Rate: {ds / dt:8.1f} Hz"
                    prev_samples = samples
                    prev_time = now

                    print(
                        f"[{i:04d}] CH{payload.get('ch', 0)}: {payload.get('mv', 0):4d} mV (raw: {payload.get('raw', 0):5d}, "
                        f"avg: {payload.get('avg', 0):5d}, min: {payload.get('min', 0):5d}, max: {payload.get('max', 0):5d}) | "
                        f"Rate: {payload.get('rate', 0)} Hz | Seq: {payload.get('seq', 0)} | Samples: {samples} | TC: {payload.get('tc', 0)} | OVR: {payload.get('ovr', 0)} | M4 HB: {payload.get('m4_hb', 0)}"
                        f"{measured_rate}"
                    )
                except json.JSONDecodeError:
                    print(f"[{i:04d}] RAW: {text}")
            else:
                text = data.decode("utf-8", errors="replace").strip()
                print(f"[{i:04d}] {text}")

        except socket.timeout:
            print(f"[{i:04d}] ERROR: Request timed out after {DEFAULT_TIMEOUT_S}s", file=sys.stderr)
        except OSError as e:
            print(f"[{i:04d}] NETWORK ERROR: {e}", file=sys.stderr)

        if count == 0 or i < count:
            time.sleep(interval)

    sock.close()
    if count > 0 and success_count == 0:
        return 1
    return 0


def main() -> None:
    parser = argparse.ArgumentParser(description="Query ADC telemetry over Ethernet UDP.")
    parser.add_argument("--host", default=DEFAULT_HOST, help=f"Target board IP address (default: {DEFAULT_HOST})")
    parser.add_argument("--port", type=int, default=DEFAULT_PORT, help=f"Target UDP port (default: {DEFAULT_PORT})")
    parser.add_argument("--count", type=int, default=10, help="Number of queries (0 = run continuously, default: 10)")
    parser.add_argument("--rate", type=float, default=10.0, help="Query rate in Hz (default: 10.0)")
    parser.add_argument("--json", action="store_true", help="Request JSON response format")
    parser.add_argument("--wave", action="store_true", help="Request binary waveform snapshot and render DSO oscillogram")

    args = parser.parse_args()
    interval = 1.0 / max(args.rate, 0.1)

    if args.wave:
        mode = "wave"
    elif args.json:
        mode = "json"
    else:
        mode = "text"

    ret = query_adc(args.host, args.port, args.count, mode, interval)
    sys.exit(ret)


if __name__ == "__main__":
    main()
