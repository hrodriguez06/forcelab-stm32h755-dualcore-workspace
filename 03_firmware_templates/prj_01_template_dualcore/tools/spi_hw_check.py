#!/usr/bin/env python3
"""Host side of the 06_spi selftest on prj_01 (preset selftest-spi), phase F0.

Runs one complete bench session against CM7/App/Src/spi_selftest.c with the
Analog Discovery 2 and writes ONE capture file: every console line and every
host measurement, with timestamps, from "[host] argv:" to "HOST VERDICT=".
Plan and meaning of every test: 01_shared_drivers/06_spi/IMPLEMENTATION_PLAN.md;
wiring and rules S1-S10: 01_shared_drivers/00_gpio/README.md section 8.

    py tools/flash.py --preset selftest-spi            # with this script stopped
    py tools/spi_hw_check.py --checklist-confirmed NAME   # resets the board through SWD and runs
    py tools/spi_hw_check.py --allow-dirty ...            # development only (capture spi_dev_...)

F0 tests: T0a (loose lines, walking 1, control), T0c (the J1 jumper, run by
CPU1; the host reads its summary), T0b(a) on mock objects before the AD2 is
opened, then T0b(b)(c)(d)(e)(f) on the bench.

Image identity (00_gpio README 9.4, R3-a/R3-b/R4-a), checked before any output
is enabled: Rev(CM7) == Rev(CM4), id(CM7) == id(CM4) (never "unknown"), each
Rev without -dirty == git rev-parse --short HEAD; without --allow-dirty also no
-dirty Rev and the image id == the id the host computes (host_build_id() of
gpio_hw_check.py: one definition of the method).

Electrical safety (no series resistors on the bench; plan S1-S14):
  S2  the AD2 opens and closes in S2: Digital-IO all input and Digital Out
      reset, stopped, every channel disabled and idle Zet (tools/ad2.py);
      again at every step boundary and on any exception (try/finally).
  S3  every OutputEnable change goes through set_oe() below, which accepts
      HOST_REQ and nothing else, and then through ad2.ad2_set_oe() with the
      bus owner of the CURRENT step; T0b(a) proves both guards on mocks first.
  S5  before either side drives the bus, both MCU pulls must be followed
      (s5_released); the same function is checked in T0b(c) and in the
      control of T0b(e), where it must fail.
  S6  1 ms or more between one side releasing and the other driving.
  S7  a step is acted on only if its announcement (name, bus owner, J1 owner,
      protocol version) matches STEPS below.
  S8  no output is enabled before the banner `SPI_BENCH v2 idle` of the
      right image has been read.
  No Digital Out (plan D6): this script never configures, enables or starts
      Digital Out; at every step boundary it reads the 16 Digital Out enables
      back and aborts unless all are 0.
  S13 and S14 are enforced by the firmware (bench_claim on both cores);
      T0b(e) runs the S5 check with PE11..PE14 already in AF5.
Standard library only (ctypes); `py`, not `python`.
"""

from __future__ import annotations

import argparse
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
from ad2 import (AD2, AD2SafetyViolation, GPIO_BENCH_BUS_MCU,  # noqa: E402
                 GPIO_BENCH_BUS_NONE)
from monitor import SerialPort, autodetect             # noqa: E402
# One definition of the image-identity method and of the tree record (00_gpio
# README 9.4), and the 00_gpio T0b(a) mock test of the S3 guard as it is.
from gpio_hw_check import (cube_programmer, git_rev, host_build_id,  # noqa: E402
                           t0b_guard_selftest, tree_record)

BENCH_VERSION = 2          # SPI_BENCH_VERSION of Common/Inc/spi_selftest.h (v2: audit B1)

# ---- Wiring (00_gpio README section 8; 06_spi plan "Banco de pruebas") -------------
DIO_BUS = 0x00FF        # DIO0..7  <- PE8..PE15
DIO_NSS = 1 << 3        # PE11 SPI4_NSS
DIO_SCK = 1 << 4        # PE12 SPI4_SCK
DIO_MISO = 1 << 5       # PE13 SPI4_MISO (input)
DIO_MOSI = 1 << 6       # PE14 SPI4_MOSI
DIO_REQ = 1 << 12       # HOST_REQ -> PE2
DIO_ACK = 1 << 13       # MCU_ACK  <- PE7
# T0a: the 13 lines the MCU pulls, in the firmware's order (k_lines[]).
T0A_DIO = [0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 14]
T0A_LINES = sum(1 << d for d in T0A_DIO)              # 0x4FFF
T0A_CTRL_DIO = 9                                       # pulled the wrong way by the firmware

BUS = {"NONE": GPIO_BENCH_BUS_NONE, "MCU": GPIO_BENCH_BUS_MCU}

# S7: the host's own copy of the firmware's step table (name, bus owner, J1 owner), in order.
STEPS = [
    ("T0A", "NONE", "NONE"), ("T0C", "NONE", "CM7"), ("T0B", "NONE", "CM4"), ("T0B_S5NEG", "MCU", "NONE"),
    ("T0B_TIMEOUT", "NONE", "NONE"), ("T0B_S13", "MCU", "NONE"), ("T0B_S14", "NONE", "CM7"),
    ("DONE", "NONE", "NONE"),
]

# The only Digital-IO output this bench ever enables (plan D6: the AD2 drives no
# other line). ad2.ad2_set_oe() would also accept DIO8, DIO10 and, in a "bus = AD2"
# step, the bus: the SPI bench has no use for them, so set_oe() refuses them first.
SPI_OE_ALLOWED = DIO_REQ

HS_TIMEOUT_S = 10.0
# T0b(d), protocol v2 (audit B1): the MCU's wait is SYNC_TIMEOUT_CYC = 4e9 cycles of
# CYCCNT (10 s at 400 MHz; about 9.87 s with the board's HSE at +1.3 %).
T0B_ACK_LOW_LIMIT_S = 15.0
T0B_TIMEOUT_MIN_S = 9.0
T0B_TIMEOUT_MAX_S = 11.0
T0B_LOW_READS = 20


class BenchAbort(Exception):
    pass


# ---- S3 for this bench: HOST_REQ only -------------------------------------------------

def spi_set_oe(ad2, mask: int, owner: int) -> None:
    """The only path of this script to an OutputEnable change. A bit outside
    HOST_REQ puts the AD2 in S2 and raises before ad2.ad2_set_oe() is called;
    otherwise ad2_set_oe() applies its own guard (S3) with the step's owner."""
    if mask & ~SPI_OE_ALLOWED:
        ad2.set_safe_state()
        raise AD2SafetyViolation(f"SPI bench: OE mask 0x{mask:04X} has bits outside HOST_REQ "
                                 f"(0x{mask & ~SPI_OE_ALLOWED:04X}); the AD2 is in S2")
    ad2.set_oe(mask, owner)


class _MockAd2:
    """Records the calls spi_set_oe() makes on an AD2 object."""

    def __init__(self) -> None:
        self.calls: list[tuple] = []

    def set_safe_state(self) -> None:
        self.calls.append(("S2",))

    def set_oe(self, mask: int, owner: int) -> None:
        self.calls.append(("set_oe", mask, owner))


def t0b_spi_oe_selftest(log: Callable[[str], None]) -> bool:
    """T0b(a), the SPI half: spi_set_oe() refuses every mask but HOST_REQ before
    the AD2 object sees it, and goes to S2; 0 and HOST_REQ pass."""
    forbidden = [(1 << 8, "NONE", "DIO8 (S3 would allow it)"), (1 << 10, "NONE", "DIO10 (S3 would allow it)"),
                 (DIO_BUS, "MCU", "the bus"), (DIO_SCK, "MCU", "DIO4 (SPI4 SCK)"), (1 << 9, "NONE", "DIO9"),
                 (1 << 14, "NONE", "DIO14 (PA8)"), (1 << 15, "NONE", "DIO15"),
                 (DIO_REQ | (1 << 8), "NONE", "HOST_REQ with DIO8")]
    ok = True
    for mask, owner, why in forbidden:
        m = _MockAd2()
        try:
            spi_set_oe(m, mask, BUS[owner])
            refused = False
        except AD2SafetyViolation:
            refused = True
        good = refused and m.calls == [("S2",)]
        ok = ok and good
        log(f"[ad2] T0B(a) spi_set_oe(0x{mask:04X}, bus={owner}) [{why}]: "
            f"{'refused, S2, nothing passed to ad2_set_oe' if good else 'NOT REFUSED CLEANLY'}")
    for mask in (0, DIO_REQ):                      # the controls
        m = _MockAd2()
        spi_set_oe(m, mask, GPIO_BENCH_BUS_NONE)
        good = m.calls == [("set_oe", mask, GPIO_BENCH_BUS_NONE)]
        ok = ok and good
        log(f"[ad2] T0B(a) control spi_set_oe(0x{mask:04X}, bus=NONE): "
            f"{'passed to ad2_set_oe' if good else 'BLOCKED'}")
    return ok


# ---- Procedure checklist (00_gpio README section 8, 06_spi plan "Banco de pruebas") ------

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
    stamp(f"[host] checklist [{mark}] GND first, then the DIO lines per 00_gpio README 8; no series resistors: {who}")
    stamp(f"[host] checklist [{mark}] J1 colocado (CN7-14 <-> CN7-12): {who}")
    stamp(f"[host] checklist [{mark}] nothing on W1, W2, V+, V-: {who}")
    stamp(f"[host] checklist [{mark}] both USB connected (AD2 and ST-LINK), AD2 on USB while the Nucleo is "
          f"powered: {who}")
    stamp(f"[host] checklist [{mark}] selftest-spi flashed with this script stopped, then a cold start: {who}")
    running = running_forbidden()
    if running is None:
        stamp("[host] checklist [ ] WaveForms GUI closed, no debugger attached: tasklist failed, not checked")
        return False
    stamp(f"[host] checklist [{'x' if not running else ' '}] WaveForms GUI closed, no debugger attached "
          f"(tasklist: {', '.join(running) if running else 'none of ' + ', '.join(FORBIDDEN_PROCESSES)})")
    return not running


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
                if "[spi] ABORT" in line:
                    self.fw_abort = line
                self._q.put((t, line))

    def expect(self, pattern: str, timeout_s: float, stop_on_abort: bool = False) -> re.Match:
        rx = re.compile(pattern)
        end = time.monotonic() + timeout_s
        while True:
            left = end - time.monotonic()
            if left <= 0:
                raise BenchAbort(f"timeout waiting for /{pattern}/")
            if stop_on_abort and self.fw_abort and self._q.empty():
                raise BenchAbort(f"firmware aborted: {self.fw_abort}")
            try:
                _, line = self._q.get(timeout=min(left, 0.2))
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
        self.step_name: Optional[str] = None
        self.checks: list[tuple[str, bool]] = []
        self.step_idx = 0
        self.dout_seen: list[tuple[str, int]] = []      # (step, Digital Out enable mask) at each boundary

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

    # -- S7, and S2 (with Digital Out read back) at every step boundary
    def step(self, name: str) -> None:
        m = self.con.expect(r"\[spi\] STEP (\S+) bus=(\S+) j1=(\S+) v=(\d+)", 120.0, stop_on_abort=True)
        got, bus, j1, ver = m.group(1), m.group(2), m.group(3), int(m.group(4))
        exp_name, exp_bus, exp_j1 = STEPS[self.step_idx]
        if (got, bus, j1, ver) != (name, exp_bus, exp_j1, BENCH_VERSION) or exp_name != name:
            raise BenchAbort(f"S7: announced {got} bus={bus} j1={j1} v={ver}, host table expects "
                             f"{exp_name} bus={exp_bus} j1={exp_j1} v={BENCH_VERSION}")
        self.step_idx += 1
        self.step_name = name
        self.ad2.set_safe_state()     # S2: Digital-IO input, Digital Out reset (ad2_dout_safe)
        en, zet = self.ad2.dout_state()
        self.dout_seen.append((name, en))
        if en != 0 or zet != 0xFFFF:
            raise BenchAbort(f"Digital Out at step {name}: enable 0x{en:04X}, Zet 0x{zet:04X} (0x0000, 0xFFFF)")
        self.owner = BUS[bus]
        self.set_oe(DIO_REQ)          # never more than HOST_REQ
        self.log(f"[ad2] step {name}: S2, Digital Out enable 0x{en:04X} Zet 0x{zet:04X}, OE 0x{self.ad2.get_oe():04X}")
        self.sync()

    def set_oe(self, mask: int) -> None:
        spi_set_oe(self.ad2, mask, self.owner)    # HOST_REQ only, then S3 with this step's owner
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

    def t0c(self) -> None:
        """T0c runs on CPU1 alone (no AD2 line reaches J1). The host checks the
        announcement, keeps the AD2 in S2 with HOST_REQ only, and reads the
        firmware's summary; the firmware aborts the session if T0c fails."""
        self.step("T0C")
        self.sync()
        m = self.con.expect(r"\[spi\] T0C summary (\w+)", 10.0)
        self.check("T0C J1 (firmware)", m.group(1) == "PASS",
                   f"CPU1's summary {m.group(1)}: control (PB5 input, PA6 pull up/down), 16 levels of PB5 on PA6, "
                   f"PB4/PA5 on their pull (lines '[spi] T0C ...' above)")

    def t0b(self) -> None:
        self.step("T0B")
        self.sync()                    # CPU1 and CPU2 claims, CPU2's bench_safe
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
        self.t0b_timeout()

    def t0b_timeout(self) -> None:
        """T0b(d), protocol v2 (audit B1). The MCU raises MCU_ACK and waits up to
        10 s for HOST_REQ, which the host never raises; it then goes to S2 (ACK
        low) and stays in the step, ACK low, until the host pulses HOST_REQ. The
        host waits for ACK low with a limit, measures the time ACK stayed high,
        reads ACK low again over T0B_LOW_READS reads while the MCU is held, and
        only then pulses HOST_REQ: nothing depends on when the console lines
        arrive. In v1 the host read ACK after the line `T0B timeout handled`,
        when the next step could already have raised it again."""
        self.step("T0B_TIMEOUT")
        self.wait_ack(True)
        t_high = time.monotonic()
        self.log("[ad2] T0B(d) MCU_ACK high: the host deliberately does not raise HOST_REQ")
        self.wait_ack(False, T0B_ACK_LOW_LIMIT_S)          # the MCU's timeout, then bench_safe()
        held_s = time.monotonic() - t_high
        reads = []
        for _ in range(T0B_LOW_READS):
            reads.append((self.ad2.digital_io_read() & DIO_ACK) == 0)
            time.sleep(0.005)
        self.con.expect(r"\[spi\] T0B timeout handled", 5.0)
        self.sync_low_pulse()                               # lets the MCU go on to the next step
        ok = all(reads) and T0B_TIMEOUT_MIN_S <= held_s <= T0B_TIMEOUT_MAX_S
        self.check("T0B(d) firmware timeout", ok,
                   f"MCU_ACK stayed high {held_s:.3f} s without HOST_REQ ({T0B_TIMEOUT_MIN_S}..{T0B_TIMEOUT_MAX_S} s; "
                   f"the MCU waits 4e9 cycles), then low in {sum(reads)}/{T0B_LOW_READS} reads over "
                   f"{T0B_LOW_READS * 5} ms while the MCU waited for the host; `T0B timeout handled` reported")

    def sync_low_pulse(self) -> None:
        """The host side of sync_ack_low(): HOST_REQ up, then down, with MCU_ACK
        low throughout (checked after each edge)."""
        self.req(True)
        time.sleep(0.005)
        hi_ok = (self.ad2.digital_io_read() & DIO_ACK) == 0
        self.req(False)
        if not hi_ok:
            raise BenchAbort("T0B(d): MCU_ACK went high during the HOST_REQ pulse")

    def t0b_s13(self) -> None:
        """T0b(e): S5 with PE11..PE14 in AF5 must pass; with AFCNTR = 1 (SPE = 0)
        the same s5_released() must fail, SCK (DIO4) held at CPOL."""
        self.step("T0B_S13")
        self.buschk("T0B_S13 before")
        self.buschk("T0B_S13 with PE11..PE14 in AF5, AFCNTR=0")
        self.check("T0B(e) S5 with PE11..PE14 in AF5", True,
                   "the bus followed both MCU pulls with SPI4 a master at rest, pins in AF5 (else BenchAbort)")
        seen = []
        for _ in range(4):                       # CPOL 0: up, down; CPOL 1: up, down
            k = len(seen)
            self.sync(pre=lambda kk=k: seen.append(self.s5_released(kk % 2 == 0)))
        others = DIO_BUS & ~(DIO_SCK | DIO_MOSI)
        rows = []
        good = True
        for k, (ok, dio, _) in enumerate(seen):
            cpol = 1 if k >= 2 else 0
            high = k % 2 == 0
            sck_ok = bool(dio & DIO_SCK) == bool(cpol)
            pull_ok = (dio & others) == (others if high else 0)
            good = good and sck_ok and pull_ok
            rows.append(f"CPOL={cpol} pull-{'up' if high else 'down'}: s5_released {ok} DIO=0x{dio:02X} "
                        f"SCK={int(bool(dio & DIO_SCK))} MOSI={int(bool(dio & DIO_MOSI))}")
        held = (not seen[0][0]) and (not seen[3][0])
        self.check("T0B(e) control AFCNTR=1", good and held,
                   "; ".join(rows) + " -- SCK at CPOL in all 4, the other lines but MOSI follow the pull, and "
                   "s5_released() fails at CPOL=0 pull-up and CPOL=1 pull-down: the check that guards every "
                   "capture would abort here")
        self.buschk("T0B_S13 after the Deinit, PE11..PE14 still in AF5")
        self.check("T0B(e) S5 after the Deinit", True, "AFCNTR = 0, CFG2 = 0: released again (else BenchAbort)")
        self.buschk("T0B_S13 after, pins in input")

    def t0b_s14(self) -> None:
        self.step("T0B_S14")
        self.sync()                    # CPU1 claims of J1 in AF8, CPU2 refused

    def run(self) -> None:
        self.t0a()
        self.t0c()
        self.t0b()
        self.t0b_s13()
        self.t0b_s14()
        self.step("DONE")
        bad = [(s, f"0x{en:04X}") for s, en in self.dout_seen if en != 0]
        self.check("No Digital Out", not bad and len(self.dout_seen) == len(STEPS),
                   f"Digital Out enable read back at {len(self.dout_seen)} step boundaries of {len(STEPS)}: "
                   f"all 0x0000 {not bad} {bad}")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", help="ST-LINK VCP (default: autodetect)")
    ap.add_argument("--out", type=pathlib.Path,
                    help="capture file (default: docs/spi_f0_bench_<rev>.txt, spi_dev_f0_bench_<rev>.txt with --allow-dirty)")
    ap.add_argument("--allow-dirty", action="store_true", help="accept a -dirty Rev (development runs)")
    ap.add_argument("--no-reset", action="store_true", help="do not reset the board; wait for the banner")
    ap.add_argument("--preset", default="selftest-spi", help="configure preset of the image (build id)")
    ap.add_argument("--checklist-confirmed", metavar="NAME",
                    help="the person at the bench confirms the wiring, J1 and flashing items of the checklist")
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
    stamp(f"[host] spi_hw_check.py, host checkout {git_rev()}, protocol v{BENCH_VERSION}, phase F0")
    procs_ok = checklist(stamp, args.checklist_confirmed)
    tree_record(stamp)
    build_id = host_build_id(args.preset)
    stamp(f"[host] build id of this checkout and build/{args.preset}: {build_id}")
    guard_a = t0b_guard_selftest(stamp)          # T0b(a): S3, 00_gpio's test as it is
    guard_spi = t0b_spi_oe_selftest(stamp)       # T0b(a): this bench's HOST_REQ-only wrapper
    stamp(f"[host] T0B(a) S3 guard on the mock: {'PASS' if guard_a else 'FAIL'}; "
          f"HOST_REQ-only guard on the mock: {'PASS' if guard_spi else 'FAIL'}")

    port = args.port or autodetect()
    if not port:
        stamp("[host] ERROR: no ST-LINK VCP found")
        return 2

    verdict_fw = None
    session: Optional[Session] = None
    abort: Optional[str] = None
    revs: dict[str, str] = {}
    if not (guard_a and guard_spi):
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
            con = Console(ser, t0, stamp)
            try:
                if en != 0:
                    raise BenchAbort(f"Digital Out enable 0x{en:04X} after open (plan D6: never)")
                if not args.no_reset:
                    stamp("[host] reset through SWD (STM32_Programmer_CLI -c port=SWD mode=HOTPLUG -rst), AD2 in S2")
                    subprocess.run([cube_programmer(), "-c", "port=SWD", "mode=HOTPLUG", "-rst"],
                                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, check=True)
                revs["cm7"] = con.expect(r"\[spi\] CM7 Rev (\S+)", 20.0).group(1)
                ids = {"cm7": con.expect(r"\[spi\] CM7 Build id (\S+)", 10.0).group(1)}
                revs["cm4"] = con.expect(r"\[spi\] CM4 Rev (\S+)", 10.0).group(1)
                ids["cm4"] = con.expect(r"\[spi\] CM4 Build id (.+?)\s*$", 10.0).group(1)
                ver = int(con.expect(r"\[spi\] SPI_BENCH v(\d+) idle", 10.0).group(1))
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
                session.set_oe(DIO_REQ)          # HOST_REQ output, low
                session.req(False)
                session.run()
                verdict_fw = con.expect(r"\[spi\] FW VERDICT=(\w+)", 30.0).group(1)
            except (BenchAbort, AD2SafetyViolation, ad2mod.AD2Error, subprocess.CalledProcessError) as e:
                abort = str(e)
                ad2.set_safe_state()
                stamp(f"[ad2] ABORT: {abort}; AD2 back in S2 (OE=0x{ad2.get_oe():04X})")
                try:
                    verdict_fw = con.expect(r"\[spi\] FW VERDICT=(\w+)", 60.0).group(1)
                except BenchAbort:
                    pass
            finally:
                ad2.set_safe_state()
                time.sleep(0.3)
                con.close()
            en, zet = ad2.dout_state()
            stamp(f"[ad2] closing in S2: OE=0x{ad2.get_oe():04X} Digital Out enable=0x{en:04X} Zet=0x{zet:04X}")

    checks = session.checks if session else []
    stamp(f"[host] T0B(a) host S3 guard: {'PASS' if guard_a else 'FAIL'}")
    stamp(f"[host] T0B(a) host HOST_REQ-only guard: {'PASS' if guard_spi else 'FAIL'}")
    for name, ok in checks:
        stamp(f"[host] CHECK {name}: {'PASS' if ok else 'FAIL'}")
    all_ok = (guard_a and guard_spi and abort is None and verdict_fw == "PASS" and bool(checks) and
              all(ok for _, ok in checks))
    if not args.checklist_confirmed:
        stamp("[host] the wiring checklist was not confirmed by an operator: this capture is not evidence")
    stamp(f"[host] {sum(1 for _, ok in checks if ok)}/{len(checks)} host checks passed, FW VERDICT={verdict_fw}, "
          f"abort={abort}")
    stamp(f"HOST VERDICT={'PASS' if all_ok else 'FAIL'}")

    out = args.out
    if out is None:
        rev = revs.get("cm7", git_rev())
        prefix = "spi_dev_f0_bench" if args.allow_dirty else "spi_f0_bench"
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
