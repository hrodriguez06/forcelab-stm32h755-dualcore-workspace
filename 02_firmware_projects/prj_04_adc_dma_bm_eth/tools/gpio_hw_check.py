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
  - Evaluates and prints explicit 15-item host checklist and final HOST VERDICT=.
"""

from __future__ import annotations

import argparse
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
GPIO_BENCH_PROTOCOL_VERSION = 1

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
        r'X\s*\(\s*(\d+)\s*,\s*BENCH_ITEM_(\w+)\s*,\s*GPIO_BENCH_PRIM_(\w+)\s*,\s*"([^"]*)"\s*,\s*"([^"]*)"\s*,\s*(0x[0-9a-fA-F]+|\d+)\s*\)'
    )
    for m in item_pattern.finditer(text):
        raw_bit = m.group(6)
        bit_val = int(raw_bit, 16) if raw_bit.startswith("0x") else int(raw_bit)
        items.append(BenchItem(
            step_id=int(m.group(1)),
            item_type=m.group(2),
            primitive=m.group(3),
            name=m.group(4),
            line_pattern=m.group(5),
            bit_arg=bit_val,
        ))

    return steps, items


PROTOCOL_STEPS, PROTOCOL_ITEMS = load_protocol_from_header()
EXPECTED_STEP_SEQUENCE = [(s.step_id, s.bus_owner) for s in PROTOCOL_STEPS]

STEP_RE = re.compile(r"^\[m7_gpio_test\] STEP id=(\d+) bus=(NONE|MCU|AD2) v=(\d+)")


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
    """

    def __init__(
        self,
        ser,
        clock=RealClock(),
        log_fn: Optional[Callable[[str], None]] = None,
        on_abort_fn: Optional[Callable[[], None]] = None,
        on_m4_line_fn: Optional[Callable[[str], None]] = None,
    ):
        self.ser = ser
        self.clock = clock
        self.log_fn = log_fn or (lambda msg: None)
        self.on_abort_fn = on_abort_fn
        self.on_m4_line_fn = on_m4_line_fn
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

    def expect(self, regex_pattern: str, timeout_s: float = 10.0) -> re.Match[str]:
        """Wait for and return the next protocol line matching regex_pattern.
        If an unexpected protocol line arrives, or timeout expires:
        immediately calls on_abort_fn and raises BenchAbort. Never returns None.
        """
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

def hs_mcu(ad2: AD2, clock=RealClock(), log_fn: Optional[Callable[[str], None]] = None, timeout_s: float = 10.0) -> None:
    """R-B1 HS_MCU (MCU signals):
    MCU asserts MCU_ACK=1 -> Host asserts HOST_REQ=1 -> MCU drops MCU_ACK=0 -> Host drops HOST_REQ=0.
    Enforces 10.0s timeout on each phase. On timeout: aborts to S2 and raises BenchAbort.
    """
    # Phase 1: Wait for MCU_ACK == 1
    start = clock.monotonic()
    while not bool(ad2.digital_io_read() & AD2_DIO_MCU_ACK):
        if clock.monotonic() - start >= timeout_s:
            ad2.set_safe_state()
            msg = "ABORT: Timeout in HS_MCU Phase 1 (waiting for MCU_ACK=1)"
            if log_fn:
                log_fn(f"[ad2] {msg}")
            raise BenchAbort(msg)
        clock.sleep(0.0005)

    # Phase 2: Host asserts HOST_REQ = 1
    ad2.digital_io_write(AD2_DIO_HOST_REQ, AD2_DIO_HOST_REQ)

    # Phase 3: Wait for MCU_ACK == 0
    start = clock.monotonic()
    while bool(ad2.digital_io_read() & AD2_DIO_MCU_ACK):
        if clock.monotonic() - start >= timeout_s:
            ad2.set_safe_state()
            msg = "ABORT: Timeout in HS_MCU Phase 3 (waiting for MCU_ACK=0)"
            if log_fn:
                log_fn(f"[ad2] {msg}")
            raise BenchAbort(msg)
        clock.sleep(0.0005)

    # Phase 4: Host drops HOST_REQ = 0
    ad2.digital_io_write(0, AD2_DIO_HOST_REQ)


def hs_host(ad2: AD2, clock=RealClock(), log_fn: Optional[Callable[[str], None]] = None, timeout_s: float = 10.0) -> None:
    """R-B1 HS_HOST (Host signals):
    Host asserts HOST_REQ=1 -> MCU asserts MCU_ACK=1 -> Host drops HOST_REQ=0 -> MCU drops MCU_ACK=0.
    Enforces 10.0s timeout on each phase. On timeout: aborts to S2 and raises BenchAbort.
    """
    # Phase 1: Host asserts HOST_REQ = 1
    ad2.digital_io_write(AD2_DIO_HOST_REQ, AD2_DIO_HOST_REQ)

    # Phase 2: Wait for MCU_ACK == 1
    start = clock.monotonic()
    while not bool(ad2.digital_io_read() & AD2_DIO_MCU_ACK):
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
) -> BusToken:
    """S5: Break-before-make electrical verification of bus release.
    1. Read-back OE & 0x00FF == 0 (Point 1: step start).
    2. Wait for BUSCHK up line (capturing optional drive=0x.. pattern).
    3. Read-back OE & 0x00FF == 0 (Point 2: pre-read).
    4. Read bus DIO0..DIO7. If != 0xFF:
       Force S2, record OE values, and raise BusContention(bus_up, msg, announced, oe_checks).
    5. Handshake HS_MCU.
    6. Wait for BUSCHK down line.
    7. Read bus DIO0..DIO7. If != 0x00:
       Force S2, record OE values, and raise BenchAbort(msg).
    8. Handshake HS_MCU.
    9. Creates and installs valid BusToken in tracker.
    """
    prefix = ""
    if step_id == STEP_T0A_MCU_TO_AD2:
        prefix = "step8"
    elif step_id == STEP_T0A_AD2_TO_MCU:
        prefix = "step9"

    oe_start = ad2.get_oe() & AD2_DIO_BUS_MASK
    if oe_start != 0:
        tracker.invalidate_token()
        ad2.set_safe_state()
        msg = f"ABORT: S5 contention check failed at step {step_id}: AD2 OE has bus bits enabled at start (0x{oe_start:04X})"
        if log_fn:
            log_fn(f"[ad2] {msg}")
        raise BenchAbort(msg)

    # 1. Wait for BUSCHK up line
    m_up = line_reader.expect(r"^\[m7_gpio_test\] BUSCHK up(?: drive=(0x[0-9a-fA-F]+))?", timeout_s=10.0)
    if rec_fn:
        if step_id == STEP_T0B_S5_NEG:
            rec_fn("step4_buschk_drive")
        elif prefix:
            rec_fn(f"{prefix}_buschk_up")
    announced = int(m_up.group(1), 16) if m_up.group(1) else None

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

    if rec_fn and prefix:
        rec_fn(f"{prefix}_up")
    hs_mcu(ad2, clock, log_fn=log_fn, timeout_s=10.0)

    # 2. Wait for BUSCHK down line
    line_reader.expect(r"^\[m7_gpio_test\] BUSCHK down", timeout_s=10.0)
    if rec_fn and prefix:
        rec_fn(f"{prefix}_buschk_down")

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

    if rec_fn and prefix:
        rec_fn(f"{prefix}_down")
    hs_mcu(ad2, clock, log_fn=log_fn, timeout_s=10.0)

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
) -> int:
    """Executes the full GPIO selftest suite with dependency injection.
    Accepts simulated mocks in testing or real hardware objects in production.
    """
    log_file = None
    if log_path:
        log_file = open(log_path, "w", encoding="utf-8", buffering=1)

    if expected_rev is None:
        expected_rev = get_git_rev()

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

    def rec(item_name: str) -> None:
        if visited_items is not None:
            visited_items.append(item_name)

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

    # Initialize 15-item host verdict checklist (B2: initialized to False)
    checklist_results: dict[str, bool] = {
        "sequence_complete": False,
        "t15_pass": False,
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

    line_reader = LineReader(serial_port, clock=clock, log_fn=log, on_abort_fn=on_abort, on_m4_line_fn=on_m4_line)

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
        m_rev = line_reader.expect(r"^\[m7_gpio_test\] Rev ([0-9a-f]{7})$", timeout_s=15.0)
        fw_rev = m_rev.group(1)
        if fw_rev != expected_rev:
            on_abort()
            raise BenchAbort(f"ABORT: Banner revision mismatch (got {fw_rev}, expected {expected_rev})")
        log(f"[host] CM7 banner revision verified: {fw_rev}")

        # Step 1: T15 announcement & verdict
        m_step1 = line_reader.expect(r"^\[m7_gpio_test\] STEP id=(\d+) bus=(NONE|MCU|AD2) v=(\d+)", timeout_s=10.0)
        rec("step1_announcement")
        step_tracker.validate_announcement(m_step1.group(0), ad2, log_fn=log)

        m_t15 = line_reader.expect(r"^\[m7_gpio_test\] T15 VERDICT: (PASS|FAIL)", timeout_s=15.0)
        rec("step1_verdict")
        if m_t15.group(1) == "PASS":
            checklist_results["t15_pass"] = True
            rec("HOST_CHECK t15_pass")
            log("[host] T15 dual-core race PASSED.")
        else:
            log("[host] T15 dual-core race FAILED!")

        # Step 2: IDLE announcement
        m_step2 = line_reader.expect(r"^\[m7_gpio_test\] STEP id=(\d+) bus=(NONE|MCU|AD2) v=(\d+)", timeout_s=10.0)
        rec("step2_announcement")
        step_tracker.validate_announcement(m_step2.group(0), ad2, log_fn=log)

        m_idle = line_reader.expect(r"^\[m7_gpio_test\] GPIO_BENCH v(\d+) idle$", timeout_s=10.0)
        rec("step2_idle")
        proto_v = int(m_idle.group(1))
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
            rec("HOST_CHECK t0b_a_oe_guards")
            log("[ad2] T0b(a) host OE guards PASSED.")
        else:
            log("[ad2] T0b(a) host OE guards FAILED!")
            return 1

        # 4. Synchronize session start with MCU: HS_HOST
        log("[host] Handshaking session start with MCU via HS_HOST...")
        rec("session_start")
        hs_host(ad2, clock=clock, log_fn=log, timeout_s=10.0)
        log("[host] Session start synchronized.")

        # 5. Step 3: T0b(b) Firmware Guards Verification
        m_step3 = line_reader.expect(r"^\[m7_gpio_test\] STEP id=(\d+) bus=(NONE|MCU|AD2) v=(\d+)", timeout_s=10.0)
        rec("step3_announcement")
        step_tracker.validate_announcement(m_step3.group(0), ad2, log_fn=log)

        m_claim = line_reader.expect(r"^\[m7_gpio_test\] T0b_b guards verification: (PASS|FAIL)", timeout_s=10.0)
        rec("step3_guards_pass")
        if m_claim.group(1) == "PASS":
            checklist_results["t0b_b_claim_guards"] = True
            rec("HOST_CHECK t0b_b_claim_guards")
            log("[ad2] T0b(b) firmware claim guards PASSED.")
        else:
            log("[ad2] T0b(b) firmware claim guards FAILED!")

        # 6. Step 4: T0b(c) S5 Negative Control (B4)
        m_step4 = line_reader.expect(r"^\[m7_gpio_test\] STEP id=(\d+) bus=(NONE|MCU|AD2) v=(\d+)", timeout_s=10.0)
        rec("step4_announcement")
        step_tracker.validate_announcement(m_step4.group(0), ad2, log_fn=log)

        log("[ad2] Step T0b(c): Calling s5_verify_released to verify S5 guard stops contention...")
        t0b_c_contention_caught = False
        try:
            s5_verify_released(ad2, step_tracker, STEP_T0B_S5_NEG, line_reader, clock=clock, log_fn=log, rec_fn=rec)
            log("[ad2] T0b(c) S5 negative control FAILED: s5_verify_released did not detect contention!")
        except BusContention as bc:
            oe_p1, oe_p2, oe_p3 = bc.oe_checks if bc.oe_checks else (1, 1, 1)
            log(f"[ad2] MCU announced drive pattern: 0x{bc.announced:02X}" if bc.announced is not None else "[ad2] MCU announced drive pattern: None")
            log(f"[ad2] Contention verified on BUSCHK up: read 0x{bc.value:02X} (announced=0x{bc.announced:02X})")
            log(f"[ad2] T0b(c) OE checks: Point 1 (start)=0x{oe_p1:02X}, Point 2 (pre-read)=0x{oe_p2:02X}, Point 3 (post-check)=0x{oe_p3:02X}")
            if bc.announced is not None and bc.value == bc.announced and oe_p1 == 0 and oe_p2 == 0 and oe_p3 == 0:
                t0b_c_contention_caught = True
                checklist_results["t0b_c_contention"] = True
                rec("HOST_CHECK t0b_c_contention")
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
        rec("t0b_c_sync")
        hs_mcu(ad2, clock=clock, log_fn=log, timeout_s=10.0)

        # MCU releases bus and announces safe state
        line_reader.expect(r"^\[m7_gpio_test\] T0b_c safe state verified: all wired pins input", timeout_s=10.0)
        rec("step4_safe_verified")

        # 7. Step 5: T0b(d) 10-second Timeout Test
        m_step5 = line_reader.expect(r"^\[m7_gpio_test\] STEP id=(\d+) bus=(NONE|MCU|AD2) v=(\d+)", timeout_s=10.0)
        rec("step5_announcement")
        step_tracker.validate_announcement(m_step5.group(0), ad2, log_fn=log)

        log("[ad2] Step T0b(d): Deliberately silent for 10.5 seconds without asserting HOST_REQ...")
        rec("HOST_WAIT t0b_d_timeout")
        clock.sleep(10.5)

        m_tout = line_reader.expect(r"^\[m7_gpio_test\] timeout expired -> bench_safe\(\) invoked \((PASS|FAIL)\)", timeout_s=5.0)
        rec("step5_timeout_pass")
        if m_tout.group(1) == "PASS":
            checklist_results["t0b_d_timeout"] = True
            rec("HOST_CHECK t0b_d_timeout")
            log("[ad2] T0b(d) timeout test PASSED.")
        else:
            log("[ad2] T0b(d) timeout test FAILED!")

        # 8. Step 6: T0a Loose Lines Check (13 lines)
        m_step6 = line_reader.expect(r"^\[m7_gpio_test\] STEP id=(\d+) bus=(NONE|MCU|AD2) v=(\d+)", timeout_s=10.0)
        rec("step6_announcement")
        step_tracker.validate_announcement(m_step6.group(0), ad2, log_fn=log)

        log("[ad2] Step T0a: Testing loose lines (13 lines)...")
        line_reader.expect(r"^\[m7_gpio_test\] BUSCHK up", timeout_s=10.0)
        rec("step6_buschk_up")
        dio_up = ad2.digital_io_read() & AD2_DIO_LOOSE_13_MASK
        rec("HOST_READ loose_up")
        log(f"[ad2] Loose check pull-up read: 0x{dio_up:04X} (expected 0x{AD2_DIO_LOOSE_13_MASK:04X})")
        loose_up_ok = (dio_up == AD2_DIO_LOOSE_13_MASK)
        if not loose_up_ok:
            diff = dio_up ^ AD2_DIO_LOOSE_13_MASK
            for b in range(16):
                if diff & (1 << b):
                    log(f"[ad2] Loose check pull-up FAILED on DIO{b} (observed=0, expected=1)")
        rec("step6_up")
        hs_mcu(ad2, clock=clock, log_fn=log, timeout_s=10.0)

        line_reader.expect(r"^\[m7_gpio_test\] BUSCHK down", timeout_s=10.0)
        rec("step6_buschk_down")
        dio_down = ad2.digital_io_read() & AD2_DIO_LOOSE_13_MASK
        rec("HOST_READ loose_down")
        log(f"[ad2] Loose check pull-down read: 0x{dio_down:04X} (expected 0x0000)")
        loose_down_ok = (dio_down == 0x0000)
        if not loose_down_ok:
            for b in range(16):
                if dio_down & (1 << b):
                    log(f"[ad2] Loose check pull-down FAILED on DIO{b} (observed=1, expected=0)")
        rec("step6_down")
        hs_mcu(ad2, clock=clock, log_fn=log, timeout_s=10.0)

        line_reader.expect(r"^\[m7_gpio_test\] T0a_loose: DONE \(host verdict\)", timeout_s=10.0)
        rec("step6_loose_done")
        if loose_up_ok and loose_down_ok:
            checklist_results["t0a_loose"] = True
            log("[ad2] T0a loose check PASSED (all 13 lines).")
        else:
            log("[ad2] T0a loose check FAILED!")

        # 9. Step 7: T0a Handshake Lines (PE2 / PE7)
        m_step7 = line_reader.expect(r"^\[m7_gpio_test\] STEP id=(\d+) bus=(NONE|MCU|AD2) v=(\d+)", timeout_s=10.0)
        rec("step7_announcement")
        step_tracker.validate_announcement(m_step7.group(0), ad2, log_fn=log)

        log("[ad2] Step T0a: Testing handshake lines (PE2 HOST_REQ, PE7 MCU_ACK)...")
        # 1. HS_HOST: MCU validates PE2 HIGH then LOW
        rec("step7_pe2")
        hs_host(ad2, clock=clock, log_fn=log, timeout_s=10.0)
        line_reader.expect(r"^\[m7_gpio_test\] Handshake PE2: OK", timeout_s=10.0)
        rec("step7_handshake_pe2")

        # 2. HS_MCU: Host validates PE7 HIGH then LOW
        rec("step7_pe7")
        hs_mcu(ad2, clock=clock, log_fn=log, timeout_s=10.0)
        line_reader.expect(r"^\[m7_gpio_test\] T0a_handshake: DONE \(host verdict\)", timeout_s=10.0)
        rec("step7_handshake_done")
        checklist_results["t0a_handshake"] = True
        log("[ad2] T0a handshake lines PASSED.")

        # 10. Step 8: T0a Walking-1 MCU -> AD2 (11 lines)
        m_step8 = line_reader.expect(r"^\[m7_gpio_test\] STEP id=(\d+) bus=(NONE|MCU|AD2) v=(\d+)", timeout_s=10.0)
        rec("step8_announcement")
        step_tracker.validate_announcement(m_step8.group(0), ad2, log_fn=log)

        log("[ad2] Step T0a: Walking-1 MCU -> AD2 (11 lines)...")
        # S5 break-before-make verification
        s5_verify_released(ad2, step_tracker, STEP_T0A_MCU_TO_AD2, line_reader, clock=clock, log_fn=log, rec_fn=rec)

        mcu_walk_bits = [9, 11, 0, 1, 2, 3, 4, 5, 6, 7, 14]
        mcu_walk_mask = (1 << 9) | (1 << 11) | (1 << 14) | AD2_DIO_BUS_MASK
        mcu_walk_passed = True

        for exp_bit in mcu_walk_bits:
            rec("HOST_READ mcu_bit")
            # During HS_MCU phase 1, MCU is asserting ACK and driving exactly exp_bit
            # Wait for MCU_ACK
            start_w = clock.monotonic()
            while not bool(ad2.digital_io_read() & AD2_DIO_MCU_ACK):
                if clock.monotonic() - start_w >= 10.0:
                    on_abort()
                    raise BenchAbort(f"ABORT: Timeout waiting for MCU_ACK on walking-1 MCU->AD2 bit {exp_bit}")
                clock.sleep(0.0005)

            dio_val = ad2.digital_io_read() & mcu_walk_mask
            exp_val = (1 << exp_bit)
            bit_ok = (dio_val == exp_val)
            if not bit_ok:
                mcu_walk_passed = False
                log(f"[ad2] Walking-1 MCU->AD2 DIO{exp_bit} FAILED! Observed=0x{dio_val:04X}, Expected=0x{exp_val:04X}")
                diff = dio_val ^ exp_val
                for b in range(16):
                    if diff & (1 << b):
                        obs_b = 1 if (dio_val & (1 << b)) else 0
                        exp_b = 1 if (exp_val & (1 << b)) else 0
                        log(f"[ad2]   Bit mismatch on DIO{b}: observed={obs_b}, expected={exp_b}")
            else:
                log(f"[ad2] Walking-1 MCU->AD2 DIO{exp_bit}: OK (0x{dio_val:04X})")

            rec(f"step8_mcu_{exp_bit}")
            # Complete HS_MCU phases 2..4
            ad2.digital_io_write(AD2_DIO_HOST_REQ, AD2_DIO_HOST_REQ)
            start_w = clock.monotonic()
            while bool(ad2.digital_io_read() & AD2_DIO_MCU_ACK):
                if clock.monotonic() - start_w >= 10.0:
                    on_abort()
                    raise BenchAbort(f"ABORT: Timeout in HS_MCU Phase 3 on walking-1 bit {exp_bit}")
                clock.sleep(0.0005)
            ad2.digital_io_write(0, AD2_DIO_HOST_REQ)

        # MCU -> AD2 Negative Control
        line_reader.expect(r"^\[m7_gpio_test\] T0a_mcu control ready", timeout_s=10.0)
        rec("step8_control_ready")
        rec("HOST_READ mcu_ctrl")
        # Read during handshake
        start_c = clock.monotonic()
        while not bool(ad2.digital_io_read() & AD2_DIO_MCU_ACK):
            if clock.monotonic() - start_c >= 10.0:
                on_abort()
                raise BenchAbort("ABORT: Timeout waiting for MCU_ACK on MCU->AD2 negative control")
            clock.sleep(0.0005)

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

        rec("step8_ctrl")
        ad2.digital_io_write(AD2_DIO_HOST_REQ, AD2_DIO_HOST_REQ)
        start_c = clock.monotonic()
        while bool(ad2.digital_io_read() & AD2_DIO_MCU_ACK):
            if clock.monotonic() - start_c >= 10.0:
                on_abort()
                raise BenchAbort("ABORT: Timeout in HS_MCU Phase 3 on MCU->AD2 negative control")
            clock.sleep(0.0005)
        ad2.digital_io_write(0, AD2_DIO_HOST_REQ)

        line_reader.expect(r"^\[m7_gpio_test\] T0a_mcu: DONE \(host verdict\)", timeout_s=10.0)
        rec("step8_mcu_done")
        if mcu_walk_passed:
            checklist_results["t0a_mcu_to_ad2"] = True
            log("[ad2] Walking-1 MCU->AD2 PASSED.")
        else:
            log("[ad2] Walking-1 MCU->AD2 FAILED!")

        # 11. Step 9: T0a Walking-1 AD2 -> MCU (10 lines)
        m_step9 = line_reader.expect(r"^\[m7_gpio_test\] STEP id=(\d+) bus=(NONE|MCU|AD2) v=(\d+)", timeout_s=10.0)
        rec("step9_announcement")
        step_tracker.validate_announcement(m_step9.group(0), ad2, log_fn=log)

        log("[ad2] Step T0a: Walking-1 AD2 -> MCU (10 lines)...")
        token = s5_verify_released(ad2, step_tracker, STEP_T0A_AD2_TO_MCU, line_reader, clock=clock, log_fn=log, rec_fn=rec)

        line_reader.expect(r"^\[m7_gpio_test\] T0a_ad2 ready", timeout_s=10.0)
        rec("step9_ad2_ready")
        rec("step9_ready")
        hs_mcu(ad2, clock=clock, log_fn=log, timeout_s=10.0)

        ad2_walk_bits = [8, 10, 0, 1, 2, 3, 4, 5, 6, 7]
        ad2_walk_passed = True

        for b in ad2_walk_bits:
            rec("HOST_DRIVE ad2_bit")
            if b < 8:
                drive_bus(ad2, step_tracker, 1 << b)
            else:
                ad2.set_oe(AD2_DIO_HOST_REQ | (1 << b), GPIO_BENCH_BUS_NONE)
                ad2.digital_io_write(1 << b, 1 << b)

            # Signal data ready via HS_HOST
            rec(f"step9_ad2_{b}")
            hs_host(ad2, clock=clock, log_fn=log, timeout_s=10.0)

            # Wait for MCU announcement
            m_bit = line_reader.expect(r"^\[m7_gpio_test\] T0a_ad2 bit=(\d+) idr=0x([0-9a-fA-F]+) exp=0x([0-9a-fA-F]+) (OK|FAIL)", timeout_s=10.0)
            rec(f"step9_ad2_{b}_res")
            bit_status = m_bit.group(4)
            if bit_status != "OK":
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
        line_reader.expect(r"^\[m7_gpio_test\] T0a_ad2 analog control ready", timeout_s=10.0)
        rec("step9_analog_ready")
        rec("HOST_DRIVE ad2_analog")
        drive_bus(ad2, step_tracker, 0xFF)
        rec("step9_analog")
        hs_host(ad2, clock=clock, log_fn=log, timeout_s=10.0)

        m_analog = line_reader.expect(r"^\[m7_gpio_test\] T0a_ad2 analog control result: (OK|FAIL)", timeout_s=10.0)
        rec("step9_analog_res")
        if m_analog.group(1) != "OK":
            ad2_walk_passed = False
            log("[ad2] AD2->MCU analog control reported FAIL by MCU!")

        ad2.set_safe_state()
        ad2.set_oe(AD2_DIO_HOST_REQ, GPIO_BENCH_BUS_NONE)

        m_ad2_verdict = line_reader.expect(r"^\[m7_gpio_test\] T0a_ad2: (PASS|FAIL)", timeout_s=10.0)
        rec("step9_verdict")
        if m_ad2_verdict.group(1) == "PASS" and ad2_walk_passed:
            checklist_results["t0a_ad2_to_mcu"] = True
            log("[ad2] Walking-1 AD2->MCU PASSED.")
        else:
            log("[ad2] Walking-1 AD2->MCU FAILED!")

        # 12. Step 10: T0a Scope 1 & Scope 2 DC Levels
        m_step10 = line_reader.expect(r"^\[m7_gpio_test\] STEP id=(\d+) bus=(NONE|MCU|AD2) v=(\d+)", timeout_s=10.0)
        rec("step10_announcement")
        step_tracker.validate_announcement(m_step10.group(0), ad2, log_fn=log)

        log("[ad2] Step T0a: Testing Scope 1 (PF14) and Scope 2 (PF15) DC levels...")
        ad2.analog_in_setup_dc(channel=0, voltage_range=10.0)
        ad2.analog_in_setup_dc(channel=1, voltage_range=10.0)
        clock.sleep(0.05)

        line_reader.expect(r"^\[m7_gpio_test\] Scope LOW ready", timeout_s=10.0)
        rec("step10_low_ready")
        clock.sleep(0.01)
        rec("HOST_MEASURE scope_low")
        v1_low = ad2.analog_in_read_dc(0)
        v2_low = ad2.analog_in_read_dc(1)
        log(f"[ad2] Scope LOW: PF14={v1_low:.3f}V, PF15={v2_low:.3f}V (expected <= 0.4V)")
        scope_low_ok = (v1_low <= 0.4 and v2_low <= 0.4)
        if not scope_low_ok:
            if v1_low > 0.4:
                log(f"[ad2]   Scope 1 (PF14) VOL out of range: {v1_low:.3f}V > 0.4V")
            if v2_low > 0.4:
                log(f"[ad2]   Scope 2 (PF15) VOL out of range: {v2_low:.3f}V > 0.4V")
        rec("step10_low")
        hs_mcu(ad2, clock=clock, log_fn=log, timeout_s=10.0)

        line_reader.expect(r"^\[m7_gpio_test\] Scope HIGH ready", timeout_s=10.0)
        rec("step10_high_ready")
        clock.sleep(0.01)
        rec("HOST_MEASURE scope_high")
        v1_high = ad2.analog_in_read_dc(0)
        v2_high = ad2.analog_in_read_dc(1)
        log(f"[ad2] Scope HIGH: PF14={v1_high:.3f}V, PF15={v2_high:.3f}V (expected >= 2.9V)")
        scope_high_ok = (v1_high >= 2.9 and v2_high >= 2.9)
        if not scope_high_ok:
            if v1_high < 2.9:
                log(f"[ad2]   Scope 1 (PF14) VOH out of range: {v1_high:.3f}V < 2.9V")
            if v2_high < 2.9:
                log(f"[ad2]   Scope 2 (PF15) VOH out of range: {v2_high:.3f}V < 2.9V")
        rec("step10_high")
        hs_mcu(ad2, clock=clock, log_fn=log, timeout_s=10.0)

        line_reader.expect(r"^\[m7_gpio_test\] T0a_scope: DONE \(host verdict\)", timeout_s=10.0)
        rec("step10_scope_done")
        if scope_low_ok and scope_high_ok:
            checklist_results["t0a_scope"] = True
            log("[ad2] Scope check PASSED.")
        else:
            log("[ad2] Scope check FAILED!")

        # 13. Step 99: Final Verdicts and CM4 Drain
        m_step99 = line_reader.expect(r"^\[m7_gpio_test\] STEP id=(\d+) bus=(NONE|MCU|AD2) v=(\d+)", timeout_s=10.0)
        rec("step99_announcement")
        step_tracker.validate_announcement(m_step99.group(0), ad2, log_fn=log)
        checklist_results["sequence_complete"] = True

        m_fw_all = line_reader.expect(r"^\[m7_gpio_test\] VERDICT: (ALL GPIO SELF-TESTS PASSED|GPIO SELF-TESTS FAILED)", timeout_s=10.0)
        rec("step99_verdict")
        if m_fw_all.group(1) == "ALL GPIO SELF-TESTS PASSED":
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

        # Explicitly print the 15-item checklist (B2)
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
