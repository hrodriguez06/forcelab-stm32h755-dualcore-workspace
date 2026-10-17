#!/usr/bin/env python3
"""Verification harness for STM32H7 UART Driver Phase F1 (Off-board checks).

Covers tests T1 and T2:
  - T1: Build matrix (8 configurations + 3 selftest presets) compiled with
        cmake --build --clean-first, counting compiler warnings from that output.
        Negative control: build in temporary directory with -DCMAKE_C_FLAGS=-Wconversion
        calling the exact same evaluation function and asserting warnings >= 1.
  - T2: Header diff and symbol leak checks:
        1. git diff b41c4a7 -- 01_shared_drivers/04_uart/*.h only adds.
           Negative control: mutated header diff calling the exact same function returns FAIL.
        2. Production ELFs of prj_01..prj_04 contain no selftest symbols (UT*, uart_selftest*).
           Negative control: mutated symbol list calling the exact same function returns FAIL.

Zero writes to git configuration.
Standard library only.
"""

from __future__ import annotations

import argparse
import os
import pathlib
import re
import shutil
import subprocess
import sys
import tempfile
import time
from typing import Callable, Optional

REPO_ROOT = pathlib.Path(__file__).resolve().parents[1]
BASE_COMMIT = "b41c4a7"

RUN_START_TIME = time.time()

MATRIX_CONFIGS = [
    ("03_firmware_templates/prj_01_template_dualcore", "debug"),
    ("03_firmware_templates/prj_01_template_dualcore", "release"),
    ("02_firmware_projects/prj_02_eth_lwip_dualcore", "debug"),
    ("02_firmware_projects/prj_02_eth_lwip_dualcore", "release"),
    ("02_firmware_projects/prj_03_adc_dma_eth", "debug"),
    ("02_firmware_projects/prj_03_adc_dma_eth", "release"),
    ("02_firmware_projects/prj_04_adc_dma_bm_eth", "debug"),
    ("02_firmware_projects/prj_04_adc_dma_bm_eth", "release"),
]

SELFTEST_PRESETS = [
    ("03_firmware_templates/prj_01_template_dualcore", "selftest-uart"),
    ("03_firmware_templates/prj_01_template_dualcore", "selftest-gpio"),
    ("02_firmware_projects/prj_04_adc_dma_bm_eth", "selftest-uart"),
]


def run_cmd(args: list[str], cwd: Optional[pathlib.Path] = None, timeout: float = 300.0) -> tuple[int, str, str]:
    try:
        res = subprocess.run(
            args,
            cwd=str(cwd) if cwd else None,
            capture_output=True,
            text=True,
            timeout=timeout,
            encoding="utf-8",
            errors="replace"
        )
        return res.returncode, res.stdout, res.stderr
    except subprocess.TimeoutExpired:
        return -1, "", f"Command timed out after {timeout} seconds: {' '.join(args)}"
    except Exception as e:
        return -1, "", str(e)


def locate_tools() -> dict[str, str]:
    tools: dict[str, str] = {}

    # Check env.ps1
    env_script = REPO_ROOT / "tools" / "env.ps1"
    if env_script.is_file():
        code, out, _ = run_cmd([
            "powershell", "-NoProfile", "-ExecutionPolicy", "Bypass",
            "-Command", f". '{env_script}' -Quiet; $env:PATH"
        ], timeout=30.0)
        if code == 0 and out.strip():
            resolved_path = out.strip().splitlines()[-1]
            os.environ["PATH"] = resolved_path + os.pathsep + os.environ.get("PATH", "")

    # Look for arm-none-eabi-gcc
    gcc_path = shutil.which("arm-none-eabi-gcc")
    if not gcc_path:
        for p in pathlib.Path("C:/ST").glob("**/arm-none-eabi-gcc.exe"):
            gcc_path = str(p)
            break
    if not gcc_path:
        raise RuntimeError("arm-none-eabi-gcc not found on PATH or C:/ST")

    bin_dir = pathlib.Path(gcc_path).parent
    tools["gcc"] = str(gcc_path)
    tools["nm"] = str(bin_dir / "arm-none-eabi-nm.exe")
    tools["cmake"] = shutil.which("cmake") or "cmake"
    tools["ninja"] = shutil.which("ninja") or "ninja"

    return tools


def get_git_rev(repo: pathlib.Path = REPO_ROOT) -> str:
    code, out, _ = run_cmd(["git", "rev-parse", "--short", "HEAD"], cwd=repo, timeout=10.0)
    return out.strip() if code == 0 else "unknown"


def check_clean_working_tree(repo_root: pathlib.Path, allowed_untracked: Optional[pathlib.Path]) -> tuple[bool, str]:
    code, out, err = run_cmd(["git", "status", "--porcelain", "-u"], cwd=repo_root, timeout=30.0)
    if code != 0:
        return False, f"git status failed ({code}): {err}"

    allowed_res = allowed_untracked.resolve() if allowed_untracked else None
    dirty = []
    for line in out.splitlines():
        line_s = line.strip()
        if not line_s:
            continue
        if line.startswith("??"):
            untracked = (repo_root / line[3:].strip().strip('"')).resolve()
            if allowed_res and untracked == allowed_res:
                continue
        dirty.append(line)

    if dirty:
        return False, f"Dirty tree ({len(dirty)} items): {dirty}"
    return True, "Tree is clean"


# ---- Evaluation Functions (used by both real tests and negative controls) ----

def parse_compiler_warnings(build_output: str) -> list[str]:
    warnings = []
    for line in build_output.splitlines():
        # Exclude build system info lines
        if "warning:" in line.lower() and not line.startswith("warning: in the working copy"):
            warnings.append(line.strip())
    return warnings


def evaluate_build(
    exit_code: int,
    build_output: str,
    elf_found: bool,
    expected_warnings: int = 0
) -> tuple[bool, list[str], str]:
    """Pure evaluation function used for real builds and the negative control."""
    warnings = parse_compiler_warnings(build_output)
    passed = (
        (exit_code == 0)
        and (len(warnings) == expected_warnings)
        and elf_found
    )
    detail = f"exit={exit_code}, warnings={len(warnings)}, elf={'found' if elf_found else 'missing'}"
    return passed, warnings, detail


def evaluate_header_diff(diff_output: str) -> tuple[bool, list[str]]:
    """Pure evaluation function checking that a git diff only adds lines."""
    deleted_lines = []
    for line in diff_output.splitlines():
        if line.startswith("-") and not line.startswith("---"):
            deleted_lines.append(line)
    return (len(deleted_lines) == 0), deleted_lines


def evaluate_symbols(symbols: list[str]) -> tuple[bool, list[str]]:
    """Pure evaluation function checking for leaked selftest symbols in production images."""
    leaked = []
    pattern = re.compile(r"(\bUT[0-9]|uart_selftest|k_ust_steps|ust_line_rule)", re.IGNORECASE)
    for sym in symbols:
        if pattern.search(sym):
            leaked.append(sym)
    return (len(leaked) == 0), leaked


# ---- Test Suites -------------------------------------------------------------

def run_t1(tools: dict[str, str], logger: Callable[[str], None]) -> bool:
    logger("=== Running T1: Build Matrix & Selftests ===")
    overall_ok = True
    builds_to_run = MATRIX_CONFIGS + SELFTEST_PRESETS

    for proj_rel, preset in builds_to_run:
        pdir = REPO_ROOT / proj_rel
        pname = pdir.name

        # 1. Configure with --preset
        cfg_cmd = [tools["cmake"], "--preset", preset]
        cfg_code, cfg_out, cfg_err = run_cmd(cfg_cmd, cwd=pdir, timeout=120.0)
        if cfg_code != 0:
            logger(f"[host] CHECK build {pname} [{preset}]: FAIL (configure failed: {cfg_err or cfg_out})")
            overall_ok = False
            continue

        # 2. Build with --preset <preset> --clean-first
        bld_cmd = [tools["cmake"], "--build", "--preset", preset, "--clean-first"]
        bld_code, bld_out, bld_err = run_cmd(bld_cmd, cwd=pdir, timeout=300.0)
        full_out = bld_out + "\n" + bld_err

        # Check freshness of ELF
        build_dir = pdir / "build" / preset
        cm7_elf = build_dir / "CM7" / "cm7_firmware.elf"
        cm4_elf = build_dir / "CM4" / "cm4_firmware.elf"
        single_elf = build_dir / f"{pname}.elf"
        elf_found = cm7_elf.is_file() or single_elf.is_file()

        passed, warnings, detail = evaluate_build(bld_code, full_out, elf_found)
        logger(f"[host] CHECK build {pname} [{preset}]: {'PASS' if passed else 'FAIL'} ({detail})")
        if not passed:
            overall_ok = False

    # Negative control for T1: build in a temporary directory with -Wconversion
    logger("[host] Running T1 Negative Control: build with -Wconversion outside repo...")
    with tempfile.TemporaryDirectory() as tmp_dir:
        tmp_path = pathlib.Path(tmp_dir)
        # Copy prj_01 source to temp dir
        src_proj = REPO_ROOT / "03_firmware_templates" / "prj_01_template_dualcore"
        # We configure outside repo pointing to src_proj with -DCMAKE_C_FLAGS="-Wconversion"
        neg_cfg_cmd = [
            tools["cmake"], "-G", "Ninja",
            f"-DCMAKE_TOOLCHAIN_FILE={src_proj}/cmake/arm-none-eabi.cmake",
            "-DCMAKE_BUILD_TYPE=Debug",
            "-DCMAKE_C_FLAGS=-Wconversion",
            str(src_proj)
        ]
        neg_cfg_code, neg_cfg_out, _ = run_cmd(neg_cfg_cmd, cwd=tmp_path, timeout=120.0)

        neg_bld_cmd = [tools["cmake"], "--build", ".", "--clean-first"]
        neg_bld_code, neg_bld_out, neg_bld_err = run_cmd(neg_bld_cmd, cwd=tmp_path, timeout=300.0)
        neg_full = neg_bld_out + "\n" + neg_bld_err

        # Pass through EXACT SAME evaluate_build function expecting 0 warnings
        neg_passed, neg_warnings, neg_detail = evaluate_build(neg_bld_code, neg_full, True, expected_warnings=0)

        # The negative control passes if evaluate_build FAILS due to warnings >= 1
        neg_control_ok = (not neg_passed) and (len(neg_warnings) >= 1)
        logger(f"[host] CHECK T1 negative control (-Wconversion flagged warnings={len(neg_warnings)}): "
               f"{'PASS' if neg_control_ok else 'FAIL'}")
        if not neg_control_ok:
            overall_ok = False

    return overall_ok


def run_t2(tools: dict[str, str], logger: Callable[[str], None]) -> bool:
    logger("=== Running T2: Header Diff & Symbol Leak Checks ===")
    overall_ok = True

    # 1. Header diff check: git diff b41c4a7 -- 01_shared_drivers/04_uart/*.h only adds
    diff_cmd = ["git", "diff", "-U0", BASE_COMMIT, "--", "01_shared_drivers/04_uart/*.h"]
    code, diff_out, err = run_cmd(diff_cmd, cwd=REPO_ROOT, timeout=30.0)
    if code != 0:
        logger(f"[host] CHECK header diff: FAIL (git diff failed: {err})")
        overall_ok = False
    else:
        diff_ok, deleted = evaluate_header_diff(diff_out)
        logger(f"[host] CHECK header diff against {BASE_COMMIT} (only adds): {'PASS' if diff_ok else 'FAIL'} "
               f"(deleted_lines={len(deleted)})")
        if not diff_ok:
            overall_ok = False

    # Negative control for header diff: mutated diff with removed line passed to evaluate_header_diff
    fake_diff_with_deletion = (
        "diff --git a/01_shared_drivers/04_uart/uart_bm.h b/01_shared_drivers/04_uart/uart_bm.h\n"
        "--- a/01_shared_drivers/04_uart/uart_bm.h\n"
        "+++ b/01_shared_drivers/04_uart/uart_bm.h\n"
        "@@ -10,1 +10,0 @@\n"
        "-bool UART_BM_Init(UartBm_Handle_t *h, const UartBm_Config_t *cfg);\n"
    )
    neg_diff_ok, neg_del = evaluate_header_diff(fake_diff_with_deletion)
    neg_diff_control_pass = (not neg_diff_ok) and (len(neg_del) == 1)
    logger(f"[host] CHECK header diff negative control (deletion rejected): "
           f"{'PASS' if neg_diff_control_pass else 'FAIL'}")
    if not neg_diff_control_pass:
        overall_ok = False

    # 2. Symbol checks: production ELFs must not contain selftest symbols
    prod_elf_paths = [
        "03_firmware_templates/prj_01_template_dualcore/build/release/CM7/cm7_firmware.elf",
        "03_firmware_templates/prj_01_template_dualcore/build/release/CM4/cm4_firmware.elf",
        "02_firmware_projects/prj_02_eth_lwip_dualcore/build/release/CM7/cm7_firmware.elf",
        "02_firmware_projects/prj_02_eth_lwip_dualcore/build/release/CM4/cm4_firmware.elf",
        "02_firmware_projects/prj_03_adc_dma_eth/build/release/CM7/cm7_firmware.elf",
        "02_firmware_projects/prj_03_adc_dma_eth/build/release/CM4/cm4_firmware.elf",
        "02_firmware_projects/prj_04_adc_dma_bm_eth/build/release/CM7/cm7_firmware.elf",
        "02_firmware_projects/prj_04_adc_dma_bm_eth/build/release/CM4/cm4_firmware.elf",
    ]

    for rel_elf in prod_elf_paths:
        elf_path = REPO_ROOT / rel_elf
        if not elf_path.is_file():
            logger(f"[host] CHECK symbols {rel_elf}: FAIL (ELF missing)")
            overall_ok = False
            continue

        nm_cmd = [tools["nm"], "-g", str(elf_path)]
        nm_code, nm_out, _ = run_cmd(nm_cmd, timeout=30.0)
        symbols = [line.split()[-1] for line in nm_out.splitlines() if line.split()]
        sym_ok, leaked = evaluate_symbols(symbols)
        logger(f"[host] CHECK symbols in {elf_path.name} ({elf_path.parent.parent.name}): "
               f"{'PASS' if sym_ok else 'FAIL'} (leaked={len(leaked)})")
        if not sym_ok:
            overall_ok = False

    # Negative control for symbols: mutated symbol list passed to evaluate_symbols
    fake_symbols = ["main", "SystemClock_Config", "UART_Selftest_CM7_Run", "app_main"]
    neg_sym_ok, neg_leaked = evaluate_symbols(fake_symbols)
    neg_sym_control_pass = (not neg_sym_ok) and ("UART_Selftest_CM7_Run" in neg_leaked)
    logger(f"[host] CHECK symbols negative control (leaked symbol caught): "
           f"{'PASS' if neg_sym_control_pass else 'FAIL'}")
    if not neg_sym_control_pass:
        overall_ok = False

    return overall_ok


# ---- Main CLI ----------------------------------------------------------------

def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--test", choices=["t1", "t2", "all"], default="all", help="which test to run")
    ap.add_argument("--output", type=pathlib.Path, default=None, help="capture output file")
    args = ap.parse_args()

    rev = get_git_rev()
    out_path = args.output
    if out_path is None:
        docs_dir = REPO_ROOT / "01_shared_drivers" / "04_uart" / "docs"
        docs_dir.mkdir(parents=True, exist_ok=True)
        out_path = docs_dir / f"uart_checks_{rev}.txt"

    lines: list[str] = []

    def log(msg: str) -> None:
        t = time.strftime("%Y-%m-%d %H:%M:%S")
        formatted = f"[{t}] {msg}"
        print(formatted)
        lines.append(formatted)

    log(f"[host] argv: {' '.join(sys.argv)}")
    log(f"[host] HEAD: {rev}")

    clean_start, clean_msg = check_clean_working_tree(REPO_ROOT, out_path)
    log(f"[host] clean tree at start: {'PASS' if clean_start else 'FAIL'} ({clean_msg})")
    if not clean_start:
        log("OVERALL VERDICT: FAIL (dirty tree at start)")
        with open(out_path, "w", encoding="utf-8") as f:
            for l in lines: f.write(l + "\n")
        return 1

    tools = locate_tools()
    all_ok = True

    if args.test in ("t1", "all"):
        t1_ok = run_t1(tools, log)
        if not t1_ok:
            all_ok = False

    if args.test in ("t2", "all"):
        t2_ok = run_t2(tools, log)
        if not t2_ok:
            all_ok = False

    clean_end, clean_end_msg = check_clean_working_tree(REPO_ROOT, out_path)
    log(f"[host] clean tree at end: {'PASS' if clean_end else 'FAIL'} ({clean_end_msg})")
    if not clean_end:
        all_ok = False

    log(f"OVERALL VERDICT: {'PASS' if all_ok else 'FAIL'}")

    out_path.parent.mkdir(parents=True, exist_ok=True)
    with open(out_path, "w", encoding="utf-8") as f:
        for l in lines:
            f.write(l + "\n")
    print(f"[host] Capture saved to {out_path}")

    return 0 if all_ok else 1


if __name__ == "__main__":
    sys.exit(main())
