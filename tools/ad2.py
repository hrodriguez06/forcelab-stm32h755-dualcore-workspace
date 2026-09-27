#!/usr/bin/env python3
"""Analog Discovery 2 (AD2) ctypes wrapper over dwf.dll with S1-S10 Safety Guards.

Pure standard library wrapper for Digilent WaveForms SDK (dwf.dll).
Enforces electrical safety rules S1-S10 (IMPLEMENTATION_PLAN.md):
  - S2: Safe state S2 (all DIO in input, DwfParamOnClose = 1)
  - S3: Single guarded path to FDwfDigitalIOOutputEnableSet via ad2_set_oe()
"""

from __future__ import annotations

import ctypes
import sys
import time
from typing import Optional

# ---- Bench Protocol & Safety Constants (S3, S7) ----------------------------
GPIO_BENCH_BUS_NONE = 0
GPIO_BENCH_BUS_MCU  = 1
GPIO_BENCH_BUS_AD2  = 2

AD2_DIO_BUS_MASK          = 0x00FF  # DIO0..DIO7 -> PE8..PE15
AD2_DIO_EXTI3_CM7         = (1 << 8)   # DIO8  -> PE3 (Solo AD2)
AD2_DIO_CM7_ACK           = (1 << 9)   # DIO9  -> PE5 (Solo MCU) - PROHIBITED
AD2_DIO_EXTI4_CM4         = (1 << 10)  # DIO10 -> PE4 (Solo AD2)
AD2_DIO_CM4_ACK           = (1 << 11)  # DIO11 -> PE6 (Solo MCU) - PROHIBITED
AD2_DIO_HOST_REQ          = (1 << 12)  # DIO12 -> PE2 (Solo AD2)
AD2_DIO_MCU_ACK           = (1 << 13)  # DIO13 -> PE7 (Solo MCU) - PROHIBITED
AD2_DIO_MCO1_PA8          = (1 << 14)  # DIO14 -> PA8 (Solo MCU) - PROHIBITED

AD2_ALLOWED_STATIC_OUTPUTS = AD2_DIO_EXTI3_CM7 | AD2_DIO_EXTI4_CM4 | AD2_DIO_HOST_REQ


class AD2Error(RuntimeError):
    """Exception raised for errors in the AD2 DWF library."""
    pass


class AD2SafetyViolation(AD2Error):
    """Exception raised when an electrical safety rule (S1-S10) is violated."""
    pass


def ad2_set_oe(dwf_lib, hdwf: int, mask: int, step_bus_owner: int) -> None:
    """Guard S3: The ONLY path in the workspace to FDwfDigitalIOOutputEnableSet.

    Validates mask before any peripheral access:
      - Allowed static outputs: DIO8 (EXTI3), DIO10 (EXTI4), DIO12 (HOST_REQ).
      - Bus DIO0..DIO7 allowed ONLY if step_bus_owner == GPIO_BENCH_BUS_AD2.
      - DIO9, DIO11, DIO13, DIO14, DIO15 are PROHIBITED for AD2 driving.

    If an invalid bit is present, immediately forces safe state S2 (all inputs)
    and raises AD2SafetyViolation BEFORE peripheral access.
    """
    allowed = AD2_ALLOWED_STATIC_OUTPUTS
    if step_bus_owner == GPIO_BENCH_BUS_AD2:
        allowed |= AD2_DIO_BUS_MASK

    if (mask & ~allowed) != 0:
        # Safety violation: force safe state S2 immediately
        try:
            dwf_lib.FDwfDigitalIOReset(ctypes.c_int(hdwf))
        except Exception:
            pass
        prohibited = mask & ~allowed
        raise AD2SafetyViolation(
            f"S3 Guard violation: prohibited bits 0x{prohibited:04X} in OE mask 0x{mask:04X} "
            f"(allowed: 0x{allowed:04X}, bus_owner={step_bus_owner})"
        )

    # Exactly one call site for FDwfDigitalIOOutputEnableSet in workspace (A27)
    dwf_lib.FDwfDigitalIOOutputEnableSet(ctypes.c_int(hdwf), ctypes.c_uint32(mask & 0xFFFF))

    # Read-back verification
    readback = ctypes.c_uint32()
    dwf_lib.FDwfDigitalIOOutputEnableGet(ctypes.c_int(hdwf), ctypes.byref(readback))
    if (readback.value & 0xFFFF) != (mask & 0xFFFF):
        raise AD2Error(f"OE read-back failed: expected 0x{mask:04X}, got 0x{readback.value:04X}")


class AD2:
    """Analog Discovery 2 instrument controller with safety guards."""

    def __init__(self, sn: Optional[str] = "210321B5D38D") -> None:
        self.target_sn = sn
        self._hdwf = ctypes.c_int(0)
        try:
            self._dwf = ctypes.cdll.dwf
        except Exception as e:
            raise AD2Error(f"Could not load dwf.dll: {e}") from e

    @property
    def handle(self) -> int:
        return self._hdwf.value

    def _get_err_msg(self) -> str:
        buf = ctypes.create_string_buffer(512)
        self._dwf.FDwfGetLastErrorMsg(buf)
        return buf.value.decode("utf-8", errors="replace")

    def open(self) -> None:
        """Enumerate and open device by serial number, configuring safe state S2."""
        if self._hdwf.value != 0:
            return

        # S2: DwfParamOnClose = 1 (stop instrument when handle closed, NEVER 0)
        self._dwf.FDwfParamSet(ctypes.c_int(4), ctypes.c_int(1))

        cdev = ctypes.c_int()
        self._dwf.FDwfEnum(ctypes.c_int(0), ctypes.byref(cdev))
        num_devices = cdev.value
        if num_devices == 0:
            raise AD2Error("No Digilent devices found. Ensure AD2 is connected.")

        device_idx = -1
        sn_buf = ctypes.create_string_buffer(32)
        for i in range(num_devices):
            self._dwf.FDwfEnumSN(ctypes.c_int(i), sn_buf)
            dev_sn = sn_buf.value.decode("utf-8", errors="replace").strip()
            if self.target_sn is None or self.target_sn in dev_sn:
                device_idx = i
                break

        if device_idx < 0:
            raise AD2Error(f"AD2 with SN matching '{self.target_sn}' not found in {num_devices} devices.")

        ret = self._dwf.FDwfDeviceOpen(ctypes.c_int(device_idx), ctypes.byref(self._hdwf))
        if ret == 0 or self._hdwf.value == 0:
            err = self._get_err_msg()
            raise AD2Error(f"Failed to open AD2 (idx={device_idx}): {err}")

        # S2: Safe state S2 on open - Digital IO reset and all inputs
        self._dwf.FDwfDigitalIOReset(self._hdwf)
        ad2_set_oe(self._dwf, self._hdwf.value, 0x0000, GPIO_BENCH_BUS_NONE)

    def set_safe_state(self) -> None:
        """S2: Put AD2 into safe state (all DIO lines configured as inputs / tri-state)."""
        if self._hdwf.value != 0:
            ad2_set_oe(self._dwf, self._hdwf.value, 0x0000, GPIO_BENCH_BUS_NONE)

    def set_oe(self, mask: int, step_bus_owner: int) -> None:
        """S3: Set DIO output enable through guarded function."""
        ad2_set_oe(self._dwf, self._hdwf.value, mask, step_bus_owner)

    def close(self) -> None:
        """Close device safely, ensuring S2 on exit."""
        if self._hdwf.value != 0:
            try:
                self.set_safe_state()
            except Exception:
                pass
            self._dwf.FDwfDeviceClose(self._hdwf)
            self._hdwf = ctypes.c_int(0)

    def __enter__(self) -> AD2:
        self.open()
        return self

    def __exit__(self, exc_type, exc_val, exc_tb) -> None:
        self.close()

    # ---- Digital I/O --------------------------------------------------------

    def digital_io_write(self, value: int, mask: int = 0xFFFF) -> None:
        """Set output values on digital IO pins enabled as outputs."""
        self._dwf.FDwfDigitalIOOutputSet(self._hdwf, ctypes.c_uint32(value & mask & 0xFFFF))

    def digital_io_read(self) -> int:
        """Read 16-bit status of digital IO pins (DIO 0..15)."""
        self._dwf.FDwfDigitalIOStatus(self._hdwf)
        val = ctypes.c_uint32()
        self._dwf.FDwfDigitalIOInputStatus(self._hdwf, ctypes.byref(val))
        return val.value & 0xFFFF

    # ---- Analog In (Scope) --------------------------------------------------

    def analog_in_setup_dc(self, channel: int = 0, voltage_range: float = 5.0) -> None:
        """Configure scope channel for DC measurement."""
        self._dwf.FDwfAnalogInChannelEnableSet(self._hdwf, ctypes.c_int(channel), ctypes.c_int(1))
        self._dwf.FDwfAnalogInChannelRangeSet(self._hdwf, ctypes.c_int(channel), ctypes.c_double(voltage_range))
        self._dwf.FDwfAnalogInChannelOffsetSet(self._hdwf, ctypes.c_int(channel), ctypes.c_double(0.0))
        self._dwf.FDwfAnalogInConfigure(self._hdwf, ctypes.c_int(1), ctypes.c_int(1))

    def analog_in_read_dc(self, channel: int = 0) -> float:
        """Read instantaneous DC voltage on scope channel."""
        sts = ctypes.c_byte()
        self._dwf.FDwfAnalogInStatus(self._hdwf, ctypes.c_int(1), ctypes.byref(sts))
        sample = ctypes.c_double()
        self._dwf.FDwfAnalogInStatusSample(self._hdwf, ctypes.c_int(channel), ctypes.byref(sample))
        return sample.value


if __name__ == "__main__":
    print("Testing AD2 connection and S3 safety guards...")
    try:
        with AD2() as ad2:
            print(f"AD2 opened in S2 safe state. Handle={ad2.handle}")
            dio = ad2.digital_io_read()
            print(f"DIO State: 0x{dio:04X}")

            # Test S3 guard: attempt to set prohibited DIO9 (Solo MCU)
            try:
                ad2.set_oe(AD2_DIO_CM7_ACK, GPIO_BENCH_BUS_NONE)
                print("FAIL: S3 guard did NOT reject prohibited bit!")
                sys.exit(1)
            except AD2SafetyViolation as e:
                print(f"PASS: S3 guard rejected prohibited bit as expected: {e}")

            # Test S3 guard: attempt to set bus when bus_owner == NONE
            try:
                ad2.set_oe(0x00FF, GPIO_BENCH_BUS_NONE)
                print("FAIL: S3 guard did NOT reject bus when bus_owner=NONE!")
                sys.exit(1)
            except AD2SafetyViolation as e:
                print(f"PASS: S3 guard rejected bus output when bus_owner=NONE: {e}")

            # Test S3 guard: valid static output (HOST_REQ DIO12)
            ad2.set_oe(AD2_DIO_HOST_REQ, GPIO_BENCH_BUS_NONE)
            print("PASS: S3 guard allowed valid static output (DIO12).")
            ad2.set_safe_state()

        print("All AD2 safety tests PASSED.")
    except Exception as e:
        print(f"AD2 test error: {e}", file=sys.stderr)
        sys.exit(1)
