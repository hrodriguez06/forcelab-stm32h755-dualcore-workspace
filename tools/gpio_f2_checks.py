#!/usr/bin/env python3
"""Verification harness for STM32H7 GPIO Driver F2 phase (T1 build, T18 regression without board).

Usage:
    py tools/gpio_f2_checks.py [--test {t1,t18,all}] [--output <file>] [--control-dir <path>] [--allow-dirty]

Standard library only.
"""

from __future__ import annotations

import argparse
import hashlib
import os
import re
import shlex
import shutil
import subprocess
import sys
import tempfile
import time
from pathlib import Path
from typing import Callable, Optional

REPO_ROOT = Path(__file__).resolve().parent.parent
CONTROL_COMMIT_EXPECTED = "3df160a"


def find_tools() -> dict[str, str]:
    # Add ST tools/bin to PATH so cmake can find ninja and gcc
    for p in Path(r"C:\ST").glob("**/tools/bin"):
        os.environ["PATH"] = str(p) + os.pathsep + os.environ.get("PATH", "")

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
    tools["size"] = str(bin_dir / "arm-none-eabi-size.exe")

    for k in ("nm", "size"):
        if not Path(tools[k]).is_file():
            which_t = shutil.which(f"arm-none-eabi-{k}")
            tools[k] = which_t if which_t else f"arm-none-eabi-{k}"

    cmake_path = shutil.which("cmake")
    if not cmake_path:
        for p in Path(r"C:\ST").glob("**/cmake.exe"):
            cmake_path = str(p)
            break
    tools["cmake"] = cmake_path or "cmake"
    return tools


class Logger:
    def __init__(self, out_path: Path | None) -> None:
        self.out_path = out_path
        self.lines: list[str] = []
        self.t0 = time.time()
        if self.out_path:
            self.out_path.parent.mkdir(parents=True, exist_ok=True)
            self.out_path.write_text("", encoding="utf-8")

    def log(self, msg: str) -> None:
        elapsed = time.time() - self.t0
        line = f"[+ {elapsed:7.3f}s] {msg}"
        print(line, flush=True)
        self.lines.append(line)
        if self.out_path:
            with open(self.out_path, "a", encoding="utf-8") as f:
                f.write(line + "\n")


def run_cmd(args: list[str], cwd: Path | None = None) -> tuple[int, str, str]:
    res = subprocess.run(args, cwd=str(cwd) if cwd else None, capture_output=True, text=True)
    return res.returncode, res.stdout, res.stderr


# ---------------------------------------------------------------------------
# Check 1 & 2: Build Matrix & Selftest-gpio builds
# ---------------------------------------------------------------------------

def check_matrix_build(logger: Logger) -> bool:
    logger.log("[check] Checking 8/8 matrix build via build_all.ps1 -Clean...")
    cmd = ["powershell", "-NoProfile", "-ExecutionPolicy", "Bypass", "-File",
           str(REPO_ROOT / "tools" / "build_all.ps1"), "-Clean"]
    t_start = time.time()
    code, stdout, stderr = run_cmd(cmd, cwd=REPO_ROOT)
    duration = time.time() - t_start

    passed = (code == 0) and ("All 8 builds PASSED successfully" in stdout)
    logger.log(f"[build_all] Finished in {duration:.1f}s, exit code {code}")
    for line in stdout.splitlines():
        if "==>" in line or "PASSED" in line or "FAILED" in line:
            logger.log(f"[build_all] {line.strip()}")
    logger.log(f"[host] CHECK build_all 8/8 (-Clean): {'PASS' if passed else 'FAIL'}")
    return passed


def check_selftest_builds(logger: Logger, tools: dict[str, str]) -> bool:
    logger.log("[check] Checking selftest-gpio compilation in prj_01 and prj_04 with --clean-first (0 warnings)...")
    ok = True
    for proj_rel in ("03_firmware_templates/prj_01_template_dualcore", "02_firmware_projects/prj_04_adc_dma_bm_eth"):
        proj_dir = REPO_ROOT / proj_rel
        code, out, err = run_cmd([tools["cmake"], "--build", "--preset", "selftest-gpio", "--clean-first"], cwd=proj_dir)
        full_out = out + "\n" + err
        warnings = [line for line in full_out.splitlines() if "warning:" in line.lower() and ("cm4_firmware" in line or "cm7_firmware" in line or "gpio" in line)]
        step_matches = re.findall(r"\[(\d+)/(\d+)\]", full_out)
        max_steps = int(step_matches[-1][1]) if step_matches else 0

        passed = (code == 0) and (len(warnings) == 0) and (max_steps > 0)
        proj_name = proj_dir.name
        logger.log(f"[host] CHECK {proj_name} selftest-gpio build: {'PASS' if passed else 'FAIL'} "
                   f"(code={code}, steps_built={max_steps}, warnings={len(warnings)})")
        if not passed:
            ok = False
    return ok


# ---------------------------------------------------------------------------
# Check 3: Header diff only adds
# ---------------------------------------------------------------------------

def verify_header_diff(header_path: Path, ref_commit: str,
                       override_content: str | None = None) -> tuple[bool, str, list[str]]:
    """Pure function: checks that git diff against ref_commit contains only additions.
    If override_content is given, checks diff against temporary file with that content."""
    if override_content is not None:
        with tempfile.NamedTemporaryFile("w", suffix=header_path.suffix, delete=False, encoding="utf-8") as f:
            f.write(override_content)
            temp_path = Path(f.name)
        try:
            code, diff_out, _ = run_cmd(["git", "diff", "--no-index", str(header_path), str(temp_path)])
        finally:
            if temp_path.is_file():
                temp_path.unlink()
    else:
        code, diff_out, _ = run_cmd(["git", "diff", ref_commit, "--", str(header_path)], cwd=REPO_ROOT)
        if code != 0:
            return False, f"git diff failed with exit code {code}", []

    illegal_removals = []
    for line in diff_out.splitlines():
        if line.startswith("---") or line.startswith("+++") or line.startswith("@@"):
            continue
        if line.startswith("-"):
            content = line[1:].strip()
            # Flag removal of code lines
            if content and not content.startswith("/*") and not content.startswith("*") and not content.startswith("//"):
                illegal_removals.append(line)

    if illegal_removals:
        return False, f"Removed or modified lines detected ({len(illegal_removals)})", illegal_removals
    return True, "Diff contains only additions", []


def check_headers(logger: Logger) -> bool:
    logger.log("[check] Checking all header files in 01_shared_drivers/00_gpio/ against control 3df160a...")
    driver_dir = REPO_ROOT / "01_shared_drivers" / "00_gpio"
    headers = sorted(driver_dir.glob("*.h"))
    all_ok = True

    for h in headers:
        passed, msg, removals = verify_header_diff(h, CONTROL_COMMIT_EXPECTED)
        logger.log(f"[host] CHECK header freeze {h.name} (only adds): {'PASS' if passed else 'FAIL'} ({msg})")
        if not passed:
            all_ok = False

    # Negative control: call verify_header_diff with a mutated header where a function declaration is removed
    bm_header = driver_dir / "gpio_bm.h"
    original_text = bm_header.read_text(encoding="utf-8")
    mutated_text = original_text.replace("bool GPIO_BM_EnablePortClock(GPIO_TypeDef *port);", "")
    neg_passed, neg_msg, neg_removals = verify_header_diff(bm_header, CONTROL_COMMIT_EXPECTED, override_content=mutated_text)
    neg_detected = (not neg_passed) and any("GPIO_BM_EnablePortClock" in r for r in neg_removals)
    logger.log(f"[host] CHECK header negative control (signature deletion flagged by verify_header_diff): "
               f"{'PASS' if neg_detected else 'FAIL'} ({neg_msg})")

    return all_ok and neg_detected


# ---------------------------------------------------------------------------
# Check 4: No EXTI handlers in 00_gpio/ or in production ELFs
# ---------------------------------------------------------------------------

def strip_c_comments(code: str) -> str:
    code = re.sub(r"/\*.*?\*/", "", code, flags=re.DOTALL)
    code = re.sub(r"//.*", "", code)
    return code


def verify_no_driver_handlers(driver_dir: Path, extra_content: str | None = None) -> tuple[bool, list[str]]:
    """Pure function: verifies no EXTI*_IRQHandler or HAL_GPIO_EXTI_Callback in driver C/H files."""
    patterns = [r"\bEXTI\w*_IRQHandler\b", r"\bHAL_GPIO_EXTI_Callback\b"]
    matches = []
    for file in sorted(driver_dir.glob("*.[ch]")):
        content = strip_c_comments(file.read_text(encoding="utf-8"))
        for pat in patterns:
            for m in re.finditer(pat, content):
                matches.append(f"{file.name}:{m.group(0)}")

    if extra_content is not None:
        clean_extra = strip_c_comments(extra_content)
        for pat in patterns:
            for m in re.finditer(pat, clean_extra):
                matches.append(f"extra:{m.group(0)}")

    return (len(matches) == 0), matches


def verify_no_production_exti_handlers(elf_path: Path, nm_tool: str,
                                       extra_symbols: list[str] | None = None) -> tuple[bool, list[str]]:
    """Pure function: verifies that no strong EXTI*_IRQHandler symbols ('T'/'t') exist in the given ELF."""
    if not elf_path.is_file():
        return False, [f"File not found: {elf_path}"]
    code, out, _ = run_cmd([nm_tool, "--defined-only", str(elf_path)])
    if code != 0:
        return False, [f"nm failed with code {code}"]

    handlers = []
    for line in out.splitlines():
        parts = line.split()
        if len(parts) >= 3:
            stype = parts[-2]
            name = parts[-1]
            if stype in ("T", "t") and re.match(r"^EXTI\w*_IRQHandler$", name):
                handlers.append(name)

    if extra_symbols:
        for s in extra_symbols:
            if re.match(r"^EXTI\w*_IRQHandler$", s):
                handlers.append(s)

    return (len(handlers) == 0), handlers


def check_no_handlers(logger: Logger, tools: dict[str, str]) -> bool:
    logger.log("[check] Checking for EXTI handlers in 01_shared_drivers/00_gpio/...")
    driver_dir = REPO_ROOT / "01_shared_drivers" / "00_gpio"
    driver_ok, driver_matches = verify_no_driver_handlers(driver_dir)
    logger.log(f"[host] CHECK no EXTI handlers in 00_gpio: {'PASS' if driver_ok else 'FAIL'}")

    # Negative control for driver handlers
    neg_driver_ok, neg_driver_matches = verify_no_driver_handlers(driver_dir, extra_content="void EXTI0_IRQHandler(void) {}")
    neg_driver_detected = (not neg_driver_ok) and any("EXTI0_IRQHandler" in m for m in neg_driver_matches)
    logger.log(f"[host] CHECK driver handler negative control: {'PASS' if neg_driver_detected else 'FAIL'}")

    # Verify 16 production ELFs have 0 EXTI handlers
    logger.log("[check] Checking that all 16 production ELFs have 0 EXTI handlers...")
    projects = [
        ("prj_01", "03_firmware_templates/prj_01_template_dualcore"),
        ("prj_02", "02_firmware_projects/prj_02_eth_lwip_dualcore"),
        ("prj_03", "02_firmware_projects/prj_03_adc_dma_eth"),
        ("prj_04", "02_firmware_projects/prj_04_adc_dma_bm_eth"),
    ]
    prod_ok = True
    for proj_id, proj_rel in projects:
        for preset in ("debug", "release"):
            for core in ("CM7", "CM4"):
                elf_path = REPO_ROOT / proj_rel / "build" / preset / core / f"{core.lower()}_firmware.elf"
                no_h, h_list = verify_no_production_exti_handlers(elf_path, tools["nm"])
                if not no_h:
                    prod_ok = False
                    logger.log(f"[host] CHECK {proj_id} {preset} {core} no EXTI handlers: FAIL ({h_list})")

    logger.log(f"[host] CHECK 16 production ELFs zero EXTI handlers: {'PASS' if prod_ok else 'FAIL'}")

    # Positive control: selftest-gpio CM7 elf DOES have EXTI handlers
    st_elf = REPO_ROOT / "03_firmware_templates/prj_01_template_dualcore/build/selftest-gpio/CM7/cm7_firmware.elf"
    st_no_h, st_handlers = verify_no_production_exti_handlers(st_elf, tools["nm"])
    pos_detected = (not st_no_h) and (len(st_handlers) > 0)
    logger.log(f"[host] CHECK selftest-gpio positive control ({len(st_handlers)} EXTI handlers present): "
               f"{'PASS' if pos_detected else 'FAIL'}")

    # Negative control: verify_no_production_exti_handlers detects injected handler
    dummy_elf = REPO_ROOT / "03_firmware_templates/prj_01_template_dualcore/build/debug/CM7/cm7_firmware.elf"
    neg_elf_ok, neg_elf_h = verify_no_production_exti_handlers(dummy_elf, tools["nm"], extra_symbols=["EXTI3_IRQHandler"])
    neg_prod_detected = (not neg_elf_ok) and ("EXTI3_IRQHandler" in neg_elf_h)
    logger.log(f"[host] CHECK production ELF negative control (injected EXTI3_IRQHandler flagged): "
               f"{'PASS' if neg_prod_detected else 'FAIL'}")

    return driver_ok and neg_driver_detected and prod_ok and pos_detected and neg_prod_detected


# ---------------------------------------------------------------------------
# Check 5: .data = 0 and .bss = 8 B in gpio_bm.c.obj via .map file
# ---------------------------------------------------------------------------

def parse_map_sizes(map_path: Path, obj_name: str = "gpio_bm.c.obj") -> tuple[int, int]:
    """Parses .map file for total .data and .bss bytes contributed by obj_name."""
    if not map_path.is_file():
        raise FileNotFoundError(f"Map file not found: {map_path}")
    text = map_path.read_text(encoding="utf-8", errors="replace")

    data_bytes = 0
    bss_bytes = 0

    lines = text.splitlines()
    for i, line in enumerate(lines):
        if obj_name in line and i > 0:
            prev = lines[i - 1].strip()
            curr = line.strip()
            m = re.search(r"0x[0-9a-fA-F]+\s+0x([0-9a-fA-F]+)", line)
            if m:
                sz = int(m.group(1), 16)
                if prev.startswith(".bss.") or curr.startswith(".bss."):
                    bss_bytes += sz
                elif prev.startswith(".data.") or curr.startswith(".data."):
                    data_bytes += sz

    return data_bytes, bss_bytes


def verify_driver_sizes(map_path: Path, override_data: int | None = None,
                        override_bss: int | None = None) -> tuple[bool, int, int]:
    """Pure function: verifies .data == 0 and .bss == 8."""
    if override_data is not None and override_bss is not None:
        data_sz, bss_sz = override_data, override_bss
    else:
        data_sz, bss_sz = parse_map_sizes(map_path, "gpio_bm.c.obj")
    passed = (data_sz == 0) and (bss_sz == 8)
    return passed, data_sz, bss_sz


def check_driver_sizes(logger: Logger) -> bool:
    logger.log("[check] Checking driver state .data=0 and .bss=8 via .map files of selftest-gpio...")
    map_dir = REPO_ROOT / "03_firmware_templates/prj_01_template_dualcore/build/selftest-gpio"
    cm7_map = map_dir / "cm7_firmware.map"
    cm4_map = map_dir / "cm4_firmware.map"

    all_ok = True
    for core, mp in (("CM7", cm7_map), ("CM4", cm4_map)):
        passed, data_sz, bss_sz = verify_driver_sizes(mp)
        logger.log(f"[host] CHECK driver state {core} from .map: data={data_sz} B (exp 0), bss={bss_sz} B (exp 8) -> "
                   f"{'PASS' if passed else 'FAIL'}")
        if not passed:
            all_ok = False

    # Negative control: call verify_driver_sizes with simulated non-compliant values
    neg_passed, neg_d, neg_b = verify_driver_sizes(cm7_map, override_data=4, override_bss=12)
    neg_detected = (not neg_passed)
    logger.log(f"[host] CHECK driver size negative control (data={neg_d}, bss={neg_b} flagged by verify_driver_sizes): "
               f"{'PASS' if neg_detected else 'FAIL'}")

    return all_ok and neg_detected


# ---------------------------------------------------------------------------
# Check 6: Production images symbol regression against control 3df160a
# ---------------------------------------------------------------------------

def get_gpio_bm_symbols(nm_path: str, elf_path: Path) -> dict[str, tuple[int, str]]:
    if not elf_path.is_file():
        raise FileNotFoundError(f"ELF not found: {elf_path}")
    code, out, err = run_cmd([nm_path, "-S", "--defined-only", str(elf_path)])
    if code != 0:
        raise RuntimeError(f"nm failed on {elf_path}: code {code}, err: {err}")
    syms = {}
    for line in out.splitlines():
        parts = line.split()
        if len(parts) >= 4 and parts[3].startswith("GPIO_BM_"):
            size = int(parts[1], 16)
            sym_type = parts[2]
            name = parts[3]
            syms[name] = (size, sym_type)
        elif len(parts) >= 3 and parts[2].startswith("GPIO_BM_"):
            sym_type = parts[1]
            name = parts[2]
            syms[name] = (0, sym_type)
    return syms


def compare_symbols(curr_syms: dict[str, tuple[int, str]],
                    ctrl_syms: dict[str, tuple[int, str]]) -> tuple[bool, list[str]]:
    """Pure comparator between production ELF symbols and control symbols."""
    diff_syms = []
    for sym, (sz, st) in curr_syms.items():
        if sym not in ctrl_syms:
            diff_syms.append(f"+{sym}")
        elif ctrl_syms[sym][0] != sz:
            diff_syms.append(f"{sym}(size curr={sz} vs ctrl={ctrl_syms[sym][0]})")
    for sym in ctrl_syms:
        if sym not in curr_syms:
            diff_syms.append(f"-{sym}")
    return (len(diff_syms) == 0), diff_syms


def check_production_symbols(logger: Logger, tools: dict[str, str], control_dir: Path) -> bool:
    logger.log(f"[check] Comparing production GPIO_BM_* symbols against control worktree at {control_dir}...")
    projects = [
        ("prj_01", "03_firmware_templates/prj_01_template_dualcore"),
        ("prj_02", "02_firmware_projects/prj_02_eth_lwip_dualcore"),
        ("prj_03", "02_firmware_projects/prj_03_adc_dma_eth"),
        ("prj_04", "02_firmware_projects/prj_04_adc_dma_bm_eth"),
    ]
    all_ok = True
    total_images_checked = 0

    for proj_id, proj_rel in projects:
        for preset in ("debug", "release"):
            for core in ("CM7", "CM4"):
                elf_rel = f"build/{preset}/{core}/{core.lower()}_firmware.elf"
                curr_elf = REPO_ROOT / proj_rel / elf_rel
                ctrl_elf = control_dir / proj_rel / elf_rel

                curr_syms = get_gpio_bm_symbols(tools["nm"], curr_elf)
                ctrl_syms = get_gpio_bm_symbols(tools["nm"], ctrl_elf)

                match, diffs = compare_symbols(curr_syms, ctrl_syms)
                logger.log(f"[host] CHECK {proj_id} {preset} {core} symbols ({len(curr_syms)} syms): "
                           f"{'PASS' if match else 'FAIL'}" + (f" [diff: {diffs}]" if not match else ""))
                if not match:
                    all_ok = False
                total_images_checked += 1

    # Negative control: call compare_symbols with mutated input (an injected symbol)
    fake_curr = {"GPIO_BM_SetSharedLock": (20, "T"), "GPIO_BM_ExtiConfig": (120, "T")}
    fake_ctrl = {"GPIO_BM_SetSharedLock": (20, "T")}
    neg_match, neg_diffs = compare_symbols(fake_curr, fake_ctrl)
    neg_detected = (not neg_match) and ("+GPIO_BM_ExtiConfig" in neg_diffs)
    logger.log(f"[host] CHECK production symbols negative control (extra GPIO_BM_ExtiConfig flagged by compare_symbols): "
               f"{'PASS' if neg_detected else 'FAIL'}")

    return all_ok and neg_detected and (total_images_checked == 16)


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

def main() -> int:
    parser = argparse.ArgumentParser(description="GPIO F2 verification checks (T1, T18 without board)")
    parser.add_argument("--test", choices=["t1", "t18", "all"], default="all", help="Which tests to run")
    parser.add_argument("--output", "-o", type=Path, default=None, help="Capture file destination")
    parser.add_argument("--control-dir", type=Path, default=REPO_ROOT.parent / "stm32h755_control_3df160a", help="Path to control worktree")
    parser.add_argument("--allow-dirty", action="store_true", help="accept a dirty working tree (development runs)")
    args = parser.parse_args()

    logger = Logger(args.output)
    # Log argv, options, HEAD first (M2, R4-b)
    logger.log(f"[host] argv: {shlex.join(sys.argv)}")
    logger.log(f"[host] options: test={args.test} output={args.output} control_dir={args.control_dir} allow_dirty={args.allow_dirty}")

    # Check git HEAD
    code_head, head_rev, _ = run_cmd(["git", "rev-parse", "HEAD"], cwd=REPO_ROOT)
    logger.log(f"[host] HEAD: {head_rev.strip()}")

    logger.log(f"[host] Working tree: {REPO_ROOT}")
    logger.log(f"[host] Control worktree: {args.control_dir}")

    # Verify working tree is clean
    code, status_out, _ = run_cmd(["git", "status", "--porcelain"], cwd=REPO_ROOT)
    untracked_clean = True
    for l in status_out.splitlines():
        if args.output and str(args.output.name) in l:
            continue
        untracked_clean = False
        break

    logger.log(f"[host] working tree clean: {'PASS' if untracked_clean else 'DIRTY'}")
    if not untracked_clean:
        if not args.allow_dirty:
            logger.log("[host] ERROR: Working tree is dirty. Aborting.")
            return 1
        logger.log("[host] working tree is dirty; --allow-dirty given, continuing development run")

    # Check control worktree clean and commit
    code, ctrl_rev, _ = run_cmd(["git", "rev-parse", "--short", "HEAD"], cwd=args.control_dir)
    ctrl_rev = ctrl_rev.strip()
    ctrl_ok = (ctrl_rev == CONTROL_COMMIT_EXPECTED)
    logger.log(f"[host] control commit {ctrl_rev} (expected {CONTROL_COMMIT_EXPECTED}): {'PASS' if ctrl_ok else 'FAIL'}")

    code_ctrl_st, ctrl_st_out, _ = run_cmd(["git", "status", "--porcelain", "--ignore-submodules"], cwd=args.control_dir)
    ctrl_clean = (code_ctrl_st == 0) and (len(ctrl_st_out.strip()) == 0)
    logger.log(f"[host] control worktree clean (--ignore-submodules): {'PASS' if ctrl_clean else 'DIRTY'}")
    logger.log("[host] note: STM32CubeH7 submodule in control worktree is linked via directory junction to main tree")

    if not ctrl_ok or not ctrl_clean:
        logger.log("[host] ERROR: Control worktree is not clean or not on expected commit.")
        return 1

    tools = find_tools()
    overall_ok = True

    if args.test in ("t1", "all"):
        logger.log("=== Running T1 Checks (Build Matrix & Selftests) ====")
        t1_matrix = check_matrix_build(logger)
        t1_selftests = check_selftest_builds(logger, tools)
        t1_ok = t1_matrix and t1_selftests
        logger.log(f"[host] T1 VERDICT: {'PASS' if t1_ok else 'FAIL'}")
        if not t1_ok:
            overall_ok = False

    if args.test in ("t18", "all"):
        logger.log("=== Running T18 Checks (Regression Without Board) ===")
        if args.test == "all":
            c_matrix = t1_matrix
            c_selftests = t1_selftests
        else:
            c_matrix = check_matrix_build(logger)
            c_selftests = check_selftest_builds(logger, tools)

        c_headers = check_headers(logger)
        c_nohandlers = check_no_handlers(logger, tools)
        c_sizes = check_driver_sizes(logger)
        c_symbols = check_production_symbols(logger, tools, args.control_dir)

        t18_ok = c_matrix and c_selftests and c_headers and c_nohandlers and c_sizes and c_symbols
        logger.log(f"[host] T18 VERDICT: {'PASS' if t18_ok else 'FAIL'}")
        if not t18_ok:
            overall_ok = False

    logger.log(f"OVERALL VERDICT: {'PASS' if overall_ok else 'FAIL'}")
    return 0 if overall_ok else 1


if __name__ == "__main__":
    sys.exit(main())
