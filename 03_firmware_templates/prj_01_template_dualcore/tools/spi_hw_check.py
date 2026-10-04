#!/usr/bin/env python3
"""Host side of the 06_spi selftest on prj_01 (preset selftest-spi), phases F0 and F1.

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

F1 tests (spi_bm, protocol v3): F1_NOPIN (the host recomputes every T3 vector),
J1_SPI6 and J1_SPI1 (T4, T6, T7, T8 lines checked against the host's own
xorshift32 pattern and CRC-32 from each seed), SPI4_AD2 (T5, T7(c), T12(b): the
AD2 logic analyzer captures SPI4 at 100 MS/s, triggered by HOST_REQ, and the host
decodes the frames; the AD2 drives nothing but HOST_REQ).

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
  S8  no output is enabled before the banner `SPI_BENCH v3 idle` of the
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

BENCH_VERSION = 3          # SPI_BENCH_VERSION of Common/Inc/spi_selftest.h (v2: audit B1; v3: F1 steps)

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
    ("F1_NOPIN", "NONE", "NONE"), ("J1_SPI6", "NONE", "CM7"), ("J1_SPI1", "NONE", "CM4"),
    ("SPI4_AD2", "MCU", "NONE"),
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
# F1: a step's closing handshake comes after all its console output (115200 Bd), so
# the host waits much longer for MCU_ACK there than in a bare handshake.
F1_STEP_TIMEOUT_S = 300.0


class BenchAbort(Exception):
    pass


# ---- F1 oracles: the host's own arithmetic, from the plan's rules ---------------------
# SpiBm_Status_t values (01_shared_drivers/06_spi/spi_bm.h, in order).
ST_OK, ST_PARAM, ST_UNSUPPORTED, ST_RANGE = 0, 1, 2, 3


def mask(bits: int) -> int:
    return (1 << bits) - 1


def xs32_frames(seed: int, n: int, bits: int) -> list[int]:
    """Frame i = the (i + 1)-th output of xorshift32 (13, 17, 5) from `seed`, masked."""
    x, out, m = seed & 0xFFFFFFFF, [], mask(bits)
    for _ in range(n):
        x ^= (x << 13) & 0xFFFFFFFF
        x ^= x >> 17
        x ^= (x << 5) & 0xFFFFFFFF
        out.append(x & m)
    return out


def crc_frames(vals: list[int]) -> int:
    """zlib CRC-32 over each frame as 4 little-endian bytes."""
    import zlib
    return zlib.crc32(b"".join(v.to_bytes(4, "little") for v in vals)) & 0xFFFFFFFF


def coverage(vals: list[int], bits: int) -> int:
    ones = zeros = 0
    for v in vals:
        ones |= v
        zeros |= ~v & mask(bits)
    return ones & zeros


def calc_prescaler(k: int, x: int) -> tuple[int, int, int]:
    """Plan rule: the smallest MBR with k / 2^(MBR+1) <= x; SCK = floor(k / 2^(MBR+1))."""
    if k == 0 or x == 0:
        return ST_PARAM, 0, 0
    for m in range(8):
        if k <= x * (1 << (m + 1)):
            return ST_OK, m, k >> (m + 1)
    return ST_RANGE, 0, 0


def calc_guard(c: int, k: int, m: int) -> tuple[int, int]:
    """One SCK period in CPU cycles, rounded up: ceil(c * 2^(m+1) / k)."""
    if c == 0 or k == 0 or m > 7:
        return ST_PARAM, 0
    v = -(-(c << (m + 1)) // k)
    return (ST_RANGE, 0) if v > 0xFFFFFFFF else (ST_OK, v)


def joined(lines: list[str]) -> list[str]:
    """Console lines with log_long()'s continuation pieces ("[spi] | ...") put back
    on the line they continue."""
    out: list[str] = []
    for line in lines:
        i = line.find("[spi] | ")
        if i >= 0 and out:
            out[-1] += " " + line[i + len("[spi] | "):]
        else:
            out.append(line)
    return out


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
        self._lines: list[str] = []           # every line, for the F1 checks made after a step
        self._lines_lock = threading.Lock()
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
                with self._lines_lock:
                    self._lines.append(line)
                self._q.put((t, line))

    def mark(self) -> int:
        with self._lines_lock:
            return len(self._lines)

    def since(self, mark: int) -> list[str]:
        """The lines received from `mark` on, continuations joined (joined())."""
        with self._lines_lock:
            return joined(self._lines[mark:])

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

    def sync(self, pre: Optional[Callable[[], None]] = None, timeout_s: float = HS_TIMEOUT_S) -> None:
        self.wait_ack(True, timeout_s)
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

    # ================================================================ F1 (spi_bm)
    def f1_nopin(self) -> None:
        """F1_NOPIN: no pin moves. The host recomputes every T3 vector the
        firmware printed, with its own arithmetic (calc_prescaler, calc_guard)."""
        mk = self.con.mark()
        self.step("F1_NOPIN")
        self.sync(timeout_s=F1_STEP_TIMEOUT_S)
        lines = self.con.since(mk)
        rp = re.compile(r"\[spi\] T3P (\d+) k=(\d+) x=(\d+) st=(\d+) mbr=(\d+) sck=(\d+)")
        rg = re.compile(r"\[spi\] T3G (\d+) c=(\d+) k=(\d+) m=(\d+) st=(\d+) cyc=(\d+)")
        np_ = ng = 0
        bad: list[str] = []
        caught = 0
        for line in lines:
            m = rp.search(line)
            if m:
                i, k, x, st, mbr, sck = (int(v) for v in m.groups())
                np_ += 1
                exp = calc_prescaler(k, x)
                if (st, mbr, sck) != exp:
                    bad.append(f"P{i} k={k} x={x}: got {(st, mbr, sck)} exp {exp}")
                if exp[0] == ST_OK and (st, mbr + 1, sck) != exp:   # control: MBR off by one is seen
                    caught += 1
                continue
            m = rg.search(line)
            if m:
                i, c, k, mm, st, cyc = (int(v) for v in m.groups())
                ng += 1
                exp2 = calc_guard(c, k, mm)
                if (st, cyc) != exp2:
                    bad.append(f"G{i} c={c} k={k} m={mm}: got {(st, cyc)} exp {exp2}")
        n_ok = sum(1 for line in lines if (m := rp.search(line)) and int(m.group(4)) == ST_OK)
        self.check("T3 pure functions (host recompute)", np_ > 0 and ng > 0 and not bad,
                   f"{np_} CalcPrescaler and {ng} CalcGuardCycles vectors recomputed from the plan's rules: "
                   f"{len(bad)} differ {bad[:4]}")
        self.check("T3 control", n_ok > 0 and caught == n_ok,
                   f"the same comparison with MBR + 1 flags {caught} of the {n_ok} OK vectors (all)")

    def j1_lines(self, inst: str, lines: list[str]) -> None:
        """T4, T4C, T6, T7, T8 lines of one J1 instance against the host's own
        pattern (xs32_frames), digest (crc_frames) and coverage."""
        r4 = re.compile(rf"\[spi\] T4 {inst} m(\d) b(\d+) ([ML]) t(\d+) n(\d+) s=([0-9A-F]{{8}}) st=(\d+) "
                        r"mis=(\d+) crc=([0-9A-F]{8}) cov=([0-9A-F]{8}) cyc=(\d+)")
        rc = re.compile(rf"\[spi\] T4C {inst} b(\d+) (up|down) n(\d+) s=([0-9A-F]{{8}}) st=(\d+) mis=(\d+) "
                        r"crc=([0-9A-F]{8})")
        rx = re.compile(rf"\[spi\] (T6|T7A|T7B|T8) {inst} .*?b(\d+) (?:n(\d+) )?s=([0-9A-F]{{8}}) st=(\d+) mis=(\d+) "
                        r"crc=([0-9A-F]{8})(?: irq=(\d+) after=(\d+))?(?: (required|recorded))?")
        n4 = 0
        bad4: list[str] = []
        cov: dict[int, int] = {}
        for line in lines:
            m = r4.search(line)
            if not m:
                continue
            mode, bits, order, thr, n, seed, st, mis, crc, cv, _ = m.groups()
            bits, n, seed = int(bits), int(n), int(seed, 16)
            tx = xs32_frames(seed, n, bits)
            n4 += 1
            cov[bits] = cov.get(bits, 0) | int(cv, 16)
            if int(st) != ST_OK or int(mis) != 0 or int(crc, 16) != crc_frames(tx) or int(cv, 16) != coverage(tx, bits):
                bad4.append(f"m{mode} b{bits} {order} t{thr} n{n} st={st} mis={mis}")
        full = sorted(b for b, c in cov.items() if c == mask(b))
        self.check(f"T4 {inst} loop (host digest)", n4 > 0 and not bad4 and len(full) == len(cov),
                   f"{n4} transfers: RX CRC-32 = CRC of the xorshift32 pattern from each seed, coverage as computed "
                   f"here, status 0, no mismatch; {len(bad4)} differ {bad4[:3]}; sizes with every data bit seen at "
                   f"0 and 1: {full}")
        nc = 0
        badc: list[str] = []
        for line in lines:
            m = rc.search(line)
            if not m:
                continue
            bits, how, n, seed, st, mis, crc = m.groups()
            bits, n = int(bits), int(n)
            tx = xs32_frames(int(seed, 16), n, bits)
            level = mask(bits) if how == "up" else 0
            exp_mis = sum(1 for v in tx if v != level)
            nc += 1
            if int(crc, 16) != crc_frames([level] * n) or int(mis) != exp_mis or exp_mis == 0:
                badc.append(f"b{bits} {how} mis={mis} (exp {exp_mis})")
        self.check(f"T4 {inst} control (MISO on PB4)", nc > 0 and not badc,
                   f"{nc} transfers with MISO moved to PB4 pulled up/down: RX = all ones/zeros by CRC, mismatches "
                   f"= the host's count (> 0); {len(badc)} differ {badc[:3]}")
        groups: dict[str, list[str]] = {}
        nreq = 0
        for line in lines:
            m = rx.search(line)
            if not m:
                continue
            kind, bits, n, seed, st, mis, crc, _irq, after, req = m.groups()
            bits = int(bits)
            n = int(n) if n else 1024                   # T8: 1024 frames
            tx = xs32_frames(int(seed, 16), n, bits)
            if kind == "T8" and req == "recorded":
                continue
            nreq += 1
            ok = int(st) == ST_OK and int(mis) == 0 and int(crc, 16) == crc_frames(tx) and (after in (None, "0"))
            if not ok:
                groups.setdefault(kind, []).append(line.split("] ", 1)[-1][:80])
        self.check(f"T6/T7/T8 {inst} (host digest)", nreq > 0 and not groups,
                   f"{nreq} interrupt, DMA and speed-sweep (<= 25 MHz) transfers checked against the seed's CRC: "
                   f"failed {groups if groups else 'none'}")

    def f1_j1(self, step: str, inst: str) -> None:
        mk = self.con.mark()
        self.step(step)
        self.sync(timeout_s=F1_STEP_TIMEOUT_S)
        self.j1_lines(inst, self.con.since(mk))

    # -- SPI4 against the AD2 (T5, T7(c), T12(b)): the AD2 only listens
    LA_RATE = 100e6
    LA_N = 4096
    LA_PREFILL = 50

    def spi4_ad2(self, k45: int) -> None:
        self.step("SPI4_AD2")
        self.buschk("SPI4_AD2 before")
        self.buschk("SPI4_AD2 with PE11..PE14 in AF5 (SPI4 at its reset values, S13)")
        rcap = re.compile(r"\[spi\] CAP (?:([a-zA-Z]) k(\d+) m(\d) b(\d+) l(\d) n(\d+) nss(\d) op(\d) mssi(\d+) "
                          r"midi(\d+) mbr(\d) s=([0-9A-F]{8})|e k(\d+) keep(\d)|done k(\d+))")
        caps: list[dict] = []
        levels: dict[int, int] = {}
        while True:
            m = self.con.expect(rcap.pattern, 60.0, stop_on_abort=True)
            if m.group(15) is not None:
                break
            if m.group(13) is not None:                   # (e): SCK level between two transfers
                keep = int(m.group(14))
                self.sync(pre=lambda kk=keep: levels.__setitem__(kk, int(bool(self.ad2.digital_io_read() & DIO_SCK))))
                continue
            kind = m.group(1)
            cap = dict(kind=kind, k=int(m.group(2)), mode=int(m.group(3)), bits=int(m.group(4)),
                       lsb=int(m.group(5)), n=int(m.group(6)), nss=int(m.group(7)), op=int(m.group(8)),
                       mssi=int(m.group(9)), midi=int(m.group(10)), mbr=int(m.group(11)), seed=int(m.group(12), 16))
            self.sync(pre=lambda: self.ad2.la_arm(self.LA_RATE, self.LA_N, DIO_REQ, self.LA_PREFILL))
            cap["smp"] = self.ad2.la_read(self.LA_N, 2.0)
            cap["st"] = int(self.con.expect(rf"\[spi\] CAPR k{cap['k']} st=(\d+)", 10.0).group(1))
            caps.append(cap)
        self.buschk("SPI4_AD2 T14: after the Deinit, PE11..PE14 still in AF5")
        self.buschk("SPI4_AD2 after, pins in input")
        self.spi4_judge(caps, levels, k45)

    @staticmethod
    def sck_period(edges: list[int], bits: int) -> float:
        """The SCK period in samples, measured inside the frames of this capture
        (consecutive sampling edges of one frame; the gaps MSSI/MIDI add are not
        in it). The board's HSE comes from the ST-LINK MCO, about 1.3 % fast, so
        the nominal 2^(MBR+1) samples would be off by that much."""
        d = [edges[i + 1] - edges[i] for i in range(len(edges) - 1) if (i + 1) % bits != 0]
        return sum(d) / len(d) if d else 0.0

    @staticmethod
    def nss_active(s: int, op: int) -> bool:
        return bool((s >> 3) & 1) == bool(op)

    def spi4_judge(self, caps: list[dict], levels: dict[int, int], k45: int) -> None:
        sck_b, mosi_b, mco_b = 4, 6, 14
        res: dict[str, list[str]] = {}
        delays: dict[tuple[str, int], float] = {}
        periods: dict[tuple[str, int], float] = {}
        widths: dict[str, tuple[int, float]] = {}
        ratios: list[str] = []
        ratio_bad: list[str] = []
        for c in caps:
            smp = c["smp"]
            tag = f"{c['kind']}{c['k']}"
            fails = res.setdefault(c["kind"], [])
            if smp is None:
                fails.append(f"{tag}: no capture")
                continue
            cpol, cpha = c["mode"] >> 1, c["mode"] & 1
            exp = xs32_frames(c["seed"], c["n"], c["bits"])
            edges = ad2mod.spi_sample_edges(smp, sck_b, cpol, cpha)
            if c["kind"] == "g":
                fs = ad2mod.la_period_high(smp, sck_b, self.LA_RATE)["freq_hz"]
                fm = ad2mod.la_period_high(smp, mco_b, self.LA_RATE)["freq_hz"]
                want = (k45 / (1 << (c["mbr"] + 1))) / (8e6 / 8)
                got = fs / fm if fm else 0.0
                err = (got - want) / want if want else 1.0
                ratios.append(f"/{2 << c['mbr']} {got:.5f} ({want:.5f}, {err * 1e6:+.0f} ppm)")
                if abs(err) > 0.005 or c["st"] != 0:
                    ratio_bad.append(tag)
                continue
            frames, nedge = ad2mod.spi_decode(smp, sck_b, mosi_b, cpol, cpha, c["bits"], bool(c["lsb"]))
            if c["st"] != 0:
                fails.append(f"{tag}: status {c['st']}")
            if c["kind"] != "N" and (nedge != c["n"] * c["bits"] or frames != exp):
                fails.append(f"{tag}: {nedge} sampling edges ({c['n'] * c['bits']}), frames differ "
                             f"{sum(1 for a, b in zip(frames, exp) if a != b) + abs(len(frames) - len(exp))}")
            if ((smp[0] >> sck_b) & 1) != cpol or ((smp[-1] >> sck_b) & 1) != cpol:
                fails.append(f"{tag}: SCK idle {(smp[0] >> sck_b) & 1}/{(smp[-1] >> sck_b) & 1} (CPOL {cpol})")
            if c["nss"] != 0 and edges:
                act = ad2mod.la_segments(smp, 3, c["op"])
                with_edges = [s for s in act if any(s[0] <= e < s[1] for e in edges)]
                if not all(self.nss_active(smp[e], c["op"]) for e in edges) or self.nss_active(smp[0], c["op"]):
                    fails.append(f"{tag}: NSS not active at every sampling edge, or active before the transfer")
                want_seg = c["n"] if c["nss"] == 2 else 1
                if len(with_edges) != want_seg:
                    fails.append(f"{tag}: {len(with_edges)} NSS active intervals with data ({want_seg})")
                if c["kind"] == "d" and with_edges:
                    allr, allf = ad2mod.la_edges(smp, sck_b)
                    first = min(allr + allf)
                    tp = self.sck_period(edges, c["bits"])
                    if c["mssi"] or not c["midi"]:
                        delays[("mssi", c["mssi"])] = first - with_edges[0][0]
                        periods[("mssi", c["mssi"])] = tp
                    if c["midi"] or not c["mssi"]:
                        per = c["bits"]
                        gaps = [edges[(f + 1) * per] - edges[f * per + per - 1] for f in range(c["n"] - 1)]
                        delays[("midi", c["midi"])] = sum(gaps) / len(gaps) if gaps else 0.0
                        periods[("midi", c["midi"])] = tp
            if c["kind"] in ("G", "N"):
                rises, falls = ad2mod.la_edges(smp, sck_b)
                w = (falls[-1] - rises[-1]) if rises and falls and falls[-1] > rises[-1] else -1
                widths[c["kind"]] = (w, self.sck_period(edges, c["bits"]))
        dbad = []
        dall = []
        for what in ("mssi", "midi"):
            base = delays.get((what, 0))
            for k in (1, 4, 15):
                d = delays.get((what, k))
                tp = periods.get((what, k), 0.0)
                if base is None or d is None or abs((d - base) - k * tp) > 3:
                    dbad.append(f"{what}={k}")
                dall.append(f"{what}={k}: {None if d is None or base is None else round(d - base, 1)} "
                            f"({k} x {tp:.2f} = {k * tp:.1f})")
        named = {"a": "T5(a) modes x sizes x bit order, hardware SS", "b": "T5(b) SSIOP = 1",
                 "c": "T5(c) SS pulsed between frames (MIDI 2)", "d": "T5(d) MSSI/MIDI frames",
                 "m": "T7(c) SPI4 transmit by DMA1 stream 0", "G": "T12(b) one frame with the guard"}
        for kind, label in named.items():
            n = sum(1 for c in caps if c["kind"] == kind)
            fails = res.get(kind, [])
            self.check(label, n > 0 and not fails,
                       f"{n} LA captures at 100 MS/s decoded on the host against the seed's xorshift32 frames, SCK idle "
                       f"at CPOL, NSS per its mode: {len(fails)} failed {fails[:3]}")
        self.check("T5(d) MSSI and MIDI timing", not dbad,
                   f"delay NSS->first SCK edge and gap between frames, minus the value at 0, = k SCK periods "
                   f"measured in the same capture (+-3 samples): {dall}; out {dbad}")
        self.check("T5(g) SCK against MCO1 (HSE/8)", len(ratios) == 7 and not ratio_bad,
                   f"f(SCK)/f(MCO1) for MBR /4../256 against {k45} Hz / 2^(MBR+1) / 1 MHz (+-0.5 %): {ratios}; "
                   f"out {ratio_bad}")
        g, tg = widths.get("G", (-1, 0.0))
        half = tg / 2.0
        self.check("T12(b) last SCK pulse with the guard", tg > 0 and abs(g - half) <= 3,
                   f"CPHA = 0, /256, one frame: last SCK high pulse {g} samples (half the period measured in the "
                   f"frame, {half:.1f}, +-3); without the guard (control, decision 6: recorded only) "
                   f"{widths.get('N', (-1, 0.0))[0]} samples")
        self.check("T5(e) keep_pins", levels.get(1) == 0 and levels.get(0) == 1,
                   f"SCK (DIO4) between two transfers, pulled up against CPOL = 0: keep_pins=1 -> {levels.get(1)} "
                   f"(0: the SPI holds it), control keep_pins=0 -> {levels.get(0)} (1: the pull)")

    def f1_clocks(self) -> int:
        m = None
        for line in self.con.since(0):
            m = re.search(r"F1 clocks .*SPI4/5 (\d+) Hz", line) or m
        if not m:
            raise BenchAbort("no 'F1 clocks' line before the F1 steps")
        return int(m.group(1))

    def run(self) -> None:
        self.t0a()
        self.t0c()
        self.t0b()
        self.t0b_s13()
        self.t0b_s14()
        self.f1_nopin()
        self.f1_j1("J1_SPI6", "SPI6")
        self.f1_j1("J1_SPI1", "SPI1")
        self.spi4_ad2(self.f1_clocks())
        self.step("DONE")
        bad = [(s, f"0x{en:04X}") for s, en in self.dout_seen if en != 0]
        self.check("No Digital Out", not bad and len(self.dout_seen) == len(STEPS),
                   f"Digital Out enable read back at {len(self.dout_seen)} step boundaries of {len(STEPS)}: "
                   f"all 0x0000 {not bad} {bad}")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", help="ST-LINK VCP (default: autodetect)")
    ap.add_argument("--out", type=pathlib.Path,
                    help="capture file (default: docs/spi_f1_bench_<rev>.txt, spi_dev_f1_bench_<rev>.txt with --allow-dirty)")
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
    stamp(f"[host] spi_hw_check.py, host checkout {git_rev()}, protocol v{BENCH_VERSION}, phases F0 and F1")
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
        prefix = "spi_dev_f1_bench" if args.allow_dirty else "spi_f1_bench"
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
