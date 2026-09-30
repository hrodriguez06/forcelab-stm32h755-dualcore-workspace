#!/usr/bin/env python3
"""Host side of the 00_gpio selftest on prj_01 (preset selftest-gpio).

Runs one complete bench session against the firmware of
CM7/App/Src/gpio_selftest.c with the Analog Discovery 2, and writes ONE capture
file: every console line and every host measurement, with timestamps, until
`HOST VERDICT=`. Wiring, safety rules and what each test proves:
01_shared_drivers/00_gpio/README.md.

    py tools/flash.py --preset selftest-gpio
    py tools/gpio_hw_check.py                   # resets the board through SWD and runs
    py tools/gpio_hw_check.py --allow-dirty     # development only: accept a -dirty Rev
                                                # (the capture then carries the git diff)
    py tools/gpio_hw_check.py --preset selftest-gpio   # build dir whose flags the build id uses

The image must be the one this checkout builds (R3-a, R3-b): the host computes
the build id of generated/fw_build_id.h over its own checkout (its commit
included) and build/<preset>, and aborts before any output is enabled when CM7
or CM4 prints another one (--allow-dirty notes it and goes on), or when the Rev
of CM7 without -dirty is not this checkout's HEAD (--allow-dirty does not
waive that one).

Electrical safety (no series resistors on the bench):
  S2  the AD2 opens and closes with every DIO in input (tools/ad2.py), and goes
      back there on any exception (try/finally).
  S3  every OutputEnable change goes through ad2.ad2_set_oe() with the bus
      owner of the CURRENT step; T0b(a) proves the guard with a mock DLL first.
  S5  before either side drives the bus, both MCU pulls must be followed
      (s5_released); the same function is used in T0b(c), where it must fail.
  S7  a step is acted on only if its announcement matches STEPS below.
  S8  no output is enabled before the banner `GPIO_BENCH v1 idle` of the
      right image has been read.
Standard library only (ctypes); `py`, not `python`.
"""

from __future__ import annotations

import argparse
import hashlib
import math
import os
import pathlib
import queue
import re
import subprocess
import sys
import threading
import time
import zlib
from typing import Callable, Optional

PROJECT = pathlib.Path(__file__).resolve().parents[1]
WORKSPACE = PROJECT.parents[1]
sys.path.insert(0, str(WORKSPACE / "tools"))
os.environ.setdefault("STM32_PROJECT_ROOT", str(PROJECT))

import ad2 as ad2mod                                   # noqa: E402
from ad2 import (AD2, AD2SafetyViolation, GPIO_BENCH_BUS_AD2,  # noqa: E402
                 GPIO_BENCH_BUS_MCU, GPIO_BENCH_BUS_NONE)
from monitor import SerialPort, autodetect             # noqa: E402

BENCH_VERSION = 1

# ---- Wiring (README "Bench") -------------------------------------------------
DIO_BUS = 0x00FF        # DIO0..7  <- PE8..PE15
DIO_PE3 = 1 << 8        # EXTI3 line, AD2 only (not driven in this bench)
DIO_PE5 = 1 << 9        # T8 line, MCU only
DIO_PE4 = 1 << 10       # EXTI4 line, AD2 only (not driven in this bench)
DIO_PE6 = 1 << 11       # MCU only, unused
DIO_REQ = 1 << 12       # HOST_REQ -> PE2
DIO_ACK = 1 << 13       # MCU_ACK  <- PE7
DIO_PA8 = 1 << 14       # MCO1     <- PA8
T0A_LINES = DIO_BUS | DIO_PE3 | DIO_PE5 | DIO_PE4 | DIO_PE6 | DIO_PA8

BUS = {"NONE": GPIO_BENCH_BUS_NONE, "MCU": GPIO_BENCH_BUS_MCU, "AD2": GPIO_BENCH_BUS_AD2}

# S7: the host's own copy of the firmware's step table (name, bus owner), in order.
STEPS = [
    ("T0A", "NONE"), ("T0B", "NONE"), ("T0B_S5NEG", "MCU"), ("T0B_TIMEOUT", "NONE"),
    ("T3_BM", "MCU"), ("T3_LL", "MCU"), ("T3_HAL", "MCU"),
    ("T4_BM", "AD2"), ("T4_LL", "AD2"), ("T4_HAL", "AD2"),
    ("T5T6", "NONE"),
    ("T8_BM", "NONE"), ("T8_LL", "NONE"), ("T8_HAL", "NONE"), ("T8_CTRL", "NONE"),
    ("T9", "NONE"),
    ("T10_BM", "NONE"), ("T10_LL", "NONE"), ("T10_HAL", "NONE"),
    ("DONE", "NONE"),
]
TIERS = ("BM", "LL", "HAL")

HS_TIMEOUT_S = 10.0

# T3: what DIO0..7 must show at each handshake, from the operations in the firmware.
T3_BASE = ([0x55, 0xAA, 0x00] + [1 << k for k in range(8)] + [0xFF] +
           [0xFF & ~(1 << k) for k in range(8)] + [0x80, 0x8F, 0xBF, 0x3E])
T3_BM_EXTRA = [0x3F, 0x55]          # set+reset of PE8 in one write (set wins); control: firmware writes 0x54

T5_RX_OHMS = 22000.0
T5_R_MIN, T5_R_MAX = 30e3, 50e3     # DS12923 table 59

T8_CASES = [("A", 1, "input PD -> output high"), ("B", 1, "input PU -> output low"),
            ("C", 0, "input PU -> output high, stale ODR 0"), ("D", 0, "input PD -> output low, stale ODR 1")]
T8_LA_RATE_HZ = 100e6
T8_LA_SAMPLES = 4096

T9_RATE_HZ = 100e6
T9_SAMPLES = 8192
T9_ROUNDS = ("BM", "LL", "HAL", "BM")
T9_SPEEDS = ("LOW", "MEDIUM", "HIGH", "VERY_HIGH")

T10_EXPECTED_HZ = 4e6               # MCO1 = HSE (8 MHz ST-LINK MCO) / 2
T10_TOLERANCE = 0.02


class BenchAbort(Exception):
    pass


# ---- Measurement helpers (the T8/T9/T10 methods of the prj_04 bench, unchanged) --------

def la_transitions(samples: list[int], bit: int) -> int:
    count, prev = 0, None
    for s in samples:
        v = (s >> bit) & 1
        if prev is not None and v != prev:
            count += 1
        prev = v
    return count


def la_frequency(samples: list[int], bit: int, rate_hz: float) -> tuple[float, int]:
    rises = [i for i in range(1, len(samples))
             if ((samples[i] >> bit) & 1) and not ((samples[i - 1] >> bit) & 1)]
    if len(rises) < 2:
        return 0.0, len(rises)
    return (len(rises) - 1) * rate_hz / (rises[-1] - rises[0]), len(rises)


def _lanczos(x: float, a: int) -> float:
    if x == 0.0:
        return 1.0
    if abs(x) >= a:
        return 0.0
    px = math.pi * x
    return a * math.sin(px) * math.sin(px / a) / (px * px)


def rise_time_10_90(v: list[float], rate_hz: float, pre_fraction: float,
                    upsample: int = 20, lanczos_a: int = 6, half_window: int = 24) -> Optional[float]:
    """10-90 % rise time in seconds, on a Lanczos (a = 6) sinc reconstruction of
    the edge upsampled 20x over +-24 samples around the 50 % crossing. Levels are
    the median of the first half of the pre-trigger samples and of the last 20 %.
    Same method as prj_04 (commit ededa84): at 100 MS/s a 15 ns edge spans under
    two samples, and a straight line between samples depends on the sampling phase."""
    n = len(v)
    pre = max(4, int(n * pre_fraction))
    lo_part = sorted(v[: pre // 2])
    hi_part = sorted(v[int(n * 0.8):])
    if not lo_part or not hi_part:
        return None
    v_lo = lo_part[len(lo_part) // 2]
    v_hi = hi_part[len(hi_part) // 2]
    if v_hi - v_lo < 1.0:
        return None
    mid = 0.5 * (v_lo + v_hi)
    c = next((i for i in range(max(1, pre // 2), n) if v[i - 1] < mid <= v[i]), None)
    if c is None:
        return None
    a = lanczos_a
    i0, i1 = max(a, c - half_window), min(n - a - 1, c + half_window)
    xs = [i0 + k / upsample for k in range((i1 - i0) * upsample + 1)]
    ys = []
    for x in xs:
        k0 = int(math.floor(x))
        ys.append(sum(v[k] * _lanczos(x - k, a) for k in range(k0 - a + 1, k0 + a + 1)))

    def crossing(level: float, start: int) -> tuple[Optional[float], int]:
        for j in range(max(1, start), len(ys)):
            if ys[j - 1] < level <= ys[j]:
                return xs[j - 1] + (level - ys[j - 1]) / (ys[j] - ys[j - 1]) * (xs[j] - xs[j - 1]), j
        return None, 0

    t10, j10 = crossing(v_lo + 0.1 * (v_hi - v_lo), 1)
    if t10 is None:
        return None
    t90, _ = crossing(v_lo + 0.9 * (v_hi - v_lo), j10)
    if t90 is None:
        return None
    return (t90 - t10) / rate_hz


def mean_std(xs: list[float]) -> tuple[float, float]:
    m = sum(xs) / len(xs)
    if len(xs) < 2:
        return m, 0.0
    return m, (sum((x - m) ** 2 for x in xs) / (len(xs) - 1)) ** 0.5


def crc32_of(values) -> int:
    return zlib.crc32(bytes(v & 0xFF for v in values)) & 0xFFFFFFFF


# ---- T0b(a): the S3 guard, with a mock DLL, before the real device is opened ----------

class _MockDwf:
    def __init__(self) -> None:
        self.oe_calls: list[int] = []
        self._oe = 0

    def FDwfDigitalIOOutputEnableSet(self, h, mask):   # noqa: N802
        self.oe_calls.append(int(mask.value))
        self._oe = int(mask.value)

    def FDwfDigitalIOOutputEnableGet(self, h, ref):    # noqa: N802
        ref._obj.value = self._oe

    def FDwfDigitalIOReset(self, h):                   # noqa: N802
        self._oe = 0

    def FDwfDigitalIOConfigure(self, h):               # noqa: N802
        pass


def t0b_guard_selftest(log: Callable[[str], None]) -> bool:
    forbidden = [(1 << 9, "NONE"), (1 << 11, "NONE"), (1 << 13, "NONE"), (1 << 14, "NONE"),
                 (1 << 15, "NONE"), (DIO_BUS, "NONE"), (DIO_BUS, "MCU"), (DIO_BUS | DIO_REQ, "MCU")]
    ok = True
    for mask, owner in forbidden:
        m = _MockDwf()
        try:
            ad2mod.ad2_set_oe(m, 1, mask, BUS[owner])
            refused = False
        except AD2SafetyViolation:
            refused = True
        good = refused and not m.oe_calls
        ok = ok and good
        log(f"[ad2] T0B(a) ad2_set_oe(0x{mask:04X}, bus={owner}): "
            f"{'refused before the DLL' if good else 'NOT REFUSED'}")
    for mask, owner in ((DIO_REQ, "NONE"), (DIO_BUS | DIO_REQ, "AD2")):   # the controls
        m = _MockDwf()
        ad2mod.ad2_set_oe(m, 1, mask, BUS[owner])
        good = m.oe_calls == [mask]
        ok = ok and good
        log(f"[ad2] T0B(a) control ad2_set_oe(0x{mask:04X}, bus={owner}): "
            f"{'passed to the DLL' if good else 'BLOCKED'}")
    return ok


# ---- Console reader --------------------------------------------------------------------

class Console:
    """Reads the ST-LINK VCP in a thread; every line is timestamped and kept."""

    def __init__(self, port: SerialPort, t0: float, sink: Callable[[str], None]) -> None:
        self._port = port
        self._t0 = t0
        self._sink = sink
        self._q: "queue.Queue[str]" = queue.Queue()
        self._stop = threading.Event()
        self.fw_abort: Optional[str] = None
        self._th = threading.Thread(target=self._run, daemon=True)
        self._th.start()

    def _run(self) -> None:
        buf = bytearray()
        while not self._stop.is_set():
            try:
                chunk = self._port.read(512)
            except OSError:
                time.sleep(0.05)
                continue
            if not chunk:
                continue
            buf.extend(chunk)
            while b"\n" in buf:
                raw, buf = buf.split(b"\n", 1)
                line = raw.decode("utf-8", errors="replace").rstrip("\r")
                self._sink(line)
                if "[gpio] ABORT" in line:
                    self.fw_abort = line
                self._q.put(line)

    def expect(self, pattern: str, timeout_s: float) -> re.Match:
        rx = re.compile(pattern)
        end = time.monotonic() + timeout_s
        while True:
            left = end - time.monotonic()
            if left <= 0:
                raise BenchAbort(f"timeout waiting for /{pattern}/")
            try:
                line = self._q.get(timeout=min(left, 0.2))
            except queue.Empty:
                continue
            m = rx.search(line)
            if m:
                return m

    def close(self) -> None:
        self._stop.set()
        self._th.join(timeout=1.0)


# ---- The session ---------------------------------------------------------------------------

class Session:
    def __init__(self, ad2: AD2, con: Console, log: Callable[[str], None]) -> None:
        self.ad2 = ad2
        self.con = con
        self.log = log
        self.owner = GPIO_BENCH_BUS_NONE
        self.checks: list[tuple[str, bool]] = []
        self.step_idx = 0

    # -- bookkeeping
    def check(self, name: str, ok: bool, detail: str) -> None:
        self.checks.append((name, ok))
        self.log(f"[ad2] {name}: {detail} -> {'PASS' if ok else 'FAIL'}")

    # -- handshake
    def req(self, high: bool) -> None:
        self.ad2.digital_io_write(DIO_REQ if high else 0, DIO_REQ)

    def wait_ack(self, high: bool, timeout_s: float = HS_TIMEOUT_S) -> None:
        end = time.monotonic() + timeout_s
        while True:
            if bool(self.ad2.digital_io_read() & DIO_ACK) == high:
                return
            if self.con.fw_abort:
                raise BenchAbort(f"firmware aborted: {self.con.fw_abort}")
            if time.monotonic() > end:
                raise BenchAbort(f"MCU_ACK did not go {'high' if high else 'low'} in {timeout_s} s")

    def sync(self, pre: Optional[Callable[[], None]] = None, post: Optional[Callable[[], None]] = None) -> None:
        self.wait_ack(True)
        if pre:
            pre()
        self.req(True)
        self.wait_ack(False)
        self.req(False)
        if post:
            post()

    # -- S7
    def step(self, name: str) -> None:
        m = self.con.expect(r"\[gpio\] STEP (\S+) bus=(\S+) v=(\d+)", 120.0)
        got, bus, ver = m.group(1), m.group(2), int(m.group(3))
        exp_name, exp_bus = STEPS[self.step_idx]
        if (got, bus, ver) != (name, exp_bus, BENCH_VERSION) or exp_name != name:
            raise BenchAbort(f"S7: announced {got} bus={bus} v={ver}, host table expects "
                             f"{exp_name} bus={exp_bus} v={BENCH_VERSION}")
        self.step_idx += 1
        self.owner = BUS[bus]
        self.set_oe(DIO_REQ)          # never more than HOST_REQ at a step boundary
        self.sync()

    def set_oe(self, mask: int) -> None:
        self.ad2.set_oe(mask, self.owner)      # S3: the only path, with this step's owner
        if self.ad2.get_oe() != mask:
            raise BenchAbort("OE read-back mismatch")

    # -- S5
    def s5_released(self, level_high: bool) -> tuple[bool, int, int]:
        oe = self.ad2.get_oe()
        dio = self.ad2.digital_io_read() & DIO_BUS
        ok = ((oe & DIO_BUS) == 0) and dio == (0xFF if level_high else 0x00)
        return ok, dio, oe

    def buschk(self, label: str) -> None:
        for high in (True, False):
            def pre(h=high) -> None:
                ok, dio, oe = self.s5_released(h)
                if not ok:
                    raise BenchAbort(f"S5 {label}: bus not released (pull-{'up' if h else 'down'}: "
                                     f"DIO=0x{dio:02X}, OE=0x{oe:04X}) - contention check failed")
            self.sync(pre=pre)
        self.log(f"[ad2] S5 {label}: bus followed both MCU pulls (0xFF, 0x00), OE bus = 0")

    # ================================================================ steps
    def t0a(self) -> None:
        self.step("T0A")
        vals = []
        self.sync(pre=lambda: vals.append(self.ad2.digital_io_read()))
        self.sync(pre=lambda: vals.append(self.ad2.digital_io_read()))
        up, dn = vals[0] & T0A_LINES, vals[1] & T0A_LINES
        self.check("T0A loose lines", up == T0A_LINES and dn == 0,
                   f"13 lines with MCU pull-up 0x{up:04X} (0x{T0A_LINES:04X}), pull-down 0x{dn:04X} (0x0000); "
                   f"handshake REQ/ACK exercised by every sync")

    def t0b(self) -> None:
        self.step("T0B")
        self.sync()
        self.step("T0B_S5NEG")
        self.buschk("T0B_S5NEG before")
        seen = []
        for high in (True, False):
            self.sync(pre=lambda h=high: seen.append(self.s5_released(h)))
        caught = all(not ok for ok, _, _ in seen)
        self.check("T0B(c) S5 negative control", caught,
                   f"MCU drives the bus: s5_released() returned "
                   f"{[(ok, f'0x{d:02X}') for ok, d, _ in seen]} (must be False twice, DIO=0x5A)")
        self.buschk("T0B_S5NEG after")
        self.step("T0B_TIMEOUT")
        self.wait_ack(True)
        self.log("[ad2] T0B(d) MCU_ACK high: the host deliberately does not raise HOST_REQ")
        self.con.expect(r"\[gpio\] T0B timeout handled", 20.0)
        low = (self.ad2.digital_io_read() & DIO_ACK) == 0
        self.check("T0B(d) firmware timeout", low, "firmware reported the timeout and MCU_ACK is low")

    def t3(self, tier: str) -> None:
        self.step(f"T3_{tier}")
        self.buschk(f"T3_{tier}")

        def oe_zero() -> None:
            if (self.ad2.get_oe() & DIO_BUS) != 0:
                raise BenchAbort("OE of the bus not 0 before the MCU drives")
        self.sync(pre=oe_zero)
        expected = T3_BASE + (T3_BM_EXTRA if tier == "BM" else [])
        seen: list[int] = []
        for _ in expected:
            self.sync(pre=lambda: seen.append(self.ad2.digital_io_read() & DIO_BUS))
        self.sync()     # released again
        diffs = [i for i, (e, s) in enumerate(zip(expected, seen)) if e != s]
        if tier == "BM":
            ctrl = len(expected) - 1
            ok = diffs == [ctrl] and seen[ctrl] == 0x54
            self.check("T3_BM control", ok, f"deliberate 0x54 against 0x55 detected at index {ctrl} only: diffs={diffs}")
            diffs = [d for d in diffs if d != ctrl]
        self.check(f"T3_{tier} pin operations", not diffs,
                   f"{len(seen)} values read by the AD2, differences at {diffs} "
                   f"[{' '.join(f'{v:02X}' for v in seen)}]")

    def t4(self, tier: str) -> None:
        self.step(f"T4_{tier}")
        self.buschk(f"T4_{tier}")

        def take_bus() -> None:
            time.sleep(0.001)                        # S6
            self.set_oe(DIO_REQ | DIO_BUS)
        self.sync(pre=take_bus)
        passes = [list(range(256))]
        if tier == "BM":
            passes.append([(~k) & 0xFF for k in range(256)])
        for pats in passes:
            for p in pats:
                self.sync(pre=lambda v=p: self.ad2.digital_io_write(v, DIO_BUS))
        for _ in range(2):                            # T7: input, then analog
            self.sync(pre=lambda: self.ad2.digital_io_write(0xFF, DIO_BUS))

        def give_bus() -> None:
            self.ad2.digital_io_write(0x00, DIO_BUS)
            self.set_oe(DIO_REQ)
            if (self.ad2.get_oe() & DIO_BUS) != 0:
                raise BenchAbort("OE of the bus did not go back to 0")
        self.sync(pre=give_bus)
        m = self.con.expect(rf"\[gpio\] T4_{tier} count=(\d+) crc_port=0x([0-9A-F]+) crc_bits=0x([0-9A-F]+)", 10.0)
        cnt, cp, cb = int(m.group(1)), int(m.group(2), 16), int(m.group(3), 16)
        host = crc32_of(passes[0])
        self.check(f"T4_{tier} input reads", cnt == 256 and cp == host and cb == host,
                   f"MCU count={cnt} crc_port=0x{cp:08X} crc_bits=0x{cb:08X}, host CRC32=0x{host:08X}")
        if tier == "BM":
            m = self.con.expect(r"\[gpio\] T4_BM inverted count=(\d+) crc_port=0x([0-9A-F]+) crc_bits=0x([0-9A-F]+)", 10.0)
            cp2 = int(m.group(2), 16)
            inv = crc32_of(passes[1])
            self.check("T4_BM control", cp2 != host and cp2 == inv,
                       f"inverted pass: MCU 0x{cp2:08X} differs from 0x{host:08X} and equals CRC32 of the inverted "
                       f"patterns 0x{inv:08X}")

    def t5t6(self) -> None:
        self.step("T5T6")
        vals: list[tuple[float, float]] = []

        def measure() -> None:
            vals.append((self.ad2.analog_in_average(0), self.ad2.analog_in_average(1)))
        for _ in range(2 + 3 * len(TIERS)):
            self.sync(pre=measure)
        v_np14, v_np15 = vals[0]
        v_pp14, v_dd = vals[1]        # V_DD from PF15 push-pull high (22k to 3V3: no current)
        # V_DD comes from PF15 driven high; it must agree with PF14 driven high on the
        # other channel, or R_IN below is computed from a wrong V_DD (seen once in
        # development: 3.235 V against 3.301 V gave R_IN = 20.8 M).
        vdd_ok = abs(v_dd - v_pp14) <= 0.05
        ctrl = v_np14 < 0.1 and v_np15 > 2.9 and v_pp14 > 2.9 and vdd_ok
        self.check("T5T6 controls", ctrl,
                   f"PF14 no pull {v_np14:.3f} V (<0.1: 22k to GND present), PF15 no pull {v_np15:.3f} V (>2.9: "
                   f"22k to 3V3 present), PF14 push-pull high {v_pp14:.3f} V (>2.9: the pin can rise), "
                   f"V_DD on PF15 {v_dd:.3f} V (within 0.05 V of PF14 high: {vdd_ok})")
        r_in = T5_RX_OHMS * v_np15 / (v_dd - v_np15)
        self.log(f"[ad2] T5 scope load measured on PF15: R_IN = {r_in / 1e3:.1f}k")
        for i, tier in enumerate(TIERS):
            v_pu14, v_pd15 = vals[2 + 3 * i]
            od_rel14, od_low15 = vals[3 + 3 * i]
            _, od_rel15 = vals[4 + 3 * i]
            r_par = 1.0 / (1.0 / T5_RX_OHMS + 1.0 / r_in)
            r_pu = r_par * (v_dd / v_pu14 - 1.0) if v_pu14 > 0 else float("nan")
            x = T5_RX_OHMS * v_pd15 / (v_dd - v_pd15)
            r_pd = 1.0 / (1.0 / x - 1.0 / r_in)
            valid = v_pu14 <= 2.0
            self.check(f"T5_{tier} pulls", valid and T5_R_MIN <= r_pu <= T5_R_MAX and T5_R_MIN <= r_pd <= T5_R_MAX,
                       f"V_PU14={v_pu14:.3f} V V_PD15={v_pd15:.3f} V -> R_PU={r_pu / 1e3:.1f}k R_PD={r_pd / 1e3:.1f}k "
                       f"(30-50k, DS12923 table 59)")
            self.check(f"T6_{tier} open-drain", od_low15 <= 0.4 and abs(od_rel15 - v_np15) <= 0.1 and od_rel14 < 0.1,
                       f"PF15 OD low {od_low15:.3f} V (<=0.4), OD released {od_rel15:.3f} V (V_NP15 {v_np15:.3f} +-0.1); "
                       f"PF14 OD released {od_rel14:.3f} V (<0.1)")

    def t8(self, name: str, cases: list[tuple[str, int, str]], control: bool) -> None:
        self.step(name)
        for case, expected, desc in cases:
            box: dict = {}

            def arm(b=box) -> None:
                b["rate"], b["n"] = self.ad2.la_arm(T8_LA_RATE_HZ, T8_LA_SAMPLES, trigger_rise_mask=DIO_REQ, prefill=100)

            def read(b=box) -> None:
                b["s"] = self.ad2.la_read(b["n"], timeout_s=2.0)
            self.sync(pre=arm, post=read)
            edges = la_transitions(box["s"], 9) if box.get("s") else -1
            ok = (edges >= 2) if control else (edges == expected)
            self.check(f"{name} case {case}", ok,
                       f"{desc}: DIO9 transitions={edges} (expected {'>=2' if control else expected})")

    def t9(self) -> None:
        self.step("T9")
        tr: dict[int, tuple[list[float], list[float]]] = {s: ([], []) for s in range(4)}
        for r, tier in enumerate(T9_ROUNDS):
            for s in range(4):
                for _ in range(10):
                    box: dict = {}

                    def arm(b=box) -> None:
                        b["rate"], b["n"] = self.ad2.scope_arm_rise(T9_RATE_HZ, T9_SAMPLES, trigger_channel=0, level_v=1.65)

                    def read(b=box) -> None:
                        b["d"] = self.ad2.scope_read(b["n"], timeout_s=1.0)
                    self.sync(pre=arm, post=read)
                    self.sync()
                    if box.get("d") is None:
                        continue
                    for ch in (0, 1):
                        t = rise_time_10_90(box["d"][ch], box["rate"], 0.1)
                        if t is not None:
                            tr[s][ch].append(t * 1e9)
            self.log(f"[ad2] T9 round {r + 1} ({tier}) done")
        box = {}
        self.sync(pre=lambda: box.update(zip(("rate", "n"), self.ad2.scope_arm_rise(
            T9_RATE_HZ, T9_SAMPLES, trigger_channel=0, level_v=1.65))),
            post=lambda: box.update(d=self.ad2.scope_read(box["n"], timeout_s=0.5)))
        for s in range(4):
            for ch, pin in ((0, "PF14"), (1, "PF15")):
                xs = tr[s][ch]
                if xs:
                    m_, s_ = mean_std(xs)
                    self.log(f"[ad2] T9 {pin} {T9_SPEEDS[s]}: tr={m_:.2f} ns sd={s_:.2f} ns n={len(xs)} "
                             f"[{', '.join(f'{x:.1f}' for x in xs)}]")
        for ch, pin in ((0, "PF14"), (1, "PF15")):
            low, vh = tr[0][ch], tr[3][ch]
            if len(low) != 40 or len(vh) != 40:
                self.check(f"T9 gate {pin}", False, f"edges measured LOW={len(low)} VERY_HIGH={len(vh)} (40 each)")
                continue
            (ml, sl), (mv, sv) = mean_std(low), mean_std(vh)
            sigma = ((sl ** 2) / len(low) + (sv ** 2) / len(vh)) ** 0.5
            self.check(f"T9 gate {pin}", (ml - mv) > 5.0 * sigma,
                       f"tr(LOW)-tr(VERY_HIGH)={ml - mv:.2f} ns, 5 sigma of the difference of means={5 * sigma:.2f} ns "
                       f"(40 edges each, 4 interleaved rounds)")
        self.check("T9 control", box.get("d") is None, "pins in input, ODR set: no edge within 0.5 s")

    def t10(self, tier: str) -> None:
        self.step(f"T10_{tier}")
        res: dict = {}

        def measure(key: str) -> None:
            rate, n = self.ad2.la_arm(100e6, 4096)
            samples = self.ad2.la_read(n, timeout_s=2.0) or []
            res[key] = la_frequency(samples, 14, rate)
        self.sync(pre=lambda: measure("f"))
        f_hz, rises = res["f"]
        self.check(f"T10_{tier} MCO1 on PA8 (AF0)", abs(f_hz - T10_EXPECTED_HZ) <= T10_EXPECTED_HZ * T10_TOLERANCE,
                   f"DIO14 {f_hz / 1e6:.4f} MHz from {rises} rising edges (4 MHz +-2 %)")
        if tier == "BM":
            self.sync(pre=lambda: measure("c"))
            fc, rc = res["c"]
            self.check("T10 control", rc < 2, f"PA8 in AF1: {rc} rising edges ({fc / 1e6:.3f} MHz), expected no signal")

    def run(self) -> None:
        self.t0a()
        self.t0b()
        for t in TIERS:
            self.t3(t)
        for t in TIERS:
            self.t4(t)
        self.t5t6()
        for t in TIERS:
            self.t8(f"T8_{t}", T8_CASES, control=False)
        self.t8("T8_CTRL", [("E", 2, "control: case C in the naive order (MODER, 1 us, BSRR)")], control=True)
        self.t9()
        for t in TIERS:
            self.t10(t)
        self.step("DONE")


def cube_programmer() -> str:
    import flash
    exe = flash.find_tool("STM32_Programmer_CLI.exe", "cubeprogrammer")
    if exe is None:
        raise SystemExit("STM32_Programmer_CLI.exe not found (run tools/env.ps1)")
    return str(exe)


def git_rev() -> str:
    try:
        return subprocess.check_output(["git", "rev-parse", "--short", "HEAD"], cwd=str(PROJECT), text=True).strip()
    except Exception:
        return "unknown"


def tree_record(stamp: Callable[[str], None]) -> bool:
    """R3: write the state of this checkout into the capture, before the AD2 is
    opened. Dirty is decided as CMakeLists.txt decides FW_GIT_REV's -dirty
    (`git status --porcelain -uno`, tracked files only); a dirty tree gets its
    whole `git diff --no-color HEAD`, so a -dirty capture carries its own patch.
    Read-only: nothing is added, stashed or checked out. Returns True if dirty."""
    try:
        status = subprocess.run(["git", "status", "--porcelain", "-uno"], cwd=str(PROJECT),
                                capture_output=True, check=True).stdout
        if not status.strip():
            stamp("[host] tree clean")
            return False
        diff = subprocess.run(["git", "diff", "--no-color", "HEAD"], cwd=str(PROJECT),
                              capture_output=True, check=True).stdout
    except (OSError, subprocess.CalledProcessError) as e:
        stamp(f"[host] tree state unknown: git failed ({e})")
        return True
    text = diff.decode("utf-8", errors="replace").splitlines()
    stamp(f"[host] dirty diff sha256={hashlib.sha256(diff).hexdigest()} lines={len(text)}")
    for line in text:
        stamp(f"[host] diff| {line}")
    return True


def host_build_id(preset: str, project: pathlib.Path = PROJECT) -> str:
    """R3-a/R3-b: FW_BUILD_ID as cmake/fw_build_id.cmake computes it, over the
    checkout of `project`: the first 16 hex digits of the SHA-256 of
    "HEAD=" <git rev-parse HEAD, 40 hex>, the bytes of `git diff --no-color
    HEAD`, "CMAKE_C_FLAGS=" <CMAKE_C_FLAGS of build/<preset>/CMakeCache.txt>
    and " PRESET=" <preset>, no newline added. "unknown" when git or the cache
    cannot be read; never an invented value. tools/run_warm_resets.py imports
    this function (--project, --preset): one definition for both tools."""
    try:
        head = subprocess.run(["git", "rev-parse", "HEAD"], cwd=str(project),
                              capture_output=True, check=True).stdout.strip()
        diff = subprocess.run(["git", "diff", "--no-color", "HEAD"], cwd=str(project),
                              capture_output=True, check=True).stdout
        cache = (project / "build" / preset / "CMakeCache.txt").read_bytes()
    except (OSError, subprocess.CalledProcessError):
        return "unknown"
    if not re.fullmatch(rb"[0-9a-f]{40}", head):
        return "unknown"
    flags = None
    for line in cache.splitlines():
        m = re.match(rb"^CMAKE_C_FLAGS:[A-Z]+=(.*)$", line)
        if m:
            flags = m.group(1)
            break
    if flags is None:
        return "unknown"
    payload = b"HEAD=" + head + diff + b"CMAKE_C_FLAGS=" + flags + b" PRESET=" + preset.encode("utf-8")
    return hashlib.sha256(payload).hexdigest()[:16]


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", help="ST-LINK VCP (default: autodetect)")
    ap.add_argument("--out", type=pathlib.Path, help="capture file (default: docs/gpio_selftest_<rev>.txt)")
    ap.add_argument("--allow-dirty", action="store_true", help="accept a -dirty Rev (development runs)")
    ap.add_argument("--no-reset", action="store_true", help="do not reset the board; wait for the banner")
    ap.add_argument("--preset", default="selftest-gpio", help="configure preset of the image (build id)")
    args = ap.parse_args()

    lines: list[str] = []
    t0 = time.monotonic()
    lock = threading.Lock()

    def stamp(text: str) -> None:
        entry = f"[+{time.monotonic() - t0:8.3f}s] {text}"
        with lock:
            lines.append(entry)
        print(entry, flush=True)

    stamp(f"[host] gpio_hw_check.py, host checkout {git_rev()}, protocol v{BENCH_VERSION}")
    stamp("[host] checklist: GND first, DIO per README table, nothing on W1/W2/V+/V-; WaveForms GUI closed; "
          "image selftest-gpio flashed with this script stopped")
    tree_record(stamp)
    build_id = host_build_id(args.preset)
    stamp(f"[host] build id of this checkout and build/{args.preset}: {build_id}")
    guard_ok = t0b_guard_selftest(stamp)

    port = args.port or autodetect()
    if not port:
        stamp("[host] ERROR: no ST-LINK VCP found")
        return 2

    verdict_fw = None
    session: Optional[Session] = None
    abort: Optional[str] = None
    revs: dict[str, str] = {}
    with AD2() as ad2, SerialPort(port, 115200) as ser:
        stamp(f"[ad2] opened in S2: OE=0x{ad2.get_oe():04X} DIO=0x{ad2.digital_io_read():04X}")
        con = Console(ser, t0, stamp)
        try:
            if not args.no_reset:
                stamp("[host] reset through SWD (STM32_Programmer_CLI -c port=SWD mode=HOTPLUG -rst), AD2 in S2")
                subprocess.run([cube_programmer(), "-c", "port=SWD", "mode=HOTPLUG", "-rst"],
                               stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, check=True)
            revs["cm7"] = con.expect(r"\[gpio\] CM7 Rev (\S+)", 20.0).group(1)
            ids = {"cm7": con.expect(r"\[gpio\] CM7 Build id (\S+)", 10.0).group(1)}
            revs["cm4"] = con.expect(r"\[gpio\] CM4 Rev (\S+)", 10.0).group(1)
            ids["cm4"] = con.expect(r"\[gpio\] CM4 Build id (.+?)\s*$", 10.0).group(1)
            ver = int(con.expect(r"\[gpio\] GPIO_BENCH v(\d+) idle", 10.0).group(1))
            clean = revs["cm7"] == revs["cm4"] and "-dirty" not in revs["cm7"] and revs["cm7"] != "unknown"
            if ver != BENCH_VERSION:
                raise BenchAbort(f"S8: firmware protocol v{ver}, host v{BENCH_VERSION}")
            if not clean and not args.allow_dirty:
                raise BenchAbort(f"R2: Rev cm7={revs['cm7']} cm4={revs['cm4']} is not one clean commit")
            # R3-b: the image must come from this checkout's commit. --allow-dirty
            # does not waive it: a -dirty run is an edit on the same commit, not
            # an image of another one.
            head = git_rev()
            base = revs["cm7"][:-len("-dirty")] if revs["cm7"].endswith("-dirty") else revs["cm7"]
            stamp(f"[host] R3-b: image Rev {revs['cm7']} (commit {base}), host HEAD {head}")
            if head == "unknown" or base != head:
                raise BenchAbort(f"R3-b: the image was built at {base}, this checkout is at {head}")
            # R3-a, in the S8 phase: nothing is enabled yet. "unknown" never matches.
            stamp(f"[host] build id host={build_id} cm7={ids['cm7']} cm4={ids['cm4']}")
            same = build_id != "unknown" and ids["cm7"] == build_id and ids["cm4"] == build_id
            if not same:
                if not args.allow_dirty:
                    raise BenchAbort(f"R3-a: the image is not the one this checkout builds "
                                     f"(host={build_id} cm7={ids['cm7']} cm4={ids['cm4']})")
                stamp("[host] R3-a: build ids differ; --allow-dirty given, going on")
            stamp(f"[host] S8: banner v{ver}, Rev cm7={revs['cm7']} cm4={revs['cm4']}")
            session = Session(ad2, con, stamp)
            session.set_oe(DIO_REQ)          # HOST_REQ output, low
            session.req(False)
            session.run()
            m = con.expect(r"\[gpio\] FW VERDICT=(\w+)", 120.0)
            verdict_fw = m.group(1)
        except (BenchAbort, AD2SafetyViolation, subprocess.CalledProcessError) as e:
            abort = str(e)
            ad2.set_safe_state()
            stamp(f"[ad2] ABORT: {abort}; AD2 back in S2 (OE=0x{ad2.get_oe():04X})")
            try:
                m = con.expect(r"\[gpio\] FW VERDICT=(\w+)", 60.0)
                verdict_fw = m.group(1)
            except BenchAbort:
                pass
        finally:
            ad2.set_safe_state()
            time.sleep(0.3)
            con.close()
        stamp(f"[ad2] closing in S2: OE=0x{ad2.get_oe():04X}")

    checks = session.checks if session else []
    stamp(f"[host] T0B(a) host guard: {'PASS' if guard_ok else 'FAIL'}")
    for name, ok in checks:
        stamp(f"[host] CHECK {name}: {'PASS' if ok else 'FAIL'}")
    all_ok = (guard_ok and abort is None and verdict_fw == "PASS" and bool(checks) and all(ok for _, ok in checks))
    stamp(f"[host] {sum(1 for _, ok in checks if ok)}/{len(checks)} host checks passed, FW VERDICT={verdict_fw}, "
          f"abort={abort}")
    stamp(f"HOST VERDICT={'PASS' if all_ok else 'FAIL'}")

    out = args.out
    if out is None:
        rev = revs.get("cm7", "unknown")
        out = PROJECT / "docs" / f"gpio_selftest_{rev}.txt"
        n = 2
        while out.exists():
            out = PROJECT / "docs" / f"gpio_selftest_{rev}_{n}.txt"
            n += 1
    out.parent.mkdir(parents=True, exist_ok=True)
    with lock:
        out.write_text("\n".join(lines) + "\n", encoding="utf-8")
    print(f"capture: {out}")
    return 0 if all_ok else 1


if __name__ == "__main__":
    sys.exit(main())
