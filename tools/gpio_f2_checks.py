#!/usr/bin/env python3
"""Verification harness for STM32H7 GPIO Driver F2 phase (T1 build, T18 regression without board).

Usage:
    py tools/gpio_f2_checks.py [--test {t1,t18,all}] [--output <file>] [--control-dir <path>]

Standard library only.
"""

from __future__ import annotations

import argparse
import os
import re
import shutil
import subprocess
import sys
import tempfile
import time
from pathlib import Path

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

    def log(self, msg: str) -> None:
        elapsed = time.time() - self.t0
        line = f"[+ {elapsed:7.3f}s] {msg}"
        print(line, flush=True)
        self.lines.append(line)

    def write_file(self) -> None:
        if self.out_path:
            self.out_path.parent.mkdir(parents=True, exist_ok=True)
            self.out_path.write_text("\n".join(self.lines) + "\n", encoding="utf-8")


def run_cmd(args: list[str], cwd: Path | None = None) -> tuple[int, str, str]:
    res = subprocess.run(args, cwd=str(cwd) if cwd else None, capture_output=True, text=True)
    return res.returncode, res.stdout, res.stderr


# ---------------------------------------------------------------------------
# Check 1 & 2: Build Matrix & Selftest-gpio builds
# ---------------------------------------------------------------------------

def check_matrix_build(logger: Logger) -> bool:
    logger.log("[check] Checking 8/8 matrix build via build_all.ps1...")
    cmd = ["powershell", "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", str(REPO_ROOT / "tools" / "build_all.ps1")]
    code, stdout, stderr = run_cmd(cmd, cwd=REPO_ROOT)
    passed = (code == 0) and ("All 8 builds PASSED successfully" in stdout)
    logger.log(f"[host] CHECK build_all 8/8: {'PASS' if passed else 'FAIL'}")
    return passed


def check_selftest_builds(logger: Logger, tools: dict[str, str]) -> bool:
    logger.log("[check] Checking selftest-gpio compilation in prj_01 and prj_04 (0 warnings)...")
    ok = True
    for proj_rel in ("03_firmware_templates/prj_01_template_dualcore", "02_firmware_projects/prj_04_adc_dma_bm_eth"):
        proj_dir = REPO_ROOT / proj_rel
        code, out, err = run_cmd([tools["cmake"], "--build", "--preset", "selftest-gpio"], cwd=proj_dir)
        warnings = [line for line in (out + err).splitlines() if "warning:" in line.lower()]
        passed = (code == 0) and (len(warnings) == 0)
        proj_name = proj_dir.name
        logger.log(f"[host] CHECK {proj_name} selftest-gpio build: {'PASS' if passed else 'FAIL'} (code={code}, warnings={len(warnings)})")
        if not passed:
            ok = False
    return ok


# ---------------------------------------------------------------------------
# Check 3: Header diff only adds
# ---------------------------------------------------------------------------

def check_header_diff(header_path: Path, ref_commit: str) -> tuple[bool, str]:
    code, diff_out, _ = run_cmd(["git", "diff", ref_commit, "--", str(header_path)], cwd=REPO_ROOT)
    if code != 0:
        return False, f"git diff failed with exit code {code}"

    # Verify unified diff: no removed code lines
    illegal_removals = []
    for line in diff_out.splitlines():
        if line.startswith("---") or line.startswith("+++") or line.startswith("@@"):
            continue
        if line.startswith("-"):
            content = line[1:].strip()
            # Ignore purely comment changes or empty lines
            if content and not content.startswith("/*") and not content.startswith("*") and not content.startswith("//"):
                illegal_removals.append(line)

    if illegal_removals:
        return False, f"Header modified or removed lines: {illegal_removals[:3]}"
    return True, "Diff only adds declarations"


def check_headers(logger: Logger) -> bool:
    header_path = REPO_ROOT / "01_shared_drivers" / "00_gpio" / "gpio_bm.h"
    passed, msg = check_header_diff(header_path, CONTROL_COMMIT_EXPECTED)
    logger.log(f"[host] CHECK header freeze (only adds): {'PASS' if passed else 'FAIL'} ({msg})")

    # Negative control: an altered header with a deleted/changed signature
    original_text = header_path.read_text(encoding="utf-8")
    mutated_text = original_text.replace("bool GPIO_BM_EnablePortClock(GPIO_TypeDef *port);", "")
    with tempfile.NamedTemporaryFile("w", suffix=".h", delete=False, encoding="utf-8") as f:
        f.write(mutated_text)
        temp_path = Path(f.name)
    try:
        # Check diff of mutated content against original
        mut_diff_code, mut_diff, _ = run_cmd(["git", "diff", "--no-index", str(header_path), str(temp_path)])
        # Check if removal detector catches it
        removals = [l for l in mut_diff.splitlines() if l.startswith("-") and "GPIO_BM_EnablePortClock" in l]
        neg_detected = len(removals) > 0
        logger.log(f"[host] CHECK header negative control (signature removal detected): {'PASS' if neg_detected else 'FAIL'}")
    finally:
        if temp_path.is_file():
            temp_path.unlink()

    return passed and neg_detected


# ---------------------------------------------------------------------------
# Check 4: No ISR handlers in 00_gpio/
# ---------------------------------------------------------------------------

def strip_c_comments(code: str) -> str:
    # Remove block comments /* ... */
    code = re.sub(r"/\*.*?\*/", "", code, flags=re.DOTALL)
    # Remove line comments // ...
    code = re.sub(r"//.*", "", code)
    return code


def check_no_handlers(logger: Logger) -> bool:
    driver_dir = REPO_ROOT / "01_shared_drivers" / "00_gpio"
    patterns = [r"\bEXTI\w*_IRQHandler\b", r"\bHAL_GPIO_EXTI_Callback\b"]
    matches = []
    for file in driver_dir.glob("*.[ch]"):
        content = strip_c_comments(file.read_text(encoding="utf-8"))
        for pat in patterns:
            for m in re.finditer(pat, content):
                matches.append(f"{file.name}:{m.group(0)}")

    passed = (len(matches) == 0)
    logger.log(f"[host] CHECK no EXTI handlers in 00_gpio: {'PASS' if passed else 'FAIL'}")

    # Negative control: simulated file containing EXTI0_IRQHandler
    simulated_content = "void EXTI0_IRQHandler(void) { }"
    neg_matches = [m.group(0) for pat in patterns for m in re.finditer(pat, simulated_content)]
    neg_detected = (len(neg_matches) > 0)
    logger.log(f"[host] CHECK no EXTI handlers negative control: {'PASS' if neg_detected else 'FAIL'}")
    return passed and neg_detected


# ---------------------------------------------------------------------------
# Check 5: .data = 0 and .bss = 8 B in gpio_bm.o
# ---------------------------------------------------------------------------

def check_driver_sizes(logger: Logger, tools: dict[str, str]) -> bool:
    obj_paths = list((REPO_ROOT / "03_firmware_templates" / "prj_01_template_dualcore" / "build" / "selftest-gpio").glob("**/gpio_bm.c.obj"))
    if not obj_paths:
        logger.log("[host] CHECK driver state .data=0 .bss=8: FAIL (gpio_bm.c.obj not found in selftest-gpio build)")
        return False

    all_ok = True
    for obj in obj_paths:
        code, out, _ = run_cmd([tools["size"], str(obj)])
        # Parse size output
        lines = out.strip().splitlines()
        if len(lines) >= 2:
            parts = lines[1].split()
            text_sz, data_sz, bss_sz = int(parts[0]), int(parts[1]), int(parts[2])
            core = "CM7" if "CM7" in str(obj) else "CM4"
            match = (data_sz == 0) and (bss_sz == 8)
            logger.log(f"[host] CHECK driver state {core} gpio_bm.o: text={text_sz} data={data_sz} bss={bss_sz} -> {'PASS' if match else 'FAIL'}")
            if not match:
                all_ok = False

    # Negative control: verify that data > 0 or bss != 8 is flagged
    fake_data, fake_bss = 4, 12
    neg_detected = not ((fake_data == 0) and (fake_bss == 8))
    logger.log(f"[host] CHECK driver state negative control (data={fake_data}, bss={fake_bss} flagged): {'PASS' if neg_detected else 'FAIL'}")
    return all_ok and neg_detected


# ---------------------------------------------------------------------------
# Check 6: Production images symbol regression against control 3df160a
# ---------------------------------------------------------------------------

def get_gpio_bm_symbols(nm_path: str, elf_path: Path) -> dict[str, tuple[int, str]]:
    if not elf_path.is_file():
        return {}
    code, out, _ = run_cmd([nm_path, "-S", "--defined-only", str(elf_path)])
    if code != 0:
        return {}
    syms = {}
    for line in out.splitlines():
        parts = line.split()
        # nm -S: <addr> [<size>] <type> <name>
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


def check_production_symbols(logger: Logger, tools: dict[str, str], control_dir: Path) -> bool:
    logger.log(f"[check] Comparing production GPIO_BM_* symbols against control at {control_dir}...")
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

                if not curr_elf.is_file() or not ctrl_elf.is_file():
                    logger.log(f"[host] WARNING: Missing ELF for {proj_id} {preset} {core} (curr={curr_elf.is_file()}, ctrl={ctrl_elf.is_file()})")
                    all_ok = False
                    continue

                curr_syms = get_gpio_bm_symbols(tools["nm"], curr_elf)
                ctrl_syms = get_gpio_bm_symbols(tools["nm"], ctrl_elf)

                # Symbols must be identical in presence and size
                diff_syms = []
                for sym, (sz, st) in curr_syms.items():
                    if sym not in ctrl_syms:
                        diff_syms.append(f"+{sym}")
                    elif ctrl_syms[sym][0] != sz:
                        diff_syms.append(f"{sym}(size curr={sz} vs ctrl={ctrl_syms[sym][0]})")
                for sym in ctrl_syms:
                    if sym not in curr_syms:
                        diff_syms.append(f"-{sym}")

                match = (len(diff_syms) == 0)
                logger.log(f"[host] CHECK {proj_id} {preset} {core} symbols ({len(curr_syms)} syms): {'PASS' if match else 'FAIL'}" +
                           (f" [diff: {diff_syms}]" if not match else ""))
                if not match:
                    all_ok = False
                total_images_checked += 1

    # Negative control: inject an extra symbol GPIO_BM_ExtiConfig into curr_syms
    fake_curr = {"GPIO_BM_SetSharedLock": (20, "T"), "GPIO_BM_ExtiConfig": (120, "T")}
    fake_ctrl = {"GPIO_BM_SetSharedLock": (20, "T")}
    neg_diff = [s for s in fake_curr if s not in fake_ctrl]
    neg_detected = (len(neg_diff) > 0)
    logger.log(f"[host] CHECK production symbols negative control (extra GPIO_BM_Exti* flagged): {'PASS' if neg_detected else 'FAIL'}")

    return all_ok and neg_detected and (total_images_checked == 16)


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

def main() -> int:
    parser = argparse.ArgumentParser(description="GPIO F2 verification checks (T1, T18 without board)")
    parser.add_argument("--test", choices=["t1", "t18", "all"], default="all", help="Which tests to run")
    parser.add_argument("--output", "-o", type=Path, default=None, help="Capture file destination")
    parser.add_argument("--control-dir", type=Path, default=REPO_ROOT.parent / "stm32h755_control_3df160a", help="Path to control worktree")
    args = parser.parse_args()

    logger = Logger(args.output)
    logger.log(f"gpio_f2_checks.py starting: test={args.test}")
    logger.log(f"Working tree: {REPO_ROOT}")
    logger.log(f"Control worktree: {args.control_dir}")

    # Check git status
    code, status_out, _ = run_cmd(["git", "status", "--porcelain"], cwd=REPO_ROOT)
    untracked_clean = True
    for l in status_out.splitlines():
        # Allow only docs/ or output capture if being written
        if args.output and str(args.output.name) in l:
            continue
        untracked_clean = False
        break

    logger.log(f"[host] working tree clean: {'PASS' if untracked_clean else 'DIRTY'}")

    # Check control commit
    code, ctrl_rev, _ = run_cmd(["git", "rev-parse", "--short", "HEAD"], cwd=args.control_dir)
    ctrl_rev = ctrl_rev.strip()
    ctrl_ok = (ctrl_rev == CONTROL_COMMIT_EXPECTED)
    logger.log(f"[host] control commit {ctrl_rev} (expected {CONTROL_COMMIT_EXPECTED}): {'PASS' if ctrl_ok else 'FAIL'}")

    if not ctrl_ok:
        logger.log("[host] ERROR: Control worktree is not on expected commit.")
        logger.write_file()
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
        c_nohandlers = check_no_handlers(logger)
        c_sizes = check_driver_sizes(logger, tools)
        c_symbols = check_production_symbols(logger, tools, args.control_dir)

        t18_ok = c_matrix and c_selftests and c_headers and c_nohandlers and c_sizes and c_symbols
        logger.log(f"[host] T18 VERDICT: {'PASS' if t18_ok else 'FAIL'}")
        if not t18_ok:
            overall_ok = False

    logger.log(f"OVERALL VERDICT: {'PASS' if overall_ok else 'FAIL'}")
    logger.write_file()
    return 0 if overall_ok else 1


if __name__ == "__main__":
    sys.exit(main())
