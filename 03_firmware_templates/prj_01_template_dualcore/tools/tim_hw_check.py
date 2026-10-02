#!/usr/bin/env python3
"""Host side of the 03_tim selftest on prj_01 (preset selftest-tim), phases F0 and F1.

Runs one complete bench session against CM7/App/Src/tim_selftest.c with the
Analog Discovery 2 and writes ONE capture file: every console line and every
host measurement, with timestamps, from "[host] argv:" to "HOST VERDICT=".
Plan and meaning of every test: 01_shared_drivers/03_tim/IMPLEMENTATION_PLAN.md;
wiring and rules S1-S10: 01_shared_drivers/00_gpio/README.md section 8.

    py tools/flash.py --preset selftest-tim
    py tools/tim_hw_check.py                  # resets the board through SWD and runs
    py tools/tim_hw_check.py --allow-dirty    # development only (capture tim_dev_...)

F0 tests: T0a (loose lines, walking 1, control), TREF (MCO1 = HSE/8 on PA8,
measured by the AD2 in 8 + 8 captures, |e| <= 2 % and within
3 sqrt(u^2 + s_ef^2/16 + sigma_ser^2) of HAL_GetTick
against the host clock; control PA8 in AF1), T0b(a) and (e) on mock DLLs
before the AD2 is opened,
then T0b(b)(c)(d)(f)(g) on the bench.

F1 tests (tim_bm): T3 recomputed here from the firmware's vectors (fractions),
and on the bench T6 (TIM1 and TIM15 PWM), T11 (one-pulse) and T13(a) (DMA to
CCR1) in the T6 steps, T8 (captures of Digital Out) and T13(b) (captures to
DMA) in the T8 step, and T9 (TIM17 on MCO1). Plan R3-2: every frequency the
AD2 measures is a ratio with MCO1 (DIO14) in the same acquisition. The other
F1 tests run in the firmware between the steps and print their verdicts.

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
  S8  no output is enabled before the banner `TIM_BENCH v2 idle` of the right
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
from fractions import Fraction
import os
import pathlib
import queue
import re
import shlex
import statistics
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

BENCH_VERSION = 3          # plan R3-3: TREF_A + TREF_B; v3: the F1 steps (R3-2)

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
    ("T0A", "NONE"), ("TREF_A", "NONE"), ("T0B", "NONE"), ("T0B_S5NEG", "MCU"), ("T0B_TIMEOUT", "NONE"),
    ("T0B_DOUT", "AD2"), ("T0B_S12", "AD2"),
    ("T6_TIM1", "MCU"), ("T6_TIM15", "NONE"), ("T8", "AD2"), ("T9", "NONE"),
    ("TREF_B", "NONE"), ("DONE", "NONE"),
]

HS_TIMEOUT_S = 10.0
LA_RATE_HZ = 100e6

TREF_EXPECTED_HZ = 1e6          # MCO1 = HSE_VALUE (8 MHz) / 8
# Plan R3-1: per group, TREF_CAPTURES LA captures of >= TREF_MIN_PERIODS periods of
# MCO1 each, started >= TREF_SPACING_S apart.
TREF_CAPTURES = 8
TREF_MIN_PERIODS = 40
TREF_SPACING_S = 0.100
# TREF criterion, plan revision 4, R4-1 (it replaces the criterion of R3-1): |e| <= 2 %,
# and the mean e of the 16 captures within 3 sqrt(u^2 + s_ef^2 / 16 + sigma_ser^2) of
# the cross-check x, u the standard error of x's slope and s_ef = max(s, r/sqrt(6))
# the measured scatter of the captures (tref_stats). The plan's first 500 ppm cannot
# hold on this board: its HSE, the ST-LINK MCO, is about +1.3 % and wanders by
# hundreds of ppm.
TREF_MAX_PPM = 20000.0
TREF_XCHK_K = 3.0
# sigma_ser: the run-to-run variation of e - x that u and s_ef do not explain,
# sqrt(mean(d^2) - mean(u^2 + s_ef^2 / 16)) with d = e - x, measured over the 17
# captures of plan revision 3: docs/tim_f0_bench_<rev>.txt for 2481396, 89f11b5,
# 781212b, 20e5b89, 105edfb, aef5596, 94abb65, cd0d613 and cd0d613_2 to _10.
# Source: plan R4-1 and AUDIT.md, audit 4 of F0, decision D6. It belongs to this
# bench (this board and this ST-LINK): measure it again, with a series of >= 10
# runs, if either changes.
TREF_SIGMA_SER_PPM = 145.0
# The cross-check (plan R3-1): CPU1 prints bursts of lines "TICK <label> <i> <HAL_GetTick>",
# TICK_BURST_LONG at the banner and at the verdict and TICK_BURST_STEP at the start
# of every step (tick_burst() in CM7/App/Src/tim_selftest.c). The host fits the
# burst means by least squares; u is the standard error of the slope (R2-10).
TICK_BURST_LONG = 16
TICK_BURST_STEP = 4
TICK_MIN_BURSTS = 10

DOUT_DIVIDER, DOUT_LOW, DOUT_HIGH = 1, 50, 50          # 100 MHz / 1 / (50 + 50) = 1 MHz
DOUT_TOL = 0.001                # +-0.1 % (plan, common tolerances; AD2 against itself here)
DOUT_HIGH_TOL_S = 15e-9         # +-15 ns


class BenchAbort(Exception):
    pass


# ---- F1: T3, the pure functions recomputed by the host (plan "Reglas de calculo") -------

ST_OK, ST_PARAM, ST_UNSUPPORTED, ST_RANGE = 0, 1, 2, 3      # TimBm_Status_t
T3_CT_RX = re.compile(r"\[tim\] T3 CT (\d+) (\d+) (\d+) (\d+) (\d+) (\d+) (\d+) 0x([0-9A-F]{16}) (-?\d+) (\d+)\s*$")
T3_DC_RX = re.compile(r"\[tim\] T3 DC (\d+) (\d+) (\d+) (\d+) (\d+) (\d+)\s*$")
T3_KC_RX = re.compile(r"\[tim\] T3 KC (\d+) (\d+) (\d+) (\d+) (\d+) (\d+)\s*$")
# RM0399 table 59, typed here: divisor of rcc_hclk1 per D2PPRE (index 0..7), per TIMPRE.
TABLE59_DIV = {0: [1, 1, 1, 1, 1, 2, 4, 8], 1: [1, 1, 1, 1, 1, 1, 2, 4]}


def t3_calc_time_base(clk: int, target: int, arr_max: int) -> tuple:
    """The plan's rule, with Fractions: (status, psc, arr, ticks, error_ppm)."""
    if target == 0 or target > clk // 2 or arr_max == 0:
        return (ST_PARAM, 0, 0, 0, 0)
    psc = max(math.ceil(Fraction(clk, target * (arr_max + 1))) - 1, 0)
    if psc > 65535:
        return (ST_RANGE, 0, 0, 0, 0)
    n = math.floor(Fraction(clk, (psc + 1) * target) + Fraction(1, 2))     # half rounds up
    n = min(n, arr_max + 1)
    if n < 2:
        return (ST_RANGE, 0, 0, 0, 0)
    ticks = (psc + 1) * n
    num = clk * 10**6 // ticks - target * 10**6                            # both >= 0 terms: floor = trunc
    return (ST_OK, psc, n - 1, ticks, int(Fraction(num, target)))          # int(): toward zero


def t3_duty(arr: int, bits: int, duty: int) -> tuple:
    if bits not in (16, 32) or duty > 10000:
        return (ST_PARAM, 0)
    cmax = (1 << bits) - 1
    if arr > cmax:
        return (ST_PARAM, 0)
    if duty == 10000 and arr + 1 > cmax:
        return (ST_RANGE, 0)
    return (ST_OK, math.floor(Fraction((arr + 1) * duty, 10000) + Fraction(1, 2)))


def t3_kernel(hclk: int, d2ppre: int, timpre: int) -> tuple:
    if d2ppre > 7 or timpre > 1:
        return (ST_PARAM, 0)
    return (ST_OK, hclk // TABLE59_DIV[timpre][d2ppre])


def t3_min_error_info(clk: int, target: int, arr_max: int, psc0: int, span: int = 4096) -> tuple[int, int]:
    """Information only (decision 3): the smallest |error| over psc0..psc0+span with the
    best n for each psc, against the plan's rule. Returns (best |ppm|, its psc)."""
    best, best_psc = None, psc0
    for psc in range(psc0, min(65535, psc0 + span) + 1):
        d = (psc + 1) * target
        for n in (clk // d, clk // d + 1):
            if 2 <= n <= arr_max + 1:
                e = abs(Fraction(clk, (psc + 1) * n) - target) / target * 10**6
                if best is None or e < best:
                    best, best_psc = e, psc
    return (int(best) if best is not None else -1, best_psc)


def t3_check(lines: list[str], log: Callable[[str], None]) -> list[tuple[str, bool, str]]:
    """Recomputes every T3 vector; returns (check name, ok, detail) entries."""
    ct, dc, kc = [], [], []
    for line in lines:
        for rx, dst in ((T3_CT_RX, ct), (T3_DC_RX, dc), (T3_KC_RX, kc)):
            m = rx.search(line)
            if m:
                dst.append(m.groups())
    bad_ct, bad_dc, bad_kc = [], [], []
    worse = []
    first_ok = None
    for g in ct:
        i, clk, target, arr_max, st, psc, arr, ticks_hex, err, _cyc = g
        clk, target, arr_max = int(clk), int(target), int(arr_max)
        got = (int(st), int(psc), int(arr), int(ticks_hex, 16), int(err))
        exp = t3_calc_time_base(clk, target, arr_max)
        if exp[0] != ST_OK:
            got = (got[0], 0, 0, 0, 0)
        if got != exp:
            bad_ct.append((int(i), got, exp))
        elif exp[0] == ST_OK:
            if first_ok is None:
                first_ok = (got, exp)
            if len(worse) < 12 and exp[4] != 0:
                bmin, bpsc = t3_min_error_info(clk, target, arr_max, exp[1], 256)
                if bmin < abs(exp[4]):
                    worse.append((int(i), exp[4], bmin, bpsc))
    for g in dc:
        i, arr, bits, duty, st, ccr = (int(x) for x in g)
        exp = t3_duty(arr, bits, duty)
        got = (st, ccr if st == ST_OK else 0)
        if got != exp:
            bad_dc.append((i, got, exp))
    for g in kc:
        i, hclk, d2ppre, timpre, st, hz = (int(x) for x in g)
        exp = t3_kernel(hclk, d2ppre, timpre)
        got = (st, hz if st == ST_OK else 0)
        if got != exp:
            bad_kc.append((i, got, exp))
    for name, bad in (("CalcTimeBase", bad_ct), ("DutyToCompare", bad_dc), ("KernelClockFromFields", bad_kc)):
        for b in bad[:5]:
            log(f"[host] T3 {name} vector {b[0]}: firmware {b[1]}, host {b[2]}")
    # The control: an expected value altered on purpose must be flagged by the same comparison.
    ctrl = False
    if first_ok is not None:
        got, exp = first_ok
        altered = (exp[0], exp[1] + 1) + exp[2:]
        ctrl = got != altered
    for i, e_rule, e_min, psc in worse:
        log(f"[host] T3 info (decision 3): vector {i}: rule {e_rule:+d} ppm, smallest over psc {psc}: {e_min} ppm")
    n_rows = sum(1 for g in kc if int(g[3]) <= 1 and int(g[2]) <= 7)
    return [
        ("T3 CalcTimeBase", len(ct) >= 200 and not bad_ct,
         f"{len(ct)} vectors (>= 200) recomputed with fractions, {len(bad_ct)} differ in status, psc, arr, ticks or error_ppm"),
        ("T3 DutyToCompare", len(dc) >= 50 and not bad_dc, f"{len(dc)} vectors (>= 50), {len(bad_dc)} differ"),
        ("T3 KernelClockFromFields", n_rows >= 16 and not bad_kc,
         f"{len(kc)} vectors, {n_rows} rows of table 59 (>= 16), {len(bad_kc)} differ"),
        ("T3 control", ctrl, f"the first OK vector with psc + 1 as the expected value is flagged: {ctrl}"),
    ]


# ---- F1 bench (protocol v3) ----------------------------------------------------------------

DIO_PE13 = 1 << 5
DIO_PE14 = 1 << 6
TIM1_CH_DIO = {1: 1, 2: 3, 3: 5, 4: 6}        # TIM1 CH1..CH4 on PE9/PE11/PE13/PE14 = DIO1/3/5/6
TIM15_CH_DIO = {1: 9, 2: 11}                  # TIM15 CH1/CH2 on PE5/PE6 = DIO9/DIO11
OC_PWM1, OC_PWM2 = 6, 7
# T6 points, the host's own copy (psc, arr, mode, ccr, active_low, center 1). P8 is TIM1 only.
T6_POINTS = [(0, 199, OC_PWM1, 100, 0, 0), (9, 199, OC_PWM1, 1, 0, 0), (9, 199, OC_PWM1, 0, 0, 0),
             (9, 199, OC_PWM1, 50, 0, 0), (9, 199, OC_PWM1, 100, 0, 0), (9, 199, OC_PWM1, 200, 0, 0),
             (9, 199, OC_PWM1, 50, 1, 0), (9, 199, OC_PWM2, 50, 0, 0), (0, 199, OC_PWM1, 50, 0, 1)]
T6_FREQ_TOL = 0.001          # +-0.1 % on the ratio with MCO1 (plan, common tolerances; R3-2)
T6_HIGH_TOL_S = 15e-9        # +-15 ns
TIMCLK_PER_MCO1 = 200        # f_timclk = 200 f_MCO1 in prj_01 (plan R3-2)
T11_CCR1, T11_ARR, T11_PULSES = 40, 139, 10
T13A_SEQ = [10 * (k + 1) for k in range(16)]
T8_POINTS = [(ti, f, d, 3 if f == 1000 else 0) for ti in (1, 2) for f in (1000, 10000, 100000, 1000000)
             for d in (25, 50, 75)]


def t6_expect(p: tuple) -> tuple[int, int]:
    """(period, high) in timer ticks of a T6 point on the OCx pin."""
    psc, arr, mode, ccr, low, center = p
    if center:
        per = 2 * arr * (psc + 1)
        high = min(2 * ccr, 2 * arr) * (psc + 1)
    else:
        per = (psc + 1) * (arr + 1)
        high = min(ccr, arr + 1) * (psc + 1)
    if mode == OC_PWM2:
        high = per - high
    if low:
        high = per - high
    return per, high


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


# ---- TREF statistics (plan R3-1) ----------------------------------------------------

def tref_stats(caps_a: list[dict], caps_b: list[dict], log: Callable[[str], None]) -> dict:
    """e_A, e_B, e (mean of the 16 captures), s (their standard deviation,
    n - 1) and s_ef = max(s, r / sqrt(6)), r the mean one-sample resolution
    of the captures: r / sqrt(6) is the standard deviation of the
    quantisation of the two extreme edges."""
    es = [c["e"] for c in caps_a + caps_b]
    n = len(es)
    e = sum(es) / n
    s = math.sqrt(sum((v - e) ** 2 for v in es) / (n - 1))
    r = sum(c["r"] for c in caps_a + caps_b) / n
    s_ef = max(s, r / math.sqrt(6))
    e_a = sum(c["e"] for c in caps_a) / len(caps_a)
    e_b = sum(c["e"] for c in caps_b) / len(caps_b)
    log(f"[host] TREF statistics: e_A={e_a:+.1f} ppm, e_B={e_b:+.1f} ppm, e={e:+.1f} ppm over {n} captures, "
        f"s={s:.1f} ppm, r={r:.1f} ppm, r/sqrt(6)={r / math.sqrt(6):.1f} ppm, s_ef=max(s, r/sqrt(6))={s_ef:.1f} ppm")
    return {"e_ppm": e, "e_a": e_a, "e_b": e_b, "s": s, "r": r, "s_ef": s_ef, "n": n}


# ---- TREF cross-check (plan R2-2, R2-10) ----------------------------------------------

TICK_RX = re.compile(r"\[tim\] TICK (\S+) (\d+) (\d+)\s*$")


def burst_fit(ticks: list[tuple[str, int, int, float]]) -> dict:
    """x and u from all TICK lines, plan R3-1. Lines are grouped into bursts by
    label, in order of arrival. For each burst j: kbar_j (board s), tbar_j (host
    s) and its own slope. Least squares of tbar = a + b kbar over the M bursts:
    x = 1/b - 1, residual scatter with M - 2 degrees of freedom, and u the
    standard error of b, se(b) / b^2. A latency that differs between bursts is
    in the residuals, so u carries it (audit O2)."""
    bursts: list[dict] = []
    for label, i, k, t in ticks:
        if not bursts or bursts[-1]["label"] != label:
            bursts.append({"label": label, "pts": []})
        bursts[-1]["pts"].append((k / 1000.0, t))
    for bj in bursts:
        ks = [k for k, _ in bj["pts"]]
        ts = [t for _, t in bj["pts"]]
        bj["n"] = len(ks)
        bj["k"], bj["t"] = sum(ks) / len(ks), sum(ts) / len(ts)
        sxx = sum((k - bj["k"]) ** 2 for k in ks)
        if len(ks) >= 2 and sxx > 0:
            bb = sum((k - bj["k"]) * (t - bj["t"]) for k, t in zip(ks, ts)) / sxx
            bj["x_ppm"] = (1.0 / bb - 1.0) * 1e6
        else:
            bj["x_ppm"] = float("nan")
    m = len(bursts)
    out = {"bursts": bursts, "m": m}
    if m < 3:
        return out
    km = sum(bj["k"] for bj in bursts) / m
    tm = sum(bj["t"] for bj in bursts) / m
    sxx = sum((bj["k"] - km) ** 2 for bj in bursts)
    b = sum((bj["k"] - km) * (bj["t"] - tm) for bj in bursts) / sxx
    a = tm - b * km
    for bj in bursts:
        bj["res_s"] = bj["t"] - (a + b * bj["k"])
    sd = math.sqrt(sum(bj["res_s"] ** 2 for bj in bursts) / (m - 2))
    se_b = sd / math.sqrt(sxx)
    out.update(sd_s=sd, x_ppm=(1.0 / b - 1.0) * 1e6, u_ppm=se_b / (b * b) * 1e6, span_s=bursts[-1]["k"] - bursts[0]["k"])
    return out


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
        self.ticks: list[tuple[str, int, int, float]] = []   # (label, i, HAL_GetTick ms, arrival s)
        self.t3: list[str] = []                               # T3 vectors, recomputed after the session
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
                mt = TICK_RX.search(line)
                if mt:
                    self.ticks.append((mt.group(1), int(mt.group(2)), int(mt.group(3)), t))
                if "[tim] T3 " in line:
                    self.t3.append(line)
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
        self.delay = 0.0        # T8: the reset delay of PWM input mode, ticks

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

    def tref(self, name: str, control: bool) -> list[dict]:
        """TREF_A (with the control of R2-1) or TREF_B (without it), plan R3-1/R3-3:
        TREF_CAPTURES LA captures of MCO1 inside one handshake (the MCU waits up
        to 10 s for HOST_REQ), each of >= TREF_MIN_PERIODS periods and started
        >= TREF_SPACING_S after the previous one. Returns e_i, r_i per capture."""
        self.step(name)
        box: dict = {}
        caps: list[dict] = []

        def measure_group() -> None:
            t_prev = None
            for i in range(TREF_CAPTURES):
                if t_prev is not None:
                    wait = TREF_SPACING_S - (time.monotonic() - t_prev)
                    if wait > 0:
                        time.sleep(wait)
                t_start = time.monotonic()
                samples, rate = self.la_capture()
                m = ad2mod.la_period_high(samples, 14, rate)
                e = (m["freq_hz"] - TREF_EXPECTED_HZ) / TREF_EXPECTED_HZ * 1e6 if m["freq_hz"] else float("nan")
                caps.append({"i": i, "e": e, "r": m["resolution_ppm"], "periods": m["periods"],
                             "span": m["span_samples"], "n": len(samples), "rate": rate,
                             "dt": 0.0 if t_prev is None else t_start - t_prev, "high_s": m["high_s"]})
                t_prev = t_start

        def measure(key: str) -> None:
            samples, rate = self.la_capture()
            box[key] = (ad2mod.la_period_high(samples, 14, rate), rate, len(samples))
        self.sync(pre=measure_group)
        # The criterion is checked at the end of the session (tref_check), when
        # the HAL_GetTick cross-check it is compared with exists.
        for c in caps:
            self.log(f"[ad2] {name} capture {c['i']}: e_{c['i']}={c['e']:+.1f} ppm, r_{c['i']}={c['r']:.1f} ppm "
                     f"({c['periods']} periods over {c['span']} of {c['n']} samples at {c['rate'] / 1e6:.0f} MS/s, "
                     f"started {c['dt'] * 1e3:.1f} ms after the previous one, time high {c['high_s'] * 1e9:.1f} ns)")
        ok = (len(caps) == TREF_CAPTURES and all(c["periods"] >= TREF_MIN_PERIODS for c in caps) and
              all(c["dt"] >= TREF_SPACING_S for c in caps[1:]))
        self.check(f"{name} captures", ok,
                   f"{len(caps)} captures (8), periods min {min(c['periods'] for c in caps)} (>= {TREF_MIN_PERIODS}), "
                   f"spacing min {min(c['dt'] for c in caps[1:]) * 1e3:.1f} ms (>= {TREF_SPACING_S * 1e3:.0f})")
        if control:
            self.sync(pre=lambda: measure("off"))
            c = box["off"][0]
            self.check(f"{name} control", c["rises"] == 0 and c["falls"] == 0,
                       f"PA8 in AF1 (TIM1_CH1, TIM1 unclocked), MCO1 still HSE/8 in the RCC: {c['rises']} rising, "
                       f"{c['falls']} falling edges on DIO14 (0 expected)")
        return caps

    def tref_check(self, tref: dict, xchk: Optional[tuple[float, float]]) -> None:
        """TREF criterion, plan R4-1: |e| <= TREF_MAX_PPM and
        |e - x| <= 3 sqrt(u^2 + s_ef^2 / n + sigma_ser^2), e the mean of the
        n = 16 captures of TREF_A and TREF_B, s_ef = max(s, r/sqrt(6)) their
        measured scatter, x the HAL_GetTick cross-check, u the standard error
        of its slope and sigma_ser = TREF_SIGMA_SER_PPM the measured run-to-run
        variation of this bench.
        Two estimates of the same board clock against two independent
        references (the AD2 crystal, the host clock), over the same interval."""
        e = tref["e_ppm"]
        if xchk is None:
            self.check("TREF MCO1 = HSE/8 on PA8", False, f"e={e:+.1f} ppm; no HAL_GetTick cross-check")
            return
        x, u = xchk
        s_ef, n = tref["s_ef"], tref["n"]
        ser = TREF_SIGMA_SER_PPM
        bound = TREF_XCHK_K * math.sqrt(u * u + s_ef * s_ef / n + ser * ser)
        ok = (n == 2 * TREF_CAPTURES and abs(e) <= TREF_MAX_PPM and abs(e - x) <= bound)
        self.check("TREF MCO1 = HSE/8 on PA8", ok,
                   f"e={e:+.1f} ppm over {n} captures (|e| <= {TREF_MAX_PPM:.0f}), s_ef={s_ef:.1f} ppm; cross-check "
                   f"x={x:+.1f} +-{u:.1f} ppm; sigma_ser={ser:.1f} ppm (plan R4-1, AUDIT.md audit 4, the 17 "
                   f"captures of revision 3); |e - x| = {abs(e - x):.1f} (<= {TREF_XCHK_K:.0f} x "
                   f"sqrt(u^2 + s_ef^2/{n} + sigma_ser^2) = {bound:.1f})")

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


    # ================================================================ F1 steps (protocol v3)
    def _mco(self, samples: list[int], rate: float) -> dict:
        m = ad2mod.la_period_high(samples, 14, rate)
        return m

    def _edges(self, samples: list[int], bit: int) -> int:
        r, f = ad2mod.la_edges(samples, bit)
        return len(r) + len(f)

    def _t6_point(self, tim: str, ch: int, k: int, bit: int, others: list[int], tag: str) -> None:
        """One T6 point: the announcement against the host table, then one LA capture
        with DIO14 (MCO1) and the channel's line in the same acquisition (R3-2)."""
        m = self.con.expect(rf"\[tim\] T6 {tim} CH{ch} P{k} psc=(\d+) arr=(\d+) mode=(\d+) ccr=(\d+) low=(\d+) "
                            rf"center=(\d+) st=(\d+)", 30.0)
        ann = tuple(int(x) for x in m.groups()[:6])
        st = int(m.group(7))
        p = T6_POINTS[k]
        box: dict = {}

        def measure() -> None:
            samples, rate = self.la_capture()
            box["mco"] = self._mco(samples, rate)
            box["out"] = ad2mod.la_period_high(samples, bit, rate)
            box["level"] = (samples[0] >> bit) & 1
            box["others"] = {b: self._edges(samples, b) for b in others}
        self.sync(pre=measure)
        mco, out = box["mco"], box["out"]
        per, high = t6_expect(p)
        f_mco = mco["freq_hz"]
        other_edges = sum(box["others"].values())
        if high == 0 or high == per:
            want = 1 if high == per else 0
            ok = (out["rises"] == 0 and out["falls"] == 0 and box["level"] == want and mco["periods"] >= 40)
            detail = (f"{'100' if want else '0'} %: {out['rises']} rising, {out['falls']} falling edges, level "
                      f"{box['level']} ({want})")
        else:
            ratio = out["freq_hz"] / f_mco if f_mco else 0.0
            exp_ratio = TIMCLK_PER_MCO1 / per
            r_err = (ratio - exp_ratio) / exp_ratio if exp_ratio else 1.0
            exp_high = high / (TIMCLK_PER_MCO1 * f_mco) if f_mco else 0.0
            ok = (abs(r_err) <= T6_FREQ_TOL and abs(out["high_s"] - exp_high) <= T6_HIGH_TOL_S and
                  out["periods"] >= 1 and mco["periods"] >= 40)
            detail = (f"f/f_MCO1 = {ratio:.6f} ({exp_ratio:.6f}, {r_err * 1e6:+.0f} ppm, +-1000), high "
                      f"{out['high_s'] * 1e9:.1f} ns ({exp_high * 1e9:.1f} +-15), {out['periods']} periods")
        ok = ok and ann == p and st == 0 and other_edges == 0
        self.check(f"T6 {tim} CH{ch} P{k}{tag}", ok,
                   f"psc={p[0]} arr={p[1]} mode={p[2]} ccr={p[3]} low={p[4]} center={p[5]} (announced {ann}, st {st}): "
                   f"{detail}; MCO1 {f_mco / 1e6:.4f} MHz over {mco['periods']} periods; edges on the other lines "
                   f"{other_edges}")

    def t6_tim1(self) -> None:
        self.step("T6_TIM1")
        self.buschk("T6_TIM1 before")
        tim1_bits = [TIM1_CH_DIO[c] for c in (1, 2, 3, 4)]
        quiet = [0, 2, 4, 7, 9, 11]
        for ch in (1, 2, 3, 4):
            for k in range(len(T6_POINTS)):
                bit = TIM1_CH_DIO[ch]
                self._t6_point("TIM1", ch, k, bit, [b for b in tim1_bits if b != bit] + quiet,
                               " center 1" if T6_POINTS[k][5] else "")
        # control: MOE = 0 -> no transition on any TIM1 line, all low (pull-down)
        self.con.expect(r"\[tim\] T6 TIM1 control", 30.0)
        box: dict = {}

        def ctrl() -> None:
            samples, rate = self.la_capture()
            box["edges"] = {b: self._edges(samples, b) for b in tim1_bits}
            box["levels"] = [(samples[0] >> b) & 1 for b in tim1_bits]
        self.sync(pre=ctrl)
        self.check("T6 TIM1 control MOE=0", sum(box["edges"].values()) == 0 and not any(box["levels"]),
                   f"CH1-4 enabled with MOE = 0: edges {box['edges']}, levels {box['levels']} (all 0)")
        self.t11()
        self.t13a()
        self.buschk("T6_TIM1 after (T14)")

    def t11(self) -> None:
        self.con.expect(r"\[tim\] T11 TIM1 one-pulse", 30.0)
        box: dict = {}

        def arm() -> None:
            box["arm"] = self.ad2.la_arm(LA_RATE_HZ, self.la_n, trigger_rise_mask=DIO_PE9, prefill=100)

        self.sync(pre=arm)
        rate, n = box["arm"]
        samples = self.ad2.la_read(n, timeout_s=2.0)
        if samples is None:
            self.check("T11 one-pulse", False, "the LA did not trigger on DIO1")
            return
        mco = self._mco(samples, rate)
        tick = 1.0 / (TIMCLK_PER_MCO1 * mco["freq_hz"]) if mco["freq_hz"] else 0.0
        r1, f1 = ad2mod.la_edges(samples, 1)
        r2, _ = ad2mod.la_edges(samples, 3)
        # trigger at sample n - prefill... the first pulse may be at the very start: include it
        lvl0 = (samples[0] >> 1) & 1
        if lvl0 and (not r1 or (f1 and f1[0] < r1[0])):
            r1 = [0] + r1
        pulses = []
        for r in r1:
            f = next((x for x in f1 if x > r), None)
            ref = max((x for x in r2 if x <= r), default=None)
            if f is not None and ref is not None:
                pulses.append(((r - ref) / rate, (f - r) / rate))
        d_exp = (T11_CCR1 - 1) * tick                      # CH2 rises one tick after the start
        w_exp = (T11_ARR - T11_CCR1 + 1) * tick
        tol = tick + 1.0 / rate
        bad = [(round(d * 1e9, 1), round(w * 1e9, 1)) for d, w in pulses
               if abs(d - d_exp) > tol or abs(w - w_exp) > tol]
        self.check("T11 one-pulse", len(r1) == T11_PULSES and len(pulses) == T11_PULSES and not bad and mco["periods"] >= 30,
                   f"{len(r1)} pulses on DIO1 ({T11_PULSES} triggers); D = CCR1 = {T11_CCR1} ticks: "
                   f"rise(CH1) - rise(CH2) {d_exp * 1e9:.1f} ns expected, W {w_exp * 1e9:.1f} ns expected, "
                   f"+-{tol * 1e9:.1f} ns (1 tick + 1 sample); out of tolerance {bad}; MCO1 {mco['freq_hz'] / 1e6:.4f} MHz")
        self.con.expect(r"\[tim\] T11 control", 30.0)
        box2: dict = {}

        def ctrl() -> None:
            samples2, rate2 = self.la_capture()
            box2["rises"] = len(ad2mod.la_edges(samples2, 1)[0])
        self.sync(pre=ctrl)
        self.check("T11 control OPM=0", box2["rises"] > 1, f"{box2['rises']} pulses in the window (> 1)")

    def t13a(self) -> None:
        self.con.expect(r"\[tim\] T13A TIM1 UP", 30.0)
        box: dict = {}

        def measure(key: str) -> None:
            samples, rate = self.la_capture()
            mco = self._mco(samples, rate)
            rises, falls = ad2mod.la_edges(samples, 1)
            highs = []
            for r, r_next in zip(rises, rises[1:]):
                f = next((x for x in falls if r < x < r_next), None)
                if f is not None:
                    highs.append((f - r) / rate)
            box[key] = (mco, highs)
        self.sync(pre=lambda: measure("dma"))
        mco, highs = box["dma"]
        tick = 1.0 / (TIMCLK_PER_MCO1 * mco["freq_hz"]) if mco["freq_hz"] else 0.0
        match = None
        for s in range(16):
            if highs and all(abs(h - T13A_SEQ[(k + s) % 16] * tick) <= T6_HIGH_TOL_S for k, h in enumerate(highs)):
                match = s
                break
        self.check("T13(a) DMA -> CCR1", match is not None and len(highs) >= 16,
                   f"{len(highs)} complete periods on DIO1; the 16 high times 10..160 ticks (tick {tick * 1e9:.3f} ns "
                   f"from MCO1) found in sequence at offset {match}, +-15 ns each; measured "
                   f"{[round(h / tick) if tick else 0 for h in highs[:20]]}")
        self.con.expect(r"\[tim\] T13A control", 30.0)
        self.sync(pre=lambda: measure("ctrl"))
        mco, highs = box["ctrl"]
        tick = 1.0 / (TIMCLK_PER_MCO1 * mco["freq_hz"]) if mco["freq_hz"] else 0.0
        same = bool(highs) and all(abs(h - 100 * tick) <= T6_HIGH_TOL_S for h in highs)
        self.check("T13(a) control UDE=0", same, f"{len(highs)} periods, all 100 ticks +-15 ns: {same}")

    def t6_tim15(self) -> None:
        self.step("T6_TIM15")
        quiet = [0, 1, 2, 3, 4, 5, 6, 7]
        for ch in (1, 2):
            for k in range(len(T6_POINTS) - 1):
                bit = TIM15_CH_DIO[ch]
                self._t6_point("TIM15", ch, k, bit, [TIM15_CH_DIO[3 - ch]] + quiet, "")
        self.con.expect(r"\[tim\] T6 TIM15 control", 30.0)
        box: dict = {}

        def ctrl() -> None:
            samples, rate = self.la_capture()
            box["edges"] = {b: self._edges(samples, b) for b in (9, 11)}
            box["levels"] = [(samples[0] >> b) & 1 for b in (9, 11)]
        self.sync(pre=ctrl)
        self.check("T6 TIM15 control CCxE=0", sum(box["edges"].values()) == 0 and not any(box["levels"]),
                   f"CH1/CH2 configured, CC1E = CC2E = 0: edges {box['edges']}, levels {box['levels']} (0)")

    # ---- T8 + T13(b)
    def _dout_start(self, ch: int, f: float, duty: int) -> tuple[float, float]:
        """Digital Out pulse on DIO1/DIO3 through the S11 guard. Returns (f_dout, high_s)."""
        clk = self.ad2.dout_clock_hz()
        for div in (1, 10, 100):
            total = int(round(clk / div / f))
            high = total * duty // 100
            try:
                f_cfg = self.ad2.dout_pulse(ch, div, total - high, high)
            except ad2mod.AD2Error:
                continue
            time.sleep(0.001)                                          # S6
            self.ad2.set_dout(1 << ch, self.owner, ad2mod.DWF_DOUT_IDLE_ZET, self.step_name)
            return f_cfg, high * div / clk
        raise BenchAbort(f"Digital Out cannot make {f} Hz")

    def _dout_stop(self) -> None:
        self.ad2.set_dout(0, self.owner)

    def _t8_on(self, ch: int, f: float, duty: int, box: dict, custom: Optional[list[int]] = None) -> None:
        if custom is not None:
            box["f_sample"] = self.ad2.dout_custom(ch, 1, custom)
            time.sleep(0.001)
            self.ad2.set_dout(1 << ch, self.owner, ad2mod.DWF_DOUT_IDLE_ZET, self.step_name)
        else:
            box["f"], box["high_s"] = self._dout_start(ch, f, duty)
        box["oe"] = self.ad2.get_oe()
        samples, rate = self.la_capture()
        box["mco"] = self._mco(samples, rate)

    def _t8_off(self, pattern: str, box: dict) -> None:
        """After the firmware's captures: its result line, a second MCO1 capture of the same
        Digital Out generation, then Digital Out off. The expected tick count uses the mean of
        the two MCO1 captures, which bracket the timer's measurement (the board clock drifts
        hundreds of ppm within seconds; one 41 us window also only resolves ~250 ppm)."""
        box["res"] = self.con.expect(pattern, 30.0)
        samples, rate = self.la_capture()
        mco2 = self._mco(samples, rate)
        f1, f2 = box["mco"]["freq_hz"], mco2["freq_hz"]
        box["mco"] = dict(box["mco"], freq_hz=(f1 + f2) / 2 if f1 and f2 else 0.0,
                          periods=min(box["mco"]["periods"], mco2["periods"]), f_before=f1, f_after=f2)
        self._dout_stop()

    @staticmethod
    def _pwm_values(m: re.Match) -> tuple[list[int], list[int]]:
        return [int(x) for x in m.group(2).split()], [int(x) for x in m.group(3).split()]

    def t8(self) -> None:
        self.step("T8")
        self.buschk("T8 before")
        self.buschk("T8 with PE9/PE11 in AF1 (S12)")
        # The reset delay d of PWM input mode, measured on one 1 MHz generation (owner's
        # decision of 2026-10-02): mean of the direct capture differences minus mean of the
        # PWM-input periods (the captures are integers either side of a fractional period, so
        # the means, not the medians, estimate it). Subtracted from every expected period and
        # high time.
        self.con.expect(r"\[tim\] T8 DELAY ti=1 f=1000000 duty=50 config 1", 30.0)
        box: dict = {}
        self.sync(pre=lambda: self._t8_on(1, 1000000, 50, box))

        def off_delay() -> None:
            box["pwm"] = self.con.expect(r"\[tim\] T8 RDELAY_PWM n=(\d+) per((?: \d+)*)", 30.0)
            box["dir"] = self.con.expect(r"\[tim\] T8 RDELAY_DIR n=(\d+) diffs((?: \d+)*)", 30.0)
            self._dout_stop()
        self.sync(pre=off_delay)
        pwm_per = [int(x) for x in box["pwm"].group(2).split()]
        direct = [int(x) for x in box["dir"].group(2).split()]
        ok_d = len(pwm_per) == 16 and len(direct) == 16
        d = (statistics.mean(direct) - statistics.mean(pwm_per)) if ok_d else 0.0
        self.delay = d
        self.check("T8 reset delay", ok_d and 0 <= d <= 4,
                   f"1 MHz on PE9, one Digital Out generation: PWM-input periods {pwm_per}, direct differences "
                   f"{direct}: d = mean(direct) - mean(PWM input) = {d:.2f} ticks (RM0399 40.4.3: reset delayed "
                   f"by the TI1 resynchronization; subtracted below)")
        for k, (ti, f, duty, psc) in enumerate(T8_POINTS):
            m = self.con.expect(rf"\[tim\] T8 P{k} ti=(\d+) f=(\d+) duty=(\d+) psc=(\d+) config (\d)", 30.0)
            ann = tuple(int(x) for x in m.groups()[:4])
            box: dict = {}
            ch = 1 if ti == 1 else 3
            self.sync(pre=lambda b=box, c=ch, fr=f, d=duty: self._t8_on(c, fr, d, b))
            self.sync(pre=lambda b=box, kk=k: self._t8_off(rf"\[tim\] T8 R{kk} n=(\d+) per((?: \d+)*) hi((?: \d+)*)", b))
            pers, his = self._pwm_values(box["res"])
            f_mco = box["mco"]["freq_hz"]
            per = (TIMCLK_PER_MCO1 * f_mco / box["f"] - self.delay) / (psc + 1)
            hi = (TIMCLK_PER_MCO1 * f_mco * box["high_s"] - self.delay) / (psc + 1)
            ptol = 1 + 0.001 * per
            htol = 2 + 0.001 * hi
            bad = [(a, b) for a, b in zip(pers, his) if abs(a - per) > ptol or abs(b - hi) > htol]
            ok = (ann == (ti, f, duty, psc) and int(m.group(5)) == 1 and len(pers) == 8 and not bad and
                  (box["oe"] & (1 << ch)) == 0 and box["mco"]["periods"] >= 40)
            self.check(f"T8 TI{ti} {f} Hz {duty} %", ok,
                       f"psc={psc}: periods {pers} ({per:.1f} +-{ptol:.1f} = (200 f_MCO1/f - d)/(PSC+1)), high {his} "
                       f"({hi:.1f} +-{htol:.1f}), out of tolerance {bad}; Digital Out {box['f']:.3f} Hz on DIO{ch}, "
                       f"OE 0x{box['oe']:04X}; MCO1 {f_mco / 1e6:.5f} MHz (mean of {box['mco']['f_before'] / 1e6:.5f} "
                       f"before and {box['mco']['f_after'] / 1e6:.5f} after the captures); d = {self.delay:.2f}")
        # control: inverted polarity
        self.con.expect(r"\[tim\] T8 INV", 30.0)
        box = {}
        self.sync(pre=lambda: self._t8_on(1, 100000, 25, box))
        self.sync(pre=lambda: self._t8_off(r"\[tim\] T8 RINV n=(\d+) per((?: \d+)*) hi((?: \d+)*)", box))
        pers, his = self._pwm_values(box["res"])
        f_mco = box["mco"]["freq_hz"]
        per = TIMCLK_PER_MCO1 * f_mco / box["f"]
        low = per - TIMCLK_PER_MCO1 * f_mco * box["high_s"] - self.delay
        ltol = 2 + 0.001 * low
        self.check("T8 control inverted polarity", len(his) == 8 and all(abs(h - low) <= ltol for h in his),
                   f"100 kHz 25 % captured on the falling edge: 'high' {his} ticks = the low time "
                   f"({low:.1f} +-{ltol:.1f}, the complementary duty, d = {self.delay:.1f})")
        # input prescaler 8
        self.con.expect(r"\[tim\] T8 PSC8", 30.0)
        box = {}
        self.sync(pre=lambda: self._t8_on(1, 1000000, 50, box))
        self.sync(pre=lambda: self._t8_off(r"\[tim\] T8 RPSC8 n=(\d+) diffs((?: \d+)*)", box))
        diffs = [int(x) for x in box["res"].group(2).split()]
        per8 = 8 * TIMCLK_PER_MCO1 * box["mco"]["freq_hz"] / box["f"]
        tol8 = 1 + 0.001 * per8
        self.check("T8 input prescaler 8", len(diffs) >= 8 and all(abs(d - per8) <= tol8 for d in diffs),
                   f"1 MHz, ICxPSC = /8: capture differences {diffs} ({per8:.1f} +-{tol8:.1f} = 8 periods)")
        # filter: one 10 ns glitch per period
        nmax = self.ad2.dout_data_max(1)
        plen = nmax
        while plen > 2048 and plen % 2 == 0:
            plen //= 2
        reps_n = nmax // plen
        period_bits = [1 if b < plen // 2 else 0 for b in range(plen)]
        period_bits[3 * plen // 4] = 1
        pattern = period_bits * reps_n
        for icf, label in ((0, "RGLITCH0"), (3, "RGLITCH3")):
            self.con.expect(rf"\[tim\] T8 GLITCH ti=1 icf={icf}", 30.0)
            box = {}
            self.sync(pre=lambda b=box: self._t8_on(1, 0.0, 0, b, custom=pattern))
            self.sync(pre=lambda b=box, lab=label: self._t8_off(rf"\[tim\] T8 {lab} n=(\d+) diffs((?: \d+)*)", b))
            diffs = [int(x) for x in box["res"].group(2).split()]
            ticks_per_bit = TIMCLK_PER_MCO1 * box["mco"]["freq_hz"] / box["f_sample"]
            full = plen * ticks_per_bit
            part = (plen // 4) * ticks_per_bit
            tol = 1 + 0.001 * full
            if icf == 0:
                seen_glitch = any(abs(d - part) <= tol for d in diffs) and any(abs(d - (full - part)) <= tol for d in diffs)
                self.check("T8 control without filter", len(diffs) >= 8 and seen_glitch,
                           f"{plen}-bit period at 100 MS/s ({nmax}-bit buffer, FDwfDigitalOutDataInfo) with a 10 ns "
                           f"glitch: differences {diffs[:12]} include {part:.1f} and {full - part:.1f} ticks (glitch captured)")
            else:
                self.check("T8 filter ICxF=0011", len(diffs) >= 8 and all(abs(d - full) <= tol for d in diffs),
                           f"the same pattern with fCK_INT N = 8 (40 ns): differences {diffs[:12]} all {full:.1f} "
                           f"+-{tol:.1f} ticks (glitch filtered)")
        # overcapture (the firmware judges it)
        self.con.expect(r"\[tim\] T8 OVR", 30.0)
        box = {}
        self.sync(pre=lambda: self._t8_on(1, 100000, 50, box))
        self.sync(pre=lambda: (self.con.expect(r"\[tim\] T8 overcapture", 30.0), self._dout_stop()))
        # T13(b): 64 captures by DMA, then the control CC1DE = 0
        for ctl in (False, True):
            self.con.expect(r"\[tim\] T13B" + (r" control" if ctl else r" TIM1 CC1"), 30.0)
            box = {}
            self.sync(pre=lambda b=box: self._t8_on(1, 1000000, 50, b))
            if not ctl:
                self.sync(pre=lambda b=box: self._t8_off(r"\[tim\] T13B R remaining=(\d+) diffs=(\d+) min=(\d+) max=(\d+)", b))
                rem, nd, dmin, dmax = (int(x) for x in box["res"].groups())
                per = TIMCLK_PER_MCO1 * box["mco"]["freq_hz"] / box["f"]
                self.check("T13(b) CCR1 -> DMA", rem == 0 and nd == 63 and abs(dmin - per) <= 1 and abs(dmax - per) <= 1,
                           f"64 captures by DMA (NDTR left {rem}): differences {dmin}..{dmax} ticks ({per:.2f} +-1 = "
                           f"200 f_MCO1 / 1 MHz)")
            else:
                self.sync(pre=lambda: (self.con.expect(r"\[tim\] T13\(b\) control", 30.0), self._dout_stop()))
        self.buschk("T8 after (T14)")

    def t9(self) -> None:
        self.step("T9")
        self.con.expect(r"\[tim\] T9 TIM17 TI1SEL", 30.0)
        box: dict = {}

        def measure() -> None:
            samples, rate = self.la_capture()
            box["mco"] = self._mco(samples, rate)
        self.sync(pre=measure)
        mco = box["mco"]
        e = (mco["freq_hz"] - TREF_EXPECTED_HZ) / TREF_EXPECTED_HZ * 1e6 if mco["freq_hz"] else float("nan")
        self.check("T9 MCO1 on PA8 during the capture", mco["periods"] >= 40 and abs(e) <= TREF_MAX_PPM,
                   f"the AD2 measures DIO14 while TIM17 captures TI1SEL = MCO1: {mco['freq_hz'] / 1e6:.4f} MHz "
                   f"({e:+.0f} ppm, |e| <= {TREF_MAX_PPM:.0f}, the TREF bound), {mco['periods']} periods; the ratio "
                   f"with MCO1 in the same acquisition is 1 by construction (plan T9; report 5.5)")

    def run(self) -> dict:
        self.t0a()
        caps_a = self.tref("TREF_A", control=True)
        self.t0b()
        self.t0b_dout()
        self.t0b_s12()
        self.t6_tim1()
        self.t6_tim15()
        self.t8()
        self.t9()
        caps_b = self.tref("TREF_B", control=False)
        self.step("DONE")
        return tref_stats(caps_a, caps_b, self.log)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", help="ST-LINK VCP (default: autodetect)")
    ap.add_argument("--out", type=pathlib.Path,
                    help="capture file (default: docs/tim_f1_bench_<rev>.txt, tim_dev_f1_bench_<rev>.txt with --allow-dirty)")
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
    stamp(f"[host] tim_hw_check.py, host checkout {git_rev()}, protocol v{BENCH_VERSION}, phases F0 and F1")
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
    con_ticks: list[tuple[str, int, int, float]] = []
    con_t3: list[str] = []
    session: Optional[Session] = None
    abort: Optional[str] = None
    revs: dict[str, str] = {}
    tref: dict = {}
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
                # The TICK and T3 lines of this boot start after its banner; anything before
                # is the tail of the boot the reset ended (seen in a development run).
                con.ticks.clear()
                con.t3.clear()
                ids = {"cm7": con.expect(r"\[tim\] CM7 Build id (\S+)", 10.0).group(1)}
                revs["cm4"] = con.expect(r"\[tim\] CM4 Rev (\S+)", 10.0).group(1)
                ids["cm4"] = con.expect(r"\[tim\] CM4 Build id (.+?)\s*$", 10.0).group(1)
                ver = int(con.expect(r"\[tim\] TIM_BENCH v(\d+) idle", 10.0).group(1))
                if ver != BENCH_VERSION:
                    raise BenchAbort(f"S8: firmware protocol v{ver}, host v{BENCH_VERSION}")
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
            con_ticks = list(con.ticks)
            con_t3 = list(con.t3)
            en, zet = ad2.dout_state()
            stamp(f"[ad2] closing in S2: OE=0x{ad2.get_oe():04X} Digital Out enable=0x{en:04X} Zet=0x{zet:04X}")

    xchk: Optional[tuple[float, float]] = None
    if con_ticks:
        fit = burst_fit(con_ticks)
        for j, bj in enumerate(fit["bursts"]):
            pts = " ".join(f"{k * 1e3:.0f}@{t:.4f}" for k, t in bj["pts"])
            stamp(f"[host] TREF cross-check burst {j} {bj['label']}: {bj['n']} lines, mean k={bj['k'] * 1e3:.1f} ms "
                  f"t={bj['t']:.5f} s, own slope {bj['x_ppm']:+.0f} ppm"
                  + (f", residual {bj['res_s'] * 1e3:+.3f} ms" if "res_s" in bj else "") + f"; {pts}")
        lens_ok = (fit["m"] >= 2 and fit["bursts"][0]["label"] == "banner" and fit["bursts"][-1]["label"] == "verdict"
                   and fit["bursts"][0]["n"] == TICK_BURST_LONG and fit["bursts"][-1]["n"] == TICK_BURST_LONG
                   and all(bj["n"] == TICK_BURST_STEP for bj in fit["bursts"][1:-1]))
        if fit["m"] >= TICK_MIN_BURSTS and lens_ok:
            stamp(f"[host] TREF cross-check: least squares of the {fit['m']} burst means over {fit['span_s']:.3f} s "
                  f"(M - 2 = {fit['m'] - 2} degrees of freedom): residual scatter {fit['sd_s'] * 1e3:.3f} ms; board "
                  f"tick against host clock x={fit['x_ppm']:+.1f} ppm, u={fit['u_ppm']:.1f} ppm (standard error "
                  f"of the slope)")
            xchk = (fit["x_ppm"], fit["u_ppm"])
        else:
            stamp(f"[host] TREF cross-check: {fit['m']} bursts (>= {TICK_MIN_BURSTS} needed), line counts as "
                  f"expected: {lens_ok}; no x")
    if tref and session is not None:
        session.tref_check(tref, xchk)
    if session is not None:
        for name, ok, detail in t3_check(con_t3, stamp):
            session.check(name, ok, detail)
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
        prefix = "tim_dev_f1_bench" if args.allow_dirty else "tim_f1_bench"
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
