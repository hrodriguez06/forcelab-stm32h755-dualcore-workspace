#!/usr/bin/env python3
"""Host side of the 05_i2c selftest on prj_01 (preset selftest-i2c), phase F0.

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
HOST_REQ-only wrapper, S15 on the F334 pin report, S18 probe check, S21 hold
guard, the T0d identity check); T0d identity of the two probes, CM7, CM4 and
the F334; T0a (AD2 lines); T0c (bus levels, S17/S19, and the F334's holds seen
by CPU1, the F334 and, with taps, the scope); T0b (claims on both cores, judged
by the firmware; the host reads the bus with the pins in AF); T0b(d) timeout.

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
  S19 --bench-state taps|notaps is required and checked: with taps the scope
      sees the bus (SCL 3.16 +- 0.10 V), without them PF14 reads < 0.1 V.
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
import os
import pathlib
import queue
import re
import shlex
import subprocess
import sys
import threading
import time
from ctypes import wintypes
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

BENCH_VERSION = 1          # I2C_BENCH_VERSION of Common/Inc/i2c_selftest.h
F334_PROTOCOL = 1          # TARGET_PROTOCOL of tools/i2c_target_f334/src/bench.h

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
         ("T0B_TIMEOUT", "NONE"), ("DONE", "NONE")]

# ---- Levels (S17, S19), plan "Reglas nuevas" ---------------------------------------------
SCL_TAPS_V = (3.06, 3.26)      # 3.16 +- 0.10 V: 1 kOhm to 3V3 against 22 kOhm to GND
SDA_MIN_V = 3.20
NOTAPS_SCL_MAX_V = 0.10        # PF14 alone: its 22 kOhm to GND
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


PIN_RX = re.compile(r"(PB8|PB9) MODER=(\d+) OTYPER=(\d+) PUPDR=(\d+) AFR=(\d+)")


def s15_report_ok(line: str) -> tuple[bool, str]:
    """S15 on the F334's own report of PB8/PB9: each pin input without pull,
    or AF4 open-drain without pull, and the firmware's own "s15=ok"."""
    pins = {m.group(1): tuple(int(x) for x in m.groups()[1:]) for m in PIN_RX.finditer(line)}
    bad = []
    for name in ("PB8", "PB9"):
        if name not in pins:
            bad.append(f"{name} missing")
            continue
        moder, otyper, pupdr, afr = pins[name]
        if pupdr != 0:
            bad.append(f"{name} pull {pupdr}")
        elif moder == 0:
            continue
        elif not (moder == 2 and otyper == 1 and afr == 4):
            bad.append(f"{name} MODER={moder} OTYPER={otyper} AFR={afr}")
    if "s15=ok" not in line:
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

    def expect(self, pattern: str, timeout_s: float) -> re.Match:
        rx = re.compile(pattern)
        end = time.monotonic() + timeout_s
        while True:
            left = end - time.monotonic()
            if left <= 0:
                raise BenchAbort(f"{self._prefix.strip()} timeout waiting for /{pattern}/")
            self.check_abort()
            try:
                line = self._q.get(timeout=min(left, 0.2))
            except queue.Empty:
                continue
            m = rx.search(line)
            if m:
                return m

    def check_abort(self) -> None:
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

    def check_abort(self) -> None:
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


# ---- The session ---------------------------------------------------------------------------

class Session:
    def __init__(self, ad2: AD2, con: H755Console, f334: F334, state: str, log: Callable[[str], None]) -> None:
        self.ad2 = ad2
        self.con = con
        self.f334 = f334
        self.state = state
        self.log = log
        self.checks: list[tuple[str, bool]] = []
        self.step_idx = 0
        self.dout_seen: list[tuple[str, int]] = []

    def check(self, name: str, ok: bool, detail: str) -> None:
        self.checks.append((name, ok))
        self.log(f"[host] {name}: {detail} -> {'PASS' if ok else 'FAIL'}")

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
        if self.state == "taps":
            scope_ok = SCL_TAPS_V[0] <= v_scl <= SCL_TAPS_V[1] and v_sda >= SDA_MIN_V
            s19_ok = v_scl > NOTAPS_SCL_MAX_V
        else:
            scope_ok = v_scl < NOTAPS_SCL_MAX_V and v_sda >= SDA_MIN_V
            s19_ok = scope_ok
        f_ok = lv["scl"] == 1 and lv["sda"] == 1 and lv["busy"] == 0 and ok15
        m_ok = True
        mtxt = "-"
        if mcu is not None:
            want_pf14 = 1 if self.state == "taps" else 0
            m_ok = (mcu["pb8"], mcu["pb9"], mcu["pf14"], mcu["pf15"]) == (1, 1, want_pf14, 1)
            mtxt = f"pb8={mcu['pb8']} pb9={mcu['pb9']} pf14={mcu['pf14']} (want {want_pf14}) pf15={mcu['pf15']}"
        ok = scope_ok and f_ok and m_ok
        self.check(f"S17 {label}",
                   ok, f"scope SCL(1+) {v_scl:.3f} V SDA(2+) {v_sda:.3f} V "
                   f"({'taps: SCL 3.16+-0.10, SDA >= 3.20' if self.state == 'taps' else 'notaps: 1+ < 0.10 V, 2+ >= 3.20'}); "
                   f"F334 scl={lv['scl']} sda={lv['sda']} busy={lv['busy']} S15 {why15}; CM7 {mtxt}")
        if not s19_ok:
            raise BenchAbort(f"S19: declared '{self.state}', scope 1+ reads {v_scl:.3f} V")
        if not ok:
            raise BenchAbort(f"S17: bus not at rest at '{label}'")

    # -- S7, S2 at every step boundary, S17 before the step
    def step(self, name: str) -> None:
        m = self.con.expect(r"\[i2c\] STEP (\S+) owner=(\S+) v=(\d+) pb8=(\d) pb9=(\d) pf14=(\d) pf15=(\d)", 120.0)
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
        m = self.con.expect(r"\[i2c\] T0B levels with CM4 AF4 pb8=(\d) pb9=(\d)", 60.0)
        self.sync(pre=lambda: self.bus_rest(f"T0B with PB8/PB9 in AF4 on CM4, I2C1 PE=0 (CM7 reads "
                                            f"pb8={m.group(1)} pb9={m.group(2)})"))
        self.sync(pre=lambda: self.bus_rest("T0B after CM4's bench_safe"))

    def t0b_cm7(self) -> None:
        self.step("T0B_CM7")
        m = self.con.expect(r"\[i2c\] T0B levels with CM7 AF6 pb8=(\d) pb9=(\d)", 60.0)
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

    def run(self) -> None:
        self.t0a()
        self.t0c()
        self.t0b()
        self.t0b_cm7()
        self.t0b_timeout()
        self.step("DONE")
        bad = [(s, f"0x{en:04X}") for s, en in self.dout_seen if en != 0]
        self.check("No Digital Out", not bad and len(self.dout_seen) == len(STEPS),
                   f"Digital Out enable read back at {len(self.dout_seen)} step boundaries of {len(STEPS)}: "
                   f"all 0x0000 {not bad} {bad}")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--bench-state", required=True, choices=("taps", "notaps"),
                    help="J2/J3 in place (taps) or removed (notaps); checked against the scope (S19)")
    ap.add_argument("--out", type=pathlib.Path,
                    help="capture file (default: docs/i2c_f0_bench_<state>_<rev>.txt, i2c_dev_... with --allow-dirty)")
    ap.add_argument("--allow-dirty", action="store_true", help="accept a -dirty Rev (development runs)")
    ap.add_argument("--no-reset", action="store_true", help="do not reset the H755; wait for the banner")
    ap.add_argument("--preset", default="selftest-i2c", help="configure preset of the H755 image (build id)")
    ap.add_argument("--checklist-confirmed", metavar="NAME",
                    help="the person at the bench confirms the wiring and flashing items of the checklist")
    ap.add_argument("--checklist-date", metavar="YYYY-MM-DD", help="date of that confirmation")
    args = ap.parse_args()

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

    stamp(f"[host] argv: {shlex.join(sys.argv[1:])}")
    stamp(f"[host] options: bench_state={args.bench_state} allow_dirty={args.allow_dirty} preset={args.preset} "
          f"no_reset={args.no_reset} checklist_confirmed={args.checklist_confirmed} "
          f"checklist_date={args.checklist_date}")
    head = git_rev()
    stamp(f"[host] i2c_hw_check.py, host checkout {head}, protocol v{BENCH_VERSION}, F334 protocol "
          f"v{F334_PROTOCOL}, phase F0")
    procs_ok = checklist(stamp, args.checklist_confirmed, args.checklist_date, args.bench_state)
    tree_record(stamp)
    h755_id = host_build_id(args.preset)
    f334_host_id = host_build_id(F334_PRESET, F334_DIR)
    stamp(f"[host] build id of this checkout: build/{args.preset} {h755_id}; "
          f"tools/i2c_target_f334/build/{F334_PRESET} {f334_host_id}")

    guards = {"S3 (00_gpio)": t0b_guard_selftest(stamp), "HOST_REQ only": t0b_oe_selftest(stamp),
              "S15 F334 report": t0b_s15_selftest(stamp), "S21 hold": t0b_s21_selftest(stamp),
              "S18 probes": t0b_s18_selftest(stamp), "T0d identity": t0b_t0d_selftest(stamp)}
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
    session: Optional[Session] = None
    verdict_fw = None
    revs: dict[str, str] = {}
    stack_free = None
    if not all(guards.values()):
        abort = "a host guard failed on its mock: nothing is opened"
    elif not procs_ok:
        abort = "WaveForms GUI or a debugger is running (or tasklist failed): nothing is opened"
    elif not ok18:
        abort = f"S18: {why18}"
    elif not h755_port or not f334_port:
        abort = "S18: a COM port could not be found by ST-LINK serial number"
    if abort:
        stamp(f"[host] ABORT: {abort}")
    else:
        try:
            with RwSerialPort(f334_port, 115200) as fser:
                f334 = F334(fser, stamp)
                try:
                    abort, session, verdict_fw, revs, stack_free = run_bench(
                        args, stamp, cli, f334, h755_port, probes, head, h755_id, f334_host_id)
                finally:
                    f334.close()
        except (OSError, BenchAbort) as e:
            abort = abort or str(e)
            stamp(f"[host] ABORT: {abort}")

    checks = session.checks if session else []
    for name, ok in guards.items():
        stamp(f"[host] T0B(a) {name}: {'PASS' if ok else 'FAIL'}")
    for name, ok in checks:
        stamp(f"[host] CHECK {name}: {'PASS' if ok else 'FAIL'}")
    all_ok = (all(guards.values()) and abort is None and verdict_fw == "PASS" and bool(checks) and
              all(ok for _, ok in checks))
    if not args.checklist_confirmed:
        stamp("[host] the wiring checklist was not confirmed by an operator: this capture is not evidence")
    stamp(f"[host] {sum(1 for _, ok in checks if ok)}/{len(checks)} host checks passed, FW VERDICT={verdict_fw}, "
          f"stack free {stack_free}, abort={abort}")
    stamp(f"HOST VERDICT={'PASS' if all_ok else 'FAIL'}")

    out = args.out
    if out is None:
        rev = revs.get("cm7", head)
        prefix = f"i2c_dev_f0_bench_{args.bench_state}" if args.allow_dirty else f"i2c_f0_bench_{args.bench_state}"
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


def run_bench(args, stamp, cli, f334: F334, h755_port: str, probes, head, h755_id, f334_host_id):
    """F334 identity first (S20), then the AD2 and the H755: one session."""
    abort: Optional[str] = None
    session: Optional[Session] = None
    verdict_fw = None
    revs: dict[str, str] = {}
    stack_free = None

    b = f334.banner()
    ok15, why15 = s15_report_ok(b["pins"])
    stamp(f"[host] F334 banner: protocol v{b['proto']} Rev {b['rev']} id {b['id']} addr 0x{b['addr']:02X} sysclk "
          f"{b['sysclk']} IDCODE 0x{b['idcode']:08X} UID {b['uid']} {b['ready']}; pins: S15 {why15}")
    if not ok15 or b["ready"] != "ready" or b["addr"] != 0x42 or (b["idcode"] & 0xFFF) != 0x438:
        raise BenchAbort(f"F334: S15 {why15}, state '{b['ready']}', address 0x{b['addr']:02X}, "
                         f"DEV_ID 0x{b['idcode'] & 0xFFF:03X} (0x438)")
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
            session.check("T0d identity (S18, A21)", True,
                          "Rev and build id of CM7, CM4 and the F334 match this checkout and the host's ids; exactly "
                          "the two expected ST-LINKs; ports chosen by their SN")
            session.set_oe(DIO_REQ)          # S8: only now, after the banner of the right image
            session.req(False)
            session.run()
            verdict_fw = con.expect(r"\[i2c\] FW VERDICT=(\w+)", 30.0).group(1)
            for text in con.since(0):
                m = re.search(r"Status_Task stack high-water mark (\d+) words", text)
                if m:
                    stack_free = int(m.group(1))
            session.check("Status_Task stack", stack_free is not None and stack_free >= STACK_FREE_MIN,
                          f"high-water mark {stack_free} words free (>= {STACK_FREE_MIN})")
            lv, raw = f334.levels()
            st = f334.command("STATUS", r"^STATUS xfers=(\d+) rx=(\d+) tx=(\d+) nk=(\d+) ev=(\d+) idle_cuts=(\d+) "
                                        r"dropped=(\d+) uart_ovr=(\d+)")
            counts = [int(x) for x in st.groups()]
            session.check("F334 at the end", lv["scl"] == 1 and lv["sda"] == 1 and s15_report_ok(raw)[0] and
                          counts == [0] * 8 and not f334.unexpected,
                          f"scl={lv['scl']} sda={lv['sda']}, S15 {s15_report_ok(raw)[1]}; xfers/rx/tx/nk/ev/idle/"
                          f"dropped/uart_ovr {counts} (all 0: in F0 nothing addresses it); unexpected lines "
                          f"{f334.unexpected}")
        except (BenchAbort, AD2SafetyViolation, ad2mod.AD2Error, subprocess.CalledProcessError,
                subprocess.TimeoutExpired, OSError) as e:
            abort = str(e)
            ad2.set_safe_state()
            stamp(f"[ad2] ABORT: {abort}; AD2 back in S2 (OE=0x{ad2.get_oe():04X})")
            try:
                verdict_fw = con.expect(r"\[i2c\] FW VERDICT=(\w+)", 60.0).group(1)
            except BenchAbort:
                pass
        finally:
            ad2.set_safe_state()
            time.sleep(0.3)
            con.close()
        en, zet = ad2.dout_state()
        stamp(f"[ad2] closing in S2: OE=0x{ad2.get_oe():04X} Digital Out enable=0x{en:04X} Zet=0x{zet:04X}")
    return abort, session, verdict_fw, revs, stack_free


if __name__ == "__main__":
    sys.exit(main())
