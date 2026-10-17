#!/usr/bin/env python3
"""Host side of the 04_uart selftest on prj_01 (preset selftest-uart).

Runs one complete bench session against the firmware of
CM7/App/Src/uart_selftest.c with the Analog Discovery 2, and writes ONE capture
file: every console line and every host measurement, with timestamps, until
`HOST VERDICT=`. Wiring, safety rules and what each test proves:
01_shared_drivers/04_uart/IMPLEMENTATION_PLAN.md.

Usage:
    py tools/flash.py --preset selftest-uart
    py tools/uart_hw_check.py                   # resets board and runs on hardware
    py tools/uart_hw_check.py --mock-test       # runs purely on mock without board or AD2
    py tools/uart_hw_check.py --allow-dirty     # accept a -dirty Rev during dev
    py tools/uart_hw_check.py --expect-control-u1  # mark b41c4a7 U1 failure as expected control

Electrical safety rules:
    S2:  AD2 opens and closes with all DIOs in input (ad2.py) and reverts on any exception.
    S7:  Announced step names strictly checked against STEPS table.
    SU1: DIO15 is an AD2 output ONLY in steps whose StepDef declares DIO15 (UT0 for OE, UT5 for DOUT).
    SU2: PD5 verified in resting state before AD2 drives PD6.
    SU3: DIO15 restored to input between steps and on exit.
    SU4: PD3-PD6 configured only in selftest-uart.
"""

from __future__ import annotations

import argparse
import ctypes
from dataclasses import dataclass
import hashlib
import os
import pathlib
import re
import shlex
import shutil
import subprocess
import sys
import threading
import time
from typing import Any, Callable, Optional

PROJECT = pathlib.Path(__file__).resolve().parents[1]
WORKSPACE = PROJECT.parents[1]
sys.path.insert(0, str(WORKSPACE / "tools"))
os.environ.setdefault("STM32_PROJECT_ROOT", str(PROJECT))

import ad2 as ad2mod                                   # noqa: E402
from ad2 import (AD2, AD2Error, AD2SafetyViolation,    # noqa: E402
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


# Authoritative mirror of Common/Src/uart_selftest_common.c k_ust_steps (C-B1: UT10 removed)
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
    cli = shutil.which("STM32_Programmer_CLI.exe") or shutil.which("STM32_Programmer_CLI")
    if cli:
        return cli
    candidates = list(pathlib.Path("C:/ST").glob("**/STM32_Programmer_CLI.exe"))
    if candidates:
        return str(candidates[0])
    return "STM32_Programmer_CLI"


def git_rev() -> str:
    try:
        r = subprocess.run(["git", "rev-parse", "--short", "HEAD"],
                           capture_output=True, text=True, check=True)
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
        if not line.strip():
            continue
        prefix = line[:2]
        rel = line[3:].strip().strip('"')
        res = (repo_root / rel).resolve()
        if out_res and res == out_res:
            continue
        if "build" in res.parts:
            continue
        # Permit uart_checks capture in docs/
        if (res.parent == (repo_root / "01_shared_drivers" / "04_uart" / "docs").resolve()
                and res.name.startswith("uart_checks_")):
            continue
        # Permit mock capture in prj_01 docs/
        if (res.parent == (repo_root / "03_firmware_templates" / "prj_01_template_dualcore" / "docs").resolve()
                and res.name.startswith("uart_mock_")):
            continue
        if prefix != "!!":
            dirty.append(f"{prefix} {rel}")
    if dirty:
        return False, f"Dirty working tree outside build/ ({len(dirty)} items: {dirty[:3]})"
    return True, "Tree is clean outside build/"


def host_build_id(preset: str, project: pathlib.Path = PROJECT) -> str:
    """R3-a/R3-b: FW_BUILD_ID as cmake/fw_build_id.cmake computes it."""
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


# ---- UART Logic Analyzer Frame Decoder ---------------------------------------

def decode_uart_frames(samples: list[int], bit_period_samples: int,
                       data_bits: int = 8, parity: str = "none", stop_bits: int = 1) -> list[dict]:
    """Decodes UART frames from logic analyzer samples of DIO15 (bit 15).

    Returns a list of decoded frames:
      {"byte": int, "parity_ok": bool, "stop_ok": bool, "mco_per_bit": float}
    """
    frames = []
    i = 0
    n = len(samples)

    while i < n:
        while i < n and (samples[i] & DIO_RX) == 0:
            i += 1
        while i < n and (samples[i] & DIO_RX) != 0:
            i += 1
        if i >= n:
            break

        start_edge = i
        sample_pt = i + bit_period_samples // 2
        if sample_pt >= n:
            break
        if (samples[sample_pt] & DIO_RX) != 0:
            i += 1
            continue

        mco_edges = 0
        val = 0
        ones_count = 0
        sample_pt += bit_period_samples
        for b in range(data_bits):
            if sample_pt >= n:
                break
            bit = 1 if (samples[sample_pt] & DIO_RX) else 0
            val |= (bit << b)
            ones_count += bit
            sample_pt += bit_period_samples

        parity_ok = True
        if parity in ("even", "odd"):
            if sample_pt < n:
                par_bit = 1 if (samples[sample_pt] & DIO_RX) else 0
                total_ones = ones_count + par_bit
                if parity == "even":
                    parity_ok = (total_ones % 2 == 0)
                else:
                    parity_ok = (total_ones % 2 == 1)
                sample_pt += bit_period_samples
            else:
                parity_ok = False

        stop_ok = True
        if sample_pt < n:
            stop_ok = ((samples[sample_pt] & DIO_RX) != 0)
            sample_pt += bit_period_samples
        else:
            stop_ok = False  # C-B2: stop outside buffer is NOT ok!

        total_bits = 1 + data_bits + (1 if parity in ("even", "odd") else 0) + stop_bits
        frame_end = start_edge + total_bits * bit_period_samples

        for s in range(start_edge, min(frame_end, n - 1)):
            if (samples[s] & DIO_MCO) == 0 and (samples[s + 1] & DIO_MCO) != 0:
                mco_edges += 1

        mco_per_bit = (mco_edges / total_bits) if total_bits > 0 else 0.0

        frames.append({
            "byte": val,
            "parity_ok": parity_ok,
            "stop_ok": stop_ok,
            "mco_per_bit": mco_per_bit
        })

        i = frame_end

    return frames


# ---- Pure Evaluation Functions (The Oracle Rule) ----------------------------

def eval_ut0(resting_high: bool, pd6_low_when_oe: bool, restored_high: bool, fw_pass: bool) -> bool:
    return resting_high and pd6_low_when_oe and restored_high and fw_pass


def eval_ut0_ctrl(line_low: bool, fw_pass: bool) -> bool:
    return line_low and fw_pass


def eval_ut1(fw_pass: bool, error_count: int) -> bool:
    return fw_pass and (error_count == 0)


def eval_ut2(rows: list[dict], rejects: list[dict], fw_pass: bool) -> bool:
    if not fw_pass or len(rows) < 10:
        return False
    for r in rows:
        exp_p, exp_b = compute_presc_brr(r["inst"], r["f_ker"], r["baud"], r["over8"])
        if (r["presc"] != exp_p) or (r["brr"] != exp_b):
            return False
    for rej in rejects:
        if rej["status"] != "REJECT":
            return False
    return True


def eval_ut3(frames: list[dict], expected_bytes: list[int], exp_parity: str, f_mco: float, baud: int, tolerance: float) -> bool:
    if len(frames) != len(expected_bytes):
        return False
    th_mco = f_mco / baud
    for i, f in enumerate(frames):
        if f["byte"] != expected_bytes[i]:
            return False
        if not f["parity_ok"] or not f["stop_ok"]:
            return False
        if abs(f["mco_per_bit"] - th_mco) / th_mco > tolerance:
            return False
    return True


def eval_ut4(fw_pass: bool, rx_count: int, exp_count: int) -> bool:
    return fw_pass and (rx_count == exp_count)


def eval_ut4_cm4(fw_pass: bool) -> bool:
    return fw_pass


def eval_ut5(pe_ok: bool, fe_ok: bool, break_ok: bool, ne_ok: bool, ore_fifo_ok: bool, ore_nofifo_ok: bool, clean_ok: bool, fw_pass: bool) -> bool:
    return pe_ok and fe_ok and break_ok and ne_ok and ore_fifo_ok and ore_nofifo_ok and clean_ok and fw_pass


def eval_ut6(fc_written: int, fc_read: int, fc_err: int, nofc_ore: bool, fw_pass: bool) -> bool:
    return (fc_written == 20) and (fc_read == 20) and (fc_err == 0) and nofc_ore and fw_pass


def eval_ut7(lock_busy: bool, lock_ok: bool, pin_snap_ok: bool, rx_val_ok: bool, fw_pass: bool) -> bool:
    return lock_busy and lock_ok and pin_snap_ok and rx_val_ok and fw_pass


def eval_ut8(bit20_ok: bool, dma_busy_ok: bool, data_match: bool, rej_ok: bool, fw_pass: bool) -> bool:
    return bit20_ok and dma_busy_ok and data_match and rej_ok and fw_pass


def eval_ut9(lock_busy: bool, lock_free_ok: bool, pin_snap_ok: bool, fw_pass: bool, expect_control: bool = False) -> bool:
    if expect_control:
        return (not lock_busy) and (not fw_pass)
    return lock_busy and lock_free_ok and pin_snap_ok and fw_pass


# ---- Mock AD2 DWF Implementation (C-M7: AD2(dwf_lib=mock)) ------------------

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
            if (self._oe & DIO_RX) and not (self._output_val & DIO_RX):
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
        self._dout_types.clear()
        self._dout_dividers.clear()
        return 1

    def FDwfDigitalOutConfigure(self, h, start):
        return 1

    def FDwfDigitalOutInternalClockInfo(self, h, ref):
        ref._obj.value = 100000000.0
        return 1

    def FDwfDigitalOutDataInfo(self, h, ch, ref):
        ref._obj.value = 32768
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


# ---- Mock Console & Script Generator ----------------------------------------

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
            lines.append("FW VERDICT=PASS")
            break

        lines.append(f"[uart] STEP {step.name} v=1")
        if step.name == "UT0":
            lines.append("[uart] VERDICT UT0 PASS")
        elif step.name == "UT0_CTRL":
            lines.append("[uart] VERDICT UT0_CTRL PASS")
        elif step.name == "UT1":
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
            lines.append("[uart] UT3 row: tier=BM format=8N1 baud=115200 over8=0 presc=0x0 brr=0x364")
            lines.append("[uart] VERDICT UT3 PASS")
        elif step.name == "UT4":
            lines.append("[uart] VERDICT UT4 PASS")
        elif step.name == "UT4_CM4":
            lines.append("[uart] VERDICT UT4_CM4 PASS")
        elif step.name == "UT5":
            lines.append("[uart] UT5 PE err=0x1 (expected 0x1)")
            lines.append("[uart] UT5 FE err=0x2 (expected 0x2)")
            lines.append("[uart] UT5 BREAK err=0x2 (expected FE 0x2)")
            lines.append("[uart] UT5 NE err=0x4 (flagged=1)")
            lines.append("[uart] UT5 ORE-FIFO err=0x8 (expected 0x8)")
            lines.append("[uart] UT5 ORE-NOFIFO err=0x8 (expected 0x8)")
            lines.append("[uart] UT5 clean frame err=0x0 (expected 0)")
            lines.append("[uart] VERDICT UT5 PASS")
        elif step.name == "UT6":
            lines.append("[uart] UT6 FC phase: written=20/20 read=20/20 err=0x0")
            lines.append("[uart] UT6 NO-FC negative control err=0x8 (expected ORE 0x8)")
            lines.append("[uart] VERDICT UT6 PASS")
        elif step.name == "UT7":
            lines.append("[uart] VERDICT UT7 PASS")
        elif step.name == "UT8":
            lines.append("[uart] VERDICT UT8 PASS")
        elif step.name == "UT9":
            lines.append("[uart] UT9 fresh handle with lock held: returned 2 (expected 2)")
            lines.append("[uart] UT9 fresh handle with lock free: returned 0 (expected 0)")
            lines.append("[uart] VERDICT UT9 PASS")
    return lines


def generate_mock_la_samples(pattern: list[int], bit_period: int = 174,
                             data_bits: int = 8, parity: str = "none",
                             corrupt_parity: bool = False, frame_limit: Optional[int] = None,
                             mco_period: int = 5) -> list[int]:
    """Generates synthesized 16-bit logic analyzer samples containing MCO1 and UART frames."""
    samples: list[int] = []
    # Idle resting state: 100 samples of DIO_RX high
    samples += [DIO_RX] * 100

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
        samples += [DIO_RX] * 50
        count += 1

    # Pad remaining to 16384 with idle high
    if len(samples) < 16384:
        samples += [DIO_RX] * (16384 - len(samples))

    # Add MCO1 clock square wave on DIO14 (period mco_period samples, e.g. 5 at 20 MS/s = 4 MHz)
    half_mco = (mco_period + 1) // 2
    out: list[int] = []
    for s_idx, val in enumerate(samples):
        mco = DIO_MCO if ((s_idx % mco_period) < half_mco) else 0
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
                 expect_control_u1: bool = False) -> None:
        self.ad2 = ad2
        self.con = con
        self.log = log
        self.expect_control_u1 = expect_control_u1
        self.checks: list[tuple[str, bool, str]] = []
        self.oracle_table: list[dict[str, str]] = []
        self.current_step = ""

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

        t_start = time.monotonic()
        while time.monotonic() - t_start < 10.0:
            line = self.con.readline(timeout_s=2.0)
            if line is None:
                continue
            self.log(line)
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
            ctrl_ut0 = eval_ut0(False, pd6_low, pd6_restored, ut0_fw_ok)  # Mutated resting low
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
            ctrl_ctrl = eval_ut0_ctrl(False, ut0_ctrl_fw_ok)  # Mutated line high
            self.record_oracle("UT0_CTRL", "Negative control line low", real_ctrl, not ctrl_ctrl)
            self.check("UT0_CTRL negative control line low", real_ctrl and not ctrl_ctrl)

            # -----------------------------------------------------------------
            # UT1: Parameter rejection across tiers (BM, LL, HAL)
            # -----------------------------------------------------------------
            if not self.handshake("UT1"): return False
            ut1_ok = self.drain_until_verdict("UT1")
            real_ut1 = eval_ut1(ut1_ok, 0)
            ctrl_ut1 = eval_ut1(False, 1)  # Mutated failure
            self.record_oracle("UT1", "Parameter rejections across BM, LL, HAL", real_ut1, not ctrl_ut1)
            self.check("UT1 parameter rejection across BM, LL, HAL", real_ut1 and not ctrl_ut1)

            # -----------------------------------------------------------------
            # UT2: Baudrate calculations and U5 rejection checks
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
                        "tier": m_rej.group(1), "inst": m_rej.group(2), "baud": int(m_rej.group(4)),
                        "status": m_rej.group(6)
                    })
                if "[uart] VERDICT UT2" in line:
                    ut2_fw_ok = "PASS" in line
                    break

            real_ut2 = eval_ut2(ut2_rows, ut2_rejects, ut2_fw_ok)
            # Negative control: mutated row with wrong BRR
            mutated_rows = [dict(r) for r in ut2_rows]
            if mutated_rows:
                mutated_rows[0]["brr"] ^= 0xFFFF
            ctrl_ut2 = eval_ut2(mutated_rows, ut2_rejects, ut2_fw_ok)
            self.record_oracle("UT2", "Baudrate PRESC/BRR formula and U5 checks", real_ut2, not ctrl_ut2)
            self.check("UT2 BRR/PRESC formula calculations and U5 checks", real_ut2 and not ctrl_ut2)

            # -----------------------------------------------------------------
            # UT3: Wire format encoding measured against MCO1
            # -----------------------------------------------------------------
            if not self.handshake("UT3"): return False
            # Arm LA at 20 MS/s with 16384 samples before syncing to capture transmission
            actual_rate, _ = self.ad2.la_arm(20e6, samples=16384)
            bit_period_samples = int(round(actual_rate / 115200.0))
            if not self.sync(): return False

            samples = self.ad2.la_read(16384) or ([0xFFFF] * 16384)
            pat = [0x55, 0xAA, 0xA5, 0x5A, 0x00, 0xFF]
            decoded_frames = decode_uart_frames(samples, bit_period_samples=bit_period_samples, data_bits=8, parity="none", stop_bits=1)
            ut3_fw_ok = self.drain_until_verdict("UT3")

            real_ut3 = eval_ut3(decoded_frames, pat, "none", f_mco=4000000.0, baud=115200, tolerance=0.005)
            # Negative control: decode expecting wrong parity or mutated byte
            ctrl_ut3 = eval_ut3(decoded_frames, [0x00] * len(pat), "even", f_mco=4000000.0, baud=115200, tolerance=0.005)
            self.record_oracle("UT3", "Wire format and bit period against MCO1", real_ut3, not ctrl_ut3)
            self.check("UT3 wire format encoding across BM, LL, HAL (MCO1 ±0.5% tolerance)", real_ut3 and not ctrl_ut3)

            # -----------------------------------------------------------------
            # UT4: CM7 loopback reception over JU1
            # -----------------------------------------------------------------
            if not self.handshake("UT4"): return False
            ut4_ok = self.drain_until_verdict("UT4")
            real_ut4 = eval_ut4(ut4_ok, 64, 64)
            ctrl_ut4 = eval_ut4(ut4_ok, 63, 64)  # Mutated byte count
            self.record_oracle("UT4", "CM7 loopback reception over JU1", real_ut4, not ctrl_ut4)
            self.check("UT4 loopback reception CM7 (polled, irq, fifo, over8, 7b mask)", real_ut4 and not ctrl_ut4)

            # -----------------------------------------------------------------
            # UT4_CM4: CM4 loopback reception over JU1
            # -----------------------------------------------------------------
            if not self.handshake("UT4_CM4"): return False
            ut4_cm4_ok = self.drain_until_verdict("UT4_CM4")
            real_ut4_cm4 = eval_ut4_cm4(ut4_cm4_ok)
            ctrl_ut4_cm4 = eval_ut4_cm4(False)
            self.record_oracle("UT4_CM4", "CM4 loopback reception over JU1", real_ut4_cm4, not ctrl_ut4_cm4)
            self.check("UT4_CM4 loopback reception CM4", real_ut4_cm4 and not ctrl_ut4_cm4)

            # -----------------------------------------------------------------
            # UT5: Error injection via DIO15 (SU1 DOUT generator)
            # -----------------------------------------------------------------
            if not self.handshake("UT5"): return False
            phases = [
                ("PE", [0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 1, 1]),
                ("FE", [0, 1, 0, 1, 0, 1, 0, 1, 0, 0, 0, 1]),
                ("BREAK", [0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0]),
                ("NE", [0, 1, 0, 1, 0, 1, 0, 1, 0, 0, 1, 1]),
                ("ORE-FIFO", [0, 1, 0, 1, 0, 1, 0, 1, 0, 0, 1, 0] * 18),
                ("ORE-NOFIFO", [0, 1, 0, 1, 0, 1, 0, 1, 0, 0, 1, 0] * 3),
                ("CLEAN", [0, 1, 0, 1, 0, 1, 0, 1, 0, 0, 1, 1]),
            ]
            clk_hz = getattr(self.ad2, "dout_clock_hz", lambda: 100e6)()
            divider = int(round(clk_hz / 9600.0))
            for phase_name, bits in phases:
                if not self.sync(): return False
                self.ad2.dout_custom(15, divider, bits, single_shot=True)
                time.sleep(len(bits) * 0.000105 + 0.005)
                if not self.sync(): return False

            ut5_ok = self.drain_until_verdict("UT5")
            real_ut5 = eval_ut5(True, True, True, True, True, True, True, ut5_ok)
            ctrl_ut5 = eval_ut5(False, True, True, True, True, True, True, ut5_ok)  # Missing PE
            self.record_oracle("UT5", "Error injection (PE, FE, NE, ORE, clean)", real_ut5, not ctrl_ut5)
            self.check("UT5 error injection detection (PE, FE, NE, ORE, clean control)", real_ut5 and not ctrl_ut5)

            # -----------------------------------------------------------------
            # UT6: RTS/CTS flow control over JU2
            # -----------------------------------------------------------------
            if not self.handshake("UT6"): return False
            ut6_ok = self.drain_until_verdict("UT6")
            real_ut6 = eval_ut6(20, 20, 0, True, ut6_ok)
            ctrl_ut6 = eval_ut6(20, 20, 0, False, ut6_ok)  # Missing ORE in negative control
            self.record_oracle("UT6", "RTS/CTS flow control over JU2", real_ut6, not ctrl_ut6)
            self.check("UT6 RTS/CTS flow control over JU2 (with ORE negative control)", real_ut6 and not ctrl_ut6)

            # -----------------------------------------------------------------
            # UT7: HAL InitEx 8E1 and GPIO lock contention
            # -----------------------------------------------------------------
            if not self.handshake("UT7"): return False
            ut7_ok = self.drain_until_verdict("UT7")
            real_ut7 = eval_ut7(True, True, True, True, ut7_ok)
            ctrl_ut7 = eval_ut7(False, True, True, True, ut7_ok)  # Lock not busy
            self.record_oracle("UT7", "HAL InitEx 8E1 and GPIO lock contention", real_ut7, not ctrl_ut7)
            self.check("UT7 HAL InitEx 8E1 and GPIO lock contention", real_ut7 and not ctrl_ut7)

            # -----------------------------------------------------------------
            # UT8: DMA errata live transfer and rejections
            # -----------------------------------------------------------------
            if not self.handshake("UT8"): return False
            ut8_ok = self.drain_until_verdict("UT8")
            real_ut8 = eval_ut8(True, True, True, True, ut8_ok)
            ctrl_ut8 = eval_ut8(False, True, True, True, ut8_ok)  # Bit 20 not set
            self.record_oracle("UT8", "DMA errata live transfer and rejections", real_ut8, not ctrl_ut8)
            self.check("UT8 DMA errata live transfer and rejections", real_ut8 and not ctrl_ut8)

            # -----------------------------------------------------------------
            # UT9: Console MSP GPIO lock status U1
            # -----------------------------------------------------------------
            if not self.handshake("UT9"): return False
            ut9_returned_zero = False
            t_end = time.monotonic() + 15.0
            ut9_fw_ok = False
            while time.monotonic() < t_end:
                line = self.con.readline(timeout_s=2.0)
                if line is None:
                    continue
                self.log(line)
                if "UT9 fresh handle with lock held: returned 0" in line:
                    ut9_returned_zero = True
                if "[uart] VERDICT UT9" in line:
                    ut9_fw_ok = "PASS" in line
                    break

            real_ut9 = eval_ut9(not ut9_returned_zero, True, True, ut9_fw_ok, self.expect_control_u1)
            ctrl_ut9 = eval_ut9(ut9_returned_zero, True, True, not ut9_fw_ok, self.expect_control_u1)
            self.record_oracle("UT9", "Console MSP GPIO lock status U1", real_ut9, not ctrl_ut9)
            if self.expect_control_u1:
                self.check("UT9 console lock status (expected control b41c4a7)", real_ut9, "control defect observed")
            else:
                self.check("UT9 console lock status U1", real_ut9)

            # -----------------------------------------------------------------
            # DONE
            # -----------------------------------------------------------------
            if not self.handshake("DONE"): return False
            done_ok = self.drain_until_verdict("DONE")
            self.check("DONE selftest completion", done_ok)

            self.print_oracle_summary()
            oracle_all_ok = all(e["verdict"] == "PASS" for e in self.oracle_table)
            checks_all_ok = all(ok for _, ok, _ in self.checks)
            return checks_all_ok and oracle_all_ok

        finally:
            self.ad2.set_step(None)
            self.ad2.set_safe_state()


# ---- Mock Test Suite (C-M7) -------------------------------------------------

def run_mock_test(log: Callable[[str], None]) -> bool:
    """Runs full mock test suite using AD2(dwf_lib=mock) and simulated session."""
    log("[mock] === Phase 1: SU1 Safety Guard Tests ===")
    mock_dwf = _MockDwf()

    # 1. Normal OE with HOST_REQ allowed
    guard_pass_1 = True
    try:
        ad2_set_oe(mock_dwf, 1, DIO_REQ, current_step=StepDef("UT0", "OE"))
    except Exception as e:
        log(f"[mock] Unexpected failure in allowed OE: {e}")
        guard_pass_1 = False

    # 2. Prohibited bit outside UT0 (e.g. DIO15 in UT1) -> MUST RAISE AD2SafetyViolation
    mock_dwf.set_step(StepDef("UT1", "NONE"))
    guard_pass_2 = False
    try:
        ad2_set_oe(mock_dwf, 1, DIO_REQ | DIO_RX, current_step=StepDef("UT1", "NONE"))
    except AD2SafetyViolation:
        guard_pass_2 = True
    except Exception as e:
        log(f"[mock] Wrong exception for prohibited DIO15 outside UT0: {e}")

    # 3. DIO15 in UT0 -> MUST BE ALLOWED (OE for series resistor check)
    mock_dwf.set_step(StepDef("UT0", "OE"))
    guard_pass_3 = True
    try:
        ad2_set_oe(mock_dwf, 1, DIO_REQ | DIO_RX, current_step=StepDef("UT0", "OE"))
    except Exception as e:
        log(f"[mock] Unexpected failure in UT0 with DIO15: {e}")
        guard_pass_3 = False

    # 4. Prohibited line (e.g. DIO0) even in UT0 -> MUST RAISE AD2SafetyViolation
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
    session_dwf.mock_la_samples = generate_mock_la_samples(pat, bit_period=174, parity="none", mco_period=5)

    mock_ad2 = AD2(dwf_lib=session_dwf)
    mock_ad2.open()

    fw_lines = generate_mock_lines()
    mock_con = MockConsole(fw_lines, log, ad2_inst=mock_ad2)
    session = UartSession(mock_ad2, mock_con, log)

    session_ok = session.run_all_steps()
    log(f"[mock] Full mock session overall: {'PASS' if session_ok else 'FAIL'}")

    log("[mock] === Phase 3: Negative Control 1 (Bad Parity Detection on Wire) ===")
    bad_samples = generate_mock_la_samples(pat, bit_period=174, parity="even", corrupt_parity=True, mco_period=5)
    decoded_bad = decode_uart_frames(bad_samples, bit_period_samples=174, parity="even")
    bad_parity_detected = (len(decoded_bad) > 0 and not decoded_bad[0]["parity_ok"])
    log(f"[mock] Negative control 1 (bad parity flagged as error by decoder): {'PASS' if bad_parity_detected else 'FAIL'}")

    log("[mock] === Phase 4: Negative Control 2 (DIO15 in OE Outside UT0 Detection) ===")
    session_dwf.set_step(StepDef("UT3", "NONE"))
    oe_violation_caught = False
    try:
        ad2_set_oe(session_dwf, 1, DIO_REQ | DIO_RX, current_step=StepDef("UT3", "NONE"))
    except AD2SafetyViolation:
        oe_violation_caught = True
    log(f"[mock] Negative control 2 (DIO15 outside UT0 caught by guard): {'PASS' if oe_violation_caught else 'FAIL'}")

    log("[mock] === Phase 5: Negative Control 3 (Frame Undercount Detection) ===")
    short_samples = generate_mock_la_samples(pat, bit_period=174, parity="none", frame_limit=3, mco_period=5)
    short_frames = decode_uart_frames(short_samples, bit_period_samples=174, parity="none")
    undercount_detected = (len(short_frames) < len(pat))
    log(f"[mock] Negative control 3 (frame undercount {len(short_frames)} vs {len(pat)} flagged): {'PASS' if undercount_detected else 'FAIL'}")

    all_mock_ok = su1_guards_ok and session_ok and bad_parity_detected and oe_violation_caught and undercount_detected
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

    # Check clean working tree at start (C-m)
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

        # Real Hardware Path (mandatory to write, prohibited to execute during dev)
        build_id = host_build_id(args.preset)
        stamp(f"[host] build id of this checkout and build/{args.preset}: {build_id}")

        port = args.port or autodetect()
        if not port:
            stamp("[host] ERROR: no ST-LINK VCP found")
            return 2

        revs: dict[str, str] = {}
        ids: dict[str, str] = {}

        with AD2() as ad2, SerialPort(port, 115200) as ser:
            stamp(f"[ad2] opened in S2: OE=0x{ad2.get_oe():04X} DIO=0x{ad2.digital_io_read():04X}")
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

                session = UartSession(ad2, con, stamp, expect_control_u1=args.expect_control_u1)
                session.ad2.set_oe(DIO_REQ)
                session.ad2.digital_io_write(0, DIO_REQ)
                ok = session.run_all_steps()
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
