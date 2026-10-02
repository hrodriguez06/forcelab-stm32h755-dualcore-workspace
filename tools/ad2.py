#!/usr/bin/env python3
"""Analog Discovery 2 (AD2) ctypes wrapper over dwf.dll with S1-S12 Safety Guards.

Pure standard library wrapper for Digilent WaveForms SDK (dwf.dll).
Enforces electrical safety rules S1-S10 (01_shared_drivers/00_gpio/README.md section 8):
  - S2: Safe state S2 (all DIO in input, DwfParamOnClose = 1)
  - S3: Single guarded path to FDwfDigitalIOOutputEnableSet via ad2_set_oe()
and S11 of 01_shared_drivers/03_tim/IMPLEMENTATION_PLAN.md:
  - S11: Digital Out (the pattern generator) drives a line even with OE = 0
    (SDK manual section 8: "The Digital-Out is only applied for a signal when
    the Digital-IO Enable and Output are zero"). Single guarded path to a
    Digital Out channel enable and start via ad2_set_dout(), and S2 also
    resets and stops Digital Out, with read-back (ad2_dout_safe()).
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

# ---- Digital Out (S11) -------------------------------------------------------
# DIO1 -> PE9 and DIO3 -> PE11 (TIM1_CH1/CH2 capture inputs of 03_tim) are the
# only lines the pattern generator may ever drive, and only in a "bus = AD2" step.
AD2_DIO_DOUT_PE9          = (1 << 1)
AD2_DIO_DOUT_PE11         = (1 << 3)
AD2_DOUT_ALLOWED          = AD2_DIO_DOUT_PE9 | AD2_DIO_DOUT_PE11
AD2_DOUT_CHANNELS         = 16

# dwf.h (WaveForms SDK inc\dwf.h): DwfDigitalOutIdle, DwfDigitalOutOutput, DwfDigitalOutType.
DWF_DOUT_IDLE_INIT        = 0
DWF_DOUT_IDLE_LOW         = 1
DWF_DOUT_IDLE_HIGH        = 2
DWF_DOUT_IDLE_ZET         = 3
DWF_DOUT_OUTPUT_PUSHPULL  = 0
DWF_DOUT_TYPE_PULSE       = 0

# S11 (c), plan R2-4: Idle = Low is accepted only in the step of the T0b(f)
# control, whose name the caller passes; in every other step only Zet.
AD2_DOUT_IDLE_LOW_STEP    = "T0B_DOUT"


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
            dwf_lib.FDwfDigitalIOConfigure(ctypes.c_int(hdwf))
        except Exception:
            pass
        ad2_dout_safe(dwf_lib, hdwf, strict=False)     # S2 includes Digital Out (S11)
        prohibited = mask & ~allowed
        raise AD2SafetyViolation(
            f"S3 Guard violation: prohibited bits 0x{prohibited:04X} in OE mask 0x{mask:04X} "
            f"(allowed: 0x{allowed:04X}, bus_owner={step_bus_owner})"
        )

    # Exactly one call site for FDwfDigitalIOOutputEnableSet in workspace (A27)
    dwf_lib.FDwfDigitalIOOutputEnableSet(ctypes.c_int(hdwf), ctypes.c_uint32(mask & 0xFFFF))
    dwf_lib.FDwfDigitalIOConfigure(ctypes.c_int(hdwf))

    # Read-back verification
    readback = ctypes.c_uint32()
    dwf_lib.FDwfDigitalIOOutputEnableGet(ctypes.c_int(hdwf), ctypes.byref(readback))
    if (readback.value & 0xFFFF) != (mask & 0xFFFF):
        raise AD2Error(f"OE read-back failed: expected 0x{mask:04X}, got 0x{readback.value:04X}")


def ad2_dout_safe(dwf_lib, hdwf: int, strict: bool = True) -> None:
    """S2, Digital Out part (S11 b): no Digital Out channel can drive a line.

    SDK manual section 10.1: FDwfDigitalOutReset (all parameters to default),
    FDwfDigitalOutConfigure(0) (stop). Section 10.2, for each of the 16
    channels: FDwfDigitalOutEnableSet(0) and FDwfDigitalOutIdleSet(Zet)
    (DwfDigitalOutIdleZet: "Three state", the output while not running), then
    Configure(0) again so the idle values are applied even with AutoConfigure
    off. Read-back with FDwfDigitalOutEnableGet = 0 and FDwfDigitalOutIdleGet
    = Zet on every channel; a mismatch raises AD2Error when `strict`.

    With strict=False (the abort paths, which raise anyway) every failure is
    swallowed so the caller still reaches its own exception.
    """
    h = ctypes.c_int(hdwf)
    try:
        dwf_lib.FDwfDigitalOutReset(h)
        dwf_lib.FDwfDigitalOutConfigure(h, ctypes.c_int(0))
        for ch in range(AD2_DOUT_CHANNELS):
            dwf_lib.FDwfDigitalOutEnableSet(h, ctypes.c_int(ch), ctypes.c_int(0))
            dwf_lib.FDwfDigitalOutIdleSet(h, ctypes.c_int(ch), ctypes.c_int(DWF_DOUT_IDLE_ZET))
        dwf_lib.FDwfDigitalOutConfigure(h, ctypes.c_int(0))
        bad = []
        for ch in range(AD2_DOUT_CHANNELS):
            en = ctypes.c_int(-1)
            idle = ctypes.c_int(-1)
            dwf_lib.FDwfDigitalOutEnableGet(h, ctypes.c_int(ch), ctypes.byref(en))
            dwf_lib.FDwfDigitalOutIdleGet(h, ctypes.c_int(ch), ctypes.byref(idle))
            if en.value != 0 or idle.value != DWF_DOUT_IDLE_ZET:
                bad.append(f"ch{ch}: enable={en.value} idle={idle.value}")
    except Exception:
        if strict:
            raise
        return
    if bad and strict:
        raise AD2Error("S2 Digital Out read-back failed: " + "; ".join(bad))


def ad2_set_dout(dwf_lib, hdwf: int, mask: int, step_bus_owner: int,
                 idle: int = DWF_DOUT_IDLE_ZET, step_name: Optional[str] = None) -> None:
    """Guard S11: the ONLY path that enables or starts a Digital Out channel.

    mask != 0: drive the channels of `mask` with the pulse configured before
    (AD2.dout_pulse). Refused, after going to S2 (Digital-IO and Digital Out),
    with AD2SafetyViolation and BEFORE any enabling call reaches the DLL, when:
      - `mask` has a bit outside {DIO1, DIO3};
      - the step is not "bus = AD2";
      - `idle` is not Zet or Low, or is Low in a step other than
        AD2_DOUT_IDLE_LOW_STEP (`step_name`, the step of the T0b(f) control;
        plan R2-4, finding m1);
      - the Digital-IO OE does not read 0 on the bits of `mask` (S11 d, applied
        to the Digital Out channels only, owner's decision of 2026-10-01; SDK
        section 8: Digital-IO has priority, and Digital Out applies only where
        its Enable and Output are zero). Other Digital-IO outputs, HOST_REQ for
        the handshake, may stay enabled: S3 keeps them off the bus.
    Otherwise, SDK section 10.2: FDwfDigitalOutIdleSet(idle) and
    FDwfDigitalOutOutputSet(PushPull) on each channel of `mask`, read back,
    BEFORE its FDwfDigitalOutEnableSet(1) (S11 c); every other channel
    EnableSet(0); enables read back; then FDwfDigitalOutConfigure(1)
    (section 10.1: start).

    mask == 0: FDwfDigitalOutConfigure(0) (stop). Each enabled channel goes to
    its idle output (Zet unless the T0b(f) control asked for Low); S2 resets
    everything regardless.
    """
    h = ctypes.c_int(hdwf)
    reason = None
    if (mask & ~AD2_DOUT_ALLOWED) != 0:
        reason = f"prohibited bits 0x{mask & ~AD2_DOUT_ALLOWED:04X} (allowed: 0x{AD2_DOUT_ALLOWED:04X})"
    elif mask != 0 and step_bus_owner != GPIO_BENCH_BUS_AD2:
        reason = f"bus_owner={step_bus_owner}, Digital Out needs a 'bus = AD2' step"
    elif mask != 0 and idle not in (DWF_DOUT_IDLE_ZET, DWF_DOUT_IDLE_LOW):
        reason = f"idle={idle} (only Zet, or Low for the T0b(f) control)"
    elif mask != 0 and idle == DWF_DOUT_IDLE_LOW and step_name != AD2_DOUT_IDLE_LOW_STEP:
        reason = f"idle=Low in step {step_name!r}: only in {AD2_DOUT_IDLE_LOW_STEP} (S11 c, plan R2-4)"
    elif mask != 0:
        oe = ctypes.c_uint32(0xFFFFFFFF)
        dwf_lib.FDwfDigitalIOOutputEnableGet(h, ctypes.byref(oe))
        if (oe.value & mask) != 0:
            reason = (f"Digital-IO OE=0x{oe.value & 0xFFFF:04X} on the Digital Out channels 0x{mask:04X}: "
                      f"must be 0 there while Digital Out runs (S11 d)")
    if reason is not None:
        try:
            dwf_lib.FDwfDigitalIOReset(h)
            dwf_lib.FDwfDigitalIOConfigure(h)
        except Exception:
            pass
        ad2_dout_safe(dwf_lib, hdwf, strict=False)
        raise AD2SafetyViolation(f"S11 Guard violation: Digital Out mask 0x{mask:04X}: {reason}")

    if mask == 0:
        dwf_lib.FDwfDigitalOutConfigure(h, ctypes.c_int(0))
        return

    for ch in range(AD2_DOUT_CHANNELS):
        if (mask >> ch) & 1:
            dwf_lib.FDwfDigitalOutIdleSet(h, ctypes.c_int(ch), ctypes.c_int(idle))
            dwf_lib.FDwfDigitalOutOutputSet(h, ctypes.c_int(ch), ctypes.c_int(DWF_DOUT_OUTPUT_PUSHPULL))
            got_idle = ctypes.c_int(-1)
            got_out = ctypes.c_int(-1)
            dwf_lib.FDwfDigitalOutIdleGet(h, ctypes.c_int(ch), ctypes.byref(got_idle))
            dwf_lib.FDwfDigitalOutOutputGet(h, ctypes.c_int(ch), ctypes.byref(got_out))
            if got_idle.value != idle or got_out.value != DWF_DOUT_OUTPUT_PUSHPULL:
                ad2_dout_safe(dwf_lib, hdwf, strict=False)
                raise AD2Error(f"Digital Out ch{ch} read-back: idle={got_idle.value} (want {idle}), "
                               f"output={got_out.value} (want push-pull)")
    for ch in range(AD2_DOUT_CHANNELS):
        dwf_lib.FDwfDigitalOutEnableSet(h, ctypes.c_int(ch), ctypes.c_int((mask >> ch) & 1))
    for ch in range(AD2_DOUT_CHANNELS):
        en = ctypes.c_int(-1)
        dwf_lib.FDwfDigitalOutEnableGet(h, ctypes.c_int(ch), ctypes.byref(en))
        if en.value != ((mask >> ch) & 1):
            ad2_dout_safe(dwf_lib, hdwf, strict=False)
            raise AD2Error(f"Digital Out ch{ch} enable read-back {en.value}, want {(mask >> ch) & 1}")
    dwf_lib.FDwfDigitalOutConfigure(h, ctypes.c_int(1))


def la_edges(samples: list[int], bit: int) -> tuple[list[int], list[int]]:
    """Indices of the rising and of the falling edges of `bit` in LA samples."""
    rises, falls = [], []
    for i in range(1, len(samples)):
        a, b = (samples[i - 1] >> bit) & 1, (samples[i] >> bit) & 1
        if b and not a:
            rises.append(i)
        elif a and not b:
            falls.append(i)
    return rises, falls


def la_period_high(samples: list[int], bit: int, rate_hz: float) -> dict:
    """Frequency and time high of `bit` over the complete periods of a capture.

    Periods run from the first to the last rising edge; the time high is the
    mean of (next falling edge - rising edge) over those periods. The edge
    positions are quantised to one sample, so `resolution_ppm` is one sample
    over the span: the frequency cannot be resolved finer than that.
    Returns rises, falls, periods, freq_hz (0 with < 2 rises), high_s,
    span_samples and resolution_ppm.
    """
    rises, falls = la_edges(samples, bit)
    out = {"rises": len(rises), "falls": len(falls), "periods": 0, "freq_hz": 0.0,
           "high_s": 0.0, "span_samples": 0, "resolution_ppm": float("inf")}
    if len(rises) < 2:
        return out
    span = rises[-1] - rises[0]
    periods = len(rises) - 1
    highs = []
    for r, r_next in zip(rises, rises[1:]):
        f = next((x for x in falls if r < x < r_next), None)
        if f is not None:
            highs.append(f - r)
    out.update(periods=periods, span_samples=span, freq_hz=periods * rate_hz / span,
               high_s=(sum(highs) / len(highs) / rate_hz) if highs else 0.0,
               resolution_ppm=1e6 / span)
    return out


class AD2:
    """Analog Discovery 2 instrument controller with safety guards."""

    def __init__(self, sn: Optional[str] = "210321B5D38D", dwf_lib: Any = None) -> None:
        self.target_sn = sn
        self._hdwf = ctypes.c_int(0)
        self._output_val = 0
        if dwf_lib is not None:
            self._dwf = dwf_lib
        else:
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
        self._output_val = 0
        self._dwf.FDwfDigitalIOReset(self._hdwf)
        ad2_set_oe(self._dwf, self._hdwf.value, 0x0000, GPIO_BENCH_BUS_NONE)
        ad2_dout_safe(self._dwf, self._hdwf.value)      # S11 b: Digital Out reset, stopped, Zet

    def set_safe_state(self) -> None:
        """S2: Put AD2 into safe state (all DIO lines configured as inputs / tri-state).

        Since S11 this also resets and stops Digital Out, with every channel
        disabled and idle Zet, read back (ad2_dout_safe)."""
        if self._hdwf.value != 0:
            self._output_val = 0
            ad2_set_oe(self._dwf, self._hdwf.value, 0x0000, GPIO_BENCH_BUS_NONE)
            ad2_dout_safe(self._dwf, self._hdwf.value)

    def set_oe(self, mask: int, step_bus_owner: int) -> None:
        """S3: Set DIO output enable through guarded function."""
        ad2_set_oe(self._dwf, self._hdwf.value, mask, step_bus_owner)

    def get_oe(self) -> int:
        """Read back current Output Enable mask from hardware."""
        if self._hdwf.value == 0:
            return 0
        readback = ctypes.c_uint32()
        self._dwf.FDwfDigitalIOOutputEnableGet(self._hdwf, ctypes.byref(readback))
        return readback.value & 0xFFFF

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
        """Set output values on digital IO pins enabled as outputs, preserving unmasked outputs (m6)."""
        self._output_val = (self._output_val & ~mask) | (value & mask)
        self._dwf.FDwfDigitalIOOutputSet(self._hdwf, ctypes.c_uint32(self._output_val & 0xFFFF))
        self._dwf.FDwfDigitalIOConfigure(self._hdwf)

    def digital_io_read(self) -> int:
        """Read 16-bit status of digital IO pins (DIO 0..15)."""
        self._dwf.FDwfDigitalIOStatus(self._hdwf)
        val = ctypes.c_uint32()
        self._dwf.FDwfDigitalIOInputStatus(self._hdwf, ctypes.byref(val))
        return val.value & 0xFFFF

    # ---- Digital Out (pattern generator, S11) --------------------------------
    # Configuration only, nothing below enables a channel or starts the
    # instrument: that is ad2_set_dout(), the single guarded path (S11 a).

    def set_dout(self, mask: int, step_bus_owner: int, idle: int = DWF_DOUT_IDLE_ZET,
                 step_name: Optional[str] = None) -> None:
        """S11: start (mask != 0) or stop (mask == 0) Digital Out through the guard."""
        ad2_set_dout(self._dwf, self._hdwf.value, mask, step_bus_owner, idle, step_name)

    def dout_clock_hz(self) -> float:
        """SDK section 10.2, FDwfDigitalOutInternalClockInfo: the clock the dividers divide."""
        hz = ctypes.c_double()
        self._dwf.FDwfDigitalOutInternalClockInfo(self._hdwf, ctypes.byref(hz))
        return hz.value

    def dout_pulse(self, channel: int, divider: int, low: int, high: int, start_high: bool = False) -> float:
        """Configure a pulse on one Digital Out channel; returns its frequency in Hz.

        SDK section 10.2: FDwfDigitalOutTypeSet(Pulse), FDwfDigitalOutDividerInitSet(0),
        FDwfDigitalOutDividerSet(divider), FDwfDigitalOutCounterInitSet(start_high, 0),
        FDwfDigitalOutCounterSet(low, high), each read back; the ranges come from
        FDwfDigitalOutDividerInfo / FDwfDigitalOutCounterInfo. Frequency = internal
        clock / divider / (low + high) (FDwfDigitalOutTypeInfo, "Pulse"). Does not
        enable the channel; only DIO1 and DIO3 can be configured.
        """
        if not (0 <= channel < AD2_DOUT_CHANNELS) or not ((AD2_DOUT_ALLOWED >> channel) & 1):
            raise AD2SafetyViolation(f"S11: Digital Out channel {channel} is not DIO1 or DIO3")
        ch = ctypes.c_int(channel)
        dmin, dmax = ctypes.c_uint(), ctypes.c_uint()
        cmin, cmax = ctypes.c_uint(), ctypes.c_uint()
        self._dwf.FDwfDigitalOutDividerInfo(self._hdwf, ch, ctypes.byref(dmin), ctypes.byref(dmax))
        self._dwf.FDwfDigitalOutCounterInfo(self._hdwf, ch, ctypes.byref(cmin), ctypes.byref(cmax))
        if not (dmin.value <= divider <= dmax.value) or not (1 <= low <= cmax.value and 1 <= high <= cmax.value):
            raise AD2Error(f"Digital Out pulse out of range: divider {divider} ({dmin.value}..{dmax.value}), "
                           f"low {low} high {high} (counter max {cmax.value})")
        self._dwf.FDwfDigitalOutTypeSet(self._hdwf, ch, ctypes.c_int(DWF_DOUT_TYPE_PULSE))
        self._dwf.FDwfDigitalOutDividerInitSet(self._hdwf, ch, ctypes.c_uint(0))
        self._dwf.FDwfDigitalOutDividerSet(self._hdwf, ch, ctypes.c_uint(divider))
        self._dwf.FDwfDigitalOutCounterInitSet(self._hdwf, ch, ctypes.c_int(1 if start_high else 0), ctypes.c_uint(0))
        self._dwf.FDwfDigitalOutCounterSet(self._hdwf, ch, ctypes.c_uint(low), ctypes.c_uint(high))
        typ, div = ctypes.c_int(-1), ctypes.c_uint()
        lo, hi = ctypes.c_uint(), ctypes.c_uint()
        self._dwf.FDwfDigitalOutTypeGet(self._hdwf, ch, ctypes.byref(typ))
        self._dwf.FDwfDigitalOutDividerGet(self._hdwf, ch, ctypes.byref(div))
        self._dwf.FDwfDigitalOutCounterGet(self._hdwf, ch, ctypes.byref(lo), ctypes.byref(hi))
        if (typ.value, div.value, lo.value, hi.value) != (DWF_DOUT_TYPE_PULSE, divider, low, high):
            raise AD2Error(f"Digital Out ch{channel} read-back: type={typ.value} divider={div.value} "
                           f"low={lo.value} high={hi.value}")
        return self.dout_clock_hz() / divider / (low + high)

    def dout_state(self) -> tuple[int, int]:
        """(enable mask, idle-Zet mask) of the 16 Digital Out channels, read back
        (SDK section 10.2, FDwfDigitalOutEnableGet / FDwfDigitalOutIdleGet)."""
        en_mask = zet_mask = 0
        for c in range(AD2_DOUT_CHANNELS):
            en, idle = ctypes.c_int(-1), ctypes.c_int(-1)
            self._dwf.FDwfDigitalOutEnableGet(self._hdwf, ctypes.c_int(c), ctypes.byref(en))
            self._dwf.FDwfDigitalOutIdleGet(self._hdwf, ctypes.c_int(c), ctypes.byref(idle))
            en_mask |= (1 if en.value else 0) << c
            zet_mask |= (1 if idle.value == DWF_DOUT_IDLE_ZET else 0) << c
        return en_mask, zet_mask

    # ---- Digital In facts (SDK section 9.2) ------------------------------------

    def digital_in_clock_hz(self) -> float:
        """FDwfDigitalInInternalClockInfo: the clock la_arm() divides (DIGITAL_IN_CLOCK_HZ assumes 100 MHz)."""
        hz = ctypes.c_double()
        self._dwf.FDwfDigitalInInternalClockInfo(self._hdwf, ctypes.byref(hz))
        return hz.value

    def digital_in_buffer_max(self) -> int:
        """FDwfDigitalInBufferSizeInfo: the largest LA buffer, in samples."""
        n = ctypes.c_int()
        self._dwf.FDwfDigitalInBufferSizeInfo(self._hdwf, ctypes.byref(n))
        return n.value

    # ---- Analog In (Scope) --------------------------------------------------

    def analog_in_setup_dc(self, channel: int = 0, voltage_range: float = 10.0) -> None:
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

    # ---- Measurement instruments (F1a) --------------------------------------
    # Everything below only listens: DigitalIn and AnalogIn. None of it touches
    # FDwfDigitalIOOutputEnableSet or Digital Out (the pattern generator, which
    # drives pins on its own), so S3 (Digital-IO) and S11 (Digital Out,
    # ad2_set_dout) stay the only paths by which the AD2 can drive a line.

    DIGITAL_IN_CLOCK_HZ = 100e6

    def _wait_state(self, status_fn, done: int, timeout_s: float) -> bool:
        sts = ctypes.c_ubyte()
        t0 = time.monotonic()
        while True:
            status_fn(self._hdwf, ctypes.c_int(1), ctypes.byref(sts))
            if sts.value == done:
                return True
            if time.monotonic() - t0 > timeout_s:
                return False
            time.sleep(0.001)

    def la_arm(self, rate_hz: float, samples: int, trigger_rise_mask: int = 0,
               prefill: int = 0) -> tuple[float, int]:
        """Arm the logic analyzer on DIO0..15 (16-bit samples).

        With `trigger_rise_mask`, the capture starts on a rising edge of any of
        those DIOs (for example HOST_REQ, which the host raises itself), with
        `prefill` samples before the trigger. Without it, it starts at once.
        Returns (actual rate in Hz, buffer size in samples).
        """
        div = max(1, int(round(self.DIGITAL_IN_CLOCK_HZ / rate_hz)))
        max_size = ctypes.c_int()
        self._dwf.FDwfDigitalInReset(self._hdwf)
        self._dwf.FDwfDigitalInBufferSizeInfo(self._hdwf, ctypes.byref(max_size))
        n = min(samples, max_size.value) if max_size.value > 0 else samples
        self._dwf.FDwfDigitalInDividerSet(self._hdwf, ctypes.c_uint(div))
        self._dwf.FDwfDigitalInSampleFormatSet(self._hdwf, ctypes.c_int(16))
        self._dwf.FDwfDigitalInBufferSizeSet(self._hdwf, ctypes.c_int(n))
        if trigger_rise_mask:
            self._dwf.FDwfDigitalInTriggerSourceSet(self._hdwf, ctypes.c_ubyte(3))  # trigsrcDetectorDigitalIn
            self._dwf.FDwfDigitalInTriggerSet(self._hdwf, ctypes.c_uint(0), ctypes.c_uint(0),
                                              ctypes.c_uint(trigger_rise_mask & 0xFFFF), ctypes.c_uint(0))
            self._dwf.FDwfDigitalInTriggerPositionSet(self._hdwf, ctypes.c_uint(max(0, n - prefill)))
        else:
            self._dwf.FDwfDigitalInTriggerSourceSet(self._hdwf, ctypes.c_ubyte(0))  # trigsrcNone
        self._dwf.FDwfDigitalInConfigure(self._hdwf, ctypes.c_int(1), ctypes.c_int(1))
        return self.DIGITAL_IN_CLOCK_HZ / div, n

    def la_read(self, samples: int, timeout_s: float = 2.0) -> Optional[list[int]]:
        """Wait for the armed capture to finish and return its 16-bit samples,
        or None when it did not complete (no trigger) within `timeout_s`."""
        if not self._wait_state(self._dwf.FDwfDigitalInStatus, 2, timeout_s):  # DwfStateDone
            self._dwf.FDwfDigitalInConfigure(self._hdwf, ctypes.c_int(0), ctypes.c_int(0))
            return None
        buf = (ctypes.c_uint16 * samples)()
        self._dwf.FDwfDigitalInStatusData(self._hdwf, buf, ctypes.c_int(2 * samples))
        return [int(v) for v in buf]

    def scope_arm_rise(self, rate_hz: float, samples: int, trigger_channel: int,
                       level_v: float, pre_fraction: float = 0.1, range_v: float = 5.0) -> tuple[float, int]:
        """Arm both scope channels, triggered by a rising edge of `trigger_channel`
        through `level_v`, with `pre_fraction` of the buffer before the trigger.
        Returns (actual rate in Hz, buffer size in samples)."""
        size_min = ctypes.c_int()
        size_max = ctypes.c_int()
        self._dwf.FDwfAnalogInReset(self._hdwf)
        self._dwf.FDwfAnalogInBufferSizeInfo(self._hdwf, ctypes.byref(size_min), ctypes.byref(size_max))
        n = min(samples, size_max.value) if size_max.value > 0 else samples
        for ch in (0, 1):
            self._dwf.FDwfAnalogInChannelEnableSet(self._hdwf, ctypes.c_int(ch), ctypes.c_int(1))
            self._dwf.FDwfAnalogInChannelRangeSet(self._hdwf, ctypes.c_int(ch), ctypes.c_double(range_v))
            self._dwf.FDwfAnalogInChannelOffsetSet(self._hdwf, ctypes.c_int(ch), ctypes.c_double(0.0))
        self._dwf.FDwfAnalogInFrequencySet(self._hdwf, ctypes.c_double(rate_hz))
        self._dwf.FDwfAnalogInBufferSizeSet(self._hdwf, ctypes.c_int(n))
        self._dwf.FDwfAnalogInAcquisitionModeSet(self._hdwf, ctypes.c_int(0))            # acqmodeSingle
        self._dwf.FDwfAnalogInTriggerSourceSet(self._hdwf, ctypes.c_ubyte(2))            # trigsrcDetectorAnalogIn
        self._dwf.FDwfAnalogInTriggerAutoTimeoutSet(self._hdwf, ctypes.c_double(0.0))    # wait for a real edge
        self._dwf.FDwfAnalogInTriggerTypeSet(self._hdwf, ctypes.c_int(0))                # trigtypeEdge
        self._dwf.FDwfAnalogInTriggerChannelSet(self._hdwf, ctypes.c_int(trigger_channel))
        self._dwf.FDwfAnalogInTriggerLevelSet(self._hdwf, ctypes.c_double(level_v))
        self._dwf.FDwfAnalogInTriggerConditionSet(self._hdwf, ctypes.c_int(0))           # rise
        actual = ctypes.c_double()
        self._dwf.FDwfAnalogInFrequencyGet(self._hdwf, ctypes.byref(actual))
        rate = actual.value if actual.value > 0 else rate_hz
        # Trigger position is relative to the centre of the buffer.
        self._dwf.FDwfAnalogInTriggerPositionSet(self._hdwf, ctypes.c_double((0.5 - pre_fraction) * n / rate))
        self._dwf.FDwfAnalogInConfigure(self._hdwf, ctypes.c_int(1), ctypes.c_int(1))
        return rate, n

    def scope_read(self, samples: int, timeout_s: float = 1.0) -> Optional[tuple[list[float], list[float]]]:
        """Both channels of the armed capture, or None when nothing triggered."""
        if not self._wait_state(self._dwf.FDwfAnalogInStatus, 2, timeout_s):
            self._dwf.FDwfAnalogInConfigure(self._hdwf, ctypes.c_int(0), ctypes.c_int(0))
            return None
        out = []
        for ch in (0, 1):
            buf = (ctypes.c_double * samples)()
            self._dwf.FDwfAnalogInStatusData(self._hdwf, ctypes.c_int(ch), buf, ctypes.c_int(samples))
            out.append([float(v) for v in buf])
        return out[0], out[1]

    def analog_in_average(self, channel: int, samples: int = 8192, rate_hz: float = 100e3,
                          range_v: float = 10.0) -> float:
        """Mean of one untriggered acquisition of `samples` on `channel`."""
        self._dwf.FDwfAnalogInReset(self._hdwf)
        self._dwf.FDwfAnalogInChannelEnableSet(self._hdwf, ctypes.c_int(channel), ctypes.c_int(1))
        self._dwf.FDwfAnalogInChannelRangeSet(self._hdwf, ctypes.c_int(channel), ctypes.c_double(range_v))
        self._dwf.FDwfAnalogInChannelOffsetSet(self._hdwf, ctypes.c_int(channel), ctypes.c_double(0.0))
        self._dwf.FDwfAnalogInFrequencySet(self._hdwf, ctypes.c_double(rate_hz))
        self._dwf.FDwfAnalogInBufferSizeSet(self._hdwf, ctypes.c_int(samples))
        self._dwf.FDwfAnalogInAcquisitionModeSet(self._hdwf, ctypes.c_int(0))
        self._dwf.FDwfAnalogInTriggerSourceSet(self._hdwf, ctypes.c_ubyte(0))
        self._dwf.FDwfAnalogInConfigure(self._hdwf, ctypes.c_int(1), ctypes.c_int(1))
        if not self._wait_state(self._dwf.FDwfAnalogInStatus, 2, 2.0):
            raise AD2Error("AnalogIn average: acquisition did not complete")
        buf = (ctypes.c_double * samples)()
        self._dwf.FDwfAnalogInStatusData(self._hdwf, ctypes.c_int(channel), buf, ctypes.c_int(samples))
        return sum(buf) / samples


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
