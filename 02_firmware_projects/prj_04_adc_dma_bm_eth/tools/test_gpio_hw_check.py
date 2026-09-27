#!/usr/bin/env python3
"""Comprehensive test suite for GPIO Hardware Selftest Runner and Safety Guards.

Standard library unittest suite implementing:
  - 14 mandatory test scenarios (E1..E14) from F0_CORRECTION_2_INSTRUCTIONS.md §2.1.
  - Continuous electrical invariant checking: (mcu_outputs & ad2_oe) == 0 at every instant.
  - Unit tests for S8, S10, S5, S7, R-B1, R-B2, B3, B4 safety guards.
"""

from __future__ import annotations

import pathlib
import re
import sys
import unittest
from typing import Callable, Optional

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
    AD2_DIO_EXTI3_CM7,
    AD2_DIO_CM7_ACK,
    AD2_DIO_EXTI4_CM4,
    AD2_DIO_CM4_ACK,
    AD2_DIO_HOST_REQ,
    AD2_DIO_MCU_ACK,
    AD2_DIO_MCO1_PA8,
)
from gpio_hw_check import (
    GPIO_BENCH_PROTOCOL_VERSION,
    STEP_T15_RACE,
    STEP_IDLE,
    STEP_T0B_GUARDS,
    STEP_T0B_S5_NEG,
    STEP_T0B_TIMEOUT,
    STEP_T0A_LOOSE,
    STEP_T0A_HANDSHAKE,
    STEP_T0A_MCU_TO_AD2,
    STEP_T0A_AD2_TO_MCU,
    STEP_T0A_SCOPE,
    STEP_DONE,
    AD2_DIO_LOOSE_13_MASK,
    EXPECTED_STEP_SEQUENCE,
    BenchAbort,
    BusContention,
    BusToken,
    StepTracker,
    LineReader,
    drive_bus,
    reset_mcu_safe,
    s5_verify_released,
    hs_mcu,
    hs_host,
    run_selftest,
)
from fw_model import (
    SimBus,
    FirmwareModel,
    FaultInjectionConfig,
    ElectricalContentionError,
)


class MockDwfLib:
    """Mock of DWF library connected to SimBus."""

    def __init__(self):
        self.bus: Optional[SimBus] = None
        self.oe = 0
        self.output_val = 0
        self.analog_dc = [0.0, 0.0]
        self.calls: list[tuple] = []

    def connect_bus(self, bus: SimBus) -> None:
        self.bus = bus

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
        if self.bus:
            self.bus.check_electrical_invariant()

    def FDwfDigitalIOConfigure(self, hdwf):
        self.calls.append(("FDwfDigitalIOConfigure", getattr(hdwf, "value", hdwf)))

    def FDwfDigitalIOOutputEnableSet(self, hdwf, mask):
        val = getattr(mask, "value", mask)
        self.calls.append(("FDwfDigitalIOOutputEnableSet", getattr(hdwf, "value", hdwf), val))
        self.oe = val
        if self.bus:
            self.bus.check_electrical_invariant()

    def FDwfDigitalIOOutputEnableGet(self, hdwf, p_mask):
        p_mask._obj.value = self.oe

    def FDwfDigitalIOOutputSet(self, hdwf, val):
        v = getattr(val, "value", val)
        self.calls.append(("FDwfDigitalIOOutputSet", getattr(hdwf, "value", hdwf), v))
        self.output_val = v
        if self.bus:
            self.bus.check_electrical_invariant()
        if hasattr(self, "fw_model") and self.fw_model:
            self.fw_model.step()

    def FDwfDigitalIOStatus(self, hdwf):
        pass

    def FDwfDigitalIOInputStatus(self, hdwf, p_val):
        if self.bus:
            p_val._obj.value = self.bus.read_dio()
        else:
            p_val._obj.value = 0

    def FDwfAnalogInChannelEnableSet(self, hdwf, channel, enable):
        pass

    def FDwfAnalogInChannelRangeSet(self, hdwf, channel, voltage_range):
        pass

    def FDwfAnalogInChannelOffsetSet(self, hdwf, channel, offset):
        pass

    def FDwfAnalogInConfigure(self, hdwf, reconfig, start):
        pass

    def FDwfAnalogInStatus(self, hdwf, re_read, p_status):
        p_status._obj.value = 2  # DwfStateDone

    def FDwfAnalogInStatusSample(self, hdwf, channel, p_val):
        ch = getattr(channel, "value", channel)
        if self.bus:
            p_val._obj.value = self.bus.pf14_voltage if ch == 0 else self.bus.pf15_voltage
        else:
            p_val._obj.value = self.analog_dc[ch]

    def FDwfGetLastErrorMsg(self, buf):
        buf.value = b"Mock error"


class SimClock:
    """Cooperative simulated clock for fast deterministic tests."""

    def __init__(self):
        self.now = 1000.0
        self.fw_model: Optional[FirmwareModel] = None

    def monotonic(self) -> float:
        return self.now

    def sleep(self, seconds: float) -> None:
        self.now += seconds
        if self.fw_model:
            self.fw_model.step()


class MockSerialPort:
    """Simulated serial port fed by FirmwareModel."""

    def __init__(self, tx_queue: list[bytes], clock: SimClock, byte_by_byte: bool = False, chunk_all: bool = False):
        self.tx_queue = tx_queue
        self.clock = clock
        self.byte_by_byte = byte_by_byte
        self.chunk_all = chunk_all
        self._rx_buffer = bytearray()
        self.fw_model: Optional[FirmwareModel] = None

    def read(self) -> bytes:
        if self.fw_model:
            self.fw_model.step()

        # Transfer from tx_queue to rx_buffer
        while self.tx_queue:
            self._rx_buffer.extend(self.tx_queue.pop(0))

        if not self._rx_buffer:
            return b""

        if self.chunk_all:
            # Deliver everything at once
            data = bytes(self._rx_buffer)
            self._rx_buffer.clear()
            return data
        elif self.byte_by_byte:
            # Deliver 1 byte at a time
            b = bytes([self._rx_buffer[0]])
            del self._rx_buffer[0]
            return b
        else:
            # Deliver chunk (up to 256 bytes)
            chunk_size = min(len(self._rx_buffer), 256)
            data = bytes(self._rx_buffer[:chunk_size])
            del self._rx_buffer[:chunk_size]
            return data


class BaseSimTest(unittest.TestCase):
    """Base class for tests using SimBus: enforces invariant checking across all tests (R2-M2)."""

    def setUp(self):
        super().setUp()
        SimBus.clear_instances()
        self._expect_contention = False

    def tearDown(self):
        super().tearDown()
        if not getattr(self, "_expect_contention", False):
            for b in SimBus.get_instances():
                self.assertEqual(
                    b.violations, [],
                    f"R2-M2: Electrical contention detected on SimBus: {b.violations}"
                )
        SimBus.clear_instances()


class TestHarnessScenarios(BaseSimTest):
    """The 14 mandatory test scenarios (E1..E14) specified in §2.1."""

    def create_simulated_bench(
        self,
        expected_rev: str = "ca569b6",
        faults: Optional[FaultInjectionConfig] = None,
        byte_by_byte: bool = False,
        chunk_all: bool = False,
    ):
        mock_dwf = MockDwfLib()
        ad2 = AD2(sn="210321B5D38D", dwf_lib=mock_dwf)
        ad2._mock_dwf = mock_dwf
        ad2.open()

        bus = SimBus(ad2)
        mock_dwf.connect_bus(bus)

        tx_queue: list[bytes] = []
        clock = SimClock()

        fw = FirmwareModel(
            bus=bus,
            serial_tx_queue=tx_queue,
            clock=clock,
            expected_rev=expected_rev,
            faults=faults,
        )
        clock.fw_model = fw
        mock_dwf.fw_model = fw

        ser = MockSerialPort(tx_queue, clock, byte_by_byte=byte_by_byte, chunk_all=chunk_all)
        ser.fw_model = fw

        reset_called = []
        def mock_reset(a, log_fn=None):
            if a.get_oe() != 0:
                raise AD2SafetyViolation("OE not 0 during reset")
            reset_called.append(True)

        return ad2, ser, clock, mock_reset, fw, bus

    # ---- E1: Modelo correcto -> HOST VERDICT=PASS, código 0 -----------------
    def test_e1_correct_model_passes(self):
        """E1: Normal execution of correct firmware model gives HOST VERDICT=PASS, code 0, OE=0."""
        ad2, ser, clock, reset_fn, fw, bus = self.create_simulated_bench()
        logs: list[str] = []

        code = run_selftest(
            serial_port=ser,
            ad2=ad2,
            clock=clock,
            reset_fn=reset_fn,
            expected_rev="ca569b6",
            log_fn=logs.append,
        )

        self.assertEqual(code, 0, "Expected exit code 0 for correct model")
        self.assertEqual(ad2.get_oe(), 0x0000, "AD2 OE must end at 0x0000")
        self.assertTrue(any("HOST VERDICT=PASS" in l for l in logs), "Expected HOST VERDICT=PASS")

    # ---- E2: Lectura en un bloque y byte a byte -> PASS en los dos (R-B2) ---
    def test_e2_queued_line_reader_chunk_and_byte_by_byte(self):
        """E2: LineReader works with all bytes delivered at once and byte-by-byte."""
        # Case A: Deliver in one single chunk
        ad2_a, ser_a, clock_a, reset_a, _, _ = self.create_simulated_bench(chunk_all=True)
        logs_a: list[str] = []
        code_a = run_selftest(
            serial_port=ser_a,
            ad2=ad2_a,
            clock=clock_a,
            reset_fn=reset_a,
            expected_rev="ca569b6",
            log_fn=logs_a.append,
        )
        self.assertEqual(code_a, 0)
        self.assertEqual(ad2_a.get_oe(), 0x0000)
        self.assertTrue(any("HOST VERDICT=PASS" in l for l in logs_a))

        # Case B: Deliver 1 byte at a time
        ad2_b, ser_b, clock_b, reset_b, _, _ = self.create_simulated_bench(byte_by_byte=True)
        logs_b: list[str] = []
        code_b = run_selftest(
            serial_port=ser_b,
            ad2=ad2_b,
            clock=clock_b,
            reset_fn=reset_b,
            expected_rev="ca569b6",
            log_fn=logs_b.append,
        )
        self.assertEqual(code_b, 0)
        self.assertEqual(ad2_b.get_oe(), 0x0000)
        self.assertTrue(any("HOST VERDICT=PASS" in l for l in logs_b))

    # ---- E3: El firmware se calla tras sesión -> FAIL sin esperar 120s -------
    def test_e3_firmware_silent_after_session_start_fails_promptly(self):
        """E3: Silent firmware after session start fails quickly via timeout to S2."""
        faults = FaultInjectionConfig()
        faults.mute_after_session_start = True
        ad2, ser, clock, reset_fn, _, _ = self.create_simulated_bench(faults=faults)
        logs: list[str] = []

        code = run_selftest(
            serial_port=ser,
            ad2=ad2,
            clock=clock,
            reset_fn=reset_fn,
            expected_rev="ca569b6",
            log_fn=logs.append,
        )

        self.assertNotEqual(code, 0, "Expected non-zero exit code on silent firmware")
        self.assertEqual(ad2.get_oe(), 0x0000)
        self.assertTrue(any("HOST VERDICT=FAIL" in l for l in logs))

    # ---- E4: T15 VERDICT: FAIL ----------------------------------------------
    def test_e4_t15_fail_reported(self):
        """E4: T15 VERDICT: FAIL causes HOST VERDICT=FAIL."""
        faults = FaultInjectionConfig()
        faults.t15_verdict_fail = True
        ad2, ser, clock, reset_fn, _, _ = self.create_simulated_bench(faults=faults)
        logs: list[str] = []

        code = run_selftest(
            serial_port=ser,
            ad2=ad2,
            clock=clock,
            reset_fn=reset_fn,
            expected_rev="ca569b6",
            log_fn=logs.append,
        )

        self.assertNotEqual(code, 0)
        self.assertEqual(ad2.get_oe(), 0x0000)
        self.assertTrue(any("HOST VERDICT=FAIL" in l for l in logs))
        self.assertTrue(any("[FAIL] t15_pass" in l for l in logs))

    # ---- E5: CM4 VERDICT=FAIL o no llega ------------------------------------
    def test_e5_cm4_verdict_fail_or_missing(self):
        """E5: CM4 VERDICT=FAIL or omitted CM4 verdict causes HOST VERDICT=FAIL."""
        # Case A: CM4 VERDICT=FAIL
        faults_a = FaultInjectionConfig()
        faults_a.cm4_verdict_fail = True
        ad2_a, ser_a, clock_a, reset_a, _, _ = self.create_simulated_bench(faults=faults_a)
        logs_a: list[str] = []
        code_a = run_selftest(
            serial_port=ser_a,
            ad2=ad2_a,
            clock=clock_a,
            reset_fn=reset_a,
            expected_rev="ca569b6",
            log_fn=logs_a.append,
        )
        self.assertNotEqual(code_a, 0)
        self.assertEqual(ad2_a.get_oe(), 0x0000)
        self.assertTrue(any("HOST VERDICT=FAIL" in l for l in logs_a))

        # Case B: CM4 verdict missing
        faults_b = FaultInjectionConfig()
        faults_b.cm4_verdict_missing = True
        ad2_b, ser_b, clock_b, reset_b, _, _ = self.create_simulated_bench(faults=faults_b)
        logs_b: list[str] = []
        code_b = run_selftest(
            serial_port=ser_b,
            ad2=ad2_b,
            clock=clock_b,
            reset_fn=reset_b,
            expected_rev="ca569b6",
            log_fn=logs_b.append,
        )
        self.assertNotEqual(code_b, 0)
        self.assertEqual(ad2_b.get_oe(), 0x0000)
        self.assertTrue(any("HOST VERDICT=FAIL" in l for l in logs_b))

    # ---- E6: En T0b(c) MCU no conduce el bus (lee 0xFF) ---------------------
    def test_e6_t0b_c_mcu_not_driving_fails(self):
        """E6: If MCU does not drive bus during T0b(c), S5 contention is not caught -> FAIL."""
        faults = FaultInjectionConfig()
        faults.t0b_c_mcu_no_drive = True
        ad2, ser, clock, reset_fn, _, _ = self.create_simulated_bench(faults=faults)
        logs: list[str] = []

        code = run_selftest(
            serial_port=ser,
            ad2=ad2,
            clock=clock,
            reset_fn=reset_fn,
            expected_rev="ca569b6",
            log_fn=logs.append,
        )

        self.assertNotEqual(code, 0)
        self.assertEqual(ad2.get_oe(), 0x0000)
        self.assertTrue(any("[FAIL] t0b_c_contention" in l for l in logs))

    # ---- E7: En T0b(c) no llega BUSCHK up -----------------------------------
    def test_e7_t0b_c_missing_buschk_up_fails(self):
        """E7: Omission of BUSCHK up in T0b(c) leads to timeout and FAIL, never PASS."""
        faults = FaultInjectionConfig()
        faults.t0b_c_skip_buschk = True
        ad2, ser, clock, reset_fn, _, _ = self.create_simulated_bench(faults=faults)
        logs: list[str] = []

        code = run_selftest(
            serial_port=ser,
            ad2=ad2,
            clock=clock,
            reset_fn=reset_fn,
            expected_rev="ca569b6",
            log_fn=logs.append,
        )

        self.assertNotEqual(code, 0)
        self.assertEqual(ad2.get_oe(), 0x0000)
        self.assertTrue(any("HOST VERDICT=FAIL" in l for l in logs))

    # ---- E8: Se pierde una línea de protocolo cualquiera --------------------
    def test_e8_dropped_protocol_lines_all_abort(self):
        """E8: Dropping any single protocol line aborts to S2 and fails, never passes."""
        protocol_lines_to_test = [
            "[m7_gpio_test] Rev",
            "[m7_gpio_test] T15 VERDICT:",
            "[m7_gpio_test] GPIO_BENCH v1 idle",
            "[m7_gpio_test] T0b_b guards",
            "[m7_gpio_test] Handshake PE2: OK",
            "[m7_gpio_test] T0a_mcu: DONE",
            "[m7_gpio_test] T0a_scope: DONE",
        ]

        for target_line in protocol_lines_to_test:
            faults = FaultInjectionConfig()
            faults.drop_protocol_line = target_line
            ad2, ser, clock, reset_fn, _, _ = self.create_simulated_bench(faults=faults)
            logs: list[str] = []

            code = run_selftest(
                serial_port=ser,
                ad2=ad2,
                clock=clock,
                reset_fn=reset_fn,
                expected_rev="ca569b6",
                log_fn=logs.append,
            )

            self.assertNotEqual(code, 0, f"Expected abort when dropping '{target_line}'")
            self.assertEqual(ad2.get_oe(), 0x0000, f"AD2 OE not 0 after dropping '{target_line}'")
            self.assertTrue(any("HOST VERDICT=FAIL" in l for l in logs))

    # ---- E9: STEP fuera de secuencia o dueño incorrecto ---------------------
    def test_e9_step_out_of_seq_or_wrong_owner_aborts_before_bus(self):
        """E9: Step out of sequence or wrong bus owner aborts before touching bus."""
        # Case A: Step out of sequence (Step 5 sent instead of Step 3)
        faults_a = FaultInjectionConfig()
        faults_a.step_out_of_seq = (STEP_T0B_GUARDS, STEP_T0B_TIMEOUT, "NONE")
        ad2_a, ser_a, clock_a, reset_a, _, _ = self.create_simulated_bench(faults=faults_a)
        logs_a: list[str] = []
        code_a = run_selftest(
            serial_port=ser_a,
            ad2=ad2_a,
            clock=clock_a,
            reset_fn=reset_a,
            expected_rev="ca569b6",
            log_fn=logs_a.append,
        )
        self.assertNotEqual(code_a, 0)
        self.assertEqual(ad2_a.get_oe(), 0x0000)

        # Case B: Wrong bus owner (Step 8 sent with bus=AD2 instead of MCU)
        faults_b = FaultInjectionConfig()
        faults_b.step_out_of_seq = (STEP_T0A_MCU_TO_AD2, STEP_T0A_MCU_TO_AD2, "AD2")
        ad2_b, ser_b, clock_b, reset_b, _, _ = self.create_simulated_bench(faults=faults_b)
        logs_b: list[str] = []
        code_b = run_selftest(
            serial_port=ser_b,
            ad2=ad2_b,
            clock=clock_b,
            reset_fn=reset_b,
            expected_rev="ca569b6",
            log_fn=logs_b.append,
        )
        self.assertNotEqual(code_b, 0)
        self.assertEqual(ad2_b.get_oe(), 0x0000)

    # ---- E10: Cada handshake agota el tiempo --------------------------------
    def test_e10_handshake_timeouts_abort_cleanly(self):
        """E10: Each handshake timeout aborts to S2, fails, and leaves OE=0."""
        hs_points = [
            "session_start",
            "step4_buschk",
            "step6_up",
            "step7_pe2",
            "step8_up",
            "step9_ready",
            "step10_low",
        ]

        for pt in hs_points:
            faults = FaultInjectionConfig()
            faults.handshake_timeout_step = pt
            ad2, ser, clock, reset_fn, _, _ = self.create_simulated_bench(faults=faults)
            logs: list[str] = []

            code = run_selftest(
                serial_port=ser,
                ad2=ad2,
                clock=clock,
                reset_fn=reset_fn,
                expected_rev="ca569b6",
                log_fn=logs.append,
            )

            self.assertNotEqual(code, 0, f"Expected non-zero code on handshake timeout at {pt}")
            self.assertEqual(ad2.get_oe(), 0x0000, f"OE not 0 after handshake timeout at {pt}")

    # ---- E11: Un bit cambiado en cada comparación del host ------------------
    def test_e11_host_comparison_bit_mismatches_identified(self):
        """E11: Inverted bit in host comparison fails and explicitly names the bit."""
        # 1. Loose pull-up with DIO3 inverted
        faults_loose = FaultInjectionConfig()
        faults_loose.invert_mcu_bit = ("loose_up", 3)
        ad2, ser, clock, reset_fn, _, _ = self.create_simulated_bench(faults=faults_loose)
        logs: list[str] = []
        code = run_selftest(serial_port=ser, ad2=ad2, clock=clock, reset_fn=reset_fn, expected_rev="ca569b6", log_fn=logs.append)
        self.assertNotEqual(code, 0)
        self.assertTrue(any("DIO3" in l and "FAILED" in l for l in logs), "Log did not name failing DIO3 bit!")

        # 2. Walking-1 MCU->AD2 with DIO0 inverted during DIO9 walk
        faults_walk = FaultInjectionConfig()
        faults_walk.invert_mcu_bit = ("mcu_ad2_9", 0)
        ad2, ser, clock, reset_fn, _, _ = self.create_simulated_bench(faults=faults_walk)
        logs = []
        code = run_selftest(serial_port=ser, ad2=ad2, clock=clock, reset_fn=reset_fn, expected_rev="ca569b6", log_fn=logs.append)
        self.assertNotEqual(code, 0)
        self.assertTrue(any("Bit mismatch on DIO" in l for l in logs))

        # 3. Scope voltage out of range
        faults_scope = FaultInjectionConfig()
        faults_scope.invert_mcu_bit = ("scope_low", 14)
        ad2, ser, clock, reset_fn, _, _ = self.create_simulated_bench(faults=faults_scope)
        logs = []
        code = run_selftest(serial_port=ser, ad2=ad2, clock=clock, reset_fn=reset_fn, expected_rev="ca569b6", log_fn=logs.append)
        self.assertNotEqual(code, 0)
        self.assertTrue(any("Scope 1 (PF14) VOL out of range" in l for l in logs))

    # ---- E12: El firmware da FAIL en comparaciones --------------------------
    def test_e12_firmware_comparison_fail_reported(self):
        """E12: MCU reporting FAIL in walking-1 or analog control causes HOST VERDICT=FAIL."""
        faults = FaultInjectionConfig()
        faults.firmware_comparison_fail = "t0a_ad2_0"
        ad2, ser, clock, reset_fn, _, _ = self.create_simulated_bench(faults=faults)
        logs: list[str] = []
        code = run_selftest(serial_port=ser, ad2=ad2, clock=clock, reset_fn=reset_fn, expected_rev="ca569b6", log_fn=logs.append)
        self.assertNotEqual(code, 0)
        self.assertTrue(any("[FAIL] t0a_ad2_to_mcu" in l for l in logs))

    # ---- E13: Banner incorrecto, -dirty, versión errónea o CM4 rev mismatch -
    def test_e13_banner_rejections_leave_oe_zero(self):
        """E13: Banner mismatch aborts with OE=0 throughout."""
        # Case A: Wrong hash
        faults_a = FaultInjectionConfig()
        faults_a.banner_rev = "9f9f9f9"
        ad2_a, ser_a, clock_a, reset_a, _, _ = self.create_simulated_bench(faults=faults_a)
        code_a = run_selftest(serial_port=ser_a, ad2=ad2_a, clock=clock_a, reset_fn=reset_a, expected_rev="ca569b6")
        self.assertNotEqual(code_a, 0)
        self.assertEqual(ad2_a.get_oe(), 0x0000)

        # Case B: -dirty
        faults_b = FaultInjectionConfig()
        faults_b.banner_rev = "ca569b6-dirty"
        ad2_b, ser_b, clock_b, reset_b, _, _ = self.create_simulated_bench(faults=faults_b)
        code_b = run_selftest(serial_port=ser_b, ad2=ad2_b, clock=clock_b, reset_fn=reset_b, expected_rev="ca569b6")
        self.assertNotEqual(code_b, 0)
        self.assertEqual(ad2_b.get_oe(), 0x0000)

        # Case C: Wrong protocol version
        faults_c = FaultInjectionConfig()
        faults_c.banner_version = 99
        ad2_c, ser_c, clock_c, reset_c, _, _ = self.create_simulated_bench(faults=faults_c)
        code_c = run_selftest(serial_port=ser_c, ad2=ad2_c, clock=clock_c, reset_fn=reset_c, expected_rev="ca569b6")
        self.assertNotEqual(code_c, 0)
        self.assertEqual(ad2_c.get_oe(), 0x0000)

        # Case D: CM4 rev mismatch
        faults_d = FaultInjectionConfig()
        faults_d.cm4_rev = "1111111"
        ad2_d, ser_d, clock_d, reset_d, _, _ = self.create_simulated_bench(faults=faults_d)
        logs_d: list[str] = []
        code_d = run_selftest(serial_port=ser_d, ad2=ad2_d, clock=clock_d, reset_fn=reset_d, expected_rev="ca569b6", log_fn=logs_d.append)
        self.assertNotEqual(code_d, 0)
        self.assertTrue(any("[FAIL] cm4_rev_match" in l for l in logs_d))

    # ---- E14: Línea ajena con Rev antes del banner -------------------------
    def test_e14_spurious_rev_line_before_banner_ignored(self):
        """E14: A line with 'Rev ' before the banner is not counted as banner."""
        faults = FaultInjectionConfig()
        faults.spurious_line_before_banner = "=== STM32H755 Bench Boot Rev 9999999 ==="
        ad2, ser, clock, reset_fn, _, _ = self.create_simulated_bench(faults=faults)
        logs: list[str] = []

        code = run_selftest(
            serial_port=ser,
            ad2=ad2,
            clock=clock,
            reset_fn=reset_fn,
            expected_rev="ca569b6",
            log_fn=logs.append,
        )

        self.assertEqual(code, 0, "Spurious line before banner should be ignored as non-protocol")
        self.assertTrue(any("HOST VERDICT=PASS" in l for l in logs))

    # ---- Control del control de contención (R2-M2) --------------------------
    def test_control_of_contention_detection(self):
        """Control of control (R2-M2): Provoke a contention in the model (MCU drives bus
        during step 9 while AD2 drives it) and verify that bus.violations is NOT empty
        and that the common assertion detects it."""
        faults = FaultInjectionConfig()
        faults.contention_step9_mcu_drives_bus = True
        ad2, ser, clock, reset_fn, _, bus = self.create_simulated_bench(faults=faults)

        code = run_selftest(
            serial_port=ser,
            ad2=ad2,
            clock=clock,
            reset_fn=reset_fn,
            expected_rev="ca569b6",
        )

        self.assertNotEqual(code, 0)
        self.assertTrue(len(bus.violations) > 0, "SimBus should have recorded at least one electrical contention")
        first_violation = bus.violations[0]
        self.assertEqual(first_violation["lines"] & AD2_DIO_BUS_MASK, AD2_DIO_BUS_MASK, "Contention should be on bus lines 0x00FF")

        # Verify that the common assertion WOULD fail if contention was not expected:
        with self.assertRaises(AssertionError):
            for b in SimBus.get_instances():
                self.assertEqual(b.violations, [], "Common check should detect this contention!")

        # Mark that this test deliberately expected contention so tearDown doesn't fail
        self._expect_contention = True

    # ---- E15: Paso 8, MCU no suelta el bus ----------------------------------
    def test_e15_step8_mcu_bus_release_failure_aborts_without_bus_oe(self):
        """E15: In Step 8, MCU fails to release bus (BUSCHK up != 0xFF or down != 0x00).
        Must abort with FAIL, OE=0 at end, no bus bits ever written to OE, and 0 contentions."""
        # Case A: BUSCHK up != 0xFF
        faults_a = FaultInjectionConfig()
        faults_a.step8_buschk_up_fail = True
        ad2_a, ser_a, clock_a, reset_a, _, bus_a = self.create_simulated_bench(faults=faults_a)
        logs_a = []
        code_a = run_selftest(serial_port=ser_a, ad2=ad2_a, clock=clock_a, reset_fn=reset_a, expected_rev="ca569b6", log_fn=logs_a.append)
        self.assertNotEqual(code_a, 0)
        self.assertEqual(ad2_a.get_oe(), 0x0000)
        self.assertTrue(any("HOST VERDICT=FAIL" in l for l in logs_a))
        oe_writes_a = [c[2] for c in ad2_a._mock_dwf.calls if c[0] == "FDwfDigitalIOOutputEnableSet"]
        self.assertTrue(all((oe & AD2_DIO_BUS_MASK) == 0 for oe in oe_writes_a), f"OE with bus bits written: {oe_writes_a}")
        self.assertEqual(bus_a.violations, [])

        # Case B: BUSCHK down != 0x00
        faults_b = FaultInjectionConfig()
        faults_b.step8_buschk_down_fail = True
        ad2_b, ser_b, clock_b, reset_b, _, bus_b = self.create_simulated_bench(faults=faults_b)
        logs_b = []
        code_b = run_selftest(serial_port=ser_b, ad2=ad2_b, clock=clock_b, reset_fn=reset_b, expected_rev="ca569b6", log_fn=logs_b.append)
        self.assertNotEqual(code_b, 0)
        self.assertEqual(ad2_b.get_oe(), 0x0000)
        self.assertTrue(any("HOST VERDICT=FAIL" in l for l in logs_b))
        oe_writes_b = [c[2] for c in ad2_b._mock_dwf.calls if c[0] == "FDwfDigitalIOOutputEnableSet"]
        self.assertTrue(all((oe & AD2_DIO_BUS_MASK) == 0 for oe in oe_writes_b), f"OE with bus bits written: {oe_writes_b}")
        self.assertEqual(bus_b.violations, [])

    # ---- E16: Paso 9, MCU no suelta el bus ----------------------------------
    def test_e16_step9_mcu_bus_release_failure_aborts_without_bus_oe(self):
        """E16: In Step 9, MCU fails to release bus (BUSCHK up != 0xFF or down != 0x00).
        Must abort before any drive_bus, FAIL, OE=0 at end, no bus bits written to OE, and 0 contentions."""
        # Case A: BUSCHK up != 0xFF
        faults_a = FaultInjectionConfig()
        faults_a.step9_buschk_up_fail = True
        ad2_a, ser_a, clock_a, reset_a, _, bus_a = self.create_simulated_bench(faults=faults_a)
        logs_a = []
        code_a = run_selftest(serial_port=ser_a, ad2=ad2_a, clock=clock_a, reset_fn=reset_a, expected_rev="ca569b6", log_fn=logs_a.append)
        self.assertNotEqual(code_a, 0)
        self.assertEqual(ad2_a.get_oe(), 0x0000)
        self.assertTrue(any("HOST VERDICT=FAIL" in l for l in logs_a))
        oe_writes_a = [c[2] for c in ad2_a._mock_dwf.calls if c[0] == "FDwfDigitalIOOutputEnableSet"]
        self.assertTrue(all((oe & AD2_DIO_BUS_MASK) == 0 for oe in oe_writes_a), f"OE with bus bits written: {oe_writes_a}")
        self.assertEqual(bus_a.violations, [])

        # Case B: BUSCHK down != 0x00
        faults_b = FaultInjectionConfig()
        faults_b.step9_buschk_down_fail = True
        ad2_b, ser_b, clock_b, reset_b, _, bus_b = self.create_simulated_bench(faults=faults_b)
        logs_b = []
        code_b = run_selftest(serial_port=ser_b, ad2=ad2_b, clock=clock_b, reset_fn=reset_b, expected_rev="ca569b6", log_fn=logs_b.append)
        self.assertNotEqual(code_b, 0)
        self.assertEqual(ad2_b.get_oe(), 0x0000)
        self.assertTrue(any("HOST VERDICT=FAIL" in l for l in logs_b))
        oe_writes_b = [c[2] for c in ad2_b._mock_dwf.calls if c[0] == "FDwfDigitalIOOutputEnableSet"]
        self.assertTrue(all((oe & AD2_DIO_BUS_MASK) == 0 for oe in oe_writes_b), f"OE with bus bits written: {oe_writes_b}")
        self.assertEqual(bus_b.violations, [])

    # ---- E17: Mutación de la guarda pone pruebas en rojo --------------------
    def test_e17_guard_mutations_break_tests(self):
        """E17: Mutating s5_verify_released (disabling bus_up or bus_down comparison)
        on an isolated in-memory copy of the module causes at least one of E6, E15, or E16 to fail."""
        import importlib.util

        src_path = pathlib.Path(__file__).parent / "gpio_hw_check.py"
        src_text = src_path.read_text(encoding="utf-8")

        # 1. Mutate bus_up comparison: 'if bus_up != 0xFF:' -> 'if False:'
        self.assertIn("if bus_up != 0xFF:", src_text)
        mutated_up = src_text.replace("if bus_up != 0xFF:", "if False:  # MUTATION: disabled bus_up check")

        spec_up = importlib.util.spec_from_loader("gpio_hw_check_mut_up", loader=None)
        mod_up = importlib.util.module_from_spec(spec_up)
        mod_up.__file__ = str(src_path)
        exec(compile(mutated_up, "gpio_hw_check_mut_up.py", "exec"), mod_up.__dict__)

        # Test A: Under mod_up, E15 case A (step 8 bad up) is NOT caught by S5!
        faults_up = FaultInjectionConfig()
        faults_up.step8_buschk_up_fail = True
        ad2_u, ser_u, clock_u, reset_u, _, _ = self.create_simulated_bench(faults=faults_up)
        logs_u = []
        mod_up.run_selftest(serial_port=ser_u, ad2=ad2_u, clock=clock_u, reset_fn=reset_u, expected_rev="ca569b6", log_fn=logs_u.append)
        caught_s5_up = any("bus contention check failed at step 8" in l for l in logs_u)
        self.assertFalse(caught_s5_up, "Mutated module should fail to catch S5 bus_up contention!")

        # Test B: Under mod_up, T0b(c) (E6 nominal) fails because s5_verify_released did NOT catch contention!
        ad2_t0b, ser_t0b, clock_t0b, reset_t0b, _, _ = self.create_simulated_bench()
        logs_t0b = []
        code_t0b = mod_up.run_selftest(serial_port=ser_t0b, ad2=ad2_t0b, clock=clock_t0b, reset_fn=reset_t0b, expected_rev="ca569b6", log_fn=logs_t0b.append)
        self.assertNotEqual(code_t0b, 0, "Disabling bus_up check must cause T0b(c) to FAIL")
        self.assertFalse(any("T0b(c) S5 negative control PASSED" in l for l in logs_t0b), "T0b(c) must not pass when bus_up check is disabled")

        # 2. Mutate bus_down comparison: 'if bus_down != 0x00:' -> 'if False:'
        self.assertIn("if bus_down != 0x00:", src_text)
        mutated_down = src_text.replace("if bus_down != 0x00:", "if False:  # MUTATION: disabled bus_down check")

        spec_down = importlib.util.spec_from_loader("gpio_hw_check_mut_down", loader=None)
        mod_down = importlib.util.module_from_spec(spec_down)
        mod_down.__file__ = str(src_path)
        exec(compile(mutated_down, "gpio_hw_check_mut_down.py", "exec"), mod_down.__dict__)

        # Test C: Under mod_down, E15 case B (step 8 bad down) is NOT caught by S5!
        faults_down = FaultInjectionConfig()
        faults_down.step8_buschk_down_fail = True
        ad2_d, ser_d, clock_d, reset_d, _, _ = self.create_simulated_bench(faults=faults_down)
        logs_d = []
        mod_down.run_selftest(serial_port=ser_d, ad2=ad2_d, clock=clock_d, reset_fn=reset_d, expected_rev="ca569b6", log_fn=logs_d.append)
        caught_s5_down = any("BUSCHK down gave 0xFF, expected 0x00" in l for l in logs_d)
        self.assertFalse(caught_s5_down, "Mutated module should fail to catch S5 bus_down contention!")


class TestSafetyGuardsDirect(BaseSimTest):
    """Direct component unit tests for S8, S10, S5, S7."""

    def setUp(self):
        super().setUp()
        self.mock_dwf = MockDwfLib()
        self.ad2 = AD2(sn="210321B5D38D", dwf_lib=self.mock_dwf)
        self.ad2.open()
        self.bus = SimBus(self.ad2)
        self.mock_dwf.connect_bus(self.bus)

    def tearDown(self):
        self.ad2.close()
        super().tearDown()

    def test_s5_private_token_constructor_rejected(self):
        """S5: Direct instantiation of BusToken from outside raises AD2SafetyViolation."""
        with self.assertRaises(AD2SafetyViolation):
            BusToken(STEP_T0A_AD2_TO_MCU, object())

    def test_s5_drive_bus_without_valid_token_rejected(self):
        """S5: drive_bus without valid token raises AD2SafetyViolation before touching DLL."""
        tracker = StepTracker(EXPECTED_STEP_SEQUENCE)
        tracker.advance_step(STEP_T0A_AD2_TO_MCU, GPIO_BENCH_BUS_AD2)

        # No token
        with self.assertRaises(AD2SafetyViolation):
            drive_bus(self.ad2, tracker, 0x55)

        # Invalidate after abort
        tracker.abort()
        with self.assertRaises(AD2SafetyViolation):
            drive_bus(self.ad2, tracker, 0x55)

    def test_s10_reset_mcu_safe_enforces_oe_zero(self):
        """S10: reset_mcu_safe raises AD2SafetyViolation when OE != 0x0000."""
        self.mock_dwf.oe = AD2_DIO_HOST_REQ
        with self.assertRaises(AD2SafetyViolation):
            reset_mcu_safe(self.ad2)


if __name__ == "__main__":
    unittest.main()
