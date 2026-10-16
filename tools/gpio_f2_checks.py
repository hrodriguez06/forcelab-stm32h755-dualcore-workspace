#!/usr/bin/env python3
"""Verification harness for STM32H7 GPIO Driver F2 phase (T1 build, T18 regression without board).

Usage:
    py tools/gpio_f2_checks.py [--test {t1,t18,all}] [--output <file>] [--control-dir <path>] [--allow-dirty]

Standard library only.
"""

from __future__ import annotations

import argparse
import ast
import os
import re
import shlex
import shutil
import subprocess
import sys
import tempfile
import time
from pathlib import Path
from typing import Optional

REPO_ROOT = Path(__file__).resolve().parent.parent
CONTROL_COMMIT_EXPECTED = "3df160a"


def run_cmd(args: list[str], cwd: Path | None = None) -> tuple[int, str, str]:
    res = subprocess.run(args, cwd=str(cwd) if cwd else None, capture_output=True, text=True)
    return res.returncode, res.stdout, res.stderr


def find_tools() -> dict[str, str]:
    # Call tools/env.ps1 to resolve ST toolchain environment (m7)
    env_script = REPO_ROOT / "tools" / "env.ps1"
    if env_script.is_file():
        code, out, _ = run_cmd(["powershell", "-NoProfile", "-ExecutionPolicy", "Bypass",
                                "-Command", f". '{env_script}' -Quiet; $env:PATH"])
        if code == 0 and out.strip():
            # Last line of output is the PATH
            resolved_path = out.strip().splitlines()[-1]
            os.environ["PATH"] = resolved_path + os.pathsep + os.environ.get("PATH", "")

    # Only if arm-none-eabi-gcc is not found on PATH, search C:\ST as fallback (minor)
    if not shutil.which("arm-none-eabi-gcc"):
        for p in Path(r"C:\ST").glob("**/tools/bin"):
            os.environ["PATH"] = os.environ.get("PATH", "") + os.pathsep + str(p)

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


# ---------------------------------------------------------------------------
# Clean tree & Control checks (R3, R5)
# ---------------------------------------------------------------------------

def check_clean_working_tree(repo_root: Path, output_file: Path | None) -> tuple[bool, str]:
    """Verifies that git status --porcelain is clean.
    Only ignores untracked lines matching the exact output capture file (R3)."""
    code, status_out, err = run_cmd(["git", "status", "--porcelain"], cwd=repo_root)
    if code != 0:
        return False, f"git status failed with exit code {code}: {err}"

    out_resolved = output_file.resolve() if output_file is not None else None

    dirty_lines = []
    for line in status_out.splitlines():
        line_s = line.strip()
        if not line_s:
            continue
        if line.startswith("??"):
            untracked_rel = line[3:].strip().strip('"')
            untracked_resolved = (repo_root / untracked_rel).resolve()
            if out_resolved is not None and untracked_resolved == out_resolved:
                continue
        dirty_lines.append(line)

    if dirty_lines:
        return False, f"Dirty tree ({len(dirty_lines)} files): {dirty_lines}"
    return True, "Tree is clean"


def check_control_worktree(control_dir: Path) -> tuple[bool, str]:
    """Verifies control worktree is clean, at expected commit, and has all 16 ELFs built from 3df160a (R5)."""
    if not control_dir.is_dir():
        return False, f"Control worktree directory not found: {control_dir}"

    code, ctrl_rev, _ = run_cmd(["git", "rev-parse", "--short", "HEAD"], cwd=control_dir)
    ctrl_rev = ctrl_rev.strip()
    if ctrl_rev != CONTROL_COMMIT_EXPECTED:
        return False, f"Control commit {ctrl_rev} does not match expected {CONTROL_COMMIT_EXPECTED}"

    code_ctrl_st, ctrl_st_out, _ = run_cmd(["git", "status", "--porcelain", "--ignore-submodules"], cwd=control_dir)
    if code_ctrl_st != 0 or len(ctrl_st_out.strip()) > 0:
        return False, f"Control worktree is dirty: {ctrl_st_out}"

    cube_junction = control_dir / "02_firmware_projects" / "prj_eth_lwip_node" / "external" / "STM32CubeH7"
    if not cube_junction.exists():
        return False, f"STM32CubeH7 junction does not exist: {cube_junction}"

    projects = [
        "03_firmware_templates/prj_01_template_dualcore",
        "02_firmware_projects/prj_02_eth_lwip_dualcore",
        "02_firmware_projects/prj_03_adc_dma_eth",
        "02_firmware_projects/prj_04_adc_dma_bm_eth",
    ]
    missing = []
    invalid_commit = []
    for p in projects:
        for preset in ("debug", "release"):
            for core in ("CM7", "CM4"):
                elf = control_dir / p / "build" / preset / core / f"{core.lower()}_firmware.elf"
                if not elf.is_file():
                    missing.append(str(elf.relative_to(control_dir)))
                else:
                    data = elf.read_bytes()
                    if b"3df160a" not in data:
                        invalid_commit.append(str(elf.relative_to(control_dir)))

    if missing:
        return False, f"Missing {len(missing)} control ELFs: {missing}"
    if invalid_commit:
        return False, f"Control ELFs not built from {CONTROL_COMMIT_EXPECTED}: {invalid_commit}"
    return True, f"Control commit {ctrl_rev} clean, 16/16 ELFs verified (built from {CONTROL_COMMIT_EXPECTED})"


def check_cube_submodule(repo_root: Path) -> tuple[bool, str]:
    """Verifies STM32CubeH7 submodule commit (R5)."""
    code, out, err = run_cmd(["git", "submodule", "status"], cwd=repo_root)
    if code != 0:
        return False, f"git submodule status failed ({code}): {err}"
    for line in out.splitlines():
        if "STM32CubeH7" in line:
            parts = line.strip().split()
            commit = parts[0].lstrip("-+U")
            if commit.startswith("8ec3113"):
                return True, f"STM32CubeH7 submodule commit {commit[:7]} (heads/master)"
            return False, f"Unexpected STM32CubeH7 commit: {commit}"
    return False, "STM32CubeH7 submodule not found in submodule status"


# ---------------------------------------------------------------------------
# Check 1 & 2: Build Matrix & Selftest-gpio builds (R2)
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


def parse_compiler_warnings(build_output: str) -> list[str]:
    """Pure function: returns all compiler warning lines from build output (R2)."""
    warnings = []
    for line in build_output.splitlines():
        if re.search(r"\bwarning:", line, re.IGNORECASE):
            warnings.append(line.strip())
    return warnings


def count_built_objects(build_output: str) -> int:
    """Pure function: counts lines where ninja or gcc built a C/CXX object (R2)."""
    return sum(1 for line in build_output.splitlines() if "Building C object" in line or "Building CXX object" in line)


def check_selftest_builds(logger: Logger, tools: dict[str, str]) -> bool:
    logger.log("[check] Checking selftest-gpio compilation in prj_01 and prj_04 (--fresh and --clean-first)...")
    ok = True
    for proj_rel in ("03_firmware_templates/prj_01_template_dualcore", "02_firmware_projects/prj_04_adc_dma_bm_eth"):
        proj_dir = REPO_ROOT / proj_rel
        # R2: check exit code of cmake configure with --fresh
        cfg_code, cfg_out, cfg_err = run_cmd([tools["cmake"], "--preset", "selftest-gpio", "--fresh"], cwd=proj_dir)
        if cfg_code != 0:
            logger.log(f"[host] ERROR: cmake --preset selftest-gpio --fresh failed on {proj_dir.name} (code={cfg_code}): {cfg_err}")
            ok = False
            continue

        # R2: build with --clean-first
        code, out, err = run_cmd([tools["cmake"], "--build", "--preset", "selftest-gpio", "--clean-first"], cwd=proj_dir)
        full_out = out + "\n" + err

        warnings = parse_compiler_warnings(full_out)
        objects_built = count_built_objects(full_out)

        passed = (code == 0) and (len(warnings) == 0) and (objects_built > 0)
        proj_name = proj_dir.name
        logger.log(f"[host] CHECK {proj_name} selftest-gpio build: {'PASS' if passed else 'FAIL'} "
                   f"(code={code}, objects_built={objects_built}, warnings={len(warnings)})")
        if not passed:
            ok = False

    # Negative control for warning counter (R2)
    fake_build_out = "Building C object CM7/CMakeFiles/app.dir/main.c.obj\nmain.c:42:5: warning: unused variable 'x' [-Wunused-variable]"
    neg_warnings = parse_compiler_warnings(fake_build_out)
    neg_warn_detected = len(neg_warnings) == 1 and "unused variable" in neg_warnings[0]
    logger.log(f"[host] CHECK warning counter negative control (synthetic warning flagged): "
               f"{'PASS' if neg_warn_detected else 'FAIL'}")

    return ok and neg_warn_detected


# ---------------------------------------------------------------------------
# Check 3: Header diff only adds (R1, R5)
# ---------------------------------------------------------------------------

def verify_header_diff(header_path: Path, ref_commit: str,
                       diff_override: str | None = None) -> tuple[bool, str, list[str]]:
    """Pure function: checks that git diff against ref_commit contains only additions.
    If diff_override is provided, parses that text directly (R1, R5)."""
    if diff_override is not None:
        diff_out = diff_override
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
            # R5: Strip full inline C comments
            clean = re.sub(r"/\*.*?\*/", "", content)
            clean = re.sub(r"//.*", "", clean).strip()
            # Handle block comment continuation or termination
            code_part = clean
            if code_part.startswith("*/"):
                code_part = code_part[2:].strip()
            elif code_part.startswith("*"):
                if "*/" in code_part:
                    code_part = code_part.split("*/", 1)[1].strip()
                else:
                    code_part = ""  # purely comment body line
            if code_part:
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

    # Negative control 1 (R1, R5): Use a temporary directory outside the repo tree!
    with tempfile.TemporaryDirectory() as tmp_dir:
        tmp_path = Path(tmp_dir)
        bm_copy = tmp_path / "gpio_bm.h"
        ctrl_copy = tmp_path / "gpio_bm_ctrl.h"

        code_cat, ctrl_text, _ = run_cmd(["git", "show", f"{CONTROL_COMMIT_EXPECTED}:01_shared_drivers/00_gpio/gpio_bm.h"], cwd=REPO_ROOT)
        if code_cat != 0:
            raise RuntimeError("Failed to fetch control gpio_bm.h from git")
        ctrl_copy.write_text(ctrl_text, encoding="utf-8")

        # Mutate function signature
        mutated_text = ctrl_text.replace(
            "bool GPIO_BM_EnablePortClock(GPIO_TypeDef *port);",
            "int GPIO_BM_EnablePortClock(GPIO_TypeDef *port, int extra_param);"
        )
        bm_copy.write_text(mutated_text, encoding="utf-8")

        # Generate diff outside repo working tree
        _, mut_diff, _ = run_cmd(["git", "diff", "--no-index", str(ctrl_copy), str(bm_copy)])
        neg_passed, neg_msg, neg_removals = verify_header_diff(bm_copy, CONTROL_COMMIT_EXPECTED, diff_override=mut_diff)
        neg_detected = (not neg_passed) and any("GPIO_BM_EnablePortClock" in r for r in neg_removals)

    logger.log(f"[host] CHECK header negative control (signature mutation flagged against {CONTROL_COMMIT_EXPECTED}): "
               f"{'PASS' if neg_detected else 'FAIL'} ({neg_msg})")

    # Additional negative control for R5: line with /* comment */ int x; or */ int x; removed
    fake_diff_1 = "-    /* helper */ int legacy_reg;\n+    int new_reg;"
    neg_comment_passed1, _, neg_c_removals1 = verify_header_diff(driver_dir / "gpio_bm.h", CONTROL_COMMIT_EXPECTED, diff_override=fake_diff_1)
    neg_comment_detected1 = (not neg_comment_passed1) and len(neg_c_removals1) == 1

    fake_diff_2 = "-    */ int legacy_code;\n+    int new_code;"
    neg_comment_passed2, _, neg_c_removals2 = verify_header_diff(driver_dir / "gpio_bm.h", CONTROL_COMMIT_EXPECTED, diff_override=fake_diff_2)
    neg_comment_detected2 = (not neg_comment_passed2) and len(neg_c_removals2) == 1

    neg_comments_ok = neg_comment_detected1 and neg_comment_detected2
    logger.log(f"[host] CHECK comment-with-code removal negative control (R5): {'PASS' if neg_comments_ok else 'FAIL'}")

    return all_ok and neg_detected and neg_comments_ok


# ---------------------------------------------------------------------------
# Check 4: No EXTI handlers in 00_gpio/ or in production ELFs (R1)
# ---------------------------------------------------------------------------

def strip_c_comments(code: str) -> str:
    code = re.sub(r"/\*.*?\*/", "", code, flags=re.DOTALL)
    code = re.sub(r"//.*", "", code)
    return code


def verify_no_driver_handlers(driver_dir: Path) -> tuple[bool, list[str]]:
    """Pure function: verifies no EXTI*_IRQHandler or HAL_GPIO_EXTI_Callback in driver C/H files."""
    patterns = [r"\bEXTI\w*_IRQHandler\b", r"\bHAL_GPIO_EXTI_Callback\b"]
    matches = []
    for file in sorted(driver_dir.glob("*.[ch]")):
        content = strip_c_comments(file.read_text(encoding="utf-8"))
        for pat in patterns:
            for m in re.finditer(pat, content):
                matches.append(f"{file.name}:{m.group(0)}")
    return (len(matches) == 0), matches


def parse_production_handlers(nm_text: str) -> list[str]:
    """Pure function: parses nm --defined-only output text for strong EXTI*_IRQHandler symbols ('T'/'t') (R1)."""
    handlers = []
    for line in nm_text.splitlines():
        parts = line.split()
        if len(parts) >= 3:
            stype = parts[-2]
            name = parts[-1]
            if stype in ("T", "t") and re.match(r"^EXTI\w*_IRQHandler$", name):
                handlers.append(name)
    return handlers


def verify_no_production_exti_handlers(elf_path: Path, nm_tool: str) -> tuple[bool, list[str]]:
    if not elf_path.is_file():
        return False, [f"File not found: {elf_path}"]
    code, out, _ = run_cmd([nm_tool, "--defined-only", str(elf_path)])
    if code != 0:
        return False, [f"nm failed with code {code}"]
    handlers = parse_production_handlers(out)
    return (len(handlers) == 0), handlers


def check_no_handlers(logger: Logger, tools: dict[str, str]) -> bool:
    logger.log("[check] Checking for EXTI handlers in 01_shared_drivers/00_gpio/...")
    driver_dir = REPO_ROOT / "01_shared_drivers" / "00_gpio"
    driver_ok, driver_matches = verify_no_driver_handlers(driver_dir)
    logger.log(f"[host] CHECK no EXTI handlers in 00_gpio: {'PASS' if driver_ok else 'FAIL'}")

    # Negative control for driver handlers: create temp directory OUTSIDE the repository tree (R1)
    with tempfile.TemporaryDirectory() as tmp_driver_dir:
        tmp_d = Path(tmp_driver_dir)
        for f in driver_dir.glob("*.[ch]"):
            shutil.copy2(f, tmp_d / f.name)
        (tmp_d / "temp_dummy_exti_test.c").write_text("void EXTI0_IRQHandler(void) {}\n", encoding="utf-8")
        neg_d_ok, neg_d_matches = verify_no_driver_handlers(tmp_d)
        neg_driver_detected = (not neg_d_ok) and any("EXTI0_IRQHandler" in m for m in neg_d_matches)

    logger.log(f"[host] CHECK driver handler negative control (temp .c file detected): "
               f"{'PASS' if neg_driver_detected else 'FAIL'}")

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

    # Negative control: inject symbol into real nm output text passed to parse_production_handlers (R1)
    dummy_elf = REPO_ROOT / "03_firmware_templates/prj_01_template_dualcore/build/debug/CM7/cm7_firmware.elf"
    _, nm_raw, _ = run_cmd([tools["nm"], "--defined-only", str(dummy_elf)])
    mutated_nm = nm_raw + "\n08009999 T EXTI3_IRQHandler\n"
    parsed_handlers = parse_production_handlers(mutated_nm)
    neg_prod_detected = "EXTI3_IRQHandler" in parsed_handlers
    logger.log(f"[host] CHECK production ELF negative control (mutated nm text flagged by parse_production_handlers): "
               f"{'PASS' if neg_prod_detected else 'FAIL'}")

    return driver_ok and neg_driver_detected and prod_ok and pos_detected and neg_prod_detected


# ---------------------------------------------------------------------------
# Check 5: .data = 0 and .bss = 8 B in gpio_bm.c.obj via .map file (R1)
# ---------------------------------------------------------------------------

def parse_map_sizes(map_text: str, obj_name: str = "gpio_bm.c.obj") -> tuple[int, int]:
    """Pure function: parses .map text for total .data and .bss bytes contributed by obj_name (R1)."""
    data_bytes = 0
    bss_bytes = 0
    lines = map_text.splitlines()
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


def verify_driver_sizes(map_text: str, obj_name: str = "gpio_bm.c.obj") -> tuple[bool, int, int]:
    """Pure function: verifies .data == 0 and .bss == 8 B via parse_map_sizes (R1)."""
    data_sz, bss_sz = parse_map_sizes(map_text, obj_name)
    passed = (data_sz == 0) and (bss_sz == 8)
    return passed, data_sz, bss_sz


def check_driver_sizes(logger: Logger) -> bool:
    logger.log("[check] Checking driver state .data=0 and .bss=8 via .map files of selftest-gpio...")
    map_dir = REPO_ROOT / "03_firmware_templates/prj_01_template_dualcore/build/selftest-gpio"
    cm7_map = map_dir / "cm7_firmware.map"
    cm4_map = map_dir / "cm4_firmware.map"

    all_ok = True
    for core, mp in (("CM7", cm7_map), ("CM4", cm4_map)):
        text = mp.read_text(encoding="utf-8", errors="replace")
        passed, data_sz, bss_sz = verify_driver_sizes(text)
        logger.log(f"[host] CHECK driver state {core} from .map: data={data_sz} B (exp 0), bss={bss_sz} B (exp 8) -> "
                   f"{'PASS' if passed else 'FAIL'}")
        if not passed:
            all_ok = False

    # Negative control: inject .data line into real map_text passed to verify_driver_sizes (R1)
    real_cm7_text = cm7_map.read_text(encoding="utf-8", errors="replace")
    mutated_map_text = real_cm7_text + "\n .data.injected_var\n                0x24000000        0x4 01_shared_drivers/00_gpio/gpio_bm.c.obj\n"
    neg_passed, neg_d, neg_b = verify_driver_sizes(mutated_map_text)
    neg_detected = (not neg_passed) and (neg_d == 4) and (neg_b == 8)
    logger.log(f"[host] CHECK driver size negative control (mutated map text data={neg_d} B flagged by verify_driver_sizes): "
               f"{'PASS' if neg_detected else 'FAIL'}")

    return all_ok and neg_detected


# ---------------------------------------------------------------------------
# Check 6: Production images symbol regression against control 3df160a (R1)
# ---------------------------------------------------------------------------

def parse_nm_symbols(nm_text: str) -> dict[str, tuple[int, str]]:
    """Pure function: parses nm -S --defined-only output for GPIO_BM_* symbols (R1).
    Fails on malformed 3-field lines missing size to prevent false empty comparisons."""
    syms = {}
    for line in nm_text.splitlines():
        parts = line.split()
        if len(parts) >= 4 and parts[3].startswith("GPIO_BM_"):
            size = int(parts[1], 16)
            sym_type = parts[2]
            name = parts[3]
            syms[name] = (size, sym_type)
        elif len(parts) >= 3 and parts[2].startswith("GPIO_BM_"):
            # R1: nm -S output must provide size. A 3-field line means size is missing!
            raise ValueError(f"Malformed nm -S line (missing size for symbol {parts[2]}): {line}")
    return syms


def get_gpio_bm_symbols(nm_path: str, elf_path: Path) -> dict[str, tuple[int, str]]:
    if not elf_path.is_file():
        raise FileNotFoundError(f"ELF not found: {elf_path}")
    code, out, err = run_cmd([nm_path, "-S", "--defined-only", str(elf_path)])
    if code != 0:
        raise RuntimeError(f"nm failed on {elf_path}: code {code}, err: {err}")
    return parse_nm_symbols(out)


def compare_symbols(curr_syms: dict[str, tuple[int, str]],
                    ctrl_syms: dict[str, tuple[int, str]]) -> tuple[bool, list[str]]:
    """Pure comparator between production ELF symbols and control symbols (R1)."""
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
    total_symbols_compared = 0
    sample_nm_out = ""
    sample_ctrl_syms: dict[str, tuple[int, str]] = {}
    sample_sym_name = ""
    sample_sym_size = 0

    for proj_id, proj_rel in projects:
        for preset in ("debug", "release"):
            for core in ("CM7", "CM4"):
                elf_rel = f"build/{preset}/{core}/{core.lower()}_firmware.elf"
                curr_elf = REPO_ROOT / proj_rel / elf_rel
                ctrl_elf = control_dir / proj_rel / elf_rel

                code_curr, curr_out, err_curr = run_cmd([tools["nm"], "-S", "--defined-only", str(curr_elf)])
                if code_curr != 0:
                    raise RuntimeError(f"nm failed on {curr_elf}: code {code_curr}, err: {err_curr}")

                curr_syms = parse_nm_symbols(curr_out)
                ctrl_syms = get_gpio_bm_symbols(tools["nm"], ctrl_elf)

                if not sample_nm_out and curr_syms:
                    sample_nm_out = curr_out
                    sample_ctrl_syms = ctrl_syms
                    for s_name, (s_sz, _) in curr_syms.items():
                        if s_sz > 0:
                            sample_sym_name = s_name
                            sample_sym_size = s_sz
                            break

                match, diffs = compare_symbols(curr_syms, ctrl_syms)
                logger.log(f"[host] CHECK {proj_id} {preset} {core} symbols ({len(curr_syms)} syms): "
                           f"{'PASS' if match else 'FAIL'}" + (f" [diff: {diffs}]" if not match else ""))
                if not match:
                    all_ok = False
                total_images_checked += 1
                total_symbols_compared += len(curr_syms)

    # R1: The symbol comparison cannot be empty across production images!
    if total_symbols_compared == 0:
        logger.log("[host] ERROR: Total symbols compared across all production images is 0!")
        all_ok = False

    # Negative control 1 (R1): Alter the size of a REAL symbol in nm output
    if not sample_sym_name or sample_sym_size == 0:
        raise RuntimeError("No suitable sample symbol with non-zero size found for negative control")

    mutated_size_nm_lines = []
    found_and_mutated = False
    for line in sample_nm_out.splitlines():
        parts = line.split()
        if len(parts) >= 4 and parts[3] == sample_sym_name:
            orig_size = int(parts[1], 16)
            mutated_size = orig_size + 16
            mutated_size_hex = f"{mutated_size:08x}"
            mutated_line = f"{parts[0]} {mutated_size_hex} {parts[2]} {parts[3]}"
            mutated_size_nm_lines.append(mutated_line)
            found_and_mutated = True
        else:
            mutated_size_nm_lines.append(line)

    if not found_and_mutated:
        raise RuntimeError(f"Failed to find {sample_sym_name} in sample_nm_out to mutate size")

    mutated_size_syms = parse_nm_symbols("\n".join(mutated_size_nm_lines))
    neg_size_match, neg_size_diffs = compare_symbols(mutated_size_syms, sample_ctrl_syms)
    neg_size_detected = (not neg_size_match) and any(f"{sample_sym_name}(size curr=" in d for d in neg_size_diffs)
    logger.log(f"[host] CHECK production real symbol size mutation negative control ({sample_sym_name}): "
               f"{'PASS' if neg_size_detected else 'FAIL'}")

    # Negative control 2 (R1): Add extra symbol into real nm output text
    mutated_nm_out = sample_nm_out + "\n08005000 00000080 T GPIO_BM_ExtiConfig\n"
    mutated_syms = parse_nm_symbols(mutated_nm_out)
    neg_match, neg_diffs = compare_symbols(mutated_syms, sample_ctrl_syms)
    neg_extra_detected = (not neg_match) and ("+GPIO_BM_ExtiConfig" in neg_diffs)
    logger.log(f"[host] CHECK production extra symbol negative control (+GPIO_BM_ExtiConfig): "
               f"{'PASS' if neg_extra_detected else 'FAIL'}")

    return all_ok and neg_size_detected and neg_extra_detected and (total_images_checked == 16)


# ---------------------------------------------------------------------------
# Check 7: Host-firmware expect regex cross-check (N1, B1)
# ---------------------------------------------------------------------------

def extract_host_expect_patterns(host_path: Path) -> tuple[list[str], list[tuple[int, str, list[str]]], list[tuple[int, str]], int]:
    """Pure AST extractor of expect patterns from host script (N1)."""
    tree = ast.parse(host_path.read_text(encoding="utf-8"), filename=str(host_path))
    steps_table = []
    tiers = ["BM", "LL", "HAL"]
    for node in ast.walk(tree):
        if isinstance(node, ast.Assign):
            for target in node.targets:
                if isinstance(target, ast.Name) and target.id == "STEPS":
                    steps_table = [elt.elts[0].value for elt in node.value.elts if isinstance(elt, ast.Tuple)]

    expect_calls = []
    for node in ast.walk(tree):
        if isinstance(node, ast.Call):
            if isinstance(node.func, ast.Attribute) and node.func.attr == "expect":
                v = node.func.value
                is_con = (isinstance(v, ast.Name) and v.id == "con") or \
                         (isinstance(v, ast.Attribute) and v.attr == "con")
                if is_con:
                    expect_calls.append(node)

    extracted_patterns = []
    for node in expect_calls:
        arg0 = node.args[0]
        line_no = node.lineno
        if isinstance(arg0, ast.Constant):
            extracted_patterns.append((line_no, arg0.value, []))
        elif isinstance(arg0, ast.JoinedStr):
            parts = []
            vars_used = []
            for part in arg0.values:
                if isinstance(part, ast.Constant):
                    parts.append(part.value)
                elif isinstance(part, ast.FormattedValue):
                    var_name = ast.unparse(part.value)
                    parts.append(f"{{{var_name}}}")
                    vars_used.append(var_name)
            extracted_patterns.append((line_no, "".join(parts), vars_used))

    expanded_patterns = []
    for line_no, pat, vars_used in extracted_patterns:
        if not vars_used:
            expanded_patterns.append((line_no, pat))
        else:
            curr_pats = [pat]
            for v in vars_used:
                next_pats = []
                for p in curr_pats:
                    if v == "step_name":
                        for s in steps_table:
                            next_pats.append(p.replace("{step_name}", s))
                    elif v == "tier":
                        for t in tiers:
                            next_pats.append(p.replace("{tier}", t))
                curr_pats = next_pats
            for p in curr_pats:
                expanded_patterns.append((line_no, p))

    return steps_table, extracted_patterns, expanded_patterns, len(expect_calls)


def synthesize_fw_lines_from_sources(fw_sources: str, steps_table: list[str]) -> set[str]:
    """Extracts format strings directly from firmware sources and synthesizes valid output lines without manual injection (N1)."""
    c_logs = re.findall(r'LOG\s*\(\s*"([^"]+)"', fw_sources)
    c_verdicts = re.findall(r'verdict\s*\([^,]+,\s*"([^"]+)"', fw_sources)
    c_step_begins = re.findall(r'step_begin\s*\(\s*"([^"]+)"', fw_sources)

    synthesized_lines = set()
    subst_variants = [
        {"%s": "dummy", "%lu": "200", "%u": "1", "%ld": "0", "0x%08lX": "0x00000008", "0x%02lX": "0x00", "0x%04lX": "0x0000", "%lx": "1234"},
        {"%s": "dummy", "%lu": "0", "%u": "0", "%ld": "0", "0x%08lX": "0x00000000", "0x%02lX": "0x00", "0x%04lX": "0x0000", "%lx": "0"},
        {"%s": "dummy", "%lu": "400", "%u": "1", "%ld": "0", "0x%08lX": "0x00000003", "0x%02lX": "0x00", "0x%04lX": "0x0000", "%lx": "abcd"},
        {"%s": "dummy", "%lu": "100", "%u": "1", "%ld": "0", "0x%08lX": "0x00000101", "0x%02lX": "0x00", "0x%04lX": "0x0000", "%lx": "9f658d5aed47dff7"},
    ]

    for fmt in c_logs:
        s_candidates = steps_table + ["PASS", "FAIL", "rev_1", "build_id_1", "(CPU2 not ready)"]
        if "%s" in fmt:
            for s_cand in s_candidates:
                for variant in subst_variants:
                    line = fmt.replace("%s", s_cand)
                    for k, val in variant.items():
                        if k != "%s":
                            line = line.replace(k, val)
                    synthesized_lines.add(f"[gpio] {line}")
        else:
            for variant in subst_variants:
                line = fmt
                for k, val in variant.items():
                    line = line.replace(k, val)
                synthesized_lines.add(f"[gpio] {line}")

    for fmt in c_verdicts:
        s_candidates = steps_table + ["PASS", "FAIL"]
        for s_cand in s_candidates:
            for variant in subst_variants:
                line = fmt.replace("%s", s_cand)
                for k, val in variant.items():
                    if k != "%s":
                        line = line.replace(k, val)
                synthesized_lines.add(f"[gpio] {line} -> PASS")
                synthesized_lines.add(f"[gpio] {line} -> FAIL")

    for sb in c_step_begins:
        synthesized_lines.add(f"[gpio] STEP {sb} bus=NONE v=1")
    for step in steps_table:
        synthesized_lines.add(f"[gpio] STEP {step} bus=NONE v=1")

    return synthesized_lines


def match_expect_patterns(extracted_patterns, expanded_patterns, synthesized_lines) -> tuple[bool, int, list[tuple[int, str]]]:
    unmatched = []
    matched = 0
    for line_no, pat, vars_used in extracted_patterns:
        exp_versions = [p for ln, p in expanded_patterns if ln == line_no]
        all_exp_matched = True
        for p in exp_versions:
            rx = re.compile(p)
            if not any(rx.search(s) for s in synthesized_lines):
                all_exp_matched = False
                unmatched.append((line_no, p))
                break
        if all_exp_matched:
            matched += 1
    return (len(unmatched) == 0), matched, unmatched


def check_host_firmware_expect_crosscheck(logger: Logger) -> bool:
    """Verifies that all host expect patterns match firmware format strings (N1, B1)."""
    logger.log("[check] Cross-checking host expect() regex patterns against firmware source format strings...")
    host_py = REPO_ROOT / "03_firmware_templates/prj_01_template_dualcore/tools/gpio_hw_check.py"
    fw_c = REPO_ROOT / "03_firmware_templates/prj_01_template_dualcore/CM7/App/Src/gpio_selftest.c"
    fw_common = REPO_ROOT / "03_firmware_templates/prj_01_template_dualcore/Common/Src/gpio_selftest_common.c"

    steps_table, extracted_patterns, expanded_patterns, total_calls = extract_host_expect_patterns(host_py)
    if len(extracted_patterns) != total_calls:
        logger.log(f"[host] ERROR: Extracted patterns count ({len(extracted_patterns)}) != con.expect calls ({total_calls})")
        return False

    real_fw_text = fw_c.read_text(encoding="utf-8") + "\n" + fw_common.read_text(encoding="utf-8")
    real_synth_lines = synthesize_fw_lines_from_sources(real_fw_text, steps_table)

    all_matched, matched_count, unmatched = match_expect_patterns(extracted_patterns, expanded_patterns, real_synth_lines)
    logger.log(f"[host] CHECK host-firmware expect patterns ({matched_count}/{len(extracted_patterns)} matched): "
               f"{'PASS' if all_matched else 'FAIL'}" + (f" [unmatched: {unmatched}]" if unmatched else ""))

    # Negative control 1 (N1): Mutate real 'CM7 count=' in temporary copy of firmware
    mutated_fw_1 = real_fw_text.replace("CM7 count=", "CM7 counts=")
    mut_synth_1 = synthesize_fw_lines_from_sources(mutated_fw_1, steps_table)
    neg_1_ok, _, _ = match_expect_patterns(extracted_patterns, expanded_patterns, mut_synth_1)
    neg_cm7_count_detected = not neg_1_ok
    logger.log(f"[host] CHECK expect crosscheck negative control 1 (mutated CM7 count= rejected): "
               f"{'PASS' if neg_cm7_count_detected else 'FAIL'}")

    # Negative control 2 (N1): Mutate real 'FW VERDICT=' in temporary copy of firmware
    mutated_fw_2 = real_fw_text.replace("FW VERDICT=", "FIRMWARE VERDICT=")
    mut_synth_2 = synthesize_fw_lines_from_sources(mutated_fw_2, steps_table)
    neg_2_ok, _, _ = match_expect_patterns(extracted_patterns, expanded_patterns, mut_synth_2)
    neg_verdict_detected = not neg_2_ok
    logger.log(f"[host] CHECK expect crosscheck negative control 2 (mutated FW VERDICT= rejected): "
               f"{'PASS' if neg_verdict_detected else 'FAIL'}")

    # Negative control 3 (N1): Mutate a real extracted expect pattern in a temporary list
    mut_extracted = [
        (ln, pat.replace("dual-core EXTI routing", "dualcore routing"), vu) if "dual-core" in pat else (ln, pat, vu)
        for ln, pat, vu in extracted_patterns
    ]
    mut_expanded = [
        (ln, pat.replace("dual-core EXTI routing", "dualcore routing"))
        for ln, pat in expanded_patterns
    ]
    neg_3_ok, _, _ = match_expect_patterns(mut_extracted, mut_expanded, real_synth_lines)
    neg_host_pat_detected = not neg_3_ok
    logger.log(f"[host] CHECK expect crosscheck negative control 3 (mutated host pattern rejected): "
               f"{'PASS' if neg_host_pat_detected else 'FAIL'}")

    return all_matched and neg_cm7_count_detected and neg_verdict_detected and neg_host_pat_detected


# ---------------------------------------------------------------------------
# Main (R5)
# ---------------------------------------------------------------------------

def main() -> int:
    parser = argparse.ArgumentParser(description="GPIO F2 verification checks (T1, T18 without board)")
    parser.add_argument("--test", choices=["t1", "t18", "all"], default="all", help="Which tests to run")
    parser.add_argument("--output", "-o", type=Path, default=None, help="Capture file destination")
    parser.add_argument("--control-dir", type=Path, default=REPO_ROOT.parent / "stm32h755_control_3df160a", help="Path to control worktree")
    parser.add_argument("--allow-dirty", action="store_true", help="accept a dirty working tree (development runs only; fails audit)")
    args = parser.parse_args()

    logger = Logger(args.output)
    logger.log(f"[host] argv: {shlex.join(sys.argv)}")
    logger.log(f"[host] options: test={args.test} output={args.output} control_dir={args.control_dir} allow_dirty={args.allow_dirty}")

    # Check git HEAD
    code_head, head_rev, _ = run_cmd(["git", "rev-parse", "HEAD"], cwd=REPO_ROOT)
    logger.log(f"[host] HEAD: {head_rev.strip()}")
    logger.log(f"[host] Working tree: {REPO_ROOT}")
    logger.log(f"[host] Control worktree: {args.control_dir}")

    # R3: Verify working tree clean
    tree_clean, tree_msg = check_clean_working_tree(REPO_ROOT, args.output)
    logger.log(f"[host] working tree clean: {'PASS' if tree_clean else 'DIRTY'} ({tree_msg})")
    if not tree_clean:
        if not args.allow_dirty:
            logger.log("[host] ERROR: Working tree is dirty. Aborting.")
            logger.log("OVERALL VERDICT: FAIL")
            return 1
        # R5: --allow-dirty cannot end in PASS for audit certification
        logger.log("[host] WARNING: --allow-dirty specified on dirty tree. Audit certification requires clean tree.")

    # R5: Check control worktree
    ctrl_clean, ctrl_msg = check_control_worktree(args.control_dir)
    logger.log(f"[host] control worktree ({CONTROL_COMMIT_EXPECTED}): {'PASS' if ctrl_clean else 'FAIL'} ({ctrl_msg})")
    if not ctrl_clean:
        logger.log(f"[host] ERROR: Control worktree verification failed: {ctrl_msg}")
        logger.log("OVERALL VERDICT: FAIL")
        return 1

    # R5: Check STM32CubeH7 submodule
    cube_ok, cube_msg = check_cube_submodule(REPO_ROOT)
    logger.log(f"[host] STM32CubeH7 submodule commit: {'PASS' if cube_ok else 'FAIL'} ({cube_msg})")
    if not cube_ok:
        logger.log(f"[host] ERROR: Submodule check failed: {cube_msg}")
        logger.log("OVERALL VERDICT: FAIL")
        return 1

    tools = find_tools()
    overall_ok = True if tree_clean else False

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
        c_cross = check_host_firmware_expect_crosscheck(logger)

        t18_ok = c_matrix and c_selftests and c_headers and c_nohandlers and c_sizes and c_symbols and c_cross
        logger.log(f"[host] T18 VERDICT: {'PASS' if t18_ok else 'FAIL'}")
        if not t18_ok:
            overall_ok = False

    if not tree_clean:
        logger.log("[host] FAIL: overall verdict cannot be PASS because working tree was dirty (--allow-dirty)")
        overall_ok = False

    logger.log(f"OVERALL VERDICT: {'PASS' if overall_ok else 'FAIL'}")
    return 0 if overall_ok else 1


if __name__ == "__main__":
    sys.exit(main())
