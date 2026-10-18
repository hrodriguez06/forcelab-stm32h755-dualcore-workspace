#!/usr/bin/env python3
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

# Hardware target defaults (NUCLEO-H755ZI-Q ST-LINK serial number cited from IMPLEMENTER_INSTRUCTIONS.md §2.2)
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
class Lt1Pre:
    deinit_rc: tuple[int, int, int]
    read_rc: tuple[int, int, int]
    modes: tuple[int, int, int]

@dataclasses.dataclass
class Lt1Data:
    pre: Optional[Lt1Pre]
    mask: int
    lock: int
    unlock: int
    pins: dict[str, PinInfo]
    ctrl_mask: int
    ctrl_lock: int
    ctrl_unlock: int
    ctrl_deinit_rc: tuple[int, int, int]
    ctrl_modes: tuple[int, int, int]
    ctrl_odr: tuple[int, int, int]
    ctrl_snap_pre: list[int]
    ctrl_snap_post: list[int]

@dataclasses.dataclass
class Lt2Data:
    mask: int
    snap_pre: list[int]
    snap_post_init: list[int]
    snap_post_ops: list[int]
    ctrl_mask: int
    ctrl_snap_pre: list[int]
    ctrl_snap_post: list[int]

@dataclasses.dataclass
class Lt3Data:
    n: int
    dwt_cycles: int
    dwt_isrs: int
    cpu_hz: int
    window_isrs: int
    lost: int
    ctrl_n: int
    ctrl_window_isrs: int
    ctrl_lost: int

@dataclasses.dataclass
class Lt4Data:
    red_mode: int
    red_odr: int
    green_pre: Optional[PinInfo]
    green_post: Optional[PinInfo]

@dataclasses.dataclass
class Lt5Data:
    tim17_cr1: int
    tim17_dier: int
    tim17_nvic: int
    tim17_apb2: int
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

def eval_snapshot_equal(pre: list[int], post: list[int]) -> tuple[bool, str]:
    """Universal step oracle: verifies equality across 15 GPIO register words."""
    if len(pre) != 15 or len(post) != 15:
        return False, f"snapshot length mismatch: {len(pre)} vs {len(post)} (expected 15 words)"
    if pre != post:
        diffs = [f"word[{i}]: 0x{pre[i]:08X} != 0x{post[i]:08X}" for i in range(15) if pre[i] != post[i]]
        return False, f"snapshots differ in {len(diffs)} words ({', '.join(diffs[:2])})"
    return True, "15-word snapshots are identical"


def eval_pin_equal(pre: Optional[PinInfo], post: Optional[PinInfo]) -> tuple[bool, str]:
    """Universal pin oracle: checks whether all fields of a pin remain identical."""
    if pre is None or post is None:
        return False, "missing pin snapshot"
    if pre != post:
        return False, f"pin state changed: pre={pre} post={post}"
    return True, "pin state unchanged"


def eval_tim17_stopped(cr1: int, dier: int, nvic_pending: int, apb2enr: int) -> tuple[bool, str]:
    """Universal timer oracle: checks that TIM17 is stopped, DIER=0, NVIC=0, APB2ENR=0."""
    errors = []
    if (cr1 & 1) != 0:
        errors.append(f"TIM17 CR1 CEN bit is set (0x{cr1:04X})")
    if (dier & 1) != 0:
        errors.append(f"TIM17 DIER UIE bit is set (0x{dier:04X})")
    if nvic_pending != 0:
        errors.append(f"TIM17 NVIC pending is set ({nvic_pending})")
    if (apb2enr & 0x00040000) != 0:
        errors.append(f"TIM17 APB2 clock enable bit is set (0x{apb2enr:08X})")
    if errors:
        return False, "; ".join(errors)
    return True, "TIM17 stopped, DIER=0, NVIC=0, APB2=0"


def eval_lt1(d: Lt1Data) -> tuple[bool, str]:
    """Evaluates LT1: L3 validation, section, cross-core hook, read-back."""
    errors = []
    # Pre-state verification
    if not d.pre:
        errors.append("missing LT1 pre-deinit telemetry")
    else:
        if d.pre.deinit_rc != (0, 0, 0):
            errors.append(f"deinit_rc != (0,0,0) (got {d.pre.deinit_rc})")
        if d.pre.read_rc != (0, 0, 0):
            errors.append(f"read_rc != (0,0,0) (got {d.pre.read_rc})")
        if d.pre.modes != (3, 3, 3):
            errors.append(f"modes != analog (3,3,3) (got {d.pre.modes})")

    # Real init checks
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

    # Control checks: hook returned false -> mask 0, lock invoked, snapshots identical
    if d.ctrl_mask != 0x00:
        errors.append(f"ctrl mask != 0x00 (got 0x{d.ctrl_mask:02X})")
    if d.ctrl_lock < 1:
        errors.append(f"ctrl lock count < 1 (got {d.ctrl_lock})")
    if d.ctrl_odr != (0, 0, 0):
        errors.append(f"ctrl odr changed (got {d.ctrl_odr})")

    snap_ok, snap_msg = eval_snapshot_equal(d.ctrl_snap_pre, d.ctrl_snap_post)
    if not snap_ok:
        errors.append(f"ctrl snapshots modified: {snap_msg}")

    if errors:
        return False, "; ".join(errors)
    return True, f"mask=0x{d.mask:02X} lock={d.lock}/{d.unlock} pins=OK ctrl=OK"


def eval_lt2(d: Lt2Data) -> tuple[bool, str]:
    """Evaluates LT2: L5 invalid port/pin handling and rejection via 15-word snapshots."""
    errors = []
    # Real checks: bad table completely rejected
    if d.mask != 0x00:
        errors.append(f"real mask != 0x00 (got 0x{d.mask:02X})")

    ok_init, msg_init = eval_snapshot_equal(d.snap_pre, d.snap_post_init)
    if not ok_init:
        errors.append(f"real pre vs post_init mismatch: {msg_init}")

    ok_ops, msg_ops = eval_snapshot_equal(d.snap_pre, d.snap_post_ops)
    if not ok_ops:
        errors.append(f"real pre vs post_ops mismatch: {msg_ops}")

    # Control checks: semi-bad table accepts green only (mask=0x01) and ALTERS the snapshot
    if d.ctrl_mask != 0x01:
        errors.append(f"ctrl mask != 0x01 (got 0x{d.ctrl_mask:02X})")

    # Control snapshot equality check: MUST DIFFER (eval_snapshot_equal must return False)
    ctrl_equal, _ = eval_snapshot_equal(d.ctrl_snap_pre, d.ctrl_snap_post)
    if ctrl_equal:
        errors.append("ctrl snapshot pre vs post unexpectedly identical (hardware was not modified)")

    if errors:
        return False, "; ".join(errors)
    return True, f"mask=0x{d.mask:02X} init_snap=OK ops_snap=OK ctrl_snap=OK"


def eval_lt3(d: Lt3Data) -> tuple[bool, str]:
    """Evaluates LT3: L4 atomic LED_Toggle vs periodic TIM17 ISR race condition."""
    errors = []
    if d.n < 100000:
        errors.append(f"N too small ({d.n} < 100000)")

    # DWT-measured frequency check (A4)
    if d.dwt_cycles == 0:
        errors.append("dwt_cycles == 0 (cycle counter not running)")
        calc_freq = 0
    else:
        calc_freq = (d.dwt_isrs * d.cpu_hz) // d.dwt_cycles
    if calc_freq < 100000:
        errors.append(f"DWT measured frequency too low ({calc_freq} Hz < 100000 Hz)")

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
    return True, f"N={d.n} freq_calc={calc_freq}Hz window_isrs={d.window_isrs} lost={d.lost} ctrl_lost={d.ctrl_lost}"


def eval_lt4(d: Lt4Data) -> tuple[bool, str]:
    """Evaluates LT4: G14 LED_PanicRed exception direct RMW, green pin unchanged."""
    errors = []
    if d.red_mode != 1:
        errors.append(f"red_mode != 1 (got {d.red_mode})")
    if d.red_odr != 1:
        errors.append(f"red_odr != 1 (got {d.red_odr})")

    green_ok, green_msg = eval_pin_equal(d.green_pre, d.green_post)
    if not green_ok:
        errors.append(f"green pin altered: {green_msg}")

    if errors:
        return False, "; ".join(errors)
    return True, f"red_mode={d.red_mode} red_odr={d.red_odr} green_pin=UNCHANGED"


def eval_lt5(d: Lt5Data) -> tuple[bool, str]:
    """Evaluates LT5: Final state restoration."""
    errors = []
    tim_ok, tim_msg = eval_tim17_stopped(d.tim17_cr1, d.tim17_dier, d.tim17_nvic, d.tim17_apb2)
    if not tim_ok:
        errors.append(f"TIM17 not stopped: {tim_msg}")

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
    return True, f"{tim_msg} pins=OK"


# ---- Console Output Parsing -------------------------------------------------

def parse_console_lines(lines: list[str]) -> SelftestRunData:
    data = SelftestRunData(rev="unknown", build_id="unknown")

    # Snapshot dictionary storage during parsing
    snapshots: dict[str, list[int]] = {}

    for line in lines:
        line = line.strip()

        # [sys] build rev=3e03485-dirty id=e78987ed88727437
        m = re.search(r"\[sys\]\s+build\s+rev=(\S+)\s+id=(\S+)", line)
        if m:
            data.rev = m.group(1)
            data.build_id = m.group(2)
            continue

        # [led] LT1_PRE: deinit_rc=0,0,0 read_rc=0,0,0 modes=3,3,3
        m = re.search(r"\[led\]\s+LT1_PRE:\s+deinit_rc=(-?\d+),(-?\d+),(-?\d+)\s+read_rc=(-?\d+),(-?\d+),(-?\d+)\s+modes=(\d+),(\d+),(\d+)", line)
        if m:
            pre = Lt1Pre(
                deinit_rc=(int(m.group(1)), int(m.group(2)), int(m.group(3))),
                read_rc=(int(m.group(4)), int(m.group(5)), int(m.group(6))),
                modes=(int(m.group(7)), int(m.group(8)), int(m.group(9)))
            )
            if not data.lt1:
                data.lt1 = Lt1Data(pre=pre, mask=0, lock=0, unlock=0, pins={},
                                   ctrl_mask=0, ctrl_lock=0, ctrl_unlock=0,
                                   ctrl_deinit_rc=(0,0,0), ctrl_modes=(0,0,0), ctrl_odr=(0,0,0),
                                   ctrl_snap_pre=[], ctrl_snap_post=[])
            else:
                data.lt1.pre = pre
            continue

        # [led] LT1: mask=0x07 lock=3 unlock=3
        m = re.search(r"\[led\]\s+LT1:\s+mask=0x([0-9a-fA-F]+)\s+lock=(\d+)\s+unlock=(\d+)", line)
        if m:
            mask = int(m.group(1), 16)
            lock = int(m.group(2))
            unlock = int(m.group(3))
            if not data.lt1:
                data.lt1 = Lt1Data(pre=None, mask=mask, lock=lock, unlock=unlock, pins={},
                                   ctrl_mask=0, ctrl_lock=0, ctrl_unlock=0,
                                   ctrl_deinit_rc=(0,0,0), ctrl_modes=(0,0,0), ctrl_odr=(0,0,0),
                                   ctrl_snap_pre=[], ctrl_snap_post=[])
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
                data.lt1 = Lt1Data(pre=None, mask=0, lock=0, unlock=0, pins=pins,
                                   ctrl_mask=0, ctrl_lock=0, ctrl_unlock=0,
                                   ctrl_deinit_rc=(0,0,0), ctrl_modes=(0,0,0), ctrl_odr=(0,0,0),
                                   ctrl_snap_pre=[], ctrl_snap_post=[])
            else:
                data.lt1.pins = pins
            continue

        # [led] LT1_CTRL: mask=0x00 lock=1 unlock=0 deinit_rc=0,0,0 modes=3,3,3 odr_g=0 odr_y=0 odr_r=0
        m = re.search(r"\[led\]\s+LT1_CTRL:\s+mask=0x([0-9a-fA-F]+)\s+lock=(\d+)\s+unlock=(\d+)\s+deinit_rc=(-?\d+),(-?\d+),(-?\d+)\s+modes=(\d+),(\d+),(\d+)\s+odr_g=(\d+)\s+odr_y=(\d+)\s+odr_r=(\d+)", line)
        if m:
            mask = int(m.group(1), 16)
            lock = int(m.group(2))
            unlock = int(m.group(3))
            deinit_rc = (int(m.group(4)), int(m.group(5)), int(m.group(6)))
            modes = (int(m.group(7)), int(m.group(8)), int(m.group(9)))
            odrs = (int(m.group(10)), int(m.group(11)), int(m.group(12)))
            if not data.lt1:
                data.lt1 = Lt1Data(pre=None, mask=0, lock=0, unlock=0, pins={},
                                   ctrl_mask=mask, ctrl_lock=lock, ctrl_unlock=unlock,
                                   ctrl_deinit_rc=deinit_rc, ctrl_modes=modes, ctrl_odr=odrs,
                                   ctrl_snap_pre=[], ctrl_snap_post=[])
            else:
                data.lt1.ctrl_mask = mask
                data.lt1.ctrl_lock = lock
                data.lt1.ctrl_unlock = unlock
                data.lt1.ctrl_deinit_rc = deinit_rc
                data.lt1.ctrl_modes = modes
                data.lt1.ctrl_odr = odrs
            continue

        # Snapshot parsing helper: [led] <LABEL>_[ABE]: <w0> <w1> <w2> <w3> <w4>
        m = re.search(r"\[led\]\s+(\S+)_(A|B|E):\s+([0-9a-fA-F\s]+)", line)
        if m:
            label = m.group(1)
            port = m.group(2)
            words = [int(w, 16) for w in m.group(3).split()]
            if label not in snapshots:
                snapshots[label] = [0] * 15
            idx = 0 if port == "A" else (5 if port == "B" else 10)
            for i, w in enumerate(words):
                if idx + i < 15:
                    snapshots[label][idx + i] = w
            continue

        # [led] LT2: mask=0x00
        m = re.search(r"\[led\]\s+LT2:\s+mask=0x([0-9a-fA-F]+)", line)
        if m:
            mask = int(m.group(1), 16)
            if not data.lt2:
                data.lt2 = Lt2Data(mask=mask, snap_pre=[], snap_post_init=[], snap_post_ops=[],
                                   ctrl_mask=0, ctrl_snap_pre=[], ctrl_snap_post=[])
            else:
                data.lt2.mask = mask
            continue

        # [led] LT2_CTRL: mask=0x01
        m = re.search(r"\[led\]\s+LT2_CTRL:\s+mask=0x([0-9a-fA-F]+)", line)
        if m:
            mask = int(m.group(1), 16)
            if not data.lt2:
                data.lt2 = Lt2Data(mask=0, snap_pre=[], snap_post_init=[], snap_post_ops=[],
                                   ctrl_mask=mask, ctrl_snap_pre=[], ctrl_snap_post=[])
            else:
                data.lt2.ctrl_mask = mask
            continue

        # [led] LT3: N=200000 dwt_cycles=2000000 dwt_isrs=1000 cpu_hz=400000000 window_isrs=6986 lost=0
        m = re.search(r"\[led\]\s+LT3:\s+N=(\d+)\s+dwt_cycles=(\d+)\s+dwt_isrs=(\d+)\s+cpu_hz=(\d+)\s+window_isrs=(\d+)\s+lost=(\d+)", line)
        if m:
            n = int(m.group(1))
            cycles = int(m.group(2))
            isrs = int(m.group(3))
            cpu = int(m.group(4))
            w = int(m.group(5))
            lost = int(m.group(6))
            if not data.lt3:
                data.lt3 = Lt3Data(n=n, dwt_cycles=cycles, dwt_isrs=isrs, cpu_hz=cpu,
                                   window_isrs=w, lost=lost,
                                   ctrl_n=0, ctrl_window_isrs=0, ctrl_lost=0)
            else:
                data.lt3.n = n
                data.lt3.dwt_cycles = cycles
                data.lt3.dwt_isrs = isrs
                data.lt3.cpu_hz = cpu
                data.lt3.window_isrs = w
                data.lt3.lost = lost
            continue

        # [led] LT3_CTRL: N=200000 window_isrs=7225 lost=2155
        m = re.search(r"\[led\]\s+LT3_CTRL:\s+N=(\d+)\s+window_isrs=(\d+)\s+lost=(\d+)", line)
        if m:
            cn = int(m.group(1))
            cw = int(m.group(2))
            clost = int(m.group(3))
            if not data.lt3:
                data.lt3 = Lt3Data(n=0, dwt_cycles=0, dwt_isrs=0, cpu_hz=0,
                                   window_isrs=0, lost=0,
                                   ctrl_n=cn, ctrl_window_isrs=cw, ctrl_lost=clost)
            else:
                data.lt3.ctrl_n = cn
                data.lt3.ctrl_window_isrs = cw
                data.lt3.ctrl_lost = clost
            continue

        # [led] LT4: red_mode=1 red_odr=1
        m = re.search(r"\[led\]\s+LT4:\s+red_mode=(\d+)\s+red_odr=(\d+)", line)
        if m:
            rm = int(m.group(1))
            ro = int(m.group(2))
            if not data.lt4:
                data.lt4 = Lt4Data(red_mode=rm, red_odr=ro, green_pre=None, green_post=None)
            else:
                data.lt4.red_mode = rm
                data.lt4.red_odr = ro
            continue

        # [led] LT4_GREEN_PRE: mode=1 otype=0 speed=0 pull=0 odr=0
        m = re.search(r"\[led\]\s+LT4_GREEN_PRE:\s+mode=(\d+)\s+otype=(\d+)\s+speed=(\d+)\s+pull=(\d+)\s+odr=(\d+)", line)
        if m:
            gp = PinInfo(mode=int(m.group(1)), otype=int(m.group(2)), speed=int(m.group(3)),
                         pull=int(m.group(4)), odr=int(m.group(5)))
            if not data.lt4:
                data.lt4 = Lt4Data(red_mode=0, red_odr=0, green_pre=gp, green_post=None)
            else:
                data.lt4.green_pre = gp
            continue

        # [led] LT4_GREEN_POST: mode=1 otype=0 speed=0 pull=0 odr=0
        m = re.search(r"\[led\]\s+LT4_GREEN_POST:\s+mode=(\d+)\s+otype=(\d+)\s+speed=(\d+)\s+pull=(\d+)\s+odr=(\d+)", line)
        if m:
            gp = PinInfo(mode=int(m.group(1)), otype=int(m.group(2)), speed=int(m.group(3)),
                         pull=int(m.group(4)), odr=int(m.group(5)))
            if not data.lt4:
                data.lt4 = Lt4Data(red_mode=0, red_odr=0, green_pre=None, green_post=gp)
            else:
                data.lt4.green_post = gp
            continue

        # [led] LT5_TIM17: cr1=0x0000 dier=0x0000 nvic_pending=0 apb2enr=0x00000000
        m = re.search(r"\[led\]\s+LT5_TIM17:\s+cr1=0x([0-9a-fA-F]+)\s+dier=0x([0-9a-fA-F]+)\s+nvic_pending=(\d+)\s+apb2enr=0x([0-9a-fA-F]+)", line)
        if m:
            cr1 = int(m.group(1), 16)
            dier = int(m.group(2), 16)
            nvic = int(m.group(3))
            apb2 = int(m.group(4), 16)
            if not data.lt5:
                data.lt5 = Lt5Data(tim17_cr1=cr1, tim17_dier=dier, tim17_nvic=nvic, tim17_apb2=apb2, pins={})
            else:
                data.lt5.tim17_cr1 = cr1
                data.lt5.tim17_dier = dier
                data.lt5.tim17_nvic = nvic
                data.lt5.tim17_apb2 = apb2
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
                data.lt5 = Lt5Data(tim17_cr1=0, tim17_dier=0, tim17_nvic=0, tim17_apb2=0, pins=pins)
            else:
                data.lt5.pins = pins
            continue

        # [led] FW VERDICT=PASS
        m = re.search(r"\[led\]\s+FW\s+VERDICT=(\S+)", line)
        if m:
            data.fw_verdict = m.group(1)
            continue

    # Assign gathered snapshots into step data structures
    if data.lt1:
        data.lt1.ctrl_snap_pre = snapshots.get("LT1_CTRL_PRE", [])
        data.lt1.ctrl_snap_post = snapshots.get("LT1_CTRL_POST", [])
    if data.lt2:
        data.lt2.snap_pre = snapshots.get("LT2_PRE", [])
        data.lt2.snap_post_init = snapshots.get("LT2_POST_INIT", [])
        data.lt2.snap_post_ops = snapshots.get("LT2_POST_OPS", [])
        data.lt2.ctrl_snap_pre = snapshots.get("LT2_CTRL_PRE", [])
        data.lt2.ctrl_snap_post = snapshots.get("LT2_CTRL_POST", [])

    return data


# ---- Verification Pipeline --------------------------------------------------

def evaluate_run(data: SelftestRunData, log: Callable[[str], None],
                 expected_rev: str, expected_build_id: str,
                 allow_dirty: bool = False) -> bool:
    all_ok = True

    # 1. Provenance Checks (A5: --allow-dirty never yields PASS)
    if allow_dirty:
        log("[host] CHECK provenance: NOT VERIFIED (--allow-dirty flag active)")
        all_ok = False
    else:
        rev_match = (data.rev == expected_rev) and not data.rev.endswith("-dirty")
        log(f"[host] CHECK provenance_git_rev: {'PASS' if rev_match else 'FAIL'} (fw_rev={data.rev}, host_head={expected_rev})")
        if not rev_match:
            all_ok = False

        id_match = (data.build_id == expected_build_id) and (data.build_id != "unknown")
        log(f"[host] CHECK provenance_build_id: {'PASS' if id_match else 'FAIL'} (fw_id={data.build_id}, host_id={expected_build_id})")
        if not id_match:
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

        # Mutation: pull on pin G
        mut_lt1_pull = copy.deepcopy(data.lt1)
        mut_lt1_pull.pins["G"].pull = 1
        m_ok, _ = eval_lt1(mut_lt1_pull)
        log(f"[host] CHECK lt1_mutation_pull_rejected: {'PASS' if not m_ok else 'FAIL'}")
        if m_ok:
            all_ok = False

        # Mutation: lock count altered
        mut_lt1_lock = copy.deepcopy(data.lt1)
        mut_lt1_lock.unlock = mut_lt1_lock.lock - 1
        m_ok, _ = eval_lt1(mut_lt1_lock)
        log(f"[host] CHECK lt1_mutation_lock_rejected: {'PASS' if not m_ok else 'FAIL'}")
        if m_ok:
            all_ok = False

        # Mutation: control snapshot altered
        mut_lt1_snap = copy.deepcopy(data.lt1)
        if mut_lt1_snap.ctrl_snap_post:
            mut_lt1_snap.ctrl_snap_post[5] ^= 0x01
        m_ok, _ = eval_lt1(mut_lt1_snap)
        log(f"[host] CHECK lt1_mutation_ctrl_snap_rejected: {'PASS' if not m_ok else 'FAIL'}")
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

        # Mutation: pre vs post_init snapshot mismatch
        mut_lt2_init = copy.deepcopy(data.lt2)
        if mut_lt2_init.snap_post_init:
            mut_lt2_init.snap_post_init[0] ^= 0x01
        m_ok, _ = eval_lt2(mut_lt2_init)
        log(f"[host] CHECK lt2_mutation_init_rejected: {'PASS' if not m_ok else 'FAIL'}")
        if m_ok:
            all_ok = False

        # Mutation: control snapshot unchanged (failed negative control)
        mut_lt2_ctrl = copy.deepcopy(data.lt2)
        mut_lt2_ctrl.ctrl_snap_post = list(mut_lt2_ctrl.ctrl_snap_pre)
        m_ok, _ = eval_lt2(mut_lt2_ctrl)
        log(f"[host] CHECK lt2_mutation_ctrl_unchanged_rejected: {'PASS' if not m_ok else 'FAIL'}")
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

        # Mutation: measured DWT frequency too low (< 100 kHz)
        mut_lt3_freq = copy.deepcopy(data.lt3)
        mut_lt3_freq.dwt_cycles = mut_lt3_freq.dwt_cycles * 5
        m_ok, _ = eval_lt3(mut_lt3_freq)
        log(f"[host] CHECK lt3_mutation_freq_low_rejected: {'PASS' if not m_ok else 'FAIL'}")
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

        # Mutation: green pin modified by PanicRed
        mut_lt4_green = copy.deepcopy(data.lt4)
        if mut_lt4_green.green_post:
            mut_lt4_green.green_post.mode = 0
        m_ok, _ = eval_lt4(mut_lt4_green)
        log(f"[host] CHECK lt4_mutation_green_modified_rejected: {'PASS' if not m_ok else 'FAIL'}")
        if m_ok:
            all_ok = False

        # Mutation: red pin not output
        mut_lt4_red = copy.deepcopy(data.lt4)
        mut_lt4_red.red_mode = 0
        m_ok, _ = eval_lt4(mut_lt4_red)
        log(f"[host] CHECK lt4_mutation_red_not_output_rejected: {'PASS' if not m_ok else 'FAIL'}")
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

        # Mutation: TIM17 CEN bit still set
        mut_lt5_tim = copy.deepcopy(data.lt5)
        mut_lt5_tim.tim17_cr1 = 1
        m_ok, _ = eval_lt5(mut_lt5_tim)
        log(f"[host] CHECK lt5_mutation_tim17_running_rejected: {'PASS' if not m_ok else 'FAIL'}")
        if m_ok:
            all_ok = False

        # Mutation: pin pull not none
        mut_lt5_pull = copy.deepcopy(data.lt5)
        mut_lt5_pull.pins["G"].pull = 1
        m_ok, _ = eval_lt5(mut_lt5_pull)
        log(f"[host] CHECK lt5_mutation_pin_pull_rejected: {'PASS' if not m_ok else 'FAIL'}")
        if m_ok:
            all_ok = False

    # 7. Firmware Verdict Check
    fw_ok = (data.fw_verdict == "PASS")
    log(f"[host] CHECK fw_verdict: {'PASS' if fw_ok else 'FAIL'} (verdict={data.fw_verdict})")
    if not fw_ok:
        all_ok = False

    return all_ok


# ---- Mock Suite (A11) -------------------------------------------------------

def generate_mock_lines(rev: str = "mock_rev", build_id: str = "mock_id",
                        mutation: Optional[str] = None) -> list[str]:
    """Generates synthetic console lines to verify parser and step oracles end-to-end."""
    lines = [
        " ",
        "=== STM32H755ZI 01_led driver selftest - CPU1 (Cortex-M7) ===",
        f"[sys] build rev={rev} id={build_id}",
        "[led] LT1_PRE: deinit_rc=0,0,0 read_rc=0,0,0 modes=3,3,3",
        "[led] LT1: mask=0x07 lock=3 unlock=3",
        "[led] LT1_PINS: G(mode=1,otype=0,speed=0,pull=0,odr=0) Y(mode=1,otype=0,speed=0,pull=0,odr=0) R(mode=1,otype=0,speed=0,pull=0,odr=0)",
        "[led] LT1_CTRL: mask=0x00 lock=1 unlock=0 deinit_rc=0,0,0 modes=3,3,3 odr_g=0 odr_y=0 odr_r=0",
        "[led] LT1_CTRL_PRE_A: 28000000 00000000 0C000000 24000000 00000000",
        "[led] LT1_CTRL_PRE_B: 00000280 00000000 000000C0 00000100 00000000",
        "[led] LT1_CTRL_PRE_E: 00000000 00000000 00000000 00000000 00000000",
        "[led] LT1_CTRL_POST_A: 28000000 00000000 0C000000 24000000 00000000",
        "[led] LT1_CTRL_POST_B: 00000280 00000000 000000C0 00000100 00000000",
        "[led] LT1_CTRL_POST_E: 00000000 00000000 00000000 00000000 00000000",
        "[led] LT2: mask=0x00",
        "[led] LT2_PRE_A: 28000000 00000000 0C000000 24000000 00000000",
        "[led] LT2_PRE_B: 10000001 00000000 000000C0 00000100 00000000",
        "[led] LT2_PRE_E: 00000004 00000000 00000000 00000000 00000000",
        "[led] LT2_POST_INIT_A: 28000000 00000000 0C000000 24000000 00000000",
        "[led] LT2_POST_INIT_B: 10000001 00000000 000000C0 00000100 00000000",
        "[led] LT2_POST_INIT_E: 00000004 00000000 00000000 00000000 00000000",
        "[led] LT2_POST_OPS_A: 28000000 00000000 0C000000 24000000 00000000",
        "[led] LT2_POST_OPS_B: 10000001 00000000 000000C0 00000100 00000000",
        "[led] LT2_POST_OPS_E: 00000004 00000000 00000000 00000000 00000000",
        "[led] LT2_CTRL: mask=0x01",
        "[led] LT2_CTRL_PRE_A: 28000000 00000000 0C000000 24000000 00000000",
        "[led] LT2_CTRL_PRE_B: 10000003 00000000 000000C0 00000100 00000000",
        "[led] LT2_CTRL_PRE_E: 00000004 00000000 00000000 00000000 00000000",
        "[led] LT2_CTRL_POST_A: 28000000 00000000 0C000000 24000000 00000000",
        "[led] LT2_CTRL_POST_B: 10000001 00000000 000000C0 00000100 00000000",
        "[led] LT2_CTRL_POST_E: 00000004 00000000 00000000 00000000 00000000",
        "[led] LT3: N=200000 dwt_cycles=2000000 dwt_isrs=1000 cpu_hz=400000000 window_isrs=7000 lost=0",
        "[led] LT3_CTRL: N=200000 window_isrs=7000 lost=2000",
        "[led] LT4: red_mode=1 red_odr=1",
        "[led] LT4_GREEN_PRE: mode=1 otype=0 speed=0 pull=0 odr=0",
        "[led] LT4_GREEN_POST: mode=1 otype=0 speed=0 pull=0 odr=0",
        "[led] LT5_TIM17: cr1=0x0000 dier=0x0000 nvic_pending=0 apb2enr=0x00000000",
        "[led] LT5_PINS: G(mode=1,otype=0,speed=0,pull=0,odr=0) Y(mode=1,otype=0,speed=0,pull=0,odr=0) R(mode=1,otype=0,speed=0,pull=0,odr=0)",
        "[led] FW VERDICT=PASS"
    ]

    if mutation == "lt3_lost_gt_0":
        lines = [re.sub(r"lost=0", "lost=5", l) if "[led] LT3:" in l else l for l in lines]
    elif mutation == "lt3_ctrl_lost_eq_0":
        lines = [re.sub(r"lost=2000", "lost=0", l) if "[led] LT3_CTRL:" in l else l for l in lines]
    elif mutation == "lt1_pull_activated":
        lines = [re.sub(r"G\(mode=1,otype=0,speed=0,pull=0,odr=0\)", "G(mode=1,otype=0,speed=0,pull=1,odr=0)", l) for l in lines]
    elif mutation == "missing_lt3":
        lines = [l for l in lines if "[led] LT3:" not in l]
    elif mutation == "lt2_post_ops_changed":
        lines = [re.sub(r"00000004", "00000005", l) if "LT2_POST_OPS_E:" in l else l for l in lines]

    return lines


def run_mock_suite(log: Callable[[str], None]) -> bool:
    log("[mock] === Running Mock Host Evaluation Suite ===")

    # 1. Nominal run through parser and evaluation pipeline (without allow_dirty)
    nominal_lines = generate_mock_lines("mock_rev", "mock_id")
    nom_data = parse_console_lines(nominal_lines)
    nom_ok = evaluate_run(nom_data, log, expected_rev="mock_rev", expected_build_id="mock_id", allow_dirty=False)
    log(f"[mock] CHECK mock_nominal: {'PASS' if nom_ok else 'FAIL'}")

    # 2. Mandatory Failure 1: lost > 0 in LT3 real run via console line mutation
    fail1_lines = generate_mock_lines("mock_rev", "mock_id", mutation="lt3_lost_gt_0")
    f1_data = parse_console_lines(fail1_lines)
    f1_ok = evaluate_run(f1_data, lambda _: None, expected_rev="mock_rev", expected_build_id="mock_id", allow_dirty=False)
    f1_detected = (not f1_ok)
    log(f"[mock] CHECK mock_failure_lt3_real_lost_gt_0_detected: {'PASS' if f1_detected else 'FAIL'}")

    # 3. Mandatory Failure 2: lost = 0 in LT3 control run via console line mutation
    fail2_lines = generate_mock_lines("mock_rev", "mock_id", mutation="lt3_ctrl_lost_eq_0")
    f2_data = parse_console_lines(fail2_lines)
    f2_ok = evaluate_run(f2_data, lambda _: None, expected_rev="mock_rev", expected_build_id="mock_id", allow_dirty=False)
    f2_detected = (not f2_ok)
    log(f"[mock] CHECK mock_failure_lt3_ctrl_lost_eq_0_detected: {'PASS' if f2_detected else 'FAIL'}")

    # 4. Mandatory Failure 3: pin in LT1 with pull via console line mutation
    fail3_lines = generate_mock_lines("mock_rev", "mock_id", mutation="lt1_pull_activated")
    f3_data = parse_console_lines(fail3_lines)
    f3_ok = evaluate_run(f3_data, lambda _: None, expected_rev="mock_rev", expected_build_id="mock_id", allow_dirty=False)
    f3_detected = (not f3_ok)
    log(f"[mock] CHECK mock_failure_lt1_pin_with_pull_detected: {'PASS' if f3_detected else 'FAIL'}")

    # 5. Missing LT line case (A11)
    fail4_lines = generate_mock_lines("mock_rev", "mock_id", mutation="missing_lt3")
    f4_data = parse_console_lines(fail4_lines)
    f4_ok = evaluate_run(f4_data, lambda _: None, expected_rev="mock_rev", expected_build_id="mock_id", allow_dirty=False)
    f4_detected = (not f4_ok)
    log(f"[mock] CHECK mock_failure_missing_lt_line_detected: {'PASS' if f4_detected else 'FAIL'}")

    # 6. Altered snapshot in LT2 via console line mutation
    fail5_lines = generate_mock_lines("mock_rev", "mock_id", mutation="lt2_post_ops_changed")
    f5_data = parse_console_lines(fail5_lines)
    f5_ok = evaluate_run(f5_data, lambda _: None, expected_rev="mock_rev", expected_build_id="mock_id", allow_dirty=False)
    f5_detected = (not f5_ok)
    log(f"[mock] CHECK mock_failure_lt2_snapshot_mismatch_detected: {'PASS' if f5_detected else 'FAIL'}")

    all_mock_ok = nom_ok and f1_detected and f2_detected and f3_detected and f4_detected and f5_detected
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
                        help="Allow dirty working tree (development only; yields HOST VERDICT != PASS)")
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
                if res.returncode != 0:
                    stamp(f"[host] ERROR: SWD reset failed with rc={res.returncode}")
                    stamp("HOST VERDICT=FAIL")
                    return 1

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

    verdict_str = "PASS" if ok else ("NOT VERIFIED" if args.allow_dirty else "FAIL")
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
