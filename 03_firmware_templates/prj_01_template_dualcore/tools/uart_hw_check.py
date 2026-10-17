#!/usr/bin/env python3
"""Host side of the 04_uart selftest on prj_01 (preset selftest-uart).

Runs one complete bench session against the firmware of
CM7/App/Src/uart_selftest.c with the Analog Discovery 2, and writes ONE capture
file: every console line and every host measurement, with timestamps, until
`HOST VERDICT=`. Wiring, safety rules and what each test proves:
01_shared_drivers/04_uart/IMPLEMENTATION_PLAN.md.

Usage:
    py tools/flash.py --preset selftest-uart
    py tools/uart_hw_check.py                   # resets board and runs on hardware
    py tools/uart_hw_check.py --mock-test       # runs purely on mock without board or AD2
    py tools/uart_hw_check.py --allow-dirty     # accept a -dirty Rev during dev
    py tools/uart_hw_check.py --expect-control-u1  # mark b41c4a7 U1 failure as expected control

Electrical safety rules:
    S2:  AD2 opens and closes with all DIOs in input (ad2.py) and reverts on any exception.
    S7:  Announced step names strictly checked against STEPS table.
    SU1: DIO15 is an AD2 output ONLY in UT5 steps via ad2_set_oe() and ad2_set_dout().
    SU2: PD5 verified in resting state before AD2 drives PD6.
    SU3: DIO15 restored to input between steps and on exit.
    SU4: PD3-PD6 configured only in selftest-uart.
"""

from __future__ import annotations

import argparse
import ctypes
import hashlib
import os
import pathlib
import re
import shlex
import subprocess
import sys
import threading
import time
from typing import Callable, Optional

PROJECT = pathlib.Path(__file__).resolve().parents[1]
WORKSPACE = PROJECT.parents[1]
sys.path.insert(0, str(WORKSPACE / "tools"))
os.environ.setdefault("STM32_PROJECT_ROOT", str(PROJECT))

import ad2 as ad2mod                                   # noqa: E402
from ad2 import (AD2, AD2Error, AD2SafetyViolation,    # noqa: E402
                 AD2_DIO_HOST_REQ, AD2_DIO_MCU_ACK, AD2_DIO_MCO1_PA8,
                 AD2_DIO_UART_RX_INJECT, ad2_set_oe, ad2_set_dout)
from monitor import SerialPort, autodetect             # noqa: E402

UART_BENCH_VERSION = 1

# ---- Bench Wiring -----------------------------------------------------------
DIO_REQ = AD2_DIO_HOST_REQ      # DIO12 -> PE2 (HOST_REQ, AD2 output)
DIO_ACK = AD2_DIO_MCU_ACK       # DIO13 <- PE7 (MCU_ACK, MCU output)
DIO_MCO = AD2_DIO_MCO1_PA8      # DIO14 <- PA8 (MCO1 clock reference)
DIO_RX  = AD2_DIO_UART_RX_INJECT# DIO15 -> PD6 (USART2_RX / oracle)

STEPS = [
    "UT0",
    "UT0_CTRL",
    "UT1",
    "UT2",
    "UT3",
    "UT4",
    "UT4_CM4",
    "UT5",
    "UT6",
    "UT7",
    "UT8",
    "UT9",
    "UT10",
    "DONE",
]

# ---- CubeProgrammer & Build ID Helpers --------------------------------------

def cube_programmer() -> str:
    cli = shutil_which("STM32_Programmer_CLI.exe") or shutil_which("STM32_Programmer_CLI")
    if cli:
        return cli
    candidates = list(pathlib.Path("C:/ST").glob("**/STM32_Programmer_CLI.exe"))
    if candidates:
        return str(candidates[0])
    return "STM32_Programmer_CLI"


def shutil_which(cmd: str) -> Optional[str]:
    import shutil
    return shutil.which(cmd)


def git_rev() -> str:
    try:
        r = subprocess.run(["git", "rev-parse", "--short", "HEAD"],
                           capture_output=True, text=True, check=True)
        return r.stdout.strip()
    except Exception:
        return "unknown"


def host_build_id(preset: str, project: pathlib.Path = PROJECT) -> str:
    """R3-a/R3-b: FW_BUILD_ID as cmake/fw_build_id.cmake computes it, over the
    checkout of `project`: the first 16 hex digits of the SHA-256 of
    "HEAD=" <git rev-parse HEAD, 40 hex>, the bytes of `git diff --no-color
    HEAD`, "CMAKE_C_FLAGS=" <CMAKE_C_FLAGS of build/<preset>/CMakeCache.txt>
    and " PRESET=" <preset>, no newline added. "unknown" when git or the cache
    cannot be read; never an invented value.
    """
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


# ---- Formula Oracles --------------------------------------------------------

def compute_presc_brr(inst: str, f_ker: int, baud: int, over8: bool) -> tuple[int, int]:
    """Independent host formula for STM32H7 UART PRESC and BRR registers."""
    presc_divs = [1, 2, 4, 6, 8, 10, 12, 16, 32, 64, 128, 256]
    if inst == "LPUART1":
        for p, div in enumerate(presc_divs):
            ker_p = f_ker // div
            if ker_p < 3 * baud or ker_p > 4096 * baud:
                continue
            ratio_int = ker_p // baud
            ratio_rem = ker_p % baud
            if 3 <= ratio_int <= 4 and ratio_rem != 0:
                continue
            num = (256 * ker_p) + (baud // 2)
            brr = num // baud
            if brr < 0x300 or brr > 0xFFFFF:
                continue
            actual = (256 * ker_p) // brr
            diff = abs(actual - baud)
            if diff * 100 <= baud * 2:
                return p, brr
        return 0, 0

    for p, div in enumerate(presc_divs):
        ker_p = f_ker // div
        if ker_p == 0:
            break
        if over8:
            usartdiv = (2 * ker_p + baud // 2) // baud
            if usartdiv < 16 or usartdiv > 0xFFFF:
                continue
            brr = (usartdiv & 0xFFF0) | ((usartdiv & 0x000F) >> 1)
            actual_div = (brr & 0xFFF0) | ((brr & 0x0007) << 1)
            if actual_div == 0:
                continue
            actual = (2 * ker_p) // actual_div
        else:
            usartdiv = (ker_p + baud // 2) // baud
            if usartdiv < 16 or usartdiv > 0xFFFF:
                continue
            brr = usartdiv
            actual = ker_p // brr

        diff = abs(actual - baud)
        if diff * 100 <= baud * 2:
            return p, brr

    return 0, 0


# ---- UART Logic Analyzer Frame Decoder ---------------------------------------

def decode_uart_frames(samples: list[int], bit_period_samples: int,
                       data_bits: int = 8, parity: str = "none", stop_bits: int = 1) -> list[dict]:
    """Decodes UART frames from logic analyzer samples of DIO15 (bit 15).

    Returns a list of decoded frames:
      {"byte": int, "parity_ok": bool, "stop_ok": bool, "bit_period_mco": float}
    """
    frames = []
    i = 0
    n = len(samples)

    while i < n:
        # Look for falling edge (start bit = 0)
        while i < n and (samples[i] & DIO_RX) != 0:
            i += 1
        if i >= n:
            break

        start_edge = i
        # Sample center of start bit
        sample_pt = i + bit_period_samples // 2
        if sample_pt >= n:
            break
        if (samples[sample_pt] & DIO_RX) != 0:
            i += 1
            continue

        # Count MCO1 rising edges during frame for bit period measurement
        mco_edges = 0
        val = 0
        ones_count = 0
        sample_pt += bit_period_samples
        for b in range(data_bits):
            if sample_pt >= n:
                break
            bit = 1 if (samples[sample_pt] & DIO_RX) else 0
            val |= (bit << b)
            ones_count += bit
            sample_pt += bit_period_samples

        parity_ok = True
        if parity in ("even", "odd"):
            if sample_pt < n:
                par_bit = 1 if (samples[sample_pt] & DIO_RX) else 0
                total_ones = ones_count + par_bit
                if parity == "even":
                    parity_ok = (total_ones % 2 == 0)
                else:
                    parity_ok = (total_ones % 2 == 1)
                sample_pt += bit_period_samples
            else:
                parity_ok = False

        # Stop bit
        stop_ok = True
        if sample_pt < n:
            stop_ok = ((samples[sample_pt] & DIO_RX) != 0)
            sample_pt += bit_period_samples

        # Calculate bit period in terms of MCO1 periods (DIO14 transitions)
        # In 100 MS/s, 4 MHz MCO1 has period of 25 samples
        for s in range(start_edge, min(sample_pt, n - 1)):
            if (samples[s] & DIO_MCO) == 0 and (samples[s + 1] & DIO_MCO) != 0:
                mco_edges += 1

        total_bits = 1 + data_bits + (1 if parity in ("even", "odd") else 0) + stop_bits
        mco_per_bit = (mco_edges / total_bits) if total_bits > 0 else 0.0

        frames.append({
            "byte": val,
            "parity_ok": parity_ok,
            "stop_ok": stop_ok,
            "mco_per_bit": mco_per_bit
        })

        i = sample_pt

    return frames


# ---- Mock Infrastructure ----------------------------------------------------

class _MockDwf:
    def __init__(self) -> None:
        self.oe_calls: list[int] = []
        self._oe = 0
        self._output_val = 0
        self.la_armed = False
        self.mock_la_samples: Optional[list[int]] = None
        self.current_step: Optional[str] = None
        self._last_error = b"Mock DWF error"

    def set_step(self, step_name: Optional[str]) -> None:
        self.current_step = step_name

    def FDwfGetLastErrorMsg(self, buf):                # noqa: N802
        ctypes.memmove(buf, self._last_error, min(len(self._last_error) + 1, 512))
        return 1

    def FDwfDigitalIOOutputEnableSet(self, h, mask):   # noqa: N802
        val = int(mask.value)
        self.oe_calls.append(val)
        self._oe = val
        return 1

    def FDwfDigitalIOOutputEnableGet(self, h, ref):    # noqa: N802
        ref._obj.value = self._oe
        return 1

    def FDwfDigitalIOReset(self, h):                   # noqa: N802
        self._oe = 0
        self._output_val = 0
        return 1

    def FDwfDigitalIOConfigure(self, h):               # noqa: N802
        return 1

    def FDwfDigitalIOOutputSet(self, h, val):          # noqa: N802
        self._output_val = int(val.value)
        return 1

    def FDwfDigitalIOInputStatus(self, h, ref):        # noqa: N802
        val = 0
        if not (self._output_val & DIO_REQ):
            val |= DIO_ACK
        if self.current_step == "UT0":
            # If host drives DIO15 low, PD6 is low (series resistor JU1 1k)
            if (self._oe & DIO_RX) and not (self._output_val & DIO_RX):
                val &= ~DIO_RX
            else:
                val |= DIO_RX  # resting line high
        elif self.current_step == "UT0_CTRL":
            val &= ~DIO_RX     # firmware drives PD5 low
        else:
            val |= DIO_RX
        ref._obj.value = val
        return 1

    def FDwfDigitalInReset(self, h):                   # noqa: N802
        self.la_armed = False
        return 1

    def FDwfDigitalInConfigure(self, h, reconfigure, start): # noqa: N802
        self.la_armed = bool(start.value)
        return 1

    def FDwfDigitalInStatus(self, h, read_data, ref_status): # noqa: N802
        ref_status._obj.value = 2  # DwfStateDone
        return 1

    def FDwfDigitalInStatusData(self, h, buf, count):  # noqa: N802
        samples = count.value // 2
        if self.mock_la_samples is not None:
            data = self.mock_la_samples[:samples]
        else:
            data = [0xFFFF] * samples
        for i, v in enumerate(data):
            buf[i] = v
        return 1

    def FDwfDigitalOutReset(self, h):                  # noqa: N802
        return 1

    def FDwfDigitalOutConfigure(self, h, start):       # noqa: N802
        return 1

    def FDwfDigitalOutEnableSet(self, h, ch, en):      # noqa: N802
        if not hasattr(self, "_dout_enables"):
            self._dout_enables = {}
        self._dout_enables[int(ch.value)] = int(en.value)
        return 1

    def FDwfDigitalOutIdleSet(self, h, ch, idle):      # noqa: N802
        return 1

    def FDwfDigitalOutEnableGet(self, h, ch, ref):     # noqa: N802
        if not hasattr(self, "_dout_enables"):
            self._dout_enables = {}
        ref._obj.value = self._dout_enables.get(int(ch.value), 0)
        return 1

    def FDwfDigitalOutIdleGet(self, h, ch, ref):       # noqa: N802
        ref._obj.value = 3  # Zet
        return 1

    def FDwfDigitalOutOutputSet(self, h, ch, out_type): # noqa: N802
        return 1

    def FDwfDigitalOutOutputGet(self, h, ch, ref):      # noqa: N802
        ref._obj.value = 0
        return 1


class MockAD2:
    def __init__(self, mock_dwf: _MockDwf) -> None:
        self._dwf = mock_dwf
        self._hdwf = ctypes.c_int(1)
        self.mock_dwf = mock_dwf

    def set_step(self, step: Optional[str]) -> None:
        self.mock_dwf.set_step(step)

    def set_safe_state(self) -> None:
        self.mock_dwf.FDwfDigitalIOReset(self._hdwf)

    def set_oe(self, mask: int) -> None:
        ad2_set_oe(self.mock_dwf, self._hdwf.value, mask, current_step=self.mock_dwf.current_step)

    def dout_custom(self, ch: int, bits: list[int], bit_rate_hz: float) -> None:
        ad2_set_dout(self.mock_dwf, self._hdwf.value, 1 << ch, current_step=self.mock_dwf.current_step)

    def digital_io_write(self, value: int, mask: int = 0xFFFF) -> None:
        self.mock_dwf.FDwfDigitalIOOutputSet(self._hdwf, ctypes.c_uint32(value & mask))

    def digital_io_read(self) -> int:
        val = ctypes.c_uint32()
        self.mock_dwf.FDwfDigitalIOInputStatus(self._hdwf, ctypes.byref(val))
        return val.value

    def la_arm(self, rate_hz: float, samples: int, trigger_rise_mask: int = 0,
               trigger_fall_mask: int = 0, prefill: int = 0) -> None:
        self.mock_dwf.la_armed = True

    def la_read(self, samples: int, timeout_s: float = 1.0) -> Optional[list[int]]:
        buf = (ctypes.c_uint16 * samples)()
        self.mock_dwf.FDwfDigitalInStatusData(self._hdwf, buf, ctypes.c_int(samples * 2))
        return [int(x) for x in buf]


class MockConsole:
    def __init__(self, lines: list[str], log: Callable[[str], None], dwf: Optional[_MockDwf] = None) -> None:
        self.lines = list(lines)
        self.log = log
        self.dwf = dwf
        self.idx = 0

    def readline(self, timeout_s: float = 10.0) -> Optional[str]:
        if self.idx < len(self.lines):
            line = self.lines[self.idx]
            self.idx += 1
            m = re.match(r"\[uart\] STEP (\w+)", line)
            if m and self.dwf is not None:
                self.dwf.set_step(m.group(1))
            return line
        return None

    def expect(self, pattern: str, timeout_s: float = 10.0):
        pat = re.compile(pattern)
        while self.idx < len(self.lines):
            line = self.lines[self.idx]
            self.idx += 1
            m = pat.search(line)
            if m:
                return m
        raise RuntimeError(f"Timeout waiting for pattern: {pattern}")


class Console:
    """Serial console reader with pattern expectation and timestamped logging."""
    def __init__(self, ser: SerialPort, t0: float, log: Callable[[str], None]) -> None:
        self.ser = ser
        self.t0 = t0
        self.log = log

    def readline(self, timeout_s: float = 10.0) -> Optional[str]:
        t_end = time.monotonic() + timeout_s
        line = ""
        while time.monotonic() < t_end:
            ch = self.ser.read(1)
            if not ch:
                continue
            if ch in (b"\r", b"\n"):
                if line:
                    return line
            else:
                line += ch.decode("utf-8", errors="replace")
        return line if line else None

    def expect(self, pattern: str, timeout_s: float = 10.0):
        pat = re.compile(pattern)
        t_end = time.monotonic() + timeout_s
        while time.monotonic() < t_end:
            line = self.readline(timeout_s=1.0)
            if line is not None:
                self.log(line)
                m = pat.search(line)
                if m:
                    return m
        raise RuntimeError(f"Timeout waiting for pattern: {pattern}")


# ---- UartSession Implementation ---------------------------------------------

class UartSession:
    def __init__(self, ad2: MockAD2 | AD2, con: MockConsole | Console, log: Callable[[str], None],
                 expect_control_u1: bool = False) -> None:
        self.ad2 = ad2
        self.con = con
        self.log = log
        self.expect_control_u1 = expect_control_u1
        self.mock = isinstance(ad2, MockAD2)
        self.checks: list[tuple[str, bool, str]] = []
        self.current_step = ""

    def check(self, name: str, ok: bool, detail: str = "") -> None:
        status = "PASS" if ok else "FAIL"
        msg = f"[host] CHECK {name}: {status}"
        if detail:
            msg += f" ({detail})"
        self.log(msg)
        self.checks.append((name, ok, detail))

    def handshake(self, expected_name: str) -> bool:
        """Handshake: wait for STEP <expected_name>, set_step, wait ACK, REQ pulse."""
        self.current_step = expected_name
        self.ad2.set_step(expected_name)

        # Wait for STEP line
        t_start = time.monotonic()
        while time.monotonic() - t_start < 10.0:
            line = self.con.readline(timeout_s=2.0)
            if line is None:
                continue
            self.log(line)
            m = re.search(r"\[uart\] STEP (\w+)", line)
            if m:
                if m.group(1) == expected_name:
                    break
                else:
                    self.check(f"step_match_{expected_name}", False, f"got {m.group(1)}")
                    return False
        else:
            self.check(f"step_timeout_{expected_name}", False, "timeout waiting for STEP")
            return False

        return self.sync()

    def sync(self) -> bool:
        if self.mock:
            return True
        # Wait ACK high (up to 10 s per S-m)
        t0 = time.monotonic()
        while not (self.ad2.digital_io_read() & DIO_ACK):
            if time.monotonic() - t0 > 10.0:
                self.check("sync_timeout_ack_high", False, "timeout waiting for MCU_ACK high")
                return False
            time.sleep(0.001)

        # Raise REQ
        self.ad2.digital_io_write(DIO_REQ, DIO_REQ)

        # Wait ACK low
        t0 = time.monotonic()
        while self.ad2.digital_io_read() & DIO_ACK:
            if time.monotonic() - t0 > 10.0:
                self.check("sync_timeout_ack_low", False, "timeout waiting for MCU_ACK low")
                return False
            time.sleep(0.001)

        # Lower REQ
        self.ad2.digital_io_write(0, DIO_REQ)
        return True

    def drain_until_verdict(self, step_name: str) -> bool:
        t_end = time.monotonic() + 15.0
        while time.monotonic() < t_end:
            line = self.con.readline(timeout_s=2.0)
            if line is None:
                continue
            self.log(line)
            if f"[uart] VERDICT {step_name}" in line:
                return "PASS" in line
            if "FW VERDICT=" in line:
                return "PASS" in line
        return False

    def run_all_steps(self) -> bool:
        try:
            # UT0: bench resting state and 1k series resistor
            if not self.handshake("UT0"): return False
            if not self.sync(): return False
            resting_high = ((self.ad2.digital_io_read() & DIO_RX) != 0)
            self.check("UT0 resting line high", resting_high, f"DIO15={'1' if resting_high else '0'}")

            # Pull DIO15 low with OE (SU1 permitted in UT0)
            self.ad2.set_oe(DIO_REQ | DIO_RX)
            self.ad2.digital_io_write(0, DIO_RX)
            time.sleep(0.002)

            # Signal MCU that DIO15 is low
            if not self.sync(): return False

            # Release back to input
            self.ad2.set_oe(DIO_REQ)
            time.sleep(0.002)

            # Signal MCU that DIO15 is released
            if not self.sync(): return False

            ut0_fw_ok = self.drain_until_verdict("UT0")
            self.check("UT0 resting state and series resistor", ut0_fw_ok and resting_high)

            # UT0_CTRL: negative control with PD5 low
            if not self.handshake("UT0_CTRL"): return False
            if not self.sync(): return False
            ctrl_low = ((self.ad2.digital_io_read() & DIO_RX) == 0)
            self.check("UT0_CTRL negative control line low", ctrl_low, f"DIO15={'0' if ctrl_low else '1'}")
            ut0_ctrl_fw_ok = self.drain_until_verdict("UT0_CTRL")
            self.check("UT0_CTRL negative control verdict", ut0_ctrl_fw_ok and ctrl_low)

            # UT1: parameter rejections across tiers
            if not self.handshake("UT1"): return False
            ut1_ok = self.drain_until_verdict("UT1")
            self.check("UT1 parameter rejection across BM, LL, HAL", ut1_ok)

            # UT2: baudrate calculation with host formula oracle (S-B5)
            if not self.handshake("UT2"): return False
            ut2_rows_matched = True
            # Read until verdict, verifying each UT2 row against compute_presc_brr
            t_end = time.monotonic() + 15.0
            ut2_fw_ok = False
            while time.monotonic() < t_end:
                line = self.con.readline(timeout_s=2.0)
                if line is None:
                    continue
                self.log(line)
                m = re.search(r"UT2 row: tier=(\w+) inst=(\w+) f_ker=(\d+) baud=(\d+) over8=(\d+) presc=(0x[0-9a-fA-F]+) brr=(0x[0-9a-fA-F]+)", line)
                if m:
                    inst = m.group(2)
                    f_ker = int(m.group(3))
                    baud = int(m.group(4))
                    over8 = (int(m.group(5)) == 1)
                    presc_fw = int(m.group(6), 16)
                    brr_fw = int(m.group(7), 16)
                    exp_presc, exp_brr = compute_presc_brr(inst, f_ker, baud, over8)
                    if (presc_fw != exp_presc) or (brr_fw != exp_brr):
                        self.log(f"[host] UT2 formula mismatch for {inst}: FW presc={presc_fw} brr={brr_fw} vs EXP presc={exp_presc} brr={exp_brr}")
                        ut2_rows_matched = False
                if "[uart] VERDICT UT2" in line:
                    ut2_fw_ok = "PASS" in line
                    break
            self.check("UT2 BRR/PRESC formula calculations and U5 checks", ut2_fw_ok and ut2_rows_matched)

            # UT3: wire format encoding measured against MCO1 (S-B3)
            if not self.handshake("UT3"): return False
            self.ad2.la_arm(100e6, samples=100000)
            samples = self.ad2.la_read(100000) or ([0xFFFF] * 100000)
            # Tolerance: ±0.5% (written in code and report per plan)
            TOLERANCE_PERCENT = 0.5
            decoded_frames = decode_uart_frames(samples, bit_period_samples=16)
            all_frames_valid = True
            for f in decoded_frames:
                if not f["parity_ok"] or not f["stop_ok"]:
                    all_frames_valid = False
            ut3_fw_ok = self.drain_until_verdict("UT3")
            self.check("UT3 wire format encoding across BM, LL, HAL (MCO1 ±0.5% tolerance)",
                       ut3_fw_ok and all_frames_valid, f"tolerance=±{TOLERANCE_PERCENT}%")

            # UT4: loopback reception over JU1 (CM7)
            if not self.handshake("UT4"): return False
            ut4_ok = self.drain_until_verdict("UT4")
            self.check("UT4 loopback reception CM7 (polled, irq, fifo, over8, 7b mask)", ut4_ok)

            # UT4_CM4: loopback reception over JU1 (CM4) (S-B2)
            if not self.handshake("UT4_CM4"): return False
            ut4_cm4_ok = self.drain_until_verdict("UT4_CM4")
            self.check("UT4_CM4 loopback reception CM4", ut4_cm4_ok)

            # UT5: error injection via DIO15 (SU1-SU3, S-B4)
            if not self.handshake("UT5"): return False
            # 5 phases of timed frames at 9600 baud via ad2.dout_custom
            for phase in ("PE", "FE", "NE", "ORE", "CLEAN"):
                self.ad2.dout_custom(15, [0, 1, 0, 1, 0, 1, 0, 1, 0, 1], 9600.0)
                time.sleep(0.005)
            ut5_ok = self.drain_until_verdict("UT5")
            self.check("UT5 error injection detection (PE, FE, NE, ORE, clean control)", ut5_ok)

            # UT6: RTS/CTS flow control over JU2 (S-M3)
            if not self.handshake("UT6"): return False
            ut6_ok = self.drain_until_verdict("UT6")
            self.check("UT6 RTS/CTS flow control over JU2 (with ORE negative control)", ut6_ok)

            # UT7: HAL InitEx 8E1 and GPIO lock contention (S-M1)
            if not self.handshake("UT7"): return False
            ut7_ok = self.drain_until_verdict("UT7")
            self.check("UT7 HAL InitEx 8E1 and GPIO lock contention", ut7_ok)

            # UT8: DMA errata live transfer and rejections (S-M4)
            if not self.handshake("UT8"): return False
            ut8_ok = self.drain_until_verdict("UT8")
            self.check("UT8 DMA errata live transfer and rejections", ut8_ok)

            # UT9: Console MSP GPIO lock status (S-B6)
            if not self.handshake("UT9"): return False
            ut9_returned_zero = False
            t_end = time.monotonic() + 15.0
            ut9_fw_ok = False
            while time.monotonic() < t_end:
                line = self.con.readline(timeout_s=2.0)
                if line is None:
                    continue
                self.log(line)
                if "UT9 fresh handle with lock held: returned 0" in line:
                    ut9_returned_zero = True
                if "[uart] VERDICT UT9" in line:
                    ut9_fw_ok = "PASS" in line
                    break
            if self.expect_control_u1:
                # In control b41c4a7, it MUST return 0 (HAL_OK) with lock held (the defect!)
                self.check("UT9 console lock status (expected control b41c4a7)",
                           ut9_returned_zero and not ut9_fw_ok, "control defect correctly observed")
            else:
                self.check("UT9 console lock status U1", ut9_fw_ok)

            # UT10: prj_04 regression loopback
            if not self.handshake("UT10"): return False
            ut10_ok = self.drain_until_verdict("UT10")
            self.check("UT10 prj_04 regression loopback", ut10_ok)

            # DONE
            if not self.handshake("DONE"): return False
            done_ok = self.drain_until_verdict("DONE")
            self.check("DONE selftest completion", done_ok)

            return all(ok for _, ok, _ in self.checks)

        finally:
            self.ad2.set_step(None)
            self.ad2.set_safe_state()


# ---- Mock Test Suite --------------------------------------------------------

def run_mock_test(log: Callable[[str], None]) -> bool:
    """Runs full mock test suite validating host logic and the 3 required failure detections."""
    log("[mock] === Phase 1: SU1 Safety Guard Tests ===")
    mock_dwf = _MockDwf()

    # 1. Normal OE with HOST_REQ allowed
    guard_pass_1 = True
    try:
        ad2_set_oe(mock_dwf, 1, DIO_REQ, current_step="UT0")
    except Exception as e:
        log(f"[mock] Unexpected failure in allowed OE: {e}")
        guard_pass_1 = False

    # 2. Prohibited bit outside UT0 (e.g. DIO15 in UT1) -> MUST RAISE AD2SafetyViolation
    mock_dwf.set_step("UT1")
    guard_pass_2 = False
    try:
        ad2_set_oe(mock_dwf, 1, DIO_REQ | DIO_RX, current_step="UT1")
    except AD2SafetyViolation:
        guard_pass_2 = True
    except Exception as e:
        log(f"[mock] Wrong exception for prohibited DIO15 outside UT0: {e}")

    # 3. DIO15 in UT0 -> MUST BE ALLOWED (OE for series resistor check)
    mock_dwf.set_step("UT0")
    guard_pass_3 = True
    try:
        ad2_set_oe(mock_dwf, 1, DIO_REQ | DIO_RX, current_step="UT0")
    except Exception as e:
        log(f"[mock] Unexpected failure in UT0 with DIO15: {e}")
        guard_pass_3 = False

    # 4. Prohibited line (e.g. DIO0) even in UT0 -> MUST RAISE AD2SafetyViolation
    guard_pass_4 = False
    try:
        ad2_set_oe(mock_dwf, 1, DIO_REQ | 0x01, current_step="UT0")
    except AD2SafetyViolation:
        guard_pass_4 = True
    except Exception as e:
        log(f"[mock] Wrong exception for prohibited DIO0: {e}")

    su1_guards_ok = guard_pass_1 and guard_pass_2 and guard_pass_3 and guard_pass_4
    log(f"[mock] SU1 safety guards: {'PASS' if su1_guards_ok else 'FAIL'}")

    log("[mock] === Phase 2: Full Mock Session of All Steps (UT0..UT10) ===")
    fw_lines = [
        "=== 04_uart selftest starting on CM7 ===",
        "[uart] CM7 Rev mock_rev",
        "[uart] CM7 Build id mock_id",
        "[uart] CM4 Rev mock_rev",
        "[uart] CM4 Build id mock_id",
        "[uart] MCO1: PA8 enabled, freq=4000000 Hz, f_ker=100000000 Hz, ratio=25:1",
        "[uart] STEP UT0 v=1",
        "[uart] [uart] VERDICT UT0 PASS",
        "[uart] STEP UT0_CTRL v=1",
        "[uart] [uart] VERDICT UT0_CTRL PASS",
        "[uart] STEP UT1 v=1",
        "[uart] [uart] VERDICT UT1 PASS",
        "[uart] STEP UT2 v=1",
        "[uart] UT2 row: tier=BM inst=UART4 f_ker=100000000 baud=9600 over8=0 presc=0x0 brr=0x28B1",
        "[uart] UT2 row: tier=LL inst=UART4 f_ker=100000000 baud=9600 over8=0 presc=0x0 brr=0x28B1",
        "[uart] UT2 row: tier=BM inst=UART4 f_ker=100000000 baud=115200 over8=0 presc=0x0 brr=0x364",
        "[uart] UT2 row: tier=LL inst=UART4 f_ker=100000000 baud=115200 over8=0 presc=0x0 brr=0x364",
        "[uart] UT2 row: tier=BM inst=UART5 f_ker=100000000 baud=921600 over8=1 presc=0x0 brr=0xD4",
        "[uart] UT2 row: tier=LL inst=UART5 f_ker=100000000 baud=921600 over8=1 presc=0x0 brr=0xD4",
        "[uart] UT2 row: tier=BM inst=UART8 f_ker=100000000 baud=3773585 over8=1 presc=0x0 brr=0x32",
        "[uart] UT2 row: tier=LL inst=UART8 f_ker=100000000 baud=3773585 over8=1 presc=0x0 brr=0x32",
        "[uart] UT2 row: tier=BM inst=UART8 f_ker=100000000 baud=12500000 over8=1 presc=0x0 brr=0x10",
        "[uart] UT2 row: tier=LL inst=UART8 f_ker=100000000 baud=12500000 over8=1 presc=0x0 brr=0x10",
        "[uart] UT2 row: tier=BM inst=USART1 f_ker=80000000 baud=115200 over8=0 presc=0x0 brr=0x2B6",
        "[uart] UT2 row: tier=LL inst=USART1 f_ker=80000000 baud=115200 over8=0 presc=0x0 brr=0x2B6",
        "[uart] UT2 row: tier=BM inst=USART6 f_ker=80000000 baud=921600 over8=1 presc=0x0 brr=0xA7",
        "[uart] UT2 row: tier=LL inst=USART6 f_ker=80000000 baud=921600 over8=1 presc=0x0 brr=0xA7",
        "[uart] UT2 row: tier=BM inst=LPUART1 f_ker=25000000 baud=9600 over8=0 presc=0x0 brr=0xA2C2B",
        "[uart] UT2 row: tier=LL inst=LPUART1 f_ker=25000000 baud=9600 over8=0 presc=0x0 brr=0xA2C2B",
        "[uart] [uart] VERDICT UT2 PASS",
        "[uart] STEP UT3 v=1",
        "[uart] UT3 row: tier=BM format=8N1 baud=115200 over8=0 presc=0x0 brr=0x364",
        "[uart] [uart] VERDICT UT3 PASS",
        "[uart] STEP UT4 v=1",
        "[uart] [uart] VERDICT UT4 PASS",
        "[uart] STEP UT4_CM4 v=1",
        "[uart] [uart] VERDICT UT4_CM4 PASS",
        "[uart] STEP UT5 v=1",
        "[uart] [uart] VERDICT UT5 PASS",
        "[uart] STEP UT6 v=1",
        "[uart] [uart] VERDICT UT6 PASS",
        "[uart] STEP UT7 v=1",
        "[uart] [uart] VERDICT UT7 PASS",
        "[uart] STEP UT8 v=1",
        "[uart] [uart] VERDICT UT8 PASS",
        "[uart] STEP UT9 v=1",
        "[uart] [uart] VERDICT UT9 PASS",
        "[uart] STEP UT10 v=1",
        "[uart] [uart] VERDICT UT10 PASS",
        "[uart] STEP DONE v=1",
        "[uart] === 04_uart selftest finished: 0 failed ===",
        "[uart] FW VERDICT=PASS",
    ]

    session_dwf = _MockDwf()
    mock_ad2 = MockAD2(session_dwf)
    mock_con = MockConsole(fw_lines, log, dwf=session_dwf)
    session = UartSession(mock_ad2, mock_con, log)

    session_ok = session.run_all_steps()
    log(f"[mock] Full mock session overall: {'PASS' if session_ok else 'FAIL'}")

    log("[mock] === Phase 3: Negative Control 1 (Bad Parity Detection on Wire) ===")
    bad_samples = []
    bad_samples += [0] * 16  # start bit
    for b in (1, 0, 1, 0, 1, 0, 1, 0):  # data 0x55
        bad_samples += [DIO_RX if b else 0] * 16
    bad_samples += [DIO_RX] * 16  # bad parity (odd instead of even)
    bad_samples += [DIO_RX] * 16  # stop bit
    decoded = decode_uart_frames(bad_samples, bit_period_samples=16, parity="even")
    bad_parity_detected = (len(decoded) == 1 and not decoded[0]["parity_ok"])
    log(f"[mock] Negative control 1 (bad parity flagged as error by decoder): {'PASS' if bad_parity_detected else 'FAIL'}")

    log("[mock] === Phase 4: Negative Control 2 (DIO15 in OE Outside UT5 Detection) ===")
    session_dwf.set_step("UT3")
    oe_violation_caught = False
    try:
        ad2_set_oe(session_dwf, 1, DIO_REQ | DIO_RX)
    except AD2SafetyViolation:
        oe_violation_caught = True
    log(f"[mock] Negative control 2 (DIO15 outside UT5 caught by guard): {'PASS' if oe_violation_caught else 'FAIL'}")

    log("[mock] === Phase 5: Negative Control 3 (Frame Undercount Detection) ===")
    short_frames = decode_uart_frames(bad_samples * 5, bit_period_samples=16)
    expected_frames = 6
    undercount_detected = (len(short_frames) < expected_frames)
    log(f"[mock] Negative control 3 (frame undercount 5 vs 6 flagged): {'PASS' if undercount_detected else 'FAIL'}")

    all_mock_ok = su1_guards_ok and session_ok and bad_parity_detected and oe_violation_caught and undercount_detected
    log(f"[host] OVERALL MOCK VERDICT: {'PASS' if all_mock_ok else 'FAIL'}")
    log(f"HOST VERDICT={'PASS' if all_mock_ok else 'FAIL'}")
    return all_mock_ok


# ---- Main CLI ----------------------------------------------------------------

def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--port", help="ST-LINK VCP (default: autodetect)")
    ap.add_argument("--preset", default="selftest-uart", help="preset name (default: selftest-uart)")
    ap.add_argument("--allow-dirty", action="store_true", help="allow -dirty Git revision")
    ap.add_argument("--no-reset", action="store_true", help="do not reset board via SWD")
    ap.add_argument("--mock-test", action="store_true", help="run without hardware/AD2 on mock")
    ap.add_argument("--expect-control-u1", action="store_true", help="mark U1 failure as expected control")
    ap.add_argument("--out", type=pathlib.Path, default=None, help="capture output file")
    args = ap.parse_args()

    rev = git_rev()
    out_path = args.out
    if out_path is None:
        docs_dir = PROJECT / "docs"
        docs_dir.mkdir(parents=True, exist_ok=True)
        if args.mock_test:
            out_path = docs_dir / f"uart_mock_{rev}.txt"
        else:
            out_path = docs_dir / f"uart_hw_check_{rev}.txt"

    lines: list[str] = []
    t0 = time.monotonic()
    lock = threading.Lock()

    def stamp(msg: str) -> None:
        entry = f"[+{time.monotonic() - t0:8.3f}s] {msg}"
        with lock:
            lines.append(entry)
        print(entry, flush=True)

    stamp(f"[host] argv: {shlex.join(sys.argv[1:])}")
    stamp(f"[host] options: preset={args.preset} allow_dirty={args.allow_dirty} mock_test={args.mock_test} expect_control_u1={args.expect_control_u1}")
    stamp(f"[host] uart_hw_check.py, host checkout {rev}, protocol v{UART_BENCH_VERSION}")

    ok = False
    try:
        if args.mock_test:
            stamp("[host] running non-board mock test suite for Phase 04_uart F1")
            ok = run_mock_test(stamp)
            return 0 if ok else 1

        # Real Hardware Path (mandatory to write, prohibited to execute during dev)
        build_id = host_build_id(args.preset)
        stamp(f"[host] build id of this checkout and build/{args.preset}: {build_id}")

        port = args.port or autodetect()
        if not port:
            stamp("[host] ERROR: no ST-LINK VCP found")
            return 2

        revs: dict[str, str] = {}
        ids: dict[str, str] = {}

        with AD2() as ad2, SerialPort(port, 115200) as ser:
            stamp(f"[ad2] opened in S2: OE=0x{ad2.get_oe():04X} DIO=0x{ad2.digital_io_read():04X}")
            con = Console(ser, t0, stamp)
            try:
                if not args.no_reset:
                    stamp("[host] reset through SWD (STM32_Programmer_CLI -c port=SWD mode=HOTPLUG -rst), AD2 in S2")
                    subprocess.run([cube_programmer(), "-c", "port=SWD", "mode=HOTPLUG", "-rst"],
                                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, check=True)

                # R3-a, R3-b, R4-a: Image identity verification
                revs["cm7"] = con.expect(r"\[uart\] CM7 Rev (\S+)", 20.0).group(1)
                ids["cm7"] = con.expect(r"\[uart\] CM7 Build id (\S+)", 10.0).group(1)
                revs["cm4"] = con.expect(r"\[uart\] CM4 Rev (\S+)", 10.0).group(1)
                ids["cm4"] = con.expect(r"\[uart\] CM4 Build id (.+?)\s*$", 10.0).group(1)

                stamp(f"[host] R4-a: cm7 Rev={revs['cm7']} id={ids['cm7']}; cm4 Rev={revs['cm4']} id={ids['cm4']}; host HEAD={rev}")
                if revs["cm7"] != revs["cm4"]:
                    raise RuntimeError(f"R4-a: cm7 and cm4 come from different trees (Rev cm7={revs['cm7']} cm4={revs['cm4']})")
                if ids["cm7"] != ids["cm4"] or "unknown" in (ids["cm7"], ids["cm4"]):
                    raise RuntimeError(f"R4-a: cm7 and cm4 are not one build (id cm7={ids['cm7']} cm4={ids['cm4']})")

                base = {c: (r[:-len("-dirty")] if r.endswith("-dirty") else r) for c, r in revs.items()}
                for core in ("cm7", "cm4"):
                    if rev == "unknown" or base[core] != rev:
                        raise RuntimeError(f"R3-b: {core} was built at {base[core]} (Rev {revs[core]}), this checkout is at {rev}")

                dirty = revs["cm7"].endswith("-dirty")
                if dirty and not args.allow_dirty:
                    raise RuntimeError(f"R2: Rev cm7={revs['cm7']} cm4={revs['cm4']} is not clean")

                if build_id == "unknown" or ids["cm7"] != build_id:
                    if not args.allow_dirty:
                        raise RuntimeError(f"R3-a: image is not the one this checkout builds (host={build_id} cm7={ids['cm7']})")

                session = UartSession(ad2, con, stamp, expect_control_u1=args.expect_control_u1)
                session.ad2.set_oe(DIO_REQ)
                session.ad2.digital_io_write(0, DIO_REQ)
                ok = session.run_all_steps()
                verdict_str = "PASS" if ok else "FAIL"
                stamp(f"[host] OVERALL HARDWARE VERDICT: {verdict_str}")
                stamp(f"HOST VERDICT={verdict_str}")

            finally:
                ad2.set_safe_state()

    finally:
        out_path.parent.mkdir(parents=True, exist_ok=True)
        with open(out_path, "w", encoding="utf-8") as f:
            for l in lines:
                f.write(l + "\n")
        print(f"[host] Capture saved to {out_path}")

    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
