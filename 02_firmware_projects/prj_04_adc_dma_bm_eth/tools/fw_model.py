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
    from tools.gpio_hw_check import PROTOCOL_STEPS, PROTOCOL_ITEMS, BenchItem, StepDef
except ImportError:
    from gpio_hw_check import PROTOCOL_STEPS, PROTOCOL_ITEMS, BenchItem, StepDef


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
        line = f"{msg}\r\n".encode("utf-8")
        self.tx_queue.append(line)

    def log_m4(self, msg: str) -> None:
        """Emit a line from CM4."""
        line = f"{msg}\r\n".encode("utf-8")
        self.tx_queue.append(line)

    def record_item(self, item_name: str) -> None:
        """Record a non-serial protocol item (e.g. host measurement point) (R2-M3)."""
        self.visited_items.append(item_name)

    def hs_mcu(self, step_name: str) -> bool:
        """Firmware executes HS_MCU primitive: MCU asserts ACK -> waits REQ -> drops ACK -> waits REQ drop."""
        self.visited_items.append(step_name)
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

    def _enter_step(self, step_id: int, bus_owner_str: str) -> None:
        self.step_id = step_id
        announcement_name = f"step{step_id}_announcement"
        if self.faults.step_out_of_seq and self.faults.step_out_of_seq[0] == step_id:
            _, bad_id, bad_bus = self.faults.step_out_of_seq
            self.log_m7(f"[m7_gpio_test] STEP id={bad_id} bus={bad_bus} v=1", item_name=announcement_name)
            return
        self.log_m7(f"[m7_gpio_test] STEP id={step_id} bus={bus_owner_str} v=1", item_name=announcement_name)

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
        self.log_m7("[m7_gpio_test] INFO T15 phase (a): CM7 solo (50000 calls)")
        self.log_m7("[m7_gpio_test] INFO T15 phase (b): CM4 solo (50000 calls)")
        self.log_m7("[m7_gpio_test] INFO T15 phase (c): concurrent (50000 calls each)")
        if self.faults.t15_verdict_fail:
            self.log_m7("[m7_gpio_test] T15 VERDICT: FAIL", item_name="step1_verdict")
        else:
            self.log_m7("[m7_gpio_test] T15 VERDICT: PASS", item_name="step1_verdict")
        self.record_item("HOST_CHECK t15_pass")
        yield

        # 4. IDLE State
        proto_v = self.faults.banner_version or 1
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

        # 13. Step 99: Done & CM4 Results
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
