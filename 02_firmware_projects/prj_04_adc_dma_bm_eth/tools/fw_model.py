#!/usr/bin/env python3
"""Firmware model for STM32H755 GPIO Selftest (CM7/CM4).

Standard library only. Simulates MCU register state, pin drivers,
protocol steps, and R-B1 handshakes (HS_MCU, HS_HOST).
Enforces the electrical invariant:
    (mcu_outputs_mask & ad2.get_oe()) == 0
at every step, clock tick, and pin transition.
"""

from __future__ import annotations

import re
import time
from typing import Callable, Optional, Sequence

try:
    from tools.gpio_hw_check import (
        PROTOCOL_STEPS, PROTOCOL_ITEMS, BenchItem, StepDef,
        table_pattern, table_item, GPIO_BENCH_PROTOCOL_VERSION
    )
except ImportError:
    from gpio_hw_check import (
        PROTOCOL_STEPS, PROTOCOL_ITEMS, BenchItem, StepDef,
        table_pattern, table_item, GPIO_BENCH_PROTOCOL_VERSION
    )


class ElectricalContentionError(Exception):
    """Raised when both MCU and AD2 drive the same line at the same time."""
    pass


# DIO bit definitions matching gpio_bench_protocol.h
DIO_BUS_MASK     = 0x00FF       # DIO0..DIO7 -> PE8..PE15
DIO_EXTI3_CM7    = (1 << 8)     # DIO8 -> PE3 (Solo AD2)
DIO_CM7_ACK      = (1 << 9)     # DIO9 -> PE5 (Solo MCU)
DIO_EXTI4_CM4    = (1 << 10)    # DIO10 -> PE4 (Solo AD2)
DIO_CM4_ACK      = (1 << 11)    # DIO11 -> PE6 (Solo MCU)
DIO_HOST_REQ     = (1 << 12)    # DIO12 -> PE2 (Solo AD2)
DIO_MCU_ACK      = (1 << 13)    # DIO13 -> PE7 (Solo MCU)
DIO_MCO1_PA8     = (1 << 14)    # DIO14 -> PA8 (Solo MCU)

DIO_LOOSE_13_MASK = (
    DIO_BUS_MASK | DIO_EXTI3_CM7 | DIO_CM7_ACK |
    DIO_EXTI4_CM4 | DIO_CM4_ACK | DIO_MCO1_PA8
)

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
STEP_T1_REJECT      = 11
STEP_T2_PARITY      = 12
STEP_B1_MEASURE     = 13
STEP_T16_LCKR       = 20
STEP_T3_MCU_OPS      = 30
STEP_T4_AD2_PATTERNS = 31
STEP_T7_ANALOG_IDR   = 32
STEP_T8_NO_GLITCH    = 33
STEP_T5_PULLS        = 34
STEP_T6_OPEN_DRAIN   = 35
STEP_T9_RISE_TIME    = 36
STEP_T10_MCO1        = 37
STEP_DONE           = 99


class SimBus:
    """Models the physical wiring between MCU and AD2."""

    _instances: list[SimBus] = []

    @classmethod
    def register(cls, instance: SimBus) -> None:
        cls._instances.append(instance)

    @classmethod
    def clear_instances(cls) -> None:
        cls._instances.clear()

    @classmethod
    def get_instances(cls) -> list[SimBus]:
        return list(cls._instances)

    def __init__(self, ad2):
        self.ad2 = ad2
        # MCU pin states
        self.mcu_oe = 0          # Bitmask of DIO lines where MCU is OUTPUT
        self.mcu_odr = 0         # Bitmask of values driven by MCU
        self.mcu_pupdr = 0       # 0: none, 1: pull-up, 2: pull-down for each bit (2 bits per line)
        self.mcu_analog = 0      # Bitmask of lines in analog mode (IDR reads 0)

        # PF14 / PF15 analog scope voltages
        self.pf14_voltage = 0.0
        self.pf15_voltage = 0.0

        # F1a measurement sources, read by the mock DWF library:
        #   la_pre_value / la_capture: DIO words before the trigger and, from the
        #   trigger on, a list of (word, duration in s) the firmware produced;
        #   dio14_freq_hz: square wave on DIO14 (MCO1) for untriggered captures;
        #   scope_edge: (tr PF14, tr PF15) in ns of the next rising edge, or None.
        self.la_pre_value = 0
        self.la_capture: Optional[list[tuple[int, float]]] = None
        self.dio14_freq_hz = 0.0
        self.scope_edge: Optional[tuple[float, float]] = None

        # R2-M2: Contention tracking list
        self.violations: list[dict] = []
        SimBus.register(self)

    def check_electrical_invariant(self) -> None:
        """Enforces: never are both AD2 and MCU driving the same pin at the same time."""
        ad2_oe = self.ad2.get_oe()
        contention = self.mcu_oe & ad2_oe
        if contention != 0:
            rec = {
                "timestamp": time.monotonic(),
                "lines": contention,
                "mcu_oe": self.mcu_oe,
                "ad2_oe": ad2_oe,
            }
            self.violations.append(rec)
            raise ElectricalContentionError(
                f"ELECTRICAL INVARIANT VIOLATED! Contention on lines 0x{contention:04X} "
                f"(MCU OE=0x{self.mcu_oe:04X}, AD2 OE=0x{ad2_oe:04X})"
            )

    def set_mcu_output(self, pin_mask: int, val_mask: int) -> None:
        """MCU claims pins as outputs and sets their output data register."""
        self.mcu_oe |= pin_mask
        self.check_electrical_invariant()
        self.mcu_odr = (self.mcu_odr & ~pin_mask) | (val_mask & pin_mask)

    def clear_mcu_output(self, pin_mask: int) -> None:
        """MCU releases pins back to input mode."""
        self.mcu_oe &= ~pin_mask
        self.mcu_odr &= ~pin_mask
        self.check_electrical_invariant()

    def set_mcu_safe(self) -> None:
        """Enforce S2 on MCU: all pins input except PE7 (MCU_ACK) which is OUTPUT LOW."""
        self.mcu_oe = DIO_MCU_ACK
        self.mcu_odr = 0
        self.mcu_analog = 0
        self.mcu_pupdr = 0
        self.pf14_voltage = 0.0
        self.pf15_voltage = 0.0
        self.check_electrical_invariant()

    def read_mcu_idr(self, mask: int) -> int:
        """MCU reads Input Data Register for the specified mask."""
        val = 0
        ad2_oe = self.ad2.get_oe()
        ad2_out = getattr(self.ad2, "_output_val", getattr(self.ad2, "output_val", 0))

        for b in range(16):
            bmask = (1 << b)
            if not (mask & bmask):
                continue
            if self.mcu_analog & bmask:
                # Analog mode: Schmitt trigger disconnected, reads 0
                bit_val = 0
            elif ad2_oe & bmask:
                # AD2 is driving
                bit_val = 1 if (ad2_out & bmask) else 0
            elif self.mcu_oe & bmask:
                # MCU is driving
                bit_val = 1 if (self.mcu_odr & bmask) else 0
            else:
                # Floating or pulled
                pull = (self.mcu_pupdr >> (b * 2)) & 3
                if pull == 1:
                    bit_val = 1
                else:
                    bit_val = 0
            if bit_val:
                val |= bmask
        return val

    def read_dio(self) -> int:
        """AD2 reads DIO pins according to physical state."""
        val = 0
        ad2_oe = self.ad2.get_oe()
        ad2_out = getattr(self.ad2, "_output_val", getattr(self.ad2, "output_val", 0))

        for b in range(16):
            bmask = (1 << b)
            if ad2_oe & bmask:
                bit_val = 1 if (ad2_out & bmask) else 0
            elif self.mcu_oe & bmask:
                bit_val = 1 if (self.mcu_odr & bmask) else 0
            else:
                pull = (self.mcu_pupdr >> (b * 2)) & 3
                if pull == 1:
                    bit_val = 1
                else:
                    bit_val = 0
            if bit_val:
                val |= bmask
        return val


class FaultInjectionConfig:
    """Configures deliberate faults for E1..E14 scenario validation."""

    def __init__(self):
        self.mute_after_session_start = False     # E3
        self.t15_verdict_fail = False             # E4
        self.cm4_verdict_fail = False             # E5
        self.cm4_verdict_missing = False          # E5
        self.t0b_c_mcu_no_drive = False           # E6 (reads 0xFF instead of 0x5A)
        self.t0b_c_skip_buschk = False            # E7 (omits BUSCHK up)
        self.drop_protocol_line: Optional[str] = None  # E8
        self.step_out_of_seq: Optional[tuple[int, int, str]] = None  # E9: (trigger_step, bad_id, bad_bus)
        self.handshake_timeout_step: Optional[str] = None            # E10: step identifier where MCU stalls
        self.invert_mcu_bit: Optional[tuple[str, int]] = None        # E11: (test_name, bit_to_invert)
        self.firmware_comparison_fail: Optional[str] = None          # E12: 't0a_ad2' or 'analog'
        self.banner_rev: Optional[str] = None     # E13: custom rev in banner
        self.banner_version: Optional[int] = None # E13: custom version
        self.cm4_rev: Optional[str] = None        # E13: custom CM4 rev
        self.cm4_early_rev: Optional[str] = None  # R2-m2: CM4 rev emitted during boot
        self.cm4_early_pass: bool = False         # R2-m2: CM4 passes during boot
        self.spurious_line_before_banner: Optional[str] = None  # E14
        self.contention_step9_mcu_drives_bus: bool = False      # R2-M2: MCU drives bus while AD2 drives in Step 9
        self.step8_buschk_up_fail: bool = False                 # E15: MCU fails buschk up in Step 8
        self.step8_buschk_down_fail: bool = False               # E15: MCU fails buschk down in Step 8
        self.step9_buschk_up_fail: bool = False                 # E16: MCU fails buschk up in Step 9
        self.step9_buschk_down_fail: bool = False               # E16: MCU fails buschk down in Step 9
        self.t0b_c_safe_fail: bool = False                      # R2-m5: T0b(c) safe state check fails
        self.inject_step7_protocol_abort: bool = False          # R3-m1: MCU aborts during Step 7 handshake
        self.lckr_locked_at_boot: bool = False                  # F1a: GPIOF LCKR not clean at boot
        self.verdict_fail: Optional[str] = None                 # F1a: 't1', 't2', 'b1' or 't16' reports FAIL
        self.bench_fault: Optional[str] = None                  # F1a bench: see FirmwareModel._bench_f1a
        self.buschk_fail: Optional[tuple[int, str]] = None      # F1a: (step, 'up'|'down') MCU keeps a bus line


class FirmwareModel:
    """Cortex-M7 and Cortex-M4 selftest firmware simulation."""

    def __init__(
        self,
        bus: SimBus,
        serial_tx_queue: list[bytes],
        clock,
        expected_rev: str = "ca569b6",
        faults: Optional[FaultInjectionConfig] = None,
    ):
        self.bus = bus
        self.tx_queue = serial_tx_queue
        self.clock = clock
        self.expected_rev = expected_rev
        self.faults = faults or FaultInjectionConfig()

        self.step_id = 0
        self.state = "INIT"
        self._running = False
        self._step_generator = None
        self.visited_items: list[str] = []
        self.trace: list[tuple[str, str]] = []

        self.bus.set_mcu_safe()

    def reset(self) -> None:
        """Simulate hardware reset: boot MCU firmware and enter initial safe state."""
        self._running = True
        self._step_generator = self.run_coroutine()
        self.bus.set_mcu_safe()

    def log_m7(self, msg: str, item_name: Optional[str] = None) -> None:
        """Emit a line from CM7."""
        if item_name:
            self.visited_items.append(item_name)
        if self.faults.drop_protocol_line:
            if (item_name and self.faults.drop_protocol_line == item_name) or (self.faults.drop_protocol_line in msg):
                return
        if item_name:
            try:
                pat = table_pattern(item_name)
            except KeyError:
                pat = msg
            self.trace.append(("LINE", pat))
        line = f"{msg}\r\n".encode("utf-8")
        self.tx_queue.append(line)

    def log_m4(self, msg: str) -> None:
        """Emit a line from CM4."""
        line = f"{msg}\r\n".encode("utf-8")
        self.tx_queue.append(line)

    def record_item(self, item_name: str) -> None:
        """Record a non-serial protocol item (e.g. host measurement point) (R2-M3)."""
        self.visited_items.append(item_name)
        self.trace.append(("MEASURE", item_name))

    def hs_mcu(self, step_name: str) -> bool:
        """Firmware executes HS_MCU primitive: MCU asserts ACK -> waits REQ -> drops ACK -> waits REQ drop."""
        self.visited_items.append(step_name)
        self.trace.append(("PRIM", "HS_MCU"))
        if self.faults.handshake_timeout_step == step_name:
            # Deliberately stall without asserting ACK
            return False

        # Phase 1: MCU asserts MCU_ACK = 1
        self.bus.set_mcu_output(DIO_MCU_ACK, DIO_MCU_ACK)

        # Phase 2: Wait for HOST_REQ = 1
        start = self.clock.monotonic()
        while not (self.bus.read_mcu_idr(DIO_HOST_REQ) & DIO_HOST_REQ):
            if self.clock.monotonic() - start > 10.0:
                self.log_m7(f"[m7_gpio_test] ABORT step={self.step_id} reason=timeout")
                self.bus.set_mcu_safe()
                return False
            yield

        # Phase 3: MCU drops MCU_ACK = 0
        self.bus.set_mcu_output(DIO_MCU_ACK, 0)

        # Phase 4: Wait for HOST_REQ = 0
        start = self.clock.monotonic()
        while self.bus.read_mcu_idr(DIO_HOST_REQ) & DIO_HOST_REQ:
            if self.clock.monotonic() - start > 10.0:
                self.log_m7(f"[m7_gpio_test] ABORT step={self.step_id} reason=timeout")
                self.bus.set_mcu_safe()
                return False
            yield

        yield
        return True

    def hs_host(self, step_name: str) -> bool:
        """Firmware executes HS_HOST primitive: waits REQ -> asserts ACK -> waits REQ drop -> drops ACK."""
        self.visited_items.append(step_name)
        self.trace.append(("PRIM", "HS_HOST"))
        if self.faults.handshake_timeout_step == step_name:
            return False

        # Phase 1: Wait for HOST_REQ = 1
        start = self.clock.monotonic()
        while not (self.bus.read_mcu_idr(DIO_HOST_REQ) & DIO_HOST_REQ):
            if self.clock.monotonic() - start > 10.0:
                self.log_m7(f"[m7_gpio_test] ABORT step={self.step_id} reason=timeout")
                self.bus.set_mcu_safe()
                return False
            yield

        if self.faults.inject_step7_protocol_abort and step_name == "step7_pe2":
            self.log_m7("[m7_gpio_test] ABORT step=7 reason=protocol")
            self.bus.set_mcu_safe()
            return False

        # Phase 2: MCU asserts MCU_ACK = 1
        self.bus.set_mcu_output(DIO_MCU_ACK, DIO_MCU_ACK)

        # Phase 3: Wait for HOST_REQ = 0
        start = self.clock.monotonic()
        while self.bus.read_mcu_idr(DIO_HOST_REQ) & DIO_HOST_REQ:
            if self.clock.monotonic() - start > 10.0:
                self.log_m7(f"[m7_gpio_test] ABORT step={self.step_id} reason=timeout")
                self.bus.set_mcu_safe()
                return False
            yield

        # Phase 4: MCU drops MCU_ACK = 0
        self.bus.set_mcu_output(DIO_MCU_ACK, 0)
        yield
        return True

    # ---- F1a bench steps (T3, T4, T7, T8, T5, T6, T9, T10) --------------------

    def hs_host_act(self, step_name: str, action=None):
        """HS_HOST with an action between HOST_REQ rising and MCU_ACK rising."""
        self.visited_items.append(step_name)
        self.trace.append(("PRIM", "HS_HOST"))
        if self.faults.handshake_timeout_step == step_name:
            return False
        start = self.clock.monotonic()
        while not (self.bus.read_mcu_idr(DIO_HOST_REQ) & DIO_HOST_REQ):
            if self.clock.monotonic() - start > 10.0:
                self.log_m7(f"[m7_gpio_test] ABORT step={self.step_id} reason=timeout")
                self.bus.set_mcu_safe()
                return False
            yield
        if action is not None:
            action()
        self.bus.set_mcu_output(DIO_MCU_ACK, DIO_MCU_ACK)
        start = self.clock.monotonic()
        while self.bus.read_mcu_idr(DIO_HOST_REQ) & DIO_HOST_REQ:
            if self.clock.monotonic() - start > 10.0:
                self.log_m7(f"[m7_gpio_test] ABORT step={self.step_id} reason=timeout")
                self.bus.set_mcu_safe()
                return False
            yield
        self.bus.set_mcu_output(DIO_MCU_ACK, 0)
        yield
        return True

    def _buschk(self, step: int):
        """S5 point 1 in the model, with the E15/E16-style faults per step."""
        fail = self.faults.buschk_fail
        self.bus.mcu_pupdr = 0xAAAA if fail == (step, "up") else 0x5555
        self.log_m7("[m7_gpio_test] BUSCHK up", item_name=f"step{step}_buschk_up")
        yield
        ok = yield from self.hs_mcu(f"step{step}_up")
        if not ok:
            return False
        self.bus.mcu_pupdr = 0x5555 if fail == (step, "down") else 0xAAAA
        self.log_m7("[m7_gpio_test] BUSCHK down", item_name=f"step{step}_buschk_down")
        yield
        ok = yield from self.hs_mcu(f"step{step}_down")
        if not ok:
            return False
        return True

    @staticmethod
    def t3_values(corrupt: bool) -> list[int]:
        vals = [v for b in range(8) for v in (1 << b, 0)]
        vals += [0xFF] + [v for b in range(8) for v in (0xFF & ~(1 << b), 0xFF)]
        vals += [0x54 if corrupt else 0x55, 0xAA, 0x0F, 0xF0, 0x00, 0x05, 0xA5, 0x00]
        vals += [0xFF, 0xF0, 0x00, 0x01, 0x81, 0x00]
        return vals

    def _bench_f1a(self):
        f = self.faults.bench_fault

        # Step 30: T3 (bus = MCU)
        self._enter_step(STEP_T3_MCU_OPS, "MCU")
        ok = yield from self._buschk(STEP_T3_MCU_OPS)
        if not ok:
            return False
        self.bus.mcu_pupdr = 0
        for ready, arm, prim, check, corrupt in (
            ("step30_ready", "HOST_ARM t3_la", "step30_burst", "HOST_CHECK t3_sequence", False),
            ("step30_ctrl_ready", "HOST_ARM t3_ctrl_la", "step30_ctrl_burst", "HOST_CHECK t3_control", True),
        ):
            self.bus.set_mcu_output(DIO_BUS_MASK, 0)
            self.log_m7(f"[m7_gpio_test] {'T3 control ready' if corrupt else 'T3 ready'}", item_name=ready)
            self.record_item(arm)
            yield
            values = self.t3_values(corrupt and f != "t3_ctrl")
            if not corrupt and f == "t3_seq":
                values[5] ^= 0x10

            def burst(values=values):
                self.bus.la_capture = [(DIO_HOST_REQ | v, 2e-6) for v in values]
                self.bus.set_mcu_output(DIO_BUS_MASK, values[-1])

            ok = yield from self.hs_host_act(prim, burst)
            if not ok:
                return False
            self.record_item(check)
        self.bus.clear_mcu_output(DIO_BUS_MASK)
        self.log_m7("[m7_gpio_test] T3: DONE (host verdict)", item_name="step30_done")
        yield

        # Step 31: T4 (bus = AD2)
        self._enter_step(STEP_T4_AD2_PATTERNS, "AD2")
        ok = yield from self._buschk(STEP_T4_AD2_PATTERNS)
        if not ok:
            return False
        self.log_m7("[m7_gpio_test] T4 ready", item_name="step31_ready")
        yield
        ok = yield from self.hs_mcu("step31_ready_hs")
        if not ok:
            return False
        import zlib
        for pass_no in (1, 2):
            got: list[int] = []
            for _ in range(256):
                ok = yield from self.hs_host_act(
                    f"step31_pass{pass_no}", lambda: got.append(self.bus.read_mcu_idr(DIO_BUS_MASK)))
                if not ok:
                    return False
            if pass_no == 2 and f == "t4_ctrl":
                got = list(range(256))
            crc = zlib.crc32(bytes(got)) & 0xFFFFFFFF
            self.log_m7(f"[m7_gpio_test] T4 pass={pass_no} count={len(got)} crc_port=0x{crc:08X} crc_bits=0x{crc:08X}",
                        item_name=f"step31_pass{pass_no}_res")
            self.record_item("HOST_CHECK t4_crc" if pass_no == 1 else "HOST_CHECK t4_control")
            yield
        ok = yield from self.hs_host_act("step31_released")
        if not ok:
            return False
        self.bus.mcu_pupdr = 0
        self.log_m7("[m7_gpio_test] T4: DONE (host verdict)", item_name="step31_done")
        yield

        # Step 32: T7 (bus = AD2)
        self._enter_step(STEP_T7_ANALOG_IDR, "AD2")
        ok = yield from self._buschk(STEP_T7_ANALOG_IDR)
        if not ok:
            return False
        self.bus.mcu_pupdr = 0
        self.bus.mcu_analog = DIO_BUS_MASK
        self.log_m7("[m7_gpio_test] T7 analog ready", item_name="step32_analog_ready")
        yield
        ok = yield from self.hs_mcu("step32_ready_hs")
        if not ok:
            return False
        self.record_item("HOST_DRIVE t7_ff")
        sample: list[int] = []
        ok = yield from self.hs_host_act("step32_analog", lambda: sample.append(self.bus.read_mcu_idr(DIO_BUS_MASK)))
        if not ok:
            return False
        self.log_m7(f"[m7_gpio_test] T7 analog idr=0x{sample[-1]:02X}", item_name="step32_analog_res")
        if f != "t7_ctrl":
            self.bus.mcu_analog = 0
        self.log_m7("[m7_gpio_test] T7 input ready", item_name="step32_input_ready")
        yield
        ok = yield from self.hs_host_act("step32_input", lambda: sample.append(self.bus.read_mcu_idr(DIO_BUS_MASK)))
        if not ok:
            return False
        self.log_m7(f"[m7_gpio_test] T7 input idr=0x{sample[-1]:02X}", item_name="step32_input_res")
        self.record_item("HOST_CHECK t7")
        yield
        ok = yield from self.hs_host_act("step32_released")
        if not ok:
            return False
        self.bus.mcu_analog = 0
        self.log_m7("[m7_gpio_test] T7: DONE (host verdict)", item_name="step32_done")
        yield

        # Step 33: T8 (bus = NONE, PE5 = DIO9)
        self._enter_step(STEP_T8_NO_GLITCH, "NONE")
        cases = {  # case -> (level before, DIO9 levels after REQ)
            "A": (0, [1]),
            "B": (1, [0]),
            "C": (1, [0, 1] if f == "t8_glitch" else [1]),
            "D": (0, [0]),
            "E": (1, [1] if f == "t8_ctrl" else [0, 1]),
        }
        for case, (before, after) in cases.items():
            self.log_m7(f"[m7_gpio_test] T8 case={case} ready", item_name=f"step33_case{case}_ready")
            self.record_item("HOST_ARM t8_la")
            yield

            def apply(before=before, after=after):
                self.bus.la_pre_value = before * DIO_CM7_ACK
                self.bus.la_capture = [(DIO_HOST_REQ | (lvl * DIO_CM7_ACK), 1e-6) for lvl in after]
                self.bus.set_mcu_output(DIO_CM7_ACK, after[-1] * DIO_CM7_ACK)

            ok = yield from self.hs_host_act(f"step33_case{case}", apply)
            if not ok:
                return False
            self.bus.clear_mcu_output(DIO_CM7_ACK)
            self.record_item("HOST_CHECK t8_case")
        self.log_m7("[m7_gpio_test] T8: DONE (host verdict)", item_name="step33_done")
        yield

        # Step 34: T5 (bus = NONE) - voltages of the auditor's reference (plan, T5 row)
        self._enter_step(STEP_T5_PULLS, "NONE")
        pf14_pu = 2.500 if f == "t5_ctrl" else 1.104
        for key, text, v14, v15 in (
            ("vdd", "T5 vdd ready", 0.003, 3.289),
            ("nopull", "T5 nopull ready", 0.150 if f == "t5_ctrl" else 0.003, 3.101),
            ("pulls", "T5 pulls ready", pf14_pu, 2.089),
        ):
            self.bus.pf14_voltage, self.bus.pf15_voltage = v14, v15
            self.log_m7(f"[m7_gpio_test] {text}", item_name=f"step34_{key}_ready")
            self.record_item(f"HOST_MEASURE t5_{key}")
            yield
            ok = yield from self.hs_mcu(f"step34_{key}")
            if not ok:
                return False
        self.log_m7("[m7_gpio_test] T5: DONE (host verdict)", item_name="step34_done")
        yield

        # Step 35: T6 (bus = NONE)
        self._enter_step(STEP_T6_OPEN_DRAIN, "NONE")
        for key, text, v14, v15 in (
            ("odlow", "T6 od low ready", -0.002, 0.004),
            ("odrel", "T6 od released ready", -0.002, 3.105),
            ("ctrl", "T6 control ready", 1.500 if f == "t6_ctrl" else 3.289, 3.105),
        ):
            self.bus.pf14_voltage, self.bus.pf15_voltage = v14, v15
            self.log_m7(f"[m7_gpio_test] {text}", item_name=f"step35_{key}_ready")
            self.record_item(f"HOST_MEASURE t6_{key}")
            yield
            ok = yield from self.hs_mcu(f"step35_{key}")
            if not ok:
                return False
        self.log_m7("[m7_gpio_test] T6: DONE (host verdict)", item_name="step35_done")
        yield

        # Step 36: T9 (bus = NONE)
        self._enter_step(STEP_T9_RISE_TIME, "NONE")
        base_ns = {0: 30.0, 1: 20.0, 2: 16.0, 3: 14.0}
        for sp in range(4):
            self.log_m7(f"[m7_gpio_test] T9 speed={sp} ready", item_name=f"step36_speed{sp}_ready")
            yield
            for e in range(10):
                jitter = 0.2 * ((e % 3) - 1)
                tr = (base_ns[sp] + jitter, base_ns[sp] + 0.5 + jitter)

                def edge(tr=tr):
                    self.bus.scope_edge = tr

                ok = yield from self.hs_host_act(f"step36_speed{sp}", edge)
                if not ok:
                    return False
        self.log_m7("[m7_gpio_test] T9 control ready", item_name="step36_ctrl_ready")
        yield

        def ctrl_edge():
            if f == "t9_ctrl":
                self.bus.scope_edge = (15.0, 15.0)

        ok = yield from self.hs_host_act("step36_ctrl", ctrl_edge)
        if not ok:
            return False
        self.record_item("HOST_CHECK t9")
        self.log_m7("[m7_gpio_test] T9: DONE (host verdict)", item_name="step36_done")
        yield

        # Step 37: T10 (bus = NONE, PA8 = DIO14)
        self._enter_step(STEP_T10_MCO1, "NONE")
        self.bus.set_mcu_output(DIO_MCO1_PA8, 0)
        self.bus.dio14_freq_hz = 17.0e6 if f == "t10_freq" else 16.0e6
        self.log_m7("[m7_gpio_test] T10 mco ready hsi=1", item_name="step37_mco_ready")
        self.record_item("HOST_MEASURE t10_freq")
        yield
        ok = yield from self.hs_mcu("step37_mco")
        if not ok:
            return False
        self.bus.dio14_freq_hz = 16.0e6 if f == "t10_ctrl" else 0.0
        self.log_m7("[m7_gpio_test] T10 control ready", item_name="step37_ctrl_ready")
        self.record_item("HOST_MEASURE t10_control")
        yield
        ok = yield from self.hs_mcu("step37_ctrl")
        if not ok:
            return False
        self.bus.dio14_freq_hz = 0.0
        self.bus.clear_mcu_output(DIO_MCO1_PA8)
        self.log_m7("[m7_gpio_test] T10: DONE (host verdict)", item_name="step37_done")
        yield
        return True

    def _enter_step(self, step_id: int, bus_owner_str: str) -> None:
        self.step_id = step_id
        announcement_name = f"step{step_id}_announcement"
        if self.faults.step_out_of_seq and self.faults.step_out_of_seq[0] == step_id:
            _, bad_id, bad_bus = self.faults.step_out_of_seq
            self.log_m7(f"[m7_gpio_test] STEP id={bad_id} bus={bad_bus} v={GPIO_BENCH_PROTOCOL_VERSION}", item_name=announcement_name)
            return
        self.log_m7(f"[m7_gpio_test] STEP id={step_id} bus={bus_owner_str} v={GPIO_BENCH_PROTOCOL_VERSION}", item_name=announcement_name)

    def run_coroutine(self):
        """Generator that runs the entire firmware flow."""
        # 1. Spurious line before banner (E14)
        if self.faults.spurious_line_before_banner:
            self.tx_queue.append(f"{self.faults.spurious_line_before_banner}\r\n".encode("utf-8"))
            yield

        # 2. S8 Banner
        rev = self.faults.banner_rev or self.expected_rev
        self.log_m7(f"[m7_gpio_test] Rev {rev}")
        yield

        # 3. T15 Race
        if self.faults.cm4_early_rev:
            self.log_m4(f"[m4_gpio_test] Rev {self.faults.cm4_early_rev}")
        if self.faults.cm4_early_pass:
            self.log_m4("[m4_gpio_test] CM4 VERDICT=PASS")
        self._enter_step(STEP_T15_RACE, "NONE")
        if self.faults.lckr_locked_at_boot:
            self.log_m7("[m7_gpio_test] LCKR at boot: GPIOF=0x00010380 LOCKED", item_name="step1_lckr_boot")
        else:
            self.log_m7("[m7_gpio_test] LCKR at boot: GPIOF=0x00000000 CLEAN", item_name="step1_lckr_boot")
        for phase in "abcdefg":
            self.log_m7(f"[m7_gpio_test] INFO T15 ({phase}) model")
        if self.faults.t15_verdict_fail:
            self.log_m7("[m7_gpio_test] T15 VERDICT: FAIL", item_name="step1_verdict")
        else:
            self.log_m7("[m7_gpio_test] T15 VERDICT: PASS", item_name="step1_verdict")
        self.record_item("HOST_CHECK t15_pass")
        yield

        # Steps 11-13: T1, T2, B1 (no AD2)
        for step, key, line_ok, line_bad, verdict_item, measure in (
            (STEP_T1_REJECT, "t1", "T1 VERDICT: PASS", "T1 VERDICT: FAIL", "step11_verdict", "HOST_CHECK t1_pass"),
            (STEP_T2_PARITY, "t2", "T2 VERDICT: PASS", "T2 VERDICT: FAIL", "step12_verdict", "HOST_CHECK t2_pass"),
            (STEP_B1_MEASURE, "b1", "B1 DONE: COMPLETE", "B1 DONE: INCOMPLETE", "step13_done", "HOST_CHECK b1_complete"),
        ):
            self._enter_step(step, "NONE")
            self.log_m7(f"[m7_gpio_test] INFO {key.upper()} model")
            text = line_bad if self.faults.verdict_fail == key else line_ok
            self.log_m7(f"[m7_gpio_test] {text}", item_name=verdict_item)
            self.record_item(measure)
            yield

        # 4. IDLE State
        proto_v = self.faults.banner_version or GPIO_BENCH_PROTOCOL_VERSION
        self._enter_step(STEP_IDLE, "NONE")
        self.log_m7(f"[m7_gpio_test] GPIO_BENCH v{proto_v} idle", item_name="step2_idle")
        self.log_m7("[m7_gpio_test] INFO Waiting for HOST_REQ to start session...")
        self.record_item("HOST_CHECK t0b_a_oe_guards")
        yield

        # Session Start Handshake: HS_HOST
        ok = yield from self.hs_host("session_start")
        if not ok:
            return

        if self.faults.mute_after_session_start:
            # E3: Silence after session start
            while True:
                yield

        # 5. Step 3: T0b(b) Firmware Guards Verification
        self._enter_step(STEP_T0B_GUARDS, "NONE")
        self.log_m7("[m7_gpio_test] T0b_b guards verification: PASS", item_name="step3_guards_pass")
        self.record_item("HOST_CHECK t0b_b_claim_guards")
        yield

        # 6. Step 4: T0b(c) S5 Negative Control
        self._enter_step(STEP_T0B_S5_NEG, "MCU")
        if not self.faults.t0b_c_mcu_no_drive:
            # Drive 0x5A on bus
            self.bus.set_mcu_output(DIO_BUS_MASK, 0x5A)

        if not self.faults.t0b_c_skip_buschk:
            self.log_m7("[m7_gpio_test] BUSCHK up drive=0x5A", item_name="step4_buschk_drive")
        self.record_item("HOST_CHECK t0b_c_contention")
        yield

        # Handshake for contention check (single handshake in T0b(c))
        ok = yield from self.hs_mcu("t0b_c_sync")
        if not ok:
            return

        # Release bus
        self.bus.clear_mcu_output(DIO_BUS_MASK)
        if self.faults.t0b_c_safe_fail:
            self.log_m7("[m7_gpio_test] T0b_c safe state verification FAILED: pins not input", item_name="step4_safe_verified")
        else:
            self.log_m7("[m7_gpio_test] T0b_c safe state verified: all wired pins input", item_name="step4_safe_verified")
        yield

        # 7. Step 5: T0b(d) 10-second Timeout Test
        self._enter_step(STEP_T0B_TIMEOUT, "NONE")
        self.record_item("HOST_WAIT t0b_d_timeout")
        # MCU waits 10.0 seconds for HOST_REQ, which host will not send
        start = self.clock.monotonic()
        while self.clock.monotonic() - start < 10.0:
            yield

        self.log_m7("[m7_gpio_test] timeout expired -> bench_safe() invoked (PASS)", item_name="step5_timeout_pass")
        self.record_item("HOST_CHECK t0b_d_timeout")
        yield

        # 8. Step 6: T0a Loose Lines Check (13 lines)
        self._enter_step(STEP_T0A_LOOSE, "NONE")
        # Pull-up check
        pupdr_val = 0
        for b in range(16):
            if (1 << b) & DIO_LOOSE_13_MASK:
                pupdr_val |= (1 << (b * 2))
        if self.faults.invert_mcu_bit and self.faults.invert_mcu_bit[0] == "loose_up":
            b_idx = self.faults.invert_mcu_bit[1]
            pupdr_val = (pupdr_val & ~(3 << (b_idx * 2))) | (2 << (b_idx * 2))
        self.bus.mcu_pupdr = pupdr_val
        self.log_m7("[m7_gpio_test] BUSCHK up", item_name="step6_buschk_up")
        self.record_item("HOST_READ loose_up")
        yield
        ok = yield from self.hs_mcu("step6_up")
        if not ok:
            return

        # Pull-down check
        pupdr_val = 0
        for b in range(16):
            if (1 << b) & DIO_LOOSE_13_MASK:
                pupdr_val |= (2 << (b * 2))
        if self.faults.invert_mcu_bit and self.faults.invert_mcu_bit[0] == "loose_down":
            b_idx = self.faults.invert_mcu_bit[1]
            pupdr_val = (pupdr_val & ~(3 << (b_idx * 2))) | (1 << (b_idx * 2))
        self.bus.mcu_pupdr = pupdr_val
        self.log_m7("[m7_gpio_test] BUSCHK down", item_name="step6_buschk_down")
        self.record_item("HOST_READ loose_down")
        yield
        ok = yield from self.hs_mcu("step6_down")
        if not ok:
            return

        self.bus.mcu_pupdr = 0
        self.log_m7("[m7_gpio_test] T0a_loose: DONE (host verdict)", item_name="step6_loose_done")
        yield

        # 9. Step 7: T0a Handshake Lines (PE2 HOST_REQ, PE7 MCU_ACK)
        self._enter_step(STEP_T0A_HANDSHAKE, "NONE")
        yield
        # 1. HS_HOST tests PE2
        ok = yield from self.hs_host("step7_pe2")
        if not ok:
            return
        if self.faults.firmware_comparison_fail == "step7_pe2":
            self.log_m7("[m7_gpio_test] Handshake PE2: FAIL", item_name="step7_handshake_pe2")
        else:
            self.log_m7("[m7_gpio_test] Handshake PE2: OK", item_name="step7_handshake_pe2")
        yield

        # 2. HS_MCU tests PE7
        ok = yield from self.hs_mcu("step7_pe7")
        if not ok:
            return
        self.log_m7("[m7_gpio_test] T0a_handshake: DONE (host verdict)", item_name="step7_handshake_done")
        yield

        # 10. Step 8: T0a Walking-1 MCU -> AD2 (11 lines)
        self._enter_step(STEP_T0A_MCU_TO_AD2, "MCU")
        # S5 break-before-make
        if self.faults.step8_buschk_up_fail:
            self.bus.mcu_pupdr = 0xAAAA  # Pulled down -> bus reads 0x00 != 0xFF
        else:
            self.bus.mcu_pupdr = 0x5555  # Bus pull-up
        self.log_m7("[m7_gpio_test] BUSCHK up", item_name="step8_buschk_up")
        yield
        ok = yield from self.hs_mcu("step8_up")
        if not ok:
            return
        if self.faults.step8_buschk_down_fail:
            self.bus.mcu_pupdr = 0x5555  # Pulled up -> bus reads 0xFF != 0x00
        else:
            self.bus.mcu_pupdr = 0xAAAA  # Bus pull-down
        self.log_m7("[m7_gpio_test] BUSCHK down", item_name="step8_buschk_down")
        yield
        ok = yield from self.hs_mcu("step8_down")
        if not ok:
            return
        self.bus.mcu_pupdr = 0

        # Walking-1: [9, 11, 0, 1, 2, 3, 4, 5, 6, 7, 14]
        mcu_walk_bits = [9, 11, 0, 1, 2, 3, 4, 5, 6, 7, 14]
        for b in mcu_walk_bits:
            val = (1 << b)
            if self.faults.invert_mcu_bit and self.faults.invert_mcu_bit[0] == f"mcu_ad2_{b}":
                val = (1 << self.faults.invert_mcu_bit[1])
            self.bus.set_mcu_output(val, val)
            self.record_item("HOST_READ mcu_bit")
            ok = yield from self.hs_mcu(f"step8_mcu_{b}")
            self.bus.clear_mcu_output(val)
            if not ok:
                return

        # MCU -> AD2 Negative Control: ODR set with pins in input with pull-down
        ctrl_val = 0
        if self.faults.invert_mcu_bit and self.faults.invert_mcu_bit[0] == "mcu_ad2_ctrl":
            # Leak output!
            ctrl_val = (1 << self.faults.invert_mcu_bit[1])
            self.bus.set_mcu_output(ctrl_val, ctrl_val)
        self.log_m7("[m7_gpio_test] T0a_mcu control ready", item_name="step8_control_ready")
        self.record_item("HOST_READ mcu_ctrl")
        yield
        ok = yield from self.hs_mcu("step8_ctrl")
        if ctrl_val:
            self.bus.clear_mcu_output(ctrl_val)
        if not ok:
            return

        self.log_m7("[m7_gpio_test] T0a_mcu: DONE (host verdict)", item_name="step8_mcu_done")
        yield

        # 11. Step 9: T0a Walking-1 AD2 -> MCU (10 lines)
        self._enter_step(STEP_T0A_AD2_TO_MCU, "AD2")
        # S5 break-before-make
        if self.faults.step9_buschk_up_fail:
            self.bus.mcu_pupdr = 0xAAAA  # Pulled down -> bus reads 0x00 != 0xFF
        else:
            self.bus.mcu_pupdr = 0x5555  # Bus pull-up
        self.log_m7("[m7_gpio_test] BUSCHK up", item_name="step9_buschk_up")
        yield
        ok = yield from self.hs_mcu("step9_up")
        if not ok:
            return
        if self.faults.step9_buschk_down_fail:
            self.bus.mcu_pupdr = 0x5555  # Pulled up -> bus reads 0xFF != 0x00
        else:
            self.bus.mcu_pupdr = 0xAAAA  # Bus pull-down
        self.log_m7("[m7_gpio_test] BUSCHK down", item_name="step9_buschk_down")
        yield
        ok = yield from self.hs_mcu("step9_down")
        if not ok:
            return
        self.bus.mcu_pupdr = 0

        self.log_m7("[m7_gpio_test] T0a_ad2 ready", item_name="step9_ad2_ready")
        yield
        ok = yield from self.hs_mcu("step9_ready")
        if not ok:
            return

        if self.faults.contention_step9_mcu_drives_bus:
            # Fault injection (R2-M2): MCU drives the bus while AD2 is supposed to drive it
            self.bus.set_mcu_output(DIO_BUS_MASK, 0xFF)

        # Host walks 10 bits: [8, 10, 0, 1, 2, 3, 4, 5, 6, 7]
        ad2_walk_bits = [8, 10, 0, 1, 2, 3, 4, 5, 6, 7]
        step9_failed = False
        for b in ad2_walk_bits:
            self.record_item("HOST_DRIVE ad2_bit")
            ok = yield from self.hs_host(f"step9_ad2_{b}")
            if not ok:
                return
            idr = self.bus.read_mcu_idr(1 << b)
            exp = (1 << b)
            bit_ok = (idr == exp)
            if self.faults.firmware_comparison_fail == f"t0a_ad2_{b}":
                bit_ok = False
            if not bit_ok:
                step9_failed = True
            self.log_m7(f"[m7_gpio_test] T0a_ad2 bit={b} idr=0x{idr:04X} exp=0x{exp:04X} {'OK' if bit_ok else 'FAIL'}", item_name=f"step9_ad2_{b}_res")
            yield

        # AD2 -> MCU Negative Control: MCU bus in analog, AD2 drives 0xFF
        self.bus.mcu_analog = DIO_BUS_MASK
        self.log_m7("[m7_gpio_test] T0a_ad2 analog control ready", item_name="step9_analog_ready")
        self.record_item("HOST_DRIVE ad2_analog")
        yield
        ok = yield from self.hs_host("step9_analog")
        if not ok:
            return
        idr = self.bus.read_mcu_idr(DIO_BUS_MASK)
        ctrl_ok = (idr == 0)
        if self.faults.firmware_comparison_fail == "analog":
            ctrl_ok = False
        if not ctrl_ok:
            step9_failed = True
        self.log_m7(f"[m7_gpio_test] T0a_ad2 analog control result: {'OK' if ctrl_ok else 'FAIL'}", item_name="step9_analog_res")
        yield
        self.bus.mcu_analog = 0

        self.log_m7(f"[m7_gpio_test] T0a_ad2: {'FAIL' if step9_failed else 'PASS'}", item_name="step9_verdict")
        yield

        # 12. Step 10: T0a Scope 1 & 2 DC Levels
        self._enter_step(STEP_T0A_SCOPE, "NONE")
        self.bus.pf14_voltage = 0.0
        self.bus.pf15_voltage = 0.0
        if self.faults.invert_mcu_bit and self.faults.invert_mcu_bit[0] == "scope_low":
            if self.faults.invert_mcu_bit[1] == 15:
                self.bus.pf15_voltage = 1.5
            else:
                self.bus.pf14_voltage = 1.5
        self.log_m7("[m7_gpio_test] Scope LOW ready", item_name="step10_low_ready")
        self.record_item("HOST_MEASURE scope_low")
        yield
        ok = yield from self.hs_mcu("step10_low")
        if not ok:
            return

        self.bus.pf14_voltage = 3.3
        self.bus.pf15_voltage = 3.3
        if self.faults.invert_mcu_bit and self.faults.invert_mcu_bit[0] == "scope_high":
            if self.faults.invert_mcu_bit[1] == 15:
                self.bus.pf15_voltage = 2.0
            else:
                self.bus.pf14_voltage = 2.0
        self.log_m7("[m7_gpio_test] Scope HIGH ready", item_name="step10_high_ready")
        self.record_item("HOST_MEASURE scope_high")
        yield
        ok = yield from self.hs_mcu("step10_high")
        if not ok:
            return

        self.log_m7("[m7_gpio_test] T0a_scope: DONE (host verdict)", item_name="step10_scope_done")
        yield

        # F1a bench steps 30-37
        ok = yield from self._bench_f1a()
        if not ok:
            return

        # 13. Step 20: T16 LCKR
        self._enter_step(STEP_T16_LCKR, "NONE")
        self.log_m7("[m7_gpio_test] INFO T16 model")
        if self.faults.verdict_fail == "t16":
            self.log_m7("[m7_gpio_test] T16 VERDICT: FAIL", item_name="step20_verdict")
        else:
            self.log_m7("[m7_gpio_test] T16 VERDICT: PASS", item_name="step20_verdict")
        self.record_item("HOST_CHECK t16_pass")
        yield

        # 14. Step 99: Done & CM4 Results
        self._enter_step(STEP_DONE, "NONE")
        cm4_rev = self.faults.cm4_rev or self.expected_rev
        self.log_m4(f"[m4_gpio_test] Rev {cm4_rev}")
        self.log_m4("[m4_gpio_test] INFO CM4 selftest started")

        if not self.faults.cm4_verdict_missing:
            verdict_cm4 = "FAIL" if self.faults.cm4_verdict_fail else "PASS"
            self.log_m4(f"[m4_gpio_test] CM4 VERDICT={verdict_cm4}")

        if (self.faults.t15_verdict_fail or self.faults.cm4_verdict_fail or
            step9_failed or self.faults.firmware_comparison_fail):
            self.log_m7("[m7_gpio_test] VERDICT: GPIO SELF-TESTS FAILED", item_name="step99_verdict")
        else:
            self.log_m7("[m7_gpio_test] VERDICT: ALL GPIO SELF-TESTS PASSED", item_name="step99_verdict")
        yield

    def step(self) -> None:
        """Advance the firmware state machine by one cooperative step."""
        if not self._running:
            return
        if getattr(self, "_stepping", False):
            return
        self._stepping = True
        try:
            if self._step_generator is None:
                self._step_generator = self.run_coroutine()
            try:
                next(self._step_generator)
            except StopIteration:
                pass
            self.bus.check_electrical_invariant()
        finally:
            self._stepping = False
