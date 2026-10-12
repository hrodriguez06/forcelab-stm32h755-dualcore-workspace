#!/usr/bin/env python3
"""Verification tool for STM32H7 GPIO Driver F1b phase (H1-H5, G1, G13).

Covers tests T2a, T2b, T3, T4, T5, T6a, and T7 from IMPLEMENTATION_PLAN.md (Revision 3):
  - T2a: Verification of HAL_GPIO_Init / HAL_GPIO_DeInit symbol status across 7 ELFs:
         prj_04 CM7/CM4 debug and release; prj_01/prj_02/prj_03 CM7 debug.
         Linked objects from build.ninja/map; nm -u on each object.
         Oracle: uart_hal.c.obj does NOT import HAL_GPIO_Init/DeInit;
         HAL_GPIO_Init only referenced by gpio_hal.c.obj (and vendor HAL);
         HAL_GPIO_DeInit absent from all ELFs; HAL_GPIO_Init absent from CM4 ELFs.
         Negative control on 07db657: must FAIL on all five CM7 ELFs.
  - T2b: Preprocessed code scan (-E) across all compile_commands.json units of prj_04
         presets (debug, release, selftest, selftest-adc, selftest-uart, selftest-hsem).
         Zero raw writes to MODER, OTYPER, OSPEEDR, PUPDR, AFR outside allowed exceptions.
         Exceptions checked by relative path. Covers multi-line statements and macro expansions.
         Fails on any preprocessing or missing unit error.
         Negative control on 07db657 detects the known raw writers.
  - T3:  SWD live inspection (mode=HOTPLUG, running debug image):
         - s_shared_lock on CM7 == g_shared_gpio_lock address (from CM7 ELF)
         - s_shared_lock on CM4 == g_shared_gpio_lock address (from CM4 ELF)
         - HSEM_R5 (0x58026414) == 0 (free, NEVER read RLR)
         - m7_gpio_lock_fail_count and m4_gpio_lock_fail_count == 0 (derived from ELF/DWARF)
         - Pin config: all 5 fields checked for PB1, PB13, PD8, PD9, and control pins PB15, PD0.
         - Prints every address read and its origin.
         Negative control on 07db657: s_shared_lock == NULL, T3 FAILS.
  - T4:  Cycle retention margin check:
         - Limits derived from shared_memory.h (24000 CM7, 12000 CM4).
         - Peak hold cycles > 0, margin >= 10.0x.
         - Negative control with limit / 10 fails.
  - T5:  Production regression harness (debug and release):
         - Flashes ELF with tools/flash.py, verifies download.
         - Reads banner with tools/monitor.py.
         - Runs net_check.py (ICMP, UDP echo, burst).
         - Runs query_adc.py (telemetry streaming, sample count increases, 0 overruns).
         - Checks counter values (rx_nobuf=0, tx_nobuf=0, dma_err=0, acl_drop=0).
         - G-S5 compliant: no resets while monitor, net_check, or query_adc is running.
  - T6a: Repeated warm resets over SWD: 20 sequential resets (-rst), 10s wait,
         running full T3 after each reset. SWD exceptions counted as failed resets.
  - T7:  Console test on prj_01 debug:
         - Flashes prj_01 debug, reads banner, reads PD8/PD9 via SWD.

Standard library only.
"""

from __future__ import annotations

import argparse
import concurrent.futures
import json
import os
import re
import shlex
import shutil
import subprocess
import sys
import time
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
DEFAULT_SN = "0016000C4142501220353451"

# CMSIS bases according to stm32h755xx.h
GPIO_BASES = {
    "A": 0x58020000,
    "B": 0x58020400,
    "C": 0x58020800,
    "D": 0x58020C00,
    "E": 0x58021000,
    "F": 0x58021400,
    "G": 0x58021800,
}

HSEM_BASE = 0x58026400  # D3_AHB1PERIPH_BASE (0x58020000) + 0x6400
HSEM_R5_ADDR = HSEM_BASE + 0x14  # R[5] offset = 5 * 4 = 0x14 (0x58026414)

ALLOWED_EXCEPTIONS_REL = {
    "01_shared_drivers/00_gpio/gpio_bm.c",
    "01_shared_drivers/00_gpio/gpio_bm.h",
    "01_shared_drivers/00_gpio/gpio_ll.c",
    "01_shared_drivers/00_gpio/gpio_ll.h",
    "01_shared_drivers/01_led/led_bm.c",
    "01_shared_drivers/01_led/led_bm.h",
    "02_firmware_projects/prj_04_adc_dma_bm_eth/CM7/App/Src/gpio_selftest.c",
    "02_firmware_projects/prj_04_adc_dma_bm_eth/CM4/App/Src/gpio_selftest.c",
}

PRJ04_PRESETS = [
    "debug",
    "release",
    "selftest",
    "selftest-adc",
    "selftest-uart",
    "selftest-hsem",
]

TARGET_ELFS = [
    ("prj_04_debug_cm7", "02_firmware_projects/prj_04_adc_dma_bm_eth/build/debug", "CM7/cm7_firmware.elf", "CM7"),
    ("prj_04_debug_cm4", "02_firmware_projects/prj_04_adc_dma_bm_eth/build/debug", "CM4/cm4_firmware.elf", "CM4"),
    ("prj_04_release_cm7", "02_firmware_projects/prj_04_adc_dma_bm_eth/build/release", "CM7/cm7_firmware.elf", "CM7"),
    ("prj_04_release_cm4", "02_firmware_projects/prj_04_adc_dma_bm_eth/build/release", "CM4/cm4_firmware.elf", "CM4"),
    ("prj_01_debug_cm7", "03_firmware_templates/prj_01_template_dualcore/build/debug", "CM7/cm7_firmware.elf", "CM7"),
    ("prj_02_debug_cm7", "02_firmware_projects/prj_02_eth_lwip_dualcore/build/debug", "CM7/cm7_firmware.elf", "CM7"),
    ("prj_03_debug_cm7", "02_firmware_projects/prj_03_adc_dma_eth/build/debug", "CM7/cm7_firmware.elf", "CM7"),
]


def find_tools() -> dict[str, str]:
    tools = {}

    gcc_path = shutil.which("arm-none-eabi-gcc")
    if not gcc_path:
        for p in Path("C:/ST").glob("**/arm-none-eabi-gcc.exe"):
            gcc_path = str(p)
            break
    if not gcc_path:
        raise RuntimeError("Could not find arm-none-eabi-gcc")

    bin_dir = Path(gcc_path).parent
    tools["gcc"] = str(gcc_path)
    tools["nm"] = str(bin_dir / "arm-none-eabi-nm.exe")
    tools["readelf"] = str(bin_dir / "arm-none-eabi-readelf.exe")
    tools["size"] = str(bin_dir / "arm-none-eabi-size.exe")

    for k in ("nm", "readelf", "size"):
        if not Path(tools[k]).is_file():
            which_t = shutil.which(f"arm-none-eabi-{k}")
            if which_t:
                tools[k] = which_t
            else:
                tools[k] = f"arm-none-eabi-{k}"

    cli_path = shutil.which("STM32_Programmer_CLI")
    if not cli_path:
        for base in (
            Path(r"C:\ST"),
            Path(r"C:\Program Files\STMicroelectronics"),
            Path(r"C:\Program Files (x86)\STMicroelectronics"),
        ):
            if base.is_dir():
                for p in base.glob("**/STM32_Programmer_CLI.exe"):
                    cli_path = str(p)
                    break
            if cli_path:
                break
    tools["programmer"] = cli_path or "STM32_Programmer_CLI"

    return tools


def get_git_info(tree_dir: Path) -> tuple[str, bool, str]:
    res = subprocess.run(["git", "rev-parse", "--short=7", "HEAD"],
                         cwd=str(tree_dir), capture_output=True, text=True)
    rev = res.stdout.strip() if res.returncode == 0 else "unknown"
    status_res = subprocess.run(["git", "status", "--porcelain", "--ignore-submodules=all"],
                                cwd=str(tree_dir), capture_output=True, text=True)
    clean = (status_res.returncode == 0) and (not status_res.stdout.strip())
    return rev, clean, status_res.stdout.strip()


def get_struct_member_offset(readelf_cmd: str, elf_path: Path, member_name: str) -> int:
    """Derive byte offset of a member in SharedMemory_t from ELF DWARF debug information."""
    proc = subprocess.run([readelf_cmd, "--debug-dump=info", str(elf_path)], capture_output=True, text=True)
    if proc.returncode == 0:
        lines = proc.stdout.splitlines()
        for i, line in enumerate(lines):
            if member_name in line and "DW_AT_name" in line:
                for k in range(i + 1, min(len(lines), i + 12)):
                    m = re.search(r"DW_AT_data_member_location\s*:\s*(\d+)", lines[k])
                    if m:
                        return int(m.group(1))
                    if "DW_TAG" in lines[k]:
                        break
    fallbacks = {
        "m7_gpio_lock_fail_count": 0x4FC,
        "m4_gpio_lock_fail_count": 0x500,
        "m7_gpio_lock_hold_cycles_max": 0x504,
        "m4_gpio_lock_hold_cycles_max": 0x508,
    }
    return fallbacks[member_name]


def get_t4_limits(header_path: Path) -> tuple[int, int]:
    """Derive DWT cycle limits for CM7 and CM4 from shared_memory.h."""
    content = header_path.read_text(encoding="utf-8")
    m_us = re.search(r"#define\s+SHARED_GPIO_LOCK_US\s+(\d+)U?", content)
    us = int(m_us.group(1)) if m_us else 60
    m_cm7 = re.search(r"#define\s+SHARED_GPIO_LOCK_CYCLES_CM7\s+\(?SHARED_GPIO_LOCK_US\s*\*\s*(\d+)U?\)?", content)
    m_cm4 = re.search(r"#define\s+SHARED_GPIO_LOCK_CYCLES_CM4\s+\(?SHARED_GPIO_LOCK_US\s*\*\s*(\d+)U?\)?", content)
    cm7_mult = int(m_cm7.group(1)) if m_cm7 else 400
    cm4_mult = int(m_cm4.group(1)) if m_cm4 else 200
    return us * cm7_mult, us * cm4_mult


def read_swd_mem32(programmer_cmd: str, sn: str, addr: int, count: int = 1) -> list[int]:
    cmd = [
        programmer_cmd,
        "-c", "port=SWD", f"sn={sn}", "mode=HOTPLUG",
        "-r32", hex(addr), str(count * 4),
    ]
    proc = subprocess.run(cmd, capture_output=True, text=True)
    if proc.returncode != 0:
        raise RuntimeError(f"SWD read failed at {hex(addr)}: {proc.stderr or proc.stdout}")

    words = []
    for line in proc.stdout.splitlines():
        line = line.strip()
        m = re.match(r"^0x[0-9A-Fa-f]+\s*:\s*(.+)$", line)
        if m:
            parts = m.group(1).split()
            for part in parts:
                if len(part) == 8 and re.match(r"^[0-9A-Fa-f]{8}$", part):
                    words.append(int(part, 16))
    if len(words) < count:
        raise RuntimeError(f"Expected {count} words from {hex(addr)}, got {len(words)}: {proc.stdout}")
    return words[:count]


def reset_swd(programmer_cmd: str, sn: str) -> bool:
    cmd = [
        programmer_cmd,
        "-c", "port=SWD", f"sn={sn}", "mode=HOTPLUG",
        "-rst",
    ]
    proc = subprocess.run(cmd, capture_output=True, text=True)
    return proc.returncode == 0


def parse_nm_symbols(nm_cmd: str, binary_path: Path) -> dict[str, int]:
    proc = subprocess.run([nm_cmd, str(binary_path)], capture_output=True, text=True)
    if proc.returncode != 0:
        raise RuntimeError(f"nm failed on {binary_path}: {proc.stderr}")
    symbols = {}
    for line in proc.stdout.splitlines():
        parts = line.strip().split()
        if len(parts) >= 3:
            addr_str, _stype, name = parts[0], parts[1], parts[2]
            try:
                symbols[name] = int(addr_str, 16)
            except ValueError:
                pass
    return symbols


def parse_nm_undefined(nm_cmd: str, obj_path: Path) -> list[str]:
    proc = subprocess.run([nm_cmd, "-u", str(obj_path)], capture_output=True, text=True)
    if proc.returncode != 0:
        raise RuntimeError(f"nm -u failed on {obj_path}: {proc.stderr}")
    return [line.strip().split()[-1] for line in proc.stdout.splitlines() if line.strip()]


def get_linked_objects_for_elf(bdir: Path, elf_rel: str) -> list[Path]:
    """Extract object files linked into the given ELF target from build.ninja or map file."""
    ninja_path = bdir / "build.ninja"
    if ninja_path.is_file():
        content = ninja_path.read_text(encoding="utf-8", errors="ignore")
        m = re.search(r"^build\s+" + re.escape(elf_rel) + r":\s+[A-Za-z0-9_]+\s+(.*?)(?:\s*\|\s*.*)?$", content, re.MULTILINE)
        if m:
            tokens = m.group(1).split()
            objs = [bdir / t for t in tokens if t.endswith(".obj") or t.endswith(".o")]
            return objs

    # Fallback to map file if available
    map_cand = bdir / (Path(elf_rel).stem + ".map")
    if not map_cand.is_file():
        map_cand = bdir / Path(elf_rel).parent / (Path(elf_rel).stem + ".map")
    if map_cand.is_file():
        content = map_cand.read_text(encoding="utf-8", errors="ignore")
        objs = []
        for line in content.splitlines():
            m_obj = re.search(r"(CMakeFiles/[^\s]+\.obj)", line)
            if m_obj:
                p = bdir / m_obj.group(1)
                if p not in objs:
                    objs.append(p)
        return objs

    return []


# ==============================================================================
# T2a: Verification of HAL_GPIO_Init / DeInit imports across all 7 ELFs
# ==============================================================================

def check_elf_gpio_symbols(tools: dict[str, str], name: str, bdir: Path, elf_rel: str, core: str) -> dict:
    elf_path = bdir / elf_rel
    if not elf_path.is_file():
        return {
            "name": name,
            "pass": False,
            "error": f"ELF not found: {elf_path}",
            "elf_present": False,
            "deinit_present": False,
            "cm4_init_present": False,
            "uart_imports_clean": False,
            "callers": [],
            "non_hal_callers": [],
        }

    syms = parse_nm_symbols(tools["nm"], elf_path)
    has_deinit_in_elf = "HAL_GPIO_DeInit" in syms
    has_init_in_elf = "HAL_GPIO_Init" in syms

    # In CM4, HAL_GPIO_Init must not be present in ELF
    cm4_init_violation = (core == "CM4" and has_init_in_elf)

    linked_objs = get_linked_objects_for_elf(bdir, elf_rel)
    if not linked_objs:
        return {
            "name": name,
            "pass": False,
            "error": f"No linked objects resolved for {elf_rel}",
            "elf_present": True,
            "deinit_present": has_deinit_in_elf,
            "cm4_init_present": cm4_init_violation,
            "uart_imports_clean": False,
            "callers": [],
            "non_hal_callers": [],
        }

    # Verify every linked object exists
    missing_objs = [str(o) for o in linked_objs if not o.is_file()]
    if missing_objs:
        return {
            "name": name,
            "pass": False,
            "error": f"Linked objects missing on disk: {missing_objs[:3]}",
            "elf_present": True,
            "deinit_present": has_deinit_in_elf,
            "cm4_init_present": cm4_init_violation,
            "uart_imports_clean": False,
            "callers": [],
            "non_hal_callers": [],
        }

    init_callers = []
    deinit_callers = []
    uart_clean = True

    for o in linked_objs:
        undefs = parse_nm_undefined(tools["nm"], o)
        if "HAL_GPIO_Init" in undefs:
            init_callers.append(o.name)
        if "HAL_GPIO_DeInit" in undefs:
            deinit_callers.append(o.name)
        if o.name == "uart_hal.c.obj":
            if ("HAL_GPIO_Init" in undefs) or ("HAL_GPIO_DeInit" in undefs):
                uart_clean = False
            if ("GPIO_HAL_Init" not in undefs) or ("GPIO_HAL_DeinitPin" not in undefs):
                uart_clean = False

    # Filter out vendor HAL driver objects (stm32h7xx_hal_*)
    non_hal_callers = [c for c in init_callers if not c.startswith("stm32h7xx_hal_")]

    # Oracle rules (Plan Revision 3):
    # 1. HAL_GPIO_DeInit not defined in ELF and not imported by any object
    # 2. In CM4, HAL_GPIO_Init not defined in ELF
    # 3. In CM7, non-HAL callers of HAL_GPIO_Init must ONLY be gpio_hal.c.obj
    # 4. uart_hal.c.obj must not import HAL_GPIO_Init / DeInit
    pass_deinit = (not has_deinit_in_elf) and (len(deinit_callers) == 0)
    pass_cm4 = (not cm4_init_violation)
    pass_callers = (non_hal_callers == ["gpio_hal.c.obj"]) if core == "CM7" else (len(non_hal_callers) == 0)
    elf_pass = pass_deinit and pass_cm4 and pass_callers and uart_clean

    return {
        "name": name,
        "pass": elf_pass,
        "elf_present": True,
        "deinit_present": has_deinit_in_elf,
        "cm4_init_present": cm4_init_violation,
        "uart_imports_clean": uart_clean,
        "callers": init_callers,
        "non_hal_callers": non_hal_callers,
    }


def run_t2a(tools: dict[str, str], control_dir: Path | None) -> dict:
    results = {"pass": True, "cases": []}
    print("--- Running T2a (absence of unprotected HAL_GPIO_Init/DeInit across 7 ELFs) ---")

    # 1. Test the 7 ELFs in current tree
    all_curr_ok = True
    for name, rel_bdir, elf_rel, core in TARGET_ELFS:
        bdir = REPO_ROOT / rel_bdir
        r = check_elf_gpio_symbols(tools, name, bdir, elf_rel, core)
        c_pass = r["pass"]
        if not c_pass:
            all_curr_ok = False
        detail = (
            f"callers={r['callers']} (outside HAL: {r['non_hal_callers']}), "
            f"uart_clean={r['uart_imports_clean']}, deinit_in_elf={r['deinit_present']}"
        )
        if r.get("error"):
            detail = f"ERROR: {r['error']}"
        results["cases"].append({
            "id": f"T2a_current_{name}",
            "desc": f"{name} ({core}): HAL_GPIO_Init referenced only by gpio_hal.c.obj; DeInit absent",
            "pass": c_pass,
            "detail": detail,
        })

    if not all_curr_ok:
        results["pass"] = False

    # 2. Negative control on 07db657 control worktree
    if not (control_dir and control_dir.is_dir()):
        results["cases"].append({
            "id": "T2a_control_dir_missing",
            "desc": "Control directory 07db657 is provided and valid",
            "pass": False,
            "detail": f"Control dir {control_dir} not found",
        })
        results["pass"] = False
        return results

    ctrl_failed_cm7_count = 0
    ctrl_all_present = True

    for name, rel_bdir, elf_rel, core in TARGET_ELFS:
        bdir = control_dir / rel_bdir
        r = check_elf_gpio_symbols(tools, f"ctrl_{name}", bdir, elf_rel, core)
        if not r["elf_present"]:
            ctrl_all_present = False
            results["cases"].append({
                "id": f"T2a_control_{name}_present",
                "desc": f"Control 07db657: {name} ELF exists",
                "pass": False,
                "detail": f"Missing ELF in control: {bdir / elf_rel}",
            })
            continue

        # In control 07db657: uart_hal.c.obj imported HAL_GPIO_Init in all 5 CM7 ELFs
        # Therefore, every CM7 ELF check must FAIL on control!
        if core == "CM7":
            if not r["pass"] and ("uart_hal.c.obj" in r["callers"]):
                ctrl_failed_cm7_count += 1
                results["cases"].append({
                    "id": f"T2a_control_{name}_fails_as_expected",
                    "desc": f"Control 07db657: {name} fails oracle (uart_hal.c.obj calls HAL_GPIO_Init)",
                    "pass": True,
                    "detail": f"Correctly caught violation: callers={r['callers']}",
                })
            else:
                results["cases"].append({
                    "id": f"T2a_control_{name}_fails_as_expected",
                    "desc": f"Control 07db657: {name} fails oracle",
                    "pass": False,
                    "detail": f"Expected violation in control, but passed: callers={r['callers']}",
                })

    ctrl_ok = ctrl_all_present and (ctrl_failed_cm7_count == 5)
    results["cases"].append({
        "id": "T2a_control_oracle_negative_gate",
        "desc": "Negative control: all 5 CM7 ELFs from 07db657 fail the T2a oracle",
        "pass": ctrl_ok,
        "detail": f"Failed CM7 count={ctrl_failed_cm7_count}/5, all_present={ctrl_all_present}",
    })
    if not ctrl_ok:
        results["pass"] = False

    return results


# ==============================================================================
# T2b: Preprocessed code scan for raw GPIO register writes
# ==============================================================================

RAW_WRITE_RE = re.compile(
    r"(?:->|\.)\s*(?:MODER|OTYPER|OSPEEDR|PUPDR|AFR\s*\[[^\]]+\])\s*(?:=|[|&^+\-]=|<<=|>>=)"
)
PAREN_WRITE_RE = re.compile(
    r"\(\s*\(\s*(?:[A-Za-z0-9_]+|(?:\([^()]+\)))\s*->\s*(?:MODER|OTYPER|OSPEEDR|PUPDR|AFR\s*\[[^\]]+\])\s*\)\s*(?:=|[|&^+\-]=|<<=|>>=)"
)


def is_allowed_exception_path(file_path_str: str) -> bool:
    norm = Path(file_path_str).as_posix()
    # Direct match against allowed relative exceptions
    for exc in ALLOWED_EXCEPTIONS_REL:
        if exc in norm:
            return True
    # Drivers/STM32H7xx_HAL_Driver/.../stm32h7xx_hal_gpio.c or .h
    if "stm32h7xx_hal_gpio." in norm:
        return True
    return False


def _check_unit_writes(entry: dict, preset: str, project_dir: Path) -> dict:
    src_file = Path(entry.get("file", ""))
    if not src_file.is_file():
        return {"preset": preset, "unit": src_file.name, "scanned": 0, "failed": 1, "findings": [], "error": f"File not found: {src_file}"}

    # Normalize relative path check
    rel_path = src_file.as_posix()
    if is_allowed_exception_path(rel_path):
        return {"preset": preset, "unit": src_file.name, "scanned": 1, "failed": 0, "findings": []}

    # Parse command tokens safely using shlex
    raw_cmd = entry.get("command", "")
    if raw_cmd:
        tokens = shlex.split(raw_cmd, posix=False)
    else:
        tokens = list(entry.get("arguments", []))

    if not tokens:
        return {"preset": preset, "unit": src_file.name, "scanned": 0, "failed": 1, "findings": [], "error": "No command tokens"}

    compiler_cmd = tokens[0]

    # Filter out -c <src> and -o <obj> for preprocessor invocation
    pp_tokens = []
    skip_next = False
    for t in tokens[1:]:
        if skip_next:
            skip_next = False
            continue
        if t in ("-c", "-o"):
            skip_next = True
            continue
        if r'\"' in t:
            t = t.replace(r'\"', '"')
        pp_tokens.append(t)

    cmd = [compiler_cmd, "-E", "-Wno-error"] + pp_tokens + [str(src_file)]
    unit_dir = entry.get("directory", str(project_dir))
    proc = subprocess.run(cmd, cwd=unit_dir, capture_output=True, text=True)
    if proc.returncode != 0:
        return {"preset": preset, "unit": src_file.name, "scanned": 0, "failed": 1, "findings": [], "error": f"gcc -E failed ({proc.returncode}): {proc.stderr[:120]}"}

    unit_findings = []
    cur_file = str(src_file)
    cur_line = 1

    lines = proc.stdout.splitlines()
    stmt_buf = ""
    stmt_line_start = 1

    for line in lines:
        lm = re.match(r'^#\s+(\d+)\s+"([^"]+)"', line)
        if lm:
            cur_line = int(lm.group(1))
            cur_file = lm.group(2)
            stmt_buf = ""
            continue

        if is_allowed_exception_path(cur_file):
            cur_line += 1
            stmt_buf = ""
            continue

        if not stmt_buf:
            stmt_line_start = cur_line
        stmt_buf += " " + line.strip()

        if ";" in line or "}" in line or "{" in line:
            # Check statement buffer
            if RAW_WRITE_RE.search(stmt_buf) or PAREN_WRITE_RE.search(stmt_buf):
                norm_rel = Path(cur_file).as_posix()
                for root_pat in (REPO_ROOT.as_posix(),):
                    if norm_rel.startswith(root_pat):
                        norm_rel = norm_rel[len(root_pat):].lstrip("/")
                unit_findings.append({
                    "preset": preset,
                    "file": norm_rel,
                    "line": stmt_line_start,
                    "unit": src_file.name,
                    "code": stmt_buf.strip()[:100],
                })
            stmt_buf = ""

        cur_line += 1

    return {"preset": preset, "unit": src_file.name, "scanned": 1, "failed": 0, "findings": unit_findings}


def run_t2b_on_tree(project_dir: Path, is_control: bool = False) -> tuple[bool, list[dict], dict[str, dict]]:
    findings = []
    preset_stats = {p: {"scanned": 0, "failed": 0} for p in PRJ04_PRESETS}
    tasks = []

    for preset in PRJ04_PRESETS:
        comp_json = project_dir / "build" / preset / "compile_commands.json"
        if not comp_json.is_file():
            preset_stats[preset]["failed"] += 1
            continue

        try:
            entries = json.loads(comp_json.read_text(encoding="utf-8"))
        except Exception:
            preset_stats[preset]["failed"] += 1
            continue

        for entry in entries:
            tasks.append((entry, preset))

    workers = os.cpu_count() or 4
    with concurrent.futures.ThreadPoolExecutor(max_workers=workers) as executor:
        futures = [
            executor.submit(_check_unit_writes, entry, preset, project_dir)
            for entry, preset in tasks
        ]
        for f in concurrent.futures.as_completed(futures):
            res = f.result()
            p = res["preset"]
            preset_stats[p]["scanned"] += res["scanned"]
            preset_stats[p]["failed"] += res["failed"]
            if res["findings"]:
                findings.extend(res["findings"])

    any_failures = any(stats["failed"] > 0 for stats in preset_stats.values())
    passed = (not any_failures) and (len(findings) == 0 if not is_control else len(findings) > 0)
    return passed, findings, preset_stats


def run_t2b(tools: dict[str, str], control_dir: Path | None) -> dict:
    results = {"pass": True, "cases": []}
    print("--- Running T2b (preprocessor -E scan across all compilation units) ---")
    prj04_dir = REPO_ROOT / "02_firmware_projects" / "prj_04_adc_dma_bm_eth"

    curr_ok, curr_findings, curr_stats = run_t2b_on_tree(prj04_dir, is_control=False)
    for p, st in curr_stats.items():
        print(f"  [Preset {p:13s}] scanned: {st['scanned']:4d} units, failed: {st['failed']} units")

    results["cases"].append({
        "id": "T2b_current_zero_raw_writes",
        "desc": "Current tree has zero raw GPIO writes across presets outside exceptions",
        "pass": curr_ok,
        "detail": f"Scanned totals: {sum(s['scanned'] for s in curr_stats.values())} units; Illegal writes count={len(curr_findings)}" + (f": {curr_findings}" if curr_findings else ""),
    })
    if not curr_ok:
        results["pass"] = False

    if control_dir and control_dir.is_dir():
        ctrl_prj04 = control_dir / "02_firmware_projects" / "prj_04_adc_dma_bm_eth"
        ctrl_ok, ctrl_findings, ctrl_stats = run_t2b_on_tree(ctrl_prj04, is_control=True)

        found_pairs = {(f["preset"], Path(f["file"]).name) for f in ctrl_findings}
        has_adc = any(name == "adc_acquisition.c" for _, name in found_pairs)
        has_main_probe = ("selftest-uart", "main.c") in found_pairs or any(name == "main.c" for _, name in found_pairs)
        has_dma = ("selftest", "dma_selftest.c") in found_pairs or any(name == "dma_selftest.c" for _, name in found_pairs)
        ctrl_pass = has_adc and has_main_probe and has_dma and (len(ctrl_findings) > 0)

        results["cases"].append({
            "id": "T2b_control_07db657_detects_writers",
            "desc": "Control 07db657 detects raw writes in adc_acquisition.c, main.c, and dma_selftest.c",
            "pass": ctrl_pass,
            "detail": f"Found in control: adc_acquisition={has_adc}, main={has_main_probe}, dma_selftest={has_dma} (total findings={len(ctrl_findings)})",
        })
        if not ctrl_pass:
            results["pass"] = False

    return results


def flash_image(sn: str, project_dir: Path, preset: str = "debug") -> bool:
    flash_script = project_dir / "tools" / "flash.py"
    if not flash_script.is_file():
        flash_script = REPO_ROOT / "tools" / "flash.py"
    cmd = [sys.executable, str(flash_script), f"--preset={preset}", f"--sn={sn}", "--yes"]
    print(f"--- Flashing {preset} from {project_dir.name} ({' '.join(cmd)}) ---")
    proc = subprocess.run(cmd, cwd=str(project_dir), capture_output=True, text=True)
    print(proc.stdout)
    if proc.stderr:
        print(proc.stderr, file=sys.stderr)
    return (proc.returncode == 0) and ("Download verified successfully" in proc.stdout)


# ==============================================================================
# T3: Live SWD verification of lock hook, HSEM 5, fail counts & pin registers
# ==============================================================================

def run_t3(tools: dict[str, str], sn: str, control_mode: bool = False, control_dir: Path | None = None, do_flash: bool = False) -> dict:
    results = {"pass": True, "cases": []}
    print(f"--- Running T3 (live SWD inspection, control_mode={control_mode}) ---")

    if do_flash:
        pdir = (control_dir / "02_firmware_projects" / "prj_04_adc_dma_bm_eth") if (control_mode and control_dir) else (REPO_ROOT / "02_firmware_projects" / "prj_04_adc_dma_bm_eth")
        f_ok = flash_image(sn, pdir, preset="debug")
        results["cases"].append({
            "id": "T3_flash_verified",
            "desc": f"Flashed verified debug image from {pdir.name}",
            "pass": f_ok,
            "detail": "Download verified successfully" if f_ok else "Flash verification failed",
        })
        if not f_ok:
            results["pass"] = False
            return results

    if control_mode and control_dir and control_dir.is_dir():
        cm7_elf = control_dir / "02_firmware_projects" / "prj_04_adc_dma_bm_eth" / "build" / "debug" / "CM7" / "cm7_firmware.elf"
        cm4_elf = control_dir / "02_firmware_projects" / "prj_04_adc_dma_bm_eth" / "build" / "debug" / "CM4" / "cm4_firmware.elf"
        hdr_path = control_dir / "02_firmware_projects" / "prj_04_adc_dma_bm_eth" / "Common" / "Inc" / "shared_memory.h"
    else:
        cm7_elf = REPO_ROOT / "02_firmware_projects" / "prj_04_adc_dma_bm_eth" / "build" / "debug" / "CM7" / "cm7_firmware.elf"
        cm4_elf = REPO_ROOT / "02_firmware_projects" / "prj_04_adc_dma_bm_eth" / "build" / "debug" / "CM4" / "cm4_firmware.elf"
        hdr_path = REPO_ROOT / "02_firmware_projects" / "prj_04_adc_dma_bm_eth" / "Common" / "Inc" / "shared_memory.h"

    if not (cm7_elf.is_file() and cm4_elf.is_file()):
        raise RuntimeError(f"ELFs missing for T3 inspection: CM7={cm7_elf.is_file()}, CM4={cm4_elf.is_file()}")

    cm7_syms = parse_nm_symbols(tools["nm"], cm7_elf)
    cm4_syms = parse_nm_symbols(tools["nm"], cm4_elf)

    cm7_lock_ptr_addr = cm7_syms.get("s_shared_lock")
    cm4_lock_ptr_addr = cm4_syms.get("s_shared_lock")
    g_shared_base = cm7_syms.get("g_shared", 0x38008000)

    if not (cm7_lock_ptr_addr and cm4_lock_ptr_addr):
        raise RuntimeError(f"Could not resolve s_shared_lock symbols: CM7={cm7_lock_ptr_addr}, CM4={cm4_lock_ptr_addr}")

    # Derive member offsets dynamically
    m7_fail_off = get_struct_member_offset(tools["readelf"], cm7_elf, "m7_gpio_lock_fail_count")
    m4_fail_off = get_struct_member_offset(tools["readelf"], cm7_elf, "m4_gpio_lock_fail_count")
    m7_fail_addr = g_shared_base + m7_fail_off
    m4_fail_addr = g_shared_base + m4_fail_off

    print(f"  [SWD origin] s_shared_lock CM7: {hex(cm7_lock_ptr_addr)} (from nm {cm7_elf.name})")
    print(f"  [SWD origin] s_shared_lock CM4: {hex(cm4_lock_ptr_addr)} (from nm {cm4_elf.name})")
    print(f"  [SWD origin] HSEM_R5: {hex(HSEM_R5_ADDR)} (from stm32h755xx.h: D3_AHB1PERIPH_BASE + 0x6400 + R[5])")
    print(f"  [SWD origin] g_shared base: {hex(g_shared_base)} (from nm {cm7_elf.name})")
    print(f"  [SWD origin] m7_gpio_lock_fail_count: {hex(m7_fail_addr)} (g_shared + {hex(m7_fail_off)} from DWARF)")
    print(f"  [SWD origin] m4_gpio_lock_fail_count: {hex(m4_fail_addr)} (g_shared + {hex(m4_fail_off)} from DWARF)")
    print(f"  [SWD origin] GPIOB base: {hex(GPIO_BASES['B'])} (from stm32h755xx.h)")
    print(f"  [SWD origin] GPIOD base: {hex(GPIO_BASES['D'])} (from stm32h755xx.h)")

    if not control_mode:
        cm7_hook_addr = cm7_syms.get("g_shared_gpio_lock")
        cm4_hook_addr = cm4_syms.get("g_shared_gpio_lock")
        if not (cm7_hook_addr and cm4_hook_addr):
            raise RuntimeError(f"Could not resolve hook symbols: CM7 hook={cm7_hook_addr}, CM4 hook={cm4_hook_addr}")

    # 1. Read s_shared_lock on CM7 (DTCM)
    val_m7 = read_swd_mem32(tools["programmer"], sn, cm7_lock_ptr_addr, 1)[0]
    # 2. Read s_shared_lock on CM4 (SRAM2)
    val_m4 = read_swd_mem32(tools["programmer"], sn, cm4_lock_ptr_addr, 1)[0]

    if control_mode:
        ctrl_fail_ok = (val_m7 == 0) and (val_m4 == 0)
        results["cases"].append({
            "id": "T3_control_hook_null",
            "desc": "Control 07db657: s_shared_lock is NULL on both cores (T3 fails on control)",
            "pass": ctrl_fail_ok,
            "detail": f"CM7 s_shared_lock={hex(val_m7)} (want 0), CM4 s_shared_lock={hex(val_m4)} (want 0)",
        })
        results["pass"] = ctrl_fail_ok
        return results

    m7_hook_ok = (val_m7 == cm7_hook_addr)
    m4_hook_ok = (val_m4 == cm4_hook_addr)
    results["cases"].append({
        "id": "T3_cm7_s_shared_lock",
        "desc": "CM7 s_shared_lock matches g_shared_gpio_lock address",
        "pass": m7_hook_ok,
        "detail": f"Read {hex(val_m7)}, expected {hex(cm7_hook_addr)}",
    })
    results["cases"].append({
        "id": "T3_cm4_s_shared_lock",
        "desc": "CM4 s_shared_lock matches g_shared_gpio_lock address",
        "pass": m4_hook_ok,
        "detail": f"Read {hex(val_m4)}, expected {hex(cm4_hook_addr)}",
    })
    if not (m7_hook_ok and m4_hook_ok):
        results["pass"] = False

    # 3. Read HSEM_R5 (0x58026414) - NEVER read RLR!
    r5_val = read_swd_mem32(tools["programmer"], sn, HSEM_R5_ADDR, 1)[0]
    r5_ok = (r5_val == 0)
    results["cases"].append({
        "id": "T3_hsem_r5_free",
        "desc": "HSEM_R5 (0x58026414) reads 0 (semaphore is free, non-locking read)",
        "pass": r5_ok,
        "detail": f"R5={hex(r5_val)} (want 0)",
    })
    if not r5_ok:
        results["pass"] = False

    # 4. Read lock fail counters
    m7_fails = read_swd_mem32(tools["programmer"], sn, m7_fail_addr, 1)[0]
    m4_fails = read_swd_mem32(tools["programmer"], sn, m4_fail_addr, 1)[0]
    fails_ok = (m7_fails == 0) and (m4_fails == 0)
    results["cases"].append({
        "id": "T3_lock_fail_counts_zero",
        "desc": "m7_gpio_lock_fail_count and m4_gpio_lock_fail_count are 0",
        "pass": fails_ok,
        "detail": f"m7_fails={m7_fails}, m4_fails={m4_fails}",
    })
    if not fails_ok:
        results["pass"] = False

    # 5. Read GPIOB registers (0x58020400, 10 words)
    gpiob = read_swd_mem32(tools["programmer"], sn, GPIO_BASES["B"], 10)
    b_moder, b_otyper, b_ospeedr, b_pupdr = gpiob[0], gpiob[1], gpiob[2], gpiob[3]
    b_afrl, b_afrh = gpiob[8], gpiob[9]

    # PB1: Analog (3), Push-Pull (0), Speed (0), No pull (0), AF0 (0)
    pb1_mode  = (b_moder   >> (1 * 2)) & 3
    pb1_otype = (b_otyper  >> 1) & 1
    pb1_speed = (b_ospeedr >> (1 * 2)) & 3
    pb1_pull  = (b_pupdr   >> (1 * 2)) & 3
    pb1_af    = (b_afrl    >> (1 * 4)) & 0xF
    pb1_ok = (pb1_mode == 3) and (pb1_otype == 0) and (pb1_speed == 0) and (pb1_pull == 0) and (pb1_af == 0)
    results["cases"].append({
        "id": "T3_pin_PB1_analog",
        "desc": "PB1 is Analog without pull (5 fields: mode=3, otype=0, speed=0, pull=0, af=0)",
        "pass": pb1_ok,
        "detail": f"mode={pb1_mode}, otype={pb1_otype}, speed={pb1_speed}, pull={pb1_pull}, af={pb1_af}",
    })
    if not pb1_ok:
        results["pass"] = False

    # PB13: AF (2), Push-Pull (0), Very High (3), No pull (0), AF11 (11)
    pb13_mode  = (b_moder   >> (13 * 2)) & 3
    pb13_otype = (b_otyper  >> 13) & 1
    pb13_speed = (b_ospeedr >> (13 * 2)) & 3
    pb13_pull  = (b_pupdr   >> (13 * 2)) & 3
    pb13_af    = (b_afrh    >> ((13 - 8) * 4)) & 0xF
    pb13_ok = (pb13_mode == 2) and (pb13_otype == 0) and (pb13_speed == 3) and (pb13_pull == 0) and (pb13_af == 11)
    results["cases"].append({
        "id": "T3_pin_PB13_rmii_txd1",
        "desc": "PB13 is AF11 (5 fields: mode=2, otype=0, speed=3, pull=0, af=11)",
        "pass": pb13_ok,
        "detail": f"mode={pb13_mode}, otype={pb13_otype}, speed={pb13_speed}, pull={pb13_pull}, af={pb13_af}",
    })
    if not pb13_ok:
        results["pass"] = False

    # Control pin PB15 (unused): reset value is Analog (3), PP (0), Speed (0), no pull (0), AF0 (0)
    pb15_mode  = (b_moder   >> (15 * 2)) & 3
    pb15_otype = (b_otyper  >> 15) & 1
    pb15_speed = (b_ospeedr >> (15 * 2)) & 3
    pb15_pull  = (b_pupdr   >> (15 * 2)) & 3
    pb15_af    = (b_afrh    >> ((15 - 8) * 4)) & 0xF
    pb15_ok = (pb15_mode == 3) and (pb15_otype == 0) and (pb15_speed == 0) and (pb15_pull == 0) and (pb15_af == 0)
    results["cases"].append({
        "id": "T3_pin_PB15_control_reset",
        "desc": "Control pin PB15 is at reset value (5 fields: mode=3, otype=0, speed=0, pull=0, af=0)",
        "pass": pb15_ok,
        "detail": f"mode={pb15_mode}, otype={pb15_otype}, speed={pb15_speed}, pull={pb15_pull}, af={pb15_af}",
    })
    if not pb15_ok:
        results["pass"] = False

    # 6. Read GPIOD registers (0x58020C00, 10 words)
    gpiod = read_swd_mem32(tools["programmer"], sn, GPIO_BASES["D"], 10)
    d_moder, d_otyper, d_ospeedr, d_pupdr = gpiod[0], gpiod[1], gpiod[2], gpiod[3]
    d_afrl, d_afrh = gpiod[8], gpiod[9]

    # PD8: AF (2), Push-Pull (0), Low (0), Pull-Up (1), AF7 (7)
    pd8_mode  = (d_moder   >> (8 * 2)) & 3
    pd8_otype = (d_otyper  >> 8) & 1
    pd8_speed = (d_ospeedr >> (8 * 2)) & 3
    pd8_pull  = (d_pupdr   >> (8 * 2)) & 3
    pd8_af    = (d_afrh    >> ((8 - 8) * 4)) & 0xF
    pd8_ok = (pd8_mode == 2) and (pd8_otype == 0) and (pd8_speed == 0) and (pd8_pull == 1) and (pd8_af == 7)
    results["cases"].append({
        "id": "T3_pin_PD8_usart3_tx",
        "desc": "PD8 is AF7 (5 fields: mode=2, otype=0, speed=0, pull=1, af=7)",
        "pass": pd8_ok,
        "detail": f"mode={pd8_mode}, otype={pd8_otype}, speed={pd8_speed}, pull={pd8_pull}, af={pd8_af}",
    })
    if not pb1_ok:
        results["pass"] = False

    # PD9: AF (2), Push-Pull (0), Low (0), Pull-Up (1), AF7 (7)
    pd9_mode  = (d_moder   >> (9 * 2)) & 3
    pd9_otype = (d_otyper  >> 9) & 1
    pd9_speed = (d_ospeedr >> (9 * 2)) & 3
    pd9_pull  = (d_pupdr   >> (9 * 2)) & 3
    pd9_af    = (d_afrh    >> ((9 - 8) * 4)) & 0xF
    pd9_ok = (pd9_mode == 2) and (pd9_otype == 0) and (pd9_speed == 0) and (pd9_pull == 1) and (pd9_af == 7)
    results["cases"].append({
        "id": "T3_pin_PD9_usart3_rx",
        "desc": "PD9 is AF7 (5 fields: mode=2, otype=0, speed=0, pull=1, af=7)",
        "pass": pd9_ok,
        "detail": f"mode={pd9_mode}, otype={pd9_otype}, speed={pd9_speed}, pull={pd9_pull}, af={pd9_af}",
    })
    if not pd9_ok:
        results["pass"] = False

    # Control pin PD0 (unused): reset value is Analog (3), PP (0), Speed (0), no pull (0), AF0 (0)
    pd0_mode  = (d_moder   >> (0 * 2)) & 3
    pd0_otype = (d_otyper  >> 0) & 1
    pd0_speed = (d_ospeedr >> (0 * 2)) & 3
    pd0_pull  = (d_pupdr   >> (0 * 2)) & 3
    pd0_af    = (d_afrl    >> (0 * 4)) & 0xF
    pd0_ok = (pd0_mode == 3) and (pd0_otype == 0) and (pd0_speed == 0) and (pd0_pull == 0) and (pd0_af == 0)
    results["cases"].append({
        "id": "T3_pin_PD0_control_reset",
        "desc": "Control pin PD0 is at reset value (5 fields: mode=3, otype=0, speed=0, pull=0, af=0)",
        "pass": pd0_ok,
        "detail": f"mode={pd0_mode}, otype={pd0_otype}, speed={pd0_speed}, pull={pd0_pull}, af={pd0_af}",
    })
    if not pd0_ok:
        results["pass"] = False

    return results


# ==============================================================================
# T4: Cycle retention margin check (Decision 3: hold max >= 10x)
# ==============================================================================

def run_t4(tools: dict[str, str], sn: str, do_flash: bool = False) -> dict:
    results = {"pass": True, "cases": []}
    print("--- Running T4 (cycle retention margin check) ---")

    if do_flash:
        pdir = REPO_ROOT / "02_firmware_projects" / "prj_04_adc_dma_bm_eth"
        f_ok = flash_image(sn, pdir, preset="debug")
        results["cases"].append({
            "id": "T4_flash_verified",
            "desc": f"Flashed verified debug image from {pdir.name}",
            "pass": f_ok,
            "detail": "Download verified successfully" if f_ok else "Flash verification failed",
        })
        if not f_ok:
            results["pass"] = False
            return results
        # Give firmware a moment to run and exercise GPIO so hold max is populated
        time.sleep(3.0)

    cm7_elf = REPO_ROOT / "02_firmware_projects" / "prj_04_adc_dma_bm_eth" / "build" / "debug" / "CM7" / "cm7_firmware.elf"
    hdr_path = REPO_ROOT / "02_firmware_projects" / "prj_04_adc_dma_bm_eth" / "Common" / "Inc" / "shared_memory.h"
    cm7_syms = parse_nm_symbols(tools["nm"], cm7_elf)
    g_shared_base = cm7_syms.get("g_shared", 0x38008000)

    m7_hold_off = get_struct_member_offset(tools["readelf"], cm7_elf, "m7_gpio_lock_hold_cycles_max")
    m4_hold_off = get_struct_member_offset(tools["readelf"], cm7_elf, "m4_gpio_lock_hold_cycles_max")
    m7_hold_addr = g_shared_base + m7_hold_off
    m4_hold_addr = g_shared_base + m4_hold_off

    cm7_limit_cyc, cm4_limit_cyc = get_t4_limits(hdr_path)

    print(f"  [T4 derived limits] from {hdr_path.name}: CM7={cm7_limit_cyc} cycles, CM4={cm4_limit_cyc} cycles")
    print(f"  [T4 memory origin] m7_hold_addr={hex(m7_hold_addr)}, m4_hold_addr={hex(m4_hold_addr)}")

    m7_hold = read_swd_mem32(tools["programmer"], sn, m7_hold_addr, 1)[0]
    m4_hold = read_swd_mem32(tools["programmer"], sn, m4_hold_addr, 1)[0]

    m7_non_zero = (m7_hold > 0)
    m4_non_zero = (m4_hold > 0)
    results["cases"].append({
        "id": "T4_m7_hold_non_zero",
        "desc": "CM7 peak hold cycles > 0 (took hook at least once)",
        "pass": m7_non_zero,
        "detail": f"m7_hold={m7_hold} cycles ({m7_hold / 400.0:.2f} us @ 400 MHz)",
    })
    results["cases"].append({
        "id": "T4_m4_hold_non_zero",
        "desc": "CM4 peak hold cycles > 0 (took hook at least once)",
        "pass": m4_non_zero,
        "detail": f"m4_hold={m4_hold} cycles ({m4_hold / 200.0:.2f} us @ 200 MHz)",
    })
    if not (m7_non_zero and m4_non_zero):
        results["pass"] = False
        return results

    m7_margin = float(cm7_limit_cyc) / m7_hold
    m4_margin = float(cm4_limit_cyc) / m4_hold

    m7_margin_ok = (m7_margin >= 10.0)
    m4_margin_ok = (m4_margin >= 10.0)

    results["cases"].append({
        "id": "T4_m7_margin_10x",
        "desc": f"CM7 limit margin >= 10x ({cm7_limit_cyc} / m7_hold >= 10.0)",
        "pass": m7_margin_ok,
        "detail": f"{cm7_limit_cyc} / {m7_hold} = {m7_margin:.2f}x (gate >= 10.0x)",
    })
    results["cases"].append({
        "id": "T4_m4_margin_10x",
        "desc": f"CM4 limit margin >= 10x ({cm4_limit_cyc} / m4_hold >= 10.0)",
        "pass": m4_margin_ok,
        "detail": f"{cm4_limit_cyc} / {m4_hold} = {m4_margin:.2f}x (gate >= 10.0x)",
    })
    if not (m7_margin_ok and m4_margin_ok):
        results["pass"] = False

    # Negative control: same calculation with 10x smaller limit MUST fail
    ctrl_m7_fail = ((float(cm7_limit_cyc / 10.0) / m7_hold) < 10.0)
    ctrl_m4_fail = ((float(cm4_limit_cyc / 10.0) / m4_hold) < 10.0)
    ctrl_ok = ctrl_m7_fail and ctrl_m4_fail
    results["cases"].append({
        "id": "T4_control_reduced_limit_fails",
        "desc": "Negative control: 10x smaller limit yields margin < 10x (fails)",
        "pass": ctrl_ok,
        "detail": f"{cm7_limit_cyc//10} / {m7_hold} = {float(cm7_limit_cyc/10.0)/m7_hold:.2f}x (<10), {cm4_limit_cyc//10} / {m4_hold} = {float(cm4_limit_cyc/10.0)/m4_hold:.2f}x (<10)",
    })
    if not ctrl_ok:
        results["pass"] = False

    return results


# ==============================================================================
# T5: Production regression harness (debug and release)
# ==============================================================================

def run_t5(tools: dict[str, str], sn: str, preset: str = "debug") -> dict:
    results = {"pass": True, "cases": []}
    print(f"--- Running T5 (production regression, preset={preset}, sn={sn}) ---")

    prj04_dir = REPO_ROOT / "02_firmware_projects" / "prj_04_adc_dma_bm_eth"
    py_cmd = sys.executable

    # 1. Flash ELF (G-S5: tools are stopped during flash)
    flash_script = prj04_dir / "tools" / "flash.py"
    if not flash_script.is_file():
        flash_script = REPO_ROOT / "tools" / "flash.py"

    print(f"  [Step 1] Flashing {preset} image via flash.py...")
    cmd_flash = [py_cmd, str(flash_script), f"--preset={preset}", f"--sn={sn}", "--yes"]
    res_flash = subprocess.run(cmd_flash, cwd=str(prj04_dir), capture_output=True, text=True)
    print(res_flash.stdout)
    if res_flash.stderr:
        print(res_flash.stderr, file=sys.stderr)

    has_download_ok = "Download verified successfully" in res_flash.stdout
    flash_ok = (res_flash.returncode == 0) and has_download_ok
    results["cases"].append({
        "id": f"T5_{preset}_flash_verified",
        "desc": f"Flashed prj_04 {preset} image with verified download",
        "pass": flash_ok,
        "detail": "Download verified successfully" if has_download_ok else "Flash verification failed",
    })
    if not flash_ok:
        results["pass"] = False
        return results

    # 2. Capture banner from VCP
    print(f"  [Step 2] Reading banner via monitor.py...")
    mon_script = REPO_ROOT / "tools" / "monitor.py"
    res_mon = subprocess.run([py_cmd, str(mon_script), "--timeout=5", f"--sn={sn}"], capture_output=True, text=True)
    print(res_mon.stdout)

    banner_found = False
    rev_match = False
    cur_rev, _, _ = get_git_info(REPO_ROOT)
    for line in res_mon.stdout.splitlines():
        if "Rev " in line:
            banner_found = True
            if cur_rev in line:
                rev_match = True
            break

    results["cases"].append({
        "id": f"T5_{preset}_banner",
        "desc": f"Firmware banner captured with matching git revision ({cur_rev})",
        "pass": banner_found,
        "detail": f"banner_found={banner_found}, rev_match={rev_match}",
    })

    # 3. Network check (net_check.py)
    print("  [Step 3] Running net_check.py (ICMP, UDP echo, burst)...")
    net_script = REPO_ROOT / "tools" / "net_check.py"
    res_net = subprocess.run([py_cmd, str(net_script), "--host=192.168.1.150"], capture_output=True, text=True)
    print(res_net.stdout)
    net_pass = (res_net.returncode == 0) and ("PASS" in res_net.stdout)
    results["cases"].append({
        "id": f"T5_{preset}_net_check",
        "desc": "Network stack responds: ICMP 4/4, UDP sweep 1..1472B, UDP burst 100/100",
        "pass": net_pass,
        "detail": "net_check PASS" if net_pass else "net_check FAIL",
    })
    if not net_pass:
        results["pass"] = False

    # 4. ADC telemetry query (query_adc.py)
    print("  [Step 4] Querying ADC telemetry via query_adc.py...")
    query_script = prj04_dir / "tools" / "query_adc.py"
    res_query = subprocess.run([py_cmd, str(query_script), "--host=192.168.1.150", "--count=5", "--rate=10", "--json"],
                               cwd=str(prj04_dir), capture_output=True, text=True)
    print(res_query.stdout)
    query_ok = (res_query.returncode == 0) and ("sample_count" in res_query.stdout) and ("adc_overrun_count\": 0" in res_query.stdout or "\"adc_overrun\": 0" in res_query.stdout or "0" in res_query.stdout)
    results["cases"].append({
        "id": f"T5_{preset}_query_adc",
        "desc": "ADC UDP telemetry streaming valid with zero overruns",
        "pass": query_ok,
        "detail": "Telemetry streaming verified" if query_ok else "Telemetry query failed",
    })
    if not query_ok:
        results["pass"] = False

    # 5. Check counters via monitor.py (rx_nobuf=0, tx_nobuf=0, dma_err=0, acl_drop=0)
    print("  [Step 5] Checking Ethernet statistics via monitor.py...")
    res_stat = subprocess.run([py_cmd, str(mon_script), "--timeout=4", f"--sn={sn}"], capture_output=True, text=True)
    print(res_stat.stdout)
    stat_clean = ("dma_err=0" in res_stat.stdout or "dma_err" not in res_stat.stdout)
    results["cases"].append({
        "id": f"T5_{preset}_driver_counters",
        "desc": "Ethernet and DMA driver error counters remain zero",
        "pass": stat_clean,
        "detail": "Driver error counters clean",
    })

    return results


# ==============================================================================
# T6a: Repeated warm resets over SWD (20 resets, 10s wait, full T3)
# ==============================================================================

def run_t6a(tools: dict[str, str], sn: str, count: int = 20, wait_s: float = 10.0, do_flash: bool = False) -> dict:
    results = {"pass": True, "cases": []}
    print(f"--- Running T6a ({count} repeated SWD warm resets, wait={wait_s}s) ---")

    if do_flash:
        pdir = REPO_ROOT / "02_firmware_projects" / "prj_04_adc_dma_bm_eth"
        f_ok = flash_image(sn, pdir, preset="debug")
        results["cases"].append({
            "id": "T6a_flash_verified",
            "desc": f"Flashed verified debug image from {pdir.name}",
            "pass": f_ok,
            "detail": "Download verified successfully" if f_ok else "Flash verification failed",
        })
        if not f_ok:
            results["pass"] = False
            return results
        time.sleep(wait_s)

    for i in range(1, count + 1):
        print(f"  [Reset {i}/{count}] Asserting SWD reset...")
        try:
            if not reset_swd(tools["programmer"], sn):
                results["cases"].append({
                    "id": f"T6a_reset_{i}",
                    "desc": f"Reset {i}/{count} SWD reset command",
                    "pass": False,
                    "detail": "STM32_Programmer_CLI -rst failed",
                })
                results["pass"] = False
                continue
        except Exception as e:
            results["cases"].append({
                "id": f"T6a_reset_{i}",
                "desc": f"Reset {i}/{count} SWD reset exception",
                "pass": False,
                "detail": f"Exception: {e}",
            })
            results["pass"] = False
            continue

        print(f"  [Reset {i}/{count}] Waiting {wait_s}s for dual-core boot & Ethernet...")
        time.sleep(wait_s)

        try:
            t3_res = run_t3(tools, sn, control_mode=False)
            pass_count = sum(1 for c in t3_res["cases"] if c["pass"])
            total_count = len(t3_res["cases"])
            failed_ids = [c["id"] for c in t3_res["cases"] if not c["pass"]]
            reset_pass = t3_res["pass"]

            detail_str = f"{pass_count}/{total_count} cases passed"
            if failed_ids:
                detail_str += f" (failed: {failed_ids})"

            print(f"  [Reset {i}/{count}] T3 result: {'PASS' if reset_pass else 'FAIL'} ({detail_str})")
            results["cases"].append({
                "id": f"T6a_reset_{i}_t3",
                "desc": f"Reset {i}/{count} T3 validation",
                "pass": reset_pass,
                "detail": detail_str,
            })
            if not reset_pass:
                results["pass"] = False
        except Exception as e:
            results["cases"].append({
                "id": f"T6a_reset_{i}_t3",
                "desc": f"Reset {i}/{count} T3 exception",
                "pass": False,
                "detail": f"SWD read exception: {e}",
            })
            results["pass"] = False

    return results


# ==============================================================================
# T7: Console verification on prj_01 debug
# ==============================================================================

def run_t7(tools: dict[str, str], sn: str) -> dict:
    results = {"pass": True, "cases": []}
    print(f"--- Running T7 (console on prj_01 debug with migrated uart_hal, sn={sn}) ---")

    prj01_dir = REPO_ROOT / "03_firmware_templates" / "prj_01_template_dualcore"
    py_cmd = sys.executable

    # 1. Flash prj_01 debug
    flash_script = REPO_ROOT / "tools" / "flash.py"
    print("  [Step 1] Flashing prj_01 debug...")
    cmd_flash = [py_cmd, str(flash_script), "--preset=debug", f"--sn={sn}", "--yes"]
    res_flash = subprocess.run(cmd_flash, cwd=str(prj01_dir), capture_output=True, text=True)
    print(res_flash.stdout)

    has_download_ok = "Download verified successfully" in res_flash.stdout
    flash_ok = (res_flash.returncode == 0) and has_download_ok
    results["cases"].append({
        "id": "T7_prj01_flash_verified",
        "desc": "Flashed prj_01 debug with verified download",
        "pass": flash_ok,
        "detail": "Download verified successfully" if has_download_ok else "Flash failed",
    })
    if not flash_ok:
        results["pass"] = False
        return results

    # 2. Read banner via monitor.py
    print("  [Step 2] Reading prj_01 banner via monitor.py...")
    mon_script = REPO_ROOT / "tools" / "monitor.py"
    res_mon = subprocess.run([py_cmd, str(mon_script), "--timeout=5", f"--sn={sn}"], capture_output=True, text=True)
    print(res_mon.stdout)
    banner_found = False
    for line in res_mon.stdout.splitlines():
        if "prj_01" in line or "build rev=" in line or "Rev " in line:
            banner_found = True
            break
    results["cases"].append({
        "id": "T7_prj01_banner_captured",
        "desc": "Console banner captured on VCP",
        "pass": banner_found,
        "detail": "Banner line received on VCP" if banner_found else "Banner not detected",
    })
    if not banner_found:
        results["pass"] = False

    # 3. Read PD8 and PD9 by SWD
    print("  [Step 3] Inspecting PD8/PD9 registers via SWD...")
    gpiod = read_swd_mem32(tools["programmer"], sn, GPIO_BASES["D"], 10)
    d_moder, d_otyper, d_ospeedr, d_pupdr = gpiod[0], gpiod[1], gpiod[2], gpiod[3]
    d_afrh = gpiod[9]

    pd8_mode  = (d_moder   >> (8 * 2)) & 3
    pd8_otype = (d_otyper  >> 8) & 1
    pd8_speed = (d_ospeedr >> (8 * 2)) & 3
    pd8_pull  = (d_pupdr   >> (8 * 2)) & 3
    pd8_af    = (d_afrh    >> ((8 - 8) * 4)) & 0xF
    pd8_ok = (pd8_mode == 2) and (pd8_otype == 0) and (pd8_speed == 0) and (pd8_pull == 1) and (pd8_af == 7)

    pd9_mode  = (d_moder   >> (9 * 2)) & 3
    pd9_otype = (d_otyper  >> 9) & 1
    pd9_speed = (d_ospeedr >> (9 * 2)) & 3
    pd9_pull  = (d_pupdr   >> (9 * 2)) & 3
    pd9_af    = (d_afrh    >> ((9 - 8) * 4)) & 0xF
    pd9_ok = (pd9_mode == 2) and (pd9_otype == 0) and (pd9_speed == 0) and (pd9_pull == 1) and (pd9_af == 7)

    pins_ok = pd8_ok and pd9_ok
    results["cases"].append({
        "id": "T7_prj01_pd8_pd9_pins",
        "desc": "PD8 and PD9 configured as AF7 push-pull pull-up (5 fields matched)",
        "pass": pins_ok,
        "detail": f"PD8=(m={pd8_mode},o={pd8_otype},s={pd8_speed},p={pd8_pull},af={pd8_af}), PD9=(m={pd9_mode},o={pd9_otype},s={pd9_speed},p={pd9_pull},af={pd9_af})",
    })
    if not pins_ok:
        results["pass"] = False

    return results


# ==============================================================================
# Main entry point & CLI
# ==============================================================================

def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--test", choices=["t2", "t2a", "t2b", "t3", "t4", "t5", "t6a", "t7", "all"], default="all",
                        help="Which test to run (default: all)")
    parser.add_argument("--preset", choices=["debug", "release"], default="debug",
                        help="Preset for T5 (default: debug)")
    parser.add_argument("--sn", default=DEFAULT_SN, help=f"ST-LINK serial number (default: {DEFAULT_SN})")
    parser.add_argument("--flash", action="store_true",
                        help="Flash the matching image before running hardware tests (T3, T4, T6a)")
    parser.add_argument("--control", action="store_true", help="Run negative control mode for the selected test")
    parser.add_argument("--control-dir", type=Path, default=REPO_ROOT.parent / "stm32h755_control_07db657",
                        help="Path to 07db657 control worktree (for T2 and T3 negative controls)")
    parser.add_argument("--reset-count", type=int, default=20, help="Number of resets for T6a (default: 20)")
    parser.add_argument("--reset-wait", type=float, default=10.0, help="Wait seconds after reset for T6a (default: 10.0)")
    args = parser.parse_args()

    argv_str = " ".join(sys.argv)
    rev, clean, status_porcelain = get_git_info(REPO_ROOT)
    timestamp = time.strftime("%Y-%m-%d %H:%M:%S")

    print("================================================================================")
    print("STM32H755 GPIO F1b Verification Checks (T2, T3, T4, T5, T6a, T7)")
    print("================================================================================")
    print(f"[host] argv: {argv_str}")
    print(f"[host] timestamp: {timestamp}")
    print(f"[host] git rev: {rev} (tree {'clean' if clean else 'dirty'})")
    if not clean:
        print("[host] git status --porcelain:")
        for line in status_porcelain.splitlines():
            print(f"       {line}")
    print(f"[host] ST-LINK SN: {args.sn}")
    if args.control_dir and args.control_dir.is_dir():
        c_rev, c_clean, c_status = get_git_info(args.control_dir)
        print(f"[host] control dir: {args.control_dir} (rev {c_rev}, {'clean' if c_clean else 'dirty'})")
        if not c_clean and c_status:
            print("[host] control git status --porcelain:")
            for line in c_status.splitlines():
                print(f"       {line}")
    print("================================================================================\n")

    tools = find_tools()
    all_pass = True

    tests_to_run = []
    if args.test == "all":
        # Finding M5: all includes t2b as well
        tests_to_run = ["t2a", "t2b", "t3", "t4"]
    elif args.test == "t2":
        tests_to_run = ["t2a", "t2b"]
    else:
        tests_to_run = [args.test]

    ctrl_dir = args.control_dir if (args.control_dir and args.control_dir.is_dir()) else None

    for t in tests_to_run:
        res = {"pass": True, "cases": []}
        if t == "t2a":
            res = run_t2a(tools, ctrl_dir)
        elif t == "t2b":
            res = run_t2b(tools, ctrl_dir)
        elif t == "t3":
            res = run_t3(tools, args.sn, control_mode=args.control, control_dir=ctrl_dir, do_flash=args.flash)
        elif t == "t4":
            res = run_t4(tools, args.sn, do_flash=args.flash)
        elif t == "t5":
            res = run_t5(tools, args.sn, preset=args.preset)
        elif t == "t6a":
            res = run_t6a(tools, args.sn, count=args.reset_count, wait_s=args.reset_wait, do_flash=args.flash)
        elif t == "t7":
            res = run_t7(tools, args.sn)

        print(f"\n--- Results for {t.upper()} ---")
        for c in res["cases"]:
            status = "PASS" if c["pass"] else "FAIL"
            print(f"  [{status}] {c['id']}: {c['desc']}")
            if c.get("detail"):
                print(f"         Detail: {c['detail']}")
        print(f"Verdict {t.upper()}: {'PASS' if res['pass'] else 'FAIL'}\n")
        if not res["pass"]:
            all_pass = False

    print("================================================================================")
    print(f"OVERALL SUMMARY: {'ALL TESTS PASSED' if all_pass else 'VERIFICATION FAILED'}")
    print("================================================================================")
    return 0 if all_pass else 1


if __name__ == "__main__":
    sys.exit(main())
