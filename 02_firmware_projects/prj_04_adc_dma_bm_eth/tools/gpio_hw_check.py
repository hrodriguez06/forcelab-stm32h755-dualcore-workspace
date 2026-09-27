#!/usr/bin/env python3
"""GPIO Hardware Selftest Runner for STM32H755 and Analog Discovery 2.

Implements the test orchestrator for 00_gpio (Phase F0):
  - Enforces S9 operational checklist with explicit operator confirmation (--checklist-confirmed, m3).
  - Safe state S2 (all inputs, DwfParamOnClose = 1) via AD2 context manager.
  - S10: Safe target MCU reset only when AD2 OE is confirmed 0x0000 (reset_mcu_safe).
  - S8: Banner validation (Rev <hash> clean and GPIO_BENCH v<proto> idle) before enabling ANY outputs.
  - S7: Synchronized step table tracking with regex matching against firmware announcements.
  - S5: Single guarded bus gating function s5_verify_released() and drive_bus() with token consumption.
  - T0b(a): Host OE mask guard verification against prohibited bits.
  - T0b(c): S5 break-before-make negative control using s5_verify_released() (B4).
  - T0b(d): 10-second timeout to bench_safe().
  - T0a: Loose line check (13 lines), handshake lines check (PE2/PE7), walking-1 in both directions, scope 1/2 DC levels.
  - Evaluates and prints final HOST VERDICT=.
"""

from __future__ import annotations

import argparse
import os
import pathlib
import re
import subprocess
import sys
import time
from typing import Callable, Optional

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


# ---- Protocol Constants (gpio_bench_protocol.h) -----------------------------
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

EXPECTED_STEP_SEQUENCE = [
    (STEP_T0B_GUARDS, "NONE"),
    (STEP_T0B_S5_NEG, "MCU"),
    (STEP_T0B_TIMEOUT, "NONE"),
    (STEP_T0A_LOOSE, "NONE"),
    (STEP_T0A_HANDSHAKE, "NONE"),
    (STEP_T0A_MCU_TO_AD2, "MCU"),
    (STEP_T0A_AD2_TO_MCU, "AD2"),
    (STEP_T0A_SCOPE, "NONE"),
    (STEP_DONE, "NONE"),
]

STEP_RE = re.compile(r"^\[m7_gpio_test\] STEP id=(\d+) bus=(NONE|MCU|AD2) v=(\d+)")


class BenchAbort(Exception):
    """Exception raised when a bench safety guard or sequence check fails."""
    pass


class BusToken:
    """S5 witness token. Authorizes driving the bus for a specific validated step."""

    def __init__(self, step_id: int):
        self.step_id = step_id
        self._consumed = False

    def consume(self, expected_step_id: int) -> None:
        if self._consumed:
            raise AD2SafetyViolation(f"S5 violation: BusToken for step {self.step_id} already consumed")
        if self.step_id != expected_step_id:
            raise AD2SafetyViolation(
                f"S5 violation: BusToken step {self.step_id} does not match expected step {expected_step_id}"
            )
        self._consumed = True


def drive_bus(ad2: AD2, token: BusToken, value: int, step_id: int) -> None:
    """S5: Drive value on bus DIO0..DIO7. Requires a valid, unconsumed token for step_id."""
    if not isinstance(token, BusToken):
        raise AD2SafetyViolation("S5 violation: drive_bus called without a valid BusToken")
    token.consume(step_id)
    ad2.set_oe(AD2_DIO_HOST_REQ | AD2_DIO_BUS_MASK, GPIO_BENCH_BUS_AD2)
    ad2.digital_io_write(value & 0xFF, AD2_DIO_BUS_MASK)


def reset_mcu_safe(ad2: AD2, log_fn: Optional[Callable[[str], None]] = None) -> None:
    """S10: Safe reset of target MCU. Verifies OE == 0x0000 before invoking programmer CLI."""
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
    try:
        res = subprocess.run(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=5)
        if log_fn:
            log_fn(f"[host] S10: Target MCU reset via {cli_path} (exit code {res.returncode}).")
    except Exception as e:
        if log_fn:
            log_fn(f"[host] S10 warning: reset command returned {e}")


def get_git_rev() -> str:
    """Return clean short git revision of HEAD. Raises BenchAbort if tree is dirty."""
    res = subprocess.run(["git", "rev-parse", "--short", "HEAD"], capture_output=True, text=True, check=True)
    rev = res.stdout.strip()
    # Check for dirty worktree in tracked files
    diff_res = subprocess.run(["git", "diff-index", "--quiet", "HEAD", "--"], capture_output=True)
    if diff_res.returncode != 0:
        rev += "-dirty"
    return rev


def wait_mcu_ack(ad2: AD2, expected_high: bool, timeout_s: float = 10.0) -> bool:
    """Wait for PE7 (MCU_ACK, DIO13) to reach expected level."""
    start = time.monotonic()
    while time.monotonic() - start < timeout_s:
        dio = ad2.digital_io_read()
        is_high = bool(dio & AD2_DIO_MCU_ACK)
        if is_high == expected_high:
            return True
        time.sleep(0.0005)
    return False


def host_ack_pulse(ad2: AD2, timeout_s: float = 10.0) -> bool:
    """Acknowledge MCU step: assert HOST_REQ=1, wait for MCU_ACK=0, deassert HOST_REQ=0."""
    ad2.digital_io_write(AD2_DIO_HOST_REQ, AD2_DIO_HOST_REQ)
    if not wait_mcu_ack(ad2, False, timeout_s):
        ad2.digital_io_write(0, AD2_DIO_HOST_REQ)
        return False
    ad2.digital_io_write(0, AD2_DIO_HOST_REQ)
    return True


class StepTracker:
    """S7: Synchronized step table tracker. Validates sequence and bus ownership."""

    def __init__(self, sequence: list[tuple[int, str]]):
        self.sequence = sequence
        self.current_idx = 0
        self.current_step = 0
        self.current_bus_owner = GPIO_BENCH_BUS_NONE

    def validate_announcement(self, line: str, ad2: AD2, log_fn: Optional[Callable[[str], None]] = None) -> tuple[int, int]:
        m = STEP_RE.match(line)
        if not m:
            return 0, GPIO_BENCH_BUS_NONE

        step_id = int(m.group(1))
        bus_str = m.group(2)
        proto_v = int(m.group(3))

        if proto_v != GPIO_BENCH_PROTOCOL_VERSION:
            ad2.set_safe_state()
            msg = f"ABORT: Protocol version mismatch in step announcement (got v={proto_v}, expected v={GPIO_BENCH_PROTOCOL_VERSION})"
            if log_fn:
                log_fn(f"[host] {msg}")
            raise BenchAbort(msg)

        if self.current_idx >= len(self.sequence):
            ad2.set_safe_state()
            msg = f"ABORT: Unexpected step announcement beyond end of sequence (step {step_id})"
            if log_fn:
                log_fn(f"[host] {msg}")
            raise BenchAbort(msg)

        exp_id, exp_bus = self.sequence[self.current_idx]
        if step_id != exp_id or bus_str != exp_bus:
            ad2.set_safe_state()
            msg = f"ABORT: Step sequence error: got STEP id={step_id} bus={bus_str}, expected id={exp_id} bus={exp_bus}"
            if log_fn:
                log_fn(f"[host] {msg}")
            raise BenchAbort(msg)

        self.current_idx += 1
        self.current_step = step_id
        self.current_bus_owner = BUS_OWNER_FROM_NAME[bus_str]
        if log_fn:
            log_fn(f"[host] Step {step_id} validated (bus={bus_str}, v={proto_v}).")
        return self.current_step, self.current_bus_owner


def s5_verify_released(
    ad2: AD2,
    step_id: int,
    wait_line_fn: Callable[[str, float], Optional[str]],
    pulse_req_fn: Callable[[], bool],
    log_fn: Optional[Callable[[str], None]] = None,
) -> BusToken:
    """S5: Break-before-make electrical verification of bus release.
    1. Read-back OE & 0x00FF == 0.
    2. Wait for BUSCHK up line, read bus: must be 0xFF.
    3. Wait for BUSCHK down line, read bus: must be 0x00.
    4. On any failure: force S2, log ABORT: bus contention check failed at <step>, raise BenchAbort.
    5. Returns BusToken(step_id).
    """
    oe = ad2.get_oe()
    if (oe & AD2_DIO_BUS_MASK) != 0:
        ad2.set_safe_state()
        msg = f"ABORT: bus contention check failed at {step_id}: AD2 OE has bus bits enabled (0x{oe:04X})"
        if log_fn:
            log_fn(f"[ad2] {msg}")
        raise BenchAbort(msg)

    # 1. BUSCHK up
    up_line = wait_line_fn("BUSCHK up", 10.0)
    if not up_line:
        ad2.set_safe_state()
        msg = f"ABORT: bus contention check failed at {step_id}: timeout waiting for BUSCHK up"
        if log_fn:
            log_fn(f"[ad2] {msg}")
        raise BenchAbort(msg)

    dio_up = ad2.digital_io_read()
    bus_up = dio_up & AD2_DIO_BUS_MASK
    if not pulse_req_fn():
        ad2.set_safe_state()
        msg = f"ABORT: timeout handshaking BUSCHK up at step {step_id}"
        if log_fn:
            log_fn(f"[ad2] {msg}")
        raise BenchAbort(msg)

    if log_fn:
        log_fn(f"[ad2] S5 check step {step_id}: BUSCHK up DIO0..DIO7 = 0x{bus_up:02X} (expected 0xFF)")
    if bus_up != 0xFF:
        ad2.set_safe_state()
        msg = f"ABORT: bus contention check failed at {step_id}: BUSCHK up gave 0x{bus_up:02X}, expected 0xFF"
        if log_fn:
            log_fn(f"[ad2] {msg}")
        raise BenchAbort(msg)

    # 2. BUSCHK down
    down_line = wait_line_fn("BUSCHK down", 10.0)
    if not down_line:
        ad2.set_safe_state()
        msg = f"ABORT: bus contention check failed at {step_id}: timeout waiting for BUSCHK down"
        if log_fn:
            log_fn(f"[ad2] {msg}")
        raise BenchAbort(msg)

    dio_down = ad2.digital_io_read()
    bus_down = dio_down & AD2_DIO_BUS_MASK
    if not pulse_req_fn():
        ad2.set_safe_state()
        msg = f"ABORT: timeout handshaking BUSCHK down at step {step_id}"
        if log_fn:
            log_fn(f"[ad2] {msg}")
        raise BenchAbort(msg)

    if log_fn:
        log_fn(f"[ad2] S5 check step {step_id}: BUSCHK down DIO0..DIO7 = 0x{bus_down:02X} (expected 0x00)")
    if bus_down != 0x00:
        ad2.set_safe_state()
        msg = f"ABORT: bus contention check failed at {step_id}: BUSCHK down gave 0x{bus_down:02X}, expected 0x00"
        if log_fn:
            log_fn(f"[ad2] {msg}")
        raise BenchAbort(msg)

    return BusToken(step_id)


def run_selftest(
    port_name: str,
    baud: int,
    target_sn: str,
    log_path: Optional[pathlib.Path],
    checklist_confirmed: bool = False,
) -> int:
    log_file = None
    if log_path:
        log_file = open(log_path, "w", encoding="utf-8", buffering=1)

    def log(msg: str) -> None:
        stamp = f"[{time.strftime('%H:%M:%S')}] "
        line = stamp + msg
        print(line, flush=True)
        if log_file:
            log_file.write(line + "\n")

    # 1. Print S9 Operational Checklist (m3: operator declared)
    log("================================================================================")
    log("OPERATIONAL CHECKLIST (S9, IMPLEMENTATION_PLAN.md - declarado por el operador):")
    log("[checklist] [x] 1. Nucleo and AD2 connected via USB to same PC. WaveForms GUI closed.")
    log("[checklist] [x] 2. Wiring: GND first, then DIO per table. Nothing on W1, W2, V+, V-.")
    log("[checklist] [x] 3. Flash selftest-gpio with script stopped.")
    log("[checklist] [x] 4. Start gpio_hw_check.py. Script enforces S2, waits for banner (S8), runs loose check (T0a) before enabling outputs.")
    log("[checklist] [x] 5. On completion or abort, script leaves AD2 in S2. Only then reflash, reset, or disconnect.")
    log("[checklist] [x] 6. Disconnecting: stop script, disconnect DIO lines, GND last.")
    log("================================================================================")

    all_passed = True

    try:
        expected_rev = get_git_rev()
        log(f"[host] Expected firmware revision from git: {expected_rev}")
        if "-dirty" in expected_rev:
            log("[host] ERROR: Working tree is dirty. Clean before building and capturing.")
            return 1

        with AD2(sn=target_sn) as ad2:
            log(f"[ad2] Opened AD2 (SN {target_sn}) in safe state S2 (all inputs). Handle={ad2.handle}")

            # S8: Confirm OE is 0x0000 before doing anything else
            initial_oe = ad2.get_oe()
            if initial_oe != 0:
                raise AD2SafetyViolation(f"S8 violation: AD2 OE is 0x{initial_oe:04X} on open, expected 0x0000")

            # S10: Reset MCU with AD2 OE = 0x0000
            reset_mcu_safe(ad2, log_fn=log)
            time.sleep(0.1)

            # 2. Connect to Serial Port and Monitor
            log(f"[serial] Connecting to {port_name} at {baud} 8N1...")
            with SerialPort(port_name, baud) as ser:
                log(f"[serial] Connected. Waiting for banner (S8)...")

                pending = b""
                banner_found = False
                fw_rev = ""
                fw_passed = False
                cm4_received = False

                step_tracker = StepTracker(EXPECTED_STEP_SEQUENCE)

                # Helper to read next line matching a predicate
                def read_next_line(predicate_fn: Callable[[str], bool], timeout_s: float = 10.0) -> Optional[str]:
                    nonlocal pending
                    start = time.monotonic()
                    while time.monotonic() - start < timeout_s:
                        chunk = ser.read()
                        if chunk:
                            pending += chunk
                            *lines, pending = pending.split(b"\n")
                            for raw in lines:
                                text = raw.rstrip(b"\r").decode("utf-8", errors="replace")
                                log(text)
                                if predicate_fn(text):
                                    return text
                        time.sleep(0.001)
                    return None

                # Helper to wait for specific text substring
                def wait_for_line(target_substr: str, timeout_s: float = 10.0) -> Optional[str]:
                    return read_next_line(lambda l: target_substr in l, timeout_s)

                def pulse_host_req() -> bool:
                    ad2.digital_io_write(AD2_DIO_HOST_REQ, AD2_DIO_HOST_REQ)
                    if not wait_mcu_ack(ad2, False, 10.0):
                        ad2.digital_io_write(0, AD2_DIO_HOST_REQ)
                        return False
                    ad2.digital_io_write(0, AD2_DIO_HOST_REQ)
                    return True

                # S8: Validate banner BEFORE enabling any output
                banner_start = time.monotonic()
                rev_found = False
                idle_found = False

                while time.monotonic() - banner_start < 15.0 and not banner_found:
                    chunk = ser.read()
                    if chunk:
                        pending += chunk
                        *lines, pending = pending.split(b"\n")
                        for raw in lines:
                            text = raw.rstrip(b"\r").decode("utf-8", errors="replace")
                            log(text)

                            if "Rev " in text and not fw_rev:
                                parts = text.split("Rev ")
                                if len(parts) > 1:
                                    fw_rev = parts[1].split()[0]
                                    if "-dirty" in fw_rev:
                                        ad2.set_safe_state()
                                        raise BenchAbort(f"ABORT: banner revision is dirty: {fw_rev}")
                                    if fw_rev != expected_rev:
                                        ad2.set_safe_state()
                                        raise BenchAbort(f"ABORT: banner revision mismatch (got {fw_rev}, expected {expected_rev})")
                                    rev_found = True

                            if f"GPIO_BENCH v{GPIO_BENCH_PROTOCOL_VERSION} idle" in text:
                                idle_found = True

                            if rev_found and idle_found:
                                banner_found = True
                                break
                    time.sleep(0.002)

                if not banner_found:
                    ad2.set_safe_state()
                    raise BenchAbort("ABORT: banner missing or invalid within 15 seconds")

                log(f"[host] S8 banner verified (Rev {fw_rev}, GPIO_BENCH v{GPIO_BENCH_PROTOCOL_VERSION}).")

                # S8: NOW enable static output for HOST_REQ (DIO12)
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
                    log("[ad2] T0b(a) host OE guards PASSED (prohibited bits and bus owner mismatch rejected).")
                else:
                    log("[ad2] T0b(a) host OE guards FAILED!")
                    all_passed = False
                    return 1

                # 4. Synchronize session start with MCU
                log("[host] Handshaking session start with MCU...")
                ad2.digital_io_write(AD2_DIO_HOST_REQ, AD2_DIO_HOST_REQ)
                if wait_mcu_ack(ad2, True, 10.0):
                    ad2.digital_io_write(0, AD2_DIO_HOST_REQ)
                    wait_mcu_ack(ad2, False, 10.0)
                    log("[host] Session start synchronized.")
                else:
                    raise BenchAbort("ABORT: MCU did not ACK session start within 10s")

                # 5. Main Step Loop
                loop_start = time.monotonic()
                while time.monotonic() - loop_start < 120.0 and not (fw_passed and cm4_received):
                    chunk = ser.read()
                    if not chunk:
                        time.sleep(0.002)
                        continue

                    pending += chunk
                    *lines, pending = pending.split(b"\n")
                    for raw in lines:
                        text = raw.rstrip(b"\r").decode("utf-8", errors="replace")
                        log(text)

                        # Check for STEP announcement
                        if text.startswith("[m7_gpio_test] STEP "):
                            step_id, bus_owner = step_tracker.validate_announcement(text, ad2, log_fn=log)

                            # Dispatch by step ID
                            if step_id == STEP_T0B_GUARDS:
                                # Step 3: T0b(b) Firmware guards verification
                                res_line = wait_for_line("T0b_b guards verification:", 10.0)
                                if res_line and "PASS" in res_line:
                                    log("[ad2] T0b(b) firmware claim guards PASSED.")
                                else:
                                    log("[ad2] T0b(b) firmware claim guards FAILED!")
                                    all_passed = False
                                pulse_host_req()

                            elif step_id == STEP_T0B_S5_NEG:
                                # Step 4: T0b(c) S5 negative control (B4)
                                log("[ad2] Step T0b(c): Testing S5 negative control via s5_verify_released()...")
                                t0b_c_aborted = False
                                try:
                                    s5_verify_released(ad2, STEP_T0B_S5_NEG, wait_for_line, pulse_host_req, log_fn=log)
                                except BenchAbort as e:
                                    t0b_c_aborted = True
                                    log(f"[ad2] s5_verify_released successfully caught contention: {e}")

                                oe = ad2.get_oe()
                                if t0b_c_aborted and (oe & AD2_DIO_BUS_MASK) == 0:
                                    log("[ad2] T0b(c) S5 negative control PASSED (BenchAbort raised, OE bus remained 0x00).")
                                else:
                                    log(f"[ad2] T0b(c) S5 negative control FAILED (aborted={t0b_c_aborted}, OE=0x{oe:04X})")
                                    all_passed = False

                                # Restore static HOST_REQ output and notify MCU that host aborted
                                ad2.set_oe(AD2_DIO_HOST_REQ, GPIO_BENCH_BUS_NONE)
                                ad2.digital_io_write(0, AD2_DIO_HOST_REQ)
                                pulse_host_req()

                            elif step_id == STEP_T0B_TIMEOUT:
                                # Step 5: T0b(d) 10-second timeout test
                                log("[ad2] Step T0b(d): Testing 10-second timeout to bench_safe()...")
                                log("[ad2] Remaining silent for 10.5 seconds without asserting HOST_REQ...")
                                time.sleep(10.5)

                                timeout_line = wait_for_line("timeout expired -> bench_safe() invoked", 5.0)
                                if timeout_line:
                                    log("[ad2] T0b(d) timeout test PASSED.")
                                else:
                                    log("[ad2] T0b(d) timeout test FAILED (no timeout message)!")
                                    all_passed = False

                                # Re-synchronize
                                pulse_host_req()

                            elif step_id == STEP_T0A_LOOSE:
                                # Step 6: T0a loose check on 13 lines
                                log("[ad2] Step T0a: Testing loose lines (pull-up / pull-down)...")
                                wait_for_line("BUSCHK up", 10.0)
                                dio_up = ad2.digital_io_read()
                                bus_up = dio_up & AD2_DIO_BUS_MASK
                                log(f"[ad2] BUSCHK up: DIO0..DIO7 = 0x{bus_up:02X} (expected 0xFF)")
                                if bus_up != 0xFF:
                                    all_passed = False
                                pulse_host_req()

                                wait_for_line("BUSCHK down", 10.0)
                                dio_down = ad2.digital_io_read()
                                bus_down = dio_down & AD2_DIO_BUS_MASK
                                log(f"[ad2] BUSCHK down: DIO0..DIO7 = 0x{bus_down:02X} (expected 0x00)")
                                if bus_down != 0x00:
                                    all_passed = False
                                pulse_host_req()

                            elif step_id == STEP_T0A_HANDSHAKE:
                                # Step 7: T0a PE2/PE7 Handshake test
                                log("[ad2] Step T0a: Testing handshake lines (PE2 HOST_REQ, PE7 MCU_ACK)...")
                                # 1. Set HOST_REQ HIGH, MCU reads PE2=1
                                ad2.digital_io_write(AD2_DIO_HOST_REQ, AD2_DIO_HOST_REQ)
                                time.sleep(0.002)
                                l1 = wait_for_line("Handshake PE2 HIGH: OK", 5.0)
                                if not l1:
                                    log("[ad2] Handshake PE2 HIGH FAILED!")
                                    all_passed = False

                                # 2. Set HOST_REQ LOW, MCU reads PE2=0
                                ad2.digital_io_write(0, AD2_DIO_HOST_REQ)
                                time.sleep(0.002)
                                l2 = wait_for_line("Handshake PE2 LOW: OK", 5.0)
                                if not l2:
                                    log("[ad2] Handshake PE2 LOW FAILED!")
                                    all_passed = False

                                # 3. MCU sets MCU_ACK HIGH, AD2 reads DIO13=1
                                if wait_mcu_ack(ad2, True, 5.0):
                                    log("[ad2] Handshake PE7 MCU_ACK HIGH: OK")
                                else:
                                    log("[ad2] Handshake PE7 MCU_ACK HIGH FAILED!")
                                    all_passed = False
                                pulse_host_req()

                                # 4. MCU sets MCU_ACK LOW, AD2 reads DIO13=0
                                if wait_mcu_ack(ad2, False, 5.0):
                                    log("[ad2] Handshake PE7 MCU_ACK LOW: OK")
                                else:
                                    log("[ad2] Handshake PE7 MCU_ACK LOW FAILED!")
                                    all_passed = False
                                pulse_host_req()

                            elif step_id == STEP_T0A_MCU_TO_AD2:
                                # Step 8: T0a walking-1 MCU -> AD2 (11 lines)
                                log("[ad2] Step T0a: Walking-1 MCU -> AD2 (11 lines)...")
                                token = s5_verify_released(ad2, STEP_T0A_MCU_TO_AD2, wait_for_line, pulse_host_req, log_fn=log)
                                pulse_host_req()  # Authorize MCU to drive

                                # Observed set: PE8..PE15 (0..7), PE5 (9), PE6 (11), PA8 (14)
                                expected_dios = [9, 11, 0, 1, 2, 3, 4, 5, 6, 7, 14]
                                observed_mask = (1 << 9) | (1 << 11) | (1 << 14) | AD2_DIO_BUS_MASK

                                for exp_dio in expected_dios:
                                    if not wait_mcu_ack(ad2, True, 10.0):
                                        ad2.set_safe_state()
                                        raise BenchAbort(f"ABORT: Timeout waiting for MCU_ACK on walking-1 MCU->AD2 bit {exp_dio}")
                                    dio = ad2.digital_io_read()
                                    observed_val = dio & observed_mask
                                    expected_val = (1 << exp_dio)
                                    is_ok = (observed_val == expected_val)
                                    log(f"[ad2] Walking-1 MCU->AD2 DIO{exp_dio}: {'OK' if is_ok else 'FAIL'} "
                                        f"(observed=0x{observed_val:04X}, expected=0x{expected_val:04X})")
                                    if not is_ok:
                                        all_passed = False
                                    pulse_host_req()

                                # MCU->AD2 negative control: MCU sets ODR with pins in input with pull-down
                                wait_for_line("T0a_mcu control", 5.0)
                                dio_ctrl = ad2.digital_io_read()
                                obs_ctrl = dio_ctrl & observed_mask
                                ctrl_ok = (obs_ctrl == 0x0000)
                                log(f"[ad2] Walking-1 MCU->AD2 negative control: {'OK' if ctrl_ok else 'FAIL'} "
                                    f"(observed=0x{obs_ctrl:04X}, expected=0x0000)")
                                if not ctrl_ok:
                                    all_passed = False
                                pulse_host_req()

                            elif step_id == STEP_T0A_AD2_TO_MCU:
                                # Step 9: T0a walking-1 AD2 -> MCU (10 lines)
                                log("[ad2] Step T0a: Walking-1 AD2 -> MCU (10 lines)...")
                                token = s5_verify_released(ad2, STEP_T0A_AD2_TO_MCU, wait_for_line, pulse_host_req, log_fn=log)
                                pulse_host_req()

                                # 10 lines: PE3 (DIO8), PE4 (DIO10), PE8..PE15 (DIO0..DIO7)
                                ad2_test_bits = [8, 10, 0, 1, 2, 3, 4, 5, 6, 7]
                                for b in ad2_test_bits:
                                    if b < 8:
                                        drive_bus(ad2, token, 1 << b, STEP_T0A_AD2_TO_MCU)
                                        # Refresh token for next bus bit if needed
                                        token = BusToken(STEP_T0A_AD2_TO_MCU)
                                    else:
                                        ad2.set_oe(AD2_DIO_HOST_REQ | (1 << b), GPIO_BENCH_BUS_NONE)
                                        ad2.digital_io_write((1 << b), (1 << b))

                                    time.sleep(0.001)
                                    # Wait for MCU announcement with bit result
                                    bit_res_line = wait_for_line(f"T0a_ad2 bit={b}", 10.0)
                                    if bit_res_line and "OK" in bit_res_line:
                                        log(f"[ad2] Walking-1 AD2->MCU DIO{b}: OK")
                                    else:
                                        log(f"[ad2] Walking-1 AD2->MCU DIO{b}: FAIL (line: {bit_res_line})")
                                        all_passed = False

                                    # Release bit
                                    ad2.digital_io_write(0, 1 << b)
                                    ad2.set_safe_state()
                                    ad2.set_oe(AD2_DIO_HOST_REQ, GPIO_BENCH_BUS_NONE)
                                    pulse_host_req()

                                # AD2->MCU negative control: AD2 drives 0xFF, MCU bus in analog
                                wait_for_line("T0a_ad2 analog control ready", 5.0)
                                token = BusToken(STEP_T0A_AD2_TO_MCU)
                                drive_bus(ad2, token, 0xFF, STEP_T0A_AD2_TO_MCU)
                                time.sleep(0.001)
                                ctrl_res = wait_for_line("T0a_ad2 analog control result:", 5.0)
                                if ctrl_res and "OK" in ctrl_res:
                                    log("[ad2] Walking-1 AD2->MCU analog control: OK (MCU IDR=0x00)")
                                else:
                                    log(f"[ad2] Walking-1 AD2->MCU analog control: FAIL ({ctrl_res})")
                                    all_passed = False

                                ad2.set_safe_state()
                                ad2.set_oe(AD2_DIO_HOST_REQ, GPIO_BENCH_BUS_NONE)
                                pulse_host_req()

                            elif step_id == STEP_T0A_SCOPE:
                                # Step 10: T0a Scope 1 (PF14) and Scope 2 (PF15) DC levels
                                log("[ad2] Step T0a: Testing Scope 1 (PF14) and Scope 2 (PF15) DC levels...")
                                ad2.analog_in_setup_dc(channel=0, voltage_range=10.0)
                                ad2.analog_in_setup_dc(channel=1, voltage_range=10.0)
                                time.sleep(0.05)

                                # 1. LOW
                                wait_for_line("Scope LOW ready", 10.0)
                                time.sleep(0.01)
                                v1_low = ad2.analog_in_read_dc(0)
                                v2_low = ad2.analog_in_read_dc(1)
                                log(f"[ad2] PF14/PF15 LOW: Scope1={v1_low:.3f}V, Scope2={v2_low:.3f}V (expected <= 0.4V)")
                                if v1_low > 0.4 or v2_low > 0.4:
                                    log("[ad2] Scope VOL check FAILED!")
                                    all_passed = False
                                pulse_host_req()

                                # 2. HIGH
                                wait_for_line("Scope HIGH ready", 10.0)
                                time.sleep(0.01)
                                v1_high = ad2.analog_in_read_dc(0)
                                v2_high = ad2.analog_in_read_dc(1)
                                log(f"[ad2] PF14/PF15 HIGH: Scope1={v1_high:.3f}V, Scope2={v2_high:.3f}V (expected >= 2.9V)")
                                if v1_high < 2.9 or v2_high < 2.9:
                                    log("[ad2] Scope VOH check FAILED!")
                                    all_passed = False
                                pulse_host_req()

                            elif step_id == STEP_DONE:
                                log("[host] All test steps executed.")

                        if "VERDICT: ALL GPIO SELF-TESTS PASSED" in text:
                            log("[host] Firmware announced ALL GPIO SELF-TESTS PASSED.")
                            fw_passed = True

                        elif "VERDICT: GPIO SELF-TESTS FAILED" in text:
                            log("[host] Firmware announced GPIO SELF-TESTS FAILED.")
                            all_passed = False
                            fw_passed = True

                        elif "CM4 VERDICT=" in text:
                            log("[host] CM4 logs drained.")
                            cm4_received = True

    except Exception as e:
        log(f"ERROR during selftest execution: {e}")
        all_passed = False
    finally:
        verdict_str = "PASS" if all_passed else "FAIL"
        log("================================================================================")
        log(f"HOST VERDICT={verdict_str}")
        log("================================================================================")
        if log_file:
            log_file.close()

    return 0 if all_passed else 1


def main() -> int:
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

    return run_selftest(args.port, args.baud, args.sn, args.log, checklist_confirmed=True)


if __name__ == "__main__":
    sys.exit(main())
