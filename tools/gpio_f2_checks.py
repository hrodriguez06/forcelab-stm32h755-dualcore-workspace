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


def check_control_worktree(control_dir: Path, logger: Logger) -> tuple[bool, str]:
    """Verifies control worktree is clean with --ignore-submodules, at expected commit 3df160a,
    and compiles all 16 control ELFs in-situ via build_all.ps1 -Clean (R5)."""
    if not control_dir.is_dir():
        return False, f"Control worktree directory not found: {control_dir}"

    code, ctrl_rev, _ = run_cmd(["git", "rev-parse", "HEAD"], cwd=control_dir)
    ctrl_rev = ctrl_rev.strip()
    if code != 0 or not ctrl_rev.startswith(CONTROL_COMMIT_EXPECTED):
        return False, f"Control commit {ctrl_rev} does not match expected {CONTROL_COMMIT_EXPECTED}"

    code_ctrl_st, ctrl_st_out, _ = run_cmd(["git", "status", "--porcelain", "--ignore-submodules"], cwd=control_dir)
    if code_ctrl_st != 0 or len(ctrl_st_out.strip()) > 0:
        return False, f"Control worktree is dirty: {ctrl_st_out}"

    cube_junction = control_dir / "02_firmware_projects" / "prj_eth_lwip_node" / "external" / "STM32CubeH7"
    if not cube_junction.exists():
        return False, f"STM32CubeH7 junction does not exist: {cube_junction}"

    # R5: In-situ compilation of the 16 control images via build_all.ps1 -Clean
    logger.log(f"[check] Compiling 16 control images via build_all.ps1 -Clean in {control_dir}...")
    cmd = ["powershell", "-NoProfile", "-ExecutionPolicy", "Bypass", "-File",
           str(control_dir / "tools" / "build_all.ps1"), "-Clean"]
    t_start = time.time()
    code_build, stdout_build, stderr_build = run_cmd(cmd, cwd=control_dir)
    duration = time.time() - t_start

    passed = (code_build == 0) and ("All 8 builds PASSED successfully" in stdout_build)
    logger.log(f"[build_all_control] Finished in {duration:.1f}s, exit code {code_build}")
    for line in stdout_build.splitlines():
        if "==>" in line or "PASSED" in line or "FAILED" in line:
            logger.log(f"[build_all_control] {line.strip()}")
    logger.log(f"[host] CHECK control build_all 16/16 (-Clean): {'PASS' if passed else 'FAIL'}")

    if not passed:
        return False, f"Control compilation failed with code {code_build}: {stderr_build}"

    projects = [
        "03_firmware_templates/prj_01_template_dualcore",
        "02_firmware_projects/prj_02_eth_lwip_dualcore",
        "02_firmware_projects/prj_03_adc_dma_eth",
        "02_firmware_projects/prj_04_adc_dma_bm_eth",
    ]
    missing = []
    for p in projects:
        for preset in ("debug", "release"):
            for core in ("CM7", "CM4"):
                elf = control_dir / p / "build" / preset / core / f"{core.lower()}_firmware.elf"
                if not elf.is_file():
                    missing.append(str(elf.relative_to(control_dir)))

    if missing:
        return False, f"Missing {len(missing)} control ELFs after build: {missing}"

    return True, f"Control commit {CONTROL_COMMIT_EXPECTED} clean, 16/16 ELFs freshly compiled"


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

C_TYPE_KEYWORDS = {
    "int", "char", "void", "bool", "uint8_t", "uint16_t", "uint32_t", "uint64_t",
    "int8_t", "int16_t", "int32_t", "int64_t", "float", "double", "size_t",
    "struct", "enum", "union", "typedef", "extern", "static", "inline", "const", "volatile"
}


def strip_c_comments(code: str) -> str:
    """Strips block comments and line comments from C code."""
    code = re.sub(r"/\*.*?\*/", "", code, flags=re.DOTALL)
    code = re.sub(r"//.*", "", code)
    return code


def is_c_code_line(line_content: str) -> bool:
    """Checks if a removed diff line contains actual C code (not just comments or whitespace)."""
    clean = re.sub(r"/\*.*?\*/", "", line_content).strip()
    clean = re.sub(r"//.*", "", clean).strip()
    if not clean:
        return False
    if clean in ("/*", "/**", "*/", "*", "***"):
        return False

    # Check for closing comment delimiter */
    if "*/" in clean:
        parts = clean.split("*/", 1)
        before, after = parts[0].strip(), parts[1].strip()
        # If there is code after */ (e.g. '*/ int legacy;')
        if after and is_c_code_line(after):
            return True
        # If there is code before */ (e.g. 'int a; */')
        if before and is_c_code_line(before):
            return True
        return False

    # If line starts with *, check if it's a pointer dereference or comment continuation
    if clean.startswith("*"):
        # 1. Pointer dereference assignment (*ptr = 0; or * ptr = 0;)
        if re.match(r"^\*\s*[a-zA-Z_]\w*\s*[-+*/%&|^]?=\s*", clean):
            return True
        # 2. Pointer increment / decrement (*ptr++, *++ptr, ++*ptr, etc.)
        if re.match(r"^\*(\s*[a-zA-Z_]\w*\s*(\+\+|--)|(\+\+|--)\s*[a-zA-Z_]\w*)", clean):
            return True
        # 3. Pointer call or declaration (*fn)(...) or *ptr;
        if re.match(r"^\*\s*[a-zA-Z_]\w*\s*;", clean) or re.match(r"^\*\s*[a-zA-Z_]\w*\s*\(", clean):
            return True
        # Any other line starting with * is a comment continuation line
        return False

    # Preprocessor directive
    if clean.startswith("#"):
        return True

    # Pre/post increment/decrement (e.g. '--counter;' or 'counter++;')
    if re.match(r"^[+-]{2}\s*[a-zA-Z_]\w*\s*;", clean) or re.match(r"^[a-zA-Z_]\w*\s*[+-]{2}\s*;", clean):
        return True

    # Check for C assignment (e.g. 'a = 5;')
    if re.search(r"\b[a-zA-Z_]\w*\s*[-+*/%&|^]?=\s*[^=]", clean) and clean.endswith(";"):
        return True

    # Check for C declaration or statement (contains C keywords or types)
    words = re.findall(r"[a-zA-Z_]\w*", clean)
    if words:
        if any(w in C_TYPE_KEYWORDS or w.startswith(("GPIO_", "GpioBm_", "HAL_", "LL_")) for w in words):
            return True

    # If it ends with semicolon and has balanced parens, likely code
    if clean.endswith(";") and "(" in clean and ")" in clean:
        return True

    return False


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
        # Only skip git diff headers (e.g. '--- a/path', '+++ b/path', '@@ -x,y +z,w @@')
        if (line.startswith("--- ") or line.startswith("--- a/") or line.startswith("--- b/")) or \
           (line.startswith("+++ ") or line.startswith("+++ a/") or line.startswith("+++ b/")) or \
           line.startswith("@@"):
            continue
        if line.startswith("-"):
            content = line[1:].strip()
            if is_c_code_line(content):
                illegal_removals.append(line)

    if illegal_removals:
        return False, f"Removed or modified lines detected ({len(illegal_removals)})", illegal_removals
    return True, "Diff contains only additions", []


def check_headers(logger: Logger) -> bool:
    logger.log(f"[check] Checking all header files in 01_shared_drivers/00_gpio/ against control {CONTROL_COMMIT_EXPECTED}...")
    driver_dir = REPO_ROOT / "01_shared_drivers" / "00_gpio"

    # List headers from git in control commit to ensure no header was deleted
    code_ls, ls_out, err_ls = run_cmd(["git", "ls-tree", "--name-only", f"{CONTROL_COMMIT_EXPECTED}:01_shared_drivers/00_gpio/"], cwd=REPO_ROOT)
    if code_ls != 0:
        logger.log(f"[host] ERROR: git ls-tree failed for control {CONTROL_COMMIT_EXPECTED}: {err_ls}")
        return False
    ctrl_headers = {f.strip() for f in ls_out.splitlines() if f.strip().endswith(".h")}
    if not ctrl_headers:
        logger.log(f"[host] ERROR: No headers found in control {CONTROL_COMMIT_EXPECTED}")
        return False
    curr_headers = {h.name for h in driver_dir.glob("*.h")}
    all_headers = sorted(ctrl_headers | curr_headers)
    all_ok = True

    for h_name in all_headers:
        h_path = driver_dir / h_name
        if not h_path.is_file():
            logger.log(f"[host] CHECK header freeze {h_name}: FAIL (header deleted from control {CONTROL_COMMIT_EXPECTED})")
            all_ok = False
            continue
        passed, msg, removals = verify_header_diff(h_path, CONTROL_COMMIT_EXPECTED)
        logger.log(f"[host] CHECK header freeze {h_name} (only adds): {'PASS' if passed else 'FAIL'} ({msg})")
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

    # Additional negative controls for R5:
    # Verify line deletions with code tokens (*ptr = 0;, * ptr = 0;, int a; */, --counter;, */ int legacy;) are flagged
    fake_diff = (
        f"--- a/01_shared_drivers/00_gpio/gpio_bm.h\n"
        f"+++ b/01_shared_drivers/00_gpio/gpio_bm.h\n"
        f"@@ -10,6 +10,12 @@\n"
        f"-    * ptr = 0;\n"
        f"-    *ptr = 0;\n"
        f"-    int a; */\n"
        f"---counter;\n"
        f"-    */ int legacy;\n"
        f"-    /* helper */ int legacy_reg;\n"
        f"-    * @brief This is comment\n"
    )
    neg_comment_passed, _, neg_c_removals = verify_header_diff(driver_dir / "gpio_bm.h", CONTROL_COMMIT_EXPECTED, diff_override=fake_diff)
    neg_comments_ok = (not neg_comment_passed) and (len(neg_c_removals) == 6)
    logger.log(f"[host] CHECK comment-with-code removal negative control (R5): {'PASS' if neg_comments_ok else 'FAIL'}")

    return all_ok and neg_detected and neg_comments_ok


# ---------------------------------------------------------------------------
# Check 4: No EXTI handlers in 00_gpio/ or in production ELFs (R1)
# ---------------------------------------------------------------------------

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
        if len(parts) >= 3 and parts[1] in ("T", "t") and parts[2].startswith("EXTI") and parts[2].endswith("_IRQHandler"):
            handlers.append(parts[2])
    return handlers


def verify_no_production_exti_handlers(elf_path: Path, nm_tool: str) -> tuple[bool, list[str]]:
    code, out, _ = run_cmd([nm_tool, "--defined-only", str(elf_path)])
    if code != 0:
        return False, [f"nm_failed_code_{code}"]
    handlers = parse_production_handlers(out)
    return (len(handlers) == 0), handlers


def check_no_handlers(logger: Logger, tools: dict[str, str]) -> bool:
    logger.log("[check] Checking for EXTI handlers in 01_shared_drivers/00_gpio/...")
    driver_dir = REPO_ROOT / "01_shared_drivers" / "00_gpio"
    driver_ok, driver_handlers = verify_no_driver_handlers(driver_dir)
    logger.log(f"[host] CHECK no EXTI handlers in 00_gpio: {'PASS' if driver_ok else 'FAIL'} ({driver_handlers})")

    # Negative control for driver handlers in temporary directory (R1)
    with tempfile.TemporaryDirectory() as tmp_dir:
        tmp_driver = Path(tmp_dir)
        for f in driver_dir.glob("*.[ch]"):
            shutil.copy(f, tmp_driver / f.name)
        dummy_c = tmp_driver / "temp_dummy_exti_test.c"
        dummy_c.write_text("void EXTI0_IRQHandler(void) {}\n", encoding="utf-8")
        neg_driver_ok, neg_matches = verify_no_driver_handlers(tmp_driver)
        neg_driver_detected = (not neg_driver_ok) and any("EXTI0_IRQHandler" in m for m in neg_matches)

    logger.log(f"[host] CHECK driver handler negative control (temp .c file detected): "
               f"{'PASS' if neg_driver_detected else 'FAIL'}")

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
                elf = REPO_ROOT / proj_rel / "build" / preset / core / f"{core.lower()}_firmware.elf"
                no_h, h_list = verify_no_production_exti_handlers(elf, tools["nm"])
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
    code_nm, nm_raw, err_nm = run_cmd([tools["nm"], "--defined-only", str(dummy_elf)])
    if code_nm != 0:
        logger.log(f"[host] ERROR: nm failed on {dummy_elf}: exit code {code_nm}, {err_nm}")
        return False
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
    """Pure parser: extracts .data and .bss byte sizes for obj_name from GNU ld .map file (R1)."""
    data_size = 0
    bss_size = 0

    lines = map_text.splitlines()
    in_section: Optional[str] = None

    for line in lines:
        if line.startswith(".data") and not line.startswith(".data."):
            in_section = "data"
            continue
        elif line.startswith(".bss") and not line.startswith(".bss."):
            in_section = "bss"
            continue
        elif line.startswith(".") and not line.startswith(".data") and not line.startswith(".bss"):
            in_section = None

        if in_section and obj_name in line:
            parts = line.split()
            if len(parts) >= 3 and obj_name in parts[-1]:
                try:
                    sz = int(parts[1], 16) if parts[1].startswith("0x") else int(parts[1])
                    if in_section == "data":
                        data_size += sz
                    elif in_section == "bss":
                        bss_size += sz
                except ValueError:
                    pass

    return data_size, bss_size


def verify_driver_sizes(map_text: str, obj_name: str = "gpio_bm.c.obj") -> tuple[bool, int, int]:
    data_sz, bss_sz = parse_map_sizes(map_text, obj_name)
    ok = (data_sz == 0) and (bss_sz == 8)
    return ok, data_sz, bss_sz


def check_driver_sizes(logger: Logger) -> bool:
    logger.log("[check] Checking driver state .data=0 and .bss=8 via .map files of selftest-gpio...")
    map_dir = REPO_ROOT / "03_firmware_templates/prj_01_template_dualcore/build/selftest-gpio"
    cm7_map = map_dir / "cm7_firmware.map"
    cm4_map = map_dir / "cm4_firmware.map"

    all_ok = True
    for core, mp in (("CM7", cm7_map), ("CM4", cm4_map)):
        if not mp.is_file():
            logger.log(f"[host] CHECK driver state {core}: FAIL (map file not found at {mp})")
            all_ok = False
            continue

        text = mp.read_text(encoding="utf-8", errors="replace")
        passed, data_sz, bss_sz = verify_driver_sizes(text)
        logger.log(f"[host] CHECK driver state {core} from .map: data={data_sz} B (exp 0), bss={bss_sz} B (exp 8) -> "
                   f"{'PASS' if passed else 'FAIL'}")
        if not passed:
            all_ok = False

    # Negative control: inject .data line into real map_text passed to verify_driver_sizes (R1)
    real_cm7_text = cm7_map.read_text(encoding="utf-8", errors="replace")
    mutated_map_text = real_cm7_text + "\n.data           0x24000000        0x4\n .data.injected_var\n                0x24000000        0x4 01_shared_drivers/00_gpio/gpio_bm.c.obj\n"
    neg_passed, neg_d, neg_b = verify_driver_sizes(mutated_map_text)
    neg_detected = (not neg_passed) and (neg_d == 4) and (neg_b == 8)
    logger.log(f"[host] CHECK driver size negative control (mutated map text data={neg_d} B flagged by verify_driver_sizes): "
               f"{'PASS' if neg_detected else 'FAIL'}")

    return all_ok and neg_detected


# ---------------------------------------------------------------------------
# Check 6: Symbols and Sizes match control 3df160a in 16 production ELFs (R1)
# ---------------------------------------------------------------------------

def parse_nm_symbols(nm_text: str) -> dict[str, tuple[int, str]]:
    """Parses nm -S output for GPIO_BM_* symbols.
    Rejects lines without size field (3-field lines) and symbols with zero size (R1).
    Returns dict mapping symbol_name -> (size, type_char).
    """
    syms = {}
    for line in nm_text.splitlines():
        parts = line.split()
        if len(parts) >= 4 and parts[3].startswith("GPIO_BM_"):
            size = int(parts[1], 16)
            if size <= 0:
                raise ValueError(f"nm output line for {parts[3]} has zero or negative size: {line}")
            syms[parts[3]] = (size, parts[2])
        elif len(parts) == 3 and parts[2].startswith("GPIO_BM_"):
            raise ValueError(f"nm output line for {parts[2]} lacks size field (3-field line): {line}")
    return syms


def get_gpio_bm_symbols(nm_path: str, elf_path: Path) -> dict[str, tuple[int, str]]:
    """Extracts GPIO_BM_* symbols with their byte sizes and type from an ELF file via nm -S (R1)."""
    code, out, err = run_cmd([nm_path, "-S", "--defined-only", str(elf_path)])
    if code != 0:
        raise RuntimeError(f"nm failed on {elf_path}: code {code}, err: {err}")
    return parse_nm_symbols(out)


def compare_symbols(curr_syms: dict[str, tuple[int, str]],
                    ctrl_syms: dict[str, tuple[int, str]]) -> tuple[bool, list[str]]:
    """Pure comparator between production ELF symbols and control symbols (R1).
    Compares symbol existence, sizes, and type characters."""
    diff_syms = []
    for sym, (sz, st) in curr_syms.items():
        if sym not in ctrl_syms:
            diff_syms.append(f"+{sym}")
        elif ctrl_syms[sym][0] != sz:
            diff_syms.append(f"{sym}(size curr={sz} vs ctrl={ctrl_syms[sym][0]})")
        elif ctrl_syms[sym][1] != st:
            diff_syms.append(f"{sym}(type curr={st} vs ctrl={ctrl_syms[sym][1]})")
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
    total_symbols_compared = 0
    total_images_checked = 0

    sample_nm_out = ""
    sample_ctrl_syms = {}
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

    # Negative control 3 (minor): Change symbol type character (T -> W)
    mutated_type_syms = dict(sample_ctrl_syms)
    mutated_type_syms[sample_sym_name] = (sample_sym_size, "W")
    neg_type_match, neg_type_diffs = compare_symbols(mutated_type_syms, sample_ctrl_syms)
    neg_type_detected = (not neg_type_match) and any(f"{sample_sym_name}(type curr=W vs ctrl=" in d for d in neg_type_diffs)
    logger.log(f"[host] CHECK production symbol type change negative control ({sample_sym_name} T->W): "
               f"{'PASS' if neg_type_detected else 'FAIL'}")

    return all_ok and neg_size_detected and neg_extra_detected and neg_type_detected


# ---------------------------------------------------------------------------
# Check 7: Host expect() regex patterns vs Firmware C sources format strings (N1)
# ---------------------------------------------------------------------------

def extract_host_expect_patterns(host_path: Path) -> tuple[list[str], list[str], list[tuple[int, str, list[str]]], list[tuple[int, str]], list[tuple[int, str]], int]:
    """Pure AST extractor of expect patterns from host script (N1).
    Extracts STEPS and TIERS from the host AST instead of hardcoded lists.
    Flags any unexpandable or unrecognized placeholders."""
    tree = ast.parse(host_path.read_text(encoding="utf-8"), filename=str(host_path))
    steps_table = []
    tiers_table = []
    for node in ast.walk(tree):
        if isinstance(node, ast.Assign):
            for target in node.targets:
                if isinstance(target, ast.Name):
                    if target.id == "STEPS":
                        steps_table = [elt.elts[0].value for elt in node.value.elts if isinstance(elt, ast.Tuple)]
                    elif target.id == "TIERS":
                        tiers_table = [elt.value for elt in node.value.elts if isinstance(elt, ast.Constant)]

    if not tiers_table:
        raise RuntimeError("TIERS table could not be extracted from host script AST")

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
    unexpandable_patterns = []
    for line_no, pat, vars_used in extracted_patterns:
        if not vars_used:
            expanded_patterns.append((line_no, pat))
        else:
            curr_pats = [pat]
            has_unexpandable = False
            for v in vars_used:
                next_pats = []
                if v == "step_name" and steps_table:
                    for p in curr_pats:
                        for s in steps_table:
                            next_pats.append(p.replace("{step_name}", s))
                elif v == "tier" and tiers_table:
                    for p in curr_pats:
                        for t in tiers_table:
                            next_pats.append(p.replace("{tier}", t))
                else:
                    # Unrecognized or unexpandable placeholder!
                    has_unexpandable = True
                    break
                curr_pats = next_pats

            if has_unexpandable or not curr_pats:
                unexpandable_patterns.append((line_no, pat))
            else:
                for p in curr_pats:
                    expanded_patterns.append((line_no, p))

    return steps_table, tiers_table, extracted_patterns, expanded_patterns, unexpandable_patterns, len(expect_calls)


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


def match_expect_patterns(extracted_patterns, expanded_patterns, unexpandable_patterns, synthesized_lines) -> tuple[bool, int, list[tuple[int, str]]]:
    unmatched = []
    matched = 0
    unexp_lines = {ln for ln, _ in unexpandable_patterns}

    for line_no, pat, vars_used in extracted_patterns:
        if line_no in unexp_lines:
            unmatched.append((line_no, f"{pat} (unexpandable placeholder)"))
            continue
        exp_versions = [p for ln, p in expanded_patterns if ln == line_no]
        if not exp_versions and vars_used:
            unmatched.append((line_no, f"{pat} (no expanded patterns)"))
            continue
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
    logger.log("[check] Cross-checking host expect() regex patterns against firmware source format strings...")
    host_py = REPO_ROOT / "03_firmware_templates/prj_01_template_dualcore/tools/gpio_hw_check.py"
    steps_table, tiers_table, extracted_patterns, expanded_patterns, unexpandable_patterns, call_count = extract_host_expect_patterns(host_py)

    assert len(extracted_patterns) == call_count, f"Extracted patterns ({len(extracted_patterns)}) != expect calls ({call_count})"

    fw_c1 = REPO_ROOT / "03_firmware_templates/prj_01_template_dualcore/CM7/App/Src/gpio_selftest.c"
    fw_c2 = REPO_ROOT / "03_firmware_templates/prj_01_template_dualcore/Common/Src/gpio_selftest_common.c"
    real_fw_text = fw_c1.read_text(encoding="utf-8") + "\n" + fw_c2.read_text(encoding="utf-8")

    real_synth_lines = synthesize_fw_lines_from_sources(real_fw_text, steps_table)

    all_matched, matched_cnt, unmatched = match_expect_patterns(extracted_patterns, expanded_patterns, unexpandable_patterns, real_synth_lines)
    logger.log(f"[host] CHECK host-firmware expect patterns ({matched_cnt}/{len(extracted_patterns)} matched): "
               f"{'PASS' if all_matched else 'FAIL'}")
    if not all_matched:
        for ln, p in unmatched:
            logger.log(f"       unmatched line {ln}: {p}")

    # Negative control 1 (N1): Mutate real 'CM7 count=' in temporary copy of firmware
    mutated_fw_1 = real_fw_text.replace("CM7 count=", "CM7 counts=")
    mut_synth_1 = synthesize_fw_lines_from_sources(mutated_fw_1, steps_table)
    neg_1_ok, _, _ = match_expect_patterns(extracted_patterns, expanded_patterns, unexpandable_patterns, mut_synth_1)
    neg_cm7_count_detected = not neg_1_ok
    logger.log(f"[host] CHECK expect crosscheck negative control 1 (mutated CM7 count= rejected): "
               f"{'PASS' if neg_cm7_count_detected else 'FAIL'}")

    # Negative control 2 (N1): Mutate real 'FW VERDICT=' in temporary copy of firmware
    mutated_fw_2 = real_fw_text.replace("FW VERDICT=", "FIRMWARE VERDICT=")
    mut_synth_2 = synthesize_fw_lines_from_sources(mutated_fw_2, steps_table)
    neg_2_ok, _, _ = match_expect_patterns(extracted_patterns, expanded_patterns, unexpandable_patterns, mut_synth_2)
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
    neg_3_ok, _, _ = match_expect_patterns(mut_extracted, mut_expanded, unexpandable_patterns, real_synth_lines)
    neg_host_pat_detected = not neg_3_ok
    logger.log(f"[host] CHECK expect crosscheck negative control 3 (mutated host pattern rejected): "
               f"{'PASS' if neg_host_pat_detected else 'FAIL'}")

    # Negative control 4 (N1): Unrecognized / unexpandable placeholder must fail
    fake_extracted = [(999, r"{self.label} NO SUCH LINE ANYWHERE", ["self.label"])]
    fake_unexp = [(999, r"{self.label} NO SUCH LINE ANYWHERE")]
    neg_unk_ok, _, neg_unk_unmatched = match_expect_patterns(fake_extracted, [], fake_unexp, real_synth_lines)
    neg_unknown_detected = (not neg_unk_ok) and len(neg_unk_unmatched) == 1
    logger.log(f"[host] CHECK expect crosscheck negative control 4 (unexpandable placeholder rejected): "
               f"{'PASS' if neg_unknown_detected else 'FAIL'}")

    return all_matched and neg_cm7_count_detected and neg_verdict_detected and neg_host_pat_detected and neg_unknown_detected


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

    try:
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

        # R5: Check control worktree and compile control images in-situ
        ctrl_clean, ctrl_msg = check_control_worktree(args.control_dir, logger)
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

    except Exception as e:
        logger.log(f"[host] ERROR: Unhandled exception during checks: {e}")
        logger.log("OVERALL VERDICT: FAIL")
        return 1


if __name__ == "__main__":
    sys.exit(main())
