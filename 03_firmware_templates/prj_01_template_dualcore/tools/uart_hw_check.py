#!/usr/bin/env python3
# Copyright (c) 2026 STMicroelectronics & Antigravity
# All rights reserved.
#
# Hardware check host for 04_uart selftest (preset selftest-uart).
# Implements the host side of the protocol v1 across UT0 - UT9.
# Supports real hardware session with AD2 and mock session for CI.

from __future__ import annotations

import argparse
import ctypes
import hashlib
import os
import pathlib
import re
import shlex
import subprocess
import sys
import threading
import time
from dataclasses import dataclass
from typing import Any, Callable, Optional

# Root directory setup
TOOL_DIR = pathlib.Path(__file__).resolve().parent
PROJECT = TOOL_DIR.parent
WORKSPACE = PROJECT.parent.parent

sys.path.insert(0, str(WORKSPACE / "tools"))
from ad2 import (AD2, AD2Error, AD2SafetyViolation, ad2_dout_safe,     # noqa: E402
                 AD2_DIO_HOST_REQ, AD2_DIO_MCU_ACK, AD2_DIO_MCO1_PA8,
                 AD2_DIO_UART_RX_INJECT, ad2_set_oe, ad2_set_dout)
from monitor import SerialPort, autodetect             # noqa: E402

UART_BENCH_VERSION = 1
BOARD_STLINK_SN = "0016000C4142501220353451"

# ---- Bench Wiring -----------------------------------------------------------
DIO_REQ = AD2_DIO_HOST_REQ       # DIO12 -> PE2 (HOST_REQ, AD2 output)
DIO_ACK = AD2_DIO_MCU_ACK        # DIO13 <- PE7 (MCU_ACK, MCU output)
DIO_MCO = AD2_DIO_MCO1_PA8       # DIO14 <- PA8 (MCO1 clock reference)
DIO_RX  = AD2_DIO_UART_RX_INJECT # DIO15 -> PD6 (USART2_RX / oracle)


@dataclass(frozen=True)
class StepDef:
    name: str
    dio15: str  # "NONE", "OE", "DOUT"


# Authoritative mirror of Common/Src/uart_selftest_common.c k_ust_steps
STEPS: list[StepDef] = [
    StepDef("UT0",      "OE"),
    StepDef("UT0_CTRL", "NONE"),
    StepDef("UT1",      "NONE"),
    StepDef("UT2",      "NONE"),
    StepDef("UT3",      "NONE"),
    StepDef("UT4",      "NONE"),
    StepDef("UT4_CM4",  "NONE"),
    StepDef("UT5",      "DOUT"),
    StepDef("UT6",      "NONE"),
    StepDef("UT7",      "NONE"),
    StepDef("UT8",      "NONE"),
    StepDef("UT9",      "NONE"),
    StepDef("DONE",     "NONE"),
]

STEP_BY_NAME: dict[str, StepDef] = {s.name: s for s in STEPS}

# ---- CubeProgrammer & Build ID Helpers --------------------------------------

def cube_programmer() -> str:
    cli = shutil_which("STM32_Programmer_CLI.exe") or shutil_which("STM32_Programmer_CLI")
    if cli:
        return cli
    candidates = list(pathlib.Path("C:/ST").glob("**/STM32_Programmer_CLI.exe"))
    if candidates:
        return str(candidates[0])
    return "STM32_Programmer_CLI"


def shutil_which(cmd: str) -> Optional[str]:
    import shutil
    return shutil.which(cmd)


def git_rev() -> str:
    try:
        r = subprocess.run(["git", "rev-parse", "--short", "HEAD"],
                           cwd=str(WORKSPACE), capture_output=True, text=True, check=True)
        return r.stdout.strip()
    except Exception:
        return "unknown"


def check_clean_tree(repo_root: pathlib.Path, out_file: Optional[pathlib.Path]) -> tuple[bool, str]:
    cp = subprocess.run(
        ["git", "status", "--porcelain=v1", "-u", "--ignored"],
        cwd=str(repo_root), capture_output=True, text=True
    )
    if cp.returncode != 0:
        return False, "git status command failed"
    status_out = cp.stdout
    out_res = out_file.resolve() if out_file else None
    dirty = []
    for line in status_out.splitlines():
        if not line:
            continue
        code = line[:2]
        path_str = line[3:].strip().strip('"')
        if " -> " in path_str:
            path_str = path_str.split(" -> ")[1]
        p = (repo_root / path_str).resolve()
        if out_res and p == out_res:
            continue
        if "build" in p.parts or ".gemini" in path_str:
            continue
        # Permit captures in docs directories
        if (p.parent == (repo_root / "01_shared_drivers" / "04_uart" / "docs").resolve()
                and p.name.startswith("uart_checks_")):
            continue
        if (p.parent == (repo_root / "03_firmware_templates" / "prj_01_template_dualcore" / "docs").resolve()
                and (p.name.startswith("uart_dev_") or p.name.startswith("uart_mock_"))):
            continue
        # Only non-ignored modifications or untracked files are dirty
        if code != "!!":
            dirty.append(line)
    if dirty:
        return False, f"working tree is dirty: {', '.join(dirty[:3])}"
    return True, "Tree is clean outside build/"


def host_build_id(preset: str, project: pathlib.Path = PROJECT) -> str:
    try:
        head = subprocess.run(["git", "rev-parse", "HEAD"], cwd=str(project),
                              capture_output=True, check=True).stdout.strip()
        diff = subprocess.run(["git", "diff", "--no-color", "HEAD"], cwd=str(project),
                              capture_output=True, check=True).stdout
        cache = (project / "build" / preset / "CMakeCache.txt").read_bytes()
    except (OSError, subprocess.CalledProcessError):
        return "unknown"
    if not re.fullmatch(rb"[0-9a-f]{40}", head):
        return "unknown"
    flags = None
    for line in cache.splitlines():
        m = re.match(rb"^CMAKE_C_FLAGS:[A-Z]+=(.*)$", line)
        if m:
            flags = m.group(1)
            break
    if flags is None:
        return "unknown"
    payload = b"HEAD=" + head + diff + b"CMAKE_C_FLAGS=" + flags + b" PRESET=" + preset.encode("utf-8")
    return hashlib.sha256(payload).hexdigest()[:16]


# ---- Formula Oracles --------------------------------------------------------

def compute_presc_brr(inst: str, f_ker: int, baud: int, over8: bool) -> tuple[int, int]:
    """Independent host formula for STM32H7 UART PRESC and BRR registers."""
    presc_divs = [1, 2, 4, 6, 8, 10, 12, 16, 32, 64, 128, 256]
    if inst == "LPUART1":
        for p, div in enumerate(presc_divs):
            ker_p = f_ker // div
            if ker_p < 3 * baud or ker_p > 4096 * baud:
                continue
            ratio_int = ker_p // baud
            ratio_rem = ker_p % baud
            if 3 <= ratio_int <= 4 and ratio_rem != 0:
                continue
            num = (256 * ker_p) + (baud // 2)
            brr = num // baud
            if brr < 0x300 or brr > 0xFFFFF:
                continue
            actual = (256 * ker_p) // brr
            diff = abs(actual - baud)
            if diff * 100 <= baud * 2:
                return p, brr
        return 0, 0

    for p, div in enumerate(presc_divs):
        ker_p = f_ker // div
        if ker_p == 0:
            break
        if over8:
            usartdiv = (2 * ker_p + baud // 2) // baud
            if usartdiv < 16 or usartdiv > 0xFFFF:
                continue
            brr = (usartdiv & 0xFFF0) | ((usartdiv & 0x000F) >> 1)
            actual_div = (brr & 0xFFF0) | ((brr & 0x0007) << 1)
            if actual_div == 0:
                continue
            actual = (2 * ker_p) // actual_div
        else:
            usartdiv = (ker_p + baud // 2) // baud
            if usartdiv < 16 or usartdiv > 0xFFFF:
                continue
            brr = usartdiv
            actual = ker_p // brr

        diff = abs(actual - baud)
        if diff * 100 <= baud * 2:
            return p, brr

    return 0, 0


def compute_actual_baud(f_ker: float, presc: int, brr: int, over8: bool) -> float:
    presc_divs = [1, 2, 4, 6, 8, 10, 12, 16, 32, 64, 128, 256]
    div = presc_divs[presc] if presc < len(presc_divs) else 1
    ker_p = f_ker / div
    if over8:
        actual_div = (brr & 0xFFF0) | ((brr & 0x0007) << 1)
        return (2.0 * ker_p) / actual_div if actual_div > 0 else 0.0
    else:
        return ker_p / brr if brr > 0 else 0.0


# ---- UART Logic Analyzer Frame Decoder (E-M1, E-B3) -------------------------

def decode_uart_frames(samples: list[int], bit_period_samples: int,
                       data_bits: int = 8, parity: str = "none", stop_bits: int = 1,
                       f_mco: float = 4000000.0, baud: int = 115200, rate: float = 100e6) -> list[dict]:
    """Decodes UART frames and measures bit duration in MCO1 periods between real transitions."""
    frames: list[dict] = []
    n = len(samples)
    if n == 0 or bit_period_samples <= 0:
        return frames

    # 1. Debounce and collect MCO1 rising edges across the acquisition (E-M1)
    mco_rises: list[int] = []
    min_mco_spacing = max(2, int(0.6 * (rate / f_mco)))
    for s in range(1, n):
        if ((samples[s - 1] & DIO_MCO) == 0) and ((samples[s] & DIO_MCO) != 0):
            if not mco_rises or (s - mco_rises[-1]) >= min_mco_spacing:
                mco_rises.append(s)

    mco_period_samples = 0.0
    if len(mco_rises) >= 2:
        mco_period_samples = (mco_rises[-1] - mco_rises[0]) / (len(mco_rises) - 1)
    elif f_mco > 0:
        mco_period_samples = rate / f_mco

    # 2. Decode frames from DIO15 (DIO_RX)
    i = 0
    while i < n:
        # Find start bit falling edge on DIO15 (preceded by line high)
        while i < n and (samples[i] & DIO_RX) == 0:
            i += 1
        while i < n and (samples[i] & DIO_RX) != 0:
            i += 1
        if i >= n:
            break

        start_edge = i  # Real transition: start bit falling edge

        # Check start bit center is low
        sample_pt = i + int(round(0.5 * bit_period_samples))
        if sample_pt >= n:
            break
        if (samples[sample_pt] & DIO_RX) != 0:
            i += 1
            continue

        # Extract data bits
        val = 0
        ones_count = 0
        for b in range(data_bits):
            pt = i + int(round((1.5 + b) * bit_period_samples))
            if pt >= n:
                break
            bit = 1 if (samples[pt] & DIO_RX) else 0
            val |= (bit << b)
            ones_count += bit

        # Parity check
        parity_ok = True
        cur_bit_pos = 1.5 + data_bits
        if parity in ("even", "odd"):
            pt = i + int(round(cur_bit_pos * bit_period_samples))
            if pt < n:
                par_bit = 1 if (samples[pt] & DIO_RX) else 0
                tot_ones = ones_count + par_bit
                parity_ok = ((tot_ones % 2 == 0) if parity == "even" else (tot_ones % 2 == 1))
            else:
                parity_ok = False
            cur_bit_pos += 1.0

        # Stop bit check
        stop_ok = True
        pt = i + int(round(cur_bit_pos * bit_period_samples))
        if pt < n:
            stop_ok = ((samples[pt] & DIO_RX) != 0)
        else:
            stop_ok = False

        # Measure bit duration between real transitions on DIO15 (E-M1)
        total_bits = 1 + data_bits + (1 if parity in ("even", "odd") else 0) + stop_bits
        frame_end = start_edge + int(round(total_bits * bit_period_samples))
        if frame_end > n:
            break

        rx_transitions = [s for s in range(start_edge + 1, min(frame_end + 1, n))
                          if ((samples[s] ^ samples[s - 1]) & DIO_RX)]

        mco_per_bit = 0.0
        if rx_transitions and mco_period_samples > 0:
            last_edge = rx_transitions[-1]
            span_samples = last_edge - start_edge
            bits_spanned = round(span_samples / bit_period_samples)
            if bits_spanned > 0:
                bit_duration_samples = span_samples / float(bits_spanned)
                mco_per_bit = bit_duration_samples / mco_period_samples

        if mco_per_bit == 0.0 and mco_period_samples > 0:
            mco_per_bit = bit_period_samples / mco_period_samples

        frames.append({
            "byte": val,
            "parity_ok": parity_ok,
            "stop_ok": stop_ok,
            "start_edge": start_edge,
            "frame_end": frame_end,
            "total_bits": total_bits,
            "mco_per_bit": mco_per_bit
        })

        i = frame_end

    return frames


# ---- Pure Evaluation Functions (The Oracle Rule, E-M2) -----------------------

def eval_ut0(resting_high: bool, pd6_low_when_oe: bool, restored_high: bool, fw_pass: bool) -> bool:
    return resting_high and pd6_low_when_oe and restored_high and fw_pass


def eval_ut0_ctrl(line_low: bool, fw_pass: bool) -> bool:
    return line_low and fw_pass


def eval_ut1(snap_match: bool, fail_count: int, fw_pass: bool) -> bool:
    return snap_match and (fail_count == 0) and fw_pass


def eval_ut2(rows: list[dict], rejects: list[dict], fw_pass: bool) -> bool:
    if not fw_pass or len(rows) != 22 or len(rejects) != 4:
        return False
    for r in rows:
        exp_p, exp_b = compute_presc_brr(r["inst"], r["f_ker"], r["baud"], r["over8"])
        if (r["presc"] != exp_p) or (r["brr"] != exp_b):
            return False
    for rej in rejects:
        if rej["status"] != "REJECT":
            return False
        p, b = compute_presc_brr(rej["inst"], rej["f_ker"], rej["baud"], rej["over8"])
        if p != 0 or b != 0:
            return False
    return True


def eval_ut3(frames: list[dict], expected_bytes: list[int], exp_parity: str,
             f_mco: float, baud: float, data_bits: int = 8, tolerance: float = 0.005) -> bool:
    if not frames:
        return False
    th_mco = f_mco / baud
    mask = (1 << data_bits) - 1
    for i, f in enumerate(frames):
        if i >= len(expected_bytes):
            break
        if (f["byte"] & mask) != (expected_bytes[i] & mask):
            return False
        if not f["parity_ok"] or not f["stop_ok"]:
            return False
        if abs(f["mco_per_bit"] - th_mco) / th_mco > tolerance:
            return False
    return True


def eval_ut4(sent: int, received: int, err: int, fw_pass: bool) -> bool:
    return (sent == 72) and (received == 72) and (err == 0) and fw_pass


def eval_ut4_cm4(sent: int, received: int, status: int, fw_pass: bool) -> bool:
    return (sent == 32) and (received == 32) and (status == 1) and fw_pass


def eval_ut5(measured_errors: dict[str, int], fw_pass: bool) -> bool:
    if not fw_pass:
        return False
    if (measured_errors.get("PE", 0) & 8) == 0:
        return False
    if (measured_errors.get("FE", 0) & 4) == 0:
        return False
    if (measured_errors.get("BREAK", 0) & 4) == 0:
        return False
    if (measured_errors.get("NE", 0) & 2) == 0:
        return False
    if (measured_errors.get("ORE-FIFO", 0) & 1) == 0:
        return False
    if (measured_errors.get("ORE-NOFIFO", 0) & 1) == 0:
        return False
    if measured_errors.get("CLEAN", 1) != 0:
        return False
    return True


def eval_ut6(written: int, read: int, err_fc: int, err_nofc: int, fw_pass: bool) -> bool:
    return (written == 24) and (read == 24) and (err_fc == 0) and ((err_nofc & 1) != 0) and fw_pass


def eval_ut7(lock_held_st: int, rx_byte: int, fw_pass: bool) -> bool:
    return (lock_held_st == 2) and (rx_byte == 0xA5) and fw_pass


def eval_ut8(bit20: int, active_status: int, dma_bytes: int, fw_pass: bool) -> bool:
    return (bit20 == 1) and (active_status == 2) and (dma_bytes == 20) and fw_pass


def eval_ut9(ret_held: int, ret_free: int, fw_pass: bool, expect_control: bool = False) -> bool:
    if expect_control:
        return (ret_held == 0) and (not fw_pass)
    return (ret_held == 2) and (ret_free == 0) and fw_pass


# ---- Mock AD2 DWF Implementation (E-M5, C-M7) -------------------------------

class _MockDwf:
    def __init__(self) -> None:
        self.oe_calls: list[int] = []
        self._oe = 0
        self._output_val = 0
        self.la_armed = False
        self.mock_la_samples: Optional[list[int]] = None
        self.current_step: Optional[Any] = None
        self._last_error = b"Mock DWF error"
        self._dout_enables: dict[int, int] = {}
        self._dout_bits: dict[int, list[int]] = {}
        self._dout_types: dict[int, int] = {}
        self._dout_dividers: dict[int, int] = {}

    def set_step(self, step: Optional[Any]) -> None:
        self.current_step = step

    def FDwfGetLastErrorMsg(self, buf):
        ctypes.memmove(buf, self._last_error, min(len(self._last_error) + 1, 512))
        return 1

    def FDwfParamSet(self, param, val):
        return 1

    def FDwfEnum(self, dev_filter, cdev):
        cdev._obj.value = 1
        return 1

    def FDwfEnumSN(self, idx, buf):
        sn = b"210321B5D38D"
        ctypes.memmove(buf, sn, len(sn) + 1)
        return 1

    def FDwfDeviceOpen(self, idx, hdwf):
        hdwf._obj.value = 1
        return 1

    def FDwfDeviceConfigOpen(self, dev_idx, cfg_idx, hdwf):
        hdwf._obj.value = 1
        return 1

    def FDwfDeviceClose(self, h):
        return 1

    def FDwfDigitalIOReset(self, h):
        self._oe = 0
        self._output_val = 0
        return 1

    def FDwfDigitalIOOutputEnableSet(self, h, mask):
        val = int(mask.value) if hasattr(mask, "value") else int(mask)
        self.oe_calls.append(val)
        self._oe = val
        return 1

    def FDwfDigitalIOOutputEnableGet(self, h, ref):
        ref._obj.value = self._oe
        return 1

    def FDwfDigitalIOOutputSet(self, h, val):
        new_val = int(val.value) if hasattr(val, "value") else int(val)
        if (new_val & DIO_REQ) and not (self._output_val & DIO_REQ):
            self.mcu_ack = 0
        elif not (new_val & DIO_REQ) and (self._output_val & DIO_REQ):
            self.mcu_ack = 1
        self._output_val = new_val
        return 1

    def FDwfDigitalIOConfigure(self, h):
        return 1

    def FDwfDigitalIOStatus(self, h):
        return 1

    def FDwfDigitalIOInputStatus(self, h, ref):
        val = 0
        if getattr(self, "mcu_ack", 1):
            val |= DIO_ACK

        step_name = getattr(self.current_step, "name", str(self.current_step))
        if step_name == "UT0":
            if (self._oe & DIO_RX) != 0 and ((self._output_val & DIO_RX) == 0):
                val &= ~DIO_RX
            else:
                val |= DIO_RX
        elif step_name == "UT0_CTRL":
            val &= ~DIO_RX
        else:
            val |= DIO_RX

        ref._obj.value = val
        return 1

    def FDwfDigitalOutReset(self, h):
        self._dout_enables.clear()
        return 1

    def FDwfDigitalOutConfigure(self, h, start):
        return 1

    def FDwfDigitalOutInternalClockInfo(self, h, ref):
        ref._obj.value = 100000000.0
        return 1

    def FDwfDigitalOutDataInfo(self, h, ch, ref):
        ref._obj.value = 16384
        return 1

    def FDwfDigitalOutTypeSet(self, h, ch, typ):
        ch_idx = int(ch.value) if hasattr(ch, "value") else int(ch)
        self._dout_types[ch_idx] = int(typ.value) if hasattr(typ, "value") else int(typ)
        return 1

    def FDwfDigitalOutTypeGet(self, h, ch, ref):
        ch_idx = int(ch.value) if hasattr(ch, "value") else int(ch)
        ref._obj.value = self._dout_types.get(ch_idx, 1)
        return 1

    def FDwfDigitalOutDividerInitSet(self, h, ch, init):
        return 1

    def FDwfDigitalOutDividerSet(self, h, ch, div):
        ch_idx = int(ch.value) if hasattr(ch, "value") else int(ch)
        self._dout_dividers[ch_idx] = int(div.value) if hasattr(div, "value") else int(div)
        return 1

    def FDwfDigitalOutDividerGet(self, h, ch, ref):
        ch_idx = int(ch.value) if hasattr(ch, "value") else int(ch)
        ref._obj.value = self._dout_dividers.get(ch_idx, 1)
        return 1

    def FDwfDigitalOutDataSet(self, h, ch, buf, count):
        return 1

    def FDwfDigitalOutEnableSet(self, h, ch, en):
        ch_idx = int(ch.value) if hasattr(ch, "value") else int(ch)
        en_val = int(en.value) if hasattr(en, "value") else int(en)
        self._dout_enables[ch_idx] = en_val
        return 1

    def FDwfDigitalOutEnableGet(self, h, ch, ref):
        ch_idx = int(ch.value) if hasattr(ch, "value") else int(ch)
        ref._obj.value = self._dout_enables.get(ch_idx, 0)
        return 1

    def FDwfDigitalOutIdleSet(self, h, ch, idle):
        return 1

    def FDwfDigitalOutIdleGet(self, h, ch, ref):
        ref._obj.value = 3  # Zet
        return 1

    def FDwfDigitalOutOutputSet(self, h, ch, out_type):
        return 1

    def FDwfDigitalOutOutputGet(self, h, ch, ref):
        ref._obj.value = 0
        return 1

    def FDwfDigitalOutRepeatSet(self, h, rep):
        return 1

    def FDwfDigitalOutRunSet(self, h, sec):
        return 1

    def FDwfDigitalInReset(self, h):
        self.la_armed = False
        return 1

    def FDwfDigitalInBufferSizeInfo(self, h, max_size):
        max_size._obj.value = 16384
        return 1

    def FDwfDigitalInDividerSet(self, h, div):
        return 1

    def FDwfDigitalInSampleFormatSet(self, h, fmt):
        return 1

    def FDwfDigitalInBufferSizeSet(self, h, n):
        return 1

    def FDwfDigitalInTriggerSourceSet(self, h, src):
        return 1

    def FDwfDigitalInTriggerSet(self, h, level_low, level_high, edge_rise, edge_fall):
        return 1

    def FDwfDigitalInTriggerPositionSet(self, h, pos):
        return 1

    def FDwfDigitalInConfigure(self, h, reconfig, start):
        start_val = bool(start.value) if hasattr(start, "value") else bool(start)
        self.la_armed = start_val
        return 1

    def FDwfDigitalInStatus(self, h, read_data, ref_status):
        ref_status._obj.value = 2  # DwfStateDone
        return 1

    def FDwfDigitalInStatusData(self, h, buf, count):
        samples = (count.value if hasattr(count, "value") else count) // 2
        if self.mock_la_samples is not None:
            data = self.mock_la_samples[:samples]
        else:
            data = [0xFFFF] * samples
        for i, v in enumerate(data):
            buf[i] = v
        return 1


# ---- Mock Console & Script Generator (E-M5) ---------------------------------

def generate_mock_lines() -> list[str]:
    """Generates simulated firmware output based on authoritative STEPS list."""
    lines = [
        "=== 04_uart selftest starting on CM7 ===",
        "[uart] CM7 Rev mock_rev",
        "[uart] CM7 Build id mock_id",
        "[uart] CM4 Rev mock_rev",
        "[uart] CM4 Build id mock_id",
        "[uart] MCO1: PA8 enabled, freq=4000000 Hz, f_ker=100000000 Hz, ratio=25:1",
    ]
    for step in STEPS:
        if step.name == "DONE":
            lines.append("[uart] STEP DONE v=1")
            lines.append("=== 04_uart selftest finished: 0 failed ===")
            lines.append("[uart] FW VERDICT=PASS")
            break

        lines.append(f"[uart] STEP {step.name} v=1")
        if step.name == "UT0":
            lines.append("[uart] VERDICT UT0 PASS")
        elif step.name == "UT0_CTRL":
            lines.append("[uart] VERDICT UT0_CTRL PASS")
        elif step.name == "UT1":
            lines.append("[uart] UT1 parameter rejections: snapshot_match=1 fail_count=0")
            lines.append("[uart] VERDICT UT1 PASS")
        elif step.name == "UT2":
            cases = [
                ("UART4", 100000000, 9600, False),
                ("UART4", 100000000, 115200, False),
                ("UART4", 100000000, 1200, False),
                ("UART5", 100000000, 921600, True),
                ("UART5", 100000000, 300, False),
                ("UART8", 100000000, 3773585, True),
                ("UART8", 100000000, 12500000, True),
                ("USART1", 80000000, 115200, False),
                ("USART6", 80000000, 921600, True),
                ("LPUART1", 25000000, 9600, False),
                ("LPUART1", 100000000, 9600, False),
            ]
            for inst, f_ker, baud, over8 in cases:
                p, b = compute_presc_brr(inst, f_ker, baud, over8)
                lines.append(f"[uart] UT2 row: tier=BM inst={inst} f_ker={f_ker} baud={baud} over8={1 if over8 else 0} presc=0x{p:X} brr=0x{b:X}")
                lines.append(f"[uart] UT2 row: tier=LL inst={inst} f_ker={f_ker} baud={baud} over8={1 if over8 else 0} presc=0x{p:X} brr=0x{b:X}")
            lines.append("[uart] UT2 reject: tier=BM inst=UART8 f_ker=100000000 baud=11764705 over8=1 status=REJECT")
            lines.append("[uart] UT2 reject: tier=LL inst=UART8 f_ker=100000000 baud=11764705 over8=1 status=REJECT")
            lines.append("[uart] UT2 reject: tier=BM inst=UART8 f_ker=100000000 baud=10526315 over8=1 status=REJECT")
            lines.append("[uart] UT2 reject: tier=LL inst=UART8 f_ker=100000000 baud=10526315 over8=1 status=REJECT")
            lines.append("[uart] VERDICT UT2 PASS")
        elif step.name == "UT3":
            bm_formats = [
                ("8N1", 115200, 0),
                ("8E1", 115200, 0),
                ("8O1", 115200, 0),
                ("7E1", 115200, 0),
                ("7N1", 115200, 0),
                ("9N1", 115200, 0),
                ("8N2", 115200, 0),
                ("8N1", 9600,   0),
                ("8N1", 921600, 1),
            ]
            for fmt, baud, over8 in bm_formats:
                p, b = compute_presc_brr("USART2", 100000000, baud, bool(over8))
                lines.append(f"[uart] UT3 row: tier=BM format={fmt} baud={baud} over8={over8} presc=0x{p:X} brr=0x{b:X}")
            ll_formats = [
                ("8N1", 115200, 0),
                ("8E1", 115200, 0),
                ("7E1", 115200, 0),
                ("9N1", 115200, 0),
            ]
            for fmt, baud, over8 in ll_formats:
                p, b = compute_presc_brr("USART2", 100000000, baud, bool(over8))
                lines.append(f"[uart] UT3 row: tier=LL format={fmt} baud={baud} over8={over8} presc=0x{p:X} brr=0x{b:X}")
            hal_formats = [
                ("8N1", 115200, 0),
                ("8E1", 115200, 0),
            ]
            for fmt, baud, over8 in hal_formats:
                p, b = compute_presc_brr("USART2", 100000000, baud, bool(over8))
                lines.append(f"[uart] UT3 row: tier=HAL format={fmt} baud={baud} over8={over8} presc=0x{p:X} brr=0x{b:X}")
            lines.append("[uart] VERDICT UT3 PASS")
        elif step.name == "UT4":
            lines.append("[uart] UT4 loopback: sent=72 received=72 err=0x0")
            lines.append("[uart] VERDICT UT4 PASS")
        elif step.name == "UT4_CM4":
            lines.append("[uart] UT4_CM4 loopback: sent=32 received=32 status=1")
            lines.append("[uart] VERDICT UT4_CM4 PASS")
        elif step.name == "UT5":
            lines.append("[uart] UT5 PE err=0x8 (expected 0x8)")
            lines.append("[uart] UT5 FE err=0x4 (expected 0x4)")
            lines.append("[uart] UT5 BREAK err=0x4 (expected FE 0x4)")
            lines.append("[uart] UT5 NE err=0x2 (flagged=1)")
            lines.append("[uart] UT5 ORE-FIFO err=0x1 (expected 0x1)")
            lines.append("[uart] UT5 ORE-NOFIFO err=0x1 (expected 0x1)")
            lines.append("[uart] UT5 clean frame err=0x0 (expected 0)")
            lines.append("[uart] VERDICT UT5 PASS")
        elif step.name == "UT6":
            lines.append("[uart] UT6 FC phase: written=24/24 read=24/24 err=0x0")
            lines.append("[uart] UT6 NO-FC negative control err=0x1 (expected ORE 0x1)")
            lines.append("[uart] VERDICT UT6 PASS")
        elif step.name == "UT7":
            lines.append("[uart] UT7 HAL InitEx: lock_held_status=2 rx_byte=0xa5")
            lines.append("[uart] VERDICT UT7 PASS")
        elif step.name == "UT8":
            lines.append("[uart] UT8 DMA errata: bit20=1 active_status=2 dma_bytes=20")
            lines.append("[uart] VERDICT UT8 PASS")
        elif step.name == "UT9":
            lines.append("[uart] UT9 fresh handle with lock held: returned 2 (expected 2)")
            lines.append("[uart] UT9 fresh handle with lock free: returned 0 (expected 0)")
            lines.append("[uart] VERDICT UT9 PASS")
    return lines


def generate_mock_la_samples(pattern: list[int], bit_period: int = 868,
                             data_bits: int = 8, parity: str = "none",
                             corrupt_parity: bool = False, frame_limit: Optional[int] = None,
                             sample_rate: float = 100e6, f_mco: float = 4000000.0) -> list[int]:
    """Generates synthesized 16-bit logic analyzer samples containing MCO1 and UART frames."""
    samples: list[int] = []
    # Idle resting state: 200 samples of DIO_RX high
    samples += [DIO_RX] * 200

    count = 0
    for byte in pattern:
        if frame_limit is not None and count >= frame_limit:
            break
        # Start bit = 0
        samples += [0] * bit_period
        # Data bits
        ones = 0
        for b in range(data_bits):
            bit = (byte >> b) & 1
            samples += [DIO_RX if bit else 0] * bit_period
            ones += bit
        # Parity bit
        if parity in ("even", "odd"):
            if parity == "even":
                par = 1 if (ones % 2 != 0) else 0
            else:
                par = 1 if (ones % 2 == 0) else 0
            if corrupt_parity:
                par ^= 1
            samples += [DIO_RX if par else 0] * bit_period
        # Stop bit = 1
        samples += [DIO_RX] * bit_period
        # Inter-frame gap
        samples += [DIO_RX] * bit_period
        count += 1

    # Pad remaining to 16384 with idle high
    if len(samples) < 16384:
        samples += [DIO_RX] * (16384 - len(samples))
    else:
        samples = samples[:16384]

    # Add MCO1 clock square wave on DIO14 using precise continuous phase
    out: list[int] = []
    for s_idx, val in enumerate(samples):
        mco = DIO_MCO if (((s_idx * f_mco / sample_rate) % 1.0) < 0.5) else 0
        out.append(val | mco)
    return out


class MockConsole:
    def __init__(self, lines: list[str], log: Callable[[str], None], ad2_inst: Optional[AD2] = None) -> None:
        self.lines = list(lines)
        self.log = log
        self.ad2_inst = ad2_inst
        self.idx = 0

    def readline(self, timeout_s: float = 10.0) -> Optional[str]:
        if self.idx < len(self.lines):
            line = self.lines[self.idx]
            self.idx += 1
            m = re.match(r"\[uart\] STEP (\w+)", line)
            if m and self.ad2_inst is not None:
                step_obj = STEP_BY_NAME.get(m.group(1), StepDef(m.group(1), "NONE"))
                self.ad2_inst.set_step(step_obj)
            return line
        return None

    def expect(self, pattern: str, timeout_s: float = 10.0):
        pat = re.compile(pattern)
        while self.idx < len(self.lines):
            line = self.lines[self.idx]
            self.idx += 1
            m = pat.search(line)
            if m:
                return m
        raise RuntimeError(f"Timeout waiting for pattern: {pattern}")


class Console:
    """Serial console reader with pattern expectation and timestamped logging."""
    def __init__(self, ser: SerialPort, t0: float, log: Callable[[str], None]) -> None:
        self.ser = ser
        self.t0 = t0
        self.log = log

    def readline(self, timeout_s: float = 10.0) -> Optional[str]:
        t_end = time.monotonic() + timeout_s
        line = ""
        while time.monotonic() < t_end:
            ch = self.ser.read(1)
            if not ch:
                continue
            if ch in (b"\r", b"\n"):
                if line:
                    return line
            else:
                line += ch.decode("utf-8", errors="replace")
        return line if line else None

    def expect(self, pattern: str, timeout_s: float = 10.0):
        pat = re.compile(pattern)
        t_end = time.monotonic() + timeout_s
        while time.monotonic() < t_end:
            line = self.readline(timeout_s=1.0)
            if line is not None:
                self.log(line)
                m = pat.search(line)
                if m:
                    return m
        raise RuntimeError(f"Timeout waiting for pattern: {pattern}")


# ---- UartSession Implementation ---------------------------------------------

class UartSession:
    def __init__(self, ad2: AD2, con: MockConsole | Console, log: Callable[[str], None],
                 expect_control_u1: bool = False, is_mock: bool = False) -> None:
        self.ad2 = ad2
        self.con = con
        self.log = log
        self.expect_control_u1 = expect_control_u1
        self.is_mock = is_mock
        self.checks: list[tuple[str, bool, str]] = []
        self.oracle_table: list[dict[str, str]] = []
        self.current_step = ""
        self.f_mco = 4000000.0
        self.f_ker = 100000000.0
        self.mco_ratio = 25.0

    def check(self, name: str, ok: bool, detail: str = "") -> None:
        status = "PASS" if ok else "FAIL"
        msg = f"[host] CHECK {name}: {status}"
        if detail:
            msg += f" ({detail})"
        self.log(msg)
        self.checks.append((name, ok, detail))

    def record_oracle(self, step: str, check_name: str, real_ok: bool, ctrl_failed: bool) -> None:
        real_str = "PASS" if real_ok else "FAIL"
        ctrl_str = "FAIL (exp)" if ctrl_failed else "PASS (unexp)"
        verdict = "PASS" if (real_ok and ctrl_failed) else "FAIL"
        self.oracle_table.append({
            "step": step,
            "check": check_name,
            "real": real_str,
            "control": ctrl_str,
            "verdict": verdict
        })

    def handshake(self, expected_name: str) -> bool:
        """Handshake: wait for STEP <expected_name>, set_step, wait ACK, REQ pulse."""
        self.current_step = expected_name
        step_obj = STEP_BY_NAME.get(expected_name, StepDef(expected_name, "NONE"))
        self.ad2.set_step(step_obj)
        # E-m: Reset Digital Out between steps
        ad2_dout_safe(self.ad2._dwf, self.ad2.handle)

        t_start = time.monotonic()
        while time.monotonic() - t_start < 10.0:
            line = self.con.readline(timeout_s=2.0)
            if line is None:
                if self.is_mock:
                    break
                continue
            self.log(line)
            if "[uart] ABORT" in line:
                self.check(f"fw_abort_{expected_name}", False, f"firmware aborted: {line}")
                return False
            m = re.search(r"\[uart\] STEP (\w+)", line)
            if m:
                if m.group(1) == expected_name:
                    break
                else:
                    self.check(f"step_match_{expected_name}", False, f"got {m.group(1)}")
                    return False
        else:
            self.check(f"step_timeout_{expected_name}", False, "timeout waiting for STEP")
            return False

        return self.sync()

    def sync(self) -> bool:
        if self.is_mock:
            return True
        t0 = time.monotonic()
        while not (self.ad2.digital_io_read() & DIO_ACK):
            if time.monotonic() - t0 > 10.0:
                self.check("sync_timeout_ack_high", False, "timeout waiting for MCU_ACK high")
                return False
            time.sleep(0.001)

        self.ad2.digital_io_write(DIO_REQ, DIO_REQ)

        t0 = time.monotonic()
        while self.ad2.digital_io_read() & DIO_ACK:
            if time.monotonic() - t0 > 10.0:
                self.check("sync_timeout_ack_low", False, "timeout waiting for MCU_ACK low")
                return False
            time.sleep(0.001)

        self.ad2.digital_io_write(0, DIO_REQ)
        return True

    def drain_until_verdict(self, step_name: str) -> bool:
        t_end = time.monotonic() + 15.0
        while time.monotonic() < t_end:
            line = self.con.readline(timeout_s=2.0)
            if line is None:
                continue
            self.log(line)
            if f"[uart] VERDICT {step_name}" in line:
                return "PASS" in line
            if "FW VERDICT=" in line:
                return "PASS" in line
        return False

    def print_oracle_summary(self) -> None:
        self.log("")
        self.log("=" * 88)
        self.log("[oracle] ORACLE VERIFICATION TABLE (The Oracle Rule)")
        self.log("=" * 88)
        self.log(f"| {'Step':<10} | {'Check Name':<42} | {'Real':<6} | {'Control':<11} | {'Status':<6} |")
        self.log(f"|{'-'*12}|{'-'*44}|{'-'*8}|{'-'*13}|{'-'*8}|")
        for entry in self.oracle_table:
            self.log(f"| {entry['step']:<10} | {entry['check']:<42} | {entry['real']:<6} | {entry['control']:<11} | {entry['verdict']:<6} |")
        self.log("=" * 88)
        self.log("")

    def run_all_steps(self) -> bool:
        try:
            # -----------------------------------------------------------------
            # UT0: Bench resting state and 1k series resistor check
            # -----------------------------------------------------------------
            if not self.handshake("UT0"): return False
            if not self.sync(): return False
            resting_high = ((self.ad2.digital_io_read() & DIO_RX) != 0)

            # Pull DIO15 low with OE (SU1 permitted in UT0 StepDef)
            self.ad2.set_oe(DIO_REQ | DIO_RX)
            self.ad2.digital_io_write(0, DIO_RX)
            time.sleep(0.002)

            if not self.sync(): return False
            pd6_low = ((self.ad2.digital_io_read() & DIO_RX) == 0)

            # Release back to input
            self.ad2.set_oe(DIO_REQ)
            time.sleep(0.002)

            if not self.sync(): return False
            pd6_restored = ((self.ad2.digital_io_read() & DIO_RX) != 0)

            ut0_fw_ok = self.drain_until_verdict("UT0")
            real_ut0 = eval_ut0(resting_high, pd6_low, pd6_restored, ut0_fw_ok)
            ctrl_ut0 = eval_ut0(not resting_high, pd6_low, pd6_restored, ut0_fw_ok)  # Mutated resting low
            self.record_oracle("UT0", "Bench resting state and 1k resistor", real_ut0, not ctrl_ut0)
            self.check("UT0 resting state and series resistor", real_ut0 and not ctrl_ut0)

            # -----------------------------------------------------------------
            # UT0_CTRL: Negative control with MCU driving PD5 low
            # -----------------------------------------------------------------
            if not self.handshake("UT0_CTRL"): return False
            if not self.sync(): return False
            ctrl_low = ((self.ad2.digital_io_read() & DIO_RX) == 0)
            ut0_ctrl_fw_ok = self.drain_until_verdict("UT0_CTRL")
            real_ctrl = eval_ut0_ctrl(ctrl_low, ut0_ctrl_fw_ok)
            ctrl_ctrl = eval_ut0_ctrl(not ctrl_low, ut0_ctrl_fw_ok)  # Mutated line high
            self.record_oracle("UT0_CTRL", "Negative control line low", real_ctrl, not ctrl_ctrl)
            self.check("UT0_CTRL negative control line low", real_ctrl and not ctrl_ctrl)

            # -----------------------------------------------------------------
            # UT1: Parameter rejection across tiers (BM, LL, HAL) (E-M2)
            # -----------------------------------------------------------------
            if not self.handshake("UT1"): return False
            snap_match = 1
            fail_cnt = 0
            t_end = time.monotonic() + 15.0
            ut1_ok = False
            while time.monotonic() < t_end:
                line = self.con.readline(timeout_s=2.0)
                if line is None:
                    continue
                self.log(line)
                m = re.search(r"UT1 parameter rejections: snapshot_match=(\d+) fail_count=(\d+)", line)
                if m:
                    snap_match = int(m.group(1))
                    fail_cnt = int(m.group(2))
                if "[uart] VERDICT UT1" in line:
                    ut1_ok = "PASS" in line
                    break

            real_ut1 = eval_ut1(snap_match == 1, fail_cnt, ut1_ok)
            ctrl_ut1 = eval_ut1(snap_match == 1, fail_cnt + 1, ut1_ok)  # Mutated failure count
            self.record_oracle("UT1", "Parameter rejections across BM, LL, HAL", real_ut1, not ctrl_ut1)
            self.check("UT1 parameter rejection across BM, LL, HAL", real_ut1 and not ctrl_ut1)

            # -----------------------------------------------------------------
            # UT2: Baudrate calculations and U5 rejection checks (E-m)
            # -----------------------------------------------------------------
            if not self.handshake("UT2"): return False
            ut2_rows: list[dict] = []
            ut2_rejects: list[dict] = []
            t_end = time.monotonic() + 15.0
            ut2_fw_ok = False
            while time.monotonic() < t_end:
                line = self.con.readline(timeout_s=2.0)
                if line is None:
                    continue
                self.log(line)
                m = re.search(r"UT2 row: tier=(\w+) inst=(\w+) f_ker=(\d+) baud=(\d+) over8=(\d+) presc=(0x[0-9a-fA-F]+) brr=(0x[0-9a-fA-F]+)", line)
                if m:
                    ut2_rows.append({
                        "tier": m.group(1), "inst": m.group(2), "f_ker": int(m.group(3)),
                        "baud": int(m.group(4)), "over8": (int(m.group(5)) == 1),
                        "presc": int(m.group(6), 16), "brr": int(m.group(7), 16)
                    })
                m_rej = re.search(r"UT2 reject: tier=(\w+) inst=(\w+) f_ker=(\d+) baud=(\d+) over8=(\d+) status=(\w+)", line)
                if m_rej:
                    ut2_rejects.append({
                        "tier": m_rej.group(1), "inst": m_rej.group(2), "f_ker": int(m_rej.group(3)),
                        "baud": int(m_rej.group(4)), "over8": (int(m_rej.group(5)) == 1),
                        "status": m_rej.group(6)
                    })
                if "[uart] VERDICT UT2" in line:
                    ut2_fw_ok = "PASS" in line
                    break

            real_ut2 = eval_ut2(ut2_rows, ut2_rejects, ut2_fw_ok)
            # Negative control: mutate measured BRR on first row
            mutated_rows = [dict(r) for r in ut2_rows]
            if mutated_rows:
                mutated_rows[0]["brr"] ^= 1
            ctrl_ut2 = eval_ut2(mutated_rows, ut2_rejects, ut2_fw_ok)
            self.record_oracle("UT2", "Baudrate PRESC/BRR formula and U5 checks", real_ut2, not ctrl_ut2)
            self.check("UT2 BRR/PRESC formula calculations and U5 checks", real_ut2 and not ctrl_ut2)

            # -----------------------------------------------------------------
            # UT3: Wire format encoding measured against MCO1 (E-B2, E-B3, E-M1)
            # -----------------------------------------------------------------
            if not self.handshake("UT3"): return False
            pat = [0x55, 0xAA, 0xA5, 0x5A, 0x00, 0xFF]
            ut3_row_results: list[bool] = []
            ctrl_ut3_results: list[bool] = []

            for row_idx in range(15):
                line = self.con.readline(timeout_s=5.0)
                if line is None:
                    self.check(f"ut3_row_{row_idx}_read", False, "timeout reading row line")
                    return False
                self.log(line)
                m = re.search(r"UT3 row: tier=(\w+) format=(\w+) baud=(\d+) over8=(\d+) presc=(0x[0-9a-fA-F]+) brr=(0x[0-9a-fA-F]+)", line)
                if not m:
                    self.check(f"ut3_row_{row_idx}_parse", False, f"unrecognized row line: {line}")
                    return False
                tier = m.group(1)
                fmt_str = m.group(2)
                baud = int(m.group(3))
                data_bits = int(fmt_str[0])
                parity = "none" if fmt_str[1] == "N" else ("even" if fmt_str[1] == "E" else "odd")
                stop_bits = int(fmt_str[2])

                over8 = (int(m.group(4)) != 0)
                presc_val = int(m.group(5), 16)
                brr_val = int(m.group(6), 16)
                baud_hw = compute_actual_baud(self.f_ker, presc_val, brr_val, over8)

                # Sample rate: 100 MS/s for 115200 and 921600; 10 MS/s for 9600 to capture full frame
                sample_rate = 10e6 if baud == 9600 else 100e6
                if self.is_mock and hasattr(self.ad2._dwf, "mock_la_samples"):
                    bp = int(round(sample_rate / baud_hw))
                    self.ad2._dwf.mock_la_samples = generate_mock_la_samples(
                        pat, bit_period=bp, data_bits=data_bits, parity=parity,
                        sample_rate=sample_rate, f_mco=self.f_mco
                    )

                # Arm LA with falling edge trigger on DIO15 (E-B3)
                rate, n = self.ad2.la_arm(sample_rate, samples=16384, trigger_fall_mask=DIO_RX, prefill=200)

                # Sync per row with MCU (E-B2)
                if not self.sync(): return False

                samples = self.ad2.la_read(n, timeout_s=3.0) or ([0xFFFF] * n)
                bit_period_samples = int(round(rate / baud))
                decoded = decode_uart_frames(
                    samples, bit_period_samples, data_bits=data_bits,
                    parity=parity, stop_bits=stop_bits, f_mco=self.f_mco, baud=baud, rate=rate
                )

                row_real = eval_ut3(decoded, pat, parity, self.f_mco, baud_hw, data_bits=data_bits, tolerance=0.005)
                # Negative control: mutate measured data (corrupt bit duration / mco_per_bit by 2.5%, violating 0.5% tolerance)
                mutated_frames = [{**f, "mco_per_bit": f["mco_per_bit"] * 1.025} for f in decoded]
                row_ctrl = eval_ut3(mutated_frames, pat, parity, self.f_mco, baud_hw, data_bits=data_bits, tolerance=0.005)

                ut3_row_results.append(row_real)
                ctrl_ut3_results.append(row_ctrl)

            ut3_fw_ok = self.drain_until_verdict("UT3")
            real_ut3 = (len(ut3_row_results) == 15) and all(ut3_row_results) and ut3_fw_ok
            ctrl_ut3 = any(ctrl_ut3_results)  # None of the mutated controls should pass

            self.record_oracle("UT3", "Wire format and bit period against MCO1", real_ut3, not ctrl_ut3)
            self.check("UT3 wire format encoding across BM, LL, HAL (MCO1 ±0.5% tolerance)", real_ut3 and not ctrl_ut3)

            # -----------------------------------------------------------------
            # UT4: CM7 loopback reception over JU1 (E-M2)
            # -----------------------------------------------------------------
            if not self.handshake("UT4"): return False
            sent_cnt = 0
            recv_cnt = 0
            err_cnt = 1
            t_end = time.monotonic() + 15.0
            ut4_ok = False
            while time.monotonic() < t_end:
                line = self.con.readline(timeout_s=2.0)
                if line is None:
                    continue
                self.log(line)
                m = re.search(r"UT4 loopback: sent=(\d+) received=(\d+) err=(0x[0-9a-fA-F]+)", line)
                if m:
                    sent_cnt = int(m.group(1))
                    recv_cnt = int(m.group(2))
                    err_cnt = int(m.group(3), 16)
                if "[uart] VERDICT UT4" in line:
                    ut4_ok = "PASS" in line
                    break

            real_ut4 = eval_ut4(sent_cnt, recv_cnt, err_cnt, ut4_ok)
            ctrl_ut4 = eval_ut4(sent_cnt, recv_cnt - 1, err_cnt, ut4_ok)  # Mutated byte count
            self.record_oracle("UT4", "CM7 loopback reception over JU1", real_ut4, not ctrl_ut4)
            self.check("UT4 loopback reception CM7 (polled, irq, fifo, over8, 7b mask)", real_ut4 and not ctrl_ut4)

            # -----------------------------------------------------------------
            # UT4_CM4: CM4 loopback reception over JU1 (E-M2)
            # -----------------------------------------------------------------
            if not self.handshake("UT4_CM4"): return False
            m4_sent = 0
            m4_recv = 0
            m4_status = 0
            t_end = time.monotonic() + 15.0
            ut4_cm4_ok = False
            while time.monotonic() < t_end:
                line = self.con.readline(timeout_s=2.0)
                if line is None:
                    continue
                self.log(line)
                m = re.search(r"UT4_CM4 loopback: sent=(\d+) received=(\d+) status=(\d+)", line)
                if m:
                    m4_sent = int(m.group(1))
                    m4_recv = int(m.group(2))
                    m4_status = int(m.group(3))
                if "[uart] VERDICT UT4_CM4" in line:
                    ut4_cm4_ok = "PASS" in line
                    break

            real_ut4_cm4 = eval_ut4_cm4(m4_sent, m4_recv, m4_status, ut4_cm4_ok)
            ctrl_ut4_cm4 = eval_ut4_cm4(m4_sent, m4_recv - 1, m4_status, ut4_cm4_ok)
            self.record_oracle("UT4_CM4", "CM4 loopback reception over JU1", real_ut4_cm4, not ctrl_ut4_cm4)
            self.check("UT4_CM4 loopback reception CM4", real_ut4_cm4 and not ctrl_ut4_cm4)

            # -----------------------------------------------------------------
            # UT5: Error injection via DIO15 (SU1 DOUT generator, E-B4, E-M4)
            # -----------------------------------------------------------------
            if not self.handshake("UT5"): return False
            clean_11b = [0, 1, 0, 1, 0, 1, 0, 1, 0, 0, 1]  # Start, 0x55, Even Parity=0, Stop=1
            pe_11b    = [0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 1]  # Parity bit corrupted
            fe_11b    = [0, 1, 0, 1, 0, 1, 0, 1, 0, 0, 0]  # Stop bit corrupted to 0
            break_11b = [0] * 11
            ore_fifo_11b   = clean_11b * 18
            ore_nofifo_11b = clean_11b * 3

            # Sub-bit glitch pattern for NE (E-M4): 4 sub-samples per bit
            ne_sub_bits = []
            for b_idx, b_val in enumerate(clean_11b):
                if b_idx == 2:  # Data bit 1 (which is 0): inject a 1-sample glitch in middle
                    ne_sub_bits.extend([0, 1, 0, 0])
                elif b_val == 1:
                    ne_sub_bits.extend([1, 1, 1, 1])
                else:
                    ne_sub_bits.extend([0, 0, 0, 0])

            phases = [
                ("PE", pe_11b, False),
                ("FE", fe_11b, False),
                ("BREAK", break_11b, False),
                ("NE", ne_sub_bits, True),
                ("ORE-FIFO", ore_fifo_11b, False),
                ("ORE-NOFIFO", ore_nofifo_11b, False),
                ("CLEAN", clean_11b, False),
            ]

            clk_hz = getattr(self.ad2, "dout_clock_hz", lambda: 100e6)()
            divider = int(round(clk_hz / 9600.0))

            for phase_name, bits, is_ne in phases:
                if not self.sync(): return False
                if is_ne:
                    div = max(1, divider // 4)
                    dur = len(bits) * (div / clk_hz)
                    self.ad2.dout_custom(15, div, bits, single_shot=True)
                else:
                    dur = len(bits) * (divider / clk_hz)
                    self.ad2.dout_custom(15, divider, bits, single_shot=True)

                # E-B4: Start DOUT, wait, stop DOUT, reset DOUT
                self.ad2.set_dout(1 << 15)
                time.sleep(dur + 0.005)
                self.ad2.set_dout(0)
                ad2_dout_safe(self.ad2._dwf, self.ad2.handle)

                if not self.sync(): return False

            # Harvest errors logged by firmware
            measured_errs: dict[str, int] = {}
            t_end = time.monotonic() + 15.0
            ut5_ok = False
            while time.monotonic() < t_end:
                line = self.con.readline(timeout_s=2.0)
                if line is None:
                    continue
                self.log(line)
                m = re.search(r"UT5 (\S+) err=(0x[0-9a-fA-F]+)", line)
                if m:
                    pname = m.group(1)
                    err_val = int(m.group(2), 16)
                    measured_errs[pname] = err_val
                m_clean = re.search(r"UT5 clean frame err=(0x[0-9a-fA-F]+)", line)
                if m_clean:
                    measured_errs["CLEAN"] = int(m_clean.group(1), 16)
                if "[uart] VERDICT UT5" in line:
                    ut5_ok = "PASS" in line
                    break

            real_ut5 = eval_ut5(measured_errs, ut5_ok)
            mutated_errs = dict(measured_errs)
            mutated_errs["PE"] = 0  # Mutate measured PE to 0
            ctrl_ut5 = eval_ut5(mutated_errs, ut5_ok)
            self.record_oracle("UT5", "Error injection (PE, FE, NE, ORE, clean)", real_ut5, not ctrl_ut5)
            self.check("UT5 error injection detection (PE, FE, NE, ORE, clean control)", real_ut5 and not ctrl_ut5)

            # -----------------------------------------------------------------
            # UT6: RTS/CTS flow control over JU2 (E-M3, E-M2)
            # -----------------------------------------------------------------
            if not self.handshake("UT6"): return False
            fc_written = 0
            fc_read = 0
            fc_err = 1
            nofc_err = 0
            t_end = time.monotonic() + 15.0
            ut6_ok = False
            while time.monotonic() < t_end:
                line = self.con.readline(timeout_s=2.0)
                if line is None:
                    continue
                self.log(line)
                m_fc = re.search(r"UT6 FC phase: written=(\d+)/24 read=(\d+)/24 err=(0x[0-9a-fA-F]+)", line)
                if m_fc:
                    fc_written = int(m_fc.group(1))
                    fc_read = int(m_fc.group(2))
                    fc_err = int(m_fc.group(3), 16)
                m_nofc = re.search(r"UT6 NO-FC negative control err=(0x[0-9a-fA-F]+)", line)
                if m_nofc:
                    nofc_err = int(m_nofc.group(1), 16)
                if "[uart] VERDICT UT6" in line:
                    ut6_ok = "PASS" in line
                    break

            real_ut6 = eval_ut6(fc_written, fc_read, fc_err, nofc_err, ut6_ok)
            ctrl_ut6 = eval_ut6(fc_written, fc_read, fc_err, 0, ut6_ok)  # Mutated NO-FC err
            self.record_oracle("UT6", "RTS/CTS flow control over JU2", real_ut6, not ctrl_ut6)
            self.check("UT6 RTS/CTS flow control over JU2 (with ORE negative control)", real_ut6 and not ctrl_ut6)

            # -----------------------------------------------------------------
            # UT7: HAL InitEx 8E1 and GPIO lock contention (E-M2)
            # -----------------------------------------------------------------
            if not self.handshake("UT7"): return False
            lock_held_st = 0
            rx_byte = 0
            t_end = time.monotonic() + 15.0
            ut7_ok = False
            while time.monotonic() < t_end:
                line = self.con.readline(timeout_s=2.0)
                if line is None:
                    continue
                self.log(line)
                m = re.search(r"UT7 HAL InitEx: lock_held_status=(\d+) rx_byte=(0x[0-9a-fA-F]+)", line)
                if m:
                    lock_held_st = int(m.group(1))
                    rx_byte = int(m.group(2), 16)
                if "[uart] VERDICT UT7" in line:
                    ut7_ok = "PASS" in line
                    break

            real_ut7 = eval_ut7(lock_held_st, rx_byte, ut7_ok)
            ctrl_ut7 = eval_ut7(0, rx_byte, ut7_ok)  # Mutated lock status
            self.record_oracle("UT7", "HAL InitEx 8E1 and GPIO lock contention", real_ut7, not ctrl_ut7)
            self.check("UT7 HAL InitEx 8E1 and GPIO lock contention", real_ut7 and not ctrl_ut7)

            # -----------------------------------------------------------------
            # UT8: DMA errata live transfer and rejections (E-M2)
            # -----------------------------------------------------------------
            if not self.handshake("UT8"): return False
            bit20 = 0
            dma_active = 0
            dma_bytes = 0
            t_end = time.monotonic() + 15.0
            ut8_ok = False
            while time.monotonic() < t_end:
                line = self.con.readline(timeout_s=2.0)
                if line is None:
                    continue
                self.log(line)
                m = re.search(r"UT8 DMA errata: bit20=(\d+) active_status=(\d+) dma_bytes=(\d+)", line)
                if m:
                    bit20 = int(m.group(1))
                    dma_active = int(m.group(2))
                    dma_bytes = int(m.group(3))
                if "[uart] VERDICT UT8" in line:
                    ut8_ok = "PASS" in line
                    break

            real_ut8 = eval_ut8(bit20, dma_active, dma_bytes, ut8_ok)
            ctrl_ut8 = eval_ut8(0, dma_active, dma_bytes, ut8_ok)  # Mutated bit20
            self.record_oracle("UT8", "DMA errata live transfer and rejections", real_ut8, not ctrl_ut8)
            self.check("UT8 DMA errata live transfer and rejections", real_ut8 and not ctrl_ut8)

            # -----------------------------------------------------------------
            # UT9: U1 console handle GPIO lock and state reset (E-M2)
            # -----------------------------------------------------------------
            if not self.handshake("UT9"): return False
            ret_held = -1
            ret_free = -1
            t_end = time.monotonic() + 15.0
            ut9_ok = False
            while time.monotonic() < t_end:
                line = self.con.readline(timeout_s=2.0)
                if line is None:
                    continue
                self.log(line)
                m_h = re.search(r"UT9 fresh handle with lock held: returned (\d+)", line)
                if m_h:
                    ret_held = int(m_h.group(1))
                m_f = re.search(r"UT9 fresh handle with lock free: returned (\d+)", line)
                if m_f:
                    ret_free = int(m_f.group(1))
                if "[uart] VERDICT UT9" in line:
                    ut9_ok = "PASS" in line
                    break

            real_ut9 = eval_ut9(ret_held, ret_free, ut9_ok, self.expect_control_u1)
            ctrl_ut9 = eval_ut9(0, ret_free, ut9_ok, self.expect_control_u1)  # Mutated lock held return
            self.record_oracle("UT9", "U1 console handle GPIO lock and state reset", real_ut9, not ctrl_ut9)
            self.check("UT9 console handle GPIO lock error propagation", real_ut9 and not ctrl_ut9)

            # -----------------------------------------------------------------
            # DONE
            # -----------------------------------------------------------------
            if not self.handshake("DONE"): return False
            done_pass = self.drain_until_verdict("DONE")

            self.print_oracle_summary()

            overall = (all(passed for _, passed, _ in self.checks) and
                       all(entry["verdict"] == "PASS" for entry in self.oracle_table) and
                       done_pass)
            return overall

        finally:
            self.ad2.set_safe_state()


# ---- Mock Test Suite (C-M7, E-M5) -------------------------------------------

def run_mock_test(log: Callable[[str], None]) -> bool:
    """Executes the complete mock validation suite exercising real UartSession logic."""
    log("[mock] === Phase 1: SU1 & S11 Guard Checks ===")
    mock_dwf = _MockDwf()
    mock_ad2 = AD2(dwf_lib=mock_dwf)
    mock_ad2.open()

    guard_pass_1 = False
    try:
        ad2_set_oe(mock_dwf, 1, DIO_REQ | DIO_RX, current_step=StepDef("UT3", "NONE"))
    except AD2SafetyViolation:
        guard_pass_1 = True
    except Exception as e:
        log(f"[mock] Wrong exception for DIO15 in UT3: {e}")

    guard_pass_2 = False
    try:
        ad2_set_dout(mock_dwf, 1, 1 << 15, current_step=StepDef("UT3", "NONE"))
    except AD2SafetyViolation:
        guard_pass_2 = True
    except Exception as e:
        log(f"[mock] Wrong exception for DOUT on DIO15 in UT3: {e}")

    guard_pass_3 = False
    try:
        ad2_set_oe(mock_dwf, 1, DIO_REQ | DIO_RX, current_step=StepDef("UT0", "OE"))
        guard_pass_3 = True
    except Exception as e:
        log(f"[mock] Unexpected failure in UT0 with DIO15: {e}")

    guard_pass_4 = False
    try:
        ad2_set_oe(mock_dwf, 1, DIO_REQ | 0x01, current_step=StepDef("UT0", "OE"))
    except AD2SafetyViolation:
        guard_pass_4 = True
    except Exception as e:
        log(f"[mock] Wrong exception for prohibited DIO0: {e}")

    su1_guards_ok = guard_pass_1 and guard_pass_2 and guard_pass_3 and guard_pass_4
    log(f"[mock] SU1 safety guards: {'PASS' if su1_guards_ok else 'FAIL'}")

    log("[mock] === Phase 2: Full Mock Session of All Steps (UT0..UT9) ===")
    session_dwf = _MockDwf()
    pat = [0x55, 0xAA, 0xA5, 0x5A, 0x00, 0xFF]
    session_dwf.mock_la_samples = generate_mock_la_samples(pat, bit_period=868, parity="none", sample_rate=100e6, f_mco=4000000.0)

    mock_ad2 = AD2(dwf_lib=session_dwf)
    mock_ad2.open()

    fw_lines = generate_mock_lines()
    mock_con = MockConsole(fw_lines, log, ad2_inst=mock_ad2)
    session = UartSession(mock_ad2, mock_con, log, is_mock=True)

    session_ok = session.run_all_steps()
    log(f"[mock] Full mock session overall: {'PASS' if session_ok else 'FAIL'}")

    log("[mock] === Phase 3: Negative Control 1 (Bad Parity Detection on Wire via Session) ===")
    bad_la = generate_mock_la_samples(pat, bit_period=868, parity="even", corrupt_parity=True, sample_rate=100e6, f_mco=4000000.0)
    decoded_bad = decode_uart_frames(bad_la, bit_period_samples=868, parity="even", f_mco=4000000.0, baud=115200)
    bad_parity_detected = (len(decoded_bad) > 0 and not decoded_bad[0]["parity_ok"])
    log(f"[mock] Negative control 1 (bad parity flagged as error by decoder): {'PASS' if bad_parity_detected else 'FAIL'}")

    log("[mock] === Phase 4: Negative Control 2 (ABORT Detection in Session) ===")
    abort_dwf = _MockDwf()
    abort_ad2 = AD2(dwf_lib=abort_dwf)
    abort_ad2.open()
    abort_lines = [
        "=== 04_uart selftest starting on CM7 ===",
        "[uart] CM7 Rev mock_rev",
        "[uart] CM7 Build id mock_id",
        "[uart] CM4 Rev mock_rev",
        "[uart] CM4 Build id mock_id",
        "[uart] ABORT step=- reason=MCO1 claim refused",
    ]
    abort_con = MockConsole(abort_lines, log, ad2_inst=abort_ad2)
    abort_session = UartSession(abort_ad2, abort_con, log, is_mock=True)
    abort_caught = not abort_session.run_all_steps()
    log(f"[mock] Negative control 2 (ABORT path detected as FAIL): {'PASS' if abort_caught else 'FAIL'}")

    all_mock_ok = su1_guards_ok and session_ok and bad_parity_detected and abort_caught
    log(f"[host] OVERALL MOCK VERDICT: {'PASS' if all_mock_ok else 'FAIL'}")
    log(f"HOST VERDICT={'PASS' if all_mock_ok else 'FAIL'}")
    return all_mock_ok


# ---- Main CLI ----------------------------------------------------------------

def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--port", help="ST-LINK VCP (default: autodetect)")
    ap.add_argument("--preset", default="selftest-uart", help="preset name (default: selftest-uart)")
    ap.add_argument("--allow-dirty", action="store_true", help="allow -dirty Git revision")
    ap.add_argument("--no-reset", action="store_true", help="do not reset board via SWD")
    ap.add_argument("--mock-test", action="store_true", help="run without hardware/AD2 on mock")
    ap.add_argument("--expect-control-u1", action="store_true", help="mark U1 failure as expected control")
    ap.add_argument("--out", type=pathlib.Path, default=None, help="capture output file")
    args = ap.parse_args()

    rev = git_rev()
    out_path = args.out
    if out_path is None:
        docs_dir = PROJECT / "docs"
        docs_dir.mkdir(parents=True, exist_ok=True)
        if args.mock_test:
            out_path = docs_dir / f"uart_mock_{rev}.txt"
        else:
            out_path = docs_dir / f"uart_hw_check_{rev}.txt"

    lines: list[str] = []
    t0 = time.monotonic()
    lock = threading.Lock()

    def stamp(msg: str) -> None:
        entry = f"[+{time.monotonic() - t0:8.3f}s] {msg}"
        with lock:
            lines.append(entry)
        print(entry, flush=True)

    stamp(f"[host] argv: {shlex.join(sys.argv[1:])}")
    stamp(f"[host] options: preset={args.preset} allow_dirty={args.allow_dirty} mock_test={args.mock_test} expect_control_u1={args.expect_control_u1}")
    stamp(f"[host] uart_hw_check.py, host checkout {rev}, protocol v{UART_BENCH_VERSION}")

    # Check clean working tree at start (E-m)
    clean_ok, clean_msg = check_clean_tree(WORKSPACE, out_path)
    stamp(f"[host] clean tree at start: {'PASS' if clean_ok else 'FAIL'} ({clean_msg})")
    if not clean_ok and not args.allow_dirty:
        stamp("HOST VERDICT=FAIL (dirty working tree)")
        return 1

    ok = False
    try:
        if args.mock_test:
            stamp("[host] running non-board mock test suite for Phase 04_uart F1")
            ok = run_mock_test(stamp)
            return 0 if ok else 1

        build_id = host_build_id(args.preset)
        stamp(f"[host] build id of this checkout and build/{args.preset}: {build_id}")

        port = args.port or autodetect()
        if not port:
            stamp("[host] ERROR: no ST-LINK VCP found")
            return 2

        revs: dict[str, str] = {}
        ids: dict[str, str] = {}

        with AD2(config_idx=6) as ad2, SerialPort(port, 115200) as ser:
            stamp(f"[ad2] opened in S2 (config 6, 16k buffer): OE=0x{ad2.get_oe():04X} DIO=0x{ad2.digital_io_read():04X}")
            con = Console(ser, t0, stamp)
            try:
                if not args.no_reset:
                    cmd_rst = [cube_programmer(), "-c", "port=SWD", f"sn={BOARD_STLINK_SN}", "mode=HOTPLUG", "-rst"]
                    stamp(f"[host] reset through SWD (sn={BOARD_STLINK_SN}): {' '.join(cmd_rst)}")
                    rst_res = subprocess.run(cmd_rst, capture_output=True, text=True)
                    stamp(f"[host] SWD reset rc={rst_res.returncode}")
                    for line in rst_res.stdout.splitlines():
                        if line.strip():
                            stamp(f"[swd] {line.strip()}")

                # R3-a, R3-b, R4-a: Image identity verification
                revs["cm7"] = con.expect(r"\[uart\] CM7 Rev (\S+)", 20.0).group(1)
                ids["cm7"] = con.expect(r"\[uart\] CM7 Build id (\S+)", 10.0).group(1)
                revs["cm4"] = con.expect(r"\[uart\] CM4 Rev (\S+)", 10.0).group(1)
                ids["cm4"] = con.expect(r"\[uart\] CM4 Build id (.+?)\s*$", 10.0).group(1)

                stamp(f"[host] R4-a: cm7 Rev={revs['cm7']} id={ids['cm7']}; cm4 Rev={revs['cm4']} id={ids['cm4']}; host HEAD={rev}")
                if revs["cm7"] != revs["cm4"]:
                    raise RuntimeError(f"R4-a: cm7 and cm4 come from different trees (Rev cm7={revs['cm7']} cm4={revs['cm4']})")
                if ids["cm7"] != ids["cm4"] or "unknown" in (ids["cm7"], ids["cm4"]):
                    raise RuntimeError(f"R4-a: cm7 and cm4 are not one build (id cm7={ids['cm7']} cm4={ids['cm4']})")

                base = {c: (r[:-len("-dirty")] if r.endswith("-dirty") else r) for c, r in revs.items()}
                for core in ("cm7", "cm4"):
                    if rev == "unknown" or base[core] != rev:
                        raise RuntimeError(f"R3-b: {core} was built at {base[core]} (Rev {revs[core]}), this checkout is at {rev}")

                dirty = revs["cm7"].endswith("-dirty")
                if dirty and not args.allow_dirty:
                    raise RuntimeError(f"R2: Rev cm7={revs['cm7']} cm4={revs['cm4']} is not clean")

                if build_id == "unknown" or ids["cm7"] != build_id:
                    if not args.allow_dirty:
                        raise RuntimeError(f"R3-a: image is not the one this checkout builds (host={build_id} cm7={ids['cm7']})")

                # Parse MCO1 line (E-B1)
                mco_m = con.expect(r"\[uart\] MCO1: PA8 enabled, freq=(\d+) Hz, f_ker=(\d+) Hz, ratio=(\d+):1", 10.0)
                f_mco = float(mco_m.group(1))
                f_ker = float(mco_m.group(2))
                mco_ratio = float(mco_m.group(3))

                session = UartSession(ad2, con, stamp, expect_control_u1=args.expect_control_u1, is_mock=False)
                session.f_mco = f_mco
                session.f_ker = f_ker
                session.mco_ratio = mco_ratio

                session.ad2.set_oe(DIO_REQ)
                session.ad2.digital_io_write(0, DIO_REQ)
                ok = session.run_all_steps()

                # Clean tree check at end (E-m)
                clean_end_ok, clean_end_msg = check_clean_tree(WORKSPACE, out_path)
                stamp(f"[host] clean tree at end: {'PASS' if clean_end_ok else 'FAIL'} ({clean_end_msg})")
                if not clean_end_ok and not args.allow_dirty:
                    ok = False

                verdict_str = "PASS" if ok else "FAIL"
                stamp(f"[host] OVERALL HARDWARE VERDICT: {verdict_str}")
                stamp(f"HOST VERDICT={verdict_str}")

            finally:
                ad2.set_safe_state()

    finally:
        out_path.parent.mkdir(parents=True, exist_ok=True)
        with open(out_path, "w", encoding="utf-8") as f:
            for l in lines:
                f.write(l + "\n")
        print(f"[host] Capture saved to {out_path}")

    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
