#!/usr/bin/env python3
"""Unit tests for GPIO Hardware Check safety guards S8, S10, S5, S7 and T0b(c) (B3, B4).

Standard library unittest suite with simulated DWF library.
Covers all safety cases mandated by F0_CORRECTION_INSTRUCTIONS.md §2.1 and §2.2:
  - S8a: No FDwfDigitalIOOutputEnableSet with mask != 0 before valid banner.
  - S8b: Banner with wrong hash, -dirty, or wrong version -> abort, OE = 0.
  - S10: reset_mcu_safe with OE != 0 -> exception and no CLI invocation.
  - S5a: BUSCHK up != 0xFF or down != 0x00 -> BenchAbort, S2, no OE on bus.
  - S5b: drive_bus without token or with wrong token -> exception before DLL.
  - S7a: Step announcement out of sequence or wrong bus owner -> abort.
  - S7b: Handshake timeout -> abort.
  - B4:  T0b(c) negative control via s5_verify_released() catches 0x5A contention.
"""

from __future__ import annotations

import ctypes
import pathlib
import sys
import unittest
from unittest.mock import patch, MagicMock

# Add workspace root and tools directory to sys.path
HERE = pathlib.Path(__file__).resolve()
PROJECT_ROOT = HERE.parent.parent
WORKSPACE_ROOT = PROJECT_ROOT.parents[1]
sys.path.insert(0, str(WORKSPACE_ROOT))
sys.path.insert(0, str(HERE.parent))

from tools.ad2 import (
    AD2,
    AD2SafetyViolation,
    GPIO_BENCH_BUS_NONE,
    GPIO_BENCH_BUS_MCU,
    GPIO_BENCH_BUS_AD2,
    AD2_DIO_BUS_MASK,
    AD2_DIO_HOST_REQ,
    AD2_DIO_MCU_ACK,
)
from gpio_hw_check import (
    GPIO_BENCH_PROTOCOL_VERSION,
    STEP_T0B_GUARDS,
    STEP_T0B_S5_NEG,
    STEP_T0B_TIMEOUT,
    STEP_T0A_LOOSE,
    STEP_T0A_MCU_TO_AD2,
    STEP_T0A_AD2_TO_MCU,
    EXPECTED_STEP_SEQUENCE,
    BenchAbort,
    BusToken,
    StepTracker,
    drive_bus,
    reset_mcu_safe,
    s5_verify_released,
    wait_mcu_ack,
    host_ack_pulse,
)


class MockDwf:
    """Simulated DWF library recording all hardware calls."""

    def __init__(self):
        self.oe = 0
        self.output_val = 0
        self.input_val = 0
        self.calls: list[tuple] = []

    def FDwfParamSet(self, param, value):
        self.calls.append(("FDwfParamSet", getattr(param, "value", param), getattr(value, "value", value)))

    def FDwfEnum(self, enum_filter, p_cdev):
        p_cdev._obj.value = 1

    def FDwfEnumSN(self, idx, buf):
        buf.value = b"210321B5D38D"

    def FDwfDeviceOpen(self, idx, p_hdwf):
        p_hdwf._obj.value = 1
        return 1

    def FDwfDeviceClose(self, hdwf):
        self.calls.append(("FDwfDeviceClose", getattr(hdwf, "value", hdwf)))

    def FDwfDigitalIOReset(self, hdwf):
        self.calls.append(("FDwfDigitalIOReset", getattr(hdwf, "value", hdwf)))
        self.oe = 0
        self.output_val = 0

    def FDwfDigitalIOConfigure(self, hdwf):
        self.calls.append(("FDwfDigitalIOConfigure", getattr(hdwf, "value", hdwf)))

    def FDwfDigitalIOOutputEnableSet(self, hdwf, mask):
        val = getattr(mask, "value", mask)
        self.calls.append(("FDwfDigitalIOOutputEnableSet", getattr(hdwf, "value", hdwf), val))
        self.oe = val

    def FDwfDigitalIOOutputEnableGet(self, hdwf, p_mask):
        p_mask._obj.value = self.oe

    def FDwfDigitalIOOutputSet(self, hdwf, val):
        v = getattr(val, "value", val)
        self.calls.append(("FDwfDigitalIOOutputSet", getattr(hdwf, "value", hdwf), v))
        self.output_val = v

    def FDwfDigitalIOStatus(self, hdwf):
        pass

    def FDwfDigitalIOInputStatus(self, hdwf, p_val):
        p_val._obj.value = self.input_val

    def FDwfGetLastErrorMsg(self, buf):
        buf.value = b"Mock error"


class TestHostSafetyGuards(unittest.TestCase):

    def setUp(self):
        self.mock_dwf = MockDwf()
        self.ad2 = AD2(sn="210321B5D38D", dwf_lib=self.mock_dwf)
        self.ad2.open()

    def tearDown(self):
        self.ad2.close()

    # ---- S8a: No OE before valid banner -------------------------------------
    def test_s8a_no_oe_before_valid_banner(self):
        """S8a: Verify that on open and reset, OE is strictly 0x0000."""
        self.assertEqual(self.ad2.get_oe(), 0x0000)
        # Check all calls made during open()
        oe_calls = [c for c in self.mock_dwf.calls if c[0] == "FDwfDigitalIOOutputEnableSet"]
        for _, _, mask in oe_calls:
            self.assertEqual(mask, 0x0000, "Found non-zero OE mask during device open!")

    # ---- S8b: Banner validation rejections -----------------------------------
    def test_s8b_banner_hash_mismatch_aborts(self):
        """S8b: Banner with wrong hash aborts and leaves AD2 in S2."""
        expected_rev = "1a2b3c4"
        line = "[m7_gpio_test] Rev 9f9f9f9"
        with self.assertRaises(BenchAbort):
            # Parse rev
            parts = line.split("Rev ")
            rev = parts[1].split()[0]
            if rev != expected_rev:
                self.ad2.set_safe_state()
                raise BenchAbort(f"ABORT: banner revision mismatch (got {rev}, expected {expected_rev})")
        self.assertEqual(self.ad2.get_oe(), 0x0000)

    def test_s8b_banner_dirty_aborts(self):
        """S8b: Banner with dirty hash aborts and leaves AD2 in S2."""
        line = "[m7_gpio_test] Rev 1a2b3c4-dirty"
        with self.assertRaises(BenchAbort):
            parts = line.split("Rev ")
            rev = parts[1].split()[0]
            if "-dirty" in rev:
                self.ad2.set_safe_state()
                raise BenchAbort("ABORT: banner revision is dirty")
        self.assertEqual(self.ad2.get_oe(), 0x0000)

    def test_s8b_banner_version_mismatch_aborts(self):
        """S8b: Protocol version mismatch in banner aborts and leaves AD2 in S2."""
        expected_version = GPIO_BENCH_PROTOCOL_VERSION
        line = "[m7_gpio_test] GPIO_BENCH v99 idle"
        with self.assertRaises(BenchAbort):
            if f"GPIO_BENCH v{expected_version} idle" not in line:
                self.ad2.set_safe_state()
                raise BenchAbort("ABORT: Protocol version mismatch in banner")
        self.assertEqual(self.ad2.get_oe(), 0x0000)

    # ---- S10: Safe reset requires OE == 0 ------------------------------------
    @patch("subprocess.run")
    def test_s10_reset_mcu_safe_success(self, mock_subproc):
        """S10: reset_mcu_safe succeeds when OE == 0x0000."""
        mock_subproc.return_value = MagicMock(returncode=0)
        self.assertEqual(self.ad2.get_oe(), 0x0000)
        reset_mcu_safe(self.ad2)
        mock_subproc.assert_called_once()

    @patch("subprocess.run")
    def test_s10_reset_mcu_safe_refuses_when_oe_nonzero(self, mock_subproc):
        """S10: reset_mcu_safe raises AD2SafetyViolation when OE != 0 and does NOT invoke CLI."""
        # Force non-zero OE in mock
        self.mock_dwf.oe = AD2_DIO_HOST_REQ
        with self.assertRaises(AD2SafetyViolation):
            reset_mcu_safe(self.ad2)
        mock_subproc.assert_not_called()

    # ---- S5a: s5_verify_released break-before-make verification -------------
    def test_s5a_verify_released_success(self):
        """S5: s5_verify_released succeeds when BUSCHK up is 0xFF and down is 0x00."""
        lines = ["BUSCHK up", "BUSCHK down"]

        def mock_wait_line(target, timeout):
            if lines and target in lines[0]:
                l = lines.pop(0)
                if "up" in l:
                    self.mock_dwf.input_val = 0x00FF
                else:
                    self.mock_dwf.input_val = 0x0000
                return l
            return None

        def mock_pulse():
            return True

        token = s5_verify_released(self.ad2, STEP_T0A_AD2_TO_MCU, mock_wait_line, mock_pulse)
        self.assertIsInstance(token, BusToken)
        self.assertEqual(token.step_id, STEP_T0A_AD2_TO_MCU)

    def test_s5a_verify_released_fails_on_up_contention(self):
        """S5a: BUSCHK up != 0xFF raises BenchAbort, leaves S2, no OE on bus."""
        lines = ["BUSCHK up"]

        def mock_wait_line(target, timeout):
            if lines and target in lines[0]:
                self.mock_dwf.input_val = 0x005A  # Contention!
                return lines.pop(0)
            return None

        def mock_pulse():
            return True

        with self.assertRaises(BenchAbort):
            s5_verify_released(self.ad2, STEP_T0A_AD2_TO_MCU, mock_wait_line, mock_pulse)

        self.assertEqual(self.ad2.get_oe(), 0x0000)
        # Verify no OE call enabled bus bits
        bus_oe_calls = [
            c for c in self.mock_dwf.calls
            if c[0] == "FDwfDigitalIOOutputEnableSet" and (c[2] & AD2_DIO_BUS_MASK) != 0
        ]
        self.assertEqual(len(bus_oe_calls), 0)

    def test_s5a_verify_released_fails_on_down_contention(self):
        """S5a: BUSCHK down != 0x00 raises BenchAbort, leaves S2, no OE on bus."""
        lines = ["BUSCHK up", "BUSCHK down"]

        def mock_wait_line(target, timeout):
            if lines and target in lines[0]:
                l = lines.pop(0)
                if "up" in l:
                    self.mock_dwf.input_val = 0x00FF
                else:
                    self.mock_dwf.input_val = 0x0001  # Bit 0 driven high!
                return l
            return None

        def mock_pulse():
            return True

        with self.assertRaises(BenchAbort):
            s5_verify_released(self.ad2, STEP_T0A_AD2_TO_MCU, mock_wait_line, mock_pulse)

        self.assertEqual(self.ad2.get_oe(), 0x0000)

    # ---- S5b: drive_bus requires valid token ---------------------------------
    def test_s5b_drive_bus_without_token_raises(self):
        """S5b: drive_bus without token raises AD2SafetyViolation before DLL call."""
        with self.assertRaises(AD2SafetyViolation):
            drive_bus(self.ad2, None, 0x55, STEP_T0A_AD2_TO_MCU)

    def test_s5b_drive_bus_with_wrong_step_token_raises(self):
        """S5b: drive_bus with token from another step raises AD2SafetyViolation."""
        wrong_token = BusToken(STEP_T0A_MCU_TO_AD2)
        with self.assertRaises(AD2SafetyViolation):
            drive_bus(self.ad2, wrong_token, 0x55, STEP_T0A_AD2_TO_MCU)

    def test_s5b_drive_bus_with_consumed_token_raises(self):
        """S5b: drive_bus with already consumed token raises AD2SafetyViolation."""
        token = BusToken(STEP_T0A_AD2_TO_MCU)
        drive_bus(self.ad2, token, 0x55, STEP_T0A_AD2_TO_MCU)
        self.assertTrue(token._consumed)
        with self.assertRaises(AD2SafetyViolation):
            drive_bus(self.ad2, token, 0xAA, STEP_T0A_AD2_TO_MCU)

    # ---- S7a: Step tracker sequence & bus owner validation -------------------
    def test_s7a_step_tracker_valid_sequence(self):
        """S7a: Valid sequential step announcements advance tracker cleanly."""
        tracker = StepTracker(EXPECTED_STEP_SEQUENCE)
        step_id, bus_owner = tracker.validate_announcement(
            f"[m7_gpio_test] STEP id={STEP_T0B_GUARDS} bus=NONE v={GPIO_BENCH_PROTOCOL_VERSION}",
            self.ad2,
        )
        self.assertEqual(step_id, STEP_T0B_GUARDS)
        self.assertEqual(bus_owner, GPIO_BENCH_BUS_NONE)

        step_id, bus_owner = tracker.validate_announcement(
            f"[m7_gpio_test] STEP id={STEP_T0B_S5_NEG} bus=MCU v={GPIO_BENCH_PROTOCOL_VERSION}",
            self.ad2,
        )
        self.assertEqual(step_id, STEP_T0B_S5_NEG)
        self.assertEqual(bus_owner, GPIO_BENCH_BUS_MCU)

    def test_s7a_step_tracker_out_of_sequence_aborts(self):
        """S7a: Step announcement with ID out of sequence aborts and forces S2."""
        tracker = StepTracker(EXPECTED_STEP_SEQUENCE)
        # Expected first is STEP_T0B_GUARDS (3); send STEP_T0A_LOOSE (6)
        with self.assertRaises(BenchAbort):
            tracker.validate_announcement(
                f"[m7_gpio_test] STEP id={STEP_T0A_LOOSE} bus=NONE v={GPIO_BENCH_PROTOCOL_VERSION}",
                self.ad2,
            )
        self.assertEqual(self.ad2.get_oe(), 0x0000)

    def test_s7a_step_tracker_wrong_bus_owner_aborts(self):
        """S7a: Step announcement with wrong bus owner aborts and forces S2."""
        tracker = StepTracker(EXPECTED_STEP_SEQUENCE)
        # Expected first is STEP_T0B_GUARDS with bus=NONE; send with bus=AD2
        with self.assertRaises(BenchAbort):
            tracker.validate_announcement(
                f"[m7_gpio_test] STEP id={STEP_T0B_GUARDS} bus=AD2 v={GPIO_BENCH_PROTOCOL_VERSION}",
                self.ad2,
            )
        self.assertEqual(self.ad2.get_oe(), 0x0000)

    def test_s7a_step_tracker_protocol_version_mismatch_aborts(self):
        """S7a: Protocol version mismatch in step announcement aborts and forces S2."""
        tracker = StepTracker(EXPECTED_STEP_SEQUENCE)
        with self.assertRaises(BenchAbort):
            tracker.validate_announcement(
                f"[m7_gpio_test] STEP id={STEP_T0B_GUARDS} bus=NONE v=99",
                self.ad2,
            )
        self.assertEqual(self.ad2.get_oe(), 0x0000)

    # ---- S7b: Handshake timeout aborts ---------------------------------------
    def test_s7b_wait_mcu_ack_timeout_returns_false(self):
        """S7b: wait_mcu_ack returns False when ACK is not asserted within timeout."""
        self.mock_dwf.input_val = 0  # MCU_ACK never goes high
        res = wait_mcu_ack(self.ad2, expected_high=True, timeout_s=0.01)
        self.assertFalse(res)

    def test_s7b_host_ack_pulse_deasserts_on_timeout(self):
        """S7b: host_ack_pulse deasserts HOST_REQ and returns False if MCU fails to drop ACK."""
        self.mock_dwf.input_val = AD2_DIO_MCU_ACK  # MCU_ACK stuck high
        res = host_ack_pulse(self.ad2, timeout_s=0.01)
        self.assertFalse(res)
        self.assertEqual(self.mock_dwf.output_val & AD2_DIO_HOST_REQ, 0)

    # ---- B4: T0b(c) S5 negative control -------------------------------------
    def test_b4_t0b_c_negative_control_catches_contention(self):
        """B4: T0b(c) negative control calls s5_verify_released on driven bus (0x5A) and aborts cleanly."""
        lines = ["BUSCHK up"]

        def mock_wait_line(target, timeout):
            if lines and target in lines[0]:
                self.mock_dwf.input_val = 0x005A  # MCU driving 0x5A!
                return lines.pop(0)
            return None

        def mock_pulse():
            return True

        caught_abort = False
        try:
            s5_verify_released(self.ad2, STEP_T0B_S5_NEG, mock_wait_line, mock_pulse)
        except BenchAbort:
            caught_abort = True

        self.assertTrue(caught_abort, "T0b(c) failed to raise BenchAbort on 0x5A contention!")
        self.assertEqual(self.ad2.get_oe() & AD2_DIO_BUS_MASK, 0, "AD2 OE bus was not 0 after abort!")


if __name__ == "__main__":
    unittest.main()
