#!/usr/bin/env python
"""
Configure Digilent Analog Discovery 2 Wavegen Channel 1 (W1) to output the real
PHM 2010 CNC Milling Cutting Force waveform `Fy (N)` (or any selected column from
`c_1_001.csv`) biased at a +1.50 V DC offset (swinging safely within [0.35 V, 2.65 V]
for the STM32H755ZI PB1 / Arduino A3 ADC input).

Usage:
    py tools/ad2_wavegen_phm.py [--channel 1] [--sec 1.00] [--offset 1.5] [--amp 1.15]
"""

from __future__ import annotations

import argparse
import ctypes
from ctypes import byref, c_double, c_int
import sys
import time
import numpy as np

DEFAULT_CSV = r"c:\Users\hrodriguez\Downloads\c_1_001.csv\c_1_001.csv"
CHANNEL_NAMES = ["Fx (N)", "Fy (N)", "Fz (N)", "Vx (g)", "Vy (g)", "Vz (g)", "AE (V)"]

# DWF Constants (from Digilent WaveForms SDK dwfconstants.py)
DwfParamOnClose = c_int(4)       # 0 = continue running after close, 1 = stop, 2 = shutdown
AnalogOutNodeCarrier = c_int(0)
funcCustom = c_int(30)


def find_phase_locked_slice(col_50k: np.ndarray, start_idx: int, target_len: int = 4038) -> np.ndarray:
    """
    Extract an integer number of spindle revolutions (~14 revolutions @ 10,400 RPM
    = ~4038 samples @ 50 kHz = 80.76 ms) starting and ending at a rising zero-crossing
    so arbitrary waveform looping on W1 has zero phase discontinuity.
    """
    seg_search = col_50k[start_idx : start_idx + target_len + 600].copy()
    mean_val = float(seg_search[:target_len].mean())
    ac = seg_search - mean_val

    # Find first rising zero-crossing near start
    i0 = 0
    for i in range(1, 350):
        if ac[i - 1] <= 0.0 and ac[i] > 0.0:
            i0 = i
            break

    # Find rising zero-crossing closest to i0 + target_len (14 full spindle revolutions)
    best_i1 = i0 + target_len
    best_dist = 999999
    for i in range(i0 + target_len - 160, i0 + target_len + 160):
        if ac[i - 1] <= 0.0 and ac[i] > 0.0:
            slope_diff = abs((ac[i] - ac[i - 1]) - (ac[i0] - ac[i0 - 1]))
            dist = abs(i - (i0 + target_len)) + int(slope_diff * 20)
            if dist < best_dist:
                best_dist = dist
                best_i1 = i

    sliced = ac[i0:best_i1].copy()
    # Gentle 16-sample crossfade at loop end for C1 continuity
    fade = 16
    if len(sliced) > 2 * fade:
        w = np.linspace(0.0, 1.0, fade, dtype=np.float64)
        sliced[-fade:] = (1.0 - w) * sliced[-fade:] + w * sliced[:fade]
    return sliced


def main() -> int:
    parser = argparse.ArgumentParser(description="Output PHM 2010 Fy(N) on Analog Discovery 2 W1 with 1.5V DC offset.")
    parser.add_argument("--csv", default=DEFAULT_CSV, help="Path to PHM 2010 c_1_001.csv")
    parser.add_argument("--channel", type=int, default=1, help="Column index (0=Fx, 1=Fy, 2=Fz, 3=Vx, 4=Vy, 5=Vz, 6=AE)")
    parser.add_argument("--sec", type=float, default=1.00, help="Start time in seconds within CSV (default: 1.00s)")
    parser.add_argument("--offset", type=float, default=1.50, help="DC Offset in Volts (default: 1.50V)")
    parser.add_argument("--amp", type=float, default=1.15, help="Peak AC Amplitude in Volts around offset (default: 1.15V)")
    args = parser.parse_args()

    print(f"[ad2] Loading {args.csv} (Column {args.channel}: {CHANNEL_NAMES[args.channel]}) ...")
    data = np.loadtxt(args.csv, delimiter=",", dtype=np.float64)
    col = data[:, args.channel]
    start_idx = int(args.sec * 50000.0)

    # Extract ~14 phase-locked spindle revolutions (~80.8 ms @ 50 kHz)
    raw_slice = find_phase_locked_slice(col, start_idx, target_len=4038)
    n_raw = len(raw_slice)
    duration_sec = n_raw / 50000.0
    rep_freq_hz = 1.0 / duration_sec

    # Normalize to [-1.0, +1.0] for DWF funcCustom
    peak_abs = float(np.max(np.abs(raw_slice)))
    norm_slice = raw_slice / max(1e-9, peak_abs)

    # Open Digilent WaveForms driver (dwf.dll)
    dwf = ctypes.cdll.dwf
    ver = ctypes.create_string_buffer(32)
    dwf.FDwfGetVersion(ver)
    print(f"[ad2] DWF Version: {ver.value.decode()}")

    # Keep Wavegen running after script exits (DwfParamOnClose = 0 -> Continue)
    dwf.FDwfParamSet(DwfParamOnClose, c_int(0))

    hdwf = c_int()
    if dwf.FDwfDeviceOpen(c_int(-1), byref(hdwf)) == 0 or hdwf.value == 0:
        err = ctypes.create_string_buffer(512)
        dwf.FDwfGetLastErrorMsg(err)
        print(f"[ad2] ERROR: Could not open Analog Discovery 2: {err.value.decode()}", file=sys.stderr)
        return 1

    # Query W1 (channel 0) buffer size
    c_samples_min = c_int()
    c_samples_max = c_int()
    dwf.FDwfAnalogOutNodeDataInfo(hdwf, c_int(0), AnalogOutNodeCarrier, byref(c_samples_min), byref(c_samples_max))
    buf_len = c_samples_max.value if c_samples_max.value > 0 else 4096

    # Resample the n_raw 50kHz samples onto the exact AD2 hardware buffer size (4096 samples)
    x_old = np.linspace(0.0, 1.0, n_raw, endpoint=False)
    x_new = np.linspace(0.0, 1.0, buf_len, endpoint=False)
    ad2_samples = np.interp(x_new, x_old, norm_slice)

    rgd_samples = (c_double * buf_len)(*ad2_samples.tolist())

    ch = c_int(0)  # W1 = Channel 0
    dwf.FDwfAnalogOutNodeEnableSet(hdwf, ch, AnalogOutNodeCarrier, c_int(1))
    dwf.FDwfAnalogOutNodeFunctionSet(hdwf, ch, AnalogOutNodeCarrier, funcCustom)
    dwf.FDwfAnalogOutNodeDataSet(hdwf, ch, AnalogOutNodeCarrier, rgd_samples, c_int(buf_len))
    dwf.FDwfAnalogOutNodeFrequencySet(hdwf, ch, AnalogOutNodeCarrier, c_double(rep_freq_hz))
    dwf.FDwfAnalogOutNodeAmplitudeSet(hdwf, ch, AnalogOutNodeCarrier, c_double(args.amp))
    dwf.FDwfAnalogOutNodeOffsetSet(hdwf, ch, AnalogOutNodeCarrier, c_double(args.offset))

    # Start W1 output
    dwf.FDwfAnalogOutConfigure(hdwf, ch, c_int(1))
    time.sleep(0.15)

    v_min = args.offset - args.amp * float(np.min(ad2_samples))
    v_max = args.offset + args.amp * float(np.max(ad2_samples))
    v_low = args.offset + args.amp * float(np.min(ad2_samples))
    print(
        f"[ad2] W1 ACTIVE! Generating PHM 2010 {CHANNEL_NAMES[args.channel]} @ t={args.sec:.2f}s:\n"
        f"      - Hardware Buffer : {buf_len} samples ({n_raw} raw @ 50 kHz = {duration_sec*1000:.2f} ms, ~14 spindle revs)\n"
        f"      - Repetition Freq : {rep_freq_hz:.4f} Hz (Spindle 1X = 173.3 Hz / 10,400 RPM)\n"
        f"      - DC Offset       : {args.offset:.3f} V (1500 mV)\n"
        f"      - Peak Amplitude  : +/-{args.amp:.3f} V -> Voltage Range: [{v_low:.3f} V .. {v_max:.3f} V]\n"
        f"      - OnClose Mode    : CONTINUE (W1 stays active continuously)"
    )

    dwf.FDwfDeviceCloseAll()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
