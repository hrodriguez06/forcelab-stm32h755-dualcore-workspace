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

The image must be the one this checkout builds (R3-a, R3-b, R4-a). Before any
output is enabled (the S8 phase), the host always requires, with or without
--allow-dirty:
  - Rev(CM7) == Rev(CM4), -dirty included: both images come from one tree;
  - id(CM7) == id(CM4), neither "unknown": one build dir, one fw_build_id.h;
  - the Rev of each core without -dirty == git rev-parse --short HEAD.
Without --allow-dirty it also requires no -dirty Rev and the image's id equal to
the id the host computes over its checkout and build/<preset> (its commit
included). --allow-dirty waives only those two: a -dirty Rev, and a host id
that differs from the image's. Every abort names cm7 and cm4 and their values.
Each capture starts with "[host] argv:" and "[host] options:" (R4-b).

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
import ctypes
import hashlib
import math
import os
import pathlib
import queue
import re
import shlex
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
    ("T11_CM7_RISING", "NONE"), ("T11_CM7_FALLING", "NONE"), ("T11_CM7_BOTH", "NONE"), ("T11_CM7_EVENT", "NONE"),
    ("T11_CM4_RISING", "NONE"), ("T11_CM4_FALLING", "NONE"), ("T11_CM4_BOTH", "NONE"), ("T11_CM4_EVENT", "NONE"),
    ("T12", "NONE"),
    ("T13_CM7", "NONE"), ("T13_CM4", "NONE"),
    ("T14", "NONE"),
    ("T17", "NONE"),
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


# ---- DigitalIn helper (supports rising and falling edge triggers without touching ad2.py)
class DigitalInHelper:
    """Helper for Logic Analyzer captures on AD2, supporting both rising and falling edge triggers."""

    DIGITAL_IN_CLOCK_HZ = 100e6

    @staticmethod
    def arm(ad2_or_dwf, rate_hz: float, samples: int,
            trig_rise_mask: int = 0, trig_fall_mask: int = 0, prefill: int = 50) -> tuple[float, int]:
        if hasattr(ad2_or_dwf, "_dwf"):
            dwf = ad2_or_dwf._dwf
            hdwf = getattr(ad2_or_dwf, "_hdwf", ctypes.c_int(1))
        else:
            dwf = ad2_or_dwf
            hdwf = ctypes.c_int(1)

        div = max(1, int(round(DigitalInHelper.DIGITAL_IN_CLOCK_HZ / rate_hz)))
        max_size = ctypes.c_int()
        dwf.FDwfDigitalInReset(hdwf)
        dwf.FDwfDigitalInBufferSizeInfo(hdwf, ctypes.byref(max_size))
        n = min(samples, max_size.value) if max_size.value > 0 else samples
        dwf.FDwfDigitalInDividerSet(hdwf, ctypes.c_uint(div))
        dwf.FDwfDigitalInSampleFormatSet(hdwf, ctypes.c_int(16))
        dwf.FDwfDigitalInBufferSizeSet(hdwf, ctypes.c_int(n))

        if trig_rise_mask or trig_fall_mask:
            dwf.FDwfDigitalInTriggerSourceSet(hdwf, ctypes.c_ubyte(3))  # trigsrcDetectorDigitalIn
            dwf.FDwfDigitalInTriggerSet(
                hdwf,
                ctypes.c_uint(0),
                ctypes.c_uint(0),
                ctypes.c_uint(trig_rise_mask & 0xFFFF),
                ctypes.c_uint(trig_fall_mask & 0xFFFF),
            )
            dwf.FDwfDigitalInTriggerPositionSet(hdwf, ctypes.c_uint(max(0, n - prefill)))
        else:
            dwf.FDwfDigitalInTriggerSourceSet(hdwf, ctypes.c_ubyte(0))  # trigsrcNone

        dwf.FDwfDigitalInConfigure(hdwf, ctypes.c_int(1), ctypes.c_int(1))
        return DigitalInHelper.DIGITAL_IN_CLOCK_HZ / div, n

    @staticmethod
    def read(ad2_or_dwf, samples: int, timeout_s: float = 0.5) -> Optional[list[int]]:
        if hasattr(ad2_or_dwf, "_dwf"):
            dwf = ad2_or_dwf._dwf
            hdwf = getattr(ad2_or_dwf, "_hdwf", ctypes.c_int(1))
        else:
            dwf = ad2_or_dwf
            hdwf = ctypes.c_int(1)

        sts = ctypes.c_ubyte()
        t0 = time.monotonic()
        while True:
            dwf.FDwfDigitalInStatus(hdwf, ctypes.c_int(1), ctypes.byref(sts))
            if sts.value == 2:  # DwfStateDone
                break
            if time.monotonic() - t0 > timeout_s:
                dwf.FDwfDigitalInConfigure(hdwf, ctypes.c_int(0), ctypes.c_int(0))
                return None
            time.sleep(0.0005)

        buf = (ctypes.c_uint16 * samples)()
        dwf.FDwfDigitalInStatusData(hdwf, buf, ctypes.c_int(2 * samples))
        return [int(v) for v in buf]


# ---- T0b(a): the S3 guard, with a mock DLL, before the real device is opened ----------

class _MockDwf:
    def __init__(self) -> None:
        self.oe_calls: list[int] = []
        self._oe = 0
        self._output_val = 0
        self.la_trigger_mask_rise = 0
        self.la_trigger_mask_fall = 0
        self.la_trigger_mask = 0
        self.la_samples = 4096
        self.la_rate = 100e6
        self.la_armed = False
        self.mock_la_samples: Optional[list[int]] = None
        self.current_step = ""
        self.step_subtest = 0

    def set_step(self, step_name: str) -> None:
        self.current_step = step_name
        self.step_subtest = 0

    def FDwfDigitalIOOutputEnableSet(self, h, mask):   # noqa: N802
        self.oe_calls.append(int(mask.value))
        self._oe = int(mask.value)

    def FDwfDigitalIOOutputEnableGet(self, h, ref):    # noqa: N802
        ref._obj.value = self._oe

    def FDwfDigitalIOReset(self, h):                   # noqa: N802
        self._oe = 0
        self._output_val = 0

    def FDwfDigitalIOConfigure(self, h):               # noqa: N802
        pass

    def FDwfDigitalIOOutputSet(self, h, val):          # noqa: N802
        self._output_val = int(val.value)

    def FDwfDigitalIOInputStatus(self, h, ref):        # noqa: N802
        # In handshake, MCU responds to HOST_REQ: if HOST_REQ high, MCU_ACK drops; if low, MCU_ACK rises.
        ack = 0 if (self._output_val & DIO_REQ) else DIO_ACK
        ref._obj.value = ack

    def FDwfDigitalInReset(self, h):                   # noqa: N802
        self.la_armed = False

    def FDwfDigitalInBufferSizeInfo(self, h, ref):     # noqa: N802
        ref._obj.value = 16384

    def FDwfDigitalInDividerSet(self, h, div):         # noqa: N802
        pass

    def FDwfDigitalInSampleFormatSet(self, h, fmt):    # noqa: N802
        pass

    def FDwfDigitalInBufferSizeSet(self, h, n):        # noqa: N802
        self.la_samples = int(n.value)

    def FDwfDigitalInTriggerSourceSet(self, h, src):   # noqa: N802
        pass

    def FDwfDigitalInTriggerSet(self, h, low, hi, rise, fall):  # noqa: N802
        self.la_trigger_mask_rise = int(rise.value)
        self.la_trigger_mask_fall = int(fall.value)
        self.la_trigger_mask = self.la_trigger_mask_rise | self.la_trigger_mask_fall

    def FDwfDigitalInTriggerPositionSet(self, h, pos):  # noqa: N802
        pass

    def FDwfDigitalInConfigure(self, h, rearm, start):  # noqa: N802
        self.la_armed = bool(start.value)

    def FDwfDigitalInStatus(self, h, read_data, ref):   # noqa: N802
        ref._obj.value = 2  # DwfStateDone

    def FDwfDigitalInStatusData(self, h, buf, nbytes):  # noqa: N802
        n_samples = nbytes.value // 2
        if self.mock_la_samples is not None:
            samples = self.mock_la_samples
        else:
            samples = [0] * n_samples
            trig_bit = None
            resp_bit = None
            if self.la_trigger_mask & DIO_PE3:
                trig_bit, resp_bit = 8, 9
            elif self.la_trigger_mask & DIO_PE4:
                trig_bit, resp_bit = 10, 11
            is_fall = bool(self.la_trigger_mask_fall & (DIO_PE3 | DIO_PE4))
            if trig_bit is not None:
                if is_fall:
                    for i in range(0, 50):
                        samples[i] |= (1 << trig_bit)
                else:
                    for i in range(50, n_samples):
                        samples[i] |= (1 << trig_bit)

                is_event = "EVENT" in self.current_step
                is_t13 = self.current_step in ("T13_CM7", "T13_CM4")
                if not is_event and not is_t13:
                    if self.current_step == "T12":
                        if self.step_subtest == 1:
                            # shared line 3: both DIO9 and DIO11 respond
                            for i in range(65, min(n_samples, 85)):
                                samples[i] |= (1 << 9) | (1 << 11)
                        elif self.step_subtest == 2:
                            # control: only DIO9 responds
                            for i in range(65, min(n_samples, 85)):
                                samples[i] |= (1 << 9)
                        else:
                            # independent
                            if resp_bit is not None:
                                for i in range(65, min(n_samples, 85)):
                                    samples[i] |= (1 << resp_bit)
                    else:
                        if resp_bit is not None:
                            for i in range(65, min(n_samples, 85)):
                                samples[i] |= (1 << resp_bit)
        for i in range(min(n_samples, len(samples))):
            buf[i] = samples[i] & 0xFFFF


class MockAD2:
    """Mock AD2 wrapper for non-board testing."""

    def __init__(self, mock_dwf: _MockDwf) -> None:
        self._dwf = mock_dwf
        self._oe = 0
        self._out = 0

    def __enter__(self) -> "MockAD2":
        return self

    def __exit__(self, *args) -> None:
        self.set_safe_state()

    def set_safe_state(self) -> None:
        self._oe = 0
        self._out = 0
        self._dwf.FDwfDigitalIOReset(1)

    def set_oe(self, mask: int, owner: int = GPIO_BENCH_BUS_NONE) -> None:
        ad2mod.ad2_set_oe(self._dwf, 1, mask, owner)
        self._oe = mask

    def get_oe(self) -> int:
        return self._oe

    def digital_io_write(self, val: int, mask: int) -> None:
        self._out = (self._out & ~mask) | (val & mask)
        self._dwf.FDwfDigitalIOOutputSet(1, ctypes.c_uint32(self._out))

    def digital_io_read(self) -> int:
        ref = ctypes.c_uint32()
        self._dwf.FDwfDigitalIOInputStatus(1, ctypes.byref(ref))
        return ref.value

    def la_arm(self, rate_hz: float, samples: int, trigger_rise_mask: int = 0,
               prefill: int = 0) -> tuple[float, int]:
        self._dwf.la_trigger_mask_rise = trigger_rise_mask
        self._dwf.la_trigger_mask_fall = 0
        self._dwf.la_trigger_mask = trigger_rise_mask
        self._dwf.la_samples = samples
        self._dwf.la_rate = rate_hz
        return rate_hz, samples

    def la_read(self, samples: int, timeout_s: float = 2.0) -> Optional[list[int]]:
        buf = (ctypes.c_uint16 * samples)()
        self._dwf.FDwfDigitalInStatusData(1, buf, ctypes.c_int(2 * samples))
        return [int(v) for v in buf]


class MockConsole:
    """Mock console that replays pre-configured firmware lines for testing without hardware."""

    def __init__(self, lines: list[str], sink: Callable[[str], None], dwf: Optional[_MockDwf] = None) -> None:
        self._sink = sink
        self._dwf = dwf
        self._q: "queue.Queue[str]" = queue.Queue()
        self.fw_abort: Optional[str] = None
        for line in lines:
            self._sink(line)
            if "[gpio] ABORT" in line:
                self.fw_abort = line
            self._q.put(line)

    def expect(self, pattern: str, timeout_s: float = 10.0) -> re.Match:
        rx = re.compile(pattern)
        while not self._q.empty():
            line = self._q.get_nowait()
            m = rx.search(line)
            if m:
                if self._dwf:
                    m_step = re.search(r"\[gpio\] STEP (\S+)", line)
                    if m_step:
                        self._dwf.set_step(m_step.group(1))
                    if "T12 independent:" in line:
                        self._dwf.step_subtest = 1
                    elif "T12 shared line 3:" in line:
                        self._dwf.step_subtest = 2
                return m
        raise BenchAbort(f"mock console: timeout waiting for /{pattern}/")

    def close(self) -> None:
        pass


def t0b_guard_selftest(log: Callable[[str], None]) -> bool:
    forbidden = [
        (1 << 9, "NONE"), (1 << 11, "NONE"), (1 << 13, "NONE"), (1 << 14, "NONE"),
        (1 << 15, "NONE"), (DIO_BUS, "NONE"), (DIO_BUS, "MCU"), (DIO_BUS | DIO_REQ, "MCU"),
        (DIO_REQ | (1 << 9), "NONE"), (DIO_REQ | (1 << 11), "NONE"), (DIO_PE3 | (1 << 9), "NONE"),
    ]
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

    controls = [
        (DIO_REQ, "NONE"),
        (DIO_REQ | DIO_PE3, "NONE"),
        (DIO_REQ | DIO_PE4, "NONE"),
        (DIO_REQ | DIO_PE3 | DIO_PE4, "NONE"),
        (DIO_BUS | DIO_REQ, "AD2"),
    ]
    for mask, owner in controls:
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
        self.t11_cm7_latencies: list[float] = []
        self.t11_cm4_latencies: list[float] = []

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
            t_pre0 = time.monotonic()
            pre()
            dt_pre = time.monotonic() - t_pre0
            self.log(f"[ad2] sync pre() executed in {dt_pre * 1e3:.2f} ms")
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

    def pulse_train(self, dio_pin: int, count: int, delay_s: float = 0.001) -> int:
        """Sends `count` pulses on `dio_pin` (DIO8 or DIO10).
        Static writes with at least delay_s (>= 1 ms) between edges.
        Part from 0, return to 0, and return OE to DIO_REQ."""
        self.set_oe(DIO_REQ | dio_pin)
        self.ad2.digital_io_write(0, dio_pin)
        time.sleep(delay_s)
        for _ in range(count):
            self.ad2.digital_io_write(dio_pin, dio_pin)
            time.sleep(delay_s)
            self.ad2.digital_io_write(0, dio_pin)
            time.sleep(delay_s)
        self.set_oe(DIO_REQ)
        return count

    def pulse_train_measured(self, dio_pin: int, resp_dio: int, count: int,
                             mode: str = "RISING", collect_latencies: bool = False,
                             expect_resp: bool = True) -> tuple[int, int, list[float]]:
        """Sends `count` pulses on `dio_pin` (DIO8 or DIO10), arms the Logic Analyzer
        on each edge at 100 MS/s (512 samples) with 50 prefill samples, verifies host-sent
        edges and MCU response edges on `resp_dio` (DIO9 for CM7, DIO11 for CM4),
        and optionally records edge-to-edge latencies."""
        pin_bit = 8 if dio_pin == DIO_PE3 else 10
        resp_bit = resp_dio
        sent_edges = 0
        resp_edges = 0
        latencies: list[float] = []
        self.set_oe(DIO_REQ | dio_pin)
        self.ad2.digital_io_write(0, dio_pin)
        time.sleep(0.0005)

        for _ in range(count):
            if mode == "RISING":
                rate, n = DigitalInHelper.arm(self.ad2, 100e6, 512, trig_rise_mask=dio_pin, trig_fall_mask=0, prefill=50)
                self.ad2.digital_io_write(dio_pin, dio_pin)
                samples = DigitalInHelper.read(self.ad2, n, timeout_s=0.2)
                self.ad2.digital_io_write(0, dio_pin)
                time.sleep(0.0005)
                if samples:
                    r_pin = next((i for i in range(1, len(samples))
                                  if ((samples[i] >> pin_bit) & 1) and not ((samples[i - 1] >> pin_bit) & 1)), None)
                    if r_pin is not None:
                        sent_edges += 1
                        r_resp = next((i for i in range(r_pin, len(samples))
                                       if ((samples[i] >> resp_bit) & 1) and not ((samples[i - 1] >> resp_bit) & 1)), None)
                        if r_resp is not None:
                            resp_edges += 1
                            if collect_latencies:
                                latencies.append((r_resp - r_pin) / rate * 1e6)
            elif mode == "FALLING":
                self.ad2.digital_io_write(dio_pin, dio_pin)
                time.sleep(0.0005)
                rate, n = DigitalInHelper.arm(self.ad2, 100e6, 512, trig_rise_mask=0, trig_fall_mask=dio_pin, prefill=50)
                self.ad2.digital_io_write(0, dio_pin)
                samples = DigitalInHelper.read(self.ad2, n, timeout_s=0.2)
                time.sleep(0.0005)
                if samples:
                    f_pin = next((i for i in range(1, len(samples))
                                  if not ((samples[i] >> pin_bit) & 1) and ((samples[i - 1] >> pin_bit) & 1)), None)
                    if f_pin is not None:
                        sent_edges += 1
                        r_resp = next((i for i in range(f_pin, len(samples))
                                       if ((samples[i] >> resp_bit) & 1) and not ((samples[i - 1] >> resp_bit) & 1)), None)
                        if r_resp is not None:
                            resp_edges += 1
            elif mode == "BOTH":
                # Part 1: Rising edge
                rate, n = DigitalInHelper.arm(self.ad2, 100e6, 512, trig_rise_mask=dio_pin, trig_fall_mask=0, prefill=50)
                self.ad2.digital_io_write(dio_pin, dio_pin)
                samples = DigitalInHelper.read(self.ad2, n, timeout_s=0.2)
                time.sleep(0.0005)
                if samples:
                    r_pin = next((i for i in range(1, len(samples))
                                  if ((samples[i] >> pin_bit) & 1) and not ((samples[i - 1] >> pin_bit) & 1)), None)
                    if r_pin is not None:
                        sent_edges += 1
                        r_resp = next((i for i in range(r_pin, len(samples))
                                       if ((samples[i] >> resp_bit) & 1) and not ((samples[i - 1] >> resp_bit) & 1)), None)
                        if r_resp is not None:
                            resp_edges += 1
                # Part 2: Falling edge
                rate, n = DigitalInHelper.arm(self.ad2, 100e6, 512, trig_rise_mask=0, trig_fall_mask=dio_pin, prefill=50)
                self.ad2.digital_io_write(0, dio_pin)
                samples = DigitalInHelper.read(self.ad2, n, timeout_s=0.2)
                time.sleep(0.0005)
                if samples:
                    f_pin = next((i for i in range(1, len(samples))
                                  if not ((samples[i] >> pin_bit) & 1) and ((samples[i - 1] >> pin_bit) & 1)), None)
                    if f_pin is not None:
                        sent_edges += 1
                        r_resp = next((i for i in range(f_pin, len(samples))
                                       if ((samples[i] >> resp_bit) & 1) and not ((samples[i - 1] >> resp_bit) & 1)), None)
                        if r_resp is not None:
                            resp_edges += 1
            elif mode == "EVENT":
                rate, n = DigitalInHelper.arm(self.ad2, 100e6, 512, trig_rise_mask=dio_pin, trig_fall_mask=0, prefill=50)
                self.ad2.digital_io_write(dio_pin, dio_pin)
                samples = DigitalInHelper.read(self.ad2, n, timeout_s=0.2)
                self.ad2.digital_io_write(0, dio_pin)
                time.sleep(0.0005)
                if samples:
                    r_pin = next((i for i in range(1, len(samples))
                                  if ((samples[i] >> pin_bit) & 1) and not ((samples[i - 1] >> pin_bit) & 1)), None)
                    if r_pin is not None:
                        sent_edges += 1
                    transitions_resp = la_transitions(samples, resp_bit)
                    if transitions_resp > 0:
                        resp_edges += transitions_resp

        self.set_oe(DIO_REQ)
        return sent_edges, resp_edges, latencies

    def t11_cm7(self, step_name: str, expected: int) -> None:
        self.step(step_name)
        mode = "RISING"
        if "FALLING" in step_name:
            mode = "FALLING"
        elif "BOTH" in step_name:
            mode = "BOTH"
        elif "EVENT" in step_name:
            mode = "EVENT"

        collect_lat = (mode == "RISING")
        expect_resp = (mode != "EVENT")
        res_box: dict[str, int] = {}

        def do_pulses() -> None:
            s_edges, r_edges, lats = self.pulse_train_measured(
                DIO_PE3, resp_dio=9, count=200, mode=mode,
                collect_latencies=collect_lat, expect_resp=expect_resp
            )
            res_box["sent"] = s_edges
            res_box["resp"] = r_edges
            if collect_lat:
                self.t11_cm7_latencies = lats

        self.sync(pre=do_pulses)
        m = self.con.expect(rf"\[gpio\] {step_name}: CM7 count=(\d+) \(expected (\d+)\)", 15.0)
        cnt, exp = int(m.group(1)), int(m.group(2))
        s_edges = res_box.get("sent", 0)
        r_edges = res_box.get("resp", 0)
        exp_sent = 400 if mode == "BOTH" else 200
        exp_resp = 0 if mode == "EVENT" else exp_sent
        la_ok = (s_edges == exp_sent) and (r_edges == exp_resp)
        self.check(f"{step_name} isr count", cnt == expected and exp == expected and la_ok,
                   f"CM7 count={cnt} (exp {expected}), LA measured {s_edges} sent on DIO8 (exp {exp_sent}), "
                   f"{r_edges} responses on DIO9 (exp {exp_resp})")

    def t11_cm4(self, step_name: str, expected: int) -> None:
        self.step(step_name)
        mode = "RISING"
        if "FALLING" in step_name:
            mode = "FALLING"
        elif "BOTH" in step_name:
            mode = "BOTH"
        elif "EVENT" in step_name:
            mode = "EVENT"

        collect_lat = (mode == "RISING")
        expect_resp = (mode != "EVENT")
        res_box: dict[str, int] = {}

        def do_pulses() -> None:
            s_edges, r_edges, lats = self.pulse_train_measured(
                DIO_PE4, resp_dio=11, count=200, mode=mode,
                collect_latencies=collect_lat, expect_resp=expect_resp
            )
            res_box["sent"] = s_edges
            res_box["resp"] = r_edges
            if collect_lat:
                self.t11_cm4_latencies = lats

        self.sync(pre=do_pulses)
        m = self.con.expect(rf"\[gpio\] {step_name}: CM4 count=(\d+) \(expected (\d+)\)", 15.0)
        cnt, exp = int(m.group(1)), int(m.group(2))
        s_edges = res_box.get("sent", 0)
        r_edges = res_box.get("resp", 0)
        exp_sent = 400 if mode == "BOTH" else 200
        exp_resp = 0 if mode == "EVENT" else exp_sent
        la_ok = (s_edges == exp_sent) and (r_edges == exp_resp)
        self.check(f"{step_name} isr count", cnt == expected and exp == expected and la_ok,
                   f"CM4 count={cnt} (exp {expected}), LA measured {s_edges} sent on DIO10 (exp {exp_sent}), "
                   f"{r_edges} responses on DIO11 (exp {exp_resp})")

    def t12(self) -> None:
        self.step("T12")
        # Subtest 1: Independent routing (PE3 on CM7, PE4 on CM4)
        box1: dict[str, int] = {}
        def pulse_both() -> None:
            s3, r9, _ = self.pulse_train_measured(DIO_PE3, resp_dio=9, count=200, mode="RISING")
            s4, r11, _ = self.pulse_train_measured(DIO_PE4, resp_dio=11, count=200, mode="RISING")
            box1["s3"] = s3
            box1["r9"] = r9
            box1["s4"] = s4
            box1["r11"] = r11
        self.sync(pre=pulse_both)
        m1 = self.con.expect(r"\[gpio\] T12 independent: CM7=(\d+) \(exp 200\), CM4=(\d+) \(exp 200\)", 15.0)
        c7_1, c4_1 = int(m1.group(1)), int(m1.group(2))
        la1_ok = (box1.get("s3") == 200 and box1.get("r9") == 200 and
                  box1.get("s4") == 200 and box1.get("r11") == 200)
        self.check("T12 independent routing", c7_1 == 200 and c4_1 == 200 and la1_ok,
                   f"CM7={c7_1} (exp 200), CM4={c4_1} (exp 200), LA PE3->DIO9={box1.get('r9')} PE4->DIO11={box1.get('r11')}")

        # Subtest 2: Shared line (PE3 routed to both CM7 and CM4)
        box2: dict[str, int] = {}
        def pulse_shared() -> None:
            sent = 0
            r9_cnt = 0
            r11_cnt = 0
            self.set_oe(DIO_REQ | DIO_PE3)
            self.ad2.digital_io_write(0, DIO_PE3)
            time.sleep(0.0005)
            for _ in range(200):
                rate, n = DigitalInHelper.arm(self.ad2, 100e6, 512, trig_rise_mask=DIO_PE3, prefill=50)
                self.ad2.digital_io_write(DIO_PE3, DIO_PE3)
                samples = DigitalInHelper.read(self.ad2, n, timeout_s=0.2)
                self.ad2.digital_io_write(0, DIO_PE3)
                time.sleep(0.0005)
                if samples:
                    r8 = next((i for i in range(1, len(samples))
                               if ((samples[i] >> 8) & 1) and not ((samples[i - 1] >> 8) & 1)), None)
                    if r8 is not None:
                        sent += 1
                        if any(((s >> 9) & 1) for s in samples[r8:]):
                            r9_cnt += 1
                        if any(((s >> 11) & 1) for s in samples[r8:]):
                            r11_cnt += 1
            self.set_oe(DIO_REQ)
            box2["sent"] = sent
            box2["r9"] = r9_cnt
            box2["r11"] = r11_cnt

        self.sync(pre=pulse_shared)
        m2 = self.con.expect(r"\[gpio\] T12 shared line 3: CM7=(\d+) \(exp 200\), CM4=(\d+) \(exp 200\)", 15.0)
        c7_2, c4_2 = int(m2.group(1)), int(m2.group(2))
        la2_ok = (box2.get("sent") == 200 and box2.get("r9") == 200 and box2.get("r11") == 200)
        self.check("T12 shared line routing", c7_2 == 200 and c4_2 == 200 and la2_ok,
                   f"CM7={c7_2} (exp 200), CM4={c4_2} (exp 200), LA DIO9={box2.get('r9')} DIO11={box2.get('r11')}")

        # Subtest 3: Control (Line 3 disabled in CM4)
        box3: dict[str, int] = {}
        def pulse_ctrl() -> None:
            sent = 0
            r9_cnt = 0
            r11_cnt = 0
            self.set_oe(DIO_REQ | DIO_PE3)
            self.ad2.digital_io_write(0, DIO_PE3)
            time.sleep(0.0005)
            for _ in range(200):
                rate, n = DigitalInHelper.arm(self.ad2, 100e6, 512, trig_rise_mask=DIO_PE3, prefill=50)
                self.ad2.digital_io_write(DIO_PE3, DIO_PE3)
                samples = DigitalInHelper.read(self.ad2, n, timeout_s=0.2)
                self.ad2.digital_io_write(0, DIO_PE3)
                time.sleep(0.0005)
                if samples:
                    r8 = next((i for i in range(1, len(samples))
                               if ((samples[i] >> 8) & 1) and not ((samples[i - 1] >> 8) & 1)), None)
                    if r8 is not None:
                        sent += 1
                        if any(((s >> 9) & 1) for s in samples[r8:]):
                            r9_cnt += 1
                        if any(((s >> 11) & 1) for s in samples[r8:]):
                            r11_cnt += 1
            self.set_oe(DIO_REQ)
            box3["sent"] = sent
            box3["r9"] = r9_cnt
            box3["r11"] = r11_cnt

        self.sync(pre=pulse_ctrl)
        m3 = self.con.expect(r"\[gpio\] T12 control disabled CM4: CM7=(\d+) \(exp 200\), CM4=(\d+) \(exp 0\)", 15.0)
        c7_3, c4_3 = int(m3.group(1)), int(m3.group(2))
        la3_ok = (box3.get("sent") == 200 and box3.get("r9") == 200 and box3.get("r11") == 0)
        self.check("T12 isolation control", c7_3 == 200 and c4_3 == 0 and la3_ok,
                   f"CM7={c7_3} (exp 200), CM4={c4_3} (exp 0), LA DIO9={box3.get('r9')} DIO11={box3.get('r11')} (exp 0)")
        self.con.expect(r"\[gpio\] T12 dual-core EXTI routing and isolation -> (PASS|FAIL)", 10.0)

    def t13_cm7(self) -> None:
        self.step("T13_CM7")
        latencies = self.t11_cm7_latencies
        def report_latencies() -> None:
            pass
        self.sync(pre=report_latencies)

        if latencies:
            lat_sorted = sorted(latencies)
            l_min = lat_sorted[0]
            l_p50 = lat_sorted[len(lat_sorted) // 2]
            l_max = lat_sorted[-1]
            ok_lat = len(latencies) == 200 and l_max <= 10.0
            self.check("T13_CM7 latency", ok_lat,
                       f"{len(latencies)}/200 edges (from T11): min={l_min:.2f} us, p50={l_p50:.2f} us, max={l_max:.2f} us (req <= 10 us)")
        else:
            self.check("T13_CM7 latency", False, "no response edges captured on DIO9 in T11")

        # Negative control: line disabled in firmware, verify 0 responses
        ctrl_box: dict[str, int] = {}
        def measure_ctrl() -> None:
            s_edges, r_edges, _ = self.pulse_train_measured(
                DIO_PE3, resp_dio=9, count=20, mode="RISING", expect_resp=False
            )
            ctrl_box["sent"] = s_edges
            ctrl_box["resp"] = r_edges

        self.sync(pre=measure_ctrl)
        ctrl_ok = (ctrl_box.get("sent") == 20) and (ctrl_box.get("resp") == 0)
        self.check("T13_CM7 control", ctrl_ok,
                   f"disabled line: {ctrl_box.get('resp')} transitions on DIO9 across {ctrl_box.get('sent')} pulses (expected 0)")
        self.con.expect(r"\[gpio\] T13_CM7 latency edges measured by host -> PASS", 10.0)

    def t13_cm4(self) -> None:
        self.step("T13_CM4")
        latencies = self.t11_cm4_latencies
        def report_latencies() -> None:
            pass
        self.sync(pre=report_latencies)

        if latencies:
            lat_sorted = sorted(latencies)
            l_min = lat_sorted[0]
            l_p50 = lat_sorted[len(lat_sorted) // 2]
            l_max = lat_sorted[-1]
            ok_lat = len(latencies) == 200 and l_max <= 10.0
            self.check("T13_CM4 latency", ok_lat,
                       f"{len(latencies)}/200 edges (from T11): min={l_min:.2f} us, p50={l_p50:.2f} us, max={l_max:.2f} us (req <= 10 us)")
        else:
            self.check("T13_CM4 latency", False, "no response edges captured on DIO11 in T11")

        # Negative control: line disabled in firmware, verify 0 responses
        ctrl_box: dict[str, int] = {}
        def measure_ctrl() -> None:
            s_edges, r_edges, _ = self.pulse_train_measured(
                DIO_PE4, resp_dio=11, count=20, mode="RISING", expect_resp=False
            )
            ctrl_box["sent"] = s_edges
            ctrl_box["resp"] = r_edges

        self.sync(pre=measure_ctrl)
        ctrl_ok = (ctrl_box.get("sent") == 20) and (ctrl_box.get("resp") == 0)
        self.check("T13_CM4 control", ctrl_ok,
                   f"disabled line: {ctrl_box.get('resp')} transitions on DIO11 across {ctrl_box.get('sent')} pulses (expected 0)")
        self.con.expect(r"\[gpio\] T13_CM4 latency edges measured by host -> PASS", 10.0)

    def t14(self) -> None:
        self.step("T14")
        # Subtest (a) part 1: single pulse to set pending, cleared by ExtiConfig
        self.sync(pre=lambda: self.pulse_train(DIO_PE3, 1))
        m_a1 = self.con.expect(
            r"\[gpio\] T14\(a\) Pending cleared by ExtiConfig: pr1_before=0x([0-9A-Fa-f]+) pr1_after=0x([0-9A-Fa-f]+) count=(\d+) \(exp 0\)",
            15.0
        )
        b_val, a_val, cnt_a1 = int(m_a1.group(1), 16), int(m_a1.group(2), 16), int(m_a1.group(3))
        self.check("T14(a) pending clear", b_val == 0x08 and a_val == 0 and cnt_a1 == 0,
                   f"PR1 before=0x{b_val:08X} (exp 0x08), after=0x{a_val:08X} (exp 0), ISR count={cnt_a1} (exp 0)")

        # Subtest (a) part 2: single pulse to set pending, uncleared control
        self.sync(pre=lambda: self.pulse_train(DIO_PE3, 1))
        m_a2 = self.con.expect(
            r"\[gpio\] T14\(a\) Control uncleared PR1: pr1=0x([0-9A-Fa-f]+) count=(\d+) \(exp 1\)",
            15.0
        )
        ctrl_pr1, cnt_a2 = int(m_a2.group(1), 16), int(m_a2.group(2))
        self.check("T14(a) control uncleared", ctrl_pr1 == 0x08 and cnt_a2 == 1,
                   f"PR1=0x{ctrl_pr1:08X} (exp 0x08), ISR count={cnt_a2} (exp 1)")

        # Subtest (b): Conflict detection
        m_b1 = self.con.expect(r"\[gpio\] T14\(b\) Conflict GPIOD: status=(\d+) photo_equal=(\d+)", 10.0)
        st_conf, pe_conf = int(m_b1.group(1)), int(m_b1.group(2))
        self.check("T14(b) conflict", st_conf == 6 and pe_conf == 1,
                   f"status={st_conf} (exp 6 = ERR_CONFLICT), photo_equal={pe_conf} (exp 1)")

        m_b2 = self.con.expect(r"\[gpio\] T14\(b\) Control free line routes to GPIOD: status=(\d+)", 10.0)
        st_free = int(m_b2.group(1))
        self.check("T14(b) control free line", st_free == 0, f"status={st_free} (exp 0 = OK)")

        m_b3 = self.con.expect(r"\[gpio\] T14\(b\) Restore routing back to PE: status=(\d+)", 10.0)
        st_rest = int(m_b3.group(1))
        self.check("T14(b) restore PE", st_rest == 0, f"status={st_rest} (exp 0 = OK)")

        # Subtest (c): Deinit rule 6 BOTH
        m_c = self.con.expect(
            r"\[gpio\] T14\(c\) Deinit rule 6 BOTH: shared kept=(\d+) \(cr=(\d+) rtsr=(\d+) ftsr=(\d+)\), last cleared=(\d+) \(cr=(\d+) rtsr=(\d+) ftsr=(\d+)\)",
            10.0
        )
        kept, clr = int(m_c.group(1)), int(m_c.group(5))
        self.check("T14(c) deinit rule 6", kept == 1 and clr == 1,
                   f"shared kept={kept} (exp 1), last cleared={clr} (exp 1)")

        # Subtest (d): Software trigger
        m_d1 = self.con.expect(r"\[gpio\] T14\(d\) ExtiSoftwareTrigger\(0\): count=(\d+) \(exp 100\) pend_ok=(\d+) \(exp 100\)", 10.0)
        cnt_d, pend_d = int(m_d1.group(1)), int(m_d1.group(2))
        self.check("T14(d) software trigger", cnt_d == 100 and pend_d == 100,
                   f"count={cnt_d} (exp 100), pend_ok={pend_d} (exp 100, bit 0 verified)")

        m_d2 = self.con.expect(r"\[gpio\] T14\(d\) Control masked line 0: count=(\d+) \(exp 0\)", 10.0)
        cnt_d_ctrl = int(m_d2.group(1))
        self.check("T14(d) control masked", cnt_d_ctrl == 0, f"count={cnt_d_ctrl} (exp 0)")

        m = self.con.expect(r"\[gpio\] T14 pending, conflict, deinit rule 6 and software trigger -> (PASS|FAIL)", 15.0)
        self.check("T14 EXTI features", m.group(1) == "PASS",
                   "pending clear, conflict detection, deinit rule 6, and software trigger")

    def t17(self) -> None:
        self.step("T17")
        m = self.con.expect(r"\[gpio\] T17 compensation cell without CSI \(refused\) and with CSI \(ready\) -> (PASS|FAIL)", 15.0)
        self.check("T17 compensation cell", m.group(1) == "PASS",
                   "refused without CSI (ERR_NOT_READY), READY=1 with CSI, CCCSR and CSION restored")

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
        self.t11_cm7("T11_CM7_RISING", 200)
        self.t11_cm7("T11_CM7_FALLING", 200)
        self.t11_cm7("T11_CM7_BOTH", 400)
        self.t11_cm7("T11_CM7_EVENT", 0)
        self.t11_cm4("T11_CM4_RISING", 200)
        self.t11_cm4("T11_CM4_FALLING", 200)
        self.t11_cm4("T11_CM4_BOTH", 400)
        self.t11_cm4("T11_CM4_EVENT", 0)
        self.t12()
        self.t13_cm7()
        self.t13_cm4()
        self.t14()
        self.t17()
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


def run_mock_test(log: Callable[[str], None]) -> bool:
    """Non-board test: verifies S3 guards with _MockDwf and runs all F2 steps (T11..T14, T17)
    against mock DWF and mock firmware console output, including a negative control with
    a firmware undercount that the host detects and flags as FAIL."""
    log("[mock] === Phase 1: S3 Safety Guard Tests ===")
    guard_ok = t0b_guard_selftest(log)
    log(f"[mock] T0B(a) host guard overall: {'PASS' if guard_ok else 'FAIL'}")

    log("[mock] === Phase 2: Mock Session of F2 Steps (T11..T14, T17) ===")
    fw_lines = [
        "[gpio] STEP T11_CM7_RISING bus=NONE v=1",
        "[gpio] T11_CM7_RISING: CM7 count=200 (expected 200)",
        "[gpio] T11_CM7_RISING count exact (200) -> PASS",
        "[gpio] STEP T11_CM7_FALLING bus=NONE v=1",
        "[gpio] T11_CM7_FALLING: CM7 count=200 (expected 200)",
        "[gpio] T11_CM7_FALLING count exact (200) -> PASS",
        "[gpio] STEP T11_CM7_BOTH bus=NONE v=1",
        "[gpio] T11_CM7_BOTH: CM7 count=400 (expected 400)",
        "[gpio] T11_CM7_BOTH count exact (400) -> PASS",
        "[gpio] STEP T11_CM7_EVENT bus=NONE v=1",
        "[gpio] T11_CM7_EVENT: CM7 count=0 (expected 0)",
        "[gpio] T11_CM7_EVENT count exact (0) -> PASS",
        "[gpio] STEP T11_CM4_RISING bus=NONE v=1",
        "[gpio] T11_CM4_RISING: CM4 count=200 (expected 200)",
        "[gpio] T11_CM4_RISING count exact (200) -> PASS",
        "[gpio] STEP T11_CM4_FALLING bus=NONE v=1",
        "[gpio] T11_CM4_FALLING: CM4 count=200 (expected 200)",
        "[gpio] T11_CM4_FALLING count exact (200) -> PASS",
        "[gpio] STEP T11_CM4_BOTH bus=NONE v=1",
        "[gpio] T11_CM4_BOTH: CM4 count=400 (expected 400)",
        "[gpio] T11_CM4_BOTH count exact (400) -> PASS",
        "[gpio] STEP T11_CM4_EVENT bus=NONE v=1",
        "[gpio] T11_CM4_EVENT: CM4 count=0 (expected 0)",
        "[gpio] T11_CM4_EVENT count exact (0) -> PASS",
        "[gpio] STEP T12 bus=NONE v=1",
        "[gpio] T12 independent: CM7=200 (exp 200), CM4=200 (exp 200)",
        "[gpio] T12 shared line 3: CM7=200 (exp 200), CM4=200 (exp 200)",
        "[gpio] T12 control disabled CM4: CM7=200 (exp 200), CM4=0 (exp 0)",
        "[gpio] T12 dual-core EXTI routing and isolation -> PASS",
        "[gpio] STEP T13_CM7 bus=NONE v=1",
        "[gpio] T13_CM7 latency edges measured by host -> PASS",
        "[gpio] STEP T13_CM4 bus=NONE v=1",
        "[gpio] T13_CM4 latency edges measured by host -> PASS",
        "[gpio] STEP T14 bus=NONE v=1",
        "[gpio] T14(a) Pending cleared by ExtiConfig: pr1_before=0x00000008 pr1_after=0x00000000 count=0 (exp 0)",
        "[gpio] T14(a) Control uncleared PR1: pr1=0x00000008 count=1 (exp 1)",
        "[gpio] T14(b) Conflict GPIOD: status=6 photo_equal=1",
        "[gpio] T14(b) Control free line routes to GPIOD: status=0",
        "[gpio] T14(b) Restore routing back to PE: status=0",
        "[gpio] T14(c) Deinit rule 6 BOTH: shared kept=1 (cr=4 rtsr=1 ftsr=1), last cleared=1 (cr=0 rtsr=0 ftsr=0)",
        "[gpio] T14(d) ExtiSoftwareTrigger(0): count=100 (exp 100) pend_ok=100 (exp 100)",
        "[gpio] T14(d) Control masked line 0: count=0 (exp 0)",
        "[gpio] T14 pending, conflict, deinit rule 6 and software trigger -> PASS",
        "[gpio] STEP T17 bus=NONE v=1",
        "[gpio] T17(a) Compensation cell without CSI: st=7 photo_equal=1",
        "[gpio] T17(a) Control manual EN=1 without CSI: ready_zero=1",
        "[gpio] T17(b) Compensation cell with CSI: st=0 cycles=245 CCCSR=0x00000003",
        "[gpio] T17 compensation cell without CSI (refused) and with CSI (ready) -> PASS",
        "[gpio] STEP DONE bus=NONE v=1",
        "[gpio] FW VERDICT=PASS failures=0",
    ]

    mock_dwf = _MockDwf()
    mock_ad2 = MockAD2(mock_dwf)
    mock_con = MockConsole(fw_lines, log, dwf=mock_dwf)
    session = Session(mock_ad2, mock_con, log)
    session.step_idx = [s[0] for s in STEPS].index("T11_CM7_RISING")
    session.set_oe(DIO_REQ)

    session.t11_cm7("T11_CM7_RISING", 200)
    session.t11_cm7("T11_CM7_FALLING", 200)
    session.t11_cm7("T11_CM7_BOTH", 400)
    session.t11_cm7("T11_CM7_EVENT", 0)
    session.t11_cm4("T11_CM4_RISING", 200)
    session.t11_cm4("T11_CM4_FALLING", 200)
    session.t11_cm4("T11_CM4_BOTH", 400)
    session.t11_cm4("T11_CM4_EVENT", 0)
    session.t12()
    session.t13_cm7()
    session.t13_cm4()
    session.t14()
    session.t17()
    session.step("DONE")

    f2_checks = session.checks
    for name, ok in f2_checks:
        log(f"[host] CHECK {name}: {'PASS' if ok else 'FAIL'}")
    f2_all_ok = bool(f2_checks) and all(ok for _, ok in f2_checks)
    log(f"[mock] F2 steps overall: {'PASS' if f2_all_ok else 'FAIL'} ({sum(1 for _, ok in f2_checks if ok)}/{len(f2_checks)} checks passed)")

    log("[mock] === Phase 3: Negative Control (Firmware Undercount Detection) ===")
    neg_lines = [
        "[gpio] STEP T11_CM7_RISING bus=NONE v=1",
        "[gpio] T11_CM7_RISING: CM7 count=199 (expected 200)",
        "[gpio] T11_CM7_RISING count exact (200) -> FAIL",
    ]
    neg_dwf = _MockDwf()
    neg_ad2 = MockAD2(neg_dwf)
    neg_con = MockConsole(neg_lines, log, dwf=neg_dwf)
    neg_session = Session(neg_ad2, neg_con, log)
    neg_session.step_idx = [s[0] for s in STEPS].index("T11_CM7_RISING")
    neg_session.set_oe(DIO_REQ)
    neg_session.t11_cm7("T11_CM7_RISING", 200)

    neg_detected = any(name == "T11_CM7_RISING isr count" and not ok for name, ok in neg_session.checks)
    log(f"[mock] Negative control: firmware undercount (199 vs 200) correctly flagged as FAIL: "
        f"{'PASS' if neg_detected else 'FAIL'}")

    all_passed = guard_ok and f2_all_ok and neg_detected
    log(f"[host] OVERALL MOCK VERDICT: {'PASS' if all_passed else 'FAIL'}")
    log(f"HOST VERDICT={'PASS' if all_passed else 'FAIL'}")
    return all_passed


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", help="ST-LINK VCP (default: autodetect)")
    ap.add_argument("--out", type=pathlib.Path, help="capture file (default: docs/gpio_selftest_<rev>.txt)")
    ap.add_argument("--allow-dirty", action="store_true", help="accept a -dirty Rev (development runs)")
    ap.add_argument("--no-reset", action="store_true", help="do not reset the board; wait for the banner")
    ap.add_argument("--preset", default="selftest-gpio", help="configure preset of the image (build id)")
    ap.add_argument("--mock-test", action="store_true", help="run non-board mock test of F2 steps and S3 guard")
    args = ap.parse_args()

    lines: list[str] = []
    t0 = time.monotonic()
    lock = threading.Lock()

    def stamp(text: str) -> None:
        entry = f"[+{time.monotonic() - t0:8.3f}s] {text}"
        with lock:
            lines.append(entry)
        print(entry, flush=True)

    # R4-b: what the run was asked to do, first. argv as given (shell-quoted),
    # then the options as argparse resolved them, defaults included.
    stamp(f"[host] argv: {shlex.join(sys.argv[1:])}")
    stamp(f"[host] options: allow_dirty={args.allow_dirty} preset={args.preset} no_reset={args.no_reset} mock_test={args.mock_test}")
    stamp(f"[host] gpio_hw_check.py, host checkout {git_rev()}, protocol v{BENCH_VERSION}")

    if args.mock_test:
        stamp("[host] running non-board mock test suite for Phase 00_gpio F2")
        tree_record(stamp)
        mock_ok = run_mock_test(stamp)
        out = args.out
        if out is None:
            rev = git_rev()
            out = PROJECT / "docs" / f"gpio_f2_mock_{rev}.txt"
        out.parent.mkdir(parents=True, exist_ok=True)
        with lock:
            out.write_text("\n".join(lines) + "\n", encoding="utf-8")
        print(f"capture: {out}")
        return 0 if mock_ok else 1
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
            if ver != BENCH_VERSION:
                raise BenchAbort(f"S8: firmware protocol v{ver}, host v{BENCH_VERSION}")
            # R4-a, R3-b: the two cores are one build of this checkout's commit.
            # --allow-dirty waives none of this.
            head = git_rev()
            base = {c: (r[:-len("-dirty")] if r.endswith("-dirty") else r) for c, r in revs.items()}
            stamp(f"[host] R4-a: cm7 Rev={revs['cm7']} id={ids['cm7']}; cm4 Rev={revs['cm4']} id={ids['cm4']}; "
                  f"host HEAD={head} id={build_id}")
            if revs["cm7"] != revs["cm4"]:
                raise BenchAbort(f"R4-a: cm7 and cm4 come from different trees "
                                 f"(Rev cm7={revs['cm7']} cm4={revs['cm4']})")
            if ids["cm7"] != ids["cm4"] or "unknown" in (ids["cm7"], ids["cm4"]):
                raise BenchAbort(f"R4-a: cm7 and cm4 are not one build "
                                 f"(id cm7={ids['cm7']} cm4={ids['cm4']})")
            for core in ("cm7", "cm4"):
                if head == "unknown" or base[core] != head:
                    raise BenchAbort(f"R3-b: {core} was built at {base[core]} (Rev {revs[core]}), "
                                     f"this checkout is at {head}")
            # What --allow-dirty may waive: a -dirty Rev (R2), and a host id that
            # differs from the image's (R3-a). "unknown" never matches.
            dirty = revs["cm7"].endswith("-dirty")
            if dirty and not args.allow_dirty:
                raise BenchAbort(f"R2: Rev cm7={revs['cm7']} cm4={revs['cm4']} is not one clean commit")
            stamp(f"[host] build id host={build_id} cm7={ids['cm7']} cm4={ids['cm4']}")
            if build_id == "unknown" or ids["cm7"] != build_id:
                if not args.allow_dirty:
                    raise BenchAbort(f"R3-a: the image is not the one this checkout builds "
                                     f"(host={build_id} cm7={ids['cm7']} cm4={ids['cm4']})")
                stamp("[host] R3-a: build ids differ from the host's; --allow-dirty given, going on")
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
