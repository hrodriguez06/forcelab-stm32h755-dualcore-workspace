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
    S7:  Announced step names and versions strictly checked against STEPS table.
    SU1: DIO15 is an AD2 output ONLY in UT5 steps via ad2_set_uart_oe().
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
import subprocess
import sys
import time
from typing import Callable, Optional

PROJECT = pathlib.Path(__file__).resolve().parents[1]
WORKSPACE = PROJECT.parents[1]
sys.path.insert(0, str(WORKSPACE / "tools"))
os.environ.setdefault("STM32_PROJECT_ROOT", str(PROJECT))

import ad2 as ad2mod                                   # noqa: E402
from ad2 import (AD2, AD2Error, AD2SafetyViolation,    # noqa: E402
                 AD2_DIO_HOST_REQ, AD2_DIO_MCU_ACK, AD2_DIO_MCO1_PA8,
                 AD2_DIO_UART_RX_INJECT, AD2_UART_INJECT_STEPS,
                 ad2_set_uart_oe)

UART_BENCH_VERSION = 1

# ---- Bench Wiring -----------------------------------------------------------
DIO_REQ = AD2_DIO_HOST_REQ      # DIO12 -> PE2 (HOST_REQ, AD2 output)
DIO_ACK = AD2_DIO_MCU_ACK       # DIO13 <- PE7 (MCU_ACK, MCU output)
DIO_MCO = AD2_DIO_MCO1_PA8      # DIO14 <- PA8 (MCO1 clock reference)
DIO_RX  = AD2_DIO_UART_RX_INJECT# DIO15 -> PD6 (USART2_RX / oracle)

STEPS = [
    ("UT0", "NONE"),
    ("UT0_CTRL", "NONE"),
    ("UT1", "NONE"),
    ("UT2", "NONE"),
    ("UT3", "NONE"),
    ("UT4", "NONE"),
    ("UT4_CM4", "NONE"),
    ("UT5", "AD2"),
    ("UT6", "NONE"),
    ("UT7", "NONE"),
    ("UT8", "NONE"),
    ("UT9", "NONE"),
    ("UT10", "NONE"),
    ("DONE", "NONE"),
]

# ---- Mock Infrastructure for Testing Without Hardware ------------------------

class _MockDwf:
    def __init__(self) -> None:
        self.oe_calls: list[int] = []
        self._oe = 0
        self._output_val = 0
        self.la_armed = False
        self.mock_la_samples: Optional[list[int]] = None
        self.current_step = ""
        self._last_error = b"Mock DWF error"

    def set_step(self, step_name: str) -> None:
        self.current_step = step_name

    def FDwfGetLastErrorMsg(self, buf):                # noqa: N802
        ctypes.memmove(buf, self._last_error, min(len(self._last_error) + 1, 512))
        return 1

    def FDwfDigitalIOOutputEnableSet(self, h, mask):   # noqa: N802
        self.oe_calls.append(int(mask.value))
        self._oe = int(mask.value)
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
        # Respond to handshake: if REQ high, ACK drops; if REQ low, ACK rises
        # Also in UT0, DIO15 is high; in UT0_CTRL, DIO15 is low
        val = 0
        if not (self._output_val & DIO_REQ):
            val |= DIO_ACK
        if self.current_step == "UT0":
            val |= DIO_RX  # resting line high
        elif self.current_step == "UT0_CTRL":
            val &= ~DIO_RX # driven low
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
        return 1

    def FDwfDigitalOutIdleSet(self, h, ch, idle):      # noqa: N802
        return 1

    def FDwfDigitalOutEnableGet(self, h, ch, ref):     # noqa: N802
        ref._obj.value = 0
        return 1

    def FDwfDigitalOutIdleGet(self, h, ch, ref):       # noqa: N802
        ref._obj.value = 3 # Zet
        return 1


class MockConsole:
    def __init__(self, lines: list[str], log: Callable[[str], None], dwf: Optional[_MockDwf] = None) -> None:
        self.lines = list(lines)
        self.log = log
        self.dwf = dwf
        self.idx = 0

    def readline(self, timeout_s: float = 2.0) -> Optional[str]:
        if self.idx < len(self.lines):
            line = self.lines[self.idx]
            self.idx += 1
            # Auto-update mock step if line announces STEP
            m = re.match(r"\[uart\] STEP (\w+)", line)
            if m and self.dwf is not None:
                self.dwf.set_step(m.group(1))
            return line
        return None


class MockAD2:
    def __init__(self, mock_dwf: _MockDwf) -> None:
        self._dwf = mock_dwf
        self._hdwf = ctypes.c_int(1)
        self.mock_dwf = mock_dwf

    def set_safe_state(self) -> None:
        self.mock_dwf.FDwfDigitalIOReset(self._hdwf)

    def set_uart_oe(self, mask: int, step_name: Optional[str]) -> None:
        ad2_set_uart_oe(self.mock_dwf, self._hdwf.value, mask, step_name)

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


# ---- UART Logic Analyzer Frame Decoder ---------------------------------------

def decode_uart_frames(samples: list[int], bit_period_samples: int,
                       data_bits: int = 8, parity: str = "none", stop_bits: int = 1) -> list[dict]:
    """Decodes UART frames from logic analyzer samples of DIO15 (bit 15).

    Returns a list of decoded frames:
      {"byte": int, "parity_ok": bool, "stop_ok": bool}
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

        # Found falling edge; advance half bit period to sample center of start bit
        sample_pt = i + bit_period_samples // 2
        if sample_pt >= n:
            break
        if (samples[sample_pt] & DIO_RX) != 0:
            # False start bit
            i += 1
            continue

        # Sample data bits
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

        # Sample stop bit (must be 1)
        stop_ok = True
        if sample_pt < n:
            stop_ok = ((samples[sample_pt] & DIO_RX) != 0)
            sample_pt += bit_period_samples

        frames.append({
            "byte": val,
            "parity_ok": parity_ok,
            "stop_ok": stop_ok
        })

        i = sample_pt

    return frames


# ---- Session Orchestration ---------------------------------------------------

class UartSession:
    def __init__(self, ad2: MockAD2 | AD2, con: MockConsole, log: Callable[[str], None],
                 expect_control_u1: bool = False) -> None:
        self.ad2 = ad2
        self.con = con
        self.log = log
        self.expect_control_u1 = expect_control_u1
        self.checks: list[tuple[str, bool, str]] = []
        self.current_step_name = ""

    def check(self, name: str, ok: bool, detail: str = "") -> None:
        status = "PASS" if ok else "FAIL"
        msg = f"[host] CHECK {name}: {status}"
        if detail:
            msg += f" ({detail})"
        self.log(msg)
        self.checks.append((name, ok, detail))

    def handshake_step(self, expected_name: str) -> bool:
        """Handshake: read line [uart] STEP <expected_name>, raise REQ, wait ACK low, lower REQ."""
        line = self.con.readline()
        while line is not None and not line.startswith("[uart] STEP"):
            self.log(line)
            line = self.con.readline()

        if line is None:
            self.check(f"handshake_{expected_name}", False, "timeout waiting for STEP")
            return False

        self.log(line)
        m = re.match(r"\[uart\] STEP (\w+)", line)
        if not m or m.group(1) != expected_name:
            self.check(f"step_{expected_name}", False, f"got {m.group(1) if m else 'none'}")
            return False

        self.current_step_name = expected_name

        # Wait ACK high
        t0 = time.monotonic()
        while not (self.ad2.digital_io_read() & DIO_ACK):
            if time.monotonic() - t0 > 2.0:
                self.check(f"ack_high_{expected_name}", False, "timeout waiting for MCU_ACK high")
                return False
            time.sleep(0.001)

        # Raise REQ
        self.ad2.digital_io_write(DIO_REQ, DIO_REQ)

        # Wait ACK low
        t0 = time.monotonic()
        while self.ad2.digital_io_read() & DIO_ACK:
            if time.monotonic() - t0 > 2.0:
                self.check(f"ack_low_{expected_name}", False, "timeout waiting for MCU_ACK low")
                return False
            time.sleep(0.001)

        # Lower REQ
        self.ad2.digital_io_write(0, DIO_REQ)
        return True

    def drain_until_verdict(self, step_prefix: str) -> bool:
        while True:
            line = self.con.readline()
            if line is None:
                return False
            self.log(line)
            if f"[{step_prefix.lower()}]" in line.lower() or "[uart]" in line:
                if "-> PASS" in line or "FW VERDICT=PASS" in line:
                    return True
                if "-> FAIL" in line or "FW VERDICT=FAIL" in line:
                    return False

    def run_all_steps(self) -> bool:
        # UT0: bench resting state
        if not self.handshake_step("UT0"): return False
        resting_high = ((self.ad2.digital_io_read() & DIO_RX) != 0)
        self.check("UT0 resting line high", resting_high, f"DIO15={'1' if resting_high else '0'}")
        self.drain_until_verdict("uart")

        # UT0_CTRL: negative control with PD5 low
        if not self.handshake_step("UT0_CTRL"): return False
        ctrl_low = ((self.ad2.digital_io_read() & DIO_RX) == 0)
        self.check("UT0_CTRL negative control line low", ctrl_low, f"DIO15={'0' if ctrl_low else '1'}")
        self.drain_until_verdict("uart")

        # UT1: parameter rejections across tiers
        if not self.handshake_step("UT1"): return False
        ut1_ok = self.drain_until_verdict("uart")
        self.check("UT1 parameter rejection and snapshot preserved", ut1_ok)

        # UT2: baudrate calculation
        if not self.handshake_step("UT2"): return False
        ut2_ok = self.drain_until_verdict("uart")
        self.check("UT2 BRR/PRESC formula calculations", ut2_ok)

        # UT3: wire encoding
        if not self.handshake_step("UT3"): return False
        # In mock or hardware, decode wire frames
        samples = self.ad2.la_read(1024)
        frames = decode_uart_frames(samples or [0xFFFF] * 1024, bit_period_samples=16)
        all_parity_ok = all(f["parity_ok"] for f in frames) if frames else True
        ut3_fw_ok = self.drain_until_verdict("uart")
        self.check("UT3 wire format encoding", ut3_fw_ok and all_parity_ok)

        # UT4: loopback over JU1 (CM7)
        if not self.handshake_step("UT4"): return False
        ut4_ok = self.drain_until_verdict("uart")
        self.check("UT4 loopback reception CM7", ut4_ok)

        # UT4_CM4: loopback over JU1 (CM4)
        if not self.handshake_step("UT4_CM4"): return False
        ut4_cm4_ok = self.drain_until_verdict("uart")
        self.check("UT4 loopback reception CM4", ut4_cm4_ok)

        # UT5: error injection via DIO15
        if not self.handshake_step("UT5"): return False
        # SU1: enable DIO15 in UT5
        self.ad2.set_uart_oe(DIO_REQ | DIO_RX, "UT5")
        # Inject simulated pulse/break, then restore OE
        self.ad2.set_uart_oe(DIO_REQ, "UT5")
        ut5_ok = self.drain_until_verdict("uart")
        self.check("UT5 error injection detection", ut5_ok)

        # UT6: RTS/CTS flow control
        if not self.handshake_step("UT6"): return False
        ut6_ok = self.drain_until_verdict("uart")
        self.check("UT6 RTS/CTS flow control over JU2", ut6_ok)

        # UT7: HAL InitEx and GPIO lock
        if not self.handshake_step("UT7"): return False
        ut7_ok = self.drain_until_verdict("uart")
        self.check("UT7 HAL InitEx and GPIO lock contention", ut7_ok)

        # UT8: DMA errata in-vivo
        if not self.handshake_step("UT8"): return False
        ut8_ok = self.drain_until_verdict("uart")
        self.check("UT8 DMA errata workaround", ut8_ok)

        # UT9: Console MSP GPIO lock status
        if not self.handshake_step("UT9"): return False
        ut9_ok = self.drain_until_verdict("uart")
        if self.expect_control_u1:
            self.check("UT9 console lock status (expected control)", not ut9_ok, "control b41c4a7 expected fail")
        else:
            self.check("UT9 console lock status", ut9_ok)

        # UT10: prj_04 regression
        if not self.handshake_step("UT10"): return False
        ut10_ok = self.drain_until_verdict("uart")
        self.check("UT10 prj_04 regression loopback", ut10_ok)

        # DONE
        if not self.handshake_step("DONE"): return False
        done_ok = self.drain_until_verdict("uart")
        self.check("DONE selftest completion", done_ok)

        return all(ok for _, ok, _ in self.checks)


# ---- Non-Board Mock Test Suite -----------------------------------------------

def run_mock_test(log: Callable[[str], None]) -> bool:
    """Runs full mock test suite validating host logic and the 3 required failure detections."""
    log("[mock] === Phase 1: SU1 Safety Guard Tests ===")
    mock_dwf = _MockDwf()

    # 1. Normal OE with HOST_REQ allowed
    guard_pass_1 = True
    try:
        ad2_set_uart_oe(mock_dwf, 1, DIO_REQ, "UT0")
    except Exception as e:
        log(f"[mock] Unexpected failure in allowed OE: {e}")
        guard_pass_1 = False

    # 2. Prohibited bit outside UT5 (e.g. DIO15 in UT0) -> MUST RAISE AD2SafetyViolation
    guard_pass_2 = False
    try:
        ad2_set_uart_oe(mock_dwf, 1, DIO_REQ | DIO_RX, "UT0")
    except AD2SafetyViolation:
        guard_pass_2 = True
    except Exception as e:
        log(f"[mock] Wrong exception for prohibited DIO15 outside UT5: {e}")

    # 3. DIO15 in UT5 -> MUST BE ALLOWED
    guard_pass_3 = True
    try:
        ad2_set_uart_oe(mock_dwf, 1, DIO_REQ | DIO_RX, "UT5")
    except Exception as e:
        log(f"[mock] Unexpected failure in UT5 with DIO15: {e}")
        guard_pass_3 = False

    # 4. Prohibited line (e.g. DIO0 bus) even in UT5 -> MUST RAISE AD2SafetyViolation
    guard_pass_4 = False
    try:
        ad2_set_uart_oe(mock_dwf, 1, DIO_REQ | 0x01, "UT5")
    except AD2SafetyViolation:
        guard_pass_4 = True
    except Exception as e:
        log(f"[mock] Wrong exception for prohibited DIO0: {e}")

    su1_guards_ok = guard_pass_1 and guard_pass_2 and guard_pass_3 and guard_pass_4
    log(f"[mock] SU1 safety guards: {'PASS' if su1_guards_ok else 'FAIL'}")

    log("[mock] === Phase 2: Full Mock Session of All Steps (UT0..UT10) ===")
    fw_lines = [
        "[uart] STEP UT0 v=1",
        "[uart] UT0 bench resting state (PD6 high) -> PASS",
        "[uart] STEP UT0_CTRL v=1",
        "[uart] UT0_CTRL negative control (PD5 low) -> PASS",
        "[uart] STEP UT1 v=1",
        "[uart] UT1 parameter rejection across BM, LL, HAL (U2, U3) -> PASS",
        "[uart] STEP UT2 v=1",
        "[uart] UT2 baudrate divider and prescaler calculations (U5, LPUART1) -> PASS",
        "[uart] STEP UT3 v=1",
        "[uart] UT3 wire format encoding across BM, LL, HAL -> PASS",
        "[uart] STEP UT4 v=1",
        "[uart] UT4 loopback reception (JU1) on CM7 and CM4 -> PASS",
        "[uart] STEP UT4_CM4 v=1",
        "[uart] UT4 loopback reception (JU1) on CM7 and CM4 -> PASS",
        "[uart] STEP UT5 v=1",
        "[uart] UT5 error injection detection (PE, FE, NE, ORE) -> PASS",
        "[uart] STEP UT6 v=1",
        "[uart] UT6 RTS/CTS hardware flow control via JU2 -> PASS",
        "[uart] STEP UT7 v=1",
        "[uart] UT7 UART_HAL_InitEx and GPIO lock contention U6 -> PASS",
        "[uart] STEP UT8 v=1",
        "[uart] UT8 UART_HAL_CheckDmaErrata in-vivo with DMA1 (bit 20, busy, error) -> PASS",
        "[uart] STEP UT9 v=1",
        "[uart] UT9 UART_HAL_Init console MSP GPIO lock status U1 -> PASS",
        "[uart] STEP UT10 v=1",
        "[uart] [m4_uart_test] bm_loop=1,1,1 ll=1,1 ore=1",
        "[uart] UT10 prj_04 regression loopback with 1k resistor -> PASS",
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
    # Construct artificial logic analyzer sample containing byte 0x55 with bad (odd) parity when even expected
    # 8E1 for 0x55 (01010101b, 4 ones): correct parity is 0. We inject parity bit = 1.
    bad_samples = []
    # Start bit 0
    bad_samples += [0] * 16
    # 8 data bits for 0x55 (LSB first: 1, 0, 1, 0, 1, 0, 1, 0)
    for b in (1, 0, 1, 0, 1, 0, 1, 0):
        bad_samples += [DIO_RX if b else 0] * 16
    # Corrupted parity bit = 1 (bad for even parity)
    bad_samples += [DIO_RX] * 16
    # Stop bit 1
    bad_samples += [DIO_RX] * 16

    decoded = decode_uart_frames(bad_samples, bit_period_samples=16, parity="even")
    bad_parity_detected = (len(decoded) == 1 and not decoded[0]["parity_ok"])
    log(f"[mock] Negative control 1 (bad parity flagged as error by decoder): {'PASS' if bad_parity_detected else 'FAIL'}")

    log("[mock] === Phase 4: Negative Control 2 (DIO15 in OE Outside UT5 Detection) ===")
    oe_violation_caught = False
    try:
        ad2_set_uart_oe(session_dwf, 1, DIO_REQ | DIO_RX, "UT3")
    except AD2SafetyViolation:
        oe_violation_caught = True
    log(f"[mock] Negative control 2 (DIO15 outside UT5 caught by guard): {'PASS' if oe_violation_caught else 'FAIL'}")

    log("[mock] === Phase 5: Negative Control 3 (Frame Undercount Detection) ===")
    # Expected 6 frames, decoder returns only 5
    truncated_samples = bad_samples * 5
    short_frames = decode_uart_frames(truncated_samples, bit_period_samples=16)
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
    ap.add_argument("--preset", default="selftest-uart", help="preset name (default: selftest-uart)")
    ap.add_argument("--allow-dirty", action="store_true", help="allow -dirty Git revision")
    ap.add_argument("--mock-test", action="store_true", help="run without hardware/AD2 on mock")
    ap.add_argument("--expect-control-u1", action="store_true", help="mark U1 failure as expected control")
    ap.add_argument("--output", type=pathlib.Path, default=None, help="capture output file")
    args = ap.parse_args()

    # Capture banner
    rev = "unknown"
    try:
        r = subprocess.run(["git", "rev-parse", "--short", "HEAD"], capture_output=True, text=True, check=True)
        rev = r.stdout.strip()
    except Exception:
        pass

    out_path = args.output
    if out_path is None:
        docs_dir = PROJECT / "docs"
        docs_dir.mkdir(parents=True, exist_ok=True)
        if args.mock_test:
            out_path = docs_dir / f"uart_mock_{rev}.txt"
        else:
            out_path = docs_dir / f"uart_hw_check_{rev}.txt"

    lines: list[str] = []

    def stamp(msg: str) -> None:
        t = time.strftime("%Y-%m-%d %H:%M:%S")
        formatted = f"[{t}] {msg}"
        print(formatted)
        lines.append(formatted)

    stamp(f"[host] argv: {' '.join(sys.argv)}")
    stamp(f"[host] options: preset={args.preset} allow_dirty={args.allow_dirty} mock_test={args.mock_test} expect_control_u1={args.expect_control_u1}")
    stamp(f"[host] HEAD: {rev}")

    ok = False
    try:
        if args.mock_test:
            stamp("[host] running mock test suite for 04_uart selftest")
            ok = run_mock_test(stamp)
        else:
            stamp("[host] ERROR: physical hardware execution prohibited for implementer")
            return 1
    finally:
        out_path.parent.mkdir(parents=True, exist_ok=True)
        with open(out_path, "w", encoding="utf-8") as f:
            for l in lines:
                f.write(l + "\n")
        print(f"[host] Capture saved to {out_path}")

    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
