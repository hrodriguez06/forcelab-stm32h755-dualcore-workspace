#!/usr/bin/env python3
"""Verification harness for STM32H7 UART Driver Phase F1 (Off-board checks).

Covers tests T1 and T2:
  - T1: Build matrix (8 configurations + 3 selftest presets) configured with --fresh
        and built with cmake --build --clean-first, counting compiler warnings from that output.
        Verifies mtime >= start time for both core ELFs and object counts.
        Negative control: build in temporary directory with cmake --preset debug -B <tmp>
        --fresh -DCMAKE_C_FLAGS=-Wconversion, verifying configure code 0 and warnings >= 1
        with [-Wconversion] via the exact same evaluate_build function.
  - T2: Header diff and symbol leak checks:
        1. git diff b41c4a7 -- 01_shared_drivers/04_uart/*.h only adds.
           Negative control: real git diff --no-index against a mutated header in temp dir
           passed through the exact same evaluate_header_diff function returns FAIL.
        2. Production ELFs (both release and debug, 16 ELFs) contain no selftest symbols.
           nm run without -g, exit code checked == 0, symbols count >= 1.
           Full relative path printed for each ELF.
           Positive control: selftest-uart ELFs from the same execution passed through the
           exact same evaluate_symbols function must catch >= 1 selftest symbol leak.

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


def snapshot_working_tree(repo_root: pathlib.Path) -> dict[str, str]:
    """Snapshots all porcelain entries (tracked mods, untracked, ignored)."""
    code, status_out, _ = run_cmd(["git", "status", "--porcelain=v1", "-u", "--ignored"], cwd=repo_root, timeout=30.0)
    if code != 0:
        return {}
    entries = {}
    for line in status_out.splitlines():
        if not line.strip():
            continue
        prefix = line[:2]
        rel_path = line[3:].strip().strip('"')
        entries[rel_path] = prefix
    return entries


def check_clean_working_tree(
    repo_root: pathlib.Path,
    output_file: Optional[pathlib.Path],
    initial_snapshot: Optional[dict[str, str]] = None
) -> tuple[bool, str]:
    """Verifies git status per Plan Revision 2:
    Only files inside build/ directories are permitted to be created or ignored.
    Outside build/:
      - Tracked modifications (M, D, A, etc.) and untracked files (??) are FAIL (except output_file).
      - If initial_snapshot is provided (at test completion), any newly created file
        outside build/ (even if ignored !!) that was not in initial_snapshot is FAIL.
    """
    current = snapshot_working_tree(repo_root)
    out_resolved = output_file.resolve() if output_file is not None else None

    dirty = []
    for rel_path, prefix in current.items():
        resolved_path = (repo_root / rel_path).resolve()
        if out_resolved is not None and resolved_path == out_resolved:
            continue
        if "build" in resolved_path.parts:
            continue

        if prefix != "!!":
            dirty.append(f"{prefix} {rel_path}")
        elif initial_snapshot is not None:
            if rel_path not in initial_snapshot:
                dirty.append(f"NEW(!!) {rel_path}")

    if dirty:
        return False, f"Dirty tree outside build/ ({len(dirty)} items): {dirty}"
    return True, "Tree is clean outside build/"


# ---- Evaluation Functions (used by both real tests and negative controls) ----

def parse_compiler_warnings(build_output: str) -> list[str]:
    warnings = []
    for line in build_output.splitlines():
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
    """Pure evaluation function checking for leaked selftest symbols.

    Pattern covers ust_, k_ust_, run_ut, UART_Selftest (T-M3).
    """
    leaked = []
    pattern = re.compile(r"(\bust_|\bk_ust_|\brun_ut|\bUART_Selftest)", re.IGNORECASE)
    for sym in symbols:
        if pattern.search(sym):
            leaked.append(sym)
    return (len(leaked) == 0), leaked


# ---- Test Suites -------------------------------------------------------------

def run_t1(tools: dict[str, str], logger: Callable[[str], None]) -> bool:
    logger("=== Running T1: Build Matrix & Selftests ===")
    overall_ok = True
    builds_to_run = MATRIX_CONFIGS + SELFTEST_PRESETS
    t1_start_time = time.time()

    for proj_rel, preset in builds_to_run:
        pdir = REPO_ROOT / proj_rel
        pname = pdir.name

        # 1. Configure with --preset <preset> --fresh (T-M2)
        cfg_cmd = [tools["cmake"], "--preset", preset, "--fresh"]
        cfg_code, cfg_out, cfg_err = run_cmd(cfg_cmd, cwd=pdir, timeout=120.0)
        if cfg_code != 0:
            logger(f"[host] CHECK build {pname} [{preset}]: FAIL (configure failed: {cfg_err or cfg_out})")
            overall_ok = False
            continue

        # 2. Build with --preset <preset> --clean-first
        bld_cmd = [tools["cmake"], "--build", "--preset", preset, "--clean-first"]
        bld_code, bld_out, bld_err = run_cmd(bld_cmd, cwd=pdir, timeout=300.0)
        full_out = bld_out + "\n" + bld_err

        # Count actual compiled .o/.obj files in build directory (T-n)
        build_dir = pdir / "build" / preset
        obj_files = list(build_dir.rglob("*.obj")) + list(build_dir.rglob("*.o"))
        obj_count = len(obj_files)

        # Check freshness of both ELFs (mtime >= t1_start_time) (T-M2)
        cm7_elf = build_dir / "CM7" / "cm7_firmware.elf"
        cm4_elf = build_dir / "CM4" / "cm4_firmware.elf"
        single_elf = build_dir / f"{pname}.elf"

        elfs_valid = False
        if cm7_elf.is_file() and cm4_elf.is_file():
            cm7_mtime = cm7_elf.stat().st_mtime
            cm4_mtime = cm4_elf.stat().st_mtime
            elfs_valid = (cm7_mtime >= t1_start_time - 1.0) and (cm4_mtime >= t1_start_time - 1.0) and (obj_count > 0)
        elif single_elf.is_file():
            elfs_valid = (single_elf.stat().st_mtime >= t1_start_time - 1.0) and (obj_count > 0)

        passed, warnings, detail = evaluate_build(bld_code, full_out, elfs_valid)
        logger(f"[host] CHECK build {pname} [{preset}]: {'PASS' if passed else 'FAIL'} "
               f"({detail}, objects={obj_count})")
        if not passed:
            overall_ok = False

    # Negative control for T1: cmake --preset debug -B <tmp> --fresh -DCMAKE_C_FLAGS=-Wconversion (T-m)
    logger("[host] Running T1 Negative Control: build with -Wconversion outside repo...")
    with tempfile.TemporaryDirectory() as tmp_dir:
        tmp_build_path = pathlib.Path(tmp_dir) / "build_neg"
        src_proj = REPO_ROOT / "03_firmware_templates" / "prj_01_template_dualcore"

        neg_cfg_cmd = [
            tools["cmake"], "--preset", "debug",
            "-B", str(tmp_build_path),
            "--fresh",
            "-DCMAKE_C_FLAGS=-Wconversion"
        ]
        neg_cfg_code, neg_cfg_out, neg_cfg_err = run_cmd(neg_cfg_cmd, cwd=src_proj, timeout=120.0)
        if neg_cfg_code != 0:
            logger(f"[host] CHECK T1 negative control configure: FAIL (code {neg_cfg_code}: {neg_cfg_err or neg_cfg_out})")
            overall_ok = False
        else:
            neg_bld_cmd = [tools["cmake"], "--build", str(tmp_build_path), "--clean-first"]
            neg_bld_code, neg_bld_out, neg_bld_err = run_cmd(neg_bld_cmd, cwd=src_proj, timeout=300.0)
            neg_full = neg_bld_out + "\n" + neg_bld_err

            cm7_neg_elf = tmp_build_path / "CM7" / "cm7_firmware.elf"
            cm4_neg_elf = tmp_build_path / "CM4" / "cm4_firmware.elf"
            neg_elf_found = cm7_neg_elf.is_file() and cm4_neg_elf.is_file()

            # Pass through EXACT SAME evaluate_build function expecting 0 warnings
            neg_passed, neg_warnings, neg_detail = evaluate_build(
                neg_bld_code, neg_full, neg_elf_found, expected_warnings=0
            )

            # [-Wconversion] / [-Wsign-conversion] enabled by -Wconversion flag (T-m)
            has_wconversion = any(re.search(r"\[-W(?:sign-)?conversion\]", w) for w in neg_warnings)
            neg_control_ok = (not neg_passed) and (len(neg_warnings) >= 1) and has_wconversion
            logger(f"[host] CHECK T1 negative control (-Wconversion warnings={len(neg_warnings)}, has_flag={has_wconversion}): "
                   f"{'PASS' if neg_control_ok else 'FAIL'}")
            if not neg_control_ok:
                overall_ok = False

    return overall_ok


def run_t2(tools: dict[str, str], logger: Callable[[str], None]) -> bool:
    logger("=== Running T2: Header Diff & Symbol Leak Checks ===")
    overall_ok = True

    # 1. Header diff check: git diff b41c4a7 -- 01_shared_drivers/04_uart/*.h only adds
    headers = list((REPO_ROOT / "01_shared_drivers" / "04_uart").glob("*.h"))
    if not headers:
        logger("[host] CHECK header pathspec: FAIL (0 header files matched)")
        overall_ok = False
        return False
    header_rel_paths = [str(h.relative_to(REPO_ROOT)).replace("\\", "/") for h in sorted(headers)]
    logger(f"[host] Header pathspec matched {len(headers)} files: {[h.name for h in sorted(headers)]}")

    diff_cmd = ["git", "diff", "-U0", BASE_COMMIT, "--"] + header_rel_paths
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

    # Negative control for header diff: real git diff --no-index on a mutated copy (T-B1)
    logger("[host] Running T2 Negative Control: mutated header diff with git diff --no-index...")
    with tempfile.TemporaryDirectory() as tmp_dir:
        orig_header = REPO_ROOT / "01_shared_drivers" / "04_uart" / "uart_bm.h"
        mut_header = pathlib.Path(tmp_dir) / "uart_bm.h"

        content = orig_header.read_text(encoding="utf-8")
        # Mutate by removing a function declaration line
        mutated_content = re.sub(r"bool UART_BM_Init[^\n]+\n", "", content)
        mut_header.write_text(mutated_content, encoding="utf-8")

        diff_no_idx_cmd = ["git", "diff", "--no-index", "-U0", str(orig_header), str(mut_header)]
        _, neg_diff_out, _ = run_cmd(diff_no_idx_cmd, timeout=30.0)

        neg_diff_ok, neg_del = evaluate_header_diff(neg_diff_out)
        neg_diff_control_pass = (not neg_diff_ok) and (len(neg_del) >= 1)
        logger(f"[host] CHECK header diff negative control (git diff --no-index deletion detected={len(neg_del)}): "
               f"{'PASS' if neg_diff_control_pass else 'FAIL'}")
        if not neg_diff_control_pass:
            overall_ok = False

    # 2. Symbol checks: production ELFs (both release and debug, 16 ELFs total) (T-M3, T-M4)
    prod_configs = [
        ("03_firmware_templates/prj_01_template_dualcore", "release", "CM7/cm7_firmware.elf"),
        ("03_firmware_templates/prj_01_template_dualcore", "release", "CM4/cm4_firmware.elf"),
        ("03_firmware_templates/prj_01_template_dualcore", "debug",   "CM7/cm7_firmware.elf"),
        ("03_firmware_templates/prj_01_template_dualcore", "debug",   "CM4/cm4_firmware.elf"),
        ("02_firmware_projects/prj_02_eth_lwip_dualcore", "release", "CM7/cm7_firmware.elf"),
        ("02_firmware_projects/prj_02_eth_lwip_dualcore", "release", "CM4/cm4_firmware.elf"),
        ("02_firmware_projects/prj_02_eth_lwip_dualcore", "debug",   "CM7/cm7_firmware.elf"),
        ("02_firmware_projects/prj_02_eth_lwip_dualcore", "debug",   "CM4/cm4_firmware.elf"),
        ("02_firmware_projects/prj_03_adc_dma_eth", "release", "CM7/cm7_firmware.elf"),
        ("02_firmware_projects/prj_03_adc_dma_eth", "release", "CM4/cm4_firmware.elf"),
        ("02_firmware_projects/prj_03_adc_dma_eth", "debug",   "CM7/cm7_firmware.elf"),
        ("02_firmware_projects/prj_03_adc_dma_eth", "debug",   "CM4/cm4_firmware.elf"),
        ("02_firmware_projects/prj_04_adc_dma_bm_eth", "release", "CM7/cm7_firmware.elf"),
        ("02_firmware_projects/prj_04_adc_dma_bm_eth", "release", "CM4/cm4_firmware.elf"),
        ("02_firmware_projects/prj_04_adc_dma_bm_eth", "debug",   "CM7/cm7_firmware.elf"),
        ("02_firmware_projects/prj_04_adc_dma_bm_eth", "debug",   "CM4/cm4_firmware.elf"),
    ]

    for proj, cfg, elf_sub in prod_configs:
        rel_elf = f"{proj}/build/{cfg}/{elf_sub}"
        elf_path = REPO_ROOT / rel_elf

        if not elf_path.is_file():
            logger(f"[host] CHECK symbols {rel_elf}: FAIL (ELF file missing)")
            overall_ok = False
            continue

        # Check mtime >= RUN_START_TIME (T-m)
        if elf_path.stat().st_mtime < RUN_START_TIME:
            logger(f"[host] CHECK symbols {rel_elf}: FAIL (ELF is stale, not from this run)")
            overall_ok = False
            continue

        # Run nm WITHOUT -g (T-M3)
        nm_cmd = [tools["nm"], str(elf_path)]
        nm_code, nm_out, nm_err = run_cmd(nm_cmd, timeout=30.0)
        if nm_code != 0:
            logger(f"[host] CHECK symbols {rel_elf}: FAIL (nm exited with code {nm_code}: {nm_err})")
            overall_ok = False
            continue

        symbols = [line.split()[-1] for line in nm_out.splitlines() if line.split()]
        if len(symbols) == 0:
            logger(f"[host] CHECK symbols {rel_elf}: FAIL (nm returned 0 symbols)")
            overall_ok = False
            continue

        sym_ok, leaked = evaluate_symbols(symbols)
        logger(f"[host] CHECK symbols {rel_elf}: {'PASS' if sym_ok else 'FAIL'} "
               f"(symbols={len(symbols)}, leaked={len(leaked)})")
        if not sym_ok:
            logger(f"       Leaked symbols: {leaked}")
            overall_ok = False

    # Positive control for symbols: selftest-uart ELFs from this execution (T-B1, T-M3)
    logger("[host] Running T2 Positive Control: verifying selftest symbols in selftest-uart ELFs...")
    selftest_elfs = [
        "03_firmware_templates/prj_01_template_dualcore/build/selftest-uart/CM7/cm7_firmware.elf",
        "03_firmware_templates/prj_01_template_dualcore/build/selftest-uart/CM4/cm4_firmware.elf",
    ]
    positive_control_ok = True
    for st_elf_rel in selftest_elfs:
        st_elf_path = REPO_ROOT / st_elf_rel
        if not st_elf_path.is_file():
            logger(f"[host] CHECK positive control {st_elf_rel}: FAIL (selftest ELF missing)")
            positive_control_ok = False
            overall_ok = False
            continue

        if st_elf_path.stat().st_mtime < RUN_START_TIME:
            logger(f"[host] CHECK positive control {st_elf_rel}: FAIL (selftest ELF is stale, not from this run)")
            positive_control_ok = False
            overall_ok = False
            continue

        nm_cmd = [tools["nm"], str(st_elf_path)]
        nm_code, nm_out, nm_err = run_cmd(nm_cmd, timeout=30.0)
        if nm_code != 0:
            logger(f"[host] CHECK positive control {st_elf_rel}: FAIL (nm failed code {nm_code})")
            positive_control_ok = False
            overall_ok = False
            continue

        symbols = [line.split()[-1] for line in nm_out.splitlines() if line.split()]
        sym_ok, detected_selftest = evaluate_symbols(symbols)
        # Positive control PASSES if evaluate_symbols catches >= 1 selftest symbol
        catches_symbols = (not sym_ok) and (len(detected_selftest) >= 1)
        logger(f"[host] CHECK positive control {st_elf_rel} (selftest symbols detected={len(detected_selftest)}): "
               f"{'PASS' if catches_symbols else 'FAIL'}")
        if not catches_symbols:
            positive_control_ok = False
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

    # Initial snapshot of tree status (including --ignored, excluding build/) (T-M1)
    baseline_snapshot = snapshot_working_tree(REPO_ROOT)
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

    clean_end, clean_end_msg = check_clean_working_tree(REPO_ROOT, out_path, initial_snapshot=baseline_snapshot)
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
