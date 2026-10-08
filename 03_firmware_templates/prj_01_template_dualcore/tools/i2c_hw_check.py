#!/usr/bin/env python3
"""Host side of the 05_i2c selftest on prj_01 (preset selftest-i2c), phases F0, F1 and F2.

Runs one complete bench session against CM7/App/Src/i2c_selftest.c, the
Analog Discovery 2 and the NUCLEO-F334R8 target (tools/i2c_target_f334), and
writes ONE capture file: every console line of both boards and every host
measurement, with timestamps, from "[host] argv:" to "HOST VERDICT=".
Plan and meaning of every test: 01_shared_drivers/05_i2c/IMPLEMENTATION_PLAN.md;
AD2 wiring and rules S1-S10: 01_shared_drivers/00_gpio/README.md section 8.

    py tools/flash.py --sn 0016000C4142501220353451 --preset selftest-i2c   # host stopped
    py tools/i2c_hw_check.py --bench-state taps --checklist-confirmed NAME --checklist-date YYYY-MM-DD
    py tools/i2c_hw_check.py --allow-dirty ...      # development only (capture i2c_dev_...)

F0 tests: T0b(a) host guards on mocks before anything is opened (S3, the
HOST_REQ-only wrapper, S15 on the F334 pin report, S17 levels, S18 probe check,
S21 hold guard, the T0d identity check, the checklist arguments, and for F1 the
CalcTiming rules, the X/RPT pairing rules and the I2C decoder); T0d identity of
the two probes, CM7, CM4 and the F334; T0a (AD2 lines); T0c (bus levels, S17/S19,
and the F334's holds seen by CPU1, the F334 and, with taps, the scope); T0b
(claims on both cores, judged by the firmware; the host reads the bus with the
pins in AF); T0b(d) timeout.

F1 tests (i2c_bm): the firmware leads and stops at "AT <tag>" points where the
host acts (the target's commands and holds, the scope, the logic analyzer for
CLKREF). Each transfer prints an X line; the host pairs those with t=1, in
order, with the target's RPT lines and judges both against its own expectation
(x_expect(): the data regenerated from the seed or the target's known content,
the CRC-32, the counts). T3 is recomputed with the plan's rules written again
here (calc_timing()) and every OK TIMINGR checked on its fields against RM0399
(timingr_rm_problems()); T5 decodes the scope captures (ad2.i2c_decode_analog)
and judges the frames and the times; T8(b), T9 and T10(a) add the scope's view
of the NACK, the recovery pulses and the stretching; T12 compares the target's
totals; the holds of S21 are checked at every HOLD_END.

Electrical safety (no series resistors on the bench; plan S1-S21):
  S2  the AD2 opens and closes in S2 and goes back to it at every step
      boundary and on any exception; Digital Out is read back disabled at
      every step boundary (the AD2 drives nothing but HOST_REQ).
  S3  every OutputEnable change goes through set_oe(), which accepts HOST_REQ
      and nothing else, then through ad2.ad2_set_oe() (S3 of 00_gpio).
  S5, S6  no step of this bench drives the AD2 bus (PE8..PE15): only T0a's
      MCU pulls, which T0a checks line by line.
  S7  a step is acted on only if its announcement matches STEPS below.
  S8  no output is enabled before the banner `I2C_BENCH v1 idle` of the
      right image has been read.
  S15 the F334's pin report (banner, LEVELS, HOLD_END) must show PB8/PB9 in
      input or AF4 open-drain, never a pull (s15_report_ok()).
  S16 judged by the firmware (bench_claim on both cores, T0b).
  S17 bus levels before every step and after each hold: scope (with taps),
      the F334's LEVELS and CPU1's IDR in the STEP line (bus_rest()).
  S18 exactly the two expected ST-LINKs (SN and board) or the session does
      not start; every STM32_Programmer_CLI call carries sn=; both COM ports
      chosen by ST-LINK serial number (monitor.port_for_sn()).
  S17 (plan revision 2, R2-1): with taps, scope 2+ (SDA) >= 3.0 V and 1+/2+
      (SCL/SDA) = 0.956 +- 0.015; without taps, 1+ < 0.10 V and 2+ >= 3.0 V;
      every failure names its line (s17_scope_problems()).
  S19 --bench-state taps|notaps is required and checked: with taps the scope
      sees the bus (1+ above 0.10 V), without them PF14 reads < 0.10 V.
  S20 the F334 answers (banner) before the H755 is reset; procedure items in
      the checklist.
  S21 the host sends no HOLD outside 1..500 ms (f334_hold_guard()), and checks
      every release (HOLD_END, levels after).
The first acquisition of the scope after opening the AD2 is made and thrown
away before any level or time is measured (bench lesson, 2026-10-06).
Standard library only (ctypes); `py`, not `python`.
"""

from __future__ import annotations

import argparse
import ctypes
import datetime
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
from ctypes import wintypes
from collections import Counter
from typing import Callable, Optional

PROJECT = pathlib.Path(__file__).resolve().parents[1]
WORKSPACE = PROJECT.parents[1]
sys.path.insert(0, str(WORKSPACE / "tools"))      # ad2, monitor: the workspace modules, ahead of
if str(PROJECT / "tools") not in sys.path:        # the project shims (tools/monitor.py runs the
    sys.path.append(str(PROJECT / "tools"))       # monitor as __main__ when imported)
os.environ.setdefault("STM32_PROJECT_ROOT", str(PROJECT))

import ad2 as ad2mod                                   # noqa: E402
import monitor                                         # noqa: E402
from ad2 import AD2, AD2SafetyViolation, GPIO_BENCH_BUS_NONE  # noqa: E402
from monitor import SerialPort                         # noqa: E402
# One definition of the image-identity method and of the tree record (00_gpio
# README 9.4), and the 00_gpio T0b(a) mock test of the S3 guard as it is.
from gpio_hw_check import (cube_programmer, git_rev, host_build_id,  # noqa: E402
                           t0b_guard_selftest, tree_record)

BENCH_VERSION = 3          # I2C_BENCH_VERSION of Common/Inc/i2c_selftest.h
F334_PROTOCOL = 3          # TARGET_PROTOCOL of tools/i2c_target_f334/src/bench.h
F334_SYSCLK = 64000000     # the kernel its TIMINGR was computed for (audit F0, m7)

# ---- Probes (S18): the bench of 2026-10-06 ---------------------------------------------
H755_SN, H755_BOARD = "0016000C4142501220353451", "NUCLEO-H755ZI-Q"
F334_SN, F334_BOARD = "066BFF393731435243254117", "NUCLEO-F334R8"
F334_DIR = WORKSPACE / "tools" / "i2c_target_f334"
F334_PRESET = "f334"

# ---- AD2 wiring (00_gpio README section 8) ----------------------------------------------
DIO_REQ = 1 << 12       # HOST_REQ -> PE2
DIO_ACK = 1 << 13       # MCU_ACK  <- PE7
T0A_DIO = [0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 14]
T0A_LINES = sum(1 << d for d in T0A_DIO)              # 0x4FFF
T0A_CTRL_DIO = 9
I2C_OE_ALLOWED = DIO_REQ  # the only Digital-IO output this bench ever enables
SCOPE_SCL, SCOPE_SDA = 0, 1    # scope 1+ on PF14 (SCL through J2), 2+ on PF15 (SDA through J3)

# S7: the host's own copy of the firmware's step table (name, bus-pin owner), in order.
STEPS = [("T0A", "NONE"), ("T0C", "NONE"), ("T0B", "CM4"), ("T0B_CM7", "CM7"),
         ("T0B_TIMEOUT", "NONE"), ("F1_NOPIN", "NONE"), ("F1_CM4", "CM4"), ("F1_CM7", "CM7"),
         ("F1_RECOVER", "CM7"), ("F2_NOPIN", "NONE"), ("F2_CM4", "CM4"), ("F2_CM7", "CM7"), ("DONE", "NONE")]

# ---- Levels (S17, S19), plan revision 2, R2-1 (D9) -----------------------------------------
# With taps, SCL/SDA is the divider of the 1 kOhm pull-up with PF14's 22 kOhm to
# GND (22/23 = 0.9565): a ratio does not depend on the 3V3 rail nor on the AD2's
# gain, which moved both levels 1.7 % between two sessions (audit F0, O5). A
# 2.2 kOhm pull-up gives 0.909, 10 kOhm 0.688.
TAPS_RATIO = (0.956 - 0.015, 0.956 + 0.015)
SDA_MIN_V = 3.0                # 2+: the SDA node (taps) or PF15 alone (notaps), both states
NOTAPS_SCL_MAX_V = 0.10        # PF14 alone: its 22 kOhm to GND (S19)
VDD = 3.3
V_LOW = 0.3 * VDD              # I2C input thresholds (0.3/0.7 VDD)
V_HIGH = 0.7 * VDD

# ---- T0c --------------------------------------------------------------------------------
T0C_HOLD_MS = 50
F334_HOLD_US = (49000, 51000)  # the F334's own measure (TIM2 on its HSI, +-1 %)
SCOPE_HOLD_MS = (47.5, 52.5)   # +-5 %: the F334's HSI and the scope's clock
SCOPE_RATE, SCOPE_N = 50e3, 8192   # 164 ms per capture, 10 % before the trigger
HOLD_MAX_MS = 500              # S21

HS_TIMEOUT_S = 10.0
T0B_ACK_LOW_LIMIT_S = 15.0
T0B_TIMEOUT_MIN_S = 9.0
T0B_TIMEOUT_MAX_S = 11.0
T0B_LOW_READS = 20
STACK_FREE_MIN = 128


class BenchAbort(Exception):
    pass


# ---- T0b(a): host guards, each with a mock test ------------------------------------------

def i2c_set_oe(ad2, mask: int, owner: int) -> None:
    """The only path of this script to an OutputEnable change. A bit outside
    HOST_REQ puts the AD2 in S2 and raises before ad2.ad2_set_oe() is called;
    otherwise ad2_set_oe() applies its own guard (S3)."""
    if mask & ~I2C_OE_ALLOWED:
        ad2.set_safe_state()
        raise AD2SafetyViolation(f"I2C bench: OE mask 0x{mask:04X} has bits outside HOST_REQ "
                                 f"(0x{mask & ~I2C_OE_ALLOWED:04X}); the AD2 is in S2")
    ad2.set_oe(mask, owner)


class _MockAd2:
    def __init__(self) -> None:
        self.calls: list[tuple] = []

    def set_safe_state(self) -> None:
        self.calls.append(("S2",))

    def set_oe(self, mask: int, owner: int) -> None:
        self.calls.append(("set_oe", mask, owner))


def t0b_oe_selftest(log: Callable[[str], None]) -> bool:
    forbidden = [(1 << 8, "DIO8 (S3 would allow it)"), (1 << 10, "DIO10 (S3 would allow it)"),
                 (0x00FF, "the AD2 bus"), (1 << 9, "DIO9"), (1 << 14, "DIO14 (PA8)"), (1 << 15, "DIO15"),
                 (DIO_REQ | (1 << 8), "HOST_REQ with DIO8")]
    ok = True
    for mask, why in forbidden:
        m = _MockAd2()
        try:
            i2c_set_oe(m, mask, GPIO_BENCH_BUS_NONE)
            refused = False
        except AD2SafetyViolation:
            refused = True
        good = refused and m.calls == [("S2",)]
        ok = ok and good
        log(f"[host] T0B(a) i2c_set_oe(0x{mask:04X}) [{why}]: "
            f"{'refused, S2, nothing passed to ad2_set_oe' if good else 'NOT REFUSED CLEANLY'}")
    for mask in (0, DIO_REQ):
        m = _MockAd2()
        i2c_set_oe(m, mask, GPIO_BENCH_BUS_NONE)
        good = m.calls == [("set_oe", mask, GPIO_BENCH_BUS_NONE)]
        ok = ok and good
        log(f"[host] T0B(a) control i2c_set_oe(0x{mask:04X}): {'passed to ad2_set_oe' if good else 'BLOCKED'}")
    return ok


PIN_RX = re.compile(r"\b(PB8|PB9) MODER=(\d+) OTYPER=(\d+) PUPDR=(\d+) AFR=(\d+)\b")


def s15_report_ok(line: str) -> tuple[bool, str]:
    """S15 on the F334's own report of PB8/PB9: each pin exactly once (audit
    F0, m14: a report with a pin twice is refused, not judged on its last
    copy), input without pull or AF4 open-drain without pull, and the
    firmware's own "s15=ok"."""
    found: dict[str, list[tuple[int, ...]]] = {}
    for m in PIN_RX.finditer(line):
        found.setdefault(m.group(1), []).append(tuple(int(x) for x in m.groups()[1:]))
    bad = []
    for name in ("PB8", "PB9"):
        if name not in found:
            bad.append(f"{name} missing")
            continue
        if len(found[name]) != 1:
            bad.append(f"{name} reported {len(found[name])} times")
            continue
        moder, otyper, pupdr, afr = found[name][0]
        if pupdr != 0:
            bad.append(f"{name} pull {pupdr}")
        elif moder == 0:
            continue
        elif not (moder == 2 and otyper == 1 and afr == 4):
            bad.append(f"{name} MODER={moder} OTYPER={otyper} AFR={afr}")
    if not re.search(r"(?<!\S)s15=ok(?!\S)", line):
        bad.append("no s15=ok")
    return not bad, ", ".join(bad) if bad else "ok"


def t0b_s15_selftest(log: Callable[[str], None]) -> bool:
    ok_af = "PB8 MODER=2 OTYPER=1 PUPDR=0 AFR=4 PB9 MODER=2 OTYPER=1 PUPDR=0 AFR=4 s15=ok"
    cases = [
        (ok_af, True, "both AF4 open-drain"),
        ("PB8 MODER=0 OTYPER=1 PUPDR=0 AFR=0 PB9 MODER=0 OTYPER=1 PUPDR=0 AFR=0 s15=ok", True, "both input"),
        (ok_af.replace("PB8 MODER=2 OTYPER=1", "PB8 MODER=2 OTYPER=0"), False, "PB8 AF push-pull"),
        (ok_af.replace("PB9 MODER=2 OTYPER=1 PUPDR=0", "PB9 MODER=2 OTYPER=1 PUPDR=1"), False, "PB9 pull-up"),
        (ok_af.replace("PB8 MODER=2", "PB8 MODER=1"), False, "PB8 GPIO output"),
        (ok_af.replace("PB9 MODER=2 OTYPER=1 PUPDR=0 AFR=4", "PB9 MODER=2 OTYPER=1 PUPDR=0 AFR=1"), False, "PB9 AF1"),
        (ok_af.replace(" s15=ok", " s15=VIOLATED"), False, "firmware says violated"),
        ("PB8 MODER=2 OTYPER=1 PUPDR=0 AFR=4 s15=ok", False, "PB9 missing"),
        ("PB8 MODER=2 OTYPER=0 PUPDR=0 AFR=4 " + ok_af, False, "PB8 twice, a bad copy first (m14)"),
        (ok_af.replace("PB9 MODER", "XPB9 MODER"), False, "PB9 only inside another word (m14)"),
        (ok_af.replace(" s15=ok", " s15=okx"), False, "s15=okx is not s15=ok (O13)"),
        (ok_af.replace(" s15=ok", " xs15=ok"), False, "xs15=ok is not s15=ok (O13)"),
    ]
    ok = True
    for text, want, why in cases:
        got, detail = s15_report_ok(text)
        good = got == want
        ok = ok and good
        log(f"[host] T0B(a) S15 check of an F334 pin report [{why}]: {'accepted' if got else 'refused'} "
            f"({detail}) -> {'as expected' if good else 'WRONG'}")
    return ok


def f334_hold_guard(line: str, ms: int, edges: int) -> Optional[str]:
    """S21 on the host: the reason a HOLD must not be sent, or None."""
    if line not in ("SDA", "SCL"):
        return f"line {line!r}"
    if not (1 <= ms <= HOLD_MAX_MS):
        return f"{ms} ms outside 1..{HOLD_MAX_MS} (S21)"
    if edges and line != "SDA":
        return "EDGES only with SDA"
    if not (0 <= edges <= 255):
        return f"edges {edges}"
    return None


def t0b_s21_selftest(log: Callable[[str], None]) -> bool:
    ok = True
    for args, want in ((("SDA", 0, 0), False), (("SDA", 501, 0), False), (("SCL", 50, 5), False),
                       (("SDA", 1000, 5), False), (("XX", 50, 0), False), (("SDA", 50, 0), True),
                       (("SCL", 500, 0), True), (("SDA", 500, 5), True)):
        written: list[str] = []
        why = f334_hold_guard(*args)
        if why is None:
            written.append(f"HOLD {args[0]} {args[1]}")
        good = (why is None) == want and (bool(written) == want)
        ok = ok and good
        log(f"[host] T0B(a) S21 hold guard HOLD {args[0]} {args[1]} EDGES {args[2]}: "
            f"{'sent' if why is None else 'refused (' + why + ')'} -> {'as expected' if good else 'WRONG'}")
    return ok


def s17_scope_problems(state: str, v_scl: float, v_sda: float) -> tuple[list[str], list[str]]:
    """S17 (plan R2-1) and S19 on the two scope readings at rest: (every
    problem, each naming the line it is about; the subset that is S19, the
    declared bench state). Pure."""
    bad: list[str] = []
    s19: list[str] = []
    if state == "taps":
        if v_scl <= NOTAPS_SCL_MAX_V:
            s19.append(f"scope 1+ (SCL node through J2) reads {v_scl:.3f} V <= {NOTAPS_SCL_MAX_V:.2f} V: "
                       f"J2 not in place, or the probe not on PF14")
        if v_sda < SDA_MIN_V:
            bad.append(f"scope 2+ (SDA node through J3) reads {v_sda:.3f} V < {SDA_MIN_V:.1f} V")
        else:
            r = v_scl / v_sda
            if not (TAPS_RATIO[0] <= r <= TAPS_RATIO[1]):
                bad.append(f"scope 1+ (SCL) / 2+ (SDA) = {v_scl:.3f} / {v_sda:.3f} V = {r:.4f}, outside "
                           f"{TAPS_RATIO[0]:.3f}..{TAPS_RATIO[1]:.3f} (1 kOhm pull-up against PF14's 22 kOhm)")
    else:
        if v_scl >= NOTAPS_SCL_MAX_V:
            s19.append(f"scope 1+ (PF14 alone, J2 removed) reads {v_scl:.3f} V >= {NOTAPS_SCL_MAX_V:.2f} V: "
                       f"J2 in place, or the probe on the bus side")
        if v_sda < SDA_MIN_V:
            bad.append(f"scope 2+ (PF15 alone, J3 removed) reads {v_sda:.3f} V < {SDA_MIN_V:.1f} V")
    return bad + s19, s19


def t0b_s17_selftest(log: Callable[[str], None]) -> bool:
    """S17/S19 thresholds on readings typed by hand (plan R2-1); each refusal
    must name its line."""
    cases = [("taps", 3.157, 3.301, True, "", "the F0 delivery levels"),
             ("taps", 3.102, 3.245, True, "", "the auditor's session (both 1.7 % lower, same ratio)"),
             ("taps", 3.000, 3.300, False, "1+ (SCL) / 2+ (SDA)", "ratio 0.909: a 2.2 kOhm pull-up"),
             ("taps", 2.800, 2.950, False, "2+ (SDA node", "SDA below 3.0 V"),
             ("taps", 0.036, 3.300, False, "1+ (SCL node", "J2 missing (S19)"),
             ("notaps", 0.057, 3.235, True, "", "the F0 notaps levels"),
             ("notaps", 3.303, 3.300, False, "1+ (PF14 alone", "probe on the bus side (S19)"),
             ("notaps", 0.057, 2.900, False, "2+ (PF15 alone", "PF15 below 3.0 V")]
    ok = True
    for state, v1, v2, want, line, why in cases:
        bad, _ = s17_scope_problems(state, v1, v2)
        good = (not bad) == want and (want or any(line in b for b in bad))
        ok = ok and good
        log(f"[host] T0B(a) S17 levels {state} 1+ {v1:.3f} V 2+ {v2:.3f} V [{why}]: "
            f"{'accepted' if not bad else 'refused: ' + '; '.join(bad)} -> {'as expected' if good else 'WRONG'}")
    return ok


def list_probes(cli: str) -> tuple[list[dict], str]:
    """STM32_Programmer_CLI -l st-link-only, parsed: [{sn, fw, board}]. Lists only."""
    out = subprocess.run([cli, "-l", "st-link-only"], capture_output=True, text=True, timeout=60).stdout
    return parse_probes(out), out


def parse_probes(text: str) -> list[dict]:
    probes: list[dict] = []
    cur: Optional[dict] = None
    for line in text.splitlines():
        if re.match(r"\s*ST-Link Probe \d+", line):
            cur = {"sn": "", "fw": "", "board": ""}
            probes.append(cur)
            continue
        if cur is None:
            continue
        m = re.match(r"\s*ST-LINK SN\s*:\s*(\S+)", line)
        if m:
            cur["sn"] = m.group(1).upper()
        m = re.match(r"\s*ST-LINK FW\s*:\s*(\S+)", line)
        if m:
            cur["fw"] = m.group(1)
        m = re.match(r"\s*Board Name\s*:\s*(.+?)\s*$", line)
        if m:
            cur["board"] = m.group(1)
    return probes


def s18_probes_ok(probes: list[dict]) -> tuple[bool, str]:
    """Exactly the two expected ST-LINKs, each on its board."""
    want = {H755_SN: H755_BOARD, F334_SN: F334_BOARD}
    got = {p["sn"]: p["board"] for p in probes}
    if len(probes) != 2:
        return False, f"{len(probes)} ST-LINKs ({sorted(got)}), expected exactly 2"
    if got != want:
        return False, f"probes {got}, expected {want}"
    return True, "the two expected ST-LINKs, nothing else"


def t0b_s18_selftest(log: Callable[[str], None]) -> bool:
    h = {"sn": H755_SN, "fw": "V3J17M11", "board": H755_BOARD}
    f = {"sn": F334_SN, "fw": "V2J38M27", "board": F334_BOARD}
    cases = [([h, f], True, "the two expected probes"),
             ([h, f, {"sn": "0670FF000000000000000000", "fw": "V2J40M27", "board": "NUCLEO-L476RG"}], False, "a third"),
             ([h], False, "the F334's missing"),
             ([h, dict(f, sn="066BFF393731435243254118")], False, "another F334 SN"),
             ([dict(h, board=F334_BOARD), dict(f, board=H755_BOARD)], False, "boards swapped")]
    ok = True
    for probes, want, why in cases:
        got, detail = s18_probes_ok(probes)
        good = got == want
        ok = ok and good
        log(f"[host] T0B(a) S18 probe check [{why}]: {'accepted' if got else 'refused'} ({detail}) -> "
            f"{'as expected' if good else 'WRONG'}")
    sample = ("ST-Link Probe 0 :\n   ST-LINK SN  : 0016000C4142501220353451\n   ST-LINK FW  : V3J17M11\n"
              "   Board Name  : NUCLEO-H755ZI-Q\nST-Link Probe 1 :\n   ST-LINK SN  : 066BFF393731435243254117\n"
              "   ST-LINK FW  : V2J38M27\n   Board Name  : NUCLEO-F334R8\n")
    parsed = parse_probes(sample)
    good = parsed == [h, f]
    ok = ok and good
    log(f"[host] T0B(a) S18 parser on a CubeProgrammer listing: {parsed} -> {'as expected' if good else 'WRONG'}")
    return ok


def identity_problems(idt: dict, allow_dirty: bool) -> list[str]:
    """T0d (S18, A21): everything that does not match. `idt` holds head,
    h755_id (host), f334_id (host), cm7/cm4 Rev and id, f334 Rev, id and
    protocol, the probe list and the two ports."""
    bad = []
    head = idt["head"]
    for who in ("cm7", "cm4", "f334"):
        rev = idt[f"{who}_rev"]
        base = rev[:-len("-dirty")] if rev.endswith("-dirty") else rev
        if head == "unknown" or base != head:
            bad.append(f"{who} Rev {rev} is not this checkout ({head})")
        if rev.endswith("-dirty") and not allow_dirty:
            bad.append(f"{who} Rev {rev} is dirty")
    if idt["cm7_id"] != idt["cm4_id"] or "unknown" in (idt["cm7_id"], idt["cm4_id"]):
        bad.append(f"cm7 id {idt['cm7_id']} != cm4 id {idt['cm4_id']}")
    if idt["cm7_id"] != idt["h755_id"] and not allow_dirty:
        bad.append(f"cm7 id {idt['cm7_id']} != host id {idt['h755_id']}")
    if idt["f334_id"] != idt["f334_host_id"] and not allow_dirty:
        bad.append(f"f334 id {idt['f334_id']} != host id {idt['f334_host_id']}")
    if idt["f334_proto"] != F334_PROTOCOL:
        bad.append(f"F334 protocol v{idt['f334_proto']}, host v{F334_PROTOCOL}")
    ok18, why18 = s18_probes_ok(idt["probes"])
    if not ok18:
        bad.append(why18)
    if not idt["h755_port"] or not idt["f334_port"] or idt["h755_port"] == idt["f334_port"]:
        bad.append(f"ports H755 {idt['h755_port']} F334 {idt['f334_port']}")
    return bad


def t0b_t0d_selftest(log: Callable[[str], None]) -> bool:
    """T0d's control on a mock identity: a different SN, Rev or id must abort."""
    good_id = {"head": "1234567", "h755_id": "aaaa", "f334_host_id": "bbbb", "cm7_rev": "1234567",
               "cm4_rev": "1234567", "cm7_id": "aaaa", "cm4_id": "aaaa", "f334_rev": "1234567", "f334_id": "bbbb",
               "f334_proto": F334_PROTOCOL, "h755_port": "COM4", "f334_port": "COM7",
               "probes": [{"sn": H755_SN, "fw": "", "board": H755_BOARD}, {"sn": F334_SN, "fw": "", "board": F334_BOARD}]}
    cases = [({}, True, "all match"),
             ({"f334_rev": "7654321"}, False, "F334 Rev from another commit"),
             ({"cm4_rev": "1234567-dirty"}, False, "CM4 dirty"),
             ({"cm7_id": "cccc"}, False, "CM7 id differs"),
             ({"f334_id": "cccc"}, False, "F334 id differs"),
             ({"probes": [{"sn": H755_SN, "fw": "", "board": H755_BOARD},
                          {"sn": "066BFF393731435243254110", "fw": "", "board": F334_BOARD}]}, False, "F334 SN differs"),
             ({"f334_port": "COM4"}, False, "one port for both")]
    ok = True
    for change, want, why in cases:
        idt = dict(good_id, **change)
        bad = identity_problems(idt, allow_dirty=False)
        good = (not bad) == want
        ok = ok and good
        log(f"[host] T0B(a) T0d identity check [{why}]: {'accepted' if not bad else 'aborts: ' + '; '.join(bad)} -> "
            f"{'as expected' if good else 'WRONG'}")
    return ok


# ---- Procedure checklist (plan "El bus I2C", S20; 00_gpio README section 8) --------------

FORBIDDEN_PROCESSES = ("WaveForms.exe", "ST-LINK_gdbserver.exe", "openocd.exe", "STM32CubeProgrammer.exe")


def running_forbidden() -> Optional[list[str]]:
    try:
        out = subprocess.run(["tasklist", "/FO", "CSV", "/NH"], capture_output=True, text=True, check=True).stdout
    except (OSError, subprocess.CalledProcessError):
        return None
    names = {line.split('","')[0].strip('"').lower() for line in out.splitlines() if line.strip()}
    return [p for p in FORBIDDEN_PROCESSES if p.lower() in names]


def checklist(stamp: Callable[[str], None], operator: Optional[str], date: Optional[str], state: str) -> bool:
    mark = "x" if operator else " "
    who = f"confirmed by {operator} on {date} (--checklist-confirmed)" if operator else "NOT confirmed by an operator"
    taps = ("J2 (SCL node - PF14, CN9-19) and J3 (SDA node - PF15, CN9-21) in place" if state == "taps" else
            "J2 and J3 removed: PF14/PF15 not on the bus")
    items = [
        "GND first: W3 H755 CN8-11 <-> F334 CN5-7; no supply rail between the boards (S20)",
        "I2C bus: SCL node H755 PB8 (CN7-2) <-> F334 PB8 (CN5-10) W1; SDA node H755 PB9 (CN7-4) <-> F334 PB9 "
        "(CN5-9) W2; 1.0 kOhm from each node to H755 3V3 (CN8-7); no series resistors",
        f"bench state '{state}': {taps}",
        "F334 checks: D15/D14 = PB8/PB9 on CN5-10/CN5-9, GND on CN5-7, BOOT0 its own pin, no pull-up or solder "
        "bridge on PB8/PB9; H755 SB39/SB62 open",
        "AD2 lines per 00_gpio README section 8 (DIO0..14), the AD2 on USB while the Nucleo is powered",
        "both Nucleo boards on USB of this PC, the F334 powered before the session (S20)",
    ]
    for item in items:
        stamp(f"[host] checklist [{mark}] {item}: {who}")
    stamp("[host] checklist (not an operator item) images: the F334 with tools/i2c_target_f334 and the H755 with "
          "selftest-i2c, each through tools/flash.py --sn with this script stopped; checked by T0d below (Rev, id)")
    running = running_forbidden()
    if running is None:
        stamp("[host] checklist [ ] WaveForms GUI closed, no debugger attached: tasklist failed, not checked")
        return False
    stamp(f"[host] checklist [{'x' if not running else ' '}] WaveForms GUI closed, no debugger attached "
          f"(tasklist: {', '.join(running) if running else 'none of ' + ', '.join(FORBIDDEN_PROCESSES)})")
    return not running


# ---- Serial ports --------------------------------------------------------------------------

_k32 = ctypes.WinDLL("kernel32", use_last_error=True)
_k32.CreateFileW.restype = wintypes.HANDLE
_k32.CreateFileW.argtypes = [wintypes.LPCWSTR, wintypes.DWORD, wintypes.DWORD, wintypes.LPVOID,
                             wintypes.DWORD, wintypes.DWORD, wintypes.HANDLE]
_k32.WriteFile.argtypes = [wintypes.HANDLE, wintypes.LPCVOID, wintypes.DWORD,
                           ctypes.POINTER(wintypes.DWORD), wintypes.LPVOID]
GENERIC_WRITE = 0x40000000


class RwSerialPort(SerialPort):
    """monitor.SerialPort opened for reading AND writing: the F334 takes commands."""

    def __init__(self, port: str, baud: int) -> None:   # noqa: D107 (same configuration as the base class)
        self.name = port
        self.handle = _k32.CreateFileW(f"\\\\.\\{port}", monitor.GENERIC_READ | GENERIC_WRITE, 0, None,
                                       monitor.OPEN_EXISTING, 0, None)
        if self.handle == monitor.INVALID_HANDLE_VALUE:
            raise OSError(f"cannot open {port} for writing (Win32 error {ctypes.get_last_error()})")
        try:
            self._configure(baud)
        except Exception:
            self.close()
            raise

    def write(self, data: bytes) -> None:
        done = wintypes.DWORD(0)
        if not _k32.WriteFile(self.handle, data, len(data), ctypes.byref(done), None) or done.value != len(data):
            raise OSError(f"write to {self.name} failed (Win32 error {ctypes.get_last_error()})")


class LineReader:
    """Reads a serial port in a thread; every line is timestamped, logged with
    a prefix and kept."""

    def __init__(self, port: SerialPort, prefix: str, sink: Callable[[str], None]) -> None:
        self._port = port
        self._prefix = prefix
        self._sink = sink
        self._q: "queue.Queue[str]" = queue.Queue()
        self._stop = threading.Event()
        self.lines: list[str] = []
        self._lock = threading.Lock()
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
                with self._lock:
                    self.lines.append(line)
                self._q.put(line)
                # A line this thread cannot log or judge must not end the thread: in
                # the second development run the byte a reset leaves on the line
                # (U+FFFD once decoded) made print() raise UnicodeEncodeError on a
                # cp1252 console, and every later line of the session was lost.
                try:
                    self._sink(f"{self._prefix}{line}")
                    self.on_line(line)
                except Exception as e:          # noqa: BLE001
                    self._sink(f"{self._prefix}(reader: {type(e).__name__} on this line, going on)")

    def on_line(self, line: str) -> None:
        pass

    def expect(self, pattern: str, timeout_s: float, stop_on_abort: bool = False) -> re.Match:
        rx = re.compile(pattern)
        end = time.monotonic() + timeout_s
        while True:
            left = end - time.monotonic()
            if left <= 0:
                raise BenchAbort(f"{self._prefix.strip()} timeout waiting for /{pattern}/")
            self.check_abort(stop_on_abort and self._q.empty())
            try:
                line = self._q.get(timeout=min(left, 0.2))
            except queue.Empty:
                continue
            m = rx.search(line)
            if m:
                return m

    def check_abort(self, firmware_abort_ends: bool = False) -> None:
        pass

    def drain(self) -> None:
        while True:
            try:
                self._q.get_nowait()
            except queue.Empty:
                return

    def close(self) -> None:
        self._stop.set()
        self._th.join(timeout=1.0)


class H755Console(LineReader):
    def __init__(self, port: SerialPort, sink: Callable[[str], None]) -> None:
        self.fw_abort: Optional[str] = None
        super().__init__(port, "", sink)

    def on_line(self, line: str) -> None:
        if "[i2c] ABORT" in line:
            self.fw_abort = line

    def check_abort(self, firmware_abort_ends: bool = False) -> None:
        # Audit F0, m16 (as spi_hw_check.py after 06_spi B1): an expect() that
        # asks for it ends at once when the firmware has printed ABORT and every
        # line before it has been looked at, instead of waiting out its timeout.
        if firmware_abort_ends and self.fw_abort:
            raise BenchAbort(f"firmware aborted: {self.fw_abort}")

    def since(self, mark: int) -> list[str]:
        """Lines from `mark` on, log_long() continuations ("[i2c] | ...") joined."""
        with self._lock:
            raw = self.lines[mark:]
        out: list[str] = []
        for line in raw:
            i = line.find("[i2c] | ")
            if i >= 0 and out:
                out[-1] += " " + line[i + len("[i2c] | "):]
            else:
                out.append(line)
        return out

    def mark(self) -> int:
        with self._lock:
            return len(self.lines)


class F334(LineReader):
    """The F334 target's console (tools/i2c_target_f334/README.md)."""

    def __init__(self, port: RwSerialPort, sink: Callable[[str], None]) -> None:
        self.unexpected: list[str] = []
        self._banner_wanted = False
        self.wport = port
        super().__init__(port, "[f334] ", sink)

    def on_line(self, line: str) -> None:
        if line.startswith("I2C_TARGET v") and not self._banner_wanted:
            self.unexpected.append(line)          # a reset nobody asked for (IWDG, power)
        if "ABORT" in line:
            self.unexpected.append(line)

    def check_abort(self, firmware_abort_ends: bool = False) -> None:
        if self.unexpected:
            raise BenchAbort(f"F334: {self.unexpected[0]}")

    def send(self, text: str) -> None:
        self.wport.write((text + "\n").encode("ascii"))

    def command(self, text: str, reply: str, timeout_s: float = 2.0) -> re.Match:
        self.drain()
        self.send(text)
        return self.expect(reply, timeout_s)

    def settle(self, quiet_s: float = 0.5, limit_s: float = 5.0) -> None:
        """Waits until the port has been silent for `quiet_s` (a board just
        flashed prints its boot banner), then drops what came."""
        end = time.monotonic() + limit_s
        n = len(self.lines)
        quiet_from = time.monotonic()
        while time.monotonic() < end:
            time.sleep(0.05)
            if len(self.lines) != n:
                n = len(self.lines)
                quiet_from = time.monotonic()
            elif time.monotonic() - quiet_from >= quiet_s:
                break
        self.drain()

    def banner(self) -> dict:
        """The banner, asked for with ID (up to 3 times: the first request
        after a flash can meet the boot banner on the line)."""
        self._banner_wanted = True
        try:
            for attempt in range(3):
                self.settle()
                self.send("")                       # ends any partial line the target holds
                self.settle(0.2, 1.0)
                self.send("ID")
                try:
                    m1 = self.expect(r"^I2C_TARGET v(\d+) Rev (\S+) id (\S+)", 3.0)
                    pins = self.expect(r"^I2C_TARGET pins (.*)$", 2.0).group(1)
                    m3 = self.expect(r"^I2C_TARGET addr=0x([0-9A-F]+) sysclk=(\d+) .*idcode=0x([0-9A-F]+) "
                                     r"uid=([0-9A-F]+)", 2.0)
                    ready = self.expect(r"^I2C_TARGET (ready|aborted.*)$", 2.0).group(1)
                    break
                except BenchAbort:
                    if attempt == 2:
                        raise
            self.settle(0.2, 1.0)
        finally:
            self._banner_wanted = False
        return {"proto": int(m1.group(1)), "rev": m1.group(2), "id": m1.group(3), "pins": pins,
                "addr": int(m3.group(1), 16), "sysclk": int(m3.group(2)), "idcode": int(m3.group(3), 16),
                "uid": m3.group(4), "ready": ready}

    def levels(self) -> tuple[dict, str]:
        m = self.command("LEVELS", r"^LVL scl=(\d) sda=(\d) busy=(\d) pe=(\d) fmp=(\d)(.*)$")
        return ({"scl": int(m.group(1)), "sda": int(m.group(2)), "busy": int(m.group(3)), "pe": int(m.group(4))},
                m.group(0))

    def hold(self, line: str, ms: int, edges: int = 0) -> str:
        why = f334_hold_guard(line, ms, edges)      # S21, before anything is written
        if why is not None:
            raise BenchAbort(f"S21: HOLD refused by the host: {why}")
        text = f"HOLD {line} {ms}" + (f" EDGES {edges}" if edges else "")
        return self.command(text, r"^(OK|ERR) HOLD.*$").group(0)


# ==== Phase F1: the host's own oracles (pure functions, each with its mock test) ============

ST = {0: "OK", 1: "ERR_PARAM", 2: "ERR_UNSUPPORTED", 3: "ERR_RANGE", 4: "ERR_BUSY", 5: "ERR_VERIFY",
      6: "ERR_TIMEOUT", 7: "ERR_NACK", 8: "ERR_ARLO", 9: "ERR_BUS", 10: "PENDING", 11: "ERR_HAL"}
ST_OK, ST_PARAM, ST_RANGE, ST_TIMEOUT, ST_NACK, ST_ARLO, ST_BUS, ST_PENDING = 0, 1, 3, 6, 7, 8, 9, 10
TARGET, NO_TARGET = 0x42, 0x43

# Plan "Reglas de cálculo": tables 405/407 (ns) and the kernel minimum of ES0445 2.19.1, per mode.
MODES = [  # scl max, tLOW, tHIGH, tSU;DAT, tHD;DAT, tVD;DAT, t_r max, t_f max, kernel min, tHD;STA, tSU;STA, tSU;STO, tBUF
    (100000, 4700, 4000, 250, 0, 3450, 1000, 300, 4000000, 4000, 4700, 4000, 4700),
    (400000, 1300, 600, 100, 0, 900, 300, 300, 10000000, 600, 600, 600, 1300),
    (1000000, 500, 260, 50, 0, 450, 120, 120, 20000000, 260, 260, 260, 500)]
T_AF = (50, 80)          # DS12923 table 105, ns
G = 10 ** 9


def _cdiv(a: int, b: int) -> int:
    return -((-a) // b)


def calc_timing(k: int, f: int, tr: int, tf: int, af: bool, dnf: int) -> tuple[int, int, int, int]:
    """The plan's calculation rules, written again from the plan (not from
    i2c_bm.c), with N rounded up as approved on 2026-10-07: (status, TIMINGR,
    mode, scl_hz_model). Status 1 ERR_PARAM, 3 ERR_RANGE."""
    if k == 0 or f == 0 or f > 1000000 or dnf > 15:
        return ST_PARAM, 0, 0, 0
    m = 0 if f <= 100000 else (1 if f <= 400000 else 2)
    _, tl, th, tsu, thd, tvd, trm, tfm, kmin = MODES[m][:9]
    if tr > trm or tf > tfm or k < kmin:
        return ST_RANGE, 0, 0, 0
    afmin, afmax = T_AF if af else (0, 0)
    sync = _cdiv((tr + tf + 2 * afmin) * k, G) + 2 * (dnf + 3)
    for p in range(16):
        d = G * (p + 1)
        scldel = max(0, _cdiv((tr + tsu) * k, d) - 1)
        if scldel > 15:
            continue
        smin = max(0, _cdiv((tf + thd - afmin) * k - (dnf + 3) * G, d))
        smax = ((tvd - tr - afmax) * k - (dnf + 4) * G) // d
        if smin > smax or smin > 15:
            continue
        n = _cdiv(_cdiv(k, f) - sync, p + 1)
        lmin, hmin = _cdiv(tl * k, d), _cdiv(th * k, d)
        lo = max(lmin, _cdiv(n * tl, tl + th))
        hi = max(hmin, n - lo)
        if lo > 256 or hi > 256:
            continue
        if (lo * (p + 1) - dnf - 4) * G <= afmax * k:
            continue
        if hi * (p + 1) <= 1:
            continue
        t = (p << 28) | (scldel << 20) | (smin << 16) | ((hi - 1) << 8) | (lo - 1)
        return ST_OK, t, m, k // (sync + (lo + hi) * (p + 1))
    return ST_RANGE, 0, 0, 0


def timingr_rm_problems(k: int, f: int, tr: int, tf: int, af: bool, dnf: int, t: int, model: int) -> list[str]:
    """What an OK TIMINGR must meet, checked on its fields against RM0399 itself
    (not against the rules): 50:342 (SCLDEL), 50:319/331 (SDADEL window), the
    counters at least tLOW/tHIGH of table 407, 50:162-163, the kernel of 2.19.1,
    reserved bits 0, and the model frequency not above the one asked."""
    m = 0 if f <= 100000 else (1 if f <= 400000 else 2)
    _, tl, th, tsu, thd, tvd, trm, tfm, kmin = MODES[m][:9]
    p, scldel, sdadel = (t >> 28) & 0xF, (t >> 20) & 0xF, (t >> 16) & 0xF
    scll, sclh = (t & 0xFF) + 1, ((t >> 8) & 0xFF) + 1
    afmin, afmax = T_AF if af else (0, 0)
    bad = []
    if t & 0x0F000000:
        bad.append("reserved bits 27:24 set")
    if (scldel + 1) * (p + 1) * G < (tr + tsu) * k:
        bad.append("SCLDEL below t_r + tSU;DAT (50:342)")
    if sdadel * (p + 1) * G < (tf + thd - afmin) * k - (dnf + 3) * G:
        bad.append("SDADEL below its minimum (50:319)")
    if sdadel * (p + 1) * G > (tvd - tr - afmax) * k - (dnf + 4) * G:
        bad.append("SDADEL above its maximum (50:331)")
    if scll * (p + 1) * G < tl * k:
        bad.append("SCLL counter below tLOW (table 407)")
    if sclh * (p + 1) * G < th * k:
        bad.append("SCLH counter below tHIGH (table 407)")
    if 4 * G >= (scll * (p + 1) - dnf) * G - afmax * k:
        bad.append("t_kernel >= (tLOW - t_filters)/4 (50:162)")
    if sclh * (p + 1) <= 1:
        bad.append("t_kernel >= tHIGH (50:163)")
    if k < kmin:
        bad.append("kernel below ES0445 2.19.1")
    if model > f:
        bad.append(f"model {model} above {f}")
    return bad


T3_RX = re.compile(r"\[i2c\] T3 (\d+) (\d+) (\d+) (\d+) (\d+) (\d) (\d+) (\d+) ([0-9A-F]+) (\d+) (\d+)")


def t3_judge(lines: list[str], alter: bool = False) -> dict:
    """T3: every vector the firmware printed against calc_timing() and every OK
    TIMINGR against RM0399. With alter, the host's expected TIMINGR of every OK
    vector is changed (SCLL + 1) before the same comparison: the control of the
    plan, which must then find every OK vector different (audit F1, m23)."""
    r = {"n": 0, "bad": 0, "rm_bad": 0, "ok_n": 0, "first": "", "first_rm": ""}
    for t in lines:
        m = T3_RX.search(t)
        if not m:
            continue
        i, k, f, tr, tf, af, dnf, st = (int(m.group(j)) for j in range(1, 9))
        tim, mode, model = int(m.group(9), 16), int(m.group(10)), int(m.group(11))
        r["n"] += 1
        want = list(calc_timing(k, f, tr, tf, bool(af), dnf))
        if want[0] != ST_OK:
            want[1:] = [0, 0, 0]
        elif alter:
            want[1] = (want[1] & ~0xFF) | (((want[1] & 0xFF) + 1) & 0xFF)
        if (st, tim, mode, model) != tuple(want):
            r["bad"] += 1
            r["first"] = r["first"] or f"vector {i}: firmware {st} 0x{tim:08X} {model}, host {want[0]} 0x{want[1]:08X} {want[3]}"
        if st == ST_OK:
            r["ok_n"] += 1
            probs = timingr_rm_problems(k, f, tr, tf, bool(af), dnf, tim, model)
            if probs:
                r["rm_bad"] += 1
                r["first_rm"] = r["first_rm"] or f"vector {i}: {'; '.join(probs)}"
    return r


def t0b_calc_selftest(log: Callable[[str], None]) -> bool:
    """T3's two oracles on vectors typed here: the bench's own case (with the
    plan's floor of N the model came out at 100 100 Hz; rounded up 99 700), the
    RM check of a good TIMINGR, and the two controls of the plan: an expected
    value altered (SCLL + 1) is told apart, and SDADEL outside its window is
    refused by the RM check."""
    ok = True
    st, t, m, model = calc_timing(100000000, 100000, 250, 20, True, 0)
    good = st == ST_OK and model == 99700 and model <= 100000 and not timingr_rm_problems(
        100000000, 100000, 250, 20, True, 0, t, model)
    ok = ok and good
    log(f"[host] T0B(a) T3 calc: 100 MHz, 100 kHz, t_r 250, t_f 20, filter: status {st} TIMINGR 0x{t:08X} model "
        f"{model} (99700, RM check clean) -> {'as expected' if good else 'WRONG'}")
    # The control through t3_judge itself (m23): T3 lines made from calc_timing are
    # clean; the same lines with the expected TIMINGR altered (SCLL + 1) are told
    # apart in every OK vector.
    vecs = [(100000000, 100000, 250, 20, 1, 0), (100000000, 400000, 300, 10, 1, 0), (64000000, 1000000, 40, 10, 1, 0),
            (3999999, 100000, 100, 10, 1, 0)]
    lines = []
    for i, (k_, f_, tr_, tf_, af_, dnf_) in enumerate(vecs):
        st_, t_, m_, mo_ = calc_timing(k_, f_, tr_, tf_, bool(af_), dnf_)
        lines.append(f"[i2c] T3 {i} {k_} {f_} {tr_} {tf_} {af_} {dnf_} {st_} {t_:08X} {m_} {mo_}")
    plain, ctl = t3_judge(lines), t3_judge(lines, alter=True)
    good = plain["bad"] == 0 and plain["ok_n"] == 3 and ctl["bad"] == ctl["ok_n"] == 3
    ok = ok and good
    log(f"[host] T0B(a) T3 control through t3_judge: {len(lines)} vectors, {plain['bad']} differ as printed; with the "
        f"expected TIMINGR altered (SCLL + 1) {ctl['bad']} of {ctl['ok_n']} OK vectors differ -> "
        f"{'as expected' if good else 'WRONG'}")
    # With t_f = 300 ns the SDADEL window starts above 0 (50:319): the vector's own
    # TIMINGR passes, the same with SDADEL = 0 must not.
    st2, t2, _, model2 = calc_timing(100000000, 100000, 1000, 300, True, 0)
    clean = st2 == ST_OK and not timingr_rm_problems(100000000, 100000, 1000, 300, True, 0, t2, model2)
    bad_sda = t2 & ~(0xF << 16)
    probs = timingr_rm_problems(100000000, 100000, 1000, 300, True, 0, bad_sda, model2)
    good = clean and ((t2 >> 16) & 0xF) > 0 and any("SDADEL" in x for x in probs)
    ok = ok and good
    log(f"[host] T0B(a) T3 control: 100 MHz, 100 kHz, t_r 1000, t_f 300: TIMINGR 0x{t2:08X} (SDADEL "
        f"{(t2 >> 16) & 0xF}) passes {clean}; with SDADEL 0 (below 50:319) the RM check "
        f"{'refuses: ' + '; '.join(probs) if probs else 'accepts'} -> {'as expected' if good else 'WRONG'}")
    for k, f, want in ((3999999, 100000, ST_RANGE), (100000000, 0, ST_PARAM), (100000000, 1000001, ST_PARAM)):
        got = calc_timing(k, f, 100, 10, True, 0)[0]
        good = got == want
        ok = ok and good
        log(f"[host] T0B(a) T3 calc refusal k={k} f={f}: status {got} ({want}) -> {'as expected' if good else 'WRONG'}")
    return ok


def ist_fill(n: int, seed: int) -> bytes:
    """The selftest's ist_fill(): xorshift32 bytes (bits 31:24 of each state)."""
    x = seed if seed else 0x2545F491
    out = bytearray()
    for _ in range(n):
        x ^= (x << 13) & 0xFFFFFFFF
        x ^= x >> 17
        x ^= (x << 5) & 0xFFFFFFFF
        out.append((x >> 24) & 0xFF)
    return bytes(out)


def target_reg(i: int) -> int:
    """The target's register i after INIT (tools/i2c_target_f334: reg[i] = i ^ 0xA5)."""
    return (i & 0xFF) ^ 0xA5


def target_bytes(p: int, n: int) -> bytes:
    return bytes(target_reg(p + i) for i in range(n))


X_RX = re.compile(r"\[i2c\] X (\S+) (cm4|cm7) f=(\d+) op=(\S+) mode=(\S+) a=([0-9A-F]+) n=(\d+) p=(\d+) st=(\d+) "
                  r"crc=([0-9A-F]+) mis=(\d+) bad=(-?\d+) cyc=(\d+) dtx=(\d+) drx=(\d+) irq=(\d+) irqa=(\d+) "
                  r"cr1=([0-9A-F]+) sen=(\d+) isr=([0-9A-F]+) aux=(\d+) t=(\d)(?: d=([0-9A-F]+))?")


def parse_x(text: str) -> Optional[dict]:
    m = X_RX.search(text)
    if not m:
        return None
    g = m.groups()
    return {"test": g[0], "core": g[1], "f": int(g[2]), "op": g[3], "mode": g[4], "a": int(g[5], 16), "n": int(g[6]),
            "p": int(g[7]), "st": int(g[8]), "crc": int(g[9], 16), "mis": int(g[10]), "bad": int(g[11]),
            "cyc": int(g[12]), "dtx": int(g[13]), "drx": int(g[14]), "irq": int(g[15]), "irqa": int(g[16]),
            "cr1": int(g[17], 16), "sen": int(g[18]), "isr": int(g[19], 16), "aux": int(g[20]), "t": int(g[21]),
            "d": bytes.fromhex(g[22]) if g[22] else None, "text": text}


RPT_RX = re.compile(r"^RPT n=(\d+) st=(\d+) sr=(\d+) sp=(\d+) dir=(\S*) rx=(\d+) tx=(\d+) nk=(\d+) nm=(\d+) "
                    r"crc_rx=([0-9A-F]+) crc_tx=([0-9A-F]+) p0=(\d+) p1=(\d+) ev=([0-9A-F]+) us=(\d+)")


def parse_rpt(text: str) -> Optional[dict]:
    m = RPT_RX.search(text)
    if not m:
        return None
    g = m.groups()
    return {"seq": int(g[0]), "st": int(g[1]), "sr": int(g[2]), "sp": int(g[3]), "dir": g[4], "rx": int(g[5]),
            "tx": int(g[6]), "nk": int(g[7]), "nm": int(g[8]), "crc_rx": int(g[9], 16), "crc_tx": int(g[10], 16),
            "p0": int(g[11]), "p1": int(g[12]), "ev": int(g[13], 16), "us": int(g[14]), "text": text}


def crc32(b: bytes) -> int:
    return zlib.crc32(b) & 0xFFFFFFFF


def tier_base(test: str) -> str:
    """The test of an X line or a SCOPE point without the i2c_ll suffix (phase F2)."""
    return test[:-3] if test.endswith("-LL") else test


def x_expect(x: dict) -> dict:
    """What the host itself expects of a transfer the selftest made (independent of
    the firmware's own verdicts): its status, the data, and the target's report.
    x["armed"] is what the host armed in the target for this transfer (NACK k,
    CORRUPT k), from its own AT handling."""
    # Phase F2: "<test>-LL" is the same transfer through i2c_ll, expected the same.
    test, op, n, p = tier_base(x["test"]), x["op"], x["n"], x["p"]
    armed = x.get("armed") or {}
    e: dict = {"st": ST_OK, "rpt": None}
    if op == "W":
        data = ist_fill(n, p)
        e["data"] = data
        e["crc"] = crc32(data)
        e["rpt"] = {"st": 1, "sr": 0, "sp": 1, "dir": "W", "rx": n, "tx": 0, "nk": 0, "crc_rx": crc32(data), "ev": 0}
        k = armed.get("nack")
        if k is not None and k < n:
            # The target NACKs data byte k: bytes 0..k received, byte k refused (R2-2)
            e["st"] = ST_NACK
            e["rpt"] = {"st": 1, "sr": 0, "sp": 1, "dir": "W", "rx": k + 1, "tx": 0, "nk": 1,
                        "crc_rx": crc32(data[:k + 1]), "ev": 0}
            if x["mode"] == "it":
                e["dtx"] = k
    elif op in ("R", "WR"):
        data = bytearray(target_bytes(p, n))
        k = armed.get("corrupt")
        if k is not None and k < n:
            data[k] ^= 0xFF                              # the target inverts the k-th byte it sends
            e["mis"] = 1
            e["bad"] = k
        data = bytes(data)
        e["data"] = data
        e["crc"] = crc32(data)
        if op == "R":
            e["rpt"] = {"st": 1, "sr": 0, "sp": 1, "dir": "R", "rx": 0, "tx": n, "nm": 1, "crc_tx": crc32(data),
                        "ev": 0, "p0": p & 0xFF}
        else:
            e["rpt"] = {"st": 2, "sr": 1, "sp": 1, "dir": "WR", "rx": 1, "tx": n, "nm": 1,
                        "crc_rx": crc32(bytes([p & 0xFF])), "crc_tx": crc32(data), "ev": 0, "p1": (p + n) & 0xFF}
    elif op == "P":
        e["rpt"] = {"st": 1, "sr": 0, "sp": 1, "dir": "W", "rx": 0, "tx": 0, "ev": 0x10}
    if x["a"] == NO_TARGET:
        e["st"] = ST_NACK
        e["rpt"] = None
    if test == "T6C":
        e["st"] = ST_TIMEOUT
        e["aux"] = ST_PENDING
        e["rpt"] = {"sp": 0, "ev_has": 0x08, "dir": "W", "rx_max": 2}
    if test == "T7C":
        e["st"] = ST_TIMEOUT
        e["sen"] = n
        e["rpt"] = {"sp": 0, "ev_has": 0x08, "dir": "R", "tx_max": 3}
    if test.startswith("T10B"):
        e["st"] = ST_BUS if test == "T10B" else ST_TIMEOUT
        e["rpt"] = None                                  # the target is off while it holds SCL
    return e


def x_problems(x: dict, rpt: Optional[dict]) -> list[str]:
    """Everything that differs between a transfer as the selftest reports it, as
    the host expects it, and as the target saw it."""
    e = x_expect(x)
    bad = []
    if x["st"] != e["st"]:
        bad.append(f"status {ST.get(x['st'], x['st'])} ({ST[e['st']]})")
    if "crc" in e and e["st"] == ST_OK and x["crc"] != e["crc"]:
        bad.append(f"crc {x['crc']:08X} (host {e['crc']:08X})")
    if x["op"] in ("R", "WR") and e["st"] == ST_OK and (x["mis"], max(x["bad"], -1)) != (e.get("mis", 0), e.get("bad", -1)):
        bad.append(f"{x['mis']} bytes differ from the target's known content, first at {x['bad']} "
                   f"(the host expects {e.get('mis', 0)} at {e.get('bad', -1)})")
    if x["d"] is not None and "data" in e and x["d"] != e["data"][:len(x["d"])]:
        bad.append(f"d={x['d'].hex().upper()} (host {e['data'][:len(x['d'])].hex().upper()})")
    if "dtx" in e and x["dtx"] != e["dtx"]:
        bad.append(f"acknowledged count {x['dtx']} ({e['dtx']})")
    if "aux" in e and x["aux"] != e["aux"]:
        bad.append(f"aux {x['aux']} ({e['aux']})")
    if "sen" in e and x["sen"] != e["sen"]:
        bad.append(f"sentinel bytes {x['sen']} ({e['sen']})")
    if x["mode"] == "it" and (x["irqa"] != 0 or (x["cr1"] & 0xFE) != 0):
        bad.append(f"after the end: IRQs {x['irqa']}, CR1 0x{x['cr1']:08X}")
    if x["mode"] == "dma" and x["aux"] != 0:
        bad.append(f"DMA steps 0x{x['aux']:X}")
    want = e["rpt"]
    if (x["t"] == 1) != (want is not None) and not tier_base(x["test"]).startswith("T10B"):
        bad.append(f"t={x['t']} but the host expects {'a' if want else 'no'} report")
    if want is not None:
        if rpt is None:
            bad.append("no RPT from the target")
        else:
            for key, val in want.items():
                if key == "ev_has":
                    if not rpt["ev"] & val:
                        bad.append(f"RPT ev 0x{rpt['ev']:02X} without 0x{val:02X}")
                elif key == "rx_max":
                    if rpt["rx"] > val:
                        bad.append(f"RPT rx {rpt['rx']} (<= {val})")
                elif key == "tx_max":
                    if rpt["tx"] > val:
                        bad.append(f"RPT tx {rpt['tx']} (<= {val})")
                elif rpt.get(key) != val:
                    got = rpt.get(key)
                    bad.append(f"RPT {key}={got:08X} ({val:08X})" if key.startswith("crc") else f"RPT {key}={got} ({val})")
    return bad


def t0b_pairing_selftest(log: Callable[[str], None]) -> bool:
    """The X/RPT rules on lines typed here: a good read, its target report with the
    CRC of the bytes it sent, and four faults that must be found."""
    data = target_bytes(0, 4)
    x = (f"[i2c] X T4 cm7 f=400000 op=R mode=poll a=42 n=4 p=0 st=0 crc={crc32(data):08X} mis=0 bad=-1 cyc=1 "
         f"dtx=0 drx=4 irq=0 irqa=0 cr1=00000001 sen=0 isr=00000001 aux=0 t=1")
    r = (f"RPT n=1 st=1 sr=0 sp=1 dir=R rx=0 tx=4 nk=0 nm=1 crc_rx=FFFFFFFF crc_tx={crc32(data):08X} p0=0 p1=4 "
         f"ev=00 us=100")
    cases = [(x, r, True, "a good read"),
             (x, r.replace("tx=4", "tx=5"), False, "the target sent 5"),
             (x, r.replace(f"crc_tx={crc32(data):08X}", "crc_tx=00000000"), False, "another CRC at the target"),
             (x.replace("st=0", "st=7"), r, False, "status NACK"),
             (x, None, False, "no report")]
    ok = True
    for xt, rt, want, why in cases:
        px = parse_x(xt)
        pr = parse_rpt(rt) if rt else None
        probs = x_problems(px, pr) if px else ["X line not parsed"]
        good = (not probs) == want
        ok = ok and good
        log(f"[host] T0B(a) X/RPT pairing [{why}]: {'accepted' if not probs else 'refused: ' + '; '.join(probs)} -> "
            f"{'as expected' if good else 'WRONG'}")
    return ok


def expected_frames(xs: list[dict]) -> list[tuple[str, list[tuple[int, int]], bool]]:
    """The bus frames a group of transfers must show: one entry per START/Sr, its
    bytes with the acknowledge bit (0 ACK, 1 NACK), and whether a STOP ends it."""
    out = []
    for x in xs:
        a, op, n, p = x["a"], x["op"], x["n"], x["p"]
        nack_addr = 1 if a == NO_TARGET else 0
        if op == "P":
            out.append(("S", [(a << 1, nack_addr)], True))
        elif op == "W":
            d = ist_fill(n, p)
            k = (x.get("armed") or {}).get("nack")
            if k is not None and k < n:
                out.append(("S", [(a << 1, 0)] + [(b, 0) for b in d[:k]] + [(d[k], 1)], True))
            elif nack_addr:
                out.append(("S", [(a << 1, 1)], True))
            else:
                out.append(("S", [(a << 1, 0)] + [(b, 0) for b in d], True))
        elif op == "R":
            d = target_bytes(p, n)
            out.append(("S", [((a << 1) | 1, 0)] + [(b, 1 if i == n - 1 else 0) for i, b in enumerate(d)], True))
        elif op == "WR":
            d = target_bytes(p, n)
            out.append(("S", [(a << 1, 0), (p & 0xFF, 0)], False))
            out.append(("Sr", [((a << 1) | 1, 0)] + [(b, 1 if i == n - 1 else 0) for i, b in enumerate(d)], True))
    return out


def frames_of(dec: dict) -> list[tuple[str, list[tuple[int, int]], bool]]:
    return [(f["kind"], list(f["bytes"]), f["stop"]) for f in dec["frames"]]


def frames_text(fr: list[tuple[str, list[tuple[int, int]], bool]]) -> str:
    return " | ".join(f"{k} " + " ".join(f"{v:02X}{'N' if a else 'A'}" for v, a in b) + (" P" if s else "")
                      for k, b, s in fr) or "(nothing)"


def t0b_su_drivers(syms: list) -> list[tuple[int, str]]:
    """The (bit, driver) of every data or acknowledge bit of a synthetic capture
    whose SDA changes while SCL is low, written from the I2C rules: the master
    drives the address and written data and acknowledges read data; the target
    acknowledges the address and written data and drives read data."""
    out: list[tuple[int, str]] = []
    for _, frame, _ in syms:
        lvl = 0                                         # SDA low after the (repeated) START
        rd = bool(frame[0] & 2)
        for j, s in enumerate(frame):
            for k in range(8, -1, -1):
                v, b = (s >> k) & 1, 8 - k
                if v != lvl:
                    master = (b < 8 and (j == 0 or not rd)) or (b == 8 and j >= 1 and rd)
                    out.append((b, "m" if master else "s"))
                lvl = v
    return out


def t0b_decoder_selftest(log: Callable[[str], None]) -> bool:
    """ad2.i2c_decode_analog() on a synthetic write-read before the instrument is
    opened: decoded exactly, each tSU;DAT given to the side that drives its bit;
    with SCL and SDA swapped (T5's control) it is not."""
    want = [("S", [(0x84, 0), (0x07, 0)], False), ("Sr", [(0x85, 0), (0xA2, 1)], True)]
    syms = [("S", [(0x84 << 1) | 0, (0x07 << 1) | 0], False), ("Sr", [(0x85 << 1) | 0, (0xA2 << 1) | 1], True)]
    want_su = t0b_su_drivers(syms)
    ok = True
    for rate, bit, rise in ((20e6, 10e-6, 250e-9), (50e6, 2.5e-6, 250e-9)):
        scl, sda = ad2mod.i2c_synth_analog(syms, rate, bit, VDD, rise)
        dec = ad2mod.i2c_decode_analog(scl, sda, rate, VDD)
        got = frames_of(dec)
        su = [(b, who) for _, b, _, who in dec["t_su_dat"]]
        swapped = frames_of(ad2mod.i2c_decode_analog(sda, scl, rate, VDD))
        good = got == want and swapped != want and su == want_su and "s" in [w for _, w in su]
        ok = ok and good
        log(f"[host] T0B(a) decoder on a synthetic capture ({rate / 1e6:.0f} MS/s, {1 / bit / 1e3:.0f} kHz): "
            f"'{frames_text(got)}'; tSU;DAT drivers {''.join(w for _, w in su)} (want "
            f"{''.join(w for _, w in want_su)}); swapped '{frames_text(swapped)[:40]}' -> "
            f"{'as expected' if good else 'WRONG'}")
    return ok


def t5_timing_problems(dec: dict, f: int, k: int, t: int, model: int, ratio: float, rate: float,
                       af: bool = True, dnf: int = 0) -> tuple[list[str], str, list[str]]:
    """T5's times of one capture: every measured time at least the mode's minimum
    (tables 405/407); EVERY tLOW and tHIGH between its counter (SCLL, SCLH) and the
    counter plus the synchronisation of the model of 50:977-989, with the filters
    the bench really uses (af, dnf, from the SCOPE line) at their largest delay and
    the t_r and t_f measured here (audit F1, m25); f_SCL not above the one asked
    and within 5 % of the model, both normalised by the CPU clock ratio (CLKREF,
    plan N1). Tolerance per interval: 2 x (2 samples + 2 kernel cycles), one per
    edge; the kernel cycles in AD2 time (N1)."""
    m = 0 if f <= 100000 else (1 if f <= 400000 else 2)
    _, tl_min, th_min, tsu_min, _, _, tr_max, tf_max, _, thdsta_min, tsusta_min, tsusto_min, tbuf_min = MODES[m]
    ns = 1e-9
    bad: list[str] = []

    def med(v: list) -> float:
        v = sorted(v)
        return v[len(v) // 2] if v else float("nan")

    tlow, thigh = dec["t_low"], dec["t_high"]
    # The low periods where the target may stretch SCL (UM10204 3.1.9): before the
    # acknowledge of the address (its ADDR flag) and at a byte boundary after the
    # address (bit 0 of byte j >= 1: a byte it has not written yet). Only the lower
    # bound applies to them, the upper one to every other low period, the master's
    # own counter (05_i2c F1, m25).
    at = [(a + (-1,))[:3] for a in (dec.get("t_low_at") or [(1, 0)] * len(tlow))]
    frames = dec.get("frames") or []

    def stretchable(b: int, j: int, fi: int) -> bool:
        # Only where a target that acknowledged its address may hold SCL (audit F1
        # part 2, O16): before that acknowledge (its ADDR flag), and at bit 0 of a
        # byte that follows (not the master's own low before a STOP or an Sr, not
        # the frame of a refused address).
        if not 0 <= fi < len(frames):
            return False
        fb = frames[fi]["bytes"]
        if b == 8 and j == 0:
            return True                                # the address's ACK bit itself
        acked = bool(fb) and fb[0][1] == 0
        return acked and b == 0 and 1 <= j < len(fb)

    tlow_at = [(v, b, j) for v, (b, j, fi) in zip(tlow, at) if True]
    keep = [stretchable(b, j, fi) for (b, j, fi) in at]
    tlow_byte = [v for v, k in zip(tlow, keep) if k]
    # tSU;DAT of the bits the H755 drives is judged; the target's own (its TIMINGR,
    # tools/i2c_target_f334) is recorded: the driver is not what sets it.
    tsud = [d for d, _, _, who in dec["t_su_dat"] if who == "m"]
    tsud_s = [d for d, _, _, who in dec["t_su_dat"] if who == "s"]
    pairs = [("tLOW", tlow, tl_min), ("tHIGH", thigh, th_min), ("tSU;DAT", tsud, tsu_min),
             ("tHD;STA", dec["t_hd_sta"], thdsta_min), ("tSU;STA", dec["t_su_sta"], tsusta_min),
             ("tSU;STO", dec["t_su_sto"], tsusto_min), ("tBUF", dec["t_buf"], tbuf_min)]
    # The mode's minimums with the measurement's own tolerance, 2 samples (decided by
    # the owner on 2026-10-08, audit F1 part 2 session 3d92f3e: tHIGH 599/600 ns at
    # 400 kHz against 600 with samples of 40 ns). A value below the minimum but within
    # the tolerance is not a failure; it is returned in notes, which the session records.
    notes: list[str] = []
    tol_min = 2 / rate
    for name, vals, lo in pairs:
        if vals and min(vals) < lo * ns - tol_min:
            bad.append(f"{name} min {min(vals) / ns:.0f} ns < {lo} - 2 samples ({tol_min / ns:.0f} ns)")
        elif vals and min(vals) < lo * ns:
            notes.append(f"{name} min {min(vals) / ns:.0f} ns: {lo - min(vals) / ns:.0f} ns below the {lo} ns minimum, "
                         f"within 2 samples ({tol_min / ns:.0f} ns)")
    trs = dec["t_r_scl"] + dec["t_r_sda"]
    tfs = dec["t_f_scl"] + dec["t_f_sda"]
    tr_m, tf_m = med(trs), med(tfs)
    if trs and max(trs) > tr_max * ns:
        bad.append(f"t_r max {max(trs) / ns:.0f} ns > {tr_max}")
    if tfs and max(tfs) > tf_max * ns:
        bad.append(f"t_f max {max(tfs) / ns:.0f} ns > {tf_max}")
    p = (t >> 28) & 0xF
    scll, sclh = ((t & 0xFF) + 1) * (p + 1), (((t >> 8) & 0xFF) + 1) * (p + 1)
    tk = ratio / k                                     # one kernel cycle, in seconds of the AD2
    afmax = T_AF[1] if af else 0
    sync = (_cdiv(int(round((tr_m + tf_m) / ns)) * k + 2 * afmax * k, G) + 2 * (dnf + 3)) if trs and tfs else 0
    tol = 2 * (2 / rate + 2 * tk)
    lo_l, hi_l = scll * tk - tol, (scll + sync) * tk + tol
    lo_h, hi_h = sclh * tk - tol, (sclh + sync) * tk + tol
    out_l = [(v, b, j) for (v, b, j), k in zip(tlow_at, keep) if v < lo_l or (v > hi_l and not k)]
    long_s = [(v, b, j) for (v, b, j), k in zip(tlow_at, keep) if v > hi_l and k]
    out_h = [v for v in thigh if not lo_h <= v <= hi_h]
    if out_l:
        bad.append(f"{len(out_l)} of {len(tlow)} tLOW outside {lo_l / ns:.0f}..{hi_l / ns:.0f} ns: "
                   + ", ".join(f"{v / ns:.0f} before bit {b} of byte {j}" for v, b, j in out_l[:3])
                   + " (where the target may stretch, the lower bound only)")
    if out_h:
        bad.append(f"{len(out_h)} of {len(thigh)} tHIGH outside {lo_h / ns:.0f}..{hi_h / ns:.0f} ns (first "
                   f"{out_h[0] / ns:.0f})")
    f_norm = dec["f_scl"] * ratio
    if dec["f_scl"] <= 0:
        bad.append("no f_SCL")
    else:
        if f_norm > f:
            bad.append(f"f_SCL {f_norm:.0f} Hz (normalised) above {f}")
        if abs(f_norm - model) > 0.05 * model:
            bad.append(f"f_SCL {f_norm:.0f} Hz (normalised) not within 5 % of the model {model}")
    txt = (f"tLOW {min(tlow, default=0) / ns:.0f}/{med(tlow) / ns:.0f}/{max(tlow, default=0) / ns:.0f} ns "
           f"(min/median/max of {len(tlow)}; counter {scll * tk / ns:.0f}, window {lo_l / ns:.0f}..{hi_l / ns:.0f}; "
           f"{len(tlow_byte)} where the target may stretch, lower bound only; above the window: "
           f"{', '.join(f'{v / ns:.0f} before bit {b} of byte {j}' for v, b, j in long_s) or 'none'}), "
           f"tHIGH {min(thigh, default=0) / ns:.0f}/{med(thigh) / ns:.0f}/{max(thigh, default=0) / ns:.0f} "
           f"(counter {sclh * tk / ns:.0f}, window {lo_h / ns:.0f}..{hi_h / ns:.0f}; filter {'analog' if af else 'off'}, "
           f"DNF {dnf}), tSU;DAT min {min(tsud, default=0) / ns:.0f} (H755 bits; target bits "
           f"{min(tsud_s, default=0) / ns:.0f}), tHD;STA "
           f"{min(dec['t_hd_sta'], default=0) / ns:.0f}, tSU;STA {min(dec['t_su_sta'], default=0) / ns:.0f}, tSU;STO "
           f"{min(dec['t_su_sto'], default=0) / ns:.0f}, tBUF {min(dec['t_buf'], default=0) / ns:.0f} ns; t_r "
           f"{tr_m / ns:.0f} t_f {tf_m / ns:.0f} ns (median, 30-70 %); f_SCL {dec['f_scl']:.0f} Hz, normalised "
           f"{f_norm:.0f} (asked {f}, model {model})")
    return bad, txt, notes


def t0b_t5min_selftest(log: Callable[[str], None]) -> bool:
    """T5's minimums with 2 samples of tolerance (owner, 2026-10-08), on a capture
    typed here at 400 kHz and 25 MS/s (40 ns): a tHIGH of 599 ns is a note, not a
    failure; one of 519 ns (more than 80 ns below 600) fails."""
    ok = True
    st, t, _, model = calc_timing(100000000, 400000, 300, 10, True, 0)
    for th, want_bad, want_note in ((599e-9, False, True), (519e-9, True, False), (620e-9, False, False)):
        dec = {"t_low": [1.58e-6] * 3, "t_low_at": [(1, 0, 0), (2, 0, 0), (3, 0, 0)], "frames": [],
               "t_high": [620e-9, th], "t_su_dat": [], "t_hd_sta": [], "t_su_sta": [], "t_su_sto": [], "t_buf": [],
               "t_r_scl": [250e-9], "t_r_sda": [], "t_f_scl": [20e-9], "t_f_sda": [], "f_scl": 398000.0}
        bad, _, notes = t5_timing_problems(dec, 400000, 100000000, t, model, 1.0, 25e6)
        got_bad = any(x.startswith("tHIGH min") for x in bad)
        got_note = any(x.startswith("tHIGH min") for x in notes)
        good = got_bad == want_bad and got_note == want_note
        ok = ok and good
        log(f"[host] T0B(a) T5 minimum tHIGH {th * 1e9:.0f} ns at 25 MS/s: {'fails' if got_bad else 'passes'}"
            f"{' with a note' if got_note else ''} -> {'as expected' if good else 'WRONG'}")
    return ok


def t0b_t10b_selftest(log: Callable[[str], None]) -> bool:
    """T10(b)'s scope judgement (audit F1 part 3, B1/O22) on captures typed here:
    SCL held 50 ms; the Write's START (SDA falls) at 3 or 15 ms; SDA let go at
    n x TIMEOUTA from the SCL fall. Judged good: START at 3 ms released at 1 x,
    START at 15 ms released at 2 x (nwant 2). Refused: START at 15 ms released at
    1 x; START at 3 ms when 2 was wanted; a release 1 ms late."""
    import types
    rate, n_s = 100000.0, 6000
    want = 489 * 2048 / 1e8

    def wave(edges: list, start: int) -> list:
        out, lvl, k = [], start, 0
        edges = sorted(edges)
        for i in range(n_s):
            while k < len(edges) and i / rate >= edges[k][0]:
                lvl = edges[k][1]
                k += 1
            out.append(3.3 if lvl else 0.0)
        return out

    cases = [(0.003, 1, 0, 0.0, True, "START at 3 ms, released at 1 x TIMEOUTA"),
             (0.015, 2, 2, 0.0, True, "START at 15 ms, released at 2 x (2 wanted)"),
             (0.015, 1, 0, 0.0, False, "START at 15 ms, released at 1 x"),
             (0.003, 1, 2, 0.0, False, "START at 3 ms when 2 was wanted"),
             (0.003, 1, 0, 0.001, False, "released 1 ms late")]
    ok = True
    for sdaf, k_rel, nwant, late, good_want, why in cases:
        scl = wave([(0.001, 0), (0.051, 1)], 1)
        sda = wave([(0.001 + sdaf, 0), (0.001 + k_rel * want + late, 1)], 1)
        groups: list = []
        fake = types.SimpleNamespace(clk_ratio=1.0, clk_measured=True, low_run=Session.low_run,
                                     record=lambda t: None, log=lambda t: None,
                                     group=lambda key, g, d: groups.append(g))
        Session.scope_t10b(fake, "T10B mock", scl, sda, rate, {"tmo": 489, "kernel": 100000000, "nwant": nwant})
        got = bool(groups) and groups[0]
        good = got == good_want
        ok = ok and good
        log(f"[host] T0B(a) T10(b) judgement [{why}]: {'accepted' if got else 'refused'} -> "
            f"{'as expected' if good else 'WRONG'}")
    return ok


# ---- What the selftest must do in a session (audit F1, m26) ------------------------------

T4_SIZES = (1, 2, 255, 256, 1000, 65535)
T6_SIZES = (1, 2, 256, 1000)
T7_SIZES = (2, 255, 256, 4096)


def expected_transfers(state: str) -> Counter:
    """Every X line the selftest must print in a session, from the plan's test list
    (sizes of T4, T6 and T7; T5 only with taps; 1 MHz only without): keys (test,
    core, f, op, mode, address, n)."""
    c: Counter = Counter()
    taps = state == "taps"
    t4_speeds = (100000, 400000) if taps else (400000, 1000000)
    fm = 400000 if taps else 1000000
    for core in ("cm4", "cm7"):
        for f in t4_speeds:
            c[("T4", core, f, "P", "poll", TARGET, 0)] += 1
            c[("T4", core, f, "P", "poll", NO_TARGET, 0)] += 1
            for op in ("R", "W", "WR"):
                for n in T4_SIZES:
                    c[("T4", core, f, op, "poll", TARGET, n)] += 1
            c[("T4C", core, f, "R", "poll", TARGET, 16)] += 1
            if taps:
                for op, a, n in (("P", TARGET, 0), ("P", NO_TARGET, 0), ("R", TARGET, 2), ("W", TARGET, 1),
                                 ("W", TARGET, 2), ("WR", TARGET, 1)):
                    c[("T5", core, f, op, "poll", a, n)] += 1
        for op in ("R", "W", "WR"):
            for n in T6_SIZES:
                c[("T6", core, fm, op, "it", TARGET, n)] += 1
        c[("T6C", core, fm, "W", "itc", TARGET, 2)] += 1
        for op in ("R", "W"):
            for n in T7_SIZES:
                c[("T7", core, fm, op, "dma", TARGET, n)] += 1
        c[("T7C", core, fm, "R", "dmac", TARGET, 64)] += 1
        c[("T8A", core, fm, "P", "it", NO_TARGET, 0)] += 1
        c[("T8A", core, fm, "R", "poll", TARGET, 4)] += 1
        c[("T8A", core, fm, "W", "poll", NO_TARGET, 1)] += 1
        c[("T8B", core, fm, "W", "it", TARGET, 8)] += 1
        c[("T8B", core, fm, "W", "poll", TARGET, 8)] += 2
        for t in ("T10A0", "T10A"):
            for op in ("R", "W"):
                c[(t, core, fm, op, "poll", TARGET, 4)] += 1
        c[("T10B", core, fm, "W", "poll", TARGET, 2)] += 1
        c[("T10BC", core, fm, "W", "poll", TARGET, 2)] += 1
    c[("T9", "cm7", fm, "W", "poll", TARGET, 2)] += 1
    c[("T9B", "cm7", fm, "W", "poll", TARGET, 2)] += 1
    # Phase F2 (steps F2_CM4, F2_CM7): T4, T5, T6, T7 through i2c_ll, the same lists.
    for core in ("cm4", "cm7"):
        for f in t4_speeds:
            c[("T4-LL", core, f, "P", "poll", TARGET, 0)] += 1
            c[("T4-LL", core, f, "P", "poll", NO_TARGET, 0)] += 1
            for op in ("R", "W", "WR"):
                for n in T4_SIZES:
                    c[("T4-LL", core, f, op, "poll", TARGET, n)] += 1
            c[("T4C-LL", core, f, "R", "poll", TARGET, 16)] += 1
            if taps:
                for op, a, n in (("P", TARGET, 0), ("P", NO_TARGET, 0), ("R", TARGET, 2), ("W", TARGET, 1),
                                 ("W", TARGET, 2), ("WR", TARGET, 1)):
                    c[("T5-LL", core, f, op, "poll", a, n)] += 1
        for op in ("R", "W", "WR"):
            for n in T6_SIZES:
                c[("T6-LL", core, fm, op, "it", TARGET, n)] += 1
        c[("T6C-LL", core, fm, "W", "itc", TARGET, 2)] += 1
        for op in ("R", "W"):
            for n in T7_SIZES:
                c[("T7-LL", core, fm, op, "dma", TARGET, n)] += 1
        c[("T7C-LL", core, fm, "R", "dmac", TARGET, 64)] += 1
    return c


def expected_scopes(state: str) -> Counter:
    """Every scope capture the selftest must announce: (test, k, f). None without taps."""
    c: Counter = Counter()
    if state != "taps":
        return c
    for _core in ("cm4", "cm7"):
        for f in (100000, 400000):
            for k in range(5):
                c[("T5", k, f)] += 1
        c[("T8", 0, 400000)] += 1
        c[("T10", 0, 400000)] += 1
        c[("T10", 1, 400000)] += 1
        c[("T10B", 0, 400000)] += 1
    c[("T9", 0, 0)] += 1
    c[("T9B", 0, 400000)] += 1
    for _core in ("cm4", "cm7"):                       # phase F2: T5 through i2c_ll
        for f in (100000, 400000):
            for k in range(5):
                c[("T5-LL", k, f)] += 1
    return c


def s19_x_problem(state: str, x: dict) -> Optional[str]:
    """S19 in the host (audit F1, m27): no transfer above 400 kHz with taps."""
    if state == "taps" and x["f"] > 400000:
        return f"S19: a transfer at {x['f']} Hz with taps: {x['text'].strip()}"
    return None


def t0b_s19_selftest(log: Callable[[str], None]) -> bool:
    """The three S19 aborts of the host (m27) on inputs typed here, through the
    code the session runs (audit F1 part 2, m35): an X line at 1 MHz, a SCOPE point
    at 1 MHz and FMP 1, each with taps, end the session; the same at 400 kHz, or
    without taps, do not."""
    import types
    ok = True
    x_tpl = ("[i2c] X T4 cm7 f={f} op=W mode=poll a=42 n=1 p=1 st=0 crc=00000000 mis=0 bad=-1 cyc=1 dtx=1 drx=0 irq=0 "
             "irqa=0 cr1=00000001 sen=0 isr=00000001 aux=0 t=1")
    for state, f, want in (("taps", 1000000, True), ("taps", 400000, False), ("notaps", 1000000, False)):
        got = s19_x_problem(state, parse_x(x_tpl.format(f=f))) is not None
        good = got == want
        ok = ok and good
        log(f"[host] T0B(a) S19 X line at {f} Hz ({state}): {'aborts' if got else 'goes on'} -> "
            f"{'as expected' if good else 'WRONG'}")
    tag = "SCOPE T5 k=0 f={f} kernel=100000000 timingr=00000000 model=0 rate=25000000 trig=sda af=1 dnf=0"
    for state, f, want in (("taps", 1000000, True), ("taps", 400000, False)):
        fake = types.SimpleNamespace(state=state, scopes_seen=Counter(), scope_arm=lambda spec: None)
        try:
            Session.at_action(fake, tag.format(f=f))
            got = False
        except BenchAbort:
            got = True
        good = got == want
        ok = ok and good
        log(f"[host] T0B(a) S19 SCOPE point at {f} Hz ({state}): {'aborts' if got else 'goes on'} -> "
            f"{'as expected' if good else 'WRONG'}")
    for state, want in (("taps", True), ("notaps", False)):
        sent: list = []
        fake = types.SimpleNamespace(state=state, f334=types.SimpleNamespace(command=lambda *a, **k: sent.append(a)))
        try:
            Session.f334_cmd(fake, ["FMP", "1"])
            got = False
        except BenchAbort:
            got = True
        good = got == want and (bool(sent) != want)
        ok = ok and good
        log(f"[host] T0B(a) S19 FMP 1 ({state}): {'aborts, nothing sent' if got else 'sent'} -> "
            f"{'as expected' if good else 'WRONG'}")
    return ok


def manifest_problems(want: Counter, got: Counter) -> list[str]:
    missing, extra = want - got, got - want
    out = []
    if missing:
        out.append(f"{sum(missing.values())} missing, first {sorted(missing)[0]}")
    if extra:
        out.append(f"{sum(extra.values())} not on the list, first {sorted(extra)[0]}")
    return out


def t0b_manifest_selftest(log: Callable[[str], None]) -> bool:
    """The list check on lists typed here: the full list passes; one T4 size left
    out, one extra transfer, and a T5 capture left out are found."""
    ok = True
    for state in ("taps", "notaps"):
        want = expected_transfers(state)
        less = want.copy()
        less[("T4", "cm4", 400000, "W", "poll", TARGET, 65535)] -= 1
        more = want.copy()
        more[("T4", "cm7", 400000, "W", "poll", TARGET, 7)] += 1
        cases = [(want, True, "the full list"), (+less, False, "T4 CM4 400 kHz without the write of 65535"),
                 (more, False, "an extra write of 7")]
        if state == "taps":
            sw = expected_scopes(state)
            sl = sw.copy()
            sl[("T5", 3, 100000)] -= 1
            cases += [(None, True, "the full capture list"), (+sl, False, "a T5 capture left out")]
        for got, good_want, why in cases:
            if got is None:
                probs = manifest_problems(expected_scopes(state), expected_scopes(state))
            elif why.startswith("a T5"):
                probs = manifest_problems(expected_scopes(state), got)
            else:
                probs = manifest_problems(want, got)
            good = (not probs) == good_want
            ok = ok and good
            log(f"[host] T0B(a) m26 list ({state}) [{why}]: {'; '.join(probs) or 'complete'} -> "
                f"{'as expected' if good else 'WRONG'}")
    return ok


# ---- The session ---------------------------------------------------------------------------

class Session:
    def __init__(self, ad2: AD2, con: H755Console, f334: F334, state: str, log: Callable[[str], None]) -> None:
        self.ad2 = ad2
        self.con = con
        self.f334 = f334
        self.state = state
        self.log = log
        self.checks: list[tuple[str, bool]] = []
        self.records: list[str] = []
        self.step_idx = 0
        self.dout_seen: list[tuple[str, int]] = []

    def check(self, name: str, ok: bool, detail: str) -> None:
        self.checks.append((name, ok))
        self.log(f"[host] {name}: {detail} -> {'PASS' if ok else 'FAIL'}")

    def record(self, text: str) -> None:
        """What the session saw but cannot fail here: logged, not counted."""
        self.records.append(text)
        self.log(f"[host] recorded: {text}")

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
            self.f334.check_abort()
            if time.monotonic() > end:
                raise BenchAbort(f"MCU_ACK did not go {'high' if high else 'low'} in {timeout_s} s")

    def sync(self, pre: Optional[Callable[[], None]] = None, timeout_s: float = HS_TIMEOUT_S) -> None:
        self.wait_ack(True, timeout_s)
        if pre:
            pre()
        self.req(True)
        self.wait_ack(False)
        self.req(False)

    def set_oe(self, mask: int) -> None:
        i2c_set_oe(self.ad2, mask, GPIO_BENCH_BUS_NONE)
        if self.ad2.get_oe() != mask:
            raise BenchAbort("OE read-back mismatch")

    # -- S17, S19
    def scope_levels(self) -> tuple[float, float]:
        return (self.ad2.analog_in_average(SCOPE_SCL, 2000, 100e3, 10.0),
                self.ad2.analog_in_average(SCOPE_SDA, 2000, 100e3, 10.0))

    def bus_rest(self, label: str, mcu: Optional[dict] = None) -> None:
        """S17 (and S19 through the scope and PF14): the bus idle and released."""
        lv, raw = self.f334.levels()
        ok15, why15 = s15_report_ok(raw)
        v_scl, v_sda = self.scope_levels()
        bad, s19_bad = s17_scope_problems(self.state, v_scl, v_sda)
        if lv["scl"] != 1 or lv["sda"] != 1 or lv["busy"] != 0:
            bad.append(f"F334 reads SCL {lv['scl']} SDA {lv['sda']} BUSY {lv['busy']} (1, 1, 0)")
        if not ok15:
            bad.append(f"F334 pins: S15 {why15}")
        mtxt = "-"
        if mcu is not None:
            want_pf14 = 1 if self.state == "taps" else 0
            for name, want in (("pb8", 1), ("pb9", 1), ("pf14", want_pf14), ("pf15", 1)):
                if mcu[name] != want:
                    bad.append(f"CM7 reads {name.upper()} {mcu[name]} ({want})")
            mtxt = f"pb8={mcu['pb8']} pb9={mcu['pb9']} pf14={mcu['pf14']} (want {want_pf14}) pf15={mcu['pf15']}"
        ratio = v_scl / v_sda if v_sda > 0.1 else float("nan")
        crit = (f"taps: 2+ >= {SDA_MIN_V:.1f} V, 1+/2+ {TAPS_RATIO[0]:.3f}..{TAPS_RATIO[1]:.3f}"
                if self.state == "taps" else f"notaps: 1+ < {NOTAPS_SCL_MAX_V:.2f} V, 2+ >= {SDA_MIN_V:.1f} V")
        self.check(f"S17 {label}",
                   not bad, f"scope SCL(1+) {v_scl:.3f} V SDA(2+) {v_sda:.3f} V ratio {ratio:.4f} ({crit}); "
                   f"F334 scl={lv['scl']} sda={lv['sda']} busy={lv['busy']} S15 {why15}; CM7 {mtxt}"
                   + (f"; failing: {'; '.join(bad)}" if bad else ""))
        if s19_bad:
            raise BenchAbort(f"S19: declared '{self.state}': {'; '.join(s19_bad)}")
        if bad:
            raise BenchAbort(f"S17 at '{label}': {'; '.join(bad)}")

    # -- S7, S2 at every step boundary, S17 before the step
    def step(self, name: str) -> None:
        m = self.con.expect(r"\[i2c\] STEP (\S+) owner=(\S+) v=(\d+) pb8=(\d) pb9=(\d) pf14=(\d) pf15=(\d)", 120.0,
                            stop_on_abort=True)
        got, owner, ver = m.group(1), m.group(2), int(m.group(3))
        exp_name, exp_owner = STEPS[self.step_idx]
        if (got, owner, ver) != (name, exp_owner, BENCH_VERSION) or exp_name != name:
            raise BenchAbort(f"S7: announced {got} owner={owner} v={ver}, host table expects "
                             f"{exp_name} owner={exp_owner} v={BENCH_VERSION}")
        self.step_idx += 1
        self.ad2.set_safe_state()
        en, zet = self.ad2.dout_state()
        self.dout_seen.append((name, en))
        if en != 0 or zet != 0xFFFF:
            raise BenchAbort(f"Digital Out at step {name}: enable 0x{en:04X}, Zet 0x{zet:04X} (0x0000, 0xFFFF)")
        self.set_oe(DIO_REQ)
        self.log(f"[host] step {name}: S2, Digital Out enable 0x{en:04X} Zet 0x{zet:04X}, OE 0x{self.ad2.get_oe():04X}")
        mcu = {k: int(m.group(i)) for k, i in (("pb8", 4), ("pb9", 5), ("pf14", 6), ("pf15", 7))}
        self.sync(pre=lambda: self.bus_rest(f"before {name}", mcu))

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
        ctrl = reads[-1] ^ T0A_LINES
        self.check("T0A control", ctrl == (1 << T0A_CTRL_DIO),
                   f"firmware pulls DIO9 (PE5) down where the host expects all up: the check flags 0x{ctrl:04X} "
                   f"(0x{1 << T0A_CTRL_DIO:04X} expected)")

    @staticmethod
    def low_run(v: list[float], rate: float) -> tuple[float, float, int]:
        """(longest run below V_LOW in ms, minimum volts, number of runs below V_LOW)."""
        best = cur = runs = 0
        for x in v:
            if x < V_LOW:
                cur += 1
                if cur == 1:
                    runs += 1
                best = max(best, cur)
            else:
                cur = 0
        return best * 1e3 / rate, min(v), runs

    def t0c_hold(self, line: str) -> None:
        held_ch, other_ch = (SCOPE_SDA, SCOPE_SCL) if line == "SDA" else (SCOPE_SCL, SCOPE_SDA)
        self.wait_ack(True)
        rate = None
        if self.state == "taps":
            rate, _ = self.ad2.scope_arm_fall(SCOPE_RATE, SCOPE_N, held_ch, 1.5, 0.1, 10.0)
        self.req(True)
        self.wait_ack(False)
        self.req(False)
        ok_line = self.f334.hold(line, T0C_HOLD_MS)
        end = self.f334.expect(rf"^HOLD_END line={line} ms=(\d+) us=(\d+) edges=(\d+) blank=(\d+) edge1_us=\S+ cause=(\w+) scl=(\d) sda=(\d)(.*)$",
                               3.0)
        cap = self.ad2.scope_read(SCOPE_N, 3.0) if self.state == "taps" else None
        mrx = rf"\[i2c\] T0C hold {line}:" + "".join(
            rf" {p} lows=(\d+) low_us=(\d+) first_us=(\d+) end_low=(\d);" for p in ("PB8", "PB9", "PF14", "PF15"))
        mm = None
        t_end = time.monotonic() + 5.0
        while mm is None and time.monotonic() < t_end:
            for text in self.con.since(self.mark_t0c):
                mm = re.search(mrx, text) or mm
            if mm is None:
                time.sleep(0.05)
        if mm is None:
            raise BenchAbort(f"no CM7 line for the {line} hold")
        us, edges, blank, cause = int(end.group(2)), int(end.group(3)), int(end.group(4)), end.group(5)
        after_ok = end.group(6) == "1" and end.group(7) == "1"
        ok15, why15 = s15_report_ok(end.group(0))
        self.check(f"T0C hold {line}: F334", ok_line.startswith("OK") and F334_HOLD_US[0] <= us <= F334_HOLD_US[1] and
                   cause == "TIME" and edges == 0 and after_ok and ok15,
                   f"'{ok_line}'; held {us} us ({F334_HOLD_US[0]}..{F334_HOLD_US[1]}), cause {cause}, SCL rising edges "
                   f"counted {edges} (0) and in the first 10 us {blank} (recorded: the glitch of SDA's own fall), "
                   f"scl={end.group(6)} sda={end.group(7)} 1 ms after the release, pins after: S15 {why15}")
        g = [int(x) for x in mm.groups()]
        pb = {"PB8": (g[0], g[1]), "PB9": (g[4], g[5])}
        pf = {"PF14": (g[8], g[9]), "PF15": (g[12], g[13])}
        held_pb, other_pb = ("PB9", "PB8") if line == "SDA" else ("PB8", "PB9")
        held_pf, other_pf = ("PF15", "PF14") if line == "SDA" else ("PF14", "PF15")
        if self.state == "taps":
            pf_ok = pf[held_pf][0] == 1 and abs(pf[held_pf][1] - pb[held_pb][1]) <= max(50, pb[held_pb][1] // 100) and \
                pf[other_pf][0] == 0
            pf_txt = (f"{held_pf} (through {'J3' if line == 'SDA' else 'J2'}) low {pf[held_pf][0]} time(s) "
                      f"{pf[held_pf][1]} us like {held_pb}, {other_pf} {pf[other_pf][0]} falls")
        else:
            pf_ok = pf["PF14"][0] == 0 and pf["PF15"][0] == 0
            pf_txt = f"without taps PF14/PF15 do not follow the bus: falls {pf['PF14'][0]}, {pf['PF15'][0]} (0, 0)"
        self.check(f"T0C hold {line}: CM7 taps", pf_ok, pf_txt)
        if self.state == "taps":
            if cap is None:
                self.check(f"T0C hold {line}: scope", False, "the scope did not trigger on the falling edge")
            else:
                held_ms, vmin, runs = self.low_run(cap[held_ch], rate or SCOPE_RATE)
                other_min = min(cap[other_ch])
                ok = SCOPE_HOLD_MS[0] <= held_ms <= SCOPE_HOLD_MS[1] and vmin <= 0.4 and runs == 1 and \
                    other_min >= V_HIGH and cap[held_ch][-1] >= V_HIGH
                self.check(f"T0C hold {line}: scope", ok,
                           f"{'2+ SDA' if line == 'SDA' else '1+ SCL'} below {V_LOW:.2f} V for {held_ms:.2f} ms "
                           f"({SCOPE_HOLD_MS[0]}..{SCOPE_HOLD_MS[1]}) in {runs} run(s), minimum {vmin:.3f} V (<= 0.4), "
                           f"high again at the end {cap[held_ch][-1]:.3f} V; the other line's minimum {other_min:.3f} V "
                           f"(>= {V_HIGH:.2f}); {SCOPE_N} samples at {rate:.0f} S/s, trigger falling edge 1.5 V")
        else:
            self.log(f"[host] T0C hold {line}: no scope view without taps (plan D3, S19)")

    def t0c(self) -> None:
        self.step("T0C")
        self.mark_t0c = self.con.mark()
        m = self.con.expect(r"\[i2c\] T0C rest pb8=(\d) pb9=(\d) pf14=(\d) pf15=(\d)", 10.0)
        mcu = {k: int(m.group(i)) for k, i in (("pb8", 1), ("pb9", 2), ("pf14", 3), ("pf15", 4))}
        self.sync(pre=lambda: self.bus_rest("T0C at rest", mcu))
        self.t0c_hold("SDA")
        self.sync(pre=lambda: self.bus_rest("T0C after the SDA hold"))
        self.t0c_hold("SCL")
        self.sync(pre=lambda: self.bus_rest("T0C after the SCL hold"))
        m = self.con.expect(r"\[i2c\] T0C rest after pb8=(\d) pb9=(\d) pf14=(\d) pf15=(\d)", 10.0)
        mcu = {k: int(m.group(i)) for k, i in (("pb8", 1), ("pb9", 2), ("pf14", 3), ("pf15", 4))}
        self.sync(pre=lambda: self.bus_rest("T0C at rest after the holds", mcu))

    def t0b(self) -> None:
        self.step("T0B")
        m = self.con.expect(r"\[i2c\] T0B levels with CM4 AF4 pb8=(\d) pb9=(\d)", 60.0, stop_on_abort=True)
        self.sync(pre=lambda: self.bus_rest(f"T0B with PB8/PB9 in AF4 on CM4, I2C1 PE=0 (CM7 reads "
                                            f"pb8={m.group(1)} pb9={m.group(2)})"))
        self.sync(pre=lambda: self.bus_rest("T0B after CM4's bench_safe"))

    def t0b_cm7(self) -> None:
        self.step("T0B_CM7")
        m = self.con.expect(r"\[i2c\] T0B levels with CM7 AF6 pb8=(\d) pb9=(\d)", 60.0, stop_on_abort=True)
        self.sync(pre=lambda: self.bus_rest(f"T0B_CM7 with PB8/PB9 in AF6 on CM7, I2C4 PE=0 (CM7 reads "
                                            f"pb8={m.group(1)} pb9={m.group(2)})"))
        self.sync(pre=lambda: self.bus_rest("T0B_CM7 after CM7's bench_safe"))

    def t0b_timeout(self) -> None:
        """T0b(d), as 06_spi protocol v2 (audit B1): the host reads MCU_ACK low
        while the MCU holds it, then pulses HOST_REQ."""
        self.step("T0B_TIMEOUT")
        self.wait_ack(True)
        t_high = time.monotonic()
        self.log("[host] T0B(d) MCU_ACK high: the host deliberately does not raise HOST_REQ")
        self.wait_ack(False, T0B_ACK_LOW_LIMIT_S)
        held_s = time.monotonic() - t_high
        reads = []
        for _ in range(T0B_LOW_READS):
            reads.append((self.ad2.digital_io_read() & DIO_ACK) == 0)
            time.sleep(0.005)
        self.con.expect(r"\[i2c\] T0B timeout handled", 5.0)
        self.req(True)
        time.sleep(0.005)
        hi_ok = (self.ad2.digital_io_read() & DIO_ACK) == 0
        self.req(False)
        if not hi_ok:
            raise BenchAbort("T0B(d): MCU_ACK went high during the HOST_REQ pulse")
        ok = all(reads) and T0B_TIMEOUT_MIN_S <= held_s <= T0B_TIMEOUT_MAX_S
        self.check("T0B(d) firmware timeout", ok,
                   f"MCU_ACK stayed high {held_s:.3f} s without HOST_REQ ({T0B_TIMEOUT_MIN_S}..{T0B_TIMEOUT_MAX_S} s; "
                   f"the MCU waits 10 s of CYCCNT), then low in {sum(reads)}/{T0B_LOW_READS} reads over "
                   f"{T0B_LOW_READS * 5} ms while the MCU waited for the host; `T0B timeout handled` reported")

    # ================================================================ phase F1
    # The firmware leads; at every "AT <tag>" line it waits for a handshake, and
    # the host does its part before raising HOST_REQ (the target, the scope, the
    # LA). Between two AT lines the host collects the X lines (one per transfer)
    # and the target's RPT lines, and pairs those with t=1 in order.

    def f1_init(self) -> None:
        self.at_mark = self.con.mark()
        self.rpt_mark = len(self.f334.lines)
        self.f334_mark = len(self.f334.lines)
        self.lenient = False
        self.armed: dict = {}
        self.scope_pending: Optional[dict] = None
        self.scope_xs: list[dict] = []
        self.hold_pending: Optional[dict] = None
        self.groups: dict = {}
        self.t12_status: dict = {}
        self.clk_ratio = 1.0
        self.clk_measured = False
        self.t3_lines: list[str] = []
        self.timing: dict = {}
        self.x_seen: Counter = Counter()
        self.scopes_seen: Counter = Counter()
        self.bus_tr = self.bus_tf = 0
        self.hold_log: list[dict] = []
        self.t9b_scope: Optional[tuple[int, int]] = None

    def group(self, key: str, ok: bool, detail: str) -> None:
        g = self.groups.setdefault(key, {"n": 0, "bad": []})
        g["n"] += 1
        if not ok:
            g["bad"].append(detail)
            self.log(f"[host] {key}: {detail}")

    def flush_groups(self) -> None:
        for key, g in self.groups.items():
            self.check(key, not g["bad"], f"{g['n']} items, {len(g['bad'])} with a problem"
                       + (f"; first: {g['bad'][0]}" if g["bad"] else ""))
        self.groups = {}

    def f1_lines_since_at(self) -> list[str]:
        lines = self.con.since(self.at_mark)
        self.at_mark = self.con.mark()
        return lines

    def f334_lines(self) -> list[str]:
        out = self.f334.lines[self.f334_mark:]
        self.f334_mark = len(self.f334.lines)
        return out

    def settle(self, label: str) -> None:
        """The group that ended at this AT line: its X lines paired with the target's
        RPT lines, the scope capture made during it, the T3 lines."""
        lines = self.f1_lines_since_at()
        xs = [x for x in (parse_x(t) for t in lines) if x]
        self.t3_lines += [t for t in lines if re.search(r"\[i2c\] T3 \d+ ", t)]
        for t in lines:
            m = re.search(r"\[i2c\] TIMING (CM4|CM7) f=(\d+) kernel=(\d+) timingr=([0-9A-F]+) model=(\d+)", t)
            if m:
                self.timing[(m.group(1), int(m.group(2)))] = (int(m.group(3)), int(m.group(4), 16), int(m.group(5)))
        want = sum(1 for x in xs if x["t"] == 1)
        end = time.monotonic() + (3.0 if want else 0.3)
        rpts: list[dict] = []
        while time.monotonic() < end:
            rpts = [r for r in (parse_rpt(t) for t in self.f334.lines[self.rpt_mark:]) if r]
            if want and len(rpts) >= want:
                break
            time.sleep(0.05)
        self.rpt_mark = len(self.f334.lines)
        it = iter(rpts)
        for x in xs:
            why = s19_x_problem(self.state, x)                                  # m27
            if why:
                raise BenchAbort(why)
            self.x_seen[(x["test"], x["core"], x["f"], x["op"], x["mode"], x["a"], x["n"])] += 1
            if x["t"] == 1 and self.armed:
                x["armed"] = self.armed
                self.armed = {}
            rpt = next(it, None) if x["t"] == 1 else None
            probs = x_problems(x, rpt)
            key = f"{x['test']} {x['core'].upper()} {x['f']} Hz {x['op']} {x['mode']}"
            self.group(f"F1 {x['test']} {x['core'].upper()} {x['f']} Hz: transfer, host data and target report",
                       not probs, f"{key} n={x['n']} p={x['p']}: {'; '.join(probs)}")
            x["rpt"] = rpt
        extra = list(it)
        if extra and not self.lenient:
            self.group("F1 target reports without a transfer", False,
                       f"{len(extra)} RPT line(s) the selftest did not announce, before '{label}': {extra[0]['text']}")
        if self.scope_pending is not None:
            # A capture spans everything from its SCOPE point to the next SCOPE point
            # (the next capture, or "SCOPE end"): a hold and its transfer may come between.
            self.scope_xs += xs
            if label.startswith("SCOPE "):
                self.scope_finish(self.scope_pending, self.scope_xs)
                self.scope_pending = None
                self.scope_xs = []

    # -- scope ----------------------------------------------------------------------
    def scope_arm(self, spec: dict) -> None:
        n = self.ad2.scope_buffer_max()
        ch = SCOPE_SDA if spec["trig"] == "sda" else SCOPE_SCL
        rate, n = self.ad2.scope_arm_fall(float(spec["rate"]), n, ch, 1.5, 0.01, 10.0)
        spec["rate_actual"], spec["samples"] = rate, n
        self.scope_pending = spec

    def scope_finish(self, spec: dict, xs: list[dict]) -> None:
        cap = self.ad2.scope_read(spec["samples"], 2.0)
        test = spec["test"]
        base = tier_base(test)                     # "T5-LL" is judged as T5 (phase F2)
        name = f"F1 {test} scope k={spec['k']} f={spec['f']}"
        if cap is None:
            self.group(f"F1 {test} scope", False, f"{name}: the scope did not trigger")
            return
        scl, sda = cap
        rate = spec["rate_actual"]
        if test in ("T9", "T9B"):
            self.scope_t9(name, scl, sda, rate, exact=(test == "T9"))
            return
        if test == "T10B":
            self.scope_t10b(name, scl, sda, rate, spec)
            return
        dec = ad2mod.i2c_decode_analog(scl, sda, rate, VDD)
        got = frames_of(dec)
        want = expected_frames([x for x in xs])
        swapped = frames_of(ad2mod.i2c_decode_analog(sda, scl, rate, VDD))
        half = frames_of(ad2mod.i2c_decode_analog(scl, sda, rate, 1.0, 0.5, 0.5))
        self.log(f"[host] {name}: decoded '{frames_text(got)}', expected '{frames_text(want)}'; control swapped "
                 f"'{frames_text(swapped)[:60]}', at 0.5 V '{frames_text(half)[:60]}' ({'same' if half == got else 'differs'}); "
                 f"{spec['samples']} samples at {rate:.0f} S/s, glitches {dec['glitches']}")
        self.group(f"F1 {test} scope: decode", got == want and swapped != want,
                   f"{name}: decoded '{frames_text(got)}', expected '{frames_text(want)}', swapped "
                   f"{'differs' if swapped != want else 'SAME'}")
        if base == "T5":
            key = ("CM4" if xs and xs[0]["core"] == "cm4" else "CM7", spec["f"])
            k, t, model = self.timing.get(key, (spec["kernel"], spec["timingr"], spec["model"]))
            af, dnf = bool(spec.get("af", 1)), int(spec.get("dnf", 0))
            probs, txt, notes = t5_timing_problems(dec, spec["f"], k, t, model, self.clk_ratio, rate, af, dnf)
            for n_ in notes:
                self.record(f"T5 {key[0]} k={spec['k']} f={spec['f']}: {n_}")
            if not self.clk_measured:
                probs.append("no CLKREF: the times are not normalised (O13)")
            # The TIMINGR of the capture recomputed by the host (O13).
            hst = calc_timing(k, spec["f"], self.bus_tr, self.bus_tf, af, dnf)
            if hst[0] != ST_OK or hst[1] != t or hst[3] != model:
                probs.append(f"TIMINGR 0x{t:08X} model {model}, host calc_timing 0x{hst[1]:08X} model {hst[3]} "
                             f"(t_r {self.bus_tr}, t_f {self.bus_tf})")
            self.log(f"[host] {name} times: {txt}")
            self.group(f"F1 T5 {key[0]} {spec['f']} Hz scope: times", not probs, f"{name}: {'; '.join(probs)}")
            raw = dec["f_scl"]
            # R3-7 (audit F1, M3): the frequency on the bus as the AD2 measures it, a record.
            self.record(f"T5 {key[0]} k={spec['k']} f_SCL raw {raw:.0f} Hz with {spec['f']} Hz asked "
                        f"({(raw / spec['f'] - 1) * 100:+.2f} %), normalised {raw * self.clk_ratio:.0f} Hz "
                        f"(CLKREF ratio {self.clk_ratio:.5f})")
        if test == "T10":
            runs = self.low_runs(scl, rate, 150e-6)
            lens = [round(r * 1e6) for r in runs]
            want_n = 5
            ok = len(runs) >= want_n and all(190e-6 <= r <= 260e-6 for r in runs[:want_n])
            self.log(f"[host] {name} SCL held low >= 150 us {len(runs)} times: {lens} us")
            self.group("F1 T10(a) scope: stretching", ok, f"{name}: SCL low runs {lens} us (>= {want_n} of 200 us)")

    @staticmethod
    def low_runs(v: list[float], rate: float, min_s: float) -> list[float]:
        out, cur = [], 0
        for x in v:
            if x < V_LOW:
                cur += 1
            else:
                if cur / rate >= min_s:
                    out.append(cur / rate)
                cur = 0
        return out

    def scope_t10b(self, name: str, scl: list[float], sda: list[float], rate: float, spec: dict) -> None:
        """T10(b): SCL held 50 ms by the target while the master under test starts
        a transfer; TIMEOUTA (~10 ms) ends it. Recorded: how long each line stays
        low and what the bus does when the target lets SCL go. Judged: SCL low one
        run of about the hold, both lines high at the end of the capture."""
        lo_scl, _, runs_scl = self.low_run(scl, rate)
        lo_sda, _, runs_sda = self.low_run(sda, rate)
        s0, se = ad2mod.i2c_edges_analog(scl, rate, V_LOW, V_HIGH)
        d0, de = ad2mod.i2c_edges_analog(sda, rate, V_LOW, V_HIGH)
        ev = sorted([(e["t"], "SCL" + ("r" if e["rise"] else "f")) for e in se] +
                    [(e["t"], "SDA" + ("r" if e["rise"] else "f")) for e in de])
        t0 = ev[0][0] if ev else 0.0
        seq = ", ".join(f"{n} {1e3 * (t - t0):.3f} ms" for t, n in ev[:12])
        ok = 45.0 <= lo_scl <= 55.0 and scl[-1] >= V_HIGH and sda[-1] >= V_HIGH
        rel_txt = "no SDA release seen"
        if spec.get("tmo") and not self.clk_measured:
            ok = False
            rel_txt = "no CLKREF: TIMEOUTA cannot be timed (O13)"
        elif spec.get("tmo"):
            # With timeout_scl_low the master lets SDA go (its software reset at TIMEOUT).
            # With SCL held low TIMEOUT fires again every TIMEOUTA (silicon fact, audit F1
            # part 3, O22), counted from the SCL fall: the Write ends at the first multiple
            # after its own START, the SDA fall: n = ceil(SDAf / TIMEOUTA) (B1), in AD2
            # time (CLKREF). nwant (from the firmware): the n its START was placed for.
            want = spec["tmo"] * 2048 / spec["kernel"] * self.clk_ratio
            fall = next((e["t"] for e in se if not e["rise"]), None)
            sdaf = next((e["t"] for e in de if not e["rise"] and fall is not None and e["t"] > fall), None)
            rel = next((e["t"] for e in de if e["rise"] and sdaf is not None and e["t"] > sdaf), None)
            if fall is not None and sdaf is not None and rel is not None:
                n = max(1, math.ceil((sdaf - fall) / want))
                dt = rel - fall
                quiet = all(not (fall < e["t"] < sdaf) for e in de) and d0 == 1
                rel_txt = (f"Write's START (SDA falls) {(sdaf - fall) * 1e3:.3f} ms after SCL fell, n = {n}; SDA let go "
                           f"{dt * 1e3:.3f} ms after SCL fell (n x TIMEOUTA {n * want * 1e3:.3f} ms)")
                ok = ok and abs(dt - n * want) <= 0.05 * want and (spec.get("nwant", 0) in (0, n))
                if n >= 2:
                    rel_txt += (f"; the {n - 1} expiry(ies) before the START ended nothing (SDA high, no SDA edge "
                                f"until the START: {quiet})")
                    ok = ok and quiet
                self.record(f"T10(b) {name}: n = {n} (wanted {spec.get('nwant') or 'any'}); TIMEOUT fires every "
                            f"TIMEOUTA while SCL is low and ends the Write at the first one after its START")
            else:
                ok = False
        self.log(f"[host] {name}: SCL low {lo_scl:.2f} ms in {runs_scl} run(s), SDA low {lo_sda:.2f} ms in "
                 f"{runs_sda} run(s); edges from the first: {seq}; {rel_txt}; both high at the end "
                 f"{scl[-1] >= V_HIGH and sda[-1] >= V_HIGH}")
        self.group("F1 T10(b) scope: SCL held, TIMEOUT, bus free after", ok,
                   f"{name}: SCL low {lo_scl:.2f} ms ({runs_scl} runs), {rel_txt}, end levels "
                   f"{scl[-1]:.2f}/{sda[-1]:.2f} V")

    def scope_t9(self, name: str, scl: list[float], sda: list[float], rate: float, exact: bool = True) -> None:
        """T9: SDA held until the 5th SCL rising edge, then the STOP. exact: the 5
        edges are all RecoverBus's pulses; T9(b) (exact False): some may be the
        refused transfer's, the target still counts 5 before it lets go."""
        s0, se = ad2mod.i2c_edges_analog(scl, rate, V_LOW, V_HIGH)
        d0, de = ad2mod.i2c_edges_analog(sda, rate, V_LOW, V_HIGH)
        falls = [e["t"] for e in se if not e["rise"]]
        rises = [e["t"] for e in se if e["rise"]]
        sda_rise = [e["t"] for e in de if e["rise"]]
        sda_fall = [e["t"] for e in de if not e["rise"]]
        # SDA released after the 5th SCL rise and before a 6th; then the STOP (SDA low
        # and high again while SCL stays high).
        rel = sda_rise[0] if sda_rise else None
        before = sum(1 for t in rises if rel is not None and t < rel)
        stop_ok = len(sda_fall) >= 1 and len(sda_rise) >= 2 and rel is not None and sda_fall[-1] > rel
        ok = d0 == 0 and before == 5 and (len(falls) == 5 or not exact) and stop_ok
        if not exact:
            # T9(b): the refused Write's SCL rises come first, then (after the pins
            # change mode, > 1 ms later) RecoverBus's pulses: split at the first gap
            # above 500 us (m34). One group only: all of them RecoverBus's.
            rs = [t for t in rises if rel is None or t < rel]
            cut = next((i + 1 for i in range(len(rs) - 1) if rs[i + 1] - rs[i] > 500e-6), 0)
            self.t9b_scope = (cut, len(rs) - cut)
        self.log(f"[host] {name}: SDA low at the start {d0 == 0}, SCL pulses {len(falls)} falls / {len(rises)} rises, "
                 f"SDA released after {before} rises, then SDA falls {len(sda_fall)} rises {len(sda_rise)} (the STOP)")
        self.group(f"F1 {'T9' if exact else 'T9(b)'} scope: recovery", ok,
                   f"{name}: {len(falls)} SCL pulses, SDA released after {before}, STOP seen {stop_ok}")

    # -- the AT points ----------------------------------------------------------------
    def at_action(self, tag: str) -> tuple[Optional[Callable[[], None]], Optional[Callable[[], None]]]:
        """(before the handshake, after it) for one AT tag."""
        m = re.match(r"F334 (.+)$", tag)
        if m:
            cmd = m.group(1).split()
            return (lambda: self.f334_cmd(cmd)), None
        m = re.match(r"HOLD (SDA|SCL) (\d+)(?: EDGES (\d+))?$", tag)
        if m:
            line, ms, edges = m.group(1), int(m.group(2)), int(m.group(3) or 0)
            return (lambda: self.hold_start(line, ms, edges)), None
        if tag == "HOLD_END":
            return self.hold_end, None
        m = re.match(r"SCOPE (\S+) k=(\d+) f=(\d+) kernel=(\d+) timingr=([0-9A-F]+) model=(\d+) rate=(\d+) "
                     r"trig=(sda|scl)(?: tmo=(\d+))?(?: nwant=(\d+))?(?: af=(\d) dnf=(\d+))?$", tag)
        if m:
            spec = {"test": m.group(1), "k": int(m.group(2)), "f": int(m.group(3)), "kernel": int(m.group(4)),
                    "timingr": int(m.group(5), 16), "model": int(m.group(6)), "rate": int(m.group(7)),
                    "trig": m.group(8), "tmo": int(m.group(9) or 0), "nwant": int(m.group(10) or 0),
                    "af": int(m.group(11) or 1), "dnf": int(m.group(12) or 0)}
            if self.state != "taps":
                raise BenchAbort(f"S19: a scope capture announced without taps: {tag}")
            if spec["f"] > 400000:
                raise BenchAbort(f"S19: a capture at {spec['f']} Hz: {tag}")
            self.scopes_seen[(spec["test"], spec["k"], spec["f"])] += 1
            return (lambda: self.scope_arm(spec)), None
        if tag == "SCOPE end":
            return None, None
        m = re.match(r"CLKREF cyc=(\d+) cpu=(\d+)$", tag)
        if m:
            cyc, cpu = int(m.group(1)), int(m.group(2))
            box: dict = {}
            return (lambda: self.clkref_arm(box)), (lambda: self.clkref_read(box, cyc, cpu))
        if tag == "RPT lenient":
            return None, (lambda: setattr(self, "lenient", True))
        if tag == "RPT strict":
            return None, (lambda: setattr(self, "lenient", False))
        raise BenchAbort(f"unknown AT point '{tag}'")

    def f334_cmd(self, cmd: list[str]) -> None:
        c = cmd[0].upper()
        if c == "INIT" and len(cmd) == 1:
            self.f334.command("INIT", r"^OK INIT")
            self.armed = {}
        elif c in ("NACK", "CORRUPT") and len(cmd) == 2:
            self.f334.command(f"{c} {int(cmd[1])}", rf"^OK {c} ")
            self.armed[c.lower()] = int(cmd[1])
        elif c == "STRETCH" and len(cmd) == 3 and cmd[1] in ("ADDR", "BYTE"):
            self.f334.command(f"STRETCH {cmd[1]} {int(cmd[2])}", r"^OK STRETCH ")
            self.armed["stretch"] = True
        elif c == "FMP" and len(cmd) == 2 and cmd[1] in ("0", "1"):
            if cmd[1] == "1" and self.state == "taps":
                raise BenchAbort("S19: FMP 1 (Fm+, 1 MHz) asked with taps")              # m27
            self.f334.command(f"FMP {cmd[1]}", rf"^OK FMP {cmd[1]}")
        elif c == "STATUS" and len(cmd) == 2 and cmd[1].startswith("t12="):
            m = self.f334.command("STATUS", r"^STATUS xfers=(\d+) rx=(\d+) tx=(\d+) nk=(\d+) ev=(\d+) idle_cuts=(\d+) "
                                            r"dropped=(\d+) uart_ovr=(\d+)")
            self.t12_status[int(cmd[1][4:])] = [int(v) for v in m.groups()]
        else:
            raise BenchAbort(f"F334 command not on the host's list: {' '.join(cmd)}")

    def hold_start(self, line: str, ms: int, edges: int) -> None:
        mark = len(self.f334.lines)
        reply = self.f334.hold(line, ms, edges)                  # S21 guard inside
        if not reply.startswith("OK"):
            raise BenchAbort(f"F334 refused the hold: {reply}")
        self.hold_pending = {"line": line, "ms": ms, "edges": edges, "mark": mark}

    def hold_end(self) -> None:
        h = self.hold_pending
        if h is None:
            raise BenchAbort("HOLD_END with no hold running")
        end = time.monotonic() + 3.0
        m = None
        while m is None and time.monotonic() < end:
            for t in self.f334.lines[h["mark"]:]:
                m = re.search(r"^HOLD_END line=(\w+) ms=(\d+) us=(\d+) edges=(\d+) blank=(\d+) edge1_us=(\S+) "
                              r"cause=(\w+) scl=(\d) sda=(\d)(.*)$", t) or m
            if m is None:
                time.sleep(0.05)
        self.hold_pending = None
        if m is None:
            raise BenchAbort(f"no HOLD_END for HOLD {h['line']} {h['ms']}")
        us, edges, blank, cause = int(m.group(3)), int(m.group(4)), int(m.group(5)), m.group(7)
        ok15, why15 = s15_report_ok(m.group(0))
        after = m.group(8) == "1" and m.group(9) == "1"
        if h["edges"]:
            ok = cause == "EDGES" and edges == h["edges"] and us < h["ms"] * 1000
            want = f"released at SCL rising edge {h['edges']} (cause EDGES)"
        else:
            ok = cause == "TIME" and h["ms"] * 1000 * 0.98 <= us <= h["ms"] * 1000 * 1.02 and \
                (h["line"] == "SCL" or edges == 0)
            want = f"released by time after {h['ms']} ms"
        # After an SCL hold the master under test is in the middle of its transfer
        # (T10(b)): the lines 1 ms after the release are recorded, not judged; the
        # bus at rest is checked at the end of the step (S17).
        ok = ok and (after or h["line"] == "SCL") and ok15
        self.log(f"[host] hold {h['line']} {h['ms']} ms edges {h['edges']}: held {us} us, SCL edges counted {edges} "
                 f"(blank {blank}), cause {cause}, scl={m.group(8)} sda={m.group(9)} after, pins S15 {why15}")
        self.group("F1 target holds (S21)", ok, f"HOLD {h['line']} {h['ms']} EDGES {h['edges']}: {want}; got cause "
                   f"{cause} after {us} us with {edges} edges, lines after {m.group(8)}{m.group(9)}")
        self.hold_log.append({"line": h["line"], "ms": h["ms"], "want": h["edges"], "edges": edges, "cause": cause})

    def clkref_arm(self, box: dict) -> None:
        nmax = self.ad2.digital_in_buffer_max()
        rate = nmax / 0.16                                       # the 100 ms pulse inside the window
        box["rate"], box["n"] = self.ad2.la_arm(rate, nmax, DIO_ACK, prefill=nmax // 16)

    def clkref_read(self, box: dict, cyc: int, cpu: int) -> None:
        cap = self.ad2.la_read(box["n"], 3.0)
        if cap is None:
            self.check("F1 CLKREF CPU clock against the AD2", False, "the logic analyzer did not trigger on MCU_ACK")
            return
        runs = ad2mod.la_segments(cap, 13, 1)
        full = [r for r in runs if r[0] > 0 and r[1] < len(cap)]
        if not full:
            self.check("F1 CLKREF CPU clock against the AD2", False, f"no complete MCU_ACK pulse in {len(cap)} samples")
            return
        w = (full[0][1] - full[0][0]) / box["rate"]
        nominal = cyc / cpu
        self.clk_ratio = w / nominal
        self.clk_measured = True
        self.check("F1 CLKREF CPU clock against the AD2", 0.95 <= self.clk_ratio <= 1.05,
                   f"MCU_ACK held {cyc} CYCCNT cycles: {w * 1e3:.4f} ms by the AD2 ({nominal * 1e3:.1f} ms at "
                   f"{cpu} Hz nominal), ratio {self.clk_ratio:.5f} (+-{100 / box['rate'] / nominal:.3f} %, one sample); "
                   f"the CPU and the I2C kernels run {(1 / self.clk_ratio - 1) * 100:+.2f} % from nominal (N1)")

    def f1_step(self, name: str) -> None:
        self.step(name)
        self.at_mark = self.con.mark()
        while True:
            m = self.con.expect(r"\[i2c\] AT (.+?)\s*$", 120.0, stop_on_abort=True)
            tag = m.group(1)
            self.settle(tag)
            if tag == f"{name} end":
                self.sync(pre=lambda: self.bus_rest(f"after {name}"))
                break
            pre, post = self.at_action(tag)
            self.sync(pre=pre)
            if post:
                post()
        self.flush_groups()

    def t9b_counts(self) -> None:
        """T9(b) (audit F1 part 2, m34): the pulses RecoverBus gave plus the SCL
        rising edges of the refused Write must be the target's EDGES count (5), with
        at least one pulse when SDA was still low after the Write. The Write's edges
        are measured on the scope with taps; without taps they are 5 - pulses, and an
        ERR_ARLO needs at least one (arbitration is lost on an SCL high)."""
        line = next((t for t in reversed(self.con.since(0)) if "T9(b) the target holds SDA" in t), "")
        m = re.search(r"Write (\d+) in \d+ cycles .*?PB8/PB9 after it (\d+) .*?RecoverBus (\d+), pulses (\d+)", line)
        hold = next((h for h in reversed(self.hold_log) if h["want"] == 5), None)
        if not m or hold is None:
            self.check("F1 T9(b) pulses + the Write's SCL edges = the target's EDGES (m34)", False,
                       f"T9(b) line {'found' if m else 'missing'}, EDGES hold {'found' if hold else 'missing'}")
            return
        sw, lv0, pulses = int(m.group(1)), int(m.group(2)), int(m.group(4))
        sda_low = (lv0 & 2) == 0
        ok = hold["cause"] == "EDGES" and hold["edges"] == 5 and (lv0 & 1) == 1 and (pulses >= 1) == sda_low
        if self.state == "taps":
            if self.t9b_scope is None:
                ok, w_txt = False, "no T9(b) capture"
            else:
                w, rec = self.t9b_scope
                ok = ok and w + pulses == hold["edges"] and rec == pulses
                w_txt = f"the scope: {w} rises of the Write, then {rec} of RecoverBus"
        else:
            w = hold["edges"] - pulses
            ok = ok and 0 <= w <= 9 and (sw != ST_ARLO or w >= 1)
            w_txt = f"without taps: the Write's edges = {hold['edges']} - {pulses} = {w} (ERR_ARLO needs >= 1)"
        self.check("F1 T9(b) pulses + the Write's SCL edges = the target's EDGES (m34)", ok,
                   f"Write status {sw}, PB8/PB9 after it {lv0} (SDA {'low' if sda_low else 'high'}), RecoverBus "
                   f"pulses {pulses}; target: cause {hold['cause']}, edges {hold['edges']}; {w_txt}")

    # -- what is judged once the firmware has finished ------------------------------------
    def f1_t3(self) -> None:
        r = t3_judge(self.t3_lines)
        self.check("F1 T3 CalcTiming against the host's own rules", r["n"] >= 300 and r["bad"] == 0,
                   f"{r['n']} vectors (>= 300), {r['bad']} differ from the host's computation"
                   f"{(': ' + r['first']) if r['first'] else ''}")
        self.check("F1 T3 every TIMINGR meets RM0399", r["ok_n"] > 0 and r["rm_bad"] == 0,
                   f"{r['ok_n']} OK vectors checked on their fields (50:342, 50:319-331, table 407, 50:162-163, 2.19.1, "
                   f"model <= f): {r['rm_bad']} do not{(': ' + r['first_rm']) if r['first_rm'] else ''}")
        c = t3_judge(self.t3_lines, alter=True)
        self.check("F1 T3 control: the expected TIMINGR altered (SCLL + 1), the same comparison",
                   c["ok_n"] > 0 and c["bad"] == c["ok_n"],
                   f"{c['bad']} of the {c['ok_n']} OK vectors told apart (all must be; audit F1, m23)")

    def f1_t12(self) -> None:
        s = self.t12_status
        if not all(v in s for v in range(5)):
            self.check("F1 T12 target totals", False, f"STATUS before/after the 4 variants missing: {sorted(s)}")
            return
        names = ["driver poll", "driver DMA", "by hand poll", "by hand DMA"]
        parts = []
        ok = True
        for v in range(4):
            d = [b - a for a, b in zip(s[v], s[v + 1])]
            parts.append(f"{names[v]}: xfers +{d[0]} rx +{d[1]} nk +{d[3]} ev +{d[4]} cuts +{d[5]}")
            if v < 2:
                ok = ok and d[0] == 1000 and d[1] == 2000 and d[3] == 0 and d[4] == 0 and d[5] == 0
        self.check("F1 T12 target totals (driver: 1000 writes of 2 bytes each, nothing else)", ok, "; ".join(parts))

    def run(self) -> None:
        self.t0a()
        self.t0c()
        self.t0b()
        self.t0b_cm7()
        self.t0b_timeout()
        self.f1_init()
        self.f1_step("F1_NOPIN")
        state = None
        for text in self.con.since(0):
            m = re.search(r"bench state from PF14: (taps|notaps)", text)
            state = m.group(1) if m else state
        self.check("F1 bench state seen by the firmware (S19)", state == self.state,
                   f"CPU1 reads PF14 as '{state}', the host was told '{self.state}' (1 MHz only without taps)")
        if state != self.state:
            raise BenchAbort(f"S19: firmware state {state}, declared {self.state}")
        for text in self.con.since(0):
            m = re.search(r"CalcTiming bus t_r (\d+) ns t_f (\d+) ns", text)
            if m:
                self.bus_tr, self.bus_tf = int(m.group(1)), int(m.group(2))
        self.f1_step("F1_CM4")
        self.f1_step("F1_CM7")
        self.f1_step("F1_RECOVER")
        self.t9b_counts()
        # Phase F2 (i2c_ll): the same AT protocol, X lines named "<test>-LL".
        self.f1_step("F2_NOPIN")
        self.f1_step("F2_CM4")
        self.f1_step("F2_CM7")
        self.step("DONE")
        self.f1_t3()
        self.f1_t12()
        # m26: everything the selftest had to do, and nothing left half done.
        probs = manifest_problems(expected_transfers(self.state), self.x_seen)
        self.check("F1 transfers against the host's list (m26)", not probs,
                   f"{sum(self.x_seen.values())} X lines, {sum(expected_transfers(self.state).values())} expected"
                   + (f": {'; '.join(probs)}" if probs else ", all there and nothing else"))
        sprobs = manifest_problems(expected_scopes(self.state), self.scopes_seen)
        pend = [w for w, v in (("a scope capture", self.scope_pending), ("a hold", self.hold_pending)) if v is not None]
        self.check("F1 scope captures against the host's list, nothing pending (m26)", not sprobs and not pend,
                   f"{sum(self.scopes_seen.values())} captures, {sum(expected_scopes(self.state).values())} expected"
                   + (f": {'; '.join(sprobs)}" if sprobs else "") + (f"; still pending: {', '.join(pend)}" if pend else ""))
        # Not a counted check (audit F0, m12): step() aborts on any enable bit.
        self.record(f"No Digital Out: enable read back 0x0000 at all {len(self.dout_seen)} step boundaries of "
                    f"{len(STEPS)} (any other value aborts the session in step())")


ISO_DATE = re.compile(r"^\d{4}-\d{2}-\d{2}$")


def checklist_args_problem(operator: Optional[str], date: Optional[str]) -> Optional[str]:
    """Audit F0, m15: --checklist-confirmed and --checklist-date go together,
    the date in ISO form (YYYY-MM-DD) and a real day. The reason, or None."""
    if operator is None and date is None:
        return None
    if operator is None or not operator.strip():
        return "--checklist-date given without --checklist-confirmed NAME"
    if date is None:
        return "--checklist-confirmed needs --checklist-date YYYY-MM-DD (the day of the confirmation)"
    if not ISO_DATE.match(date):
        return f"--checklist-date {date!r} is not YYYY-MM-DD"
    try:
        datetime.date.fromisoformat(date)
    except ValueError:
        return f"--checklist-date {date!r} is not a calendar date"
    return None


def t0b_checklist_selftest(log: Callable[[str], None]) -> bool:
    cases = [(("Hector", "2026-10-07"), True, "name and date"), ((None, None), True, "neither (not evidence)"),
             (("Hector", None), False, "a name without a date"), ((None, "2026-10-07"), False, "a date alone"),
             (("Hector", "07/10/2026"), False, "not ISO"), (("Hector", "2026-02-30"), False, "no such day"),
             (("  ", "2026-10-07"), False, "an empty name")]
    ok = True
    for (who, day), want, why in cases:
        problem = checklist_args_problem(who, day)
        good = (problem is None) == want
        ok = ok and good
        log(f"[host] T0B(a) checklist arguments [{why}]: {'accepted' if problem is None else 'refused: ' + problem}"
            f" -> {'as expected' if good else 'WRONG'}")
    return ok


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--bench-state", required=True, choices=("taps", "notaps"),
                    help="J2/J3 in place (taps) or removed (notaps); checked against the scope (S19)")
    ap.add_argument("--out", type=pathlib.Path,
                    help="capture file (default: docs/i2c_f2_bench_<state>_<rev>.txt, i2c_dev_... with --allow-dirty)")
    ap.add_argument("--allow-dirty", action="store_true", help="accept a -dirty Rev (development runs)")
    ap.add_argument("--no-reset", action="store_true", help="do not reset the H755; wait for the banner")
    ap.add_argument("--preset", default="selftest-i2c", help="configure preset of the H755 image (build id)")
    ap.add_argument("--checklist-confirmed", metavar="NAME",
                    help="the person at the bench confirms the wiring and flashing items of the checklist")
    ap.add_argument("--checklist-date", metavar="YYYY-MM-DD",
                    help="date of that confirmation (required with --checklist-confirmed)")
    args = ap.parse_args()
    why = checklist_args_problem(args.checklist_confirmed, args.checklist_date)
    if why:
        ap.error(why)

    # The console may not encode every character a board sends (a reset leaves a
    # stray byte on the line): print what it can; the capture file keeps UTF-8.
    sys.stdout.reconfigure(errors="backslashreplace")
    lines: list[str] = []
    t0 = time.monotonic()
    lock = threading.Lock()

    def stamp(text: str) -> None:
        entry = f"[+{time.monotonic() - t0:8.3f}s] {text}"
        with lock:
            lines.append(entry)
        print(entry, flush=True)

    head = git_rev()
    res: dict = {"abort": None, "session": None, "verdict_fw": None, "revs": {}, "stack_free": None,
                 "guards": {}}
    all_ok = False
    # Audit F0, m11: whatever ends the session (an AD2 or programmer error, a
    # subprocess timeout, Ctrl+C), the summary, HOST VERDICT and the capture
    # are written. The AD2 is in S2 by then: run_bench() puts it there in its
    # finally, and closing it keeps it there (DwfParamOnClose = 1).
    try:
        session_body(args, stamp, head, res)
    except KeyboardInterrupt:
        res["abort"] = res["abort"] or "interrupted by the operator (Ctrl+C)"
        stamp(f"[host] ABORT: {res['abort']}")
    except Exception as e:                    # noqa: BLE001 - every error ends in a capture
        res["abort"] = res["abort"] or f"{type(e).__name__}: {e}"
        stamp(f"[host] ABORT: {res['abort']}")
    finally:
        all_ok = summary(args, stamp, res)
        out = args.out
        if out is None:
            rev = res["revs"].get("cm7", head)
            prefix = f"i2c_dev_f2_bench_{args.bench_state}" if args.allow_dirty else f"i2c_f2_bench_{args.bench_state}"
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


def summary(args, stamp, res: dict) -> bool:
    """The closing lines of a capture and the host verdict (always written).
    Only checks that can fail are counted (audit F0, m12); what is recorded
    but cannot fail (a mismatch aborts the session before it) is a RECORD."""
    session = res["session"]
    guards = res["guards"]
    checks = session.checks if session else []
    records = session.records if session else []
    for name, ok in guards.items():
        stamp(f"[host] T0B(a) {name}: {'PASS' if ok else 'FAIL'}")
    for name, ok in checks:
        stamp(f"[host] CHECK {name}: {'PASS' if ok else 'FAIL'}")
    for text in records:
        stamp(f"[host] RECORD {text}")
    all_ok = (bool(guards) and all(guards.values()) and res["abort"] is None and res["verdict_fw"] == "PASS" and
              bool(checks) and all(ok for _, ok in checks))
    if not args.checklist_confirmed:
        stamp("[host] the wiring checklist was not confirmed by an operator: this capture is not evidence")
    stamp(f"[host] {sum(1 for _, ok in checks if ok)}/{len(checks)} host checks passed ({len(records)} records not "
          f"counted), FW VERDICT={res['verdict_fw']}, stack free {res['stack_free']}, abort={res['abort']}")
    stamp(f"HOST VERDICT={'PASS' if all_ok else 'FAIL'}")
    return all_ok


def session_body(args, stamp, head: str, res: dict) -> None:
    stamp(f"[host] argv: {shlex.join(sys.argv[1:])}")
    stamp(f"[host] options: bench_state={args.bench_state} allow_dirty={args.allow_dirty} preset={args.preset} "
          f"no_reset={args.no_reset} checklist_confirmed={args.checklist_confirmed} "
          f"checklist_date={args.checklist_date}")
    stamp(f"[host] i2c_hw_check.py, host checkout {head}, protocol v{BENCH_VERSION}, F334 protocol "
          f"v{F334_PROTOCOL}, phase F2 (with the F0 and F1 steps)")
    procs_ok = checklist(stamp, args.checklist_confirmed, args.checklist_date, args.bench_state)
    tree_record(stamp)
    h755_id = host_build_id(args.preset)
    f334_host_id = host_build_id(F334_PRESET, F334_DIR)
    stamp(f"[host] build id of this checkout: build/{args.preset} {h755_id}; "
          f"tools/i2c_target_f334/build/{F334_PRESET} {f334_host_id}")

    guards = res["guards"]
    guards.update({"S3 (00_gpio)": t0b_guard_selftest(stamp), "HOST_REQ only": t0b_oe_selftest(stamp),
                   "S15 F334 report": t0b_s15_selftest(stamp), "S17 levels": t0b_s17_selftest(stamp),
                   "S21 hold": t0b_s21_selftest(stamp), "S18 probes": t0b_s18_selftest(stamp),
                   "T0d identity": t0b_t0d_selftest(stamp), "checklist arguments": t0b_checklist_selftest(stamp),
                   "T3 calculation rules": t0b_calc_selftest(stamp), "X/RPT pairing": t0b_pairing_selftest(stamp),
                   "I2C decoder": t0b_decoder_selftest(stamp), "m26 lists": t0b_manifest_selftest(stamp),
                   "S19 aborts": t0b_s19_selftest(stamp), "T5 minimums": t0b_t5min_selftest(stamp),
                   "T10(b) n x TIMEOUTA": t0b_t10b_selftest(stamp)})
    for name, ok in guards.items():
        stamp(f"[host] T0B(a) {name} guard on its mock: {'PASS' if ok else 'FAIL'}")

    cli = cube_programmer()
    probes, raw = list_probes(cli)
    for p in probes:
        stamp(f"[host] S18 probe: ST-LINK SN {p['sn']} FW {p['fw']} board {p['board']}")
    ok18, why18 = s18_probes_ok(probes)
    stamp(f"[host] S18 {why18}")
    ports = monitor.stlink_ports_by_sn()
    h755_port, f334_port = ports.get(H755_SN), ports.get(F334_SN)
    stamp(f"[host] S18 ports by ST-LINK SN: H755 {H755_SN} -> {h755_port}, F334 {F334_SN} -> {f334_port}")

    abort: Optional[str] = None
    if not all(guards.values()):
        abort = "a host guard failed on its mock: nothing is opened"
    elif not procs_ok:
        abort = "WaveForms GUI or a debugger is running (or tasklist failed): nothing is opened"
    elif not ok18:
        abort = f"S18: {why18}"
    elif not h755_port or not f334_port:
        abort = "S18: a COM port could not be found by ST-LINK serial number"
    if abort:
        res["abort"] = abort
        stamp(f"[host] ABORT: {abort}")
        return
    try:
        with RwSerialPort(f334_port, 115200) as fser:
            f334 = F334(fser, stamp)
            try:
                run_bench(args, stamp, cli, f334, h755_port, probes, head, h755_id, f334_host_id, res)
            finally:
                f334.close()
    except (OSError, BenchAbort) as e:
        res["abort"] = res["abort"] or str(e)
        stamp(f"[host] ABORT: {res['abort']}")


def run_bench(args, stamp, cli, f334: F334, h755_port: str, probes, head, h755_id, f334_host_id,
              res: dict) -> None:
    """F334 identity first (S20), then the AD2 and the H755: one session. The
    results go into `res` as they come, so an exception keeps what was done."""
    revs = res["revs"]
    b = f334.banner()
    ok15, why15 = s15_report_ok(b["pins"])
    stamp(f"[host] F334 banner: protocol v{b['proto']} Rev {b['rev']} id {b['id']} addr 0x{b['addr']:02X} sysclk "
          f"{b['sysclk']} IDCODE 0x{b['idcode']:08X} UID {b['uid']} {b['ready']}; pins: S15 {why15}")
    # Audit F0, m7: the target's TIMINGR is right for a 64 MHz kernel only; a
    # banner with the 8 MHz HSI (the PLL did not lock) ends the session.
    if (not ok15 or b["ready"] != "ready" or b["addr"] != 0x42 or (b["idcode"] & 0xFFF) != 0x438 or
            b["sysclk"] != F334_SYSCLK):
        raise BenchAbort(f"F334: S15 {why15}, state '{b['ready']}', address 0x{b['addr']:02X}, "
                         f"DEV_ID 0x{b['idcode'] & 0xFFF:03X} (0x438), sysclk {b['sysclk']} ({F334_SYSCLK})")
    for cmd, rx in (("CLR", r"^OK CLR"), ("INIT", r"^OK INIT"), ("DISARM", r"^OK DISARM"),
                    ("FMP 0", r"^OK FMP 0"), ("STATUS", r"^STATUS ")):
        f334.command(cmd, rx)

    with AD2() as ad2, SerialPort(h755_port, 115200) as ser:
        en, zet = ad2.dout_state()
        stamp(f"[ad2] opened in S2: OE=0x{ad2.get_oe():04X} DIO=0x{ad2.digital_io_read():04X} "
              f"Digital Out enable=0x{en:04X} Zet=0x{zet:04X}")
        d1 = ad2.analog_in_average(SCOPE_SCL, 2000, 100e3, 10.0)
        d2 = ad2.analog_in_average(SCOPE_SDA, 2000, 100e3, 10.0)
        stamp(f"[ad2] first acquisition after opening, DISCARDED (bench lesson): 1+ {d1:.3f} V, 2+ {d2:.3f} V")
        con = H755Console(ser, stamp)
        try:
            if en != 0:
                raise BenchAbort(f"Digital Out enable 0x{en:04X} after open")
            if not args.no_reset:
                cmd = [cli, "-c", "port=SWD", "mode=HOTPLUG", f"sn={H755_SN}", "-rst"]
                stamp(f"[host] reset the H755 through SWD ({' '.join(cmd[1:])}), AD2 in S2, the F334 idle; "
                      f"a system reset, not a power cycle")
                subprocess.run(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, check=True, timeout=60)
            else:
                stamp("[host] --no-reset: the H755 is not reset by this script; waiting for its banner")
            revs["cm7"] = con.expect(r"\[i2c\] CM7 Rev (\S+)", 20.0).group(1)
            ids = {"cm7": con.expect(r"\[i2c\] CM7 Build id (\S+)", 10.0).group(1)}
            revs["cm4"] = con.expect(r"\[i2c\] CM4 Rev (\S+)", 10.0).group(1)
            ids["cm4"] = con.expect(r"\[i2c\] CM4 Build id (.+?)\s*$", 10.0).group(1)
            ver = int(con.expect(r"\[i2c\] I2C_BENCH v(\d+) idle", 10.0).group(1))
            if ver != BENCH_VERSION:
                raise BenchAbort(f"S8: firmware protocol v{ver}, host v{BENCH_VERSION}")
            idt = {"head": head, "h755_id": h755_id, "f334_host_id": f334_host_id, "cm7_rev": revs["cm7"],
                   "cm4_rev": revs["cm4"], "cm7_id": ids["cm7"], "cm4_id": ids["cm4"], "f334_rev": b["rev"],
                   "f334_id": b["id"], "f334_proto": b["proto"], "probes": probes, "h755_port": h755_port,
                   "f334_port": f334.wport.name}
            stamp(f"[host] T0d identity: host HEAD {head}; CM7 Rev {revs['cm7']} id {ids['cm7']}; CM4 Rev "
                  f"{revs['cm4']} id {ids['cm4']}; host id {h755_id}; F334 Rev {b['rev']} id {b['id']} (host id "
                  f"{f334_host_id}); probes H755 {H755_SN} on {h755_port}, F334 {F334_SN} on {f334.wport.name}")
            problems = identity_problems(idt, args.allow_dirty)
            if problems:
                raise BenchAbort("T0d: " + "; ".join(problems))
            stamp("[host] T0d identity: all match")
            session = Session(ad2, con, f334, args.bench_state, stamp)
            res["session"] = session
            # Not a counted check (audit F0, m12): a mismatch aborted above.
            session.record("T0d identity (S18, A21): Rev and build id of CM7, CM4 and the F334 match this checkout "
                           "and the host's ids; exactly the two expected ST-LINKs; ports chosen by their SN")
            session.set_oe(DIO_REQ)          # S8: only now, after the banner of the right image
            session.req(False)
            session.run()
            res["verdict_fw"] = con.expect(r"\[i2c\] FW VERDICT=(\w+)", 30.0).group(1)
            stack_free = None
            for text in con.since(0):
                m = re.search(r"Status_Task stack high-water mark (\d+) words", text)
                if m:
                    stack_free = int(m.group(1))
            res["stack_free"] = stack_free
            session.check("Status_Task stack", stack_free is not None and stack_free >= STACK_FREE_MIN,
                          f"high-water mark {stack_free} words free (>= {STACK_FREE_MIN})")
            lv, raw = f334.levels()
            st = f334.command("STATUS", r"^STATUS xfers=(\d+) rx=(\d+) tx=(\d+) nk=(\d+) ev=(\d+) idle_cuts=(\d+) "
                                        r"dropped=(\d+) uart_ovr=(\d+)")
            counts = [int(x) for x in st.groups()]
            session.check("F334 at the end", lv["scl"] == 1 and lv["sda"] == 1 and s15_report_ok(raw)[0] and
                          counts[7] == 0 and not f334.unexpected,
                          f"scl={lv['scl']} sda={lv['sda']}, S15 {s15_report_ok(raw)[1]}; xfers/rx/tx/nk/ev/idle/"
                          f"dropped/uart_ovr {counts} (uart_ovr 0; reports dropped only in T12's flood); unexpected "
                          f"lines {f334.unexpected}")
        except (BenchAbort, AD2SafetyViolation, ad2mod.AD2Error, subprocess.CalledProcessError,
                subprocess.TimeoutExpired, OSError) as e:
            res["abort"] = f"{type(e).__name__}: {e}" if not isinstance(e, BenchAbort) else str(e)
            ad2.set_safe_state()
            stamp(f"[ad2] ABORT: {res['abort']}; AD2 back in S2 (OE=0x{ad2.get_oe():04X})")
            try:
                res["verdict_fw"] = con.expect(r"\[i2c\] FW VERDICT=(\w+)", 60.0).group(1)
            except BenchAbort:
                pass
        finally:
            ad2.set_safe_state()
            time.sleep(0.3)
            con.close()
        en, zet = ad2.dout_state()
        stamp(f"[ad2] closing in S2: OE=0x{ad2.get_oe():04X} Digital Out enable=0x{en:04X} Zet=0x{zet:04X}")


if __name__ == "__main__":
    sys.exit(main())
