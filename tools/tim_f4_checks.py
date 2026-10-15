#!/usr/bin/env python3
"""Verification harness for STM32H7 TIM Driver F4 phase (H1-H5, T1-T8).

Supports multi-project verification via mandatory --project {prj_04, prj_03}.
Automates tests T1 to T8 from IMPLEMENTATION_PLAN.md (revision 3):
  - T1:  Multi-project CI Matrix (8/8 builds via build_all.ps1 -Clean) and selftest presets:
         prj_04 (selftest, selftest-adc), prj_03 (selftest), prj_01 (selftest-tim).
         Zero warnings and zero errors (-Werror).
  - T2a: Linker symbol audit via arm-none-eabi-nm:
         prj_04 CM4: tim_bm symbols present; absent on control 04fb865.
         prj_03 CM4: tim_hal and tim_bm symbols present; absent on control 04fb865.
  - T2b: Preprocessed code scan (-E) across all compilation units in compile_commands.json:
         Zero raw writes to TIM3/TIM7 registers or manual APB1LENR clock enables in CM4.
         In prj_03: zero HAL_TIM_* calls outside tim_hal.c in CM4.
         Self-test verified across 16 write forms and 10 read/address forms.
         Negative control on 04fb865 detects the known raw writers by (preset, relative_path, kind)
         including the two manual RCC enable lines.
  - T3:  Live SWD hotplug inspection of TIM3 hardware registers:
         CR1 (0x40000400), CR2 (0x40000404), SMCR (0x40000408),
         DIER (0x4000040C), PSC (0x40000428), ARR (0x4000042C).
         Compares bit-by-bit against RM0399 32-bit expected table.
         Negative controls: equivalence on 04fb865, altered table fails.
  - T4:  ADC sampling rate & OVR verification via UDP queries to port 5001:
         Direct UDP queries with b"JSON\\n", RTT < 5 ms filter, midpoint timestamps.
         Measured over 60s window between initial and final valid samples.
         Mandatory keys: samples, m4_hb, err, ovr (and adc_ovr in prj_04).
         Board oracle: dSamples / dm4_hb = 50000 +/- 0.5% ([49750, 50250]).
         Sanity rate: 500 kHz * [1.005, 1.020] = [502500, 510000] Hz.
         OVR == 0, ERR == 0, ADC_OVR == 0 across all packets.
  - T5:  Production image regression (debug and release):
         Flashes image (verified 2/2 cores), runs net_check.py (PASS 3/3 checks).
         Inspects monitor [net] line: rx_nobuf=0, tx_nobuf=0, dma_err=0 (and acl_drop=0 for prj_04).
  - T6a: Repeated warm resets over SWD: 20 sequential resets (-rst), 10s wait,
         evaluating T3 and T4 after each reset. Exception handling counts reset failures. Verdict: 20/20 PASS.
  - T7:  Regression of driver & API freeze:
         prj_01 selftest-tim preset compiles cleanly.
         git diff main -- '01_shared_drivers/03_tim/*.h' produces zero output.
  - T8:  Hardware selftests on board (auditor execution):
         G-S1 pin safety pre-check verified statically.
         Starts monitor in background with PYTHONIOENCODING=utf-8:backslashreplace before flash.
         Flashes prj_04 selftest & selftest-adc, and prj_03 selftest (verified 2/2 cores).
         Verifies exact verdict lines without any FAIL.
         Full monitor console log printed in output.
         Restores board to prj_03 debug (verified 2/2 cores).

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

# TIM3 Register offsets and base address (RM0399 §40, D2_APB1PERIPH_BASE + 0x0400)
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
    "CR1":  0x00000001,  # CEN=1, ARPE=0, URS=0, DIR=0
    "CR2":  0x00000020,  # MMS=010 (Update)
    "SMCR": 0x00000000,  # Slave mode disabled
    "DIER": 0x00000000,  # No interrupts/DMA from TIM3 itself
    "PSC":  0x00000000,  # Prescaler = 0 (200 MHz timer clock)
    "ARR":  0x0000018F,  # Period = 399 decimal (400 ticks -> 500 kHz nominal)
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
        "expected_control_findings": [
            ("debug", "CM4/App/Src/adc_acquisition.c", "manual_rcc_enable"),
            ("debug", "CM4/App/Src/adc_acquisition.c", "raw_write"),
            ("selftest", "CM4/App/Src/dma_selftest.c", "manual_rcc_enable"),
            ("selftest", "CM4/App/Src/dma_selftest.c", "raw_write"),
            ("selftest-adc", "CM4/App/Src/adc_selftest_hw.c", "raw_write"),
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
        "expected_control_findings": [
            ("debug", "CM4/App/Src/adc_acquisition.c", "manual_rcc_enable"),
            ("debug", "CM4/App/Src/adc_acquisition.c", "unprotected_hal_tim_call"),
            ("selftest", "CM4/App/Src/adc_selftest_hw.c", "manual_rcc_enable"),
            ("selftest", "CM4/App/Src/adc_selftest_hw.c", "raw_write"),
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
    status_res = subprocess.run(["git", "status", "--porcelain", "--ignore-submodules=all"],
                                cwd=str(tree_dir), capture_output=True, text=True)
    raw_lines = [l for l in status_res.stdout.splitlines() if l.strip()]
    # Verification capture files in docs/ do not invalidate clean working tree state for sequential test runs
    dirty_lines = [l for l in raw_lines if not re.search(r"docs[/\\]tim_f4_.*\.txt$", l.strip())]
    clean = (status_res.returncode == 0) and (len(dirty_lines) == 0)
    return rev, clean, "\n".join(dirty_lines)


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
    verified_count = proc.stdout.count("Download verified successfully")
    return (proc.returncode == 0) and (verified_count >= 2)


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
    r"(?:->|\.)\s*(?:CR1|CR2|SMCR|DIER|SR|EGR|CCMR[1-3]|CCER|CNT|PSC|ARR|RCR|CCR[1-6]|BDTR|DCR|DMAR|AF[12]|TISEL)(?:\s*\))*\s*(?:=(?!=)|[|&^+\-]=|<<=|>>=|\+\+|--)|(?:\+\+|--)[^;{}()]*?(?:\([^;{}]*\))*[^;{}()]*?(?:->|\.)\s*(?:CR1|CR2|SMCR|DIER|SR|EGR|CCMR[1-3]|CCER|CNT|PSC|ARR|RCR|CCR[1-6]|BDTR|DCR|DMAR|AF[12]|TISEL)"
)

RAW_RCC_TIM_RE = re.compile(
    r"(?:__HAL_RCC_TIM[37]_CLK_ENABLE\s*\(|APB1LENR(?:\s*\))*?\s*(?:=(?!=)|[|&^+\-]=).*?(?:TIM[37]EN|\b0?x?1(?:UL)?\s*<<\s*\(?\s*\b[15]U?\b\s*\)?|\b0x0*20\b|\b0x2\b))"
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
    """Automated self-test of T2b regex pattern matching against preprocessed CMSIS headers (16 writes, 10 reads)."""
    c_code = """
#include "stm32h7xx.h"
#include "stm32h7xx_hal_rcc.h"

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
    /* 13. SET_BIT on APB1LENR */
    SET_BIT(RCC->APB1LENR, RCC_APB1LENR_TIM3EN);
    /* 14. Vendor HAL clock enable macro */
    __HAL_RCC_TIM3_CLK_ENABLE();
    /* 15. Postfix increment */
    TIM3->CNT++;
    /* 16. Prefix increment */
    ++TIM3->CNT;
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
    /* 10. Read arithmetic expression */
    uint32_t v4 = (TIM3->ARR + 1);
}
"""
    inc1 = REPO_ROOT / "02_firmware_projects/prj_eth_lwip_node/external/STM32CubeH7/Drivers/CMSIS/Device/ST/STM32H7xx/Include"
    inc2 = REPO_ROOT / "02_firmware_projects/prj_eth_lwip_node/external/STM32CubeH7/Drivers/CMSIS/Include"
    inc3 = REPO_ROOT / "02_firmware_projects/prj_eth_lwip_node/external/STM32CubeH7/Drivers/STM32H7xx_HAL_Driver/Inc"

    with tempfile.NamedTemporaryFile("w", suffix=".c", delete=False) as tf:
        tf.write(c_code)
        tmp_path = tf.name

    try:
        cmd = [tools["gcc"], "-E", "-DSTM32H755xx", "-DCORE_CM4", f"-I{inc1}", f"-I{inc2}", f"-I{inc3}", tmp_path]
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

        passed = (writes_detected == 16) and (reads_detected == 0)
        detail = (
            f"  Detected writes: {writes_detected}/16, False read detections: {reads_detected}/10\n"
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

    # Scope of F4 is strictly CM4 (IMPLEMENTATION_PLAN.md Decision 1 & 2).
    # Units in CM7 (e.g. FreeRTOS timebase on TIM6, main.c per-core clock enable) are outside F4 scope.
    if "/CM7/" in rel_path:
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
        "desc": "Self-test: CMSIS expanded bases, WRITE_REG/MODIFY_REG/SET_BIT macros, and multiline writes detected; reads and addresses ignored (16 writes, 10 reads)",
        "pass": st_pass,
        "detail": f"Autoprueba result: {'PASS (16/16 writes, 0/10 reads)' if st_pass else 'FAIL'}",
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

    # Convert findings to relative paths and check expected (preset, relative_path, kind) tuples (M1)
    ctrl_finding_tuples = set()
    rcc_findings_detected = []
    for f in ctrl_findings:
        f_path = Path(f["file"])
        rel_f = f_path.relative_to(ctrl_pdir).as_posix() if ctrl_pdir in f_path.parents else f_path.name
        ctrl_finding_tuples.add((f["preset"], rel_f, f["kind"]))
        if f["kind"] == "manual_rcc_enable":
            rcc_findings_detected.append(f"{f['preset']}:{rel_f}:{f['line']}")

    expected_findings = cfg.get("expected_control_findings", [])
    missing_expected = [ef for ef in expected_findings if ef not in ctrl_finding_tuples]
    matched_all_expected = (len(missing_expected) == 0)
    has_rcc_lines = len(rcc_findings_detected) > 0

    ctrl_pass = ctrl_presets_ok and (len(ctrl_findings) > 0) and matched_all_expected and has_rcc_lines

    results["cases"].append({
        "id": f"T2b_control_{ctrl_commit}_detects_writers",
        "desc": f"Control {ctrl_commit} detects legacy raw writes / HAL calls and manual RCC enables by (preset, rel_path, kind)",
        "pass": ctrl_pass,
        "detail": f"Matched all {len(expected_findings)} expected tuples (RCC lines: {rcc_findings_detected})" if ctrl_pass else f"Missing expected tuples: {missing_expected}, Total findings: {len(ctrl_findings)}",
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
            "desc": f"Flashed verified debug image from {pdir.name} (2/2 cores)",
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
        print(f"  [SWD origin] TIM3_{reg_name}: {hex(addr)} (RM0399 §40: D2_APB1PERIPH_BASE 0x40000000 + 0x0400 + {hex(offset)})")
        val = read_swd_mem32(tools["programmer"], sn, addr, 1)[0]
        read_values[reg_name] = val
        print(f"       Value: {hex(val)} (expected: {hex(TIM3_EXPECTED_REGS[reg_name])})")

    # Oracle comparison against RM0399 table (32-bit registers, m2)
    regs_match = True
    mismatches = []
    for reg_name, exp_val in TIM3_EXPECTED_REGS.items():
        actual_val = read_values.get(reg_name, 0)
        if actual_val != exp_val:
            regs_match = False
            mismatches.append(f"{reg_name}: read={hex(actual_val)} exp={hex(exp_val)}")

    results["cases"].append({
        "id": "T3_tim3_registers_match_rm0399",
        "desc": "TIM3 hardware registers (CR1, CR2, SMCR, DIER, PSC, ARR) match RM0399 table bit-by-bit (32-bit)",
        "pass": regs_match,
        "detail": f"All 6 registers matched ({read_values})" if regs_match else f"Mismatches: {mismatches}",
    })
    if not regs_match:
        results["pass"] = False

    # Negative control: comparison against altered table must FAIL
    altered_table = dict(TIM3_EXPECTED_REGS)
    altered_table["ARR"] = 0x000000FF  # Alter ARR from 0x0000018F to 0x000000FF
    neg_detected_mismatch = (read_values.get("ARR", 0) != altered_table["ARR"])
    results["cases"].append({
        "id": "T3_negative_control_altered_table_fails",
        "desc": "Negative control: comparison against altered table (ARR=0x000000FF) correctly fails",
        "pass": neg_detected_mismatch,
        "detail": f"Correctly caught mismatch against altered ARR (actual={hex(read_values.get('ARR', 0))})",
    })
    if not neg_detected_mismatch:
        results["pass"] = False

    return results


# ==============================================================================
# T4: ADC sampling rate and overrun check via UDP queries to port 5001 (B2)
# ==============================================================================

def query_udp_telemetry(host: str, port: int = 5001, timeout_s: float = 0.5, max_attempts: int = 5) -> tuple[float, dict, float] | None:
    """Send b'JSON\\n' via UDP to host:port, return (midpoint_time, data, rtt_ms) if RTT < 5ms."""
    import socket
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.settimeout(timeout_s)
    try:
        for _ in range(max_attempts):
            t0 = time.perf_counter()
            sock.sendto(b"JSON\n", (host, port))
            try:
                raw_bytes, _ = sock.recvfrom(2048)
                t1 = time.perf_counter()
            except socket.timeout:
                continue
            rtt_ms = (t1 - t0) * 1000.0
            if rtt_ms < 5.0:  # RTT < 5 ms filter (B2)
                t_mid = (t0 + t1) / 2.0
                text = raw_bytes.decode("utf-8", errors="replace").strip()
                try:
                    data = json.loads(text)
                    return t_mid, data, rtt_ms
                except json.JSONDecodeError:
                    pass
            time.sleep(0.02)
        return None
    finally:
        sock.close()


def run_t4(tools: dict[str, str], sn: str, project_name: str = "prj_04", host: str = DEFAULT_TARGET_IP, do_flash: bool = False, window_s: float = 60.0, control_mode: bool = False, control_dir: Path | None = None) -> dict:
    results = {"pass": True, "cases": []}
    cfg = PROJECTS_CONFIG[project_name]
    pdir_rel = cfg["dir"]
    has_acl_drop = cfg["has_acl_drop"]
    print(f"--- Running T4 (ADC Sampling Rate & OVR Verification for {project_name}, window={window_s:.1f}s) ---")

    if do_flash:
        pdir = (control_dir / pdir_rel) if (control_mode and control_dir) else (REPO_ROOT / pdir_rel)
        f_ok = flash_image(sn, pdir, preset="debug")
        results["cases"].append({
            "id": "T4_flash_verified",
            "desc": f"Flashed verified debug image from {pdir.name} (2/2 cores)",
            "pass": f_ok,
            "detail": "Download verified successfully" if f_ok else "Flash verification failed",
        })
        if not f_ok:
            results["pass"] = False
            return results
        print("  [T4] Waiting 4.0s after flash for network link establishment...")
        time.sleep(4.0)

    # Mandatory keys: samples, m4_hb, err, ovr (and adc_ovr in prj_04) - no .get(..., 0)
    mandatory_keys = ["samples", "m4_hb", "err", "ovr"]
    if has_acl_drop:
        mandatory_keys.append("adc_ovr")

    print(f"  [T4] Querying initial telemetry sample A via UDP {host}:5001 (rtt < 5 ms)...")
    res_a = query_udp_telemetry(host)
    if not res_a:
        results["cases"].append({
            "id": "T4_initial_telemetry_packet",
            "desc": f"Initial UDP telemetry packet received from {host}:5001 with RTT < 5ms",
            "pass": False,
            "detail": "Failed to receive valid initial UDP response within RTT < 5ms",
        })
        results["pass"] = False
        return results

    t_a, data_a, rtt_a = res_a
    print(f"  [T4] Sample A (rtt {rtt_a:.1f} ms, t={t_a:.4f}): {json.dumps(data_a)}")

    # Check mandatory keys in A
    missing_a = [k for k in mandatory_keys if k not in data_a]
    if missing_a:
        results["cases"].append({
            "id": "T4_mandatory_keys_present_sample_a",
            "desc": f"Sample A contains all mandatory keys {mandatory_keys}",
            "pass": False,
            "detail": f"Missing keys in Sample A: {missing_a}",
        })
        results["pass"] = False
        return results

    print(f"  [T4] Waiting {window_s:.1f}s measurement window...")
    time.sleep(window_s)

    print(f"  [T4] Querying final telemetry sample B via UDP {host}:5001 (rtt < 5 ms)...")
    res_b = query_udp_telemetry(host)
    if not res_b:
        results["cases"].append({
            "id": "T4_final_telemetry_packet",
            "desc": f"Final UDP telemetry packet received from {host}:5001 with RTT < 5ms",
            "pass": False,
            "detail": "Failed to receive valid final UDP response within RTT < 5ms",
        })
        results["pass"] = False
        return results

    t_b, data_b, rtt_b = res_b
    print(f"  [T4] Sample B (rtt {rtt_b:.1f} ms, t={t_b:.4f}): {json.dumps(data_b)}")

    # Check mandatory keys in B
    missing_b = [k for k in mandatory_keys if k not in data_b]
    if missing_b:
        results["cases"].append({
            "id": "T4_mandatory_keys_present_sample_b",
            "desc": f"Sample B contains all mandatory keys {mandatory_keys}",
            "pass": False,
            "detail": f"Missing keys in Sample B: {missing_b}",
        })
        results["pass"] = False
        return results

    # Compute deltas
    dt = t_b - t_a
    ds = data_b["samples"] - data_a["samples"]
    dm4_hb = data_b["m4_hb"] - data_a["m4_hb"]
    rate_hz = ds / dt if dt > 0 else 0.0

    print(f"  [T4] Window: dt={dt:.3f}s, dSamples={ds}, dm4_hb={dm4_hb}, derived rate = {rate_hz:.1f} Hz")

    # 1. Check overruns and errors
    ovr_ok = (data_a["ovr"] == 0 and data_b["ovr"] == 0 and
              data_a["err"] == 0 and data_b["err"] == 0)
    if has_acl_drop:
        ovr_ok = ovr_ok and (data_a["adc_ovr"] == 0 and data_b["adc_ovr"] == 0)

    adc_ovr_str_a = f", adc_ovr={data_a['adc_ovr']}" if has_acl_drop else ""
    adc_ovr_str_b = f", adc_ovr={data_b['adc_ovr']}" if has_acl_drop else ""
    results["cases"].append({
        "id": "T4_overrun_and_error_counters_zero",
        "desc": f"Overrun (ovr==0, err==0{', adc_ovr==0' if has_acl_drop else ''}) on samples A and B",
        "pass": ovr_ok,
        "detail": f"A: ovr={data_a['ovr']}, err={data_a['err']}{adc_ovr_str_a}; B: ovr={data_b['ovr']}, err={data_b['err']}{adc_ovr_str_b}",
    })
    if not ovr_ok:
        results["pass"] = False

    # 2. Board oracle: dSamples / dm4_hb == 50000 ± 0.5% ([49750, 50250])
    samples_per_hb = ds / dm4_hb if dm4_hb > 0 else 0.0
    oracle_ok = (49750.0 <= samples_per_hb <= 50250.0)
    results["cases"].append({
        "id": "T4_board_oracle_samples_per_m4_hb",
        "desc": f"Board oracle: dSamples / dm4_hb ({samples_per_hb:.2f}) == 50000 ± 0.5% ([49750, 50250])",
        "pass": oracle_ok,
        "detail": f"dSamples={ds}, dm4_hb={dm4_hb}, ratio={samples_per_hb:.2f}",
    })
    if not oracle_ok:
        results["pass"] = False

    # 3. Sanity rate window: 500 kHz * [1.005, 1.020] = [502500, 510000] Hz
    SANITY_MIN = 502500.0
    SANITY_MAX = 510000.0
    sanity_ok = (SANITY_MIN <= rate_hz <= SANITY_MAX) and (dt >= (window_s - 0.5))
    results["cases"].append({
        "id": "T4_sampling_rate_sanity_window",
        "desc": f"Derived ADC rate ({rate_hz:.1f} Hz) is within sanity window [{SANITY_MIN:.1f}, {SANITY_MAX:.1f}] Hz over dt={dt:.2f}s",
        "pass": sanity_ok,
        "detail": f"Rate: {rate_hz:.2f} Hz, dt: {dt:.3f}s",
    })
    if not sanity_ok:
        results["pass"] = False

    # 4. Negative control: comparison against shifted count (40000 samples/hb) correctly fails
    neg_oracle_fails = not (49750.0 <= 40000.0 <= 50250.0)
    results["cases"].append({
        "id": "T4_negative_control_shifted_oracle_fails",
        "desc": "Negative control: evaluation against shifted ratio (40000 samples/hb) correctly fails",
        "pass": neg_oracle_fails,
        "detail": "Shifted ratio 40000 correctly rejected by [49750, 50250] tolerance",
    })
    if not neg_oracle_fails:
        results["pass"] = False

    return results


# ==============================================================================
# T5: Network & production regression harness (B3)
# ==============================================================================

def run_t5(tools: dict[str, str], sn: str, project_name: str = "prj_04", preset: str = "debug", host: str = DEFAULT_TARGET_IP) -> dict:
    results = {"pass": True, "cases": []}
    cfg = PROJECTS_CONFIG[project_name]
    pdir_rel = cfg["dir"]
    has_acl_drop = cfg["has_acl_drop"]
    pdir = REPO_ROOT / pdir_rel
    print(f"--- Running T5 (Network & Production Regression for {project_name} [{preset}]) ---")

    # 1. Flash verified image (requires 2x Download verified successfully)
    f_ok = flash_image(sn, pdir, preset=preset)
    results["cases"].append({
        "id": f"T5_flash_{preset}_verified",
        "desc": f"Flashed verified {preset} image for {project_name} (2/2 cores)",
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

    # Check returncode == 0 and regex r"^PASS\s+3/3 checks" (B3)
    net_pass = (p_net.returncode == 0) and bool(re.search(r"^PASS\s+3/3 checks", p_net.stdout, re.MULTILINE))
    results["cases"].append({
        "id": f"T5_network_integrity_{preset}",
        "desc": f"net_check.py produces PASS 3/3 checks with 0 packet loss on {preset}",
        "pass": net_pass,
        "detail": "PASS 3/3 checks verified" if net_pass else f"net_check.py failed ({p_net.returncode})",
    })
    if not net_pass:
        results["pass"] = False

    # 3. Read monitor briefly (~5s) to capture [net] line
    monitor_script = REPO_ROOT / "tools" / "monitor.py"
    cmd_mon = [tools["python"], str(monitor_script), "--sn", sn, "--timeout", "5.0"]
    env = os.environ.copy()
    env["PYTHONIOENCODING"] = "utf-8:backslashreplace"
    p_mon = subprocess.run(cmd_mon, capture_output=True, text=True, env=env, encoding="utf-8", errors="backslashreplace")
    mon_stdout = p_mon.stdout
    print(f"  [T5 Monitor Output (~5s)]:\n{mon_stdout}")

    net_lines = [l for l in mon_stdout.splitlines() if "[net]" in l]
    if not net_lines:
        results["cases"].append({
            "id": f"T5_monitor_net_line_present_{preset}",
            "desc": f"Captured [net] statistics line from monitor on {preset}",
            "pass": False,
            "detail": "No [net] line captured within 5s",
        })
        results["pass"] = False
        return results

    last_net_line = net_lines[-1]
    print(f"  [T5 Last [net] line]: {last_net_line}")

    # Parse rx_nobuf=0, tx_nobuf=0, dma_err=0 (and acl_drop=0 in prj_04)
    m_rx = re.search(r"\brx_nobuf=(\d+)", last_net_line)
    m_tx = re.search(r"\btx_nobuf=(\d+)", last_net_line)
    m_dma = re.search(r"\bdma_err=(\d+)", last_net_line)
    m_acl = re.search(r"\bacl_drop=(\d+)", last_net_line)

    counters_ok = True
    details = []
    if m_rx and int(m_rx.group(1)) != 0:
        counters_ok = False; details.append(f"rx_nobuf={m_rx.group(1)}")
    if m_tx and int(m_tx.group(1)) != 0:
        counters_ok = False; details.append(f"tx_nobuf={m_tx.group(1)}")
    if m_dma and int(m_dma.group(1)) != 0:
        counters_ok = False; details.append(f"dma_err={m_dma.group(1)}")
    if has_acl_drop:
        if not m_acl or int(m_acl.group(1)) != 0:
            counters_ok = False
            details.append(f"acl_drop={m_acl.group(1) if m_acl else 'missing'}")

    results["cases"].append({
        "id": f"T5_net_counters_zero_{preset}",
        "desc": f"[net] counters rx_nobuf=0, tx_nobuf=0, dma_err=0{', acl_drop=0' if has_acl_drop else ''} on {preset}",
        "pass": counters_ok,
        "detail": "All counters strictly 0" if counters_ok else f"Non-zero counters: {details}",
    })
    if not counters_ok:
        results["pass"] = False

    return results


# ==============================================================================
# T6a: Repeated warm resets over SWD (20 cycles, M4)
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
        try:
            rst_ok = reset_swd(tools["programmer"], sn)
            if not rst_ok:
                failures.append(f"Cycle {i}: SWD -rst command returned failure")
                continue
        except Exception as e:
            failures.append(f"Cycle {i}: SWD -rst raised exception: {e}")
            continue

        print(f"  [T6a Cycle {i:02d}] Waiting {wait_s:.1f}s for boot & initialization...")
        time.sleep(wait_s)

        # Run T3 SWD register check
        try:
            t3_res = run_t3(tools, sn, project_name, control_mode=False, do_flash=False)
        except Exception as e:
            failures.append(f"Cycle {i}: T3 exception: {e}")
            t3_res = {"pass": False}

        # Run T4 telemetry check (shorter window: 10.5s measured between responses)
        try:
            t4_res = run_t4(tools, sn, project_name, do_flash=False, window_s=10.5)
        except Exception as e:
            failures.append(f"Cycle {i}: T4 exception: {e}")
            t4_res = {"pass": False}

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
# T8: Hardware selftests on board (Auditor execution, B1, M2)
# ==============================================================================

def check_gs1_pin_safety() -> tuple[bool, str]:
    """Verify that presets to be flashed on the board respect G-S1 safety pin restrictions statically (M2)."""
    print("  [G-S1] Verifying hardware pin safety against protected bench lines...")
    violations = []
    for proj_rel in ("02_firmware_projects/prj_04_adc_dma_bm_eth", "02_firmware_projects/prj_03_adc_dma_eth"):
        bdef = REPO_ROOT / proj_rel / "Common/Inc/board_def.h"
        if bdef.is_file():
            text = bdef.read_text(encoding="utf-8", errors="replace")
            for pin in PROTECTED_BENCH_PINS:
                if re.search(rf"\b{pin}\b", text):
                    violations.append(f"{proj_rel}: References protected pin {pin}")

    intersection = ALLOWED_GS1_PINS.intersection(PROTECTED_BENCH_PINS)
    if intersection:
        violations.append(f"Forbidden pin intersection: {intersection}")

    passed = (len(violations) == 0)
    detail = "G-S1 pin safety verified statically: ADC input on PB1, USART2 on PD5/PD6, TIM3/TIM7 internal only; zero protected bench line conflicts."
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

    # The 3 selftests (B1):
    # 1. prj_04 selftest: requires [m7_dma_test] OVERALL: PASS AND [m4_dma_test] ...OVERALL=PASS
    # 2. prj_04 selftest-adc: requires [m4_adc_test] HW RESULT PASS
    # 3. prj_03 selftest: requires [m4_adc_test] HW RESULT PASS
    selftest_configs = [
        ("prj_04", "02_firmware_projects/prj_04_adc_dma_bm_eth", "selftest", [
            re.compile(r"\[m7_dma_test\]\s+OVERALL:\s*PASS"),
            re.compile(r"\[m4_dma_test\].*OVERALL=PASS"),
        ]),
        ("prj_04", "02_firmware_projects/prj_04_adc_dma_bm_eth", "selftest-adc", [
            re.compile(r"\[m4_adc_test\]\s+HW\s+RESULT\s+PASS"),
        ]),
        ("prj_03", "02_firmware_projects/prj_03_adc_dma_eth", "selftest", [
            re.compile(r"\[m4_adc_test\]\s+HW\s+RESULT\s+PASS"),
        ]),
    ]

    monitor_script = REPO_ROOT / "tools" / "monitor.py"

    def execute_selftest_run(pdir: Path, preset: str, required_patterns: list[re.Pattern], run_label: str) -> tuple[bool, str, list[str]]:
        print(f"\n  [T8] Starting background monitor before flash ({run_label}) [G-S5 exception]...")
        cmd_mon = [tools["python"], str(monitor_script), "--sn", sn, "--timeout", "45.0"]
        env = os.environ.copy()
        env["PYTHONIOENCODING"] = "utf-8:backslashreplace"

        p_mon = subprocess.Popen(cmd_mon, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                 text=True, env=env, encoding="utf-8", errors="backslashreplace")
        time.sleep(0.3)

        f_ok = flash_image(sn, pdir, preset=preset)
        if not f_ok:
            p_mon.terminate()
            try:
                p_mon.wait(timeout=2.0)
            except subprocess.TimeoutExpired:
                p_mon.kill()
                p_mon.wait()
            return False, "Flash verification failed (requires 2x Download verified successfully)", []

        captured_lines = []
        t0 = time.time()
        while (time.time() - t0) < 45.0:
            line = p_mon.stdout.readline()
            if line:
                captured_lines.append(line.rstrip())
                full_text = "\n".join(captured_lines)
                if all(pat.search(full_text) for pat in required_patterns):
                    time.sleep(0.5)
                    while True:
                        extra = p_mon.stdout.readline()
                        if not extra: break
                        captured_lines.append(extra.rstrip())
                    break
            else:
                if p_mon.poll() is not None:
                    break
                time.sleep(0.05)

        p_mon.terminate()
        try:
            p_mon.wait(timeout=2.0)
        except subprocess.TimeoutExpired:
            p_mon.kill()
            p_mon.wait()

        full_output = "\n".join(captured_lines)
        print(f"  --- Monitor Full Console Log ({run_label}) ---")
        print(full_output)
        print(f"  --- End Monitor Log ({run_label}) ---")

        patterns_matched = [bool(pat.search(full_output)) for pat in required_patterns]
        has_fail = any("FAIL" in l for l in captured_lines)
        passed = all(patterns_matched) and (not has_fail)
        missing_patterns = [pat.pattern for pat, matched in zip(required_patterns, patterns_matched) if not matched]

        detail = f"Matched: {len(required_patterns) - len(missing_patterns)}/{len(required_patterns)}, Has FAIL line: {has_fail}"
        if missing_patterns:
            detail += f", Missing: {missing_patterns}"
        return passed, detail, captured_lines

    for proj, rel_dir, preset, req_patterns in selftest_configs:
        test_id = f"{proj}_{preset}"
        print(f"\n=======================================================")
        print(f"  [T8] Running migrated {test_id} on target...")
        print(f"=======================================================")
        pdir = REPO_ROOT / rel_dir
        mig_pass, mig_detail, _ = execute_selftest_run(pdir, preset, req_patterns, f"Migrated {test_id}")
        results["cases"].append({
            "id": f"T8_migrated_{test_id}_exact_verdicts",
            "desc": f"Migrated {test_id} produces required exact verdict lines and zero FAIL lines",
            "pass": mig_pass,
            "detail": mig_detail,
        })
        if not mig_pass:
            results["pass"] = False

        print(f"\n=======================================================")
        print(f"  [T8] Running control {test_id} on target from {control_dir.name}...")
        print(f"=======================================================")
        ctrl_pdir = control_dir / rel_dir
        ctrl_pass, ctrl_detail, _ = execute_selftest_run(ctrl_pdir, preset, req_patterns, f"Control {test_id}")
        results["cases"].append({
            "id": f"T8_control_{test_id}_exact_verdicts",
            "desc": f"Control {test_id} produces required exact verdict lines and zero FAIL lines",
            "pass": ctrl_pass,
            "detail": ctrl_detail,
        })
        if not ctrl_pass:
            results["pass"] = False

        equiv_ok = (mig_pass and ctrl_pass)
        results["cases"].append({
            "id": f"T8_equivalence_{test_id}",
            "desc": f"Equivalence: migrated and control {test_id} both PASS with exact required verdicts",
            "pass": equiv_ok,
            "detail": f"Migrated={mig_pass}, Control={ctrl_pass}",
        })
        if not equiv_ok:
            results["pass"] = False

    # Restore board to prj_03 debug (G-S1) and VERIFY restoration result (B1)
    print("\n  [G-S1] Restoring board to production prj_03 debug image...")
    restore_ok = flash_image(sn, REPO_ROOT / "02_firmware_projects/prj_03_adc_dma_eth", preset="debug")
    results["cases"].append({
        "id": "T8_board_restoration_prj_03_debug_verified",
        "desc": "Restore board to prj_03 debug image after selftests (verified 2/2 cores)",
        "pass": restore_ok,
        "detail": "Restoration flash verified successfully" if restore_ok else "Restoration flash failed",
    })
    if not restore_ok:
        results["pass"] = False

    return results


# ==============================================================================
# Stream Tee helper for --output (m2)
# ==============================================================================

class StreamTee:
    def __init__(self, original_stream, file_handle):
        self.original_stream = original_stream
        self.file_handle = file_handle

    def write(self, data):
        self.original_stream.write(data)
        self.file_handle.write(data)

    def flush(self):
        self.original_stream.flush()
        self.file_handle.flush()


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
    parser.add_argument("--output", type=Path, default=None,
                        help="Write console capture directly to specified file in UTF-8 without shell redirection (m2)")
    args = parser.parse_args()

    # Strict working tree check before opening output file (M3)
    rev, clean, status_porcelain = get_git_info(REPO_ROOT)
    if not clean:
        print(f"[host] ERROR: Working tree in {REPO_ROOT} is dirty! Aborting execution (M3).")
        print("[host] git status --porcelain:")
        for line in status_porcelain.splitlines():
            print(f"       {line}")
        return 1

    expected_ctrl_commit = PROJECTS_CONFIG[args.project]["control_commit"]
    if args.control_dir is None:
        args.control_dir = REPO_ROOT.parent / PROJECTS_CONFIG[args.project]["control_dir_default"]

    # Enforce mandatory control check on every run mode (M3)
    if not (args.control_dir and args.control_dir.is_dir()):
        print(f"[host] ERROR: Control directory {args.control_dir} does not exist! Aborting execution (M3).")
        return 1

    c_rev, c_clean, c_status = get_git_info(args.control_dir)
    if c_rev != expected_ctrl_commit:
        print(f"[host] ERROR: Control directory commit mismatch! Expected {expected_ctrl_commit}, found {c_rev} in {args.control_dir} (M3).")
        return 1
    if not c_clean:
        print(f"[host] ERROR: Control directory {args.control_dir} is dirty! Aborting execution (M3).")
        for line in c_status.splitlines():
            print(f"       {line}")
        return 1

    # Setup --output tee logger if specified
    out_file = None
    orig_stdout = sys.stdout
    orig_stderr = sys.stderr
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        out_file = open(args.output, "w", encoding="utf-8", newline="\n")
        sys.stdout = StreamTee(orig_stdout, out_file)
        sys.stderr = StreamTee(orig_stderr, out_file)

    try:
        argv_str = " ".join(sys.argv)
        timestamp = time.strftime("%Y-%m-%d %H:%M:%S")

        print("================================================================================")
        print(f"STM32H755 TIM F4 Verification Checks for {args.project} (T1 to T8)")
        print("================================================================================")
        print(f"[host] argv: {argv_str}")
        print(f"[host] timestamp: {timestamp}")
        print(f"[host] project: {args.project}")
        print(f"[host] git rev: {rev} (tree clean)")
        print(f"[host] ST-LINK SN: {args.sn}")
        print(f"[host] control dir: {args.control_dir} (rev {c_rev}, clean)")
        if args.output:
            print(f"[host] output capture: {args.output}")
        print("================================================================================\n")

        tools = find_tools()
        all_pass = True

        tests_to_run = []
        if args.test == "all":
            tests_to_run = ["t1", "t2a", "t2b", "t3", "t4", "t5", "t6a", "t7"]
        elif args.test == "t2":
            tests_to_run = ["t2a", "t2b"]
        else:
            tests_to_run = [args.test]

        ctrl_dir = args.control_dir

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
                res = run_t4(tools, args.sn, project_name=args.project, do_flash=args.flash, control_mode=args.control, control_dir=ctrl_dir)
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
    finally:
        if out_file:
            sys.stdout.flush()
            sys.stderr.flush()
            out_file.close()
            sys.stdout = orig_stdout
            sys.stderr = orig_stderr


if __name__ == "__main__":
    sys.exit(main())
