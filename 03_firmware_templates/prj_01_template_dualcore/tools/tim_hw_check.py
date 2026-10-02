#!/usr/bin/env python3
"""Host side of the 03_tim selftest on prj_01 (preset selftest-tim), phase F0.

Runs one complete bench session against CM7/App/Src/tim_selftest.c with the
Analog Discovery 2 and writes ONE capture file: every console line and every
host measurement, with timestamps, from "[host] argv:" to "HOST VERDICT=".
Plan and meaning of every test: 01_shared_drivers/03_tim/IMPLEMENTATION_PLAN.md;
wiring and rules S1-S10: 01_shared_drivers/00_gpio/README.md section 8.

    py tools/flash.py --preset selftest-tim
    py tools/tim_hw_check.py                  # resets the board through SWD and runs
    py tools/tim_hw_check.py --allow-dirty    # development only (capture tim_dev_...)

F0 tests: T0a (loose lines, walking 1, control), TREF (MCO1 = HSE/8 on PA8,
measured by the AD2, |e| <= 2 % and within 3 sqrt(u^2 + r^2) of HAL_GetTick
against the host clock; control PA8 in AF1), T0b(a) and (e) on mock DLLs
before the AD2 is opened,
then T0b(b)(c)(d)(f)(g) on the bench.

Image identity (00_gpio README 9.4, R3-a/R3-b/R4-a), checked before any output
is enabled: Rev(CM7) == Rev(CM4), id(CM7) == id(CM4) (never "unknown"), each
Rev without -dirty == git rev-parse --short HEAD; without --allow-dirty also no
-dirty Rev and the image id == the id the host computes (host_build_id() of
gpio_hw_check.py: one definition of the method).

Electrical safety (no series resistors on the bench):
  S2  the AD2 opens and closes in S2: Digital-IO all input and Digital Out
      reset, stopped, every channel disabled and idle Zet (tools/ad2.py);
      again at every step boundary and on any exception (try/finally).
  S3  every OutputEnable change goes through ad2.ad2_set_oe() with the bus
      owner of the CURRENT step; T0b(a) proves the guard on a mock first.
  S5  before either side drives the bus, both MCU pulls must be followed
      (s5_released); the same function is checked in T0b(c) and T0b(f), where
      it must fail.
  S6  1 ms or more between one side releasing and the other driving.
  S7  a step is acted on only if its announcement matches STEPS below.
  S8  no output is enabled before the banner `TIM_BENCH v1 idle` of the right
      image has been read.
  S11 Digital Out only through ad2.ad2_set_dout(): DIO1/DIO3, "bus = AD2"
      steps, Digital-IO OE read back 0 on the Digital Out lines while it runs
      (S11 d; HOST_REQ may stay enabled); T0b(e) proves the guard on a mock
      first.
  S12 is enforced by the firmware (bench_claim reads TIM1 back); T0b(g) runs
      the S5 check with PE9/PE11 already in AF1.
Standard library only (ctypes); `py`, not `python`.
"""

from __future__ import annotations

import argparse
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
from typing import Callable, Optional

PROJECT = pathlib.Path(__file__).resolve().parents[1]
WORKSPACE = PROJECT.parents[1]
sys.path.insert(0, str(WORKSPACE / "tools"))      # ad2, monitor: the workspace modules, ahead of
if str(PROJECT / "tools") not in sys.path:        # the project shims (tools/monitor.py runs the
    sys.path.append(str(PROJECT / "tools"))       # monitor as __main__ when imported)
os.environ.setdefault("STM32_PROJECT_ROOT", str(PROJECT))

import ad2 as ad2mod                                   # noqa: E402
from ad2 import (AD2, AD2SafetyViolation, GPIO_BENCH_BUS_AD2,  # noqa: E402
                 GPIO_BENCH_BUS_MCU, GPIO_BENCH_BUS_NONE)
from monitor import SerialPort, autodetect             # noqa: E402
# One definition of the image-identity method and of the tree record (00_gpio
# README 9.4), and the 00_gpio T0b(a) mock test of the S3 guard as it is.
from gpio_hw_check import (cube_programmer, git_rev, host_build_id,  # noqa: E402
                           t0b_guard_selftest, tree_record)

BENCH_VERSION = 1

# ---- Wiring (00_gpio README section 8) ----------------------------------------
DIO_BUS = 0x00FF        # DIO0..7  <- PE8..PE15
DIO_PE9 = 1 << 1        # TIM1_CH1, Digital Out line (S11)
DIO_PE11 = 1 << 3       # TIM1_CH2, Digital Out line (S11)
DIO_PE3 = 1 << 8
DIO_PE5 = 1 << 9        # TIM15_CH1, MCU only
DIO_PE4 = 1 << 10
DIO_PE6 = 1 << 11       # TIM15_CH2, MCU only
DIO_REQ = 1 << 12       # HOST_REQ -> PE2
DIO_ACK = 1 << 13       # MCU_ACK  <- PE7
DIO_PA8 = 1 << 14       # MCO1     <- PA8
# T0a: the 13 lines the MCU pulls, in the firmware's order (k_lines[]).
T0A_DIO = [0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 14]
T0A_LINES = sum(1 << d for d in T0A_DIO)              # 0x4FFF
T0A_CTRL_DIO = 9                                       # pulled the wrong way by the firmware

BUS = {"NONE": GPIO_BENCH_BUS_NONE, "MCU": GPIO_BENCH_BUS_MCU, "AD2": GPIO_BENCH_BUS_AD2}

# S7: the host's own copy of the firmware's step table (name, bus owner), in order.
STEPS = [
    ("T0A", "NONE"), ("TREF", "NONE"), ("T0B", "NONE"), ("T0B_S5NEG", "MCU"), ("T0B_TIMEOUT", "NONE"),
    ("T0B_DOUT", "AD2"), ("T0B_S12", "AD2"), ("DONE", "NONE"),
]

HS_TIMEOUT_S = 10.0
LA_RATE_HZ = 100e6

TREF_EXPECTED_HZ = 1e6          # MCO1 = HSE_VALUE (8 MHz) / 8
# TREF criterion, owner's decision of 2026-10-01 (the plan's 500 ppm cannot hold on
# this board: its HSE, the ST-LINK MCO, measured +13 171 and +13 685 ppm in
# tim_f0_bench_d01061e.txt and tim_f0_bench_38d15c1.txt): |e_ppm| <= 2 %, and the
# AD2 estimate within 3 sqrt(u^2 + r^2) of the HAL_GetTick-against-host cross-check:
# u its uncertainty, r the one-sample resolution of the TREF capture (second decision
# of 2026-10-01; with 3 u alone, tim_f0_bench_e30622b.txt passed at 574 against 574).
TREF_MAX_PPM = 20000.0
TREF_XCHK_K = 3.0
TREF_XCHK_JITTER_S = 2e-3       # arrival jitter assumed per console line (USB VCP)

DOUT_DIVIDER, DOUT_LOW, DOUT_HIGH = 1, 50, 50          # 100 MHz / 1 / (50 + 50) = 1 MHz
DOUT_TOL = 0.001                # +-0.1 % (plan, common tolerances; AD2 against itself here)
DOUT_HIGH_TOL_S = 15e-9         # +-15 ns


class BenchAbort(Exception):
    pass


# ---- Procedure checklist (00_gpio README section 8) -----------------------------------

# Programs that must not run during a capture: the WaveForms GUI holds the AD2,
# and a debug server holds the ST-LINK (captures are made without a debugger).
FORBIDDEN_PROCESSES = ("WaveForms.exe", "ST-LINK_gdbserver.exe", "openocd.exe", "STM32CubeProgrammer.exe")


def running_forbidden() -> Optional[list[str]]:
    """The forbidden programs that `tasklist` lists, or None if it cannot be read."""
    try:
        out = subprocess.run(["tasklist", "/FO", "CSV", "/NH"], capture_output=True, text=True, check=True).stdout
    except (OSError, subprocess.CalledProcessError):
        return None
    names = {line.split('","')[0].strip('"').lower() for line in out.splitlines() if line.strip()}
    return [p for p in FORBIDDEN_PROCESSES if p.lower() in names]


def checklist(stamp: Callable[[str], None], operator: Optional[str]) -> bool:
    """Prints the procedure checklist. The wiring items can only be confirmed by
    the person at the bench (--checklist-confirmed NAME); the process items are
    checked here. Returns False when a forbidden program runs."""
    mark = "x" if operator else " "
    who = f"confirmed by {operator} (--checklist-confirmed)" if operator else "NOT confirmed by an operator"
    stamp(f"[host] checklist [{mark}] GND first, then the DIO lines per 00_gpio README 8; nothing on W1/W2/V+/V-; "
          f"no series resistors; AD2 on USB while the Nucleo is powered: {who}")
    stamp(f"[host] checklist [{mark}] selftest-tim flashed with this script stopped: {who}")
    running = running_forbidden()
    if running is None:
        stamp("[host] checklist [ ] WaveForms GUI closed, no debugger attached: tasklist failed, not checked")
        return False
    stamp(f"[host] checklist [{'x' if not running else ' '}] WaveForms GUI closed, no debugger attached "
          f"(tasklist: {', '.join(running) if running else 'none of ' + ', '.join(FORBIDDEN_PROCESSES)})")
    return not running


# ---- T0b(e): the S11 guard, on a mock DLL, before the real device is opened -----------

class _MockDwfDout:
    """Records every DLL call. Digital-IO OE starts at `oe`."""

    def __init__(self, oe: int = 0) -> None:
        self.calls: list[tuple] = []
        self.oe = oe
        self.en = [0] * 16
        self.idle = [0] * 16
        self.out = [0] * 16

    def FDwfDigitalIOReset(self, h):                    # noqa: N802
        self.calls.append(("IOReset",))
        self.oe = 0

    def FDwfDigitalIOConfigure(self, h):                # noqa: N802
        self.calls.append(("IOConfigure",))

    def FDwfDigitalIOOutputEnableGet(self, h, ref):     # noqa: N802
        ref._obj.value = self.oe

    def FDwfDigitalOutReset(self, h):                   # noqa: N802
        self.calls.append(("Reset",))
        self.en = [0] * 16

    def FDwfDigitalOutConfigure(self, h, f):            # noqa: N802
        self.calls.append(("Configure", f.value))

    def FDwfDigitalOutEnableSet(self, h, c, f):         # noqa: N802
        self.calls.append(("EnableSet", c.value, f.value))
        self.en[c.value] = f.value

    def FDwfDigitalOutEnableGet(self, h, c, ref):       # noqa: N802
        ref._obj.value = self.en[c.value]

    def FDwfDigitalOutIdleSet(self, h, c, v):           # noqa: N802
        self.calls.append(("IdleSet", c.value, v.value))
        self.idle[c.value] = v.value

    def FDwfDigitalOutIdleGet(self, h, c, ref):         # noqa: N802
        ref._obj.value = self.idle[c.value]

    def FDwfDigitalOutOutputSet(self, h, c, v):         # noqa: N802
        self.calls.append(("OutputSet", c.value, v.value))
        self.out[c.value] = v.value

    def FDwfDigitalOutOutputGet(self, h, c, ref):       # noqa: N802
        ref._obj.value = self.out[c.value]


def _enabling(calls: list[tuple]) -> list[tuple]:
    return [c for c in calls if (c[0] == "EnableSet" and c[2] == 1) or c == ("Configure", 1)]


def _s2_reached(m: _MockDwfDout) -> bool:
    return (("IOReset",) in m.calls and ("Reset",) in m.calls and ("Configure", 0) in m.calls and
            all(("EnableSet", c, 0) in m.calls and ("IdleSet", c, ad2mod.DWF_DOUT_IDLE_ZET) in m.calls
                for c in range(16)))


def t0b_dout_guard_selftest(log: Callable[[str], None]) -> bool:
    zet, low = ad2mod.DWF_DOUT_IDLE_ZET, ad2mod.DWF_DOUT_IDLE_LOW
    dout = ad2mod.AD2_DOUT_IDLE_LOW_STEP     # "T0B_DOUT", the step of the T0b(f) control
    forbidden = [  # (mask, owner, idle, Digital-IO OE, step, why)
        (1 << 0, "AD2", zet, 0, dout, "DIO0 (PE8)"), (1 << 2, "AD2", zet, 0, dout, "DIO2 (PE10)"),
        (1 << 9, "AD2", zet, 0, dout, "DIO9 (PE5, MCU only)"), (1 << 12, "AD2", zet, 0, dout, "DIO12 (HOST_REQ)"),
        (1 << 14, "AD2", zet, 0, dout, "DIO14 (PA8, MCU only)"),
        (DIO_PE9 | (1 << 2), "AD2", zet, 0, dout, "DIO1 with DIO2"),
        (DIO_PE9, "NONE", zet, 0, "T0A", "bus = NONE"), (DIO_PE9, "MCU", zet, 0, "T0B_S5NEG", "bus = MCU"),
        (DIO_PE11, "MCU", zet, 0, "T0B_S5NEG", "bus = MCU"),
        (DIO_PE9, "AD2", ad2mod.DWF_DOUT_IDLE_HIGH, 0, dout, "idle High"),
        (DIO_PE9, "AD2", ad2mod.DWF_DOUT_IDLE_INIT, 0, dout, "idle Init"),
        (DIO_PE9, "AD2", low, 0, "T0B_S12", "idle Low outside T0B_DOUT (R2-4)"),
        (DIO_PE9, "AD2", low, 0, None, "idle Low with no step named (R2-4)"),
        (DIO_PE9, "AD2", zet, DIO_PE9 | DIO_REQ, dout, "Digital-IO OE on DIO1 (S11 d)"),
        (DIO_PE11, "AD2", zet, DIO_PE11, dout, "Digital-IO OE on DIO3 (S11 d)"),
        (DIO_PE9 | DIO_PE11, "AD2", zet, DIO_PE11 | DIO_REQ, dout, "DIO1|DIO3 with OE on DIO3 (S11 d)"),
    ]
    ok = True
    for mask, owner, idle, oe, step, why in forbidden:
        m = _MockDwfDout(oe)
        try:
            ad2mod.ad2_set_dout(m, 1, mask, BUS[owner], idle, step)
            refused = False
        except AD2SafetyViolation:
            refused = True
        good = refused and not _enabling(m.calls) and _s2_reached(m)
        ok = ok and good
        log(f"[ad2] T0B(e) ad2_set_dout(0x{mask:04X}, bus={owner}, idle={idle}, step={step}) [{why}]: "
            f"{'refused, S2 reached, no enabling call reached the DLL' if good else 'NOT REFUSED CLEANLY'}")
    # The controls; HOST_REQ (DIO12) enabled in Digital-IO is not a Digital Out line (S11 d).
    # Zet is accepted in any "bus = AD2" step; Low only in T0B_DOUT.
    for mask, idle, oe, step in ((DIO_PE9, zet, 0, dout), (DIO_PE11, zet, 0, dout), (DIO_PE9 | DIO_PE11, zet, 0, dout),
                                 (DIO_PE9, low, 0, dout), (DIO_PE9, zet, 0, "T0B_S12"),
                                 (DIO_PE9, zet, DIO_REQ, dout), (DIO_PE9 | DIO_PE11, zet, DIO_REQ, dout)):
        m = _MockDwfDout(oe)
        ad2mod.ad2_set_dout(m, 1, mask, GPIO_BENCH_BUS_AD2, idle, step)
        en = [c for c in range(16) if (mask >> c) & 1]
        order_ok = True
        for c in en:
            i_en = m.calls.index(("EnableSet", c, 1))
            order_ok = order_ok and m.calls.index(("IdleSet", c, idle)) < i_en and \
                m.calls.index(("OutputSet", c, ad2mod.DWF_DOUT_OUTPUT_PUSHPULL)) < i_en
        good = (order_ok and m.calls[-1] == ("Configure", 1) and
                [c[1] for c in _enabling(m.calls) if c[0] == "EnableSet"] == en)
        ok = ok and good
        log(f"[ad2] T0B(e) control ad2_set_dout(0x{mask:04X}, bus=AD2, idle={idle}, step={step}, "
            f"Digital-IO OE=0x{oe:04X}): "
            f"{'passed to the DLL, Idle and push-pull before EnableSet(1), Configure(1) last' if good else 'BLOCKED'}")
    return ok


# ---- Console reader --------------------------------------------------------------------

class Console:
    """Reads the ST-LINK VCP in a thread; every line is timestamped and kept."""

    def __init__(self, port: SerialPort, t0: float, sink: Callable[[str], None]) -> None:
        self._port = port
        self._t0 = t0
        self._sink = sink
        self._q: "queue.Queue[tuple[float, str]]" = queue.Queue()
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
            t = time.monotonic()
            buf.extend(chunk)
            while b"\n" in buf:
                raw, buf = buf.split(b"\n", 1)
                line = raw.decode("utf-8", errors="replace").rstrip("\r")
                self._sink(line)
                if "[tim] ABORT" in line:
                    self.fw_abort = line
                self._q.put((t, line))

    def expect_t(self, pattern: str, timeout_s: float) -> tuple[re.Match, float]:
        rx = re.compile(pattern)
        end = time.monotonic() + timeout_s
        while True:
            left = end - time.monotonic()
            if left <= 0:
                raise BenchAbort(f"timeout waiting for /{pattern}/")
            try:
                t, line = self._q.get(timeout=min(left, 0.2))
            except queue.Empty:
                continue
            m = rx.search(line)
            if m:
                return m, t

    def expect(self, pattern: str, timeout_s: float) -> re.Match:
        return self.expect_t(pattern, timeout_s)[0]

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
        self.step_name: Optional[str] = None
        self.checks: list[tuple[str, bool]] = []
        self.step_idx = 0
        self.la_n = 0

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

    def sync(self, pre: Optional[Callable[[], None]] = None) -> None:
        self.wait_ack(True)
        if pre:
            pre()
        self.req(True)
        self.wait_ack(False)
        self.req(False)

    # -- S7, and S2 at every step boundary
    def step(self, name: str) -> None:
        m = self.con.expect(r"\[tim\] STEP (\S+) bus=(\S+) v=(\d+)", 120.0)
        got, bus, ver = m.group(1), m.group(2), int(m.group(3))
        exp_name, exp_bus = STEPS[self.step_idx]
        if (got, bus, ver) != (name, exp_bus, BENCH_VERSION) or exp_name != name:
            raise BenchAbort(f"S7: announced {got} bus={bus} v={ver}, host table expects "
                             f"{exp_name} bus={exp_bus} v={BENCH_VERSION}")
        self.step_idx += 1
        self.step_name = name
        self.ad2.set_safe_state()     # S2: Digital-IO input, Digital Out reset (S11 b)
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

    # -- LA
    def la_capture(self) -> tuple[list[int], float]:
        rate, n = self.ad2.la_arm(LA_RATE_HZ, self.la_n)
        samples = self.ad2.la_read(n, timeout_s=2.0)
        if samples is None:
            raise BenchAbort("logic analyzer capture did not complete")
        return samples, rate

    # ================================================================ steps
    def t0a(self) -> None:
        self.step("T0A")
        reads: list[int] = []
        for _ in range(2 + len(T0A_DIO) + 1):
            self.sync(pre=lambda: reads.append(self.ad2.digital_io_read() & T0A_LINES))
        up, dn = reads[0], reads[1]
        self.check("T0A loose lines", up == T0A_LINES and dn == 0,
                   f"13 lines with MCU pull-up 0x{up:04X} (0x{T0A_LINES:04X}), pull-down 0x{dn:04X} (0x0000); "
                   f"HOST_REQ/MCU_ACK exercised by every handshake")
        bad = [(f"DIO{d}", f"0x{v:04X}") for d, v in zip(T0A_DIO, reads[2:2 + len(T0A_DIO)]) if v != (1 << d)]
        self.check("T0A walking 1", not bad,
                   f"one line pulled up at a time, the others down: {len(T0A_DIO)} patterns, mismatches {bad}")
        ctrl = reads[-1] ^ T0A_LINES     # the host expects all up here; the firmware pulls DIO9 down
        self.check("T0A control", ctrl == (1 << T0A_CTRL_DIO),
                   f"firmware pulls DIO9 (PE5) down where the host expects all up: the check flags 0x{ctrl:04X} "
                   f"(0x{1 << T0A_CTRL_DIO:04X} expected)")

    def tref(self) -> dict:
        self.step("TREF")
        box: dict = {}

        def measure(key: str) -> None:
            samples, rate = self.la_capture()
            box[key] = (ad2mod.la_period_high(samples, 14, rate), rate, len(samples))
        self.sync(pre=lambda: measure("on"))
        self.sync(pre=lambda: measure("off"))
        (m, rate, n), (c, _, _) = box["on"], box["off"]
        e_ppm = (m["freq_hz"] - TREF_EXPECTED_HZ) / TREF_EXPECTED_HZ * 1e6 if m["freq_hz"] else float("nan")
        # The check itself is made at the end of the session (tref_check), when
        # the HAL_GetTick cross-check it is compared with exists.
        self.log(f"[ad2] TREF measured: DIO14 {m['freq_hz']:.3f} Hz over {m['periods']} periods "
                 f"({m['span_samples']} samples of {n} at {rate / 1e6:.0f} MS/s), e_ppm={e_ppm:+.1f}, resolution "
                 f"{m['resolution_ppm']:.1f} ppm, time high {m['high_s'] * 1e9:.1f} ns")
        self.check("TREF control", c["rises"] == 0 and c["falls"] == 0,
                   f"PA8 in AF1 (TIM1_CH1, TIM1 unclocked), MCO1 still HSE/8 in the RCC: {c['rises']} rising, "
                   f"{c['falls']} falling edges on DIO14 (0 expected)")
        return {"e_ppm": e_ppm, "res_ppm": m["resolution_ppm"], "periods": m["periods"]}

    def tref_check(self, tref: dict, xchk: Optional[tuple[float, float]]) -> None:
        """TREF criterion (owner's decision, 2026-10-01): |e_ppm| <= 2 % and the
        TREF e_ppm within 3 sqrt(u^2 + r^2) of the HAL_GetTick cross-check, both
        estimates of the same board clock against two independent references
        (the AD2 crystal, the host clock). u is the cross-check's uncertainty
        and r the one-sample resolution of this TREF capture."""
        e = tref["e_ppm"]
        if xchk is None:
            self.check("TREF MCO1 = HSE/8 on PA8", False, f"e_ppm={e:+.1f}; no HAL_GetTick cross-check (aborted)")
            return
        x, u = xchk
        r = tref["res_ppm"]
        bound = TREF_XCHK_K * math.sqrt(u * u + r * r)
        ok = (tref["periods"] > 0 and abs(e) <= TREF_MAX_PPM and abs(e - x) <= bound)
        self.check("TREF MCO1 = HSE/8 on PA8", ok,
                   f"e_ppm={e:+.1f} (|e| <= {TREF_MAX_PPM:.0f}), resolution r={r:.1f} ppm; cross-check "
                   f"{x:+.1f} +-{u:.1f} ppm, |e - cross-check| = {abs(e - x):.1f} (<= {TREF_XCHK_K:.0f} x "
                   f"sqrt(u^2 + r^2) = {bound:.1f})")

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
        self.con.expect(r"\[tim\] T0B timeout handled", 20.0)
        low = (self.ad2.digital_io_read() & DIO_ACK) == 0
        self.check("T0B(d) firmware timeout", low, "firmware reported the timeout and MCU_ACK is low")

    def _dout_run(self, idle: int, res: dict) -> None:
        """Inside one handshake: DIO1 at 1 MHz from Digital Out, then stopped."""
        # HOST_REQ stays enabled (S11 d covers the Digital Out lines only); the
        # guard refuses to start if the Digital-IO OE of DIO1 is not 0.
        f_cfg = self.ad2.dout_pulse(1, DOUT_DIVIDER, DOUT_LOW, DOUT_HIGH)
        time.sleep(0.001)                                  # S6
        self.ad2.set_dout(DIO_PE9, self.owner, idle, self.step_name)   # S11: the guarded path
        en, zet = self.ad2.dout_state()
        oe_run = self.ad2.get_oe()
        samples, rate = self.la_capture()
        self.ad2.set_dout(0, self.owner)                   # stop: Configure(0), DIO1 goes to its idle
        en_stop, zet_stop = self.ad2.dout_state()
        oe_stop = self.ad2.get_oe()
        m = ad2mod.la_period_high(samples, 1, rate)
        others = {b: ad2mod.la_edges(samples, b) for b in range(8) if b != 1}
        res.update(f_cfg=f_cfg, en=en, zet=zet, oe_run=oe_run, m=m, en_stop=en_stop, zet_stop=zet_stop,
                   oe_stop=oe_stop, other_edges=sum(len(r) + len(f) for r, f in others.values()))

    def t0b_dout(self) -> None:
        self.step("T0B_DOUT")
        self.buschk("T0B_DOUT before")
        zet, low = ad2mod.DWF_DOUT_IDLE_ZET, ad2mod.DWF_DOUT_IDLE_LOW
        for idle, label in ((zet, "Idle=Zet"), (low, "control Idle=Low")):
            res: dict = {}
            self.sync(pre=lambda r=res, i=idle: self._dout_run(i, r))
            m = res["m"]
            ran = (abs(m["freq_hz"] - res["f_cfg"]) <= res["f_cfg"] * DOUT_TOL and
                   abs(m["high_s"] - DOUT_HIGH / (DOUT_LOW + DOUT_HIGH) / res["f_cfg"]) <= DOUT_HIGH_TOL_S)
            want_zet = DIO_PE9 if idle == zet else 0
            self.check(f"T0B(f) {label}: Digital Out ran on DIO1 only",
                       ran and res["en"] == DIO_PE9 and (res["zet"] & DIO_PE9) == want_zet and
                       (res["oe_run"] & DIO_PE9) == 0 and res["oe_run"] == DIO_REQ and res["other_edges"] == 0,
                       f"DIO1 {m['freq_hz'] / 1e6:.4f} MHz (configured {res['f_cfg'] / 1e6:.4f}), high "
                       f"{m['high_s'] * 1e9:.1f} ns, {m['periods']} periods; enable mask 0x{res['en']:04X}, "
                       f"Zet mask 0x{res['zet']:04X}; Digital-IO OE while running 0x{res['oe_run']:04X} (HOST_REQ only, 0 on DIO1: S11 d); "
                       f"edges on the other bus lines {res['other_edges']}")
            self.log(f"[ad2] T0B(f) {label} stopped: enable 0x{res['en_stop']:04X} Zet 0x{res['zet_stop']:04X} "
                     f"OE 0x{res['oe_stop']:04X}")
            seen = []
            for high in (True, False):
                self.sync(pre=lambda h=high: seen.append(self.s5_released(h)))
            (ok_up, dio_up, _), (ok_dn, dio_dn, _) = seen
            if idle == zet:
                self.check("T0B(f) release with Idle=Zet", ok_up and ok_dn,
                           f"after Configure(0) the bus follows both MCU pulls: s5_released() up {ok_up} "
                           f"(DIO=0x{dio_up:02X}), down {ok_dn} (DIO=0x{dio_dn:02X})")
            else:
                self.check("T0B(f) control Idle=Low", (not ok_up) and dio_up == (DIO_BUS & ~DIO_PE9),
                           f"DIO1 held low by Digital Out against the MCU pull-up: s5_released() up {ok_up} "
                           f"(DIO=0x{dio_up:02X}, 0xFD expected), down {ok_dn} (DIO=0x{dio_dn:02X}); the "
                           f"check that guards every capture would abort here")

        def s2() -> None:
            self.ad2.set_safe_state()
            self.set_oe(DIO_REQ)
            en, zet_m = self.ad2.dout_state()
            self.check("T0B(f) S2 after the control", en == 0 and zet_m == 0xFFFF,
                       f"Digital Out enable mask 0x{en:04X} (0x0000), Zet mask 0x{zet_m:04X} (0xFFFF)")
        self.sync(pre=s2)
        self.buschk("T0B_DOUT after S2")
        self.check("T0B(f) bus released after S2", True, "the S5 check above passed (else BenchAbort)")

    def t0b_s12(self) -> None:
        self.step("T0B_S12")
        self.buschk("T0B_S12 before")
        self.buschk("T0B_S12 with PE9/PE11 in AF1")
        self.check("T0B(g) S5 with PE9/PE11 in AF1", True,
                   "the bus followed both MCU pulls with TIM1 CH1/CH2 inputs in AF (else BenchAbort)")
        self.buschk("T0B_S12 after")

    def run(self) -> dict:
        self.t0a()
        tref = self.tref()
        self.t0b()
        self.t0b_dout()
        self.t0b_s12()
        self.step("DONE")
        return tref


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", help="ST-LINK VCP (default: autodetect)")
    ap.add_argument("--out", type=pathlib.Path,
                    help="capture file (default: docs/tim_f0_bench_<rev>.txt, tim_dev_f0_bench_<rev>.txt with --allow-dirty)")
    ap.add_argument("--allow-dirty", action="store_true", help="accept a -dirty Rev (development runs)")
    ap.add_argument("--no-reset", action="store_true", help="do not reset the board; wait for the banner")
    ap.add_argument("--preset", default="selftest-tim", help="configure preset of the image (build id)")
    ap.add_argument("--checklist-confirmed", metavar="NAME",
                    help="the person at the bench confirms the wiring and flashing items of the checklist")
    args = ap.parse_args()

    lines: list[str] = []
    t0 = time.monotonic()
    lock = threading.Lock()

    def stamp(text: str) -> None:
        entry = f"[+{time.monotonic() - t0:8.3f}s] {text}"
        with lock:
            lines.append(entry)
        print(entry, flush=True)

    stamp(f"[host] argv: {shlex.join(sys.argv[1:])}")
    stamp(f"[host] options: allow_dirty={args.allow_dirty} preset={args.preset} no_reset={args.no_reset} "
          f"checklist_confirmed={args.checklist_confirmed}")
    stamp(f"[host] tim_hw_check.py, host checkout {git_rev()}, protocol v{BENCH_VERSION}, phase F0")
    procs_ok = checklist(stamp, args.checklist_confirmed)
    tree_record(stamp)
    build_id = host_build_id(args.preset)
    stamp(f"[host] build id of this checkout and build/{args.preset}: {build_id}")
    guard_a = t0b_guard_selftest(stamp)          # T0b(a): S3, 00_gpio's test as it is
    guard_e = t0b_dout_guard_selftest(stamp)     # T0b(e): S11
    stamp(f"[host] T0B(a) S3 guard on the mock: {'PASS' if guard_a else 'FAIL'}; "
          f"T0B(e) S11 guard on the mock: {'PASS' if guard_e else 'FAIL'}")

    port = args.port or autodetect()
    if not port:
        stamp("[host] ERROR: no ST-LINK VCP found")
        return 2

    verdict_fw = None
    session: Optional[Session] = None
    abort: Optional[str] = None
    revs: dict[str, str] = {}
    tref: dict = {}
    ticks: dict[str, tuple[int, float]] = {}
    if not (guard_a and guard_e):
        abort = "a host guard failed on its mock: the AD2 is not opened"
        stamp(f"[host] ABORT: {abort}")
    elif not procs_ok:
        abort = "WaveForms GUI or a debugger is running (or tasklist failed): the AD2 is not opened"
        stamp(f"[host] ABORT: {abort}")
    else:
        with AD2() as ad2, SerialPort(port, 115200) as ser:
            en, zet = ad2.dout_state()
            stamp(f"[ad2] opened in S2: OE=0x{ad2.get_oe():04X} DIO=0x{ad2.digital_io_read():04X} "
                  f"Digital Out enable=0x{en:04X} Zet=0x{zet:04X}")
            la_clk, la_max, do_clk = ad2.digital_in_clock_hz(), ad2.digital_in_buffer_max(), ad2.dout_clock_hz()
            stamp(f"[ad2] DigitalIn clock {la_clk / 1e6:.3f} MHz (la_arm assumes {ad2.DIGITAL_IN_CLOCK_HZ / 1e6:.0f}), "
                  f"buffer max {la_max} samples; DigitalOut clock {do_clk / 1e6:.3f} MHz")
            con = Console(ser, t0, stamp)
            try:
                if abs(la_clk - ad2.DIGITAL_IN_CLOCK_HZ) > 1.0:
                    raise BenchAbort(f"DigitalIn clock {la_clk} Hz, la_arm() assumes {ad2.DIGITAL_IN_CLOCK_HZ}")
                if not args.no_reset:
                    stamp("[host] reset through SWD (STM32_Programmer_CLI -c port=SWD mode=HOTPLUG -rst), AD2 in S2")
                    subprocess.run([cube_programmer(), "-c", "port=SWD", "mode=HOTPLUG", "-rst"],
                                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, check=True)
                revs["cm7"] = con.expect(r"\[tim\] CM7 Rev (\S+)", 20.0).group(1)
                ids = {"cm7": con.expect(r"\[tim\] CM7 Build id (\S+)", 10.0).group(1)}
                revs["cm4"] = con.expect(r"\[tim\] CM4 Rev (\S+)", 10.0).group(1)
                ids["cm4"] = con.expect(r"\[tim\] CM4 Build id (.+?)\s*$", 10.0).group(1)
                ver = int(con.expect(r"\[tim\] TIM_BENCH v(\d+) idle", 10.0).group(1))
                if ver != BENCH_VERSION:
                    raise BenchAbort(f"S8: firmware protocol v{ver}, host v{BENCH_VERSION}")
                m, t_tick = con.expect_t(r"\[tim\] TICK (\d+) \(HAL_GetTick at the banner\)", 10.0)
                ticks["banner"] = (int(m.group(1)), t_tick)
                head = git_rev()
                base = {c: (r[:-len("-dirty")] if r.endswith("-dirty") else r) for c, r in revs.items()}
                stamp(f"[host] R4-a: cm7 Rev={revs['cm7']} id={ids['cm7']}; cm4 Rev={revs['cm4']} id={ids['cm4']}; "
                      f"host HEAD={head} id={build_id}")
                if revs["cm7"] != revs["cm4"]:
                    raise BenchAbort(f"R4-a: cm7 and cm4 come from different trees "
                                     f"(Rev cm7={revs['cm7']} cm4={revs['cm4']})")
                if ids["cm7"] != ids["cm4"] or "unknown" in (ids["cm7"], ids["cm4"]):
                    raise BenchAbort(f"R4-a: cm7 and cm4 are not one build (id cm7={ids['cm7']} cm4={ids['cm4']})")
                for core in ("cm7", "cm4"):
                    if head == "unknown" or base[core] != head:
                        raise BenchAbort(f"R3-b: {core} was built at {base[core]} (Rev {revs[core]}), "
                                         f"this checkout is at {head}")
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
                session.la_n = la_max
                session.set_oe(DIO_REQ)          # HOST_REQ output, low
                session.req(False)
                tref = session.run()
                m, t_tick = con.expect_t(r"\[tim\] TICK (\d+) \(HAL_GetTick at the verdict\)", 120.0)
                ticks["verdict"] = (int(m.group(1)), t_tick)
                verdict_fw = con.expect(r"\[tim\] FW VERDICT=(\w+)", 30.0).group(1)
            except (BenchAbort, AD2SafetyViolation, ad2mod.AD2Error, subprocess.CalledProcessError) as e:
                abort = str(e)
                ad2.set_safe_state()
                stamp(f"[ad2] ABORT: {abort}; AD2 back in S2 (OE=0x{ad2.get_oe():04X})")
                try:
                    verdict_fw = con.expect(r"\[tim\] FW VERDICT=(\w+)", 60.0).group(1)
                except BenchAbort:
                    pass
            finally:
                ad2.set_safe_state()
                time.sleep(0.3)
                con.close()
            en, zet = ad2.dout_state()
            stamp(f"[ad2] closing in S2: OE=0x{ad2.get_oe():04X} Digital Out enable=0x{en:04X} Zet=0x{zet:04X}")

    xchk: Optional[tuple[float, float]] = None
    if "banner" in ticks and "verdict" in ticks:
        (k0, h0), (k1, h1) = ticks["banner"], ticks["verdict"]
        span = h1 - h0
        if span > 0:
            xchk = (((k1 - k0) / 1000.0 / span - 1.0) * 1e6, TREF_XCHK_JITTER_S / span * 1e6)
            stamp(f"[host] TREF cross-check: HAL_GetTick advanced {k1 - k0} ms in {span:.3f} s of host time "
                  f"(line arrival): board tick against host clock {xchk[0]:+.0f} ppm, +-{xchk[1]:.0f} ppm "
                  f"for {TREF_XCHK_JITTER_S * 1e3:.0f} ms of arrival jitter")
    if tref and session is not None:
        session.tref_check(tref, xchk)
    checks = session.checks if session else []
    stamp(f"[host] T0B(a) host S3 guard: {'PASS' if guard_a else 'FAIL'}")
    stamp(f"[host] T0B(e) host S11 guard: {'PASS' if guard_e else 'FAIL'}")
    for name, ok in checks:
        stamp(f"[host] CHECK {name}: {'PASS' if ok else 'FAIL'}")
    all_ok = (guard_a and guard_e and abort is None and verdict_fw == "PASS" and bool(checks) and
              all(ok for _, ok in checks))
    if not args.checklist_confirmed:
        stamp("[host] the wiring checklist was not confirmed by an operator: this capture is not evidence")
    stamp(f"[host] {sum(1 for _, ok in checks if ok)}/{len(checks)} host checks passed, FW VERDICT={verdict_fw}, "
          f"abort={abort}")
    stamp(f"HOST VERDICT={'PASS' if all_ok else 'FAIL'}")

    out = args.out
    if out is None:
        rev = revs.get("cm7", git_rev())
        prefix = "tim_dev_f0_bench" if args.allow_dirty else "tim_f0_bench"
        out = PROJECT / "docs" / f"{prefix}_{rev}.txt"
        n = 2
        while out.exists():
            out = PROJECT / "docs" / f"{prefix}_{rev}_{n}.txt"
            n += 1
    out.parent.mkdir(parents=True, exist_ok=True)
    with lock:
        out.write_text("\n".join(lines) + "\n", encoding="utf-8")
    print(f"capture: {out}")
    return 0 if all_ok else 1


if __name__ == "__main__":
    sys.exit(main())
