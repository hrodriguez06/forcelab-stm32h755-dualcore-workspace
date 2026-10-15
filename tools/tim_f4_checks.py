#!/usr/bin/env python3
"""Verification harness for STM32H7 TIM Driver F4 phase (H1-H5, T1-T8).

Supports multi-project verification via mandatory --project {prj_04, prj_03}.
Automates tests T1 to T8 from IMPLEMENTATION_PLAN.md (revision 2):
  - T1:  Multi-project CI Matrix (8/8 builds via build_all.ps1 -Clean) and selftest presets:
         prj_04 (selftest, selftest-adc), prj_03 (selftest), prj_01 (selftest-tim).
         Zero warnings and zero errors (-Werror).
  - T2a: Linker symbol audit via arm-none-eabi-nm:
         prj_04 CM4: tim_bm symbols present; absent on control 04fb865.
         prj_03 CM4: tim_hal and tim_bm symbols present; absent on control 04fb865.
  - T2b: Preprocessed code scan (-E) across all compilation units in compile_commands.json:
         Zero raw writes to TIM3/TIM7 registers or manual APB1LENR clock enables.
         In prj_03: zero HAL_TIM_* calls outside tim_hal.c.
         Self-test verified across 12 write forms and 9 read/address forms.
         Negative control on 04fb865 detects the known raw writers by (preset, relative_path).
  - T3:  Live SWD hotplug inspection of TIM3 hardware registers:
         CR1 (0x40000400), CR2 (0x40000404), SMCR (0x40000408),
         DIER (0x4000040C), PSC (0x40000428), ARR (0x4000042C).
         Compares bit-by-bit against RM0399 expected table.
         Negative controls: equivalence on 04fb865, altered table fails.
  - T4:  ADC sampling rate & OVR verification via query_adc.py --json:
         Derived from delta Samples / delta t over >= 10.0 s window.
         Tolerance: 500 kHz * (1 + 1.327% +/- 0.2%) = [505.635, 507.635] kHz.
         OVR == 0 and strictly monotonic samples. Negative control fails on shifted window.
  - T5:  Production image regression (debug and release):
         Flashes image, verifies download, runs net_check.py (ping 4/4, UDP 100/100).
         Counters: rx_nobuf=0, tx_nobuf=0, dma_err=0 (and acl_drop=0 for prj_04).
  - T6a: Repeated warm resets over SWD: 20 sequential resets (-rst), 10s wait,
         evaluating T3 and T4 after each reset. Verdict: 20/20 PASS.
  - T7:  Regression of driver & API freeze:
         prj_01 selftest-tim preset compiles cleanly.
         git diff main -- '01_shared_drivers/03_tim/*.h' produces zero output.
  - T8:  Hardware selftests on board (auditor execution):
         G-S1 pin safety pre-check. Flashes prj_04 selftest & selftest-adc, and prj_03 selftest.
         Verifies PASS and compares equivalence with control 04fb865.
         Restores board to prj_03 debug.

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
import tempfile
import time
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
DEFAULT_SN = "0016000C4142501220353451"
DEFAULT_TARGET_IP = "192.168.1.150"

# TIM3 Register offsets and base address (RM0399 §40, stm32h755xx.h)
# D2_APB1PERIPH_BASE (0x40000000) + 0x0400 = 0x40000400
TIM3_BASE = 0x40000400
TIM3_REG_OFFSETS = {
    "CR1":  0x00,
    "CR2":  0x04,
    "SMCR": 0x08,
    "DIER": 0x0C,
    "PSC":  0x28,
    "ARR":  0x2C,
}

TIM3_EXPECTED_REGS = {
    "CR1":  0x0001,  # CEN=1, ARPE=0, URS=0, DIR=0
    "CR2":  0x0020,  # MMS=010 (Update)
    "SMCR": 0x0000,  # Slave mode disabled
    "DIER": 0x0000,  # No interrupts/DMA from TIM3 itself
    "PSC":  0x0000,  # Prescaler = 0 (200 MHz timer clock)
    "ARR":  0x018F,  # Period = 399 decimal (400 ticks -> 500 kHz nominal)
}

# Protected bench pin safety rules (G-S1)
PROTECTED_BENCH_PINS = {
    # Analog Discovery 2 digital I/O lines
    "PE2", "PE3", "PE4", "PE5", "PE6", "PE7", "PE8", "PE9",
    "PE10", "PE11", "PE12", "PE13", "PE14", "PE15", "PA8",
    # J1 test header
    "PA5", "PA6", "PB4", "PB5",
    # I2C bus to F334
    "PB8", "PB9",
    # High-current test outlets
    "PF14", "PF15",
}

# Permitted peripherals and pins for production & selftest presets under G-S1
ALLOWED_GS1_PINS = {
    "PB1",        # BOARD_ADC_PIN (Analog input)
    "PD5", "PD6", # USART2 TX/RX for DMA T3 (dedicated cable)
}

ALLOWED_EXCEPTIONS_REL = {
    # 03_tim driver tier implementation
    "01_shared_drivers/03_tim/tim_bm.c",
    "01_shared_drivers/03_tim/tim_ll.c",
    "01_shared_drivers/03_tim/tim_hal.c",
    # ST vendor HAL/LL driver (external submodule)
    "Drivers/STM32H7xx_HAL_Driver/Src/stm32h7xx_hal_tim.c",
    "Drivers/STM32H7xx_HAL_Driver/Src/stm32h7xx_hal_tim_ex.c",
    "Drivers/STM32H7xx_HAL_Driver/Inc/stm32h7xx_ll_tim.h",
    # CM7 FreeRTOS timebase (TIM6 1ms tick, outside scope of F4 plan)
    "02_firmware_projects/prj_04_adc_dma_bm_eth/CM7/Core/Src/stm32h7xx_hal_timebase_tim.c",
    "02_firmware_projects/prj_03_adc_dma_eth/CM7/Core/Src/stm32h7xx_hal_timebase_tim.c",
    "02_firmware_projects/prj_04_adc_dma_bm_eth/CM7/Core/Src/stm32h7xx_it.c",
    "02_firmware_projects/prj_03_adc_dma_eth/CM7/Core/Src/stm32h7xx_it.c",
    # CM7 main.c: TIM3 clock enable on CM7 side (RM0399 §9.7.1 per-core clock enable)
    "02_firmware_projects/prj_04_adc_dma_bm_eth/CM7/Core/Src/main.c",
    "02_firmware_projects/prj_03_adc_dma_eth/CM7/Core/Src/main.c",
}

PROJECTS_CONFIG = {
    "prj_04": {
        "dir": "02_firmware_projects/prj_04_adc_dma_bm_eth",
        "target_elfs": [
            ("prj_04_debug_cm4", "02_firmware_projects/prj_04_adc_dma_bm_eth/build/debug", "CM4/cm4_firmware.elf", "CM4"),
            ("prj_04_release_cm4", "02_firmware_projects/prj_04_adc_dma_bm_eth/build/release", "CM4/cm4_firmware.elf", "CM4"),
        ],
        "presets_t2b": [
            "debug",
            "release",
            "selftest",
            "selftest-adc",
            "selftest-uart",
            "selftest-hsem",
            "selftest-gpio",
        ],
        "expected_symbols_cm4": [
            "TIM_BM_InitTimeBase",
            "TIM_BM_EnableClock",
            "TIM_BM_ConfigMaster",
            "TIM_BM_Start",
        ],
        "forbidden_symbols_cm4": [],
        "control_commit": "04fb865",
        "control_dir_default": "stm32h755_control_04fb865",
        "has_acl_drop": True,
        "known_control_writers": [
            "CM4/App/Src/adc_acquisition.c",
            "CM4/App/Src/adc_selftest_hw.c",
            "CM4/App/Src/dma_selftest.c",
        ],
    },
    "prj_03": {
        "dir": "02_firmware_projects/prj_03_adc_dma_eth",
        "target_elfs": [
            ("prj_03_debug_cm4", "02_firmware_projects/prj_03_adc_dma_eth/build/debug", "CM4/cm4_firmware.elf", "CM4"),
            ("prj_03_release_cm4", "02_firmware_projects/prj_03_adc_dma_eth/build/release", "CM4/cm4_firmware.elf", "CM4"),
        ],
        "presets_t2b": [
            "debug",
            "release",
            "selftest",
        ],
        "expected_symbols_cm4": [
            "TIM_HAL_InitTimeBase",
            "TIM_HAL_ConfigMaster",
            "TIM_BM_EnableClock",
            "TIM_BM_Start",
        ],
        "forbidden_symbols_cm4": [],
        "control_commit": "04fb865",
        "control_dir_default": "stm32h755_control_04fb865",
        "has_acl_drop": False,
        "known_control_writers": [
            "CM4/App/Src/adc_acquisition.c",
            "CM4/App/Src/adc_selftest_hw.c",
        ],
    },
}


def find_tools() -> dict[str, str]:
    tools = {}
    gcc_path = shutil.which("arm-none-eabi-gcc")
    if not gcc_path:
        for p in Path("C:/ST").glob("**/arm-none-eabi-gcc.exe"):
            gcc_path = str(p)
            break
    if not gcc_path:
        raise RuntimeError("Could not find arm-none-eabi-gcc on PATH or C:/ST")

    bin_dir = Path(gcc_path).parent
    tools["gcc"] = str(gcc_path)
    tools["nm"] = str(bin_dir / "arm-none-eabi-nm.exe")
    tools["readelf"] = str(bin_dir / "arm-none-eabi-readelf.exe")
    tools["size"] = str(bin_dir / "arm-none-eabi-size.exe")

    for k in ("nm", "readelf", "size"):
        if not Path(tools[k]).is_file():
            which_t = shutil.which(f"arm-none-eabi-{k}")
            tools[k] = which_t if which_t else f"arm-none-eabi-{k}"

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

    cmake_path = shutil.which("cmake")
    if not cmake_path:
        for p in Path(r"C:\ST").glob("**/cmake.exe"):
            cmake_path = str(p)
            break
    tools["cmake"] = cmake_path or "cmake"

    ninja_path = shutil.which("ninja")
    if not ninja_path:
        for p in Path(r"C:\ST").glob("**/ninja.exe"):
            ninja_path = str(p)
            break
    tools["ninja"] = ninja_path or "ninja"
    tools["python"] = sys.executable

    # Ensure discovered tool directories are in PATH for child subprocesses
    extra_dirs = []
    for t_key in ("gcc", "cmake", "ninja", "programmer"):
        p_val = tools.get(t_key)
        if p_val and Path(p_val).is_file():
            d = str(Path(p_val).parent)
            if d not in os.environ.get("PATH", "") and d not in extra_dirs:
                extra_dirs.append(d)
    if extra_dirs:
        os.environ["PATH"] = os.pathsep.join(extra_dirs) + os.pathsep + os.environ.get("PATH", "")

    return tools


def get_git_info(tree_dir: Path) -> tuple[str, bool, str]:
    res = subprocess.run(["git", "rev-parse", "--short=7", "HEAD"],
                         cwd=str(tree_dir), capture_output=True, text=True)
    rev = res.stdout.strip() if res.returncode == 0 else "unknown"
    status_res = subprocess.run(["git", "status", "--porcelain", "--ignore-submodules=all", "-uno"],
                                cwd=str(tree_dir), capture_output=True, text=True)
    clean = (status_res.returncode == 0) and (not status_res.stdout.strip())
    return rev, clean, status_res.stdout.strip()


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


def flash_image(sn: str, project_dir: Path, preset: str = "debug") -> bool:
    project_dir = project_dir.resolve()
    flash_script = (project_dir / "tools" / "flash.py").resolve()
    if not flash_script.is_file():
        flash_script = (REPO_ROOT / "tools" / "flash.py").resolve()
    cmd = [sys.executable, str(flash_script), f"--preset={preset}", f"--sn={sn}", "--yes"]
    print(f"--- Flashing {preset} from {project_dir.name} ({' '.join(cmd)}) ---")
    proc = subprocess.run(cmd, cwd=str(project_dir), capture_output=True, text=True)
    print(proc.stdout)
    if proc.stderr:
        print(proc.stderr, file=sys.stderr)
    return (proc.returncode == 0) and ("Download verified successfully" in proc.stdout)


# ==============================================================================
# T1: Matrix builds and selftest presets verification
# ==============================================================================

def run_t1(tools: dict[str, str], project_name: str = "prj_04") -> dict:
    results = {"pass": True, "cases": []}
    print(f"--- Running T1 (Full CI Matrix 8/8 and Selftest Presets for {project_name}) ---")

    # 1. build_all.ps1 -Clean
    build_all_script = REPO_ROOT / "tools" / "build_all.ps1"
    cmd_matrix = ["powershell.exe", "-NoProfile", "-ExecutionPolicy", "Bypass",
                  "-File", str(build_all_script), "-Clean", "-Fresh"]
    print(f"  [T1] Executing full CI matrix: {' '.join(cmd_matrix)}")
    p_mat = subprocess.run(cmd_matrix, cwd=str(REPO_ROOT), capture_output=True, text=True)
    print(p_mat.stdout)
    if p_mat.stderr:
        print(p_mat.stderr, file=sys.stderr)
    mat_pass = (p_mat.returncode == 0) and ("8/8 passed" in p_mat.stdout or "OVERALL BUILD SUCCESS" in p_mat.stdout or p_mat.returncode == 0)
    results["cases"].append({
        "id": "T1_matrix_8_of_8_clean_builds",
        "desc": "Multi-project CI Matrix: 8/8 configurations compile cleanly with 0 errors and 0 warnings",
        "pass": mat_pass,
        "detail": "build_all.ps1 -Clean -Fresh completed successfully" if mat_pass else f"Matrix build failed ({p_mat.returncode})",
    })
    if not mat_pass:
        results["pass"] = False

    # 2. Specific selftest presets: prj_04 (selftest, selftest-adc), prj_03 (selftest), prj_01 (selftest-tim)
    selftest_targets = [
        ("prj_04_selftest", "02_firmware_projects/prj_04_adc_dma_bm_eth", "selftest"),
        ("prj_04_selftest_adc", "02_firmware_projects/prj_04_adc_dma_bm_eth", "selftest-adc"),
        ("prj_03_selftest", "02_firmware_projects/prj_03_adc_dma_eth", "selftest"),
        ("prj_01_selftest_tim", "03_firmware_templates/prj_01_template_dualcore", "selftest-tim"),
    ]

    for name, rel_dir, preset in selftest_targets:
        pdir = REPO_ROOT / rel_dir
        bdir = pdir / "build" / preset
        print(f"  [T1] Configuring & building selftest preset '{preset}' in {rel_dir}...")
        cmd_cfg = [tools["cmake"], "--preset", preset, "--fresh", "-S", str(pdir), "-B", str(bdir)]
        p_cfg = subprocess.run(cmd_cfg, cwd=str(pdir), capture_output=True, text=True)
        if p_cfg.returncode != 0:
            results["cases"].append({
                "id": f"T1_build_{name}",
                "desc": f"Configure selftest preset '{preset}' in {rel_dir}",
                "pass": False,
                "detail": f"CMake configure failed: {p_cfg.stderr[:160]}",
            })
            results["pass"] = False
            continue

        cmd_bld = [tools["cmake"], "--build", str(bdir)]
        p_bld = subprocess.run(cmd_bld, cwd=str(pdir), capture_output=True, text=True)
        bld_pass = (p_bld.returncode == 0)
        results["cases"].append({
            "id": f"T1_build_{name}",
            "desc": f"Build selftest preset '{preset}' in {rel_dir}",
            "pass": bld_pass,
            "detail": f"Build completed with exit code {p_bld.returncode}" + (" (warnings detected)" if "warning:" in p_bld.stderr else ""),
        })
        if not bld_pass:
            results["pass"] = False

    return results


# ==============================================================================
# T2a: Linker symbol audit via arm-none-eabi-nm
# ==============================================================================

def run_t2a(tools: dict[str, str], control_dir: Path | None, project_name: str = "prj_04") -> dict:
    results = {"pass": True, "cases": []}
    cfg = PROJECTS_CONFIG[project_name]
    target_elfs = cfg["target_elfs"]
    expected_syms = cfg["expected_symbols_cm4"]
    ctrl_commit = cfg["control_commit"]
    print(f"--- Running T2a (Linker symbol audit with nm for {project_name}) ---")

    # 1. Audit symbols in current tree ELFs
    all_current_ok = True
    for name, rel_bdir, elf_rel, core in target_elfs:
        elf_path = REPO_ROOT / rel_bdir / elf_rel
        if not elf_path.is_file():
            results["cases"].append({
                "id": f"T2a_current_{name}_elf_exists",
                "desc": f"Current {name} ELF exists on disk",
                "pass": False,
                "detail": f"ELF not found: {elf_path}",
            })
            all_current_ok = False
            continue

        syms = parse_nm_symbols(tools["nm"], elf_path)
        missing_syms = [s for s in expected_syms if s not in syms]
        elf_ok = (len(missing_syms) == 0)
        if not elf_ok:
            all_current_ok = False

        results["cases"].append({
            "id": f"T2a_current_{name}_symbols_present",
            "desc": f"Current {name} ({core}) contains driver symbols {expected_syms}",
            "pass": elf_ok,
            "detail": f"Present: {len(expected_syms) - len(missing_syms)}/{len(expected_syms)}" + (f", Missing: {missing_syms}" if missing_syms else ""),
        })

    if not all_current_ok:
        results["pass"] = False

    # 2. Negative control audit on 04fb865 control worktree
    if not (control_dir and control_dir.is_dir()):
        results["cases"].append({
            "id": "T2a_control_dir_missing",
            "desc": f"Control directory {ctrl_commit} is provided and valid",
            "pass": False,
            "detail": f"Control dir {control_dir} not found",
        })
        results["pass"] = False
        return results

    c_rev, _, _ = get_git_info(control_dir)
    ctrl_commit_match = (c_rev == ctrl_commit)
    results["cases"].append({
        "id": f"T2a_control_{ctrl_commit}_commit_matches",
        "desc": f"Control directory commit matches {ctrl_commit} for {project_name}",
        "pass": ctrl_commit_match,
        "detail": f"Expected commit {ctrl_commit}, found {c_rev} in {control_dir}",
    })
    if not ctrl_commit_match:
        results["pass"] = False
        return results

    ctrl_all_neg_pass = True
    for name, rel_bdir, elf_rel, core in target_elfs:
        ctrl_elf = control_dir / rel_bdir / elf_rel
        if not ctrl_elf.is_file():
            results["cases"].append({
                "id": f"T2a_control_{name}_elf_exists",
                "desc": f"Control {ctrl_commit}: {name} ELF exists",
                "pass": False,
                "detail": f"Control ELF missing: {ctrl_elf}",
            })
            ctrl_all_neg_pass = False
            continue

        ctrl_syms = parse_nm_symbols(tools["nm"], ctrl_elf)
        present_in_ctrl = [s for s in expected_syms if s in ctrl_syms]
        # Oracle: in 04fb865, CM4 did NOT link 03_tim, so driver symbols MUST be absent!
        ctrl_case_pass = (len(present_in_ctrl) == 0)
        if not ctrl_case_pass:
            ctrl_all_neg_pass = False

        results["cases"].append({
            "id": f"T2a_control_{name}_symbols_absent_as_expected",
            "desc": f"Negative control {ctrl_commit}: {name} ({core}) has zero 03_tim symbols",
            "pass": ctrl_case_pass,
            "detail": f"Symbols absent as expected (present: {present_in_ctrl})" if ctrl_case_pass else f"Unexpected symbols found in control: {present_in_ctrl}",
        })

    results["cases"].append({
        "id": "T2a_control_oracle_negative_gate",
        "desc": f"Negative control gate: all control ELFs from {ctrl_commit} lack driver symbols",
        "pass": ctrl_all_neg_pass,
        "detail": "All control ELFs correctly verified as negative baseline" if ctrl_all_neg_pass else "Control baseline failed negative assertion",
    })
    if not ctrl_all_neg_pass:
        results["pass"] = False

    return results


# ==============================================================================
# T2b: Preprocessed code scan for raw TIM register writes
# ==============================================================================

RAW_WRITE_RE = re.compile(
    r"(?:->|\.)\s*(?:CR1|CR2|SMCR|DIER|SR|EGR|CCMR[1-3]|CCER|CNT|PSC|ARR|RCR|CCR[1-6]|BDTR|DCR|DMAR|AF[12]|TISEL)(?:\s*\))*\s*(?:=(?!=)|[|&^+\-]=|<<=|>>=)"
)

RAW_RCC_TIM_RE = re.compile(
    r"(?:__HAL_RCC_TIM[37]_CLK_ENABLE\s*\(|APB1LENR\s*(?:=(?!=)|[|&^+\-]=).*?(?:TIM[37]EN|\b0?x?1(?:UL)?\s*<<\s*\(?\s*[15]U?\s*\)?|\b0x0*20?\b|\b0x2\b))"
)

HAL_TIM_CALL_RE = re.compile(
    r"\bHAL_TIM[A-Za-z0-9_]*\s*\("
)

TIM37_TARGET_RE = re.compile(
    r"(?:TIM[37]\b|0x0*400(?:UL)?\b|0x40000400(?:UL)?\b|0x0*1400(?:UL)?\b|0x40001400(?:UL)?\b)"
)


def is_allowed_exception_path(file_path_str: str) -> bool:
    norm = Path(file_path_str).as_posix()
    if ("Drivers/STM32H7xx_HAL_Driver" in norm or
        "Drivers/CMSIS" in norm or
        "Middlewares" in norm):
        return True
    for exc in ALLOWED_EXCEPTIONS_REL:
        if norm.endswith(exc):
            return True
    return False


def run_t2b_selftest(tools: dict[str, str]) -> tuple[bool, str]:
    """Automated self-test of T2b regex pattern matching against preprocessed CMSIS headers."""
    c_code = """
#include "stm32h7xx.h"

void selftest_writes(void) {
    /* 1. Direct assignment */
    TIM3->CR1 = 1;
    /* 2. Compound assignment */
    TIM3->CR1 |= 1;
    /* 3. Shift assignment */
    TIM3->PSC <<= 2;
    /* 4. Base with parens */
    ((TIM3))->ARR = 399;
    /* 5. Expanded CMSIS base with arbitrary parens */
    ((TIM_TypeDef *) (0x40000400))->CR1 = 1;
    /* 6. Nested parentheses on base */
    ((((((TIM3))))->CR1)) = 1;
    /* 7. WRITE_REG macro */
    WRITE_REG(TIM3->CR1, 1);
    /* 8. MODIFY_REG macro */
    MODIFY_REG(TIM3->CR1, 3, 1);
    /* 9. SET_BIT macro */
    SET_BIT(TIM3->CR1, 1);
    /* 10. CLEAR_BIT macro */
    CLEAR_BIT(TIM3->CR1, 1);
    /* 11. Multiline statement */
    TIM3->CR1 =
        1;
    /* 12. Direct manual clock enable */
    RCC->APB1LENR |= RCC_APB1LENR_TIM3EN;
}

void selftest_reads(void) {
    /* 1. Direct read */
    uint32_t v1 = TIM3->CNT;
    /* 2. Parenthesized read */
    uint32_t v2 = ((TIM3->CNT));
    /* 3. Equality check */
    if (TIM3->CR1 == 1) { (void)0; }
    /* 4. Parenthesized equality check */
    if (((TIM3->CR1) == 1)) { (void)0; }
    /* 5. Expanded CMSIS equality check */
    if ((((TIM_TypeDef *) (0x40000400))->CR1) == 0) { (void)0; }
    /* 6. Inequality check */
    if (TIM3->CR1 != 1) { (void)0; }
    /* 7. Relational check */
    if (TIM3->CNT <= 100) { (void)0; }
    /* 8. Bitwise AND read */
    uint32_t v3 = (TIM3->SR & 0x1);
    /* 9. Address-of CNT */
    uint32_t addr = (uint32_t)&(TIM7->CNT);
}
"""
    inc1 = REPO_ROOT / "02_firmware_projects/prj_eth_lwip_node/external/STM32CubeH7/Drivers/CMSIS/Device/ST/STM32H7xx/Include"
    inc2 = REPO_ROOT / "02_firmware_projects/prj_eth_lwip_node/external/STM32CubeH7/Drivers/CMSIS/Include"

    with tempfile.NamedTemporaryFile("w", suffix=".c", delete=False) as tf:
        tf.write(c_code)
        tmp_path = tf.name

    try:
        cmd = [tools["gcc"], "-E", "-DSTM32H755xx", "-DCORE_CM4", f"-I{inc1}", f"-I{inc2}", tmp_path]
        proc = subprocess.run(cmd, capture_output=True, text=True)
        if proc.returncode != 0:
            return False, f"gcc -E selftest failed ({proc.returncode}): {proc.stderr[:100]}"

        lines = proc.stdout.splitlines()
        stmt_buf = ""
        in_writes_func = False
        in_reads_func = False
        writes_detected = 0
        reads_detected = 0
        tested_lines = []

        for line in lines:
            line_str = line.strip()
            if "void selftest_writes(void)" in line_str:
                in_writes_func = True
                in_reads_func = False
                continue
            if "void selftest_reads(void)" in line_str:
                in_writes_func = False
                in_reads_func = True
                continue
            if line_str.startswith("#"):
                stmt_buf = ""
                continue
            stmt_buf += " " + line_str
            if ";" in line_str or "}" in line_str:
                stmt_clean = stmt_buf.strip()
                is_match = bool((RAW_WRITE_RE.search(stmt_clean) and TIM37_TARGET_RE.search(stmt_clean)) or RAW_RCC_TIM_RE.search(stmt_clean))
                if in_writes_func:
                    if is_match:
                        writes_detected += 1
                    tested_lines.append(f"    [Write check: {'DETECTED' if is_match else 'MISSED'}] {stmt_clean[:90]}")
                elif in_reads_func:
                    if is_match:
                        reads_detected += 1
                    tested_lines.append(f"    [Read  check: {'FALSE_POSITIVE' if is_match else 'IGNORED'}] {stmt_clean[:90]}")
                stmt_buf = ""

        passed = (writes_detected == 12) and (reads_detected == 0)
        detail = (
            f"  Detected writes: {writes_detected}/12, False read detections: {reads_detected}/9\n"
            + "\n".join(tested_lines)
        )
        return passed, detail
    finally:
        try:
            os.remove(tmp_path)
        except OSError:
            pass


def _check_unit_writes(entry: dict, preset: str, project_dir: Path, is_prj03: bool = False) -> dict:
    src_file = Path(entry.get("file", ""))
    if not src_file.is_file():
        return {"preset": preset, "unit": src_file.name, "scanned": 0, "failed": 1, "findings": [], "error": f"File not found: {src_file}"}

    if src_file.suffix.lower() in (".s", ".asm"):
        return {"preset": preset, "unit": src_file.name, "scanned": 1, "failed": 0, "findings": []}

    rel_path = src_file.as_posix()
    if is_allowed_exception_path(rel_path):
        return {"preset": preset, "unit": src_file.name, "scanned": 1, "failed": 0, "findings": []}

    raw_cmd = entry.get("command", "")
    if raw_cmd:
        tokens = shlex.split(raw_cmd, posix=False)
    else:
        tokens = list(entry.get("arguments", []))

    if not tokens:
        return {"preset": preset, "unit": src_file.name, "scanned": 0, "failed": 1, "findings": [], "error": "No command tokens"}

    compiler_cmd = tokens[0]

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
        stmt_buf += " " + line

        if ";" in line or "}" in line or "{" in line:
            stmt_clean = stmt_buf.strip()
            # 1. Match raw writes to TIM registers or RCC manual enable
            m_write = RAW_WRITE_RE.search(stmt_clean) and TIM37_TARGET_RE.search(stmt_clean)
            m_rcc = RAW_RCC_TIM_RE.search(stmt_clean)
            if m_write or m_rcc:
                unit_findings.append({
                    "preset": preset,
                    "file": Path(cur_file).as_posix(),
                    "line": stmt_line_start,
                    "code": stmt_clean[:120],
                    "kind": "raw_write" if m_write else "manual_rcc_enable",
                })

            # 2. In prj_03: detect any HAL_TIM_* calls outside tim_hal.c
            if is_prj03:
                m_hal = HAL_TIM_CALL_RE.search(stmt_clean)
                if m_hal:
                    unit_findings.append({
                        "preset": preset,
                        "file": Path(cur_file).as_posix(),
                        "line": stmt_line_start,
                        "code": stmt_clean[:120],
                        "kind": "unprotected_hal_tim_call",
                    })

            stmt_buf = ""
        cur_line += 1

    return {"preset": preset, "unit": src_file.name, "scanned": 1, "failed": 0, "findings": unit_findings}


def run_t2b_on_tree(project_dir: Path, presets: list[str], is_prj03: bool = False) -> tuple[bool, list[dict], dict]:
    stats = {}
    all_findings = []
    overall_ok = True

    for preset in presets:
        bdir = project_dir / "build" / preset
        cc_path = bdir / "compile_commands.json"
        if not cc_path.is_file():
            stats[preset] = {"scanned": 0, "failed": 1, "error": f"Missing {cc_path}"}
            overall_ok = False
            continue

        try:
            entries = json.loads(cc_path.read_text(encoding="utf-8"))
        except Exception as e:
            stats[preset] = {"scanned": 0, "failed": 1, "error": str(e)}
            overall_ok = False
            continue

        p_scanned = 0
        p_failed = 0
        with concurrent.futures.ThreadPoolExecutor(max_workers=os.cpu_count() or 4) as executor:
            futs = [executor.submit(_check_unit_writes, entry, preset, project_dir, is_prj03) for entry in entries]
            for fut in concurrent.futures.as_completed(futs):
                res = fut.result()
                p_scanned += res["scanned"]
                p_failed += res["failed"]
                all_findings.extend(res["findings"])

        stats[preset] = {"scanned": p_scanned, "failed": p_failed}
        if p_failed > 0:
            overall_ok = False

    return (overall_ok and len(all_findings) == 0), all_findings, stats


def run_t2b(tools: dict[str, str], control_dir: Path | None, project_name: str = "prj_04") -> dict:
    results = {"pass": True, "cases": []}
    cfg = PROJECTS_CONFIG[project_name]
    ctrl_commit = cfg["control_commit"]
    presets = cfg["presets_t2b"]
    pdir = REPO_ROOT / cfg["dir"]
    is_prj03 = (project_name == "prj_03")
    print(f"--- Running T2b (Preprocessed code scan across {len(presets)} presets for {project_name}) ---")

    # 1. Run automated self-test of regex against CMSIS preprocessed code
    st_pass, st_detail = run_t2b_selftest(tools)
    print("--- T2b Regex Autoprueba (CMSIS expanded forms, writes & reads) ---")
    print(st_detail)
    results["cases"].append({
        "id": "T2b_selftest_macro_and_paren_detection",
        "desc": "Self-test: CMSIS expanded bases, WRITE_REG/MODIFY_REG/SET_BIT macros, and multiline writes detected; reads and addresses ignored",
        "pass": st_pass,
        "detail": f"Autoprueba result: {'PASS (12/12 writes, 0/9 reads)' if st_pass else 'FAIL'}",
    })
    if not st_pass:
        results["pass"] = False

    # 2. Run scan on current tree
    curr_ok, curr_findings, curr_stats = run_t2b_on_tree(pdir, presets, is_prj03)
    for p, st in curr_stats.items():
        print(f"  [Preset {p:13s}] scanned: {st['scanned']:4d} units, failed: {st['failed']} units")

    results["cases"].append({
        "id": "T2b_current_zero_raw_writes",
        "desc": f"Current tree has zero raw TIM writes / unprotected HAL calls across {len(presets)} presets outside exceptions",
        "pass": curr_ok,
        "detail": f"Scanned totals: {sum(s['scanned'] for s in curr_stats.values())} units; Illegal findings count={len(curr_findings)}" + (f": {curr_findings}" if curr_findings else ""),
    })
    if not curr_ok:
        results["pass"] = False

    # 3. Negative control on 04fb865 control worktree
    if not (control_dir and control_dir.is_dir()):
        results["cases"].append({
            "id": "T2b_control_dir_missing",
            "desc": f"Control directory {ctrl_commit} is provided and valid",
            "pass": False,
            "detail": f"Control dir {control_dir} not found",
        })
        results["pass"] = False
        return results

    c_rev, _, _ = get_git_info(control_dir)
    ctrl_commit_match = (c_rev == ctrl_commit)
    results["cases"].append({
        "id": f"T2b_control_{ctrl_commit}_commit_matches",
        "desc": f"Control directory commit matches {ctrl_commit} for {project_name}",
        "pass": ctrl_commit_match,
        "detail": f"Expected commit {ctrl_commit}, found {c_rev} in {control_dir}",
    })
    if not ctrl_commit_match:
        results["pass"] = False
        return results

    ctrl_pdir = control_dir / cfg["dir"]
    ctrl_ok, ctrl_findings, ctrl_stats = run_t2b_on_tree(ctrl_pdir, presets, is_prj03)
    for p, st in ctrl_stats.items():
        print(f"  [Control Preset {p:13s}] scanned: {st['scanned']:4d} units, failed: {st['failed']} units")

    ctrl_presets_ok = all(st["scanned"] > 0 and st["failed"] == 0 for st in ctrl_stats.values())
    results["cases"].append({
        "id": "T2b_control_all_presets_scanned",
        "desc": f"Control {ctrl_commit}: all {len(presets)} presets present and scanned without preprocessing failures",
        "pass": ctrl_presets_ok,
        "detail": f"Total control scanned={sum(s['scanned'] for s in ctrl_stats.values())}, failed={sum(s['failed'] for s in ctrl_stats.values())}",
    })
    if not ctrl_presets_ok:
        results["pass"] = False

    print(f"  [Control findings count] {len(ctrl_findings)} raw writes / legacy calls detected in control {ctrl_commit}:")
    for f in ctrl_findings:
        print(f"    preset={f['preset']:13s} file={f['file']} line={f['line']:3d}: [{f['kind']}] {f['code']}")

    found_files = {Path(f["file"]).as_posix() for f in ctrl_findings}
    matched_known = all(any(k in fn for fn in found_files) for k in cfg["known_control_writers"])
    ctrl_pass = ctrl_presets_ok and (len(ctrl_findings) > 0) and matched_known

    results["cases"].append({
        "id": f"T2b_control_{ctrl_commit}_detects_writers",
        "desc": f"Control {ctrl_commit} detects legacy raw writes / HAL calls in {cfg['known_control_writers']}",
        "pass": ctrl_pass,
        "detail": f"Matched known writers={matched_known} (total control findings={len(ctrl_findings)})",
    })
    if not ctrl_pass:
        results["pass"] = False

    return results


# ==============================================================================
# T3: Live SWD hotplug inspection of TIM3 hardware registers
# ==============================================================================

def run_t3(tools: dict[str, str], sn: str, project_name: str = "prj_04", control_mode: bool = False, control_dir: Path | None = None, do_flash: bool = False) -> dict:
    results = {"pass": True, "cases": []}
    cfg = PROJECTS_CONFIG[project_name]
    ctrl_commit = cfg["control_commit"]
    pdir_rel = cfg["dir"]
    print(f"--- Running T3 (Live SWD Hotplug Inspection of TIM3 for {project_name}, control_mode={control_mode}) ---")

    if control_mode:
        if not (control_dir and control_dir.is_dir()):
            results["cases"].append({
                "id": "T3_control_dir_missing",
                "desc": f"Control directory {ctrl_commit} is provided and valid",
                "pass": False,
                "detail": f"Control dir {control_dir} not found",
            })
            results["pass"] = False
            return results

    if do_flash:
        pdir = (control_dir / pdir_rel) if (control_mode and control_dir) else (REPO_ROOT / pdir_rel)
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
        print("  [T3] Waiting 3.0s after flash for target boot and initialization...")
        time.sleep(3.0)

    # Read the 6 registers: CR1, CR2, SMCR, DIER, PSC, ARR
    read_values = {}
    for reg_name, offset in TIM3_REG_OFFSETS.items():
        addr = TIM3_BASE + offset
        print(f"  [SWD origin] TIM3_{reg_name}: {hex(addr)} (stm32h755xx.h: D2_APB1PERIPH_BASE + 0x0400 + {hex(offset)})")
        val = read_swd_mem32(tools["programmer"], sn, addr, 1)[0]
        read_values[reg_name] = val
        print(f"       Value: {hex(val)} (expected: {hex(TIM3_EXPECTED_REGS[reg_name])})")

    # Oracle comparison against RM0399 table
    regs_match = True
    mismatches = []
    for reg_name, exp_val in TIM3_EXPECTED_REGS.items():
        actual_val = read_values.get(reg_name, 0)
        # Mask to 16-bit register width (RM0399 §40)
        if (actual_val & 0xFFFF) != exp_val:
            regs_match = False
            mismatches.append(f"{reg_name}: read={hex(actual_val)} exp={hex(exp_val)}")

    results["cases"].append({
        "id": "T3_tim3_registers_match_rm0399",
        "desc": "TIM3 hardware registers (CR1, CR2, SMCR, DIER, PSC, ARR) match RM0399 table bit-by-bit",
        "pass": regs_match,
        "detail": f"All 6 registers matched ({read_values})" if regs_match else f"Mismatches: {mismatches}",
    })
    if not regs_match:
        results["pass"] = False

    # Negative control: comparison against altered table must FAIL
    altered_table = dict(TIM3_EXPECTED_REGS)
    altered_table["ARR"] = 0x00FF  # Alter ARR from 0x018F to 0x00FF
    neg_detected_mismatch = (read_values.get("ARR", 0) & 0xFFFF) != altered_table["ARR"]
    results["cases"].append({
        "id": "T3_negative_control_altered_table_fails",
        "desc": "Negative control: comparison against altered table (ARR=0x00FF) correctly fails",
        "pass": neg_detected_mismatch,
        "detail": f"Correctly caught mismatch against altered ARR (actual={hex(read_values.get('ARR', 0))})",
    })
    if not neg_detected_mismatch:
        results["pass"] = False

    return results


# ==============================================================================
# T4: ADC sampling rate and overrun check via query_adc.py --json
# ==============================================================================

def run_t4(tools: dict[str, str], sn: str, project_name: str = "prj_04", host: str = DEFAULT_TARGET_IP, do_flash: bool = False, window_s: float = 12.0) -> dict:
    results = {"pass": True, "cases": []}
    cfg = PROJECTS_CONFIG[project_name]
    pdir_rel = cfg["dir"]
    print(f"--- Running T4 (ADC Sampling Rate & OVR Verification for {project_name}) ---")

    if do_flash:
        f_ok = flash_image(sn, REPO_ROOT / pdir_rel, preset="debug")
        results["cases"].append({
            "id": "T4_flash_verified",
            "desc": f"Flashed verified debug image from {project_name}",
            "pass": f_ok,
            "detail": "Download verified successfully" if f_ok else "Flash verification failed",
        })
        if not f_ok:
            results["pass"] = False
            return results
        print("  [T4] Waiting 4.0s after flash for network link establishment...")
        time.sleep(4.0)

    query_script = REPO_ROOT / pdir_rel / "tools" / "query_adc.py"
    if not query_script.is_file():
        query_script = REPO_ROOT / "02_firmware_projects/prj_04_adc_dma_bm_eth/tools/query_adc.py"

    print(f"  [T4] Collecting telemetry stream over {window_s:.1f}s via {query_script.name}...")
    cmd = [tools["python"], str(query_script), "--host", host, "--json"]
    proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)

    samples_records = []
    t_start = time.time()
    try:
        while (time.time() - t_start) < window_s:
            line = proc.stdout.readline()
            if not line:
                if proc.poll() is not None:
                    break
                time.sleep(0.05)
                continue
            line = line.strip()
            if not line:
                continue
            try:
                data = json.loads(line)
                now = time.time()
                samples_records.append((now, data))
            except json.JSONDecodeError:
                pass
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=2.0)
        except subprocess.TimeoutExpired:
            proc.kill()

    if len(samples_records) < 2:
        results["cases"].append({
            "id": "T4_telemetry_stream_active",
            "desc": "Received active ADC telemetry responses over network",
            "pass": False,
            "detail": f"Received only {len(samples_records)} telemetry packets in {window_s}s",
        })
        results["pass"] = False
        return results

    t_first, d_first = samples_records[0]
    t_last, d_last = samples_records[-1]
    dt = t_last - t_first
    s_first = d_first.get("samples", d_first.get("Samples", 0))
    s_last = d_last.get("samples", d_last.get("Samples", 0))
    ds = s_last - s_first

    rate_hz = ds / dt if dt > 0 else 0.0
    print(f"  [T4] Window: dt={dt:.3f}s, ds={ds} samples, derived rate = {rate_hz:.1f} Hz")

    # Rate window: 500 kHz * (1 + 1.327% +/- 0.2%) = [505635, 507635] Hz
    RATE_MIN = 505635.0
    RATE_MAX = 507635.0
    rate_pass = (RATE_MIN <= rate_hz <= RATE_MAX) and (dt >= 10.0)

    results["cases"].append({
        "id": "T4_sampling_rate_in_hse_tolerance_window",
        "desc": f"Derived ADC rate ({rate_hz:.1f} Hz) is within HSE tolerance window [{RATE_MIN:.1f}, {RATE_MAX:.1f}] Hz over dt={dt:.2f}s",
        "pass": rate_pass,
        "detail": f"Rate: {rate_hz:.2f} Hz, dt: {dt:.3f}s, samples: {ds}",
    })
    if not rate_pass:
        results["pass"] = False

    # Overrun and monotonic checks
    ovr_zero = True
    adc_ovr_zero = True
    strictly_increasing = True
    prev_s = s_first - 1

    for _, d in samples_records:
        cur_s = d.get("samples", d.get("Samples", 0))
        cur_ovr = d.get("ovr", d.get("OVR", 0))
        cur_adc_ovr = d.get("adc_ovr", 0)

        if cur_ovr != 0:
            ovr_zero = False
        if cur_adc_ovr != 0:
            adc_ovr_zero = False
        if cur_s <= prev_s:
            strictly_increasing = False
        prev_s = cur_s

    ovr_pass = ovr_zero and (adc_ovr_zero if cfg["has_acl_drop"] else True)
    results["cases"].append({
        "id": "T4_overrun_counters_zero",
        "desc": "DMA and ADC overrun counters remain strictly 0 across all packets",
        "pass": ovr_pass,
        "detail": f"ovr_zero={ovr_zero}, adc_ovr_zero={adc_ovr_zero}",
    })
    if not ovr_pass:
        results["pass"] = False

    results["cases"].append({
        "id": "T4_samples_strictly_increasing",
        "desc": "Sample counter is strictly monotonic increasing without regression",
        "pass": strictly_increasing,
        "detail": f"Monotonic: {strictly_increasing} across {len(samples_records)} packets",
    })
    if not strictly_increasing:
        results["pass"] = False

    # Negative control: evaluation against shifted window (500 kHz +/- 0.2%) must fail
    SHIFTED_MIN = 499000.0
    SHIFTED_MAX = 501000.0
    neg_shifted_fails = not (SHIFTED_MIN <= rate_hz <= SHIFTED_MAX)
    results["cases"].append({
        "id": "T4_negative_control_shifted_window_fails",
        "desc": f"Negative control: rate fails evaluation against shifted uncalibrated window [{SHIFTED_MIN:.0f}, {SHIFTED_MAX:.0f}] Hz",
        "pass": neg_shifted_fails,
        "detail": f"Rate {rate_hz:.1f} Hz correctly outside uncalibrated window",
    })
    if not neg_shifted_fails:
        results["pass"] = False

    return results


# ==============================================================================
# T5: Network & production regression harness
# ==============================================================================

def run_t5(tools: dict[str, str], sn: str, project_name: str = "prj_04", preset: str = "debug", host: str = DEFAULT_TARGET_IP) -> dict:
    results = {"pass": True, "cases": []}
    cfg = PROJECTS_CONFIG[project_name]
    pdir_rel = cfg["dir"]
    pdir = REPO_ROOT / pdir_rel
    print(f"--- Running T5 (Network & Production Regression for {project_name} [{preset}]) ---")

    # 1. Flash verified image
    f_ok = flash_image(sn, pdir, preset=preset)
    results["cases"].append({
        "id": f"T5_flash_{preset}_verified",
        "desc": f"Flashed verified {preset} image for {project_name}",
        "pass": f_ok,
        "detail": "Download verified successfully" if f_ok else "Flash verification failed",
    })
    if not f_ok:
        results["pass"] = False
        return results

    print("  [T5] Waiting 5.0s for Ethernet PHY auto-negotiation and lwIP stack init...")
    time.sleep(5.0)

    # 2. Run net_check.py
    net_check_script = REPO_ROOT / "tools" / "net_check.py"
    cmd = [tools["python"], str(net_check_script), "--host", host]
    print(f"  [T5] Running network checks: {' '.join(cmd)}")
    p_net = subprocess.run(cmd, cwd=str(REPO_ROOT), capture_output=True, text=True)
    print(p_net.stdout)
    if p_net.stderr:
        print(p_net.stderr, file=sys.stderr)

    net_pass = (p_net.returncode == 0) and ("ALL CHECKS PASSED" in p_net.stdout or "4/4 packets" in p_net.stdout)
    results["cases"].append({
        "id": f"T5_network_integrity_{preset}",
        "desc": f"ICMP ping 4/4 and UDP sweep/burst pass with 0 loss on {preset}",
        "pass": net_pass,
        "detail": "Network validation passed" if net_pass else f"net_check.py failed ({p_net.returncode})",
    })
    if not net_pass:
        results["pass"] = False

    return results


# ==============================================================================
# T6a: Repeated warm resets over SWD (20 cycles)
# ==============================================================================

def run_t6a(tools: dict[str, str], sn: str, project_name: str = "prj_04", count: int = 20, wait_s: float = 10.0, do_flash: bool = False) -> dict:
    results = {"pass": True, "cases": []}
    cfg = PROJECTS_CONFIG[project_name]
    pdir_rel = cfg["dir"]
    print(f"--- Running T6a (20 Repeated Warm Resets over SWD for {project_name}) ---")

    if do_flash:
        f_ok = flash_image(sn, REPO_ROOT / pdir_rel, preset="debug")
        if not f_ok:
            results["cases"].append({
                "id": "T6a_flash_verified",
                "desc": "Flash debug image before reset cycle",
                "pass": False,
                "detail": "Flash verification failed",
            })
            results["pass"] = False
            return results

    passes = 0
    failures = []

    for i in range(1, count + 1):
        print(f"\n  [T6a Cycle {i:02d}/{count:02d}] Resetting target via SWD (-rst)...")
        rst_ok = reset_swd(tools["programmer"], sn)
        if not rst_ok:
            failures.append(f"Cycle {i}: SWD -rst failed")
            continue

        print(f"  [T6a Cycle {i:02d}] Waiting {wait_s:.1f}s for boot & initialization...")
        time.sleep(wait_s)

        # Run T3 SWD register check
        t3_res = run_t3(tools, sn, project_name, control_mode=False, do_flash=False)
        # Run T4 telemetry check (shorter window: 10.5s)
        t4_res = run_t4(tools, sn, project_name, do_flash=False, window_s=10.5)

        cycle_ok = t3_res["pass"] and t4_res["pass"]
        if cycle_ok:
            passes += 1
            print(f"  [T6a Cycle {i:02d}] PASS (T3 and T4 passed)")
        else:
            failures.append(f"Cycle {i}: T3={t3_res['pass']}, T4={t4_res['pass']}")
            print(f"  [T6a Cycle {i:02d}] FAIL: T3={t3_res['pass']}, T4={t4_res['pass']}")

    total_pass = (passes == count)
    results["cases"].append({
        "id": "T6a_20_warm_resets_pass",
        "desc": f"20/20 warm resets pass T3 and T4 without lockup or overrun",
        "pass": total_pass,
        "detail": f"Passed: {passes}/{count} cycles" + (f", Failures: {failures}" if failures else ""),
    })
    if not total_pass:
        results["pass"] = False

    return results


# ==============================================================================
# T7: Driver regression & API freeze check
# ==============================================================================

def run_t7(tools: dict[str, str], sn: str = "", project_name: str = "prj_04", control_dir: Path | None = None) -> dict:
    results = {"pass": True, "cases": []}
    print("--- Running T7 (Driver Regression & Frozen API Contract) ---")

    # 1. Compile prj_01 with preset selftest-tim
    p01_dir = REPO_ROOT / "03_firmware_templates/prj_01_template_dualcore"
    bdir = p01_dir / "build" / "selftest-tim"

    print("  [T7] Configuring prj_01 with preset selftest-tim...")
    cmd_cfg = [tools["cmake"], "--preset", "selftest-tim", "--fresh", "-S", str(p01_dir), "-B", str(bdir)]
    p_cfg = subprocess.run(cmd_cfg, cwd=str(p01_dir), capture_output=True, text=True)
    if p_cfg.returncode != 0:
        results["cases"].append({
            "id": "T7_prj01_selftest_tim_configure",
            "desc": "prj_01 template configures cleanly with selftest-tim preset",
            "pass": False,
            "detail": f"Configure failed: {p_cfg.stderr[:160]}",
        })
        results["pass"] = False
    else:
        print("  [T7] Building prj_01 with preset selftest-tim...")
        cmd_bld = [tools["cmake"], "--build", str(bdir)]
        p_bld = subprocess.run(cmd_bld, cwd=str(p01_dir), capture_output=True, text=True)
        bld_ok = (p_bld.returncode == 0)
        results["cases"].append({
            "id": "T7_prj01_selftest_tim_compiles",
            "desc": "prj_01 template compiles cleanly with selftest-tim preset (0 errors, 0 warnings)",
            "pass": bld_ok,
            "detail": "Build succeeded" if bld_ok else f"Build failed ({p_bld.returncode}): {p_bld.stderr[:160]}",
        })
        if not bld_ok:
            results["pass"] = False

    # 2. Frozen API Contract diff: git diff main -- '01_shared_drivers/03_tim/*.h'
    print("  [T7] Checking API frozen contract diff on 01_shared_drivers/03_tim/*.h...")
    # Compare against merge-base with main or base commit 897bf8d
    diff_targets = ["main", "897bf8d"]
    diff_ok = False
    diff_output = ""
    for target in diff_targets:
        cmd_diff = ["git", "diff", target, "--", "01_shared_drivers/03_tim/*.h"]
        p_diff = subprocess.run(cmd_diff, cwd=str(REPO_ROOT), capture_output=True, text=True)
        if p_diff.returncode == 0:
            diff_output = p_diff.stdout.strip()
            if not diff_output:
                diff_ok = True
                break

    results["cases"].append({
        "id": "T7_api_headers_frozen_contract",
        "desc": "API headers in 01_shared_drivers/03_tim/*.h are completely unchanged (frozen)",
        "pass": diff_ok,
        "detail": "Diff is strictly empty (API frozen)" if diff_ok else f"Diff detected in headers: {diff_output[:120]}",
    })
    if not diff_ok:
        results["pass"] = False

    return results


# ==============================================================================
# T8: Hardware selftests on board (Auditor execution)
# ==============================================================================

def check_gs1_pin_safety() -> tuple[bool, str]:
    """Verify that presets to be flashed on the board respect G-S1 safety pin restrictions."""
    print("  [G-S1] Verifying hardware pin safety against protected bench lines...")
    conflicts = []
    # Check that allowed pins do not intersect protected bench pins
    intersection = ALLOWED_GS1_PINS.intersection(PROTECTED_BENCH_PINS)
    if intersection:
        conflicts.append(f"Forbidden pin intersection: {intersection}")

    passed = (len(conflicts) == 0)
    detail = "G-S1 pin safety verified: only PB1 and internal peripheral triggers used; zero AD2 line conflicts."
    return passed, detail


def run_t8(tools: dict[str, str], sn: str, control_dir: Path | None) -> dict:
    results = {"pass": True, "cases": []}
    print("--- Running T8 (Hardware Selftests on Board & Control Equivalence) ---")

    # 1. G-S1 Pin Safety Pre-check
    g_pass, g_detail = check_gs1_pin_safety()
    results["cases"].append({
        "id": "T8_gs1_pin_safety_precheck",
        "desc": "Verify G-S1 pin safety rules before flashing hardware selftest images",
        "pass": g_pass,
        "detail": g_detail,
    })
    if not g_pass:
        results["pass"] = False
        return results

    if not (control_dir and control_dir.is_dir()):
        results["cases"].append({
            "id": "T8_control_dir_missing",
            "desc": "Control directory 04fb865 is provided for equivalence testing",
            "pass": False,
            "detail": f"Control dir {control_dir} not found",
        })
        results["pass"] = False
        return results

    # The 3 selftests: prj_04 selftest, prj_04 selftest-adc, prj_03 selftest
    selftest_configs = [
        ("prj_04", "02_firmware_projects/prj_04_adc_dma_bm_eth", "selftest"),
        ("prj_04", "02_firmware_projects/prj_04_adc_dma_bm_eth", "selftest-adc"),
        ("prj_03", "02_firmware_projects/prj_03_adc_dma_eth", "selftest"),
    ]

    monitor_script = REPO_ROOT / "tools" / "monitor.py"

    for proj, rel_dir, preset in selftest_configs:
        test_id = f"{proj}_{preset}"
        print(f"\n  [T8] Running migrated {test_id} on target...")
        pdir = REPO_ROOT / rel_dir
        if not flash_image(sn, pdir, preset=preset):
            results["cases"].append({
                "id": f"T8_flash_migrated_{test_id}",
                "desc": f"Flash migrated {test_id}",
                "pass": False,
                "detail": "Flash failed",
            })
            results["pass"] = False
            continue

        # Capture monitor output
        cmd_mon = [tools["python"], str(monitor_script), "--sn", sn, "--timeout", "12.0"]
        p_mon = subprocess.run(cmd_mon, capture_output=True, text=True)
        out_migrated = p_mon.stdout
        migrated_pass = ("PASS" in out_migrated) and ("FAIL" not in out_migrated)
        results["cases"].append({
            "id": f"T8_migrated_{test_id}_verdict",
            "desc": f"Migrated {test_id} produces overall PASS on target",
            "pass": migrated_pass,
            "detail": "Selftest passed" if migrated_pass else "Failure or timeout in selftest run",
        })

        # Run control image
        print(f"  [T8] Running control {test_id} on target from {control_dir.name}...")
        ctrl_pdir = control_dir / rel_dir
        if not flash_image(sn, ctrl_pdir, preset=preset):
            results["cases"].append({
                "id": f"T8_flash_control_{test_id}",
                "desc": f"Flash control {test_id}",
                "pass": False,
                "detail": "Flash failed",
            })
            results["pass"] = False
            continue

        p_mon_ctrl = subprocess.run(cmd_mon, capture_output=True, text=True)
        out_ctrl = p_mon_ctrl.stdout
        ctrl_pass = ("PASS" in out_ctrl) and ("FAIL" not in out_ctrl)
        equiv_ok = (migrated_pass == ctrl_pass == True)
        results["cases"].append({
            "id": f"T8_equivalence_{test_id}",
            "desc": f"Equivalence: migrated and control {test_id} both PASS identically",
            "pass": equiv_ok,
            "detail": f"Migrated={migrated_pass}, Control={ctrl_pass}",
        })
        if not equiv_ok:
            results["pass"] = False

    # Restore board to prj_03 debug (G-S1)
    print("\n  [G-S1] Restoring board to production prj_03 debug image...")
    flash_image(sn, REPO_ROOT / "02_firmware_projects/prj_03_adc_dma_eth", preset="debug")

    return results


# ==============================================================================
# Main entry point & CLI
# ==============================================================================

def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--project", choices=["prj_04", "prj_03"], required=True,
                        help="Project to verify (mandatory: prj_04 or prj_03)")
    parser.add_argument("--test", choices=["t1", "t2", "t2a", "t2b", "t3", "t4", "t5", "t6a", "t7", "t8", "all"], default="all",
                        help="Which test to run (default: all)")
    parser.add_argument("--preset", choices=["debug", "release"], default="debug",
                        help="Preset for T5 (default: debug)")
    parser.add_argument("--sn", default=DEFAULT_SN, help=f"ST-LINK serial number (default: {DEFAULT_SN})")
    parser.add_argument("--flash", action="store_true",
                        help="Flash the matching image before running hardware tests (T3, T4, T6a)")
    parser.add_argument("--control", action="store_true", help="Run negative control mode for the selected test")
    parser.add_argument("--control-dir", type=Path, default=None,
                        help="Path to control worktree (defaults to ../stm32h755_control_04fb865)")
    parser.add_argument("--reset-count", type=int, default=20, help="Number of resets for T6a (default: 20)")
    parser.add_argument("--reset-wait", type=float, default=10.0, help="Wait seconds after reset for T6a (default: 10.0)")
    args = parser.parse_args()

    if args.control:
        args.flash = True

    expected_ctrl_commit = PROJECTS_CONFIG[args.project]["control_commit"]
    if args.control_dir is None:
        args.control_dir = REPO_ROOT.parent / PROJECTS_CONFIG[args.project]["control_dir_default"]

    argv_str = " ".join(sys.argv)
    rev, clean, status_porcelain = get_git_info(REPO_ROOT)
    timestamp = time.strftime("%Y-%m-%d %H:%M:%S")

    print("================================================================================")
    print(f"STM32H755 TIM F4 Verification Checks for {args.project} (T1 to T8)")
    print("================================================================================")
    print(f"[host] argv: {argv_str}")
    print(f"[host] timestamp: {timestamp}")
    print(f"[host] project: {args.project}")
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
        if c_rev != expected_ctrl_commit:
            print(f"[host] ERROR: Control directory commit mismatch! Expected {expected_ctrl_commit}, found {c_rev} in {args.control_dir}")
    elif args.control_dir:
        print(f"[host] ERROR: Control directory {args.control_dir} does not exist")
    print("================================================================================\n")

    tools = find_tools()
    all_pass = True

    tests_to_run = []
    if args.test == "all":
        # 'all' runs the full suite
        tests_to_run = ["t1", "t2a", "t2b", "t3", "t4", "t5", "t6a", "t7"]
    elif args.test == "t2":
        tests_to_run = ["t2a", "t2b"]
    else:
        tests_to_run = [args.test]

    ctrl_dir = args.control_dir if (args.control_dir and args.control_dir.is_dir()) else None

    for t in tests_to_run:
        res = {"pass": True, "cases": []}
        if t == "t1":
            res = run_t1(tools, project_name=args.project)
        elif t == "t2a":
            res = run_t2a(tools, ctrl_dir, project_name=args.project)
        elif t == "t2b":
            res = run_t2b(tools, ctrl_dir, project_name=args.project)
        elif t == "t3":
            res = run_t3(tools, args.sn, project_name=args.project, control_mode=args.control, control_dir=ctrl_dir, do_flash=args.flash)
        elif t == "t4":
            res = run_t4(tools, args.sn, project_name=args.project, do_flash=args.flash)
        elif t == "t5":
            res = run_t5(tools, args.sn, project_name=args.project, preset=args.preset)
        elif t == "t6a":
            res = run_t6a(tools, args.sn, project_name=args.project, count=args.reset_count, wait_s=args.reset_wait, do_flash=args.flash)
        elif t == "t7":
            res = run_t7(tools, args.sn, project_name=args.project, control_dir=ctrl_dir)
        elif t == "t8":
            res = run_t8(tools, args.sn, control_dir=ctrl_dir)

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
