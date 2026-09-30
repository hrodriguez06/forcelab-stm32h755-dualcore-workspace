#!/usr/bin/env python3
"""GPIO Hardware Selftest Runner for STM32H755 and Analog Discovery 2.

Implements the test orchestrator for 00_gpio (Phase F0):
  - Enforces S9 operational checklist with explicit operator confirmation (--checklist-confirmed, m3).
  - Safe state S2 (all inputs, DwfParamOnClose = 1) via AD2 context manager.
  - S10: Safe target MCU reset only when AD2 OE is confirmed 0x0000 (reset_mcu_safe).
  - S8: Banner validation (Rev <hash> clean and GPIO_BENCH v<proto> idle) before enabling ANY outputs.
  - S7: Synchronized step table tracking with regex matching against firmware announcements.
  - S5: Sentinel-guarded BusToken and drive_bus() with token consumption.
  - R-B2: Single-source LineReader with FIFO queue and strict expect() matching.
  - R-B1: Two handshake primitives only (HS_MCU and HS_HOST) with 10s phase timeouts.
  - T0b(a): Host OE mask guard verification against prohibited bits.
  - T0b(c): S5 contention validation via s5_verify_released() (B4).
  - T0b(d): 10-second timeout to bench_safe().
  - T0a: Loose line check (13 lines), handshake lines check (PE2/PE7), walking-1 in both directions, scope 1/2 DC levels.
  - F1a (protocol v2): T15/T1/T2/B1/T16 results from the firmware, and the AD2 tests
    T3 (LA), T4 (CRC32 of 256 patterns), T7, T8 (LA), T5/T6 (scope DC), T9 (scope
    100 MS/s) and T10 (LA frequency), each with its negative control.
  - Evaluates and prints the explicit host checklist and final HOST VERDICT=.
"""

from __future__ import annotations

import argparse
import ast
from dataclasses import dataclass
import os
import pathlib
import re
import subprocess
import sys
import time
from typing import Callable, Optional, Sequence

# Add workspace root to sys.path so we can import tools.ad2 and tools.monitor
HERE = pathlib.Path(__file__).resolve()
PROJECT_ROOT = HERE.parent.parent
WORKSPACE_ROOT = PROJECT_ROOT.parents[1]
sys.path.insert(0, str(WORKSPACE_ROOT))

from tools.ad2 import (
    AD2,
    AD2Error,
    AD2SafetyViolation,
    GPIO_BENCH_BUS_NONE,
    GPIO_BENCH_BUS_MCU,
    GPIO_BENCH_BUS_AD2,
    AD2_DIO_BUS_MASK,
    AD2_DIO_EXTI3_CM7,
    AD2_DIO_CM7_ACK,
    AD2_DIO_EXTI4_CM4,
    AD2_DIO_CM4_ACK,
    AD2_DIO_HOST_REQ,
    AD2_DIO_MCU_ACK,
    AD2_DIO_MCO1_PA8,
)
from tools.monitor import SerialPort


# ---- Protocol Constants & Table Parser (gpio_bench_protocol.h) -------------
PROTOCOL_HEADER = PROJECT_ROOT / "Common" / "Inc" / "gpio_bench_protocol.h"


def load_protocol_version(header_path: Optional[pathlib.Path] = None) -> int:
    """GPIO_BENCH_PROTOCOL_VERSION from the header: one source for both sides."""
    text = (header_path or PROTOCOL_HEADER).read_text(encoding="utf-8")
    m = re.search(r"#define\s+GPIO_BENCH_PROTOCOL_VERSION\s+(\d+)U?", text)
    if not m:
        raise RuntimeError("GPIO_BENCH_PROTOCOL_VERSION not found in gpio_bench_protocol.h")
    return int(m.group(1))


GPIO_BENCH_PROTOCOL_VERSION = load_protocol_version()

STEP_T15_RACE       = 1
STEP_IDLE           = 2
STEP_T0B_GUARDS     = 3
STEP_T0B_S5_NEG     = 4
STEP_T0B_TIMEOUT    = 5
STEP_T0A_LOOSE      = 6
STEP_T0A_HANDSHAKE  = 7
STEP_T0A_MCU_TO_AD2 = 8
STEP_T0A_AD2_TO_MCU = 9
STEP_T0A_SCOPE      = 10
STEP_T1_REJECT      = 11
STEP_T2_PARITY      = 12
STEP_B1_MEASURE     = 13
STEP_T16_LCKR       = 20
STEP_DONE           = 99

AD2_DIO_LOOSE_13_MASK = (
    AD2_DIO_BUS_MASK | AD2_DIO_EXTI3_CM7 | AD2_DIO_CM7_ACK |
    AD2_DIO_EXTI4_CM4 | AD2_DIO_CM4_ACK | AD2_DIO_MCO1_PA8
)

BUS_OWNER_NAMES = {
    GPIO_BENCH_BUS_NONE: "NONE",
    GPIO_BENCH_BUS_MCU:  "MCU",
    GPIO_BENCH_BUS_AD2:  "AD2",
}

BUS_OWNER_FROM_NAME = {
    "NONE": GPIO_BENCH_BUS_NONE,
    "MCU":  GPIO_BENCH_BUS_MCU,
    "AD2":  GPIO_BENCH_BUS_AD2,
}


@dataclass(frozen=True)
class StepDef:
    step_id: int
    name: str
    bus_owner: str


@dataclass(frozen=True)
class BenchItem:
    step_id: int
    item_type: str         # 'LINE', 'PRIM', 'MEASURE'
    primitive: str         # 'NONE', 'HS_MCU', 'HS_HOST'
    name: str
    line_pattern: str
    bit_arg: int
    repeat: int = 1        # the item occurs this many times in a row


def load_protocol_from_header(header_path: Optional[pathlib.Path] = None) -> tuple[list[StepDef], list[BenchItem]]:
    """Parse protocol step definitions and items from gpio_bench_protocol.h (R2-M3)."""
    if header_path is None:
        header_path = PROJECT_ROOT / "Common" / "Inc" / "gpio_bench_protocol.h"
    text = header_path.read_text(encoding="utf-8")

    steps: list[StepDef] = []
    step_pattern = re.compile(r'X\s*\(\s*(\d+)\s*,\s*"([^"]*)"\s*,\s*GPIO_BENCH_BUS_(\w+)\s*\)')
    for m in step_pattern.finditer(text):
        steps.append(StepDef(step_id=int(m.group(1)), name=m.group(2), bus_owner=m.group(3)))

    items: list[BenchItem] = []
    item_pattern = re.compile(
        r'X\s*\(\s*(\d+)\s*,\s*BENCH_ITEM_(\w+)\s*,\s*GPIO_BENCH_PRIM_(\w+)\s*,\s*"([^"]*)"\s*,\s*("(?:[^"\\]|\\.)*")\s*,\s*(0x[0-9a-fA-F]+|\d+)\s*,\s*(\d+)\s*\)'
    )
    for m in item_pattern.finditer(text):
        raw_bit = m.group(6)
        bit_val = int(raw_bit, 16) if raw_bit.startswith("0x") else int(raw_bit)
        raw_quoted = m.group(5)
        pattern = ast.literal_eval(raw_quoted)
        items.append(BenchItem(
            step_id=int(m.group(1)),
            item_type=m.group(2),
            primitive=m.group(3),
            name=m.group(4),
            line_pattern=pattern,
            bit_arg=bit_val,
            repeat=int(m.group(7)),
        ))

    return steps, items


PROTOCOL_STEPS, PROTOCOL_ITEMS = load_protocol_from_header()
EXPECTED_STEP_SEQUENCE = [(s.step_id, s.bus_owner) for s in PROTOCOL_STEPS]


def table_item(name: str) -> BenchItem:
    """Return BenchItem from PROTOCOL_ITEMS matching unique item name (R2-M3)."""
    for it in PROTOCOL_ITEMS:
        if it.name == name:
            return it
    raise KeyError(f"No protocol item with name '{name}'")


def table_pattern(name: str) -> str:
    """Return unescaped regex line pattern for protocol item from table (R2-M3)."""
    return table_item(name).line_pattern

STEP_RE = re.compile(r"^\[m7_gpio_test\] STEP id=(\d+) bus=(NONE|MCU|AD2) v=(\d+)")

# ---- F1a bench analysis (pure functions; the oracle is the AD2 data) --------

STEP_T3_MCU_OPS      = 30
STEP_T4_AD2_PATTERNS = 31
STEP_T7_ANALOG_IDR   = 32
STEP_T8_NO_GLITCH    = 33
STEP_T5_PULLS        = 34
STEP_T6_OPEN_DRAIN   = 35
STEP_T9_RISE_TIME    = 36
STEP_T10_MCO1        = 37

# T3: the bus values the firmware's burst must leave, in order, after the
# starting 0x00. Written out here, independently of gpio_selftest.c.
T3_EXPECTED: list[int] = (
    [v for b in range(8) for v in (1 << b, 0x00)]            # Set / Reset walking 1
    + [0xFF]                                                   # WritePort all ones
    + [v for b in range(8) for v in (0xFF & ~(1 << b), 0xFF)]  # Write walking 0
    + [0x55, 0xAA, 0x0F, 0xF0, 0x00]                           # WritePort, full mask
    + [0x05, 0xA5, 0x00]                                       # WritePort, partial masks
    + [0xFF, 0xF0, 0x00]                                       # Toggle
    + [0x01, 0x81, 0x00]                                       # set + reset in one write: set wins
)
T3_CONTROL_INDEX = T3_EXPECTED.index(0x55)                     # the value the control gets wrong
T3_LA_RATE_HZ = 25e6
T3_LA_SAMPLES = 4096
T3_MIN_RUN = 10                                                # 400 ns: shorter runs are edge skew

T8_LA_RATE_HZ = 100e6
T8_LA_SAMPLES = 4096
# case -> (expected DIO9 transitions, description). E is the naive-order control.
T8_CASES = {
    "A": (1, "input PD -> output high"),
    "B": (1, "input PU -> output low"),
    "C": (0, "input PU -> output high, stale ODR 0"),
    "D": (0, "input PD -> output low, stale ODR 1"),
    "E": (2, "control: C in naive order (MODER, 1 us, BSRR)"),
}

T5_RX_OHMS = 22000.0
T5_R_MIN, T5_R_MAX = 30e3, 50e3                                # DS12923 table 59

T9_RATE_HZ = 100e6
T9_SAMPLES = 8192
T9_EDGES = 10
T10_EXPECTED_HZ = 16e6
T10_TOLERANCE = 0.02


def crc32_bytes(values) -> int:
    import zlib
    return zlib.crc32(bytes(v & 0xFF for v in values)) & 0xFFFFFFFF


def la_stable_sequence(samples: list[int], mask: int, shift: int, min_run: int) -> list[int]:
    """Values of (sample & mask) >> shift that last at least `min_run` samples,
    with consecutive repeats merged."""
    runs: list[list[int]] = []
    for s in samples:
        v = (s & mask) >> shift
        if runs and runs[-1][0] == v:
            runs[-1][1] += 1
        else:
            runs.append([v, 1])
    out: list[int] = []
    for v, n in runs:
        if n >= min_run and (not out or out[-1] != v):
            out.append(v)
    return out


def la_transitions(samples: list[int], bit: int) -> int:
    count = 0
    prev = None
    for s in samples:
        v = (s >> bit) & 1
        if prev is not None and v != prev:
            count += 1
        prev = v
    return count


def la_frequency(samples: list[int], bit: int, rate_hz: float) -> tuple[float, int]:
    """Frequency from the rising edges of `bit`: (hz, number of rising edges)."""
    rises = [i for i in range(1, len(samples))
             if ((samples[i] >> bit) & 1) and not ((samples[i - 1] >> bit) & 1)]
    if len(rises) < 2:
        return 0.0, len(rises)
    return (len(rises) - 1) * rate_hz / (rises[-1] - rises[0]), len(rises)


def _lanczos(x: float, a: int) -> float:
    import math
    if x == 0.0:
        return 1.0
    if abs(x) >= a:
        return 0.0
    px = math.pi * x
    return a * math.sin(px) * math.sin(px / a) / (px * px)


def rise_time_10_90(v: list[float], rate_hz: float, pre_fraction: float,
                    upsample: int = 20, lanczos_a: int = 6, half_window: int = 24) -> Optional[float]:
    """10-90 % rise time in seconds.

    Levels come from the flat parts of the capture: the median of the first half
    of the pre-trigger samples, and of the last 20 % of the buffer. At 100 MS/s a
    15 ns edge spans under two samples, so a straight line between samples makes
    the result depend on the sampling phase (a +3 ns bias and 1-2 ns of spread
    in the first F1a run). The scope front end limits the signal to about 30 MHz,
    under the 50 MHz Nyquist limit, so the edge is reconstructed from its samples
    with a Lanczos-windowed sinc (a = 6), upsampled 20x over +-24 samples around
    the 50 % crossing, and the 10 % and 90 % crossings are taken on that.
    """
    import math
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



class BenchAbort(Exception):
    """Exception raised when a bench safety guard or sequence check fails."""
    pass


class BusContention(BenchAbort):
    """Raised specifically when bus contention is detected during S5 check."""
    def __init__(
        self,
        value: int,
        msg: str = "",
        announced: Optional[int] = None,
        oe_checks: Optional[tuple[int, int, int]] = None,
    ):
        self.value = value
        self.announced = announced
        self.oe_checks = oe_checks
        super().__init__(msg or f"Bus contention detected: read 0x{value:02X}")


# Sentinel object for private BusToken creation
_TOKEN_SENTINEL = object()


class BusToken:
    """S5 witness token. Authorizes driving the bus for a specific validated step.
    Can ONLY be instantiated with the module-private _TOKEN_SENTINEL.
    """

    def __init__(self, step_id: int, sentinel: object):
        if sentinel is not _TOKEN_SENTINEL:
            raise AD2SafetyViolation("S5 violation: BusToken cannot be constructed directly. Use s5_verify_released.")
        self.step_id = step_id
        self._valid = True

    def invalidate(self) -> None:
        self._valid = False

    @property
    def is_valid(self) -> bool:
        return self._valid


class StepTracker:
    """S7: Synchronized step table tracker. Validates sequence and bus ownership."""

    def __init__(self, sequence: list[tuple[int, str]]):
        self.sequence = sequence
        self.current_idx = 0
        self.current_step = 0
        self.current_bus_owner = GPIO_BENCH_BUS_NONE
        self.current_token: Optional[BusToken] = None
        self._aborted = False

    def abort(self) -> None:
        self._aborted = True
        if self.current_token:
            self.current_token.invalidate()
            self.current_token = None

    def invalidate_token(self) -> None:
        if self.current_token:
            self.current_token.invalidate()
            self.current_token = None

    def advance_step(self, step_id: int, bus_owner: int) -> None:
        if self.current_token:
            self.current_token.invalidate()
            self.current_token = None
        self.current_step = step_id
        self.current_bus_owner = bus_owner

    def set_token(self, token: BusToken) -> None:
        if self._aborted:
            raise AD2SafetyViolation("Cannot set token on aborted tracker")
        self.current_token = token

    def validate_announcement(self, line: str, ad2: AD2, log_fn: Optional[Callable[[str], None]] = None) -> tuple[int, int]:
        m = STEP_RE.match(line)
        if not m:
            return 0, GPIO_BENCH_BUS_NONE

        step_id = int(m.group(1))
        bus_str = m.group(2)
        proto_v = int(m.group(3))

        if proto_v != GPIO_BENCH_PROTOCOL_VERSION:
            self.abort()
            ad2.set_safe_state()
            msg = f"ABORT: Protocol version mismatch in step announcement (got v={proto_v}, expected v={GPIO_BENCH_PROTOCOL_VERSION})"
            if log_fn:
                log_fn(f"[host] {msg}")
            raise BenchAbort(msg)

        if self.current_idx >= len(self.sequence):
            self.abort()
            ad2.set_safe_state()
            msg = f"ABORT: Unexpected step announcement beyond end of sequence (step {step_id})"
            if log_fn:
                log_fn(f"[host] {msg}")
            raise BenchAbort(msg)

        exp_id, exp_bus = self.sequence[self.current_idx]
        if step_id != exp_id or bus_str != exp_bus:
            self.abort()
            ad2.set_safe_state()
            msg = f"ABORT: Step sequence error: got STEP id={step_id} bus={bus_str}, expected id={exp_id} bus={exp_bus}"
            if log_fn:
                log_fn(f"[host] {msg}")
            raise BenchAbort(msg)

        self.current_idx += 1
        bus_owner_val = BUS_OWNER_FROM_NAME[bus_str]
        self.advance_step(step_id, bus_owner_val)
        if log_fn:
            log_fn(f"[host] Step {step_id} validated (bus={bus_str}, v={proto_v}).")
        return self.current_step, self.current_bus_owner


def drive_bus(ad2: AD2, tracker: StepTracker, value: int) -> None:
    """S5: Drive value on bus DIO0..DIO7. Requires a valid, unconsumed token for current step."""
    if tracker._aborted:
        raise AD2SafetyViolation("S5 violation: drive_bus called after tracker abort")
    if tracker.current_bus_owner != GPIO_BENCH_BUS_AD2:
        raise AD2SafetyViolation(f"S5 violation: drive_bus called when bus owner is {tracker.current_bus_owner}, expected AD2")
    if tracker.current_token is None or not tracker.current_token.is_valid:
        raise AD2SafetyViolation("S5 violation: drive_bus called without valid BusToken")
    if tracker.current_token.step_id != tracker.current_step:
        raise AD2SafetyViolation(
            f"S5 violation: BusToken step {tracker.current_token.step_id} does not match current step {tracker.current_step}"
        )

    ad2.set_oe(AD2_DIO_HOST_REQ | AD2_DIO_BUS_MASK, GPIO_BENCH_BUS_AD2)
    ad2.digital_io_write(value & 0xFF, AD2_DIO_BUS_MASK)


class RealClock:
    """Default wall-clock implementation."""
    @staticmethod
    def monotonic() -> float:
        return time.monotonic()

    @staticmethod
    def sleep(seconds: float) -> None:
        time.sleep(seconds)


class LineReader:
    """R-B2: Queued Serial Line Reader.
    Sole consumer of ser.read(). Buffers incoming bytes, splits on newline,
    logs every line exactly once in order of arrival, and categorizes into:
      1. Protocol: starts with [m7_gpio_test] and not INFO. Queued for expect().
      2. Informative: [m7_gpio_test] INFO ... Logged and discarded from queue.
      3. Other: [m4_gpio_test], [ipc], etc. Logged; m4 saved for verdict check.
    Supports automatic tracing of expect() lines (R2-M3) and immediate abort detection (R3-m1).
    """

    def __init__(
        self,
        ser,
        clock=RealClock(),
        log_fn: Optional[Callable[[str], None]] = None,
        on_abort_fn: Optional[Callable[[], None]] = None,
        on_m4_line_fn: Optional[Callable[[str], None]] = None,
        trace: Optional[list] = None,
    ):
        self.ser = ser
        self.clock = clock
        self.log_fn = log_fn or (lambda msg: None)
        self.on_abort_fn = on_abort_fn
        self.on_m4_line_fn = on_m4_line_fn
        self.trace = trace
        self._pending_bytes = bytearray()
        self._protocol_queue: list[str] = []
        self.m4_lines: list[str] = []
        self.other_lines: list[str] = []

    def _process_chunk(self, chunk: bytes) -> None:
        if not chunk:
            return
        self._pending_bytes.extend(chunk)
        while b"\n" in self._pending_bytes:
            pos = self._pending_bytes.index(b"\n")
            raw_line = self._pending_bytes[:pos]
            del self._pending_bytes[:pos + 1]

            text = raw_line.rstrip(b"\r").decode("utf-8", errors="replace")
            self.log_fn(text)

            if text.startswith("[m7_gpio_test] ABORT"):
                if self.on_abort_fn:
                    self.on_abort_fn()
                raise BenchAbort(f"ABORT received from firmware: {text}")

            if text.startswith("[m7_gpio_test]"):
                if text.startswith("[m7_gpio_test] INFO"):
                    # Informative: logged and discarded
                    pass
                else:
                    # Protocol line: queued for expect()
                    self._protocol_queue.append(text)
            elif text.startswith("[m4_gpio_test]"):
                self.m4_lines.append(text)
                if self.on_m4_line_fn:
                    self.on_m4_line_fn(text)
            else:
                self.other_lines.append(text)

    def poll(self) -> None:
        """Read any currently pending bytes without blocking."""
        chunk = self.ser.read()
        if chunk:
            self._process_chunk(chunk)

    def expect(self, regex_pattern: str, timeout_s: float = 10.0, record_trace: bool = True) -> re.Match[str]:
        """Wait for and return the next protocol line matching regex_pattern.
        If record_trace is True and self.trace is set, automatically records ("LINE", regex_pattern).
        If an unexpected protocol line arrives, or timeout expires:
        immediately calls on_abort_fn and raises BenchAbort. Never returns None.
        """
        if record_trace and self.trace is not None:
            self.trace.append(("LINE", regex_pattern))

        pattern = re.compile(regex_pattern)
        start = self.clock.monotonic()

        while self.clock.monotonic() - start < timeout_s:
            if self._protocol_queue:
                line = self._protocol_queue.pop(0)
                m = pattern.search(line)
                if m:
                    return m
                else:
                    if self.on_abort_fn:
                        self.on_abort_fn()
                    raise BenchAbort(
                        f"ABORT: Unexpected protocol line received: '{line}' (expected pattern: '{regex_pattern}')"
                    )

            chunk = self.ser.read()
            if chunk:
                self._process_chunk(chunk)
                continue
            self.clock.sleep(0.001)

        if self.on_abort_fn:
            self.on_abort_fn()
        raise BenchAbort(f"ABORT: Timeout waiting for protocol line matching '{regex_pattern}'")


# ---- R-B1 Handshake Primitives ----------------------------------------------

def hs_mcu(
    ad2: AD2,
    clock=RealClock(),
    log_fn: Optional[Callable[[str], None]] = None,
    timeout_s: float = 10.0,
    line_reader: Optional[LineReader] = None,
    trace: Optional[list] = None,
    on_ack_fn: Optional[Callable[[], None]] = None,
) -> None:
    """R-B1 HS_MCU (MCU signals):
    MCU asserts MCU_ACK=1 -> Host asserts HOST_REQ=1 -> MCU drops MCU_ACK=0 -> Host drops HOST_REQ=0.
    Polls line_reader during wait states to detect immediate firmware ABORT (R3-m1).
    Automatically records ("PRIM", "HS_MCU") into trace (R2-M3 Form b).
    Enforces timeout_s on each phase. On timeout: aborts to S2 and raises BenchAbort.
    """
    t = trace if trace is not None else (line_reader.trace if line_reader is not None else None)
    if t is not None:
        t.append(("PRIM", "HS_MCU"))

    # Phase 1: Wait for MCU_ACK == 1
    start = clock.monotonic()
    while not bool(ad2.digital_io_read() & AD2_DIO_MCU_ACK):
        if line_reader is not None:
            line_reader.poll()
        if clock.monotonic() - start >= timeout_s:
            ad2.set_safe_state()
            msg = "ABORT: Timeout in HS_MCU Phase 1 (waiting for MCU_ACK=1)"
            if log_fn:
                log_fn(f"[ad2] {msg}")
            raise BenchAbort(msg)
        clock.sleep(0.0005)

    if on_ack_fn is not None:
        on_ack_fn()

    # Phase 2: Host asserts HOST_REQ = 1
    ad2.digital_io_write(AD2_DIO_HOST_REQ, AD2_DIO_HOST_REQ)

    # Phase 3: Wait for MCU_ACK == 0
    start = clock.monotonic()
    while bool(ad2.digital_io_read() & AD2_DIO_MCU_ACK):
        if line_reader is not None:
            line_reader.poll()
        if clock.monotonic() - start >= timeout_s:
            ad2.set_safe_state()
            msg = "ABORT: Timeout in HS_MCU Phase 3 (waiting for MCU_ACK=0)"
            if log_fn:
                log_fn(f"[ad2] {msg}")
            raise BenchAbort(msg)
        clock.sleep(0.0005)

    # Phase 4: Host drops HOST_REQ = 0
    ad2.digital_io_write(0, AD2_DIO_HOST_REQ)


def hs_host(
    ad2: AD2,
    clock=RealClock(),
    log_fn: Optional[Callable[[str], None]] = None,
    timeout_s: float = 10.0,
    line_reader: Optional[LineReader] = None,
    trace: Optional[list] = None,
) -> None:
    """R-B1 HS_HOST (Host signals):
    Host asserts HOST_REQ=1 -> MCU asserts MCU_ACK=1 -> Host drops HOST_REQ=0 -> MCU drops MCU_ACK=0.
    Polls line_reader during wait states to detect immediate firmware ABORT (R3-m1).
    Automatically records ("PRIM", "HS_HOST") into trace (R2-M3 Form b).
    Enforces timeout_s on each phase. On timeout: aborts to S2 and raises BenchAbort.
    """
    t = trace if trace is not None else (line_reader.trace if line_reader is not None else None)
    if t is not None:
        t.append(("PRIM", "HS_HOST"))

    # Phase 1: Host asserts HOST_REQ = 1
    ad2.digital_io_write(AD2_DIO_HOST_REQ, AD2_DIO_HOST_REQ)

    # Phase 2: Wait for MCU_ACK == 1
    start = clock.monotonic()
    while not bool(ad2.digital_io_read() & AD2_DIO_MCU_ACK):
        if line_reader is not None:
            line_reader.poll()
        if clock.monotonic() - start >= timeout_s:
            ad2.set_safe_state()
            msg = "ABORT: Timeout in HS_HOST Phase 2 (waiting for MCU_ACK=1)"
            if log_fn:
                log_fn(f"[ad2] {msg}")
            raise BenchAbort(msg)
        clock.sleep(0.0005)

    # Phase 3: Host drops HOST_REQ = 0
    ad2.digital_io_write(0, AD2_DIO_HOST_REQ)

    # Phase 4: Wait for MCU_ACK == 0
    start = clock.monotonic()
    while bool(ad2.digital_io_read() & AD2_DIO_MCU_ACK):
        if line_reader is not None:
            line_reader.poll()
        if clock.monotonic() - start >= timeout_s:
            ad2.set_safe_state()
            msg = "ABORT: Timeout in HS_HOST Phase 4 (waiting for MCU_ACK=0)"
            if log_fn:
                log_fn(f"[ad2] {msg}")
            raise BenchAbort(msg)
        clock.sleep(0.0005)


def reset_mcu_safe(ad2: AD2, log_fn: Optional[Callable[[str], None]] = None) -> None:
    """S10: Safe reset of target MCU. Verifies OE == 0x0000 before invoking programmer CLI.
    Raises BenchAbort if OE != 0 or if CLI returns non-zero.
    """
    oe = ad2.get_oe()
    if oe != 0:
        raise AD2SafetyViolation(f"S10 safety violation: AD2 OE is 0x{oe:04X}, expected 0x0000 for safe reset")
    if log_fn:
        log_fn(f"[host] S10: Confirmed AD2 in S2 safe state (OE=0x{oe:04X}). Resetting target MCU...")

    cli_path = "STM32_Programmer_CLI"
    cube_paths = [
        pathlib.Path(r"C:\ST\STM32CubeIDE_2.2.0\STM32CubeIDE\plugins\com.st.stm32cube.ide.mcu.externaltools.cubeprogrammer.win32_2.2.500.202603051304\tools\bin\STM32_Programmer_CLI.exe"),
    ]
    for p in cube_paths:
        if p.is_file():
            cli_path = str(p)
            break

    cmd = [cli_path, "-c", "port=SWD", "mode=HOTPLUG", "-rst"]
    res = subprocess.run(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=5)
    if res.returncode != 0:
        raise BenchAbort(f"S10 reset failed: programmer CLI returned {res.returncode}")
    if log_fn:
        log_fn(f"[host] S10: Target MCU reset via {cli_path} (exit code {res.returncode}).")


def get_git_rev() -> str:
    """Return clean short git revision of HEAD. Appends -dirty if working tree is dirty."""
    res = subprocess.run(["git", "rev-parse", "--short", "HEAD"], capture_output=True, text=True, check=True)
    rev = res.stdout.strip()
    diff_res = subprocess.run(["git", "diff-index", "--quiet", "HEAD", "--"], capture_output=True)
    if diff_res.returncode != 0:
        rev += "-dirty"
    return rev


def s5_verify_released(
    ad2: AD2,
    tracker: StepTracker,
    step_id: int,
    line_reader: LineReader,
    clock=RealClock(),
    log_fn: Optional[Callable[[str], None]] = None,
    rec_fn: Optional[Callable[[str], None]] = None,
    up_pattern: Optional[str] = None,
    down_pattern: Optional[str] = None,
) -> BusToken:
    """S5: Break-before-make electrical verification of bus release.
    1. Read-back OE & 0x00FF == 0 (Point 1: step start).
    2. Wait for BUSCHK up line (pattern obtained from table).
    3. Read-back OE & 0x00FF == 0 (Point 2: pre-read).
    4. Read bus DIO0..DIO7. If != 0xFF:
       Force S2, record OE values, and raise BusContention(bus_up, msg, announced, oe_checks).
    5. Handshake HS_MCU (automatically traced).
    6. Wait for BUSCHK down line (pattern obtained from table).
    7. Read bus DIO0..DIO7. If != 0x00:
       Force S2, record OE values, and raise BenchAbort(msg).
    8. Handshake HS_MCU (automatically traced).
    9. Creates and installs valid BusToken in tracker.
    """
    def step_pattern(kind: str) -> str:
        try:
            return table_pattern(f"step{step_id}_buschk_{kind}")
        except KeyError:
            return table_pattern(f"step6_buschk_{kind}")

    if up_pattern is None:
        if step_id == STEP_T0B_S5_NEG:
            up_pattern = table_pattern("step4_buschk_drive")
        else:
            up_pattern = step_pattern("up")

    if down_pattern is None:
        down_pattern = step_pattern("down")

    oe_start = ad2.get_oe() & AD2_DIO_BUS_MASK
    if oe_start != 0:
        tracker.invalidate_token()
        ad2.set_safe_state()
        msg = f"ABORT: S5 contention check failed at step {step_id}: AD2 OE has bus bits enabled at start (0x{oe_start:04X})"
        if log_fn:
            log_fn(f"[ad2] {msg}")
        raise BenchAbort(msg)

    # 1. Wait for BUSCHK up line
    m_up = line_reader.expect(up_pattern, timeout_s=10.0)
    m_drive = re.search(r"drive=(0x[0-9a-fA-F]+)", m_up.group(0))
    announced = int(m_drive.group(1), 16) if m_drive else None

    oe_pre = ad2.get_oe() & AD2_DIO_BUS_MASK
    dio_up = ad2.digital_io_read()
    bus_up = dio_up & AD2_DIO_BUS_MASK

    if bus_up != 0xFF:
        tracker.invalidate_token()
        ad2.set_safe_state()
        oe_post = ad2.get_oe() & AD2_DIO_BUS_MASK
        msg = (
            f"ABORT: bus contention check failed at step {step_id}: "
            f"BUSCHK up gave 0x{bus_up:02X}, expected 0xFF (OE start=0x{oe_start:02X}, pre=0x{oe_pre:02X}, post=0x{oe_post:02X})"
        )
        if log_fn:
            log_fn(f"[ad2] {msg}")
        raise BusContention(bus_up, msg, announced=announced, oe_checks=(oe_start, oe_pre, oe_post))

    hs_mcu(ad2, clock, log_fn=log_fn, timeout_s=10.0, line_reader=line_reader)

    # 2. Wait for BUSCHK down line
    line_reader.expect(down_pattern, timeout_s=10.0)

    oe_pre_down = ad2.get_oe() & AD2_DIO_BUS_MASK
    dio_down = ad2.digital_io_read()
    bus_down = dio_down & AD2_DIO_BUS_MASK

    if bus_down != 0x00:
        tracker.invalidate_token()
        ad2.set_safe_state()
        oe_post = ad2.get_oe() & AD2_DIO_BUS_MASK
        msg = (
            f"ABORT: bus contention check failed at step {step_id}: "
            f"BUSCHK down gave 0x{bus_down:02X}, expected 0x00 (OE start=0x{oe_start:02X}, pre=0x{oe_pre_down:02X}, post=0x{oe_post:02X})"
        )
        if log_fn:
            log_fn(f"[ad2] {msg}")
        raise BenchAbort(msg)

    hs_mcu(ad2, clock, log_fn=log_fn, timeout_s=10.0, line_reader=line_reader)

    token = BusToken(step_id, _TOKEN_SENTINEL)
    tracker.set_token(token)
    return token


# ---- Main Test Runner -------------------------------------------------------

def run_selftest(
    serial_port,
    ad2: AD2,
    clock=RealClock(),
    reset_fn: Optional[Callable[[AD2, Optional[Callable[[str], None]]], None]] = None,
    expected_rev: Optional[str] = None,
    log_path: Optional[pathlib.Path] = None,
    checklist_confirmed: bool = False,
    log_fn: Optional[Callable[[str], None]] = None,
    visited_items: Optional[list[str]] = None,
    trace: Optional[list] = None,
) -> int:
    """Executes the full GPIO selftest suite with dependency injection.
    Accepts simulated mocks in testing or real hardware objects in production.
    """
    log_file = None
    if log_path:
        log_file = open(log_path, "w", encoding="utf-8", buffering=1)

    if expected_rev is None:
        expected_rev = get_git_rev()

    if trace is None:
        trace = []

    def log(msg: str) -> None:
        stamp = f"[{time.strftime('%H:%M:%S')}] "
        line = stamp + msg
        try:
            print(line, flush=True)
        except (UnicodeEncodeError, UnicodeError):
            enc = getattr(sys.stdout, "encoding", None) or "ascii"
            safe_line = line.encode(enc, errors="replace").decode(enc, errors="replace")
            print(safe_line, flush=True)
        except Exception:
            pass

        if log_file:
            try:
                log_file.write(line + "\n")
            except Exception:
                pass
        if log_fn:
            try:
                log_fn(line)
            except Exception:
                pass

    def rec_measure(name: str) -> None:
        trace.append(("MEASURE", name))
        if visited_items is not None:
            visited_items.append(name)

    # Print S9 Operational Checklist (m3: operator declared)
    log("================================================================================")
    log("OPERATIONAL CHECKLIST (S9, IMPLEMENTATION_PLAN.md - declarado por el operador):")
    log("[checklist] [x] 1. Nucleo and AD2 connected via USB to same PC. WaveForms GUI closed.")
    log("[checklist] [x] 2. Wiring: GND first, then DIO per table. Nothing on W1, W2, V+, V-.")
    log("[checklist] [x] 3. Flash selftest-gpio with script stopped.")
    log("[checklist] [x] 4. Start gpio_hw_check.py. Script enforces S2, waits for banner (S8), runs loose check (T0a) before enabling outputs.")
    log("[checklist] [x] 5. On completion or abort, script leaves AD2 in S2. Only then reflash, reset, or disconnect.")
    log("[checklist] [x] 6. Disconnecting: stop script, disconnect DIO lines, GND last.")
    log("================================================================================")

    # Host verdict checklist (B2: every item initialized to False)
    checklist_results: dict[str, bool] = {
        "sequence_complete": False,
        "lckr_clean_at_boot": False,
        "t15_pass": False,
        "t1_pass": False,
        "t2_pass": False,
        "b1_complete": False,
        "cm4_rev_match": False,
        "cm4_pass": False,
        "fw_all_passed": False,
        "t0b_a_oe_guards": False,
        "t0b_b_claim_guards": False,
        "t0b_c_contention": False,
        "t0b_d_timeout": False,
        "t0a_loose": False,
        "t0a_handshake": False,
        "t0a_mcu_to_ad2": False,
        "t0a_mcu_to_ad2_ctrl": False,
        "t0a_ad2_to_mcu": False,
        "t0a_scope": False,
        "t3_sequence": False,
        "t3_control": False,
        "t4_crc": False,
        "t4_control": False,
        "t7_analog": False,
        "t7_control": False,
        "t8_driver": False,
        "t8_control": False,
        "t5_pulls": False,
        "t5_controls": False,
        "t6_open_drain": False,
        "t6_control": False,
        "t9_gate": False,
        "t9_control": False,
        "t10_freq": False,
        "t10_control": False,
        "t16_pass": False,
    }

    step_tracker = StepTracker(EXPECTED_STEP_SEQUENCE)

    def on_abort():
        step_tracker.abort()
        ad2.set_safe_state()

    def on_m4_line(text: str) -> None:
        m_m4_rev = re.match(r"^\[m4_gpio_test\] Rev ([0-9a-f]{7})$", text)
        if m_m4_rev:
            m4_rev = m_m4_rev.group(1)
            if m4_rev != expected_rev:
                on_abort()
                raise BenchAbort(f"ABORT: CM4 banner revision mismatch (got {m4_rev}, expected {expected_rev})")
            checklist_results["cm4_rev_match"] = True
            log(f"[host] CM4 banner revision verified: {m4_rev}")
        if "CM4 VERDICT=PASS" in text:
            checklist_results["cm4_pass"] = True

    line_reader = LineReader(serial_port, clock=clock, log_fn=log, on_abort_fn=on_abort, on_m4_line_fn=on_m4_line, trace=trace)

    try:
        log(f"[host] Expected firmware revision from git: {expected_rev}")
        if "-dirty" in expected_rev:
            log("[host] ERROR: Working tree is dirty. Clean before building and capturing.")
            return 1

        # S8: Confirm OE is 0x0000 before doing anything else
        initial_oe = ad2.get_oe()
        if initial_oe != 0:
            raise AD2SafetyViolation(f"S8 violation: AD2 OE is 0x{initial_oe:04X} on open, expected 0x0000")

        # R2-m1: Drain serial port for at least 100 ms before reset and log discarded bytes
        drain_start = clock.monotonic()
        discarded_bytes = 0
        while clock.monotonic() - drain_start < 0.100:
            chunk = serial_port.read()
            if chunk:
                discarded_bytes += len(chunk)
            clock.sleep(0.005)
        chunk = serial_port.read()
        if chunk:
            discarded_bytes += len(chunk)
        log(f"[host] Serial port drained before reset: {discarded_bytes} bytes discarded.")

        # S10: Reset MCU with AD2 OE = 0x0000
        if reset_fn:
            reset_fn(ad2, log)
        else:
            reset_mcu_safe(ad2, log_fn=log)

        # S8 Banner Validation: strictly match Rev and idle before enabling outputs
        m_rev = line_reader.expect(r"^\[m7_gpio_test\] Rev ([0-9a-f]{7})$", timeout_s=15.0, record_trace=False)
        fw_rev = m_rev.group(1)
        if fw_rev != expected_rev:
            on_abort()
            raise BenchAbort(f"ABORT: Banner revision mismatch (got {fw_rev}, expected {expected_rev})")
        log(f"[host] CM7 banner revision verified: {fw_rev}")

        # Step 1: T15 announcement & verdict
        m_step1 = line_reader.expect(table_pattern("step1_announcement"), timeout_s=10.0)
        step_tracker.validate_announcement(m_step1.group(0), ad2, log_fn=log)

        line_reader.expect(table_pattern("step1_lckr_boot"), timeout_s=10.0)
        checklist_results["lckr_clean_at_boot"] = True
        log("[host] GPIOF LCKR clean at boot (T16 of the previous run was reset away).")

        m_t15 = line_reader.expect(table_pattern("step1_verdict"), timeout_s=30.0)
        if "PASS" in m_t15.group(0):
            checklist_results["t15_pass"] = True
            rec_measure("HOST_CHECK t15_pass")
            log("[host] T15 dual-core race PASSED.")
        else:
            log("[host] T15 dual-core race FAILED!")

        # Steps 11-13: T1, T2 and B1 run without the AD2; the host only reads.
        for step_name, verdict_name, item, key, label in (
            ("step11_announcement", "step11_verdict", "HOST_CHECK t1_pass", "t1_pass", "T1 refusals"),
            ("step12_announcement", "step12_verdict", "HOST_CHECK t2_pass", "t2_pass", "T2 parity"),
            ("step13_announcement", "step13_done", "HOST_CHECK b1_complete", "b1_complete", "B1 measurements"),
        ):
            m_step = line_reader.expect(table_pattern(step_name), timeout_s=10.0)
            step_tracker.validate_announcement(m_step.group(0), ad2, log_fn=log)
            line_reader.expect(table_pattern(verdict_name), timeout_s=30.0)
            checklist_results[key] = True
            rec_measure(item)
            log(f"[host] {label} reported by the firmware.")

        # Step 2: IDLE announcement
        m_step2 = line_reader.expect(table_pattern("step2_announcement"), timeout_s=10.0)
        step_tracker.validate_announcement(m_step2.group(0), ad2, log_fn=log)

        m_idle = line_reader.expect(table_pattern("step2_idle"), timeout_s=10.0)
        m_pv = re.search(r"v(\d+)", m_idle.group(0))
        proto_v = int(m_pv.group(1)) if m_pv else GPIO_BENCH_PROTOCOL_VERSION
        if proto_v != GPIO_BENCH_PROTOCOL_VERSION:
            on_abort()
            raise BenchAbort(f"ABORT: Protocol version mismatch in banner (got v={proto_v}, expected v={GPIO_BENCH_PROTOCOL_VERSION})")

        # Confirm OE remained 0 throughout banner and T15
        if ad2.get_oe() != 0:
            on_abort()
            raise AD2SafetyViolation(f"S8 violation: AD2 OE enabled before session start (0x{ad2.get_oe():04X})")

        # Now enable static output for HOST_REQ (DIO12)
        ad2.set_oe(AD2_DIO_HOST_REQ, GPIO_BENCH_BUS_NONE)
        ad2.digital_io_write(0, AD2_DIO_HOST_REQ)

        # 3. T0b(a) Host OE Guards Verification
        log("[ad2] Running T0b(a) host OE guards verification...")
        t0b_a_passed = True
        try:
            ad2.set_oe(AD2_DIO_CM7_ACK, GPIO_BENCH_BUS_NONE)
            t0b_a_passed = False
        except AD2SafetyViolation:
            pass
        try:
            ad2.set_oe(AD2_DIO_BUS_MASK, GPIO_BENCH_BUS_NONE)
            t0b_a_passed = False
        except AD2SafetyViolation:
            pass
        try:
            ad2.set_oe(AD2_DIO_BUS_MASK, GPIO_BENCH_BUS_MCU)
            t0b_a_passed = False
        except AD2SafetyViolation:
            pass

        ad2.set_safe_state()
        ad2.set_oe(AD2_DIO_HOST_REQ, GPIO_BENCH_BUS_NONE)
        ad2.digital_io_write(0, AD2_DIO_HOST_REQ)

        if t0b_a_passed:
            checklist_results["t0b_a_oe_guards"] = True
            rec_measure("HOST_CHECK t0b_a_oe_guards")
            log("[ad2] T0b(a) host OE guards PASSED.")
        else:
            log("[ad2] T0b(a) host OE guards FAILED!")
            return 1

        # 4. Synchronize session start with MCU: HS_HOST
        log("[host] Handshaking session start with MCU via HS_HOST...")
        hs_host(ad2, clock=clock, log_fn=log, timeout_s=10.0, line_reader=line_reader)
        log("[host] Session start synchronized.")

        # 5. Step 3: T0b(b) Firmware Guards Verification
        m_step3 = line_reader.expect(table_pattern("step3_announcement"), timeout_s=10.0)
        step_tracker.validate_announcement(m_step3.group(0), ad2, log_fn=log)

        m_claim = line_reader.expect(table_pattern("step3_guards_pass"), timeout_s=10.0)
        if "PASS" in m_claim.group(0):
            checklist_results["t0b_b_claim_guards"] = True
            rec_measure("HOST_CHECK t0b_b_claim_guards")
            log("[ad2] T0b(b) firmware claim guards PASSED.")
        else:
            log("[ad2] T0b(b) firmware claim guards FAILED!")

        # 6. Step 4: T0b(c) S5 Negative Control (B4)
        m_step4 = line_reader.expect(table_pattern("step4_announcement"), timeout_s=10.0)
        step_tracker.validate_announcement(m_step4.group(0), ad2, log_fn=log)

        log("[ad2] Step T0b(c): Calling s5_verify_released to verify S5 guard stops contention...")
        t0b_c_contention_caught = False
        try:
            s5_verify_released(ad2, step_tracker, STEP_T0B_S5_NEG, line_reader, clock=clock, log_fn=log)
            log("[ad2] T0b(c) S5 negative control FAILED: s5_verify_released did not detect contention!")
        except BusContention as bc:
            oe_p1, oe_p2, oe_p3 = bc.oe_checks if bc.oe_checks else (1, 1, 1)
            log(f"[ad2] MCU announced drive pattern: 0x{bc.announced:02X}" if bc.announced is not None else "[ad2] MCU announced drive pattern: None")
            log(f"[ad2] Contention verified on BUSCHK up: read 0x{bc.value:02X} (announced=0x{bc.announced:02X})")
            log(f"[ad2] T0b(c) OE checks: Point 1 (start)=0x{oe_p1:02X}, Point 2 (pre-read)=0x{oe_p2:02X}, Point 3 (post-check)=0x{oe_p3:02X}")
            if bc.announced is not None and bc.value == bc.announced and oe_p1 == 0 and oe_p2 == 0 and oe_p3 == 0:
                t0b_c_contention_caught = True
                checklist_results["t0b_c_contention"] = True
                rec_measure("HOST_CHECK t0b_c_contention")
                log("[ad2] T0b(c) S5 negative control PASSED.")
            else:
                log(f"[ad2] T0b(c) S5 negative control FAILED (read=0x{bc.value:02X}, exp=0x{bc.announced:02X}, OE=({oe_p1},{oe_p2},{oe_p3}))")
        except Exception as e:
            log(f"[ad2] T0b(c) S5 negative control FAILED with unexpected exception: {e}")
            step_tracker.abort()
            raise

        if not t0b_c_contention_caught:
            step_tracker.abort()
            raise BenchAbort("T0b(c) S5 negative control failed")

        # Handshake for contention check (single handshake in T0b(c))
        # Restore HOST_REQ as output static (since S2 set it to input)
        ad2.set_oe(AD2_DIO_HOST_REQ, GPIO_BENCH_BUS_NONE)
        hs_mcu(ad2, clock=clock, log_fn=log, timeout_s=10.0, line_reader=line_reader)

        # MCU releases bus and announces safe state
        line_reader.expect(table_pattern("step4_safe_verified"), timeout_s=10.0)

        # 7. Step 5: T0b(d) 10-second Timeout Test
        m_step5 = line_reader.expect(table_pattern("step5_announcement"), timeout_s=10.0)
        step_tracker.validate_announcement(m_step5.group(0), ad2, log_fn=log)

        log("[ad2] Step T0b(d): Deliberately silent for 10.5 seconds without asserting HOST_REQ...")
        rec_measure("HOST_WAIT t0b_d_timeout")
        clock.sleep(10.5)

        m_tout = line_reader.expect(table_pattern("step5_timeout_pass"), timeout_s=5.0)
        if "PASS" in m_tout.group(0):
            checklist_results["t0b_d_timeout"] = True
            rec_measure("HOST_CHECK t0b_d_timeout")
            log("[ad2] T0b(d) timeout test PASSED.")
        else:
            log("[ad2] T0b(d) timeout test FAILED!")

        # 8. Step 6: T0a Loose Lines Check (13 lines)
        m_step6 = line_reader.expect(table_pattern("step6_announcement"), timeout_s=10.0)
        step_tracker.validate_announcement(m_step6.group(0), ad2, log_fn=log)

        log("[ad2] Step T0a: Testing loose lines (13 lines)...")
        line_reader.expect(table_pattern("step6_buschk_up"), timeout_s=10.0)
        dio_up = ad2.digital_io_read() & AD2_DIO_LOOSE_13_MASK
        rec_measure("HOST_READ loose_up")
        log(f"[ad2] Loose check pull-up read: 0x{dio_up:04X} (expected 0x{AD2_DIO_LOOSE_13_MASK:04X})")
        loose_up_ok = (dio_up == AD2_DIO_LOOSE_13_MASK)
        if not loose_up_ok:
            diff = dio_up ^ AD2_DIO_LOOSE_13_MASK
            for b in range(16):
                if diff & (1 << b):
                    log(f"[ad2] Loose check pull-up FAILED on DIO{b} (observed=0, expected=1)")
        hs_mcu(ad2, clock=clock, log_fn=log, timeout_s=10.0, line_reader=line_reader)

        line_reader.expect(table_pattern("step6_buschk_down"), timeout_s=10.0)
        dio_down = ad2.digital_io_read() & AD2_DIO_LOOSE_13_MASK
        rec_measure("HOST_READ loose_down")
        log(f"[ad2] Loose check pull-down read: 0x{dio_down:04X} (expected 0x0000)")
        loose_down_ok = (dio_down == 0x0000)
        if not loose_down_ok:
            for b in range(16):
                if dio_down & (1 << b):
                    log(f"[ad2] Loose check pull-down FAILED on DIO{b} (observed=1, expected=0)")
        hs_mcu(ad2, clock=clock, log_fn=log, timeout_s=10.0, line_reader=line_reader)

        line_reader.expect(table_pattern("step6_loose_done"), timeout_s=10.0)
        if loose_up_ok and loose_down_ok:
            checklist_results["t0a_loose"] = True
            log("[ad2] T0a loose check PASSED (all 13 lines).")
        else:
            log("[ad2] T0a loose check FAILED!")

        # 9. Step 7: T0a Handshake Lines (PE2 / PE7)
        m_step7 = line_reader.expect(table_pattern("step7_announcement"), timeout_s=10.0)
        step_tracker.validate_announcement(m_step7.group(0), ad2, log_fn=log)

        log("[ad2] Step T0a: Testing handshake lines (PE2 HOST_REQ, PE7 MCU_ACK)...")
        # 1. HS_HOST: MCU validates PE2 HIGH then LOW
        hs_host(ad2, clock=clock, log_fn=log, timeout_s=10.0, line_reader=line_reader)
        line_reader.expect(table_pattern("step7_handshake_pe2"), timeout_s=10.0)

        # 2. HS_MCU: Host validates PE7 HIGH then LOW
        hs_mcu(ad2, clock=clock, log_fn=log, timeout_s=10.0, line_reader=line_reader)
        line_reader.expect(table_pattern("step7_handshake_done"), timeout_s=10.0)
        checklist_results["t0a_handshake"] = True
        log("[ad2] T0a handshake lines PASSED.")

        # 10. Step 8: T0a Walking-1 MCU -> AD2 (11 lines)
        m_step8 = line_reader.expect(table_pattern("step8_announcement"), timeout_s=10.0)
        step_tracker.validate_announcement(m_step8.group(0), ad2, log_fn=log)

        log("[ad2] Step T0a: Walking-1 MCU -> AD2 (11 lines)...")
        # S5 break-before-make verification
        s5_verify_released(ad2, step_tracker, STEP_T0A_MCU_TO_AD2, line_reader, clock=clock, log_fn=log)

        mcu_walk_bits = [9, 11, 0, 1, 2, 3, 4, 5, 6, 7, 14]
        mcu_walk_mask = (1 << 9) | (1 << 11) | (1 << 14) | AD2_DIO_BUS_MASK
        mcu_walk_passed = True

        for exp_bit in mcu_walk_bits:
            rec_measure("HOST_READ mcu_bit")

            def check_bit(b=exp_bit):
                nonlocal mcu_walk_passed
                dio_val = ad2.digital_io_read() & mcu_walk_mask
                exp_val = (1 << b)
                bit_ok = (dio_val == exp_val)
                if not bit_ok:
                    mcu_walk_passed = False
                    log(f"[ad2] Walking-1 MCU->AD2 DIO{b} FAILED! Observed=0x{dio_val:04X}, Expected=0x{exp_val:04X}")
                else:
                    log(f"[ad2] Walking-1 MCU->AD2 DIO{b}: OK (0x{dio_val:04X})")

            hs_mcu(ad2, clock=clock, log_fn=log, timeout_s=10.0, line_reader=line_reader, on_ack_fn=check_bit)

        # MCU -> AD2 Negative Control
        line_reader.expect(table_pattern("step8_control_ready"), timeout_s=10.0)
        rec_measure("HOST_READ mcu_ctrl")

        def check_ctrl():
            dio_ctrl = ad2.digital_io_read() & mcu_walk_mask
            ctrl_ok = (dio_ctrl == 0x0000)
            if ctrl_ok:
                checklist_results["t0a_mcu_to_ad2_ctrl"] = True
                log("[ad2] Walking-1 MCU->AD2 negative control: OK (0x0000)")
            else:
                log(f"[ad2] Walking-1 MCU->AD2 negative control FAILED! Observed 0x{dio_ctrl:04X}, expected 0x0000")
                for b in range(16):
                    if dio_ctrl & (1 << b):
                        log(f"[ad2]   Leaked high output on DIO{b}!")

        hs_mcu(ad2, clock=clock, log_fn=log, timeout_s=10.0, line_reader=line_reader, on_ack_fn=check_ctrl)

        line_reader.expect(table_pattern("step8_mcu_done"), timeout_s=10.0)
        if mcu_walk_passed:
            checklist_results["t0a_mcu_to_ad2"] = True
            log("[ad2] Walking-1 MCU->AD2 PASSED.")
        else:
            log("[ad2] Walking-1 MCU->AD2 FAILED!")

        # 11. Step 9: T0a Walking-1 AD2 -> MCU (10 lines)
        m_step9 = line_reader.expect(table_pattern("step9_announcement"), timeout_s=10.0)
        step_tracker.validate_announcement(m_step9.group(0), ad2, log_fn=log)

        log("[ad2] Step T0a: Walking-1 AD2 -> MCU (10 lines)...")
        token = s5_verify_released(ad2, step_tracker, STEP_T0A_AD2_TO_MCU, line_reader, clock=clock, log_fn=log)

        line_reader.expect(table_pattern("step9_ad2_ready"), timeout_s=10.0)
        hs_mcu(ad2, clock=clock, log_fn=log, timeout_s=10.0, line_reader=line_reader)

        ad2_walk_bits = [8, 10, 0, 1, 2, 3, 4, 5, 6, 7]
        ad2_walk_passed = True

        for b in ad2_walk_bits:
            rec_measure("HOST_DRIVE ad2_bit")
            if b < 8:
                drive_bus(ad2, step_tracker, 1 << b)
            else:
                ad2.set_oe(AD2_DIO_HOST_REQ | (1 << b), GPIO_BENCH_BUS_NONE)
                ad2.digital_io_write(1 << b, 1 << b)

            # Signal data ready via HS_HOST
            hs_host(ad2, clock=clock, log_fn=log, timeout_s=10.0, line_reader=line_reader)

            # Wait for MCU announcement
            m_bit = line_reader.expect(table_pattern(f"step9_ad2_{b}_res"), timeout_s=10.0)
            if "OK" not in m_bit.group(0):
                ad2_walk_passed = False
                log(f"[ad2] Walking-1 AD2->MCU DIO{b} reported FAIL by MCU!")

            # Release bit
            if b < 8:
                drive_bus(ad2, step_tracker, 0)
            else:
                ad2.digital_io_write(0, 1 << b)
            ad2.set_safe_state()
            ad2.set_oe(AD2_DIO_HOST_REQ, GPIO_BENCH_BUS_NONE)

        # AD2 -> MCU Negative Control
        line_reader.expect(table_pattern("step9_analog_ready"), timeout_s=10.0)
        rec_measure("HOST_DRIVE ad2_analog")
        drive_bus(ad2, step_tracker, 0xFF)
        hs_host(ad2, clock=clock, log_fn=log, timeout_s=10.0, line_reader=line_reader)

        m_analog = line_reader.expect(table_pattern("step9_analog_res"), timeout_s=10.0)
        if "OK" not in m_analog.group(0):
            ad2_walk_passed = False
            log("[ad2] AD2->MCU analog control reported FAIL by MCU!")

        ad2.set_safe_state()
        ad2.set_oe(AD2_DIO_HOST_REQ, GPIO_BENCH_BUS_NONE)

        m_ad2_verdict = line_reader.expect(table_pattern("step9_verdict"), timeout_s=10.0)
        if "PASS" in m_ad2_verdict.group(0) and ad2_walk_passed:
            checklist_results["t0a_ad2_to_mcu"] = True
            log("[ad2] Walking-1 AD2->MCU PASSED.")
        else:
            log("[ad2] Walking-1 AD2->MCU FAILED!")

        # 12. Step 10: T0a Scope 1 & Scope 2 DC Levels
        m_step10 = line_reader.expect(table_pattern("step10_announcement"), timeout_s=10.0)
        step_tracker.validate_announcement(m_step10.group(0), ad2, log_fn=log)

        log("[ad2] Step T0a: Testing Scope 1 (PF14) and Scope 2 (PF15) DC levels...")
        ad2.analog_in_setup_dc(channel=0, voltage_range=10.0)
        ad2.analog_in_setup_dc(channel=1, voltage_range=10.0)
        clock.sleep(0.05)

        line_reader.expect(table_pattern("step10_low_ready"), timeout_s=10.0)
        clock.sleep(0.01)
        rec_measure("HOST_MEASURE scope_low")
        v1_low = ad2.analog_in_read_dc(0)
        v2_low = ad2.analog_in_read_dc(1)
        log(f"[ad2] Scope LOW: PF14={v1_low:.3f}V, PF15={v2_low:.3f}V (expected <= 0.4V)")
        scope_low_ok = (v1_low <= 0.4 and v2_low <= 0.4)
        if not scope_low_ok:
            if v1_low > 0.4:
                log(f"[ad2]   Scope 1 (PF14) VOL out of range: {v1_low:.3f}V > 0.4V")
            if v2_low > 0.4:
                log(f"[ad2]   Scope 2 (PF15) VOL out of range: {v2_low:.3f}V > 0.4V")
        hs_mcu(ad2, clock=clock, log_fn=log, timeout_s=10.0, line_reader=line_reader)

        line_reader.expect(table_pattern("step10_high_ready"), timeout_s=10.0)
        clock.sleep(0.01)
        rec_measure("HOST_MEASURE scope_high")
        v1_high = ad2.analog_in_read_dc(0)
        v2_high = ad2.analog_in_read_dc(1)
        log(f"[ad2] Scope HIGH: PF14={v1_high:.3f}V, PF15={v2_high:.3f}V (expected >= 2.9V)")
        scope_high_ok = (v1_high >= 2.9 and v2_high >= 2.9)
        if not scope_high_ok:
            if v1_high < 2.9:
                log(f"[ad2]   Scope 1 (PF14) VOH out of range: {v1_high:.3f}V < 2.9V")
            if v2_high < 2.9:
                log(f"[ad2]   Scope 2 (PF15) VOH out of range: {v2_high:.3f}V < 2.9V")
        hs_mcu(ad2, clock=clock, log_fn=log, timeout_s=10.0, line_reader=line_reader)

        line_reader.expect(table_pattern("step10_scope_done"), timeout_s=10.0)
        if scope_low_ok and scope_high_ok:
            checklist_results["t0a_scope"] = True
            log("[ad2] Scope check PASSED.")
        else:
            log("[ad2] Scope check FAILED!")

        # ---- F1a steps with the AD2 ------------------------------------------

        def enter(step_id: int) -> None:
            m = line_reader.expect(table_pattern(f"step{step_id}_announcement"), timeout_s=10.0)
            step_tracker.validate_announcement(m.group(0), ad2, log_fn=log)

        def host_req_output() -> None:
            ad2.set_oe(AD2_DIO_HOST_REQ, GPIO_BENCH_BUS_NONE)
            ad2.digital_io_write(0, AD2_DIO_HOST_REQ)

        def release_bus_then_strobe(step_name: str) -> None:
            """S5 point 2, end: OE = 0 with read-back, then a HOST_REQ pulse."""
            ad2.set_safe_state()
            if ad2.get_oe() != 0:
                raise AD2SafetyViolation(f"S5: OE not 0 after releasing the bus in {step_name}")
            host_req_output()
            hs_host(ad2, clock=clock, log_fn=log, timeout_s=10.0, line_reader=line_reader)

        # Step 30: T3 (bus = MCU)
        enter(STEP_T3_MCU_OPS)
        log("[ad2] Step T3: Set/Reset/Write/WritePort/Toggle on the bus, LA capture...")
        s5_verify_released(ad2, step_tracker, STEP_T3_MCU_OPS, line_reader, clock=clock, log_fn=log)
        t3_results = []
        for ready_name, arm_name, check_name, is_control in (
            ("step30_ready", "HOST_ARM t3_la", "HOST_CHECK t3_sequence", False),
            ("step30_ctrl_ready", "HOST_ARM t3_ctrl_la", "HOST_CHECK t3_control", True),
        ):
            line_reader.expect(table_pattern(ready_name), timeout_s=10.0)
            rec_measure(arm_name)
            rate, n = ad2.la_arm(T3_LA_RATE_HZ, T3_LA_SAMPLES, trigger_rise_mask=AD2_DIO_HOST_REQ, prefill=50)
            hs_host(ad2, clock=clock, log_fn=log, timeout_s=10.0, line_reader=line_reader)
            samples = ad2.la_read(n, timeout_s=2.0)
            rec_measure(check_name)
            seq = la_stable_sequence(samples or [], AD2_DIO_BUS_MASK, 0, T3_MIN_RUN)
            if seq and seq[0] == 0x00:
                seq = seq[1:]
            diffs = [i for i in range(min(len(seq), len(T3_EXPECTED))) if seq[i] != T3_EXPECTED[i]]
            if len(seq) != len(T3_EXPECTED):
                diffs.append(-1)
            log(f"[ad2] T3 {'control' if is_control else 'sequence'}: {len(seq)} values captured at "
                f"{rate / 1e6:.0f} MHz, expected {len(T3_EXPECTED)}; mismatches at {diffs if diffs else 'none'}")
            if diffs:
                log("[ad2] T3 captured: " + " ".join(f"{v:02X}" for v in seq))
            t3_results.append(diffs)
        checklist_results["t3_sequence"] = (t3_results[0] == [])
        checklist_results["t3_control"] = (t3_results[1] == [T3_CONTROL_INDEX])
        log(f"[ad2] T3 sequence {'PASSED' if checklist_results['t3_sequence'] else 'FAILED'}; "
            f"control (one wrong value at index {T3_CONTROL_INDEX}) {'detected' if checklist_results['t3_control'] else 'NOT detected'}.")
        line_reader.expect(table_pattern("step30_done"), timeout_s=10.0)

        # Step 31: T4 (bus = AD2)
        enter(STEP_T4_AD2_PATTERNS)
        log("[ad2] Step T4: 256 patterns driven by the AD2, read with ReadPort/Read...")
        s5_verify_released(ad2, step_tracker, STEP_T4_AD2_PATTERNS, line_reader, clock=clock, log_fn=log)
        line_reader.expect(table_pattern("step31_ready"), timeout_s=10.0)
        hs_mcu(ad2, clock=clock, log_fn=log, timeout_s=10.0, line_reader=line_reader)
        clock.sleep(0.001)                                  # S6: 1 ms after the MCU let go
        t4_lines = {}
        for pass_no, patterns in ((1, list(range(256))), (2, [(~p) & 0xFF for p in range(256)])):
            for p in patterns:
                drive_bus(ad2, step_tracker, p)
                hs_host(ad2, clock=clock, log_fn=log, timeout_s=10.0, line_reader=line_reader)
            m = line_reader.expect(table_pattern(f"step31_pass{pass_no}_res"), timeout_s=10.0)
            t4_lines[pass_no] = m.group(0)
            rec_measure("HOST_CHECK t4_crc" if pass_no == 1 else "HOST_CHECK t4_control")
        crc_ok = crc32_bytes(range(256))
        crc_inv = crc32_bytes((~p) & 0xFF for p in range(256))

        def parse_t4(text: str) -> tuple[int, int, int]:
            mm = re.search(r"count=(\d+) crc_port=0x([0-9A-F]{8}) crc_bits=0x([0-9A-F]{8})", text)
            return int(mm.group(1)), int(mm.group(2), 16), int(mm.group(3), 16)

        c1, p1, b1 = parse_t4(t4_lines[1])
        c2, p2, b2 = parse_t4(t4_lines[2])
        checklist_results["t4_crc"] = (c1 == 256 and p1 == crc_ok and b1 == crc_ok)
        checklist_results["t4_control"] = (c2 == 256 and p2 != crc_ok and p2 == crc_inv and b2 == crc_inv)
        log(f"[ad2] T4 pass 1: count={c1} crc_port=0x{p1:08X} crc_bits=0x{b1:08X}, host CRC32=0x{crc_ok:08X} -> "
            f"{'PASSED' if checklist_results['t4_crc'] else 'FAILED'}")
        log(f"[ad2] T4 control (inverted pass): crc_port=0x{p2:08X} differs from 0x{crc_ok:08X} and equals "
            f"inverted CRC 0x{crc_inv:08X} -> {'detected' if checklist_results['t4_control'] else 'NOT detected'}")
        release_bus_then_strobe("T4")
        line_reader.expect(table_pattern("step31_done"), timeout_s=10.0)

        # Step 32: T7 (bus = AD2)
        enter(STEP_T7_ANALOG_IDR)
        log("[ad2] Step T7: AD2 drives 0xFF; MCU reads IDR in analog, then in input...")
        s5_verify_released(ad2, step_tracker, STEP_T7_ANALOG_IDR, line_reader, clock=clock, log_fn=log)
        line_reader.expect(table_pattern("step32_analog_ready"), timeout_s=10.0)
        hs_mcu(ad2, clock=clock, log_fn=log, timeout_s=10.0, line_reader=line_reader)
        clock.sleep(0.001)
        rec_measure("HOST_DRIVE t7_ff")
        drive_bus(ad2, step_tracker, 0xFF)
        hs_host(ad2, clock=clock, log_fn=log, timeout_s=10.0, line_reader=line_reader)
        m_an = line_reader.expect(table_pattern("step32_analog_res"), timeout_s=10.0)
        line_reader.expect(table_pattern("step32_input_ready"), timeout_s=10.0)
        hs_host(ad2, clock=clock, log_fn=log, timeout_s=10.0, line_reader=line_reader)
        m_in = line_reader.expect(table_pattern("step32_input_res"), timeout_s=10.0)
        rec_measure("HOST_CHECK t7")
        idr_an = int(re.search(r"idr=0x([0-9A-F]{2})", m_an.group(0)).group(1), 16)
        idr_in = int(re.search(r"idr=0x([0-9A-F]{2})", m_in.group(0)).group(1), 16)
        checklist_results["t7_analog"] = (idr_an == 0x00)
        checklist_results["t7_control"] = (idr_in == 0xFF)
        log(f"[ad2] T7 analog IDR=0x{idr_an:02X} (expected 0x00); control, input IDR=0x{idr_in:02X} (expected 0xFF)")
        release_bus_then_strobe("T7")
        line_reader.expect(table_pattern("step32_done"), timeout_s=10.0)

        # Step 33: T8 (bus = NONE, PE5 = DIO9)
        enter(STEP_T8_NO_GLITCH)
        log("[ad2] Step T8: input -> output on PE5 (DIO9), LA at 100 MHz...")
        t8_ok = True
        t8_ctrl = False
        for case, (expected, desc) in T8_CASES.items():
            line_reader.expect(table_pattern(f"step33_case{case}_ready"), timeout_s=10.0)
            rec_measure("HOST_ARM t8_la")
            rate, n = ad2.la_arm(T8_LA_RATE_HZ, T8_LA_SAMPLES, trigger_rise_mask=AD2_DIO_HOST_REQ, prefill=100)
            hs_host(ad2, clock=clock, log_fn=log, timeout_s=10.0, line_reader=line_reader)
            samples = ad2.la_read(n, timeout_s=2.0)
            rec_measure("HOST_CHECK t8_case")
            edges = la_transitions(samples or [], 9) if samples else -1
            if case == "E":
                t8_ctrl = edges >= 2
                verdict = "pulse seen" if t8_ctrl else "NO PULSE"
            else:
                ok = (edges == expected)
                t8_ok = t8_ok and ok
                verdict = "OK" if ok else "FAIL"
            log(f"[ad2] T8 case {case} ({desc}): DIO9 transitions={edges} (expected "
                f"{'>=2' if case == 'E' else expected}) {verdict}")
        checklist_results["t8_driver"] = t8_ok
        checklist_results["t8_control"] = t8_ctrl
        line_reader.expect(table_pattern("step33_done"), timeout_s=10.0)

        # Step 34: T5 (bus = NONE)
        enter(STEP_T5_PULLS)
        log("[ad2] Step T5: pull resistances on PF14 / PF15...")
        v = {}
        for key in ("vdd", "nopull", "pulls"):
            line_reader.expect(table_pattern(f"step34_{key}_ready"), timeout_s=10.0)
            rec_measure(f"HOST_MEASURE t5_{key}")
            v[key] = (ad2.analog_in_average(0), ad2.analog_in_average(1))
            hs_mcu(ad2, clock=clock, log_fn=log, timeout_s=10.0, line_reader=line_reader)
        v_dd = v["vdd"][1]
        v_np14, v_np15 = v["nopull"]
        v_pu14, v_pd15 = v["pulls"]
        t5_ctrl = (v_np14 < 0.1) and (v_np15 > 2.9) and (v_pu14 <= 2.0)
        r_pu = r_pd = r_in = float("nan")
        try:
            r_in = T5_RX_OHMS * v_np15 / (v_dd - v_np15)
            r_par = 1.0 / (1.0 / T5_RX_OHMS + 1.0 / r_in)
            r_pu = r_par * (v_dd / v_pu14 - 1.0)
            x = T5_RX_OHMS * v_pd15 / (v_dd - v_pd15)
            r_pd = 1.0 / (1.0 / x - 1.0 / r_in)
        except ZeroDivisionError:
            pass
        t5_ok = t5_ctrl and (T5_R_MIN <= r_pu <= T5_R_MAX) and (T5_R_MIN <= r_pd <= T5_R_MAX)
        checklist_results["t5_controls"] = t5_ctrl
        checklist_results["t5_pulls"] = t5_ok
        log(f"[ad2] T5 V_DD={v_dd:.3f} V, V_NP14={v_np14:.3f} V, V_NP15={v_np15:.3f} V, "
            f"V_PU14={v_pu14:.3f} V, V_PD15={v_pd15:.3f} V (R_X={T5_RX_OHMS / 1e3:.0f}k nominal)")
        log(f"[ad2] T5 R_IN={r_in / 1e3:.1f}k R_PU={r_pu / 1e3:.1f}k R_PD={r_pd / 1e3:.1f}k "
            f"(range {T5_R_MIN / 1e3:.0f}-{T5_R_MAX / 1e3:.0f}k) -> {'PASSED' if t5_ok else 'FAILED'}")
        log(f"[ad2] T5 controls: V_NP14<0.1 {v_np14 < 0.1}, V_NP15>2.9 {v_np15 > 2.9}, V_PU14<=2.0 {v_pu14 <= 2.0} "
            f"(scope offset read on PF14 with no pull: {v_np14 * 1e3:.0f} mV)")
        line_reader.expect(table_pattern("step34_done"), timeout_s=10.0)

        # Step 35: T6 (bus = NONE)
        enter(STEP_T6_OPEN_DRAIN)
        log("[ad2] Step T6: open-drain on PF15 (22k to 3V3) and PF14 (22k to GND)...")
        w = {}
        for key in ("odlow", "odrel", "ctrl"):
            line_reader.expect(table_pattern(f"step35_{key}_ready"), timeout_s=10.0)
            rec_measure(f"HOST_MEASURE t6_{key}")
            w[key] = (ad2.analog_in_average(0), ad2.analog_in_average(1))
            hs_mcu(ad2, clock=clock, log_fn=log, timeout_s=10.0, line_reader=line_reader)
        v15_low = w["odlow"][1]
        v14_rel = w["odlow"][0]
        v15_rel = w["odrel"][1]
        v14_pp = w["ctrl"][0]
        t6_ok = (v15_low <= 0.4) and (abs(v15_rel - v_np15) <= 0.1) and (v14_rel < 0.1)
        checklist_results["t6_open_drain"] = t6_ok
        checklist_results["t6_control"] = (v14_pp > 2.9)
        log(f"[ad2] T6 (a) PF15 OD low={v15_low:.3f} V (<=0.4), OD released={v15_rel:.3f} V (V_NP15={v_np15:.3f} +-0.1); "
            f"(b) PF14 OD released={v14_rel:.3f} V (<0.1) -> {'PASSED' if t6_ok else 'FAILED'}")
        log(f"[ad2] T6 control: PF14 push-pull high={v14_pp:.3f} V (>2.9)")
        line_reader.expect(table_pattern("step35_done"), timeout_s=10.0)

        # Step 36: T9 (bus = NONE)
        enter(STEP_T9_RISE_TIME)
        log("[ad2] Step T9: 10-90 % rise time per OSPEEDR on PF14 / PF15, 100 MS/s...")
        tr: dict[int, tuple[list[float], list[float]]] = {}
        for sp in range(4):
            line_reader.expect(table_pattern(f"step36_speed{sp}_ready"), timeout_s=10.0)
            r14: list[float] = []
            r15: list[float] = []
            for _ in range(T9_EDGES):
                rate, n = ad2.scope_arm_rise(T9_RATE_HZ, T9_SAMPLES, trigger_channel=0, level_v=1.65)
                hs_host(ad2, clock=clock, log_fn=log, timeout_s=10.0, line_reader=line_reader)
                data = ad2.scope_read(n, timeout_s=1.0)
                if data is None:
                    continue
                t14 = rise_time_10_90(data[0], rate, 0.1)
                t15 = rise_time_10_90(data[1], rate, 0.1)
                if t14 is not None:
                    r14.append(t14 * 1e9)
                if t15 is not None:
                    r15.append(t15 * 1e9)
            tr[sp] = (r14, r15)
            for name, rs in (("PF14", r14), ("PF15", r15)):
                if rs:
                    m_, s_ = mean_std(rs)
                    log(f"[ad2] T9 speed={sp} {name}: tr={m_:.2f} ns sd={s_:.2f} ns over {len(rs)} edges "
                        f"[{', '.join(f'{x:.1f}' for x in rs)}]")
                else:
                    log(f"[ad2] T9 speed={sp} {name}: no edge measured")
        line_reader.expect(table_pattern("step36_ctrl_ready"), timeout_s=10.0)
        rate, n = ad2.scope_arm_rise(T9_RATE_HZ, T9_SAMPLES, trigger_channel=0, level_v=1.65)
        hs_host(ad2, clock=clock, log_fn=log, timeout_s=10.0, line_reader=line_reader)
        ctrl_data = ad2.scope_read(n, timeout_s=0.5)
        rec_measure("HOST_CHECK t9")
        gate = True
        for idx, name in ((0, "PF14"), (1, "PF15")):
            low, vh = tr[0][idx], tr[3][idx]
            if len(low) < 2 or len(vh) < 2:
                gate = False
                log(f"[ad2] T9 gate {name}: not enough edges")
                continue
            (m_l, s_l), (m_v, s_v) = mean_std(low), mean_std(vh)
            sigma = ((s_l ** 2) / len(low) + (s_v ** 2) / len(vh)) ** 0.5
            ok = (m_l - m_v) > 5.0 * sigma
            gate = gate and ok
            log(f"[ad2] T9 gate {name}: tr(LOW)-tr(VERY_HIGH)={m_l - m_v:.2f} ns, 5 sigma={5 * sigma:.2f} ns "
                f"(sigma of the difference of means) -> {'OK' if ok else 'FAIL'}")
        checklist_results["t9_gate"] = gate
        checklist_results["t9_control"] = ctrl_data is None
        log(f"[ad2] T9 control (pins in input): {'no edge' if ctrl_data is None else 'EDGE SEEN'}")
        line_reader.expect(table_pattern("step36_done"), timeout_s=10.0)

        # Step 37: T10 (bus = NONE, PA8 = DIO14)
        enter(STEP_T10_MCO1)
        log("[ad2] Step T10: MCO1 = HSI/4 on PA8, frequency on DIO14...")
        m_mco = line_reader.expect(table_pattern("step37_mco_ready"), timeout_s=10.0)
        rec_measure("HOST_MEASURE t10_freq")
        rate, n = ad2.la_arm(100e6, 4096)
        samples = ad2.la_read(n, timeout_s=2.0) or []
        f_hz, rises = la_frequency(samples, 14, rate)
        hs_mcu(ad2, clock=clock, log_fn=log, timeout_s=10.0, line_reader=line_reader)
        hsi_on = m_mco.group(0).endswith("hsi=1")
        f_ok = hsi_on and abs(f_hz - T10_EXPECTED_HZ) <= T10_EXPECTED_HZ * T10_TOLERANCE
        checklist_results["t10_freq"] = f_ok
        log(f"[ad2] T10 DIO14: {f_hz / 1e6:.3f} MHz from {rises} rising edges (expected 16 MHz +-2 %, HSI ready={hsi_on}) "
            f"-> {'PASSED' if f_ok else 'FAILED'}")
        line_reader.expect(table_pattern("step37_ctrl_ready"), timeout_s=10.0)
        rec_measure("HOST_MEASURE t10_control")
        rate, n = ad2.la_arm(100e6, 4096)
        samples = ad2.la_read(n, timeout_s=2.0) or []
        f_ctrl, rises_ctrl = la_frequency(samples, 14, rate)
        hs_mcu(ad2, clock=clock, log_fn=log, timeout_s=10.0, line_reader=line_reader)
        checklist_results["t10_control"] = (rises_ctrl < 2)
        log(f"[ad2] T10 control (PA8 in AF1): {rises_ctrl} rising edges, {f_ctrl / 1e6:.3f} MHz (expected no signal)")
        line_reader.expect(table_pattern("step37_done"), timeout_s=10.0)

        # 13. Step 20: T16 LCKR, last test of the image
        m_step20 = line_reader.expect(table_pattern("step20_announcement"), timeout_s=10.0)
        step_tracker.validate_announcement(m_step20.group(0), ad2, log_fn=log)
        line_reader.expect(table_pattern("step20_verdict"), timeout_s=10.0)
        checklist_results["t16_pass"] = True
        rec_measure("HOST_CHECK t16_pass")
        log("[host] T16 LCKR reported by the firmware.")

        # 14. Step 99: Final Verdicts and CM4 Drain
        m_step99 = line_reader.expect(table_pattern("step99_announcement"), timeout_s=10.0)
        step_tracker.validate_announcement(m_step99.group(0), ad2, log_fn=log)
        checklist_results["sequence_complete"] = True

        m_fw_all = line_reader.expect(table_pattern("step99_verdict"), timeout_s=10.0)
        if "ALL GPIO SELF-TESTS PASSED" in m_fw_all.group(0):
            checklist_results["fw_all_passed"] = True

        # Check CM4 lines
        line_reader.poll()
        for l in line_reader.m4_lines:
            m_m4_rev = re.match(r"^\[m4_gpio_test\] Rev ([0-9a-f]{7})$", l)
            if m_m4_rev:
                if m_m4_rev.group(1) == fw_rev:
                    checklist_results["cm4_rev_match"] = True
                else:
                    log(f"[host] CM4 revision mismatch! Got {m_m4_rev.group(1)}, expected {fw_rev}")
            if "CM4 VERDICT=PASS" in l:
                checklist_results["cm4_pass"] = True

    except Exception as e:
        log(f"ERROR during selftest execution: {e}")
    finally:
        on_abort()
        try:
            line_reader.poll()
        except Exception:
            pass
        for l in line_reader.m4_lines:
            m_m4_rev = re.match(r"^\[m4_gpio_test\] Rev ([0-9a-f]{7})$", l)
            if m_m4_rev and m_m4_rev.group(1) == expected_rev:
                checklist_results["cm4_rev_match"] = True
            if "CM4 VERDICT=PASS" in l:
                checklist_results["cm4_pass"] = True

        # Explicitly print the checklist (B2)
        log("================================================================================")
        log("HOST VERDICT CHECKLIST (B2):")
        all_passed = True
        for key, res in checklist_results.items():
            status_str = "OK" if res else "FAIL"
            if not res:
                all_passed = False
            log(f"  [{status_str}] {key}")
        log("================================================================================")
        final_verdict = "PASS" if all_passed else "FAIL"
        log(f"HOST VERDICT={final_verdict}")
        log("================================================================================")
        if log_file:
            log_file.close()

    return 0 if all_passed else 1


def main() -> int:
    if hasattr(sys.stdout, "reconfigure"):
        try:
            sys.stdout.reconfigure(errors="replace")
        except Exception:
            pass
    if hasattr(sys.stderr, "reconfigure"):
        try:
            sys.stderr.reconfigure(errors="replace")
        except Exception:
            pass

    parser = argparse.ArgumentParser(description="GPIO Hardware Check for STM32H755 + AD2")
    parser.add_argument("--port", default="COM4", help="Serial port for ST-LINK VCP (default: COM4)")
    parser.add_argument("--baud", type=int, default=115200, help="Baud rate (default: 115200)")
    parser.add_argument("--sn", default="210321B5D38D", help="Analog Discovery 2 serial number")
    parser.add_argument("--log", type=pathlib.Path, default=None, help="Output file to save capture")
    parser.add_argument("--checklist-confirmed", action="store_true", help="Confirm S9 operational checklist (operator declared, m3)")
    args = parser.parse_args()

    if not args.checklist_confirmed:
        print("ERROR: S9 operational checklist not confirmed! (m3)", file=sys.stderr)
        print("Please review physical bench setup (S1-S10) and pass --checklist-confirmed.", file=sys.stderr)
        return 2

    rev = get_git_rev()
    if "-dirty" in rev:
        print(f"ERROR: Working tree is dirty ({rev}). Clean before building and capturing.", file=sys.stderr)
        return 1

    with AD2(sn=args.sn) as ad2:
        with SerialPort(args.port, args.baud) as ser:
            return run_selftest(
                serial_port=ser,
                ad2=ad2,
                clock=RealClock(),
                reset_fn=reset_mcu_safe,
                expected_rev=rev,
                log_path=args.log,
                checklist_confirmed=True,
            )


if __name__ == "__main__":
    sys.exit(main())
