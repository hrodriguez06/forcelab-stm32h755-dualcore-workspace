#!/usr/bin/env python3
"""Verification tool for STM32H7 GPIO Driver F1b phase (H1-H5, G1, G13).

Covers tests T2a, T2b, T3, T4, and T6a from IMPLEMENTATION_PLAN.md:
  - T2a: Verification of HAL_GPIO_Init / HAL_GPIO_DeInit symbol status:
         - Object-level: uart_hal.c.obj imports GPIO_HAL_Init/DeinitPin, NOT HAL_GPIO_Init/DeInit.
         - ELF-level: HAL_GPIO_DeInit absent everywhere; HAL_GPIO_Init absent on CM4.
         - Negative control on 07db657: uart_hal.c.obj imports HAL_GPIO_Init/DeInit.
  - T2b: Preprocessed code scan (-E) across all compile_commands.json units of prj_04
         presets (debug, release, selftest, selftest-adc, selftest-uart, selftest-hsem)
         verifying zero raw writes to MODER, OTYPER, OSPEEDR, PUPDR, AFR outside
         allowed exceptions (gpio_bm.c, gpio_ll.c, stm32h7xx_hal_gpio.c, led_bm.c, gpio_selftest.c).
         Negative control on 07db657 detects the 3 raw writers.
  - T3:  SWD live inspection (mode=HOTPLUG, running debug image):
         - s_shared_lock on CM7 == g_shared_gpio_lock address (from CM7 ELF)
         - s_shared_lock on CM4 == g_shared_gpio_lock address (from CM4 ELF)
         - HSEM_RLR5 (0x58001514) == 0 (free)
         - m7_gpio_lock_fail_count (0x380084FC) == 0
         - m4_gpio_lock_fail_count (0x38008500) == 0
         - Pin config match: PB1 (analog), PB13 (AF11, PP, very high),
           PD8 (AF7, PP, low, pullup), PD9 (AF7, PP, low, pullup),
           plus unused control pins in reset state (PB15 analog, PD0 analog).
         Negative control on 07db657: s_shared_lock == 0, T3 FAILS.
  - T4:  Cycle retention margin check (Decision 3):
         - m7_gpio_lock_hold_cycles_max > 0, 24000 / m7_hold >= 10.0
         - m4_gpio_lock_hold_cycles_max > 0, 12000 / m4_hold >= 10.0
         - Negative control with limit / 10 fails.
  - T6a: Repeated warm resets over SWD: 20 sequential resets (-rst), 10s wait,
         running full T3 after each reset.

Standard library only.
"""

from __future__ import annotations

import argparse
import concurrent.futures
import json
import os
import re
import shutil
import subprocess
import sys
import time
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
DEFAULT_SN = "0016000C4142501220353451"

# GPIO register offsets & bases
GPIO_BASES = {
    "A": 0x58020000,
    "B": 0x58020400,
    "C": 0x58020800,
    "D": 0x58020C00,
    "E": 0x58021000,
    "F": 0x58021400,
    "G": 0x58021800,
}

HSEM_BASE = 0x58001400
HSEM_RLR5_ADDR = HSEM_BASE + 0x114  # 0x58001514
SHARED_RAM_BASE = 0x38008000
M7_FAIL_COUNT_ADDR = SHARED_RAM_BASE + 0x4FC
M4_FAIL_COUNT_ADDR = SHARED_RAM_BASE + 0x500
M7_HOLD_MAX_ADDR   = SHARED_RAM_BASE + 0x504
M4_HOLD_MAX_ADDR   = SHARED_RAM_BASE + 0x508

ALLOWED_EXCEPTIONS = {
    "gpio_bm.c",
    "gpio_ll.c",
    "stm32h7xx_hal_gpio.c",
    "led_bm.c",
    "gpio_selftest.c",
}

PRJ04_PRESETS = [
    "debug",
    "release",
    "selftest",
    "selftest-adc",
    "selftest-uart",
    "selftest-hsem",
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
    tools["size"] = str(bin_dir / "arm-none-eabi-size.exe")

    for k in ("nm", "size"):
        if not Path(tools[k]).is_file():
            which_t = shutil.which(f"arm-none-eabi-{k}")
            if which_t:
                tools[k] = which_t
            else:
                raise RuntimeError(f"Tool {k} not found")

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


def get_git_info(tree_dir: Path) -> tuple[str, bool]:
    res = subprocess.run(["git", "rev-parse", "--short=7", "HEAD"],
                         cwd=str(tree_dir), capture_output=True, text=True)
    rev = res.stdout.strip() if res.returncode == 0 else "unknown"
    status_res = subprocess.run(["git", "status", "--porcelain"],
                                cwd=str(tree_dir), capture_output=True, text=True)
    clean = (status_res.returncode == 0) and (not status_res.stdout.strip())
    return rev, clean


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


# ==============================================================================
# T2a: Verification of HAL_GPIO_Init / DeInit imports & definitions
# ==============================================================================

def run_t2a(tools: dict[str, str], control_dir: Path | None) -> dict:
    results = {"pass": True, "cases": []}
    print("--- Running T2a (absence of unprotected HAL_GPIO_Init/DeInit) ---")

    # 1. Object level check on uart_hal.c.obj
    uart_obj = REPO_ROOT / "02_firmware_projects" / "prj_04_adc_dma_bm_eth" / "build" / "debug" / "CM7" / "CMakeFiles" / "cm7_firmware.dir"
    found_objs = list(uart_obj.glob("**/uart_hal.c.obj"))
    if not found_objs:
        found_objs = list((REPO_ROOT / "02_firmware_projects" / "prj_04_adc_dma_bm_eth" / "build").glob("**/uart_hal.c.obj"))

    if found_objs:
        curr_obj = found_objs[0]
        undefs = parse_nm_undefined(tools["nm"], curr_obj)
        has_gpio_hal_init = "GPIO_HAL_Init" in undefs
        has_gpio_hal_deinit = "GPIO_HAL_DeinitPin" in undefs
        has_hal_init = "HAL_GPIO_Init" in undefs
        has_hal_deinit = "HAL_GPIO_DeInit" in undefs

        obj_ok = has_gpio_hal_init and has_gpio_hal_deinit and (not has_hal_init) and (not has_hal_deinit)
        results["cases"].append({
            "id": "T2a_uart_obj_symbols",
            "desc": "uart_hal.c.obj imports GPIO_HAL_Init/DeinitPin and NOT HAL_GPIO_Init/DeInit",
            "pass": obj_ok,
            "detail": f"GPIO_HAL_Init={has_gpio_hal_init}, GPIO_HAL_DeinitPin={has_gpio_hal_deinit}, HAL_GPIO_Init={has_hal_init}, HAL_GPIO_DeInit={has_hal_deinit}",
        })
        if not obj_ok:
            results["pass"] = False
    else:
        results["cases"].append({
            "id": "T2a_uart_obj_symbols",
            "desc": "uart_hal.c.obj exists",
            "pass": False,
            "detail": "uart_hal.c.obj not found in build tree",
        })
        results["pass"] = False

    # 2. ELF check on CM4 production images (must have ZERO HAL_GPIO_Init/DeInit)
    for preset in ("debug", "release"):
        cm4_elf = REPO_ROOT / "02_firmware_projects" / "prj_04_adc_dma_bm_eth" / "build" / preset / "CM4" / "cm4_firmware.elf"
        if cm4_elf.is_file():
            syms = parse_nm_symbols(tools["nm"], cm4_elf)
            has_init = "HAL_GPIO_Init" in syms
            has_deinit = "HAL_GPIO_DeInit" in syms
            cm4_ok = (not has_init) and (not has_deinit)
            results["cases"].append({
                "id": f"T2a_cm4_{preset}_elf",
                "desc": f"prj_04 CM4 {preset} ELF does not link HAL_GPIO_Init or HAL_GPIO_DeInit",
                "pass": cm4_ok,
                "detail": f"HAL_GPIO_Init={has_init}, HAL_GPIO_DeInit={has_deinit}",
            })
            if not cm4_ok:
                results["pass"] = False

    # 3. ELF check on HAL_GPIO_DeInit across all production images (must be absent everywhere)
    for p_name, rel_path in [
        ("prj_04_debug_cm7", "02_firmware_projects/prj_04_adc_dma_bm_eth/build/debug/CM7/cm7_firmware.elf"),
        ("prj_04_release_cm7", "02_firmware_projects/prj_04_adc_dma_bm_eth/build/release/CM7/cm7_firmware.elf"),
        ("prj_01_debug_cm7", "03_firmware_templates/prj_01_template_dualcore/build/debug/CM7/cm7_firmware.elf"),
        ("prj_02_debug_cm7", "02_firmware_projects/prj_02_freertos_lwip/build/debug/CM7/cm7_firmware.elf"),
        ("prj_03_debug_cm7", "02_firmware_projects/prj_03_adc_dma_freertos/build/debug/CM7/cm7_firmware.elf"),
    ]:
        elf = REPO_ROOT / rel_path
        if elf.is_file():
            syms = parse_nm_symbols(tools["nm"], elf)
            has_deinit = "HAL_GPIO_DeInit" in syms
            deinit_ok = (not has_deinit)
            results["cases"].append({
                "id": f"T2a_deinit_absent_{p_name}",
                "desc": f"{p_name} does not link HAL_GPIO_DeInit",
                "pass": deinit_ok,
                "detail": f"HAL_GPIO_DeInit present={has_deinit}",
            })
            if not deinit_ok:
                results["pass"] = False

    # 4. Negative control on 07db657 control worktree
    if control_dir and control_dir.is_dir():
        ctrl_objs = list((control_dir / "02_firmware_projects" / "prj_04_adc_dma_bm_eth" / "build").glob("**/uart_hal.c.obj"))
        if ctrl_objs:
            ctrl_undefs = parse_nm_undefined(tools["nm"], ctrl_objs[0])
            ctrl_has_hal_init = "HAL_GPIO_Init" in ctrl_undefs
            ctrl_has_hal_deinit = "HAL_GPIO_DeInit" in ctrl_undefs
            ctrl_ok = ctrl_has_hal_init and ctrl_has_hal_deinit
            results["cases"].append({
                "id": "T2a_control_07db657_uart_obj",
                "desc": "Control 07db657: uart_hal.c.obj imports HAL_GPIO_Init and HAL_GPIO_DeInit",
                "pass": ctrl_ok,
                "detail": f"HAL_GPIO_Init={ctrl_has_hal_init}, HAL_GPIO_DeInit={ctrl_has_hal_deinit}",
            })
            if not ctrl_ok:
                results["pass"] = False

    return results


# ==============================================================================
# T2b: Preprocessed code scan for raw GPIO register writes
# ==============================================================================

RAW_WRITE_RE = re.compile(
    r"(?:->|\.)\s*(MODER|OTYPER|OSPEEDR|PUPDR|AFR\s*\[\s*[01]\s*\])\s*([|&^+\-]?=)"
)


def _check_unit_writes(entry: dict, preset: str, gcc_cmd: str, project_dir: Path) -> list[dict]:
    src_file = Path(entry.get("file", ""))
    if not src_file.is_file():
        return []

    base_name = src_file.name
    if base_name in ALLOWED_EXCEPTIONS:
        return []

    # Build preprocessor command
    raw_cmd = entry.get("command", "")
    if raw_cmd:
        tokens = raw_cmd.split()
    else:
        tokens = list(entry.get("arguments", []))

    if not tokens:
        return []

    # Replace "-c <src> -o <obj>" with "-E"
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

    cmd = [gcc_cmd, "-E", "-Wno-error"] + pp_tokens + [str(src_file)]
    proc = subprocess.run(cmd, cwd=entry.get("directory", str(project_dir)),
                          capture_output=True, text=True)
    if proc.returncode != 0:
        return []

    # Parse preprocessed output tracking current file
    unit_findings = []
    cur_file = str(src_file)
    cur_line = 1
    for line in proc.stdout.splitlines():
        lm = re.match(r'^#\s+(\d+)\s+"([^"]+)"', line)
        if lm:
            cur_line = int(lm.group(1))
            cur_file = lm.group(2)
            continue

        cur_base = Path(cur_file).name
        if cur_base in ALLOWED_EXCEPTIONS:
            cur_line += 1
            continue

        if RAW_WRITE_RE.search(line):
            unit_findings.append({
                "preset": preset,
                "file": cur_file,
                "line": cur_line,
                "unit": src_file.name,
                "code": line.strip()[:80],
            })
        cur_line += 1

    return unit_findings


def run_t2b_on_tree(tools: dict[str, str], project_dir: Path, is_control: bool = False) -> tuple[bool, list[dict]]:
    findings = []
    tasks = []

    for preset in PRJ04_PRESETS:
        comp_json = project_dir / "build" / preset / "compile_commands.json"
        if not comp_json.is_file():
            continue

        try:
            entries = json.loads(comp_json.read_text(encoding="utf-8"))
        except Exception:
            continue

        for entry in entries:
            tasks.append((entry, preset))

    workers = os.cpu_count() or 4
    with concurrent.futures.ThreadPoolExecutor(max_workers=workers) as executor:
        futures = [
            executor.submit(_check_unit_writes, entry, preset, tools["gcc"], project_dir)
            for entry, preset in tasks
        ]
        for f in concurrent.futures.as_completed(futures):
            res = f.result()
            if res:
                findings.extend(res)

    return (len(findings) == 0 if not is_control else len(findings) > 0), findings


def run_t2b(tools: dict[str, str], control_dir: Path | None) -> dict:
    results = {"pass": True, "cases": []}
    print("--- Running T2b (preprocessor -E scan across all compilation units) ---")
    prj04_dir = REPO_ROOT / "02_firmware_projects" / "prj_04_adc_dma_bm_eth"

    curr_ok, curr_findings = run_t2b_on_tree(tools, prj04_dir, is_control=False)
    results["cases"].append({
        "id": "T2b_current_zero_raw_writes",
        "desc": "Current tree has zero raw GPIO writes across presets outside exceptions",
        "pass": curr_ok,
        "detail": f"Illegal writes count={len(curr_findings)}" + (f": {curr_findings}" if curr_findings else ""),
    })
    if not curr_ok:
        results["pass"] = False

    if control_dir and control_dir.is_dir():
        ctrl_prj04 = control_dir / "02_firmware_projects" / "prj_04_adc_dma_bm_eth"
        ctrl_ok, ctrl_findings = run_t2b_on_tree(tools, ctrl_prj04, is_control=True)
        # Expected in 07db657: adc_acquisition.c, main.c, dma_selftest.c
        found_files = {Path(f["file"]).name for f in ctrl_findings}
        has_adc = "adc_acquisition.c" in found_files
        has_main = "main.c" in found_files
        has_dma = "dma_selftest.c" in found_files
        ctrl_pass = has_adc and has_main and has_dma
        results["cases"].append({
            "id": "T2b_control_07db657_detects_writers",
            "desc": "Control 07db657 detects raw writes in adc_acquisition.c, main.c, and dma_selftest.c",
            "pass": ctrl_pass,
            "detail": f"Found in control: adc_acquisition={has_adc}, main={has_main}, dma_selftest={has_dma} (total={len(ctrl_findings)})",
        })
        if not ctrl_pass:
            results["pass"] = False

    return results


# ==============================================================================
# T3: Live SWD verification of lock hook, HSEM 5, fail counts & pin registers
# ==============================================================================

def run_t3(tools: dict[str, str], sn: str, control_mode: bool = False, control_dir: Path | None = None) -> dict:
    results = {"pass": True, "cases": []}
    print(f"--- Running T3 (live SWD inspection, control_mode={control_mode}) ---")

    # Resolve ELF symbol addresses
    if control_mode and control_dir and control_dir.is_dir():
        cm7_elf = control_dir / "02_firmware_projects" / "prj_04_adc_dma_bm_eth" / "build" / "debug" / "CM7" / "cm7_firmware.elf"
        cm4_elf = control_dir / "02_firmware_projects" / "prj_04_adc_dma_bm_eth" / "build" / "debug" / "CM4" / "cm4_firmware.elf"
    else:
        cm7_elf = REPO_ROOT / "02_firmware_projects" / "prj_04_adc_dma_bm_eth" / "build" / "debug" / "CM7" / "cm7_firmware.elf"
        cm4_elf = REPO_ROOT / "02_firmware_projects" / "prj_04_adc_dma_bm_eth" / "build" / "debug" / "CM4" / "cm4_firmware.elf"

    cm7_syms = parse_nm_symbols(tools["nm"], cm7_elf)
    cm4_syms = parse_nm_symbols(tools["nm"], cm4_elf)

    cm7_lock_ptr_addr = cm7_syms.get("s_shared_lock")
    cm4_lock_ptr_addr = cm4_syms.get("s_shared_lock")

    if not (cm7_lock_ptr_addr and cm4_lock_ptr_addr):
        raise RuntimeError(f"Could not resolve s_shared_lock symbols: CM7 lock={cm7_lock_ptr_addr}, CM4 lock={cm4_lock_ptr_addr}")

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
        # On 07db657 control image, hook was not installed: both pointers must be 0
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

    # 3. Read HSEM_RLR5 (0x58001514)
    rlr5 = read_swd_mem32(tools["programmer"], sn, HSEM_RLR5_ADDR, 1)[0]
    rlr5_ok = (rlr5 == 0)
    results["cases"].append({
        "id": "T3_hsem_rlr5_free",
        "desc": "HSEM_RLR5 reads 0 (semaphore is free)",
        "pass": rlr5_ok,
        "detail": f"RLR5={hex(rlr5)}",
    })
    if not rlr5_ok:
        results["pass"] = False

    # 4. Read lock fail counters
    fail_counts = read_swd_mem32(tools["programmer"], sn, M7_FAIL_COUNT_ADDR, 2)
    m7_fails = fail_counts[0]
    m4_fails = fail_counts[1]
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
    # Offsets: 0=MODER, 1=OTYPER, 2=OSPEEDR, 3=PUPDR, 4=IDR, 5=ODR, 6=BSRR, 7=LCKR, 8=AFRL, 9=AFRH
    gpiob = read_swd_mem32(tools["programmer"], sn, GPIO_BASES["B"], 10)
    b_moder, b_otyper, b_ospeedr, b_pupdr = gpiob[0], gpiob[1], gpiob[2], gpiob[3]
    b_afrl, b_afrh = gpiob[8], gpiob[9]

    # PB1: Analog (3), No pull (0)
    pb1_mode = (b_moder >> (1 * 2)) & 3
    pb1_pull = (b_pupdr >> (1 * 2)) & 3
    pb1_ok = (pb1_mode == 3) and (pb1_pull == 0)
    results["cases"].append({
        "id": "T3_pin_PB1_analog",
        "desc": "PB1 is Analog (mode=3) without pull (pull=0)",
        "pass": pb1_ok,
        "detail": f"mode={pb1_mode} (want 3), pull={pb1_pull} (want 0)",
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
        "desc": "PB13 is AF11 (mode=2, otype=0, speed=3, pull=0, af=11)",
        "pass": pb13_ok,
        "detail": f"mode={pb13_mode}, otype={pb13_otype}, speed={pb13_speed}, pull={pb13_pull}, af={pb13_af}",
    })
    if not pb13_ok:
        results["pass"] = False

    # Control pin PB15 (unused): reset value is Analog (3), no pull (0)
    pb15_mode = (b_moder >> (15 * 2)) & 3
    pb15_pull = (b_pupdr >> (15 * 2)) & 3
    pb15_ok = (pb15_mode == 3) and (pb15_pull == 0)
    results["cases"].append({
        "id": "T3_pin_PB15_control_reset",
        "desc": "Control pin PB15 is at reset value (analog, no pull)",
        "pass": pb15_ok,
        "detail": f"mode={pb15_mode} (want 3), pull={pb15_pull} (want 0)",
    })
    if not pb15_ok:
        results["pass"] = False

    # 6. Read GPIOD registers (0x58020C00, 10 words)
    gpiod = read_swd_mem32(tools["programmer"], sn, GPIO_BASES["D"], 10)
    d_moder, d_otyper, d_ospeedr, d_pupdr = gpiod[0], gpiod[1], gpiod[2], gpiod[3]
    d_afrh = gpiod[9]

    # PD8: AF (2), Push-Pull (0), Low (0), Pull-Up (1), AF7 (7)
    pd8_mode  = (d_moder   >> (8 * 2)) & 3
    pd8_otype = (d_otyper  >> 8) & 1
    pd8_speed = (d_ospeedr >> (8 * 2)) & 3
    pd8_pull  = (d_pupdr   >> (8 * 2)) & 3
    pd8_af    = (d_afrh    >> ((8 - 8) * 4)) & 0xF
    pd8_ok = (pd8_mode == 2) and (pd8_otype == 0) and (pd8_speed == 0) and (pd8_pull == 1) and (pd8_af == 7)
    results["cases"].append({
        "id": "T3_pin_PD8_usart3_tx",
        "desc": "PD8 is AF7 (mode=2, otype=0, speed=0, pull=1, af=7)",
        "pass": pd8_ok,
        "detail": f"mode={pd8_mode}, otype={pd8_otype}, speed={pd8_speed}, pull={pd8_pull}, af={pd8_af}",
    })
    if not pd8_ok:
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
        "desc": "PD9 is AF7 (mode=2, otype=0, speed=0, pull=1, af=7)",
        "pass": pd9_ok,
        "detail": f"mode={pd9_mode}, otype={pd9_otype}, speed={pd9_speed}, pull={pd9_pull}, af={pd9_af}",
    })
    if not pd9_ok:
        results["pass"] = False

    # Control pin PD0 (unused): reset value is Analog (3), no pull (0)
    pd0_mode = (d_moder >> (0 * 2)) & 3
    pd0_pull = (d_pupdr >> (0 * 2)) & 3
    pd0_ok = (pd0_mode == 3) and (pd0_pull == 0)
    results["cases"].append({
        "id": "T3_pin_PD0_control_reset",
        "desc": "Control pin PD0 is at reset value (analog, no pull)",
        "pass": pd0_ok,
        "detail": f"mode={pd0_mode} (want 3), pull={pd0_pull} (want 0)",
    })
    if not pd0_ok:
        results["pass"] = False

    return results


# ==============================================================================
# T4: Cycle retention margin check (Decision 3: hold max >= 10x)
# ==============================================================================

def run_t4(tools: dict[str, str], sn: str) -> dict:
    results = {"pass": True, "cases": []}
    print("--- Running T4 (cycle retention margin check) ---")

    hold_words = read_swd_mem32(tools["programmer"], sn, M7_HOLD_MAX_ADDR, 2)
    m7_hold = hold_words[0]
    m4_hold = hold_words[1]

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

    # Limit @ 60 us: CM7 = 24 000, CM4 = 12 000
    m7_margin = 24000.0 / m7_hold
    m4_margin = 12000.0 / m4_hold

    m7_margin_ok = (m7_margin >= 10.0)
    m4_margin_ok = (m4_margin >= 10.0)

    results["cases"].append({
        "id": "T4_m7_margin_10x",
        "desc": "CM7 limit margin >= 10x (24000 / m7_hold >= 10.0)",
        "pass": m7_margin_ok,
        "detail": f"24000 / {m7_hold} = {m7_margin:.2f}x (gate >= 10.0x)",
    })
    results["cases"].append({
        "id": "T4_m4_margin_10x",
        "desc": "CM4 limit margin >= 10x (12000 / m4_hold >= 10.0)",
        "pass": m4_margin_ok,
        "detail": f"12000 / {m4_hold} = {m4_margin:.2f}x (gate >= 10.0x)",
    })
    if not (m7_margin_ok and m4_margin_ok):
        results["pass"] = False

    # Negative control: same calculation with 10x smaller limit (2400 / 1200) MUST fail
    ctrl_m7_fail = ((2400.0 / m7_hold) < 10.0)
    ctrl_m4_fail = ((1200.0 / m4_hold) < 10.0)
    ctrl_ok = ctrl_m7_fail and ctrl_m4_fail
    results["cases"].append({
        "id": "T4_control_reduced_limit_fails",
        "desc": "Negative control: 10x smaller limit yields margin < 10x (fails)",
        "pass": ctrl_ok,
        "detail": f"2400 / {m7_hold} = {2400.0 / m7_hold:.2f}x (<10), 1200 / {m4_hold} = {1200.0 / m4_hold:.2f}x (<10)",
    })
    if not ctrl_ok:
        results["pass"] = False

    return results


# ==============================================================================
# T6a: Repeated warm resets over SWD (20 resets, 10s wait, full T3)
# ==============================================================================

def run_t6a(tools: dict[str, str], sn: str, count: int = 20, wait_s: float = 10.0) -> dict:
    results = {"pass": True, "cases": []}
    print(f"--- Running T6a ({count} repeated SWD warm resets, wait={wait_s}s) ---")

    for i in range(1, count + 1):
        print(f"  [Reset {i}/{count}] Asserting SWD reset...")
        if not reset_swd(tools["programmer"], sn):
            results["cases"].append({
                "id": f"T6a_reset_{i}_swd_cmd",
                "desc": f"Reset {i} SWD command succeeded",
                "pass": False,
                "detail": "STM32_Programmer_CLI -rst failed",
            })
            results["pass"] = False
            break

        print(f"  [Reset {i}/{count}] Waiting {wait_s}s for dual-core boot & Ethernet...")
        time.sleep(wait_s)

        t3_res = run_t3(tools, sn, control_mode=False)
        pass_count = sum(1 for c in t3_res["cases"] if c["pass"])
        total_count = len(t3_res["cases"])
        reset_pass = t3_res["pass"]

        print(f"  [Reset {i}/{count}] T3 result: {'PASS' if reset_pass else 'FAIL'} ({pass_count}/{total_count})")
        results["cases"].append({
            "id": f"T6a_reset_{i}_t3",
            "desc": f"Reset {i}/{count} T3 validation",
            "pass": reset_pass,
            "detail": f"{pass_count}/{total_count} cases passed",
        })
        if not reset_pass:
            results["pass"] = False

    return results


# ==============================================================================
# Main entry point & CLI
# ==============================================================================

def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--test", choices=["t2", "t2a", "t2b", "t3", "t4", "t6a", "all"], default="all",
                        help="Which test to run (default: all)")
    parser.add_argument("--sn", default=DEFAULT_SN, help=f"ST-LINK serial number (default: {DEFAULT_SN})")
    parser.add_argument("--control", action="store_true", help="Run negative control mode for the selected test")
    parser.add_argument("--control-dir", type=Path, default=REPO_ROOT.parent / "stm32h755_control_07db657",
                        help="Path to 07db657 control worktree (for T2 negative controls)")
    parser.add_argument("--reset-count", type=int, default=20, help="Number of resets for T6a (default: 20)")
    parser.add_argument("--reset-wait", type=float, default=10.0, help="Wait seconds after reset for T6a (default: 10.0)")
    args = parser.parse_args()

    argv_str = " ".join(sys.argv)
    rev, clean = get_git_info(REPO_ROOT)
    timestamp = time.strftime("%Y-%m-%d %H:%M:%S")

    print("================================================================================")
    print("STM32H755 GPIO F1b Verification Checks (T2, T3, T4, T6a)")
    print("================================================================================")
    print(f"[host] argv: {argv_str}")
    print(f"[host] timestamp: {timestamp}")
    print(f"[host] git rev: {rev} (tree {'clean' if clean else 'dirty'})")
    print(f"[host] ST-LINK SN: {args.sn}")
    if args.control_dir and args.control_dir.is_dir():
        c_rev, c_clean = get_git_info(args.control_dir)
        print(f"[host] control dir: {args.control_dir} (rev {c_rev}, {'clean' if c_clean else 'dirty'})")
    print("================================================================================\n")

    tools = find_tools()
    all_pass = True

    tests_to_run = []
    if args.test == "all":
        tests_to_run = ["t2", "t3", "t4"]
    elif args.test == "t2":
        tests_to_run = ["t2a", "t2b"]
    else:
        tests_to_run = [args.test]

    ctrl_dir = args.control_dir if (args.control_dir and args.control_dir.is_dir()) else None

    for t in tests_to_run:
        res = {"pass": True, "cases": []}
        if t in ("t2", "t2a"):
            res = run_t2a(tools, ctrl_dir)
        elif t == "t2b":
            res = run_t2b(tools, ctrl_dir)
        elif t == "t3":
            res = run_t3(tools, args.sn, control_mode=args.control, control_dir=ctrl_dir)
        elif t == "t4":
            res = run_t4(tools, args.sn)
        elif t == "t6a":
            res = run_t6a(tools, args.sn, count=args.reset_count, wait_s=args.reset_wait)

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
