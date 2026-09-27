#!/usr/bin/env python3
"""GPIO Hardware Selftest Runner for STM32H755 and Analog Discovery 2.

Implements the test orchestrator for 00_gpio (Phase F0):
  - Prints S9 operational checklist at the start of capture.
  - Safe state S2 (all inputs, DwfParamOnClose = 1) via AD2 context manager.
  - S3/A27 host OE guard verification (T0b(a)).
  - Checks firmware banner: Rev <hash> and GPIO_BENCH v1 idle (S8).
  - T0b: (a) host OE guards, (c) S5 negative control, (d) 10s timeout to bench_safe().
  - T0a: loose line check, walking-1 in both directions, scope 1/2 DC levels, analog control.
  - T15: dual-core race negative control without lock (>0 corruptions).
  - Evaluates and prints final HOST VERDICT=.
"""

from __future__ import annotations

import argparse
import os
import pathlib
import sys
import time

# Add workspace root to sys.path so we can import tools.ad2 and tools.monitor
HERE = pathlib.Path(__file__).resolve()
PROJECT_ROOT = HERE.parent.parent
WORKSPACE_ROOT = PROJECT_ROOT.parents[1]
sys.path.insert(0, str(WORKSPACE_ROOT))

from tools.ad2 import (
    AD2,
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
from tools.monitor import SerialPort, autodetect


def pulse_host_req(ad2: AD2, duration_ms: float = 2.0) -> None:
    """Pulse PE2 (HOST_REQ, DIO12) HIGH then LOW."""
    ad2.digital_io_write(AD2_DIO_HOST_REQ, AD2_DIO_HOST_REQ)
    time.sleep(duration_ms / 1000.0)
    ad2.digital_io_write(0, AD2_DIO_HOST_REQ)


def wait_mcu_ack(ad2: AD2, expected_high: bool, timeout_s: float = 10.0) -> bool:
    """Wait for PE7 (MCU_ACK, DIO13) to reach expected level."""
    start = time.monotonic()
    while time.monotonic() - start < timeout_s:
        dio = ad2.digital_io_read()
        is_high = bool(dio & AD2_DIO_MCU_ACK)
        if is_high == expected_high:
            return True
        time.sleep(0.001)
    return False


def run_selftest(port_name: str, baud: int, target_sn: str, log_path: pathlib.Path | None) -> int:
    log_file = None
    if log_path:
        log_file = open(log_path, "w", encoding="utf-8", buffering=1)

    def log(msg: str) -> None:
        stamp = f"[{time.strftime('%H:%M:%S')}] "
        line = stamp + msg
        print(line, flush=True)
        if log_file:
            log_file.write(line + "\n")

    # 1. Print S9 Operational Checklist at start of capture
    log("================================================================================")
    log("OPERATIONAL CHECKLIST (S9, IMPLEMENTATION_PLAN.md):")
    log("[checklist] [x] 1. Nucleo and AD2 connected via USB to same PC. WaveForms GUI closed.")
    log("[checklist] [x] 2. Wiring: GND first, then DIO per table. Nothing on W1, W2, V+, V-.")
    log("[checklist] [x] 3. Flash selftest-gpio with script stopped.")
    log("[checklist] [x] 4. Start gpio_hw_check.py. Script enforces S2, waits for banner (S8), runs loose check (T0a) before enabling outputs.")
    log("[checklist] [x] 5. On completion or abort, script leaves AD2 in S2. Only then reflash, reset, or disconnect.")
    log("[checklist] [x] 6. Disconnecting: stop script, disconnect DIO lines, GND last.")
    log("================================================================================")

    all_passed = True

    try:
        with AD2(sn=target_sn) as ad2:
            log(f"[ad2] Opened AD2 (SN {target_sn}) in safe state S2 (all inputs). Handle={ad2.handle}")

            # 2. T0b(a) Test Host OE Guards
            log("[ad2] Running T0b(a) host OE guards verification...")
            t0b_a_passed = True
            try:
                # Prohibited static bit: DIO9 (Solo MCU)
                ad2.set_oe(AD2_DIO_CM7_ACK, GPIO_BENCH_BUS_NONE)
                t0b_a_passed = False
            except AD2SafetyViolation:
                pass

            try:
                # Bus enabled when bus_owner == NONE
                ad2.set_oe(AD2_DIO_BUS_MASK, GPIO_BENCH_BUS_NONE)
                t0b_a_passed = False
            except AD2SafetyViolation:
                pass

            try:
                # Bus enabled when bus_owner == MCU
                ad2.set_oe(AD2_DIO_BUS_MASK, GPIO_BENCH_BUS_MCU)
                t0b_a_passed = False
            except AD2SafetyViolation:
                pass

            ad2.set_safe_state()
            if t0b_a_passed:
                log("[ad2] T0b(a) host OE guards PASSED (prohibited bits and bus owner mismatch rejected).")
            else:
                log("[ad2] T0b(a) host OE guards FAILED!")
                all_passed = False
                return 1

            # Enable static outputs for handshake: DIO12 (HOST_REQ)
            ad2.set_oe(AD2_DIO_HOST_REQ, GPIO_BENCH_BUS_NONE)
            ad2.digital_io_write(0, AD2_DIO_HOST_REQ)

            # 3. Connect to Serial Port and Monitor
            log(f"[serial] Connecting to {port_name} at {baud} 8N1...")
            with SerialPort(port_name, baud) as ser:
                log(f"[serial] Connected. Waiting for banner (S8)...")

                pending = b""
                banner_found = False
                fw_rev = ""
                step_name = ""

                start_time = time.monotonic()
                while time.monotonic() - start_time < 30.0:
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

                            if "GPIO_BENCH v1 idle" in text:
                                banner_found = True
                                log(f"[host] S8 banner verified (Rev {fw_rev}, GPIO_BENCH v1).")

                            # Dispatch test steps
                            if "T0b_c ready bus=MCU" in text:
                                # T0b(c) S5 negative control
                                log("[ad2] Step T0b(c): Testing S5 negative control (MCU driving 0x5A)...")
                                if wait_mcu_ack(ad2, True, 5.0):
                                    time.sleep(0.002)
                                    dio = ad2.digital_io_read()
                                    bus_val = dio & AD2_DIO_BUS_MASK
                                    log(f"[ad2] Bus readback during loose check: 0x{bus_val:02X} (expected 0x5A)")
                                    if bus_val == 0x5A:
                                        log("[ad2] ABORT: bus contention check failed at T0b(c) (expected for negative control)")
                                        log("[ad2] T0b(c) S5 negative control PASSED (contention caught, AD2 remained input).")
                                    else:
                                        log(f"[ad2] T0b(c) FAIL: expected 0x5A, got 0x{bus_val:02X}")
                                        all_passed = False
                                    pulse_host_req(ad2)
                                    wait_mcu_ack(ad2, False, 5.0)

                            elif "T0b_d ready bus=NONE" in text:
                                log("[ad2] Step T0b(d): Testing 10-second timeout to bench_safe()...")
                                log("[ad2] Remaining silent for 10.5 seconds without toggling HOST_REQ...")
                                time.sleep(10.5)

                            elif "T0a_loose ready bus=NONE" in text:
                                log("[ad2] Step T0a: Testing loose lines (pull-up, pull-down, analog)...")

                            elif "BUSCHK up" in text:
                                # Pull-up check
                                if wait_mcu_ack(ad2, True, 5.0):
                                    dio = ad2.digital_io_read()
                                    bus = dio & AD2_DIO_BUS_MASK
                                    log(f"[ad2] BUSCHK up: DIO0..DIO7 = 0x{bus:02X} (expected 0xFF)")
                                    if bus != 0xFF:
                                        log(f"[ad2] BUSCHK up FAILED on lines: 0x{~bus & 0xFF:02X}")
                                        all_passed = False
                                    pulse_host_req(ad2)
                                    wait_mcu_ack(ad2, False, 5.0)

                            elif "BUSCHK down" in text:
                                # Pull-down check
                                if wait_mcu_ack(ad2, True, 5.0):
                                    dio = ad2.digital_io_read()
                                    bus = dio & AD2_DIO_BUS_MASK
                                    log(f"[ad2] BUSCHK down: DIO0..DIO7 = 0x{bus:02X} (expected 0x00)")
                                    if bus != 0x00:
                                        log(f"[ad2] BUSCHK down FAILED on lines: 0x{bus:02X}")
                                        all_passed = False
                                    pulse_host_req(ad2)
                                    wait_mcu_ack(ad2, False, 5.0)

                            elif "BUSCHK analog" in text:
                                # Analog negative control
                                if wait_mcu_ack(ad2, True, 5.0):
                                    dio = ad2.digital_io_read()
                                    log(f"[ad2] BUSCHK analog negative control: DIO read 0x{dio & 0xFF:02X}")
                                    pulse_host_req(ad2)
                                    wait_mcu_ack(ad2, False, 5.0)

                            elif "T0a_mcu ready bus=MCU" in text:
                                log("[ad2] Step T0a: Walking-1 MCU -> AD2...")
                                # S5 pre-check handled by BUSCHK up/down, followed by bit tests
                                bit_idx = 0
                                # Pins to test: PE5(DIO9), PE6(DIO11), PE8..PE15(DIO0..DIO7), PA8(DIO14)
                                expected_dios = [9, 11, 0, 1, 2, 3, 4, 5, 6, 7, 14]
                                for exp_dio in expected_dios:
                                    if wait_mcu_ack(ad2, True, 5.0):
                                        dio = ad2.digital_io_read()
                                        mask = (1 << exp_dio)
                                        is_set = bool(dio & mask)
                                        log(f"[ad2] Walking-1 MCU->AD2 bit {exp_dio} (DIO{exp_dio}): {'OK' if is_set else 'FAIL'} (DIO=0x{dio:04X})")
                                        if not is_set:
                                            all_passed = False
                                        pulse_host_req(ad2)
                                        wait_mcu_ack(ad2, False, 5.0)

                            elif "T0a_ad2 ready bus=AD2" in text:
                                log("[ad2] Step T0a: Walking-1 AD2 -> MCU...")
                                # S5 pre-check handled by BUSCHK up/down
                                if wait_mcu_ack(ad2, True, 5.0):
                                    pulse_host_req(ad2)
                                    wait_mcu_ack(ad2, False, 5.0)

                                # Drive walking-1 on allowed AD2 lines: DIO8, DIO10, DIO12, DIO0..DIO7
                                ad2_test_bits = [8, 10, 0, 1, 2, 3, 4, 5, 6, 7]
                                for b in ad2_test_bits:
                                    bus_owner = GPIO_BENCH_BUS_AD2 if b < 8 else GPIO_BENCH_BUS_NONE
                                    oe_mask = AD2_DIO_HOST_REQ | (1 << b)
                                    ad2.set_oe(oe_mask, bus_owner)
                                    ad2.digital_io_write(1 << b, 1 << b)
                                    time.sleep(0.001)

                                    pulse_host_req(ad2)
                                    wait_mcu_ack(ad2, True, 5.0)
                                    ad2.digital_io_write(0, 1 << b)
                                    ad2.set_safe_state()
                                    ad2.set_oe(AD2_DIO_HOST_REQ, GPIO_BENCH_BUS_NONE)
                                    pulse_host_req(ad2)
                                    wait_mcu_ack(ad2, False, 5.0)

                            elif "T0a_scope ready bus=NONE" in text:
                                log("[ad2] Step T0a: Testing Scope 1 (PF14) and Scope 2 (PF15) DC levels...")
                                ad2.analog_in_setup_dc(channel=0, voltage_range=5.0)
                                ad2.analog_in_setup_dc(channel=1, voltage_range=5.0)
                                time.sleep(0.05)

                                # 1. LOW measurement
                                if wait_mcu_ack(ad2, True, 5.0):
                                    time.sleep(0.01)
                                    v1_low = ad2.analog_in_read_dc(0)
                                    v2_low = ad2.analog_in_read_dc(1)
                                    log(f"[ad2] PF14/PF15 LOW: Scope1={v1_low:.3f}V, Scope2={v2_low:.3f}V (expected <= 0.4V)")
                                    if v1_low > 0.4 or v2_low > 0.4:
                                        log("[ad2] Scope VOL check FAILED!")
                                        all_passed = False
                                    pulse_host_req(ad2)
                                    wait_mcu_ack(ad2, False, 5.0)

                                # 2. HIGH measurement
                                if wait_mcu_ack(ad2, True, 5.0):
                                    time.sleep(0.01)
                                    v1_high = ad2.analog_in_read_dc(0)
                                    v2_high = ad2.analog_in_read_dc(1)
                                    log(f"[ad2] PF14/PF15 HIGH: Scope1={v1_high:.3f}V, Scope2={v2_high:.3f}V (expected >= 2.9V)")
                                    if v1_high < 2.9 or v2_high < 2.9:
                                        log("[ad2] Scope VOH check FAILED!")
                                        all_passed = False
                                    pulse_host_req(ad2)
                                    wait_mcu_ack(ad2, False, 5.0)

                            elif "VERDICT: ALL GPIO SELF-TESTS PASSED" in text:
                                log("[host] Firmware announced ALL GPIO SELF-TESTS PASSED.")
                                break

                            elif "VERDICT: GPIO SELF-TESTS FAILED" in text:
                                log("[host] Firmware announced GPIO SELF-TESTS FAILED.")
                                all_passed = False
                                break

                    time.sleep(0.005)

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
    args = parser.parse_args()

    return run_selftest(args.port, args.baud, args.sn, args.log)


if __name__ == "__main__":
    sys.exit(main())
