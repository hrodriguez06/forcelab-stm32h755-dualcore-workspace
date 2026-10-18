#!/usr/bin/env python3
# Copyright (c) 2026 STMicroelectronics & Antigravity
# All rights reserved.
#
# Hardware check host for 01_led selftest (preset selftest-led).
# Evaluates steps LT1 - LT5 on live board or via mock session.
#
# Standard library + monitor.py (ctypes Win32 serial API) only.
# No AD2 required.

from __future__ import annotations

import argparse
import copy
import dataclasses
import hashlib
import os
import pathlib
import re
import shutil
import subprocess
import sys
import time
from typing import Any, Callable, Optional

# Root directory setup
TOOL_DIR = pathlib.Path(__file__).resolve().parent
PROJECT = TOOL_DIR.parent
WORKSPACE = PROJECT.parent.parent

sys.path.insert(0, str(WORKSPACE / "tools"))
try:
    from monitor import SerialPort, autodetect, port_for_sn
except ImportError:
    SerialPort = None
    autodetect = None
    port_for_sn = None

# Hardware target defaults (NUCLEO-H755ZI-Q ST-LINK serial number cited from plan/session)
DEFAULT_SN = "0016000C4142501220353451"
DEFAULT_BAUD = 115200


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
        if "build" in p.parts:
            continue
        if "docs" in p.parts and p.name.startswith("led_dev_"):
            continue
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
        cache_path = project / "build" / preset / "CMakeCache.txt"
        if not cache_path.is_file():
            return "unknown"
        cache = cache_path.read_bytes()
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


# ---- Data Structures for Captured Step Data ---------------------------------

@dataclasses.dataclass
class PinInfo:
    mode: int
    otype: int
    speed: int
    pull: int
    odr: int

@dataclasses.dataclass
class Lt1Data:
    mask: int
    lock: int
    unlock: int
    pins: dict[str, PinInfo]
    ctrl_mask: int
    ctrl_regs_equal: int
    ctrl_odr_g: int
    ctrl_odr_y: int
    ctrl_odr_r: int

@dataclasses.dataclass
class Lt2Data:
    mask: int
    init_equal: int
    ops_equal: int
    ctrl_mask: int
    ctrl_pb0_mode_before: int
    ctrl_pb0_mode_after: int
    ctrl_gpiob_changed: int

@dataclasses.dataclass
class Lt3Data:
    n: int
    freq_hz: int
    window_isrs: int
    lost: int
    ctrl_n: int
    ctrl_freq_hz: int
    ctrl_window_isrs: int
    ctrl_lost: int

@dataclasses.dataclass
class Lt4Data:
    red_mode: int
    red_odr: int
    ctrl_green_unchanged: int
    ctrl_green_mode: int
    ctrl_green_odr: int

@dataclasses.dataclass
class Lt5Data:
    tim17_stopped: int
    pins: dict[str, PinInfo]

@dataclasses.dataclass
class SelftestRunData:
    rev: str
    build_id: str
    lt1: Optional[Lt1Data] = None
    lt2: Optional[Lt2Data] = None
    lt3: Optional[Lt3Data] = None
    lt4: Optional[Lt4Data] = None
    lt5: Optional[Lt5Data] = None
    fw_verdict: str = ""


# ---- Step Evaluation Oracles -----------------------------------------------

def eval_lt1(d: Lt1Data) -> tuple[bool, str]:
    """Evaluates LT1: L3 validation, section, cross-core hook, read-back."""
    errors = []
    # Real checks
    if d.mask != 0x07:
        errors.append(f"real mask != 0x07 (got 0x{d.mask:02X})")
    if d.lock < 3:
        errors.append(f"real lock count < 3 (got {d.lock})")
    if d.lock != d.unlock:
        errors.append(f"real lock != unlock ({d.lock} != {d.unlock})")
    for name in ("G", "Y", "R"):
        p = d.pins.get(name)
        if not p:
            errors.append(f"missing pin info for {name}")
            continue
        if p.mode != 1:
            errors.append(f"pin {name} mode != 1 (got {p.mode})")
        if p.otype != 0:
            errors.append(f"pin {name} otype != 0 (got {p.otype})")
        if p.speed != 0:
            errors.append(f"pin {name} speed != 0 (got {p.speed})")
        if p.pull != 0:
            errors.append(f"pin {name} pull != 0 (got {p.pull})")
        if p.odr != 0:
            errors.append(f"pin {name} odr != 0 (got {p.odr})")

    # Control checks
    if d.ctrl_mask != 0x00:
        errors.append(f"ctrl mask != 0x00 (got 0x{d.ctrl_mask:02X})")
    if d.ctrl_regs_equal != 1:
        errors.append(f"ctrl regs_equal != 1 (got {d.ctrl_regs_equal})")
    if d.ctrl_odr_g != 0 or d.ctrl_odr_y != 0 or d.ctrl_odr_r != 0:
        errors.append(f"ctrl odr changed ({d.ctrl_odr_g},{d.ctrl_odr_y},{d.ctrl_odr_r})")

    if errors:
        return False, "; ".join(errors)
    return True, f"mask=0x{d.mask:02X} lock={d.lock}/{d.unlock} pins=OK ctrl=OK"


def eval_lt2(d: Lt2Data) -> tuple[bool, str]:
    """Evaluates LT2: L5 invalid port/pin handling and rejection."""
    errors = []
    # Real checks: bad table completely rejected
    if d.mask != 0x00:
        errors.append(f"real mask != 0x00 (got 0x{d.mask:02X})")
    if d.init_equal != 1:
        errors.append(f"real init_equal != 1 (got {d.init_equal})")
    if d.ops_equal != 1:
        errors.append(f"real ops_equal != 1 (got {d.ops_equal})")

    # Control checks: semi-bad table accepts green only and alters PB0
    if d.ctrl_mask != 0x01:
        errors.append(f"ctrl mask != 0x01 (got 0x{d.ctrl_mask:02X})")
    if d.ctrl_pb0_mode_before != 3:
        errors.append(f"ctrl pb0_mode_before != 3 (got {d.ctrl_pb0_mode_before})")
    if d.ctrl_pb0_mode_after != 1:
        errors.append(f"ctrl pb0_mode_after != 1 (got {d.ctrl_pb0_mode_after})")
    if d.ctrl_gpiob_changed != 1:
        errors.append(f"ctrl gpiob_changed != 1 (got {d.ctrl_gpiob_changed})")

    if errors:
        return False, "; ".join(errors)
    return True, f"mask=0x{d.mask:02X} init_equal={d.init_equal} ops_equal={d.ops_equal} ctrl=OK"


def eval_lt3(d: Lt3Data) -> tuple[bool, str]:
    """Evaluates LT3: L4 atomic LED_Toggle vs periodic TIM17 ISR race condition."""
    errors = []
    if d.n < 100000:
        errors.append(f"N too small ({d.n} < 100000)")
    if d.freq_hz < 100000:
        errors.append(f"freq_hz too small ({d.freq_hz} < 100000)")
    if d.window_isrs == 0:
        errors.append("real window_isrs == 0 (timer ISR did not run)")
    if d.lost != 0:
        errors.append(f"real lost != 0 (got {d.lost})")

    # Control checks: old non-atomic toggle MUST lose updates
    if d.ctrl_window_isrs == 0:
        errors.append("ctrl window_isrs == 0 (timer ISR did not run)")
    if d.ctrl_lost == 0:
        errors.append("ctrl lost == 0 (control failed to demonstrate race condition)")

    if errors:
        return False, "; ".join(errors)
    return True, f"N={d.n} freq={d.freq_hz} window_isrs={d.window_isrs} lost={d.lost} ctrl_lost={d.ctrl_lost}"


def eval_lt4(d: Lt4Data) -> tuple[bool, str]:
    """Evaluates LT4: G14 LED_PanicRed exception direct RMW."""
    errors = []
    if d.red_mode != 1:
        errors.append(f"red_mode != 1 (got {d.red_mode})")
    if d.red_odr != 1:
        errors.append(f"red_odr != 1 (got {d.red_odr})")
    if d.ctrl_green_unchanged != 1:
        errors.append(f"ctrl_green_unchanged != 1 (got {d.ctrl_green_unchanged})")
    if d.ctrl_green_mode != 1:
        errors.append(f"ctrl_green_mode != 1 (got {d.ctrl_green_mode})")
    if d.ctrl_green_odr != 0:
        errors.append(f"ctrl_green_odr != 0 (got {d.ctrl_green_odr})")

    if errors:
        return False, "; ".join(errors)
    return True, f"red_mode={d.red_mode} red_odr={d.red_odr} ctrl_green_unchanged={d.ctrl_green_unchanged}"


def eval_lt5(d: Lt5Data) -> tuple[bool, str]:
    """Evaluates LT5: Final state restoration."""
    errors = []
    if d.tim17_stopped != 1:
        errors.append(f"tim17_stopped != 1 (got {d.tim17_stopped})")
    for name in ("G", "Y", "R"):
        p = d.pins.get(name)
        if not p:
            errors.append(f"missing pin info for {name}")
            continue
        if p.mode != 1:
            errors.append(f"pin {name} mode != 1 (got {p.mode})")
        if p.otype != 0:
            errors.append(f"pin {name} otype != 0 (got {p.otype})")
        if p.speed != 0:
            errors.append(f"pin {name} speed != 0 (got {p.speed})")
        if p.pull != 0:
            errors.append(f"pin {name} pull != 0 (got {p.pull})")
        if p.odr != 0:
            errors.append(f"pin {name} odr != 0 (got {p.odr})")

    if errors:
        return False, "; ".join(errors)
    return True, f"tim17_stopped={d.tim17_stopped} pins=OK"


# ---- Console Output Parsing -------------------------------------------------

def parse_console_lines(lines: list[str]) -> SelftestRunData:
    data = SelftestRunData(rev="unknown", build_id="unknown")

    for line in lines:
        line = line.strip()
        # [sys] build rev=3e03485-dirty id=e78987ed88727437
        m = re.search(r"\[sys\]\s+build\s+rev=(\S+)\s+id=(\S+)", line)
        if m:
            data.rev = m.group(1)
            data.build_id = m.group(2)
            continue

        # [led] LT1: mask=0x07 lock=3 unlock=3
        m = re.search(r"\[led\]\s+LT1:\s+mask=0x([0-9a-fA-F]+)\s+lock=(\d+)\s+unlock=(\d+)", line)
        if m:
            mask = int(m.group(1), 16)
            lock = int(m.group(2))
            unlock = int(m.group(3))
            if not data.lt1:
                data.lt1 = Lt1Data(mask=mask, lock=lock, unlock=unlock, pins={},
                                   ctrl_mask=0, ctrl_regs_equal=0, ctrl_odr_g=0, ctrl_odr_y=0, ctrl_odr_r=0)
            else:
                data.lt1.mask = mask
                data.lt1.lock = lock
                data.lt1.unlock = unlock
            continue

        # [led] LT1_PINS: G(mode=1,otype=0,speed=0,pull=0,odr=0) Y(...) R(...)
        m = re.search(r"\[led\]\s+LT1_PINS:\s+(.*)", line)
        if m:
            pins_str = m.group(1)
            pins = {}
            for pin_m in re.finditer(r"([GYR])\(mode=(\d+),otype=(\d+),speed=(\d+),pull=(\d+),odr=(\d+)\)", pins_str):
                pname = pin_m.group(1)
                pins[pname] = PinInfo(
                    mode=int(pin_m.group(2)),
                    otype=int(pin_m.group(3)),
                    speed=int(pin_m.group(4)),
                    pull=int(pin_m.group(5)),
                    odr=int(pin_m.group(6))
                )
            if not data.lt1:
                data.lt1 = Lt1Data(mask=0, lock=0, unlock=0, pins=pins,
                                   ctrl_mask=0, ctrl_regs_equal=0, ctrl_odr_g=0, ctrl_odr_y=0, ctrl_odr_r=0)
            else:
                data.lt1.pins = pins
            continue

        # [led] LT1_CTRL: mask=0x00 regs_equal=1 odr_g=0 odr_y=0 odr_r=0
        m = re.search(r"\[led\]\s+LT1_CTRL:\s+mask=0x([0-9a-fA-F]+)\s+regs_equal=(\d+)\s+odr_g=(\d+)\s+odr_y=(\d+)\s+odr_r=(\d+)", line)
        if m:
            if not data.lt1:
                data.lt1 = Lt1Data(mask=0, lock=0, unlock=0, pins={},
                                   ctrl_mask=int(m.group(1), 16),
                                   ctrl_regs_equal=int(m.group(2)),
                                   ctrl_odr_g=int(m.group(3)),
                                   ctrl_odr_y=int(m.group(4)),
                                   ctrl_odr_r=int(m.group(5)))
            else:
                data.lt1.ctrl_mask = int(m.group(1), 16)
                data.lt1.ctrl_regs_equal = int(m.group(2))
                data.lt1.ctrl_odr_g = int(m.group(3))
                data.lt1.ctrl_odr_y = int(m.group(4))
                data.lt1.ctrl_odr_r = int(m.group(5))
            continue

        # [led] LT2: mask=0x00 init_equal=1 ops_equal=1
        m = re.search(r"\[led\]\s+LT2:\s+mask=0x([0-9a-fA-F]+)\s+init_equal=(\d+)\s+ops_equal=(\d+)", line)
        if m:
            mask = int(m.group(1), 16)
            init_eq = int(m.group(2))
            ops_eq = int(m.group(3))
            if not data.lt2:
                data.lt2 = Lt2Data(mask=mask, init_equal=init_eq, ops_equal=ops_eq,
                                   ctrl_mask=0, ctrl_pb0_mode_before=0, ctrl_pb0_mode_after=0, ctrl_gpiob_changed=0)
            else:
                data.lt2.mask = mask
                data.lt2.init_equal = init_eq
                data.lt2.ops_equal = ops_eq
            continue

        # [led] LT2_CTRL: mask=0x01 pb0_mode_before=3 pb0_mode_after=1 gpiob_changed=1
        m = re.search(r"\[led\]\s+LT2_CTRL:\s+mask=0x([0-9a-fA-F]+)\s+pb0_mode_before=(\d+)\s+pb0_mode_after=(\d+)\s+gpiob_changed=(\d+)", line)
        if m:
            if not data.lt2:
                data.lt2 = Lt2Data(mask=0, init_equal=0, ops_equal=0,
                                   ctrl_mask=int(m.group(1), 16),
                                   ctrl_pb0_mode_before=int(m.group(2)),
                                   ctrl_pb0_mode_after=int(m.group(3)),
                                   ctrl_gpiob_changed=int(m.group(4)))
            else:
                data.lt2.ctrl_mask = int(m.group(1), 16)
                data.lt2.ctrl_pb0_mode_before = int(m.group(2))
                data.lt2.ctrl_pb0_mode_after = int(m.group(3))
                data.lt2.ctrl_gpiob_changed = int(m.group(4))
            continue

        # [led] LT3: N=200000 freq_hz=200000 window_isrs=6986 lost=0
        m = re.search(r"\[led\]\s+LT3:\s+N=(\d+)\s+freq_hz=(\d+)\s+window_isrs=(\d+)\s+lost=(\d+)", line)
        if m:
            n = int(m.group(1))
            f = int(m.group(2))
            w = int(m.group(3))
            lost = int(m.group(4))
            if not data.lt3:
                data.lt3 = Lt3Data(n=n, freq_hz=f, window_isrs=w, lost=lost,
                                   ctrl_n=0, ctrl_freq_hz=0, ctrl_window_isrs=0, ctrl_lost=0)
            else:
                data.lt3.n = n
                data.lt3.freq_hz = f
                data.lt3.window_isrs = w
                data.lt3.lost = lost
            continue

        # [led] LT3_CTRL: N=200000 freq_hz=200000 window_isrs=7531 lost=2152
        m = re.search(r"\[led\]\s+LT3_CTRL:\s+N=(\d+)\s+freq_hz=(\d+)\s+window_isrs=(\d+)\s+lost=(\d+)", line)
        if m:
            if not data.lt3:
                data.lt3 = Lt3Data(n=0, freq_hz=0, window_isrs=0, lost=0,
                                   ctrl_n=int(m.group(1)),
                                   ctrl_freq_hz=int(m.group(2)),
                                   ctrl_window_isrs=int(m.group(3)),
                                   ctrl_lost=int(m.group(4)))
            else:
                data.lt3.ctrl_n = int(m.group(1))
                data.lt3.ctrl_freq_hz = int(m.group(2))
                data.lt3.ctrl_window_isrs = int(m.group(3))
                data.lt3.ctrl_lost = int(m.group(4))
            continue

        # [led] LT4: red_mode=1 red_odr=1
        m = re.search(r"\[led\]\s+LT4:\s+red_mode=(\d+)\s+red_odr=(\d+)", line)
        if m:
            rm = int(m.group(1))
            ro = int(m.group(2))
            if not data.lt4:
                data.lt4 = Lt4Data(red_mode=rm, red_odr=ro,
                                   ctrl_green_unchanged=0, ctrl_green_mode=0, ctrl_green_odr=0)
            else:
                data.lt4.red_mode = rm
                data.lt4.red_odr = ro
            continue

        # [led] LT4_CTRL: green_unchanged=1 green_mode=1 green_odr=0
        m = re.search(r"\[led\]\s+LT4_CTRL:\s+green_unchanged=(\d+)\s+green_mode=(\d+)\s+green_odr=(\d+)", line)
        if m:
            if not data.lt4:
                data.lt4 = Lt4Data(red_mode=0, red_odr=0,
                                   ctrl_green_unchanged=int(m.group(1)),
                                   ctrl_green_mode=int(m.group(2)),
                                   ctrl_green_odr=int(m.group(3)))
            else:
                data.lt4.ctrl_green_unchanged = int(m.group(1))
                data.lt4.ctrl_green_mode = int(m.group(2))
                data.lt4.ctrl_green_odr = int(m.group(3))
            continue

        # [led] LT5: tim17_stopped=1
        m = re.search(r"\[led\]\s+LT5:\s+tim17_stopped=(\d+)", line)
        if m:
            ts = int(m.group(1))
            if not data.lt5:
                data.lt5 = Lt5Data(tim17_stopped=ts, pins={})
            else:
                data.lt5.tim17_stopped = ts
            continue

        # [led] LT5_PINS: G(...) Y(...) R(...)
        m = re.search(r"\[led\]\s+LT5_PINS:\s+(.*)", line)
        if m:
            pins_str = m.group(1)
            pins = {}
            for pin_m in re.finditer(r"([GYR])\(mode=(\d+),otype=(\d+),speed=(\d+),pull=(\d+),odr=(\d+)\)", pins_str):
                pname = pin_m.group(1)
                pins[pname] = PinInfo(
                    mode=int(pin_m.group(2)),
                    otype=int(pin_m.group(3)),
                    speed=int(pin_m.group(4)),
                    pull=int(pin_m.group(5)),
                    odr=int(pin_m.group(6))
                )
            if not data.lt5:
                data.lt5 = Lt5Data(tim17_stopped=0, pins=pins)
            else:
                data.lt5.pins = pins
            continue

        # [led] FW VERDICT=PASS
        m = re.search(r"\[led\]\s+FW\s+VERDICT=(\S+)", line)
        if m:
            data.fw_verdict = m.group(1)
            continue

    return data


# ---- Verification Pipeline --------------------------------------------------

def evaluate_run(data: SelftestRunData, log: Callable[[str], None],
                 expected_rev: str, expected_build_id: str,
                 allow_dirty: bool = False) -> bool:
    all_ok = True

    # 1. Provenance Checks
    rev_match = (data.rev == expected_rev) or (data.rev.startswith(expected_rev) and (allow_dirty or not data.rev.endswith("-dirty")))
    if not allow_dirty and data.rev.endswith("-dirty"):
        rev_match = False
    log(f"[host] CHECK provenance_git_rev: {'PASS' if rev_match else 'FAIL'} (fw_rev={data.rev}, host_head={expected_rev})")
    if not rev_match:
        all_ok = False

    id_match = (data.build_id == expected_build_id) or (allow_dirty and data.build_id != "unknown")
    log(f"[host] CHECK provenance_build_id: {'PASS' if id_match else 'FAIL'} (fw_id={data.build_id}, host_id={expected_build_id})")
    if not id_match and not allow_dirty:
        all_ok = False

    # 2. LT1 Evaluation & Mutations
    if not data.lt1:
        log("[host] CHECK lt1: FAIL (LT1 data missing)")
        all_ok = False
    else:
        ok1, msg1 = eval_lt1(data.lt1)
        log(f"[host] CHECK lt1: {'PASS' if ok1 else 'FAIL'} ({msg1})")
        if not ok1:
            all_ok = False

        # Mutation tests through eval_lt1
        mut_lt1_pull = copy.deepcopy(data.lt1)
        mut_lt1_pull.pins["G"].pull = 1
        m_ok, _ = eval_lt1(mut_lt1_pull)
        log(f"[host] CHECK lt1_mutation_pull_rejected: {'PASS' if not m_ok else 'FAIL'}")
        if m_ok:
            all_ok = False

        mut_lt1_mask = copy.deepcopy(data.lt1)
        mut_lt1_mask.mask = 0x06
        m_ok, _ = eval_lt1(mut_lt1_mask)
        log(f"[host] CHECK lt1_mutation_mask_rejected: {'PASS' if not m_ok else 'FAIL'}")
        if m_ok:
            all_ok = False

        mut_lt1_lock = copy.deepcopy(data.lt1)
        mut_lt1_lock.unlock = mut_lt1_lock.lock - 1
        m_ok, _ = eval_lt1(mut_lt1_lock)
        log(f"[host] CHECK lt1_mutation_lock_rejected: {'PASS' if not m_ok else 'FAIL'}")
        if m_ok:
            all_ok = False

    # 3. LT2 Evaluation & Mutations
    if not data.lt2:
        log("[host] CHECK lt2: FAIL (LT2 data missing)")
        all_ok = False
    else:
        ok2, msg2 = eval_lt2(data.lt2)
        log(f"[host] CHECK lt2: {'PASS' if ok2 else 'FAIL'} ({msg2})")
        if not ok2:
            all_ok = False

        mut_lt2_mask = copy.deepcopy(data.lt2)
        mut_lt2_mask.mask = 0x01
        m_ok, _ = eval_lt2(mut_lt2_mask)
        log(f"[host] CHECK lt2_mutation_mask_rejected: {'PASS' if not m_ok else 'FAIL'}")
        if m_ok:
            all_ok = False

        mut_lt2_init = copy.deepcopy(data.lt2)
        mut_lt2_init.init_equal = 0
        m_ok, _ = eval_lt2(mut_lt2_init)
        log(f"[host] CHECK lt2_mutation_init_rejected: {'PASS' if not m_ok else 'FAIL'}")
        if m_ok:
            all_ok = False

        mut_lt2_ctrl = copy.deepcopy(data.lt2)
        mut_lt2_ctrl.ctrl_gpiob_changed = 0
        m_ok, _ = eval_lt2(mut_lt2_ctrl)
        log(f"[host] CHECK lt2_mutation_ctrl_rejected: {'PASS' if not m_ok else 'FAIL'}")
        if m_ok:
            all_ok = False

    # 4. LT3 Evaluation & Mutations
    if not data.lt3:
        log("[host] CHECK lt3: FAIL (LT3 data missing)")
        all_ok = False
    else:
        ok3, msg3 = eval_lt3(data.lt3)
        log(f"[host] CHECK lt3: {'PASS' if ok3 else 'FAIL'} ({msg3})")
        if not ok3:
            all_ok = False

        # Mutation: real lost > 0
        mut_lt3_lost = copy.deepcopy(data.lt3)
        mut_lt3_lost.lost = 1
        m_ok, _ = eval_lt3(mut_lt3_lost)
        log(f"[host] CHECK lt3_mutation_lost_gt_0_rejected: {'PASS' if not m_ok else 'FAIL'}")
        if m_ok:
            all_ok = False

        # Mutation: control lost == 0
        mut_lt3_ctrl = copy.deepcopy(data.lt3)
        mut_lt3_ctrl.ctrl_lost = 0
        m_ok, _ = eval_lt3(mut_lt3_ctrl)
        log(f"[host] CHECK lt3_mutation_ctrl_lost_eq_0_rejected: {'PASS' if not m_ok else 'FAIL'}")
        if m_ok:
            all_ok = False

    # 5. LT4 Evaluation & Mutations
    if not data.lt4:
        log("[host] CHECK lt4: FAIL (LT4 data missing)")
        all_ok = False
    else:
        ok4, msg4 = eval_lt4(data.lt4)
        log(f"[host] CHECK lt4: {'PASS' if ok4 else 'FAIL'} ({msg4})")
        if not ok4:
            all_ok = False

        mut_lt4_odr = copy.deepcopy(data.lt4)
        mut_lt4_odr.red_odr = 0
        m_ok, _ = eval_lt4(mut_lt4_odr)
        log(f"[host] CHECK lt4_mutation_red_odr_rejected: {'PASS' if not m_ok else 'FAIL'}")
        if m_ok:
            all_ok = False

        mut_lt4_ctrl = copy.deepcopy(data.lt4)
        mut_lt4_ctrl.ctrl_green_unchanged = 0
        m_ok, _ = eval_lt4(mut_lt4_ctrl)
        log(f"[host] CHECK lt4_mutation_green_ctrl_rejected: {'PASS' if not m_ok else 'FAIL'}")
        if m_ok:
            all_ok = False

    # 6. LT5 Evaluation & Mutations
    if not data.lt5:
        log("[host] CHECK lt5: FAIL (LT5 data missing)")
        all_ok = False
    else:
        ok5, msg5 = eval_lt5(data.lt5)
        log(f"[host] CHECK lt5: {'PASS' if ok5 else 'FAIL'} ({msg5})")
        if not ok5:
            all_ok = False

        mut_lt5_tim = copy.deepcopy(data.lt5)
        mut_lt5_tim.tim17_stopped = 0
        m_ok, _ = eval_lt5(mut_lt5_tim)
        log(f"[host] CHECK lt5_mutation_tim17_running_rejected: {'PASS' if not m_ok else 'FAIL'}")
        if m_ok:
            all_ok = False

    # 7. Firmware Verdict Check
    fw_ok = (data.fw_verdict == "PASS")
    log(f"[host] CHECK fw_verdict: {'PASS' if fw_ok else 'FAIL'} (verdict={data.fw_verdict})")
    if not fw_ok:
        all_ok = False

    return all_ok


# ---- Mock Suite -------------------------------------------------------------

def generate_mock_lines(rev: str = "mock_rev", build_id: str = "mock_id") -> list[str]:
    return [
        " ",
        "=== STM32H755ZI 01_led driver selftest - CPU1 (Cortex-M7) ===",
        f"[sys] build rev={rev} id={build_id}",
        "[led] LT1: mask=0x07 lock=3 unlock=3",
        "[led] LT1_PINS: G(mode=1,otype=0,speed=0,pull=0,odr=0) Y(mode=1,otype=0,speed=0,pull=0,odr=0) R(mode=1,otype=0,speed=0,pull=0,odr=0)",
        "[led] LT1_CTRL: mask=0x00 regs_equal=1 odr_g=0 odr_y=0 odr_r=0",
        "[led] LT2: mask=0x00 init_equal=1 ops_equal=1",
        "[led] LT2_CTRL: mask=0x01 pb0_mode_before=3 pb0_mode_after=1 gpiob_changed=1",
        "[led] LT3: N=200000 freq_hz=200000 window_isrs=7000 lost=0",
        "[led] LT3_CTRL: N=200000 freq_hz=200000 window_isrs=7000 lost=2000",
        "[led] LT4: red_mode=1 red_odr=1",
        "[led] LT4_CTRL: green_unchanged=1 green_mode=1 green_odr=0",
        "[led] LT5: tim17_stopped=1",
        "[led] LT5_PINS: G(mode=1,otype=0,speed=0,pull=0,odr=0) Y(mode=1,otype=0,speed=0,pull=0,odr=0) R(mode=1,otype=0,speed=0,pull=0,odr=0)",
        "[led] FW VERDICT=PASS"
    ]


def run_mock_suite(log: Callable[[str], None]) -> bool:
    log("[mock] === Running Mock Host Evaluation Suite ===")
    nominal_lines = generate_mock_lines("mock_rev", "mock_id")
    nom_data = parse_console_lines(nominal_lines)

    nom_ok = evaluate_run(nom_data, log, expected_rev="mock_rev", expected_build_id="mock_id", allow_dirty=True)
    log(f"[mock] CHECK mock_nominal: {'PASS' if nom_ok else 'FAIL'}")

    # Mandatory Failure 1: lost > 0 in LT3 real run
    fail1_data = copy.deepcopy(nom_data)
    fail1_data.lt3.lost = 5
    f1_ok, _ = eval_lt3(fail1_data.lt3)
    f1_detected = (not f1_ok)
    log(f"[mock] CHECK mock_failure_lt3_real_lost_gt_0_detected: {'PASS' if f1_detected else 'FAIL'}")

    # Mandatory Failure 2: lost = 0 in LT3 control run
    fail2_data = copy.deepcopy(nom_data)
    fail2_data.lt3.ctrl_lost = 0
    f2_ok, _ = eval_lt3(fail2_data.lt3)
    f2_detected = (not f2_ok)
    log(f"[mock] CHECK mock_failure_lt3_ctrl_lost_eq_0_detected: {'PASS' if f2_detected else 'FAIL'}")

    # Mandatory Failure 3: pin in LT1 with pull
    fail3_data = copy.deepcopy(nom_data)
    fail3_data.lt1.pins["Y"].pull = 2
    f3_ok, _ = eval_lt1(fail3_data.lt1)
    f3_detected = (not f3_ok)
    log(f"[mock] CHECK mock_failure_lt1_pin_with_pull_detected: {'PASS' if f3_detected else 'FAIL'}")

    all_mock_ok = nom_ok and f1_detected and f2_detected and f3_detected
    verdict_str = "PASS" if all_mock_ok else "FAIL"
    log(f"[mock] OVERALL MOCK VERDICT: {verdict_str}")
    log(f"HOST VERDICT={verdict_str}")
    return all_mock_ok


# ---- Main Entry Point -------------------------------------------------------

def main() -> int:
    parser = argparse.ArgumentParser(description="Host check for 01_led selftest (preset selftest-led)")
    parser.add_argument("--sn", default=DEFAULT_SN,
                        help=f"ST-LINK serial number (default: {DEFAULT_SN}, NUCLEO-H755ZI-Q)")
    parser.add_argument("--port", default=None,
                        help="Virtual COM Port name (default: auto-detect via monitor.py / port_for_sn)")
    parser.add_argument("--out", default=None,
                        help="Output capture file path")
    parser.add_argument("--allow-dirty", action="store_true",
                        help="Allow dirty working tree or unverified build id (development only)")
    parser.add_argument("--mock", action="store_true",
                        help="Run mock evaluation suite without board")
    parser.add_argument("--preset", default="selftest-led",
                        help="CMake preset for build id resolution (default: selftest-led)")
    parser.add_argument("--no-reset", action="store_true",
                        help="Skip SWD reset (for testing already-running console)")
    args = parser.parse_args()

    lines: list[str] = []
    t0 = time.time()

    def stamp(msg: str) -> None:
        elapsed = time.time() - t0
        line = f"[+ {elapsed:7.3f}s] {msg}"
        print(line, flush=True)
        lines.append(line)

    stamp(f"[host] argv: {' '.join(sys.argv)}")
    stamp(f"[host] options: preset={args.preset} sn={args.sn} port={args.port} out={args.out} allow_dirty={args.allow_dirty} mock={args.mock}")

    out_path = pathlib.Path(args.out).resolve() if args.out else None

    # Handle --mock mode
    if args.mock:
        ok = run_mock_suite(stamp)
        if out_path:
            out_path.parent.mkdir(parents=True, exist_ok=True)
            with open(out_path, "w", encoding="utf-8") as f:
                for l in lines:
                    f.write(l + "\n")
            print(f"[host] Capture written to {out_path}")
        return 0 if ok else 1

    # Hardware Mode
    head_rev = git_rev()
    exp_id = host_build_id(args.preset, PROJECT)
    stamp(f"[host] git rev={head_rev} expected_build_id={exp_id}")

    # Check clean tree outside build/
    clean_tree_ok, clean_msg = check_clean_tree(WORKSPACE, out_path)
    stamp(f"[host] CHECK tree_clean: {'PASS' if clean_tree_ok else 'FAIL'} ({clean_msg})")
    if not clean_tree_ok and not args.allow_dirty:
        stamp("[host] ABORT: working tree is dirty without --allow-dirty")
        stamp("HOST VERDICT=FAIL")
        return 1

    # Resolve COM port
    port = args.port
    if not port:
        if port_for_sn and args.sn:
            port = port_for_sn(args.sn)
        if not port and autodetect:
            port = autodetect()
    if not port:
        stamp(f"[host] ERROR: could not detect COM port for ST-LINK SN {args.sn}")
        stamp("HOST VERDICT=FAIL")
        return 1
    stamp(f"[host] using serial port {port}")

    if SerialPort is None:
        stamp("[host] ERROR: monitor.py / Win32 SerialPort not available")
        stamp("HOST VERDICT=FAIL")
        return 1

    raw_fw_lines = []
    try:
        stamp(f"[host] opening serial port {port} at {DEFAULT_BAUD} baud")
        with SerialPort(port, DEFAULT_BAUD) as sp:
            if not args.no_reset:
                prog = cube_programmer()
                cmd = [prog, "-c", "port=SWD", f"sn={args.sn}", "mode=HOTPLUG", "-rst"]
                stamp(f"[host] triggering SWD reset: {' '.join(cmd)}")
                res = subprocess.run(cmd, capture_output=True, text=True)
                if res.returncode != 0:
                    cmd_alt = [prog, "-c", "port=SWD", f"sn={args.sn}", "-rst"]
                    stamp(f"[host] HOTPLUG reset failed ({res.returncode}), trying direct: {' '.join(cmd_alt)}")
                    res = subprocess.run(cmd_alt, capture_output=True, text=True)
                stamp(f"[host] reset returned rc={res.returncode}")

            # Capture console stream
            t_start = time.time()
            buf = b""
            while time.time() - t_start < 15.0:
                chunk = sp.read()
                if chunk:
                    buf += chunk
                    while b"\n" in buf:
                        raw_line, buf = buf.split(b"\n", 1)
                        line_str = raw_line.decode("utf-8", errors="replace").rstrip("\r")
                        raw_fw_lines.append(line_str)
                        stamp(f"[target] {line_str}")
                        if "FW VERDICT=" in line_str:
                            time.sleep(0.1)
                            # Read any trailing output
                            tail = sp.read()
                            if tail:
                                for l in tail.decode("utf-8", errors="replace").splitlines():
                                    l = l.rstrip("\r")
                                    if l:
                                        raw_fw_lines.append(l)
                                        stamp(f"[target] {l}")
                            break
                    if raw_fw_lines and any("FW VERDICT=" in l for l in raw_fw_lines):
                        break

    except Exception as e:
        stamp(f"[host] exception during capture: {e}")
        stamp("HOST VERDICT=FAIL")
        if out_path:
            out_path.parent.mkdir(parents=True, exist_ok=True)
            with open(out_path, "w", encoding="utf-8") as f:
                for l in lines:
                    f.write(l + "\n")
        return 1

    # Parse and evaluate
    run_data = parse_console_lines(raw_fw_lines)
    ok = evaluate_run(run_data, stamp, expected_rev=head_rev, expected_build_id=exp_id, allow_dirty=args.allow_dirty)

    verdict_str = "PASS" if ok else "FAIL"
    stamp(f"[host] OVERALL HARDWARE VERDICT: {verdict_str}")
    stamp(f"HOST VERDICT={verdict_str}")

    if out_path:
        out_path.parent.mkdir(parents=True, exist_ok=True)
        with open(out_path, "w", encoding="utf-8") as f:
            for l in lines:
                f.write(l + "\n")
        print(f"[host] Capture written to {out_path}")

    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
