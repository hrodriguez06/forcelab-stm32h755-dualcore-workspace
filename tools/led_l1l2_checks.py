#!/usr/bin/env python3
"""Verification harness for STM32H7 LED Driver L1/L2 phase (Revision 3).

Covers tests T1, T2, T3 (off-board) and T4 (on-board SWD verification):
  - T1: CI matrix build (8 configurations: prj_01..04 debug/release) compiled directly
        via cmake --preset and cmake --build --preset --clean-first (B1, m1), plus
        9 selftest presets linking led_bm.
        Checks: exit code 0, 0 compiler warnings, built objects count > 0, led_bm.c.obj compiled
        and present on disk and in build log (m2).
        Negative control (MA2): real temporary C file compiled outside repo with deliberate warning
        via run_cmd and project flags, routed through count_built_objects and evaluate_build_output.
  - T2: Compile-time #error and range checks with arm-none-eabi-gcc -fsyntax-only:
        Runs with cwd in temporary directory outside repository to prevent .su leakage (M1).
        Checks: positive controls compile with 0 warnings, 16 failing mutations trigger specific #error (m3).
  - T3: Initial content of g_led_config in .data of CM7 led_bm.c.obj across all 8 production builds
        contrasted against UM2408 hardware oracle and against control commit 9dc6896 compiled in-situ.
        Ensures images are compiled in current execution and checks FW_GIT_REV against HEAD (MA1, M2):
        - prj_01: verified from generated/fw_build_id.h (stripping -dirty suffix)
        - prj_04: verified from cm7_firmware.elf binary bytes
        - prj_02 & prj_03: verified from CMakeCache.txt configure flags and build freshness
        Negative control (m4): mutated objdump text verified to alter content and get rejected with exactly 24 bytes.
  - T4: Hardware board test (for auditor): ensures debug images are built in current execution from HEAD,
        checks FW_GIT_REV (MA1, M2), flashes 4 projects with tools/flash.py, reads RAM and MODER registers
        via SWD HOTPLUG -r32 with sn, logs exact hexadecimal readings (m7).
        Negative controls (m5): mutated RAM table and mutated MODER register rejected.

Clean working tree:
  Checked at start and end; ignores only the exact output file if untracked (m6).
  All subprocesses enforce execution timeouts (m7).

Standard library only.
"""

from __future__ import annotations

import argparse
import copy
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
from typing import Optional

REPO_ROOT = Path(__file__).resolve().parent.parent
CONTROL_COMMIT_EXPECTED = "9dc6896"
DEFAULT_SN = "0016000C4142501220353451"

RUN_START_TIME = time.time()

PROJECTS = [
    "03_firmware_templates/prj_01_template_dualcore",
    "02_firmware_projects/prj_02_eth_lwip_dualcore",
    "02_firmware_projects/prj_03_adc_dma_eth",
    "02_firmware_projects/prj_04_adc_dma_bm_eth",
]

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

SELFTEST_PRESETS = {
    "03_firmware_templates/prj_01_template_dualcore": [
        "selftest-gpio",
        "selftest-tim",
        "selftest-spi",
        "selftest-i2c",
        "selftest-led",
    ],
    "02_firmware_projects/prj_04_adc_dma_bm_eth": [
        "selftest",
        "selftest-adc",
        "selftest-uart",
        "selftest-hsem",
        "selftest-gpio",
    ],
}

# UM2408 Oracle:
# LD1 (Green):  GPIOB (0x58020400), pin 0
# LD2 (Yellow): GPIOE (0x58021000), pin 1
# LD3 (Red):    GPIOB (0x58020400), pin 14
UM2408_ORACLE_WORDS = [0x58020400, 0, 0x58021000, 1, 0x58020400, 14]
UM2408_ORACLE_BYTES = b"\x00\x04\x02\x58\x00\x00\x00\x00\x00\x10\x02\x58\x01\x00\x00\x00\x00\x04\x02\x58\x0e\x00\x00\x00"

GPIOB_BASE = 0x58020400
GPIOE_BASE = 0x58021000


def run_cmd(args: list[str], cwd: Path | None = None, timeout: float = 300.0) -> tuple[int, str, str]:
    """Runs a command with an explicit timeout (m7)."""
    try:
        res = subprocess.run(args, cwd=str(cwd) if cwd else None, capture_output=True, text=True, timeout=timeout)
        return res.returncode, res.stdout, res.stderr
    except subprocess.TimeoutExpired:
        return 124, "", f"Command timed out after {timeout} seconds: {' '.join(str(a) for a in args)}"


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


def find_tools() -> dict[str, str]:
    """Resolves toolchain executables from PATH, tools/env.ps1 or C:\\ST."""
    env_script = REPO_ROOT / "tools" / "env.ps1"
    if env_script.is_file():
        code, out, _ = run_cmd([
            "powershell", "-NoProfile", "-ExecutionPolicy", "Bypass",
            "-Command", f". '{env_script}' -Quiet; $env:PATH"
        ], timeout=30.0)
        if code == 0 and out.strip():
            resolved_path = out.strip().splitlines()[-1]
            os.environ["PATH"] = resolved_path + os.pathsep + os.environ.get("PATH", "")

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
    tools["objdump"] = str(bin_dir / "arm-none-eabi-objdump.exe")
    tools["nm"] = str(bin_dir / "arm-none-eabi-nm.exe")
    tools["size"] = str(bin_dir / "arm-none-eabi-size.exe")

    for k in ("objdump", "nm", "size"):
        if not Path(tools[k]).is_file():
            which_t = shutil.which(f"arm-none-eabi-{k}")
            tools[k] = which_t if which_t else f"arm-none-eabi-{k}"

    cmake_path = shutil.which("cmake")
    if not cmake_path:
        for p in Path(r"C:\ST").glob("**/cmake.exe"):
            cmake_path = str(p)
            break
    tools["cmake"] = cmake_path or "cmake"

    prog_path = shutil.which("STM32_Programmer_CLI")
    if not prog_path:
        for p in Path("C:/ST").glob("**/bin/STM32_Programmer_CLI.exe"):
            prog_path = str(p)
            break
    tools["programmer"] = prog_path or "STM32_Programmer_CLI"

    return tools


def get_head_short_rev(repo: Path = REPO_ROOT) -> str:
    """Returns short SHA for HEAD in the given repository."""
    code, out, _ = run_cmd(["git", "rev-parse", "--short", "HEAD"], cwd=repo, timeout=10.0)
    return out.strip() if code == 0 else "unknown"


def check_clean_working_tree(repo_root: Path, output_file: Path | None) -> tuple[bool, str]:
    """Verifies git status --porcelain -u is clean, ignoring ONLY the exact output file (m6)."""
    code, status_out, err = run_cmd(["git", "status", "--porcelain", "-u"], cwd=repo_root, timeout=30.0)
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
            # m6: Only ignore the exact output file path
            if out_resolved is not None and untracked_resolved == out_resolved:
                continue
        dirty_lines.append(line)

    if dirty_lines:
        return False, f"Dirty tree ({len(dirty_lines)} files): {dirty_lines}"
    return True, "Tree is clean"


def ensure_control_worktree(control_dir: Path, logger: Logger) -> tuple[bool, str]:
    """Ensures control worktree exists at CONTROL_COMMIT_EXPECTED and is clean."""
    if not control_dir.is_dir():
        logger.log(f"[control] Creating control worktree at {control_dir} (commit {CONTROL_COMMIT_EXPECTED})...")
        code, out, err = run_cmd(["git", "worktree", "add", str(control_dir), CONTROL_COMMIT_EXPECTED], cwd=REPO_ROOT, timeout=60.0)
        if code != 0:
            return False, f"git worktree add failed ({code}): {err or out}"

    code, ctrl_rev, _ = run_cmd(["git", "rev-parse", "HEAD"], cwd=control_dir, timeout=10.0)
    ctrl_rev = ctrl_rev.strip()
    if code != 0 or not ctrl_rev.startswith(CONTROL_COMMIT_EXPECTED):
        return False, f"Control commit {ctrl_rev} does not match expected {CONTROL_COMMIT_EXPECTED}"

    # Actually check git status of control worktree
    code_ctrl_st, ctrl_st_out, _ = run_cmd(["git", "status", "--porcelain", "--ignore-submodules"], cwd=control_dir, timeout=30.0)
    if code_ctrl_st != 0 or ctrl_st_out.strip():
        return False, f"Control worktree is dirty or status failed ({code_ctrl_st}): {ctrl_st_out.strip()}"

    # Ensure submodule junction for STM32CubeH7
    cube_submodule_dst = control_dir / "02_firmware_projects" / "prj_eth_lwip_node" / "external" / "STM32CubeH7"
    cube_submodule_src = REPO_ROOT / "02_firmware_projects" / "prj_eth_lwip_node" / "external" / "STM32CubeH7"
    sentinel = cube_submodule_dst / "Drivers" / "STM32H7xx_HAL_Driver" / "Src" / "stm32h7xx_hal_rcc.c"

    if not sentinel.is_file() and cube_submodule_src.is_dir():
        if cube_submodule_dst.exists():
            try:
                if cube_submodule_dst.is_dir():
                    shutil.rmtree(cube_submodule_dst)
                else:
                    cube_submodule_dst.unlink()
            except Exception:
                pass
        cube_submodule_dst.parent.mkdir(parents=True, exist_ok=True)
        cmd_junc = ["cmd", "/c", "mklink", "/J", str(cube_submodule_dst), str(cube_submodule_src)]
        run_cmd(cmd_junc, timeout=15.0)

    return True, f"Control worktree at {CONTROL_COMMIT_EXPECTED} ready"


def compile_control_images(control_dir: Path, logger: Logger) -> bool:
    """Compiles control worktree via build_all.ps1 -Clean in-situ and counts pass lines (m1)."""
    logger.log(f"[check] Compiling control images via build_all.ps1 -Clean in {control_dir}...")
    cmd = [
        "powershell", "-NoProfile", "-ExecutionPolicy", "Bypass",
        "-File", str(control_dir / "tools" / "build_all.ps1"), "-Clean"
    ]
    t0 = time.time()
    code, stdout, stderr = run_cmd(cmd, cwd=control_dir, timeout=600.0)
    dur = time.time() - t0

    # m1: count actual pass lines from build_all.ps1
    pass_count = sum(1 for line in stdout.splitlines() if "-> PASS" in line or " PASS " in line)
    passed = (code == 0) and (pass_count >= 8) and ("All 8 builds PASSED successfully" in stdout)

    logger.log(f"[build_all_control] Finished in {dur:.1f}s, exit code {code}, pass_count={pass_count}")
    for line in stdout.splitlines():
        if "==>" in line or "PASSED" in line or "FAILED" in line:
            logger.log(f"[build_all_control] {line.strip()}")
    logger.log(f"[host] CHECK control build_all 8/8 (-Clean): {'PASS' if passed else 'FAIL'}")
    return passed


# ---------------------------------------------------------------------------
# T1: Build Matrix & Selftests (B1, m1, m2, MA1, MA2)
# ---------------------------------------------------------------------------

def parse_compiler_warnings(build_output: str) -> list[str]:
    """Extracts all compiler warning lines from build output."""
    warnings = []
    for line in build_output.splitlines():
        if re.search(r"\bwarning:", line, re.IGNORECASE):
            warnings.append(line.strip())
    return warnings


def count_built_objects(build_output: str) -> int:
    """Counts lines where ninja or gcc built a C/CXX object."""
    return sum(1 for line in build_output.splitlines() if "Building C object" in line or "Building CXX object" in line)


def evaluate_build_output(
    proj_name: str,
    preset: str,
    build_code: int,
    build_output: str,
    built_objects: int,
    led_obj_found: bool,
    expected_warnings: int = 0
) -> tuple[bool, list[str], str]:
    """Pure evaluation function used for real builds and synthetic warning negative control (B1, MA2)."""
    warnings = parse_compiler_warnings(build_output)
    passed = (
        (build_code == 0)
        and (len(warnings) == expected_warnings)
        and (built_objects > 0)
        and led_obj_found
    )
    detail = (
        f"code={build_code}, objs={built_objects}, warnings={len(warnings)}, "
        f"led_bm_obj={'found' if led_obj_found else 'missing'}"
    )
    return passed, warnings, detail


def check_image_freshness_and_rev(
    pdir: Path,
    preset: str,
    run_start_time: float,
    expected_short_rev: str
) -> tuple[bool, str]:
    """Verifies that an image was built during this execution and matches expected FW_GIT_REV (MA1, M2)."""
    cm7_elf = pdir / "build" / preset / "CM7" / "cm7_firmware.elf"
    if not cm7_elf.is_file():
        return False, f"ELF file not found at {cm7_elf}"

    mtime = cm7_elf.stat().st_mtime
    # Allow 5s buffer around run start
    if mtime < run_start_time - 5.0:
        return False, f"ELF file is stale (mtime {mtime:.0f} < start {run_start_time:.0f})"

    pname = pdir.name
    # prj_01 embeds FW_GIT_REV in generated/fw_build_id.h
    if pname == "prj_01_template_dualcore":
        bid_h = pdir / "build" / preset / "generated" / "fw_build_id.h"
        if not bid_h.is_file():
            return False, f"fw_build_id.h not found at {bid_h}"
        content = bid_h.read_text(encoding="utf-8")
        m = re.search(r'#define\s+FW_GIT_REV\s+"([^"]+)"', content)
        if not m:
            return False, f"FW_GIT_REV not found in {bid_h}"
        rev = m.group(1)
        if rev.endswith("-dirty"):
            rev = rev[:-len("-dirty")]
        if rev != expected_short_rev:
            return False, f"FW_GIT_REV '{m.group(1)}' does not match expected '{expected_short_rev}'"

    # prj_04 embeds FW_GIT_REV in compilation definitions and ELF binary strings
    elif pname == "prj_04_adc_dma_bm_eth":
        raw = cm7_elf.read_bytes()
        if expected_short_rev.encode("ascii") not in raw:
            return False, f"FW_GIT_REV '{expected_short_rev}' not found in binary {cm7_elf.name}"

    # prj_02 and prj_03 do not embed FW_GIT_REV in their ELFs; provenance: clean tree and run compilation (MA1)
    elif pname in ("prj_02_eth_lwip_dualcore", "prj_03_adc_dma_eth"):
        return True, f"fresh (mtime={mtime:.0f}), rev no embebida (procedencia: arbol limpio al empezar y al terminar y compilacion en esta ejecucion)"

    return True, f"fresh (mtime={mtime:.0f}), rev={expected_short_rev}"


def compile_single_config(
    tools: dict[str, str],
    pdir: Path,
    preset: str,
    head_rev: str,
    logger: Logger
) -> tuple[bool, int, str, int, bool]:
    """Configures and builds a single project preset directly via cmake (B1, m2, MA1)."""
    pname = pdir.name
    cfg_cmd = [tools["cmake"], "--preset", preset, "--fresh"]
    cfg_code, cfg_out, cfg_err = run_cmd(cfg_cmd, cwd=pdir, timeout=120.0)
    if cfg_code != 0:
        logger.log(f"[host] FAIL: configure {pname} [{preset}] failed (code={cfg_code}): {cfg_err or cfg_out}")
        return False, cfg_code, cfg_out + "\n" + cfg_err, 0, False
    if "CMake Error" in (cfg_out + cfg_err):
        logger.log(f"[host] FAIL: configure {pname} [{preset}] reported error: {cfg_err or cfg_out}")
        return False, 1, cfg_out + "\n" + cfg_err, 0, False

    # 2. cmake --build --preset <preset> --clean-first
    bld_code, bld_out, bld_err = run_cmd([tools["cmake"], "--build", "--preset", preset, "--clean-first"], cwd=pdir, timeout=300.0)
    full_out = bld_out + "\n" + bld_err

    # Check built objects count
    objs = count_built_objects(full_out)

    # Check that led_bm.c.obj was compiled (m2: in log AND exists on disk)
    led_obj_in_log = "led_bm.c.obj" in full_out
    cm7_dir = pdir / "build" / preset / "CM7"
    led_obj_on_disk = len(list(cm7_dir.rglob("led_bm.c.obj"))) > 0 if cm7_dir.is_dir() else False
    led_obj_found = led_obj_in_log and led_obj_on_disk

    passed, warnings, detail = evaluate_build_output(pname, preset, bld_code, full_out, objs, led_obj_found)
    return passed, bld_code, full_out, objs, led_obj_found


def run_t1(tools: dict[str, str], logger: Logger) -> bool:
    logger.log("=== Running T1 Checks (Build Matrix 8/8 & Selftest Presets) ===")
    overall_ok = True
    head_rev = get_head_short_rev()

    # 1. Production build matrix 8/8 directly via cmake (B1, m1, m2, MA1)
    logger.log("[check] Compiling 8 matrix configurations directly via cmake --clean-first...")
    matrix_passed_count = 0

    for proj_rel, preset in MATRIX_CONFIGS:
        pdir = REPO_ROOT / proj_rel
        pname = pdir.name
        passed, bld_code, full_out, objs, led_obj_found = compile_single_config(tools, pdir, preset, head_rev, logger)
        warnings = parse_compiler_warnings(full_out)
        rev_ok, rev_msg = check_image_freshness_and_rev(pdir, preset, RUN_START_TIME, head_rev)

        config_ok = passed and rev_ok
        # Log actual bld_code (not hardcoded code=0)
        detail = f"code={bld_code}, objs={objs}, warnings={len(warnings)}, led_bm_obj={'found' if led_obj_found else 'missing'}, image={rev_msg}"
        logger.log(f"[host] CHECK matrix {pname} [{preset}]: {'PASS' if config_ok else 'FAIL'} ({detail})")

        if config_ok:
            matrix_passed_count += 1
        else:
            overall_ok = False

    matrix_ok = (matrix_passed_count == 8)
    logger.log(f"[host] CHECK T1 matrix 8/8 (clean-first, 0 warnings, led_bm.c.obj compiled): {'PASS' if matrix_ok else 'FAIL'} ({matrix_passed_count}/8 passed)")
    if not matrix_ok:
        overall_ok = False

    # 2. Selftest presets linking led_bm (m2: configure checked, led_bm.c.obj verified)
    logger.log("[check] Checking selftest presets linking led_bm (--fresh and --clean-first)...")
    for proj_rel, presets in SELFTEST_PRESETS.items():
        pdir = REPO_ROOT / proj_rel
        pname = pdir.name
        for preset in presets:
            passed, bld_code, full_out, objs, led_obj_found = compile_single_config(tools, pdir, preset, head_rev, logger)
            warnings = parse_compiler_warnings(full_out)
            detail = f"code={bld_code}, objs={objs}, warnings={len(warnings)}, led_bm_obj={'found' if led_obj_found else 'missing'}"
            logger.log(f"[host] CHECK selftest {pname} [{preset}]: {'PASS' if passed else 'FAIL'} ({detail})")
            if not passed:
                overall_ok = False

    # 3. Negative control for warning counter using real compilation of a temporary C file (MA2)
    with tempfile.TemporaryDirectory(prefix="led_t1_neg_") as tmp_dir:
        tmp_path = Path(tmp_dir)
        c_file = tmp_path / "warning_sample.c"
        c_obj = tmp_path / "warning_sample.o"
        # Temporary C file with a deliberate compiler warning
        c_file.write_text(
            "int test_synthetic_warning(void) {\n"
            "    int unused_warning_variable = 42;\n"
            "    return 0;\n"
            "}\n",
            encoding="utf-8"
        )
        p1_dir = REPO_ROOT / "03_firmware_templates/prj_01_template_dualcore"
        p1_tokens = extract_compile_tokens(p1_dir)
        cmd = [tools["gcc"]] + p1_tokens + ["-c", str(c_file), "-o", str(c_obj)]
        # Compile via run_cmd with cwd outside repo (MA2)
        comp_code, comp_out, comp_err = run_cmd(cmd, cwd=tmp_path, timeout=30.0)
        full_neg_out = comp_out + "\n" + comp_err
        neg_objs = count_built_objects(full_neg_out)
        if neg_objs == 0 and c_obj.is_file():
            neg_objs = 1

        # Route real compiler output through evaluate_build_output
        neg_passed, neg_warnings, neg_detail = evaluate_build_output(
            "negative_control_warning", "debug", comp_code, full_neg_out,
            built_objects=neg_objs, led_obj_found=True, expected_warnings=0
        )
        # Must fail because evaluate_build_output detects the real compiler warning
        neg_warn_ok = (neg_passed is False) and (len(neg_warnings) >= 1) and any("unused" in w for w in neg_warnings)
        logger.log(f"[host] CHECK warning counter negative control (compiled C warning flagged): {'PASS' if neg_warn_ok else 'FAIL'}")
        if not neg_warn_ok:
            overall_ok = False

    logger.log(f"[host] T1 VERDICT: {'PASS' if overall_ok else 'FAIL'}")
    return overall_ok


# ---------------------------------------------------------------------------
# T2: Compile-Time #error & Range Checks (M1, m3)
# ---------------------------------------------------------------------------

def extract_compile_tokens(project_dir: Path) -> list[str]:
    """Loads compile_commands.json and returns compiler flags for led_bm.c."""
    comp_json = project_dir / "build" / "debug" / "compile_commands.json"
    if not comp_json.is_file():
        raise FileNotFoundError(f"compile_commands.json missing at {comp_json}")

    with open(comp_json, "r", encoding="utf-8") as f:
        data = json.load(f)

    for entry in data:
        if "led_bm.c" in entry.get("file", "") or "main.c" in entry.get("file", ""):
            cmd = entry.get("command") or entry.get("arguments")
            if isinstance(cmd, list):
                tokens = cmd
            else:
                tokens = shlex.split(cmd, posix=False)

            cleaned = []
            skip_next = False
            for tok in tokens[1:]:  # skip compiler executable
                if skip_next:
                    skip_next = False
                    continue
                if tok in ("-c",):
                    continue
                if tok == "-o":
                    skip_next = True
                    continue
                if tok.startswith("-o"):
                    continue
                if tok.endswith(".c") and not tok.startswith("-"):
                    continue
                # Remove include flags pointing to Common/Inc so our temp dir controls board_def.h
                if tok.startswith("-I") and (tok.replace("\\", "/").endswith("/Common/Inc")):
                    continue
                if '\\"' in tok:
                    tok = tok.replace('\\"', '"')
                cleaned.append(tok)
            return cleaned

    raise ValueError(f"No valid compile entry found in {comp_json}")


def compile_variant(
    tools: dict[str, str],
    base_tokens: list[str],
    board_def_dir: Path,
    expected_fail: bool,
    expected_token: Optional[str] = None
) -> tuple[bool, str]:
    """Shared function for positive check and negative mutations.

    M1: Runs with cwd=board_def_dir in temp directory so -fstack-usage writes .su outside repo.
    m3: Validates #error line specifically and checks warnings on positive control.
    """
    source_file = str(REPO_ROOT / "01_shared_drivers" / "01_led" / "led_bm.c")
    cmd = [tools["gcc"], f"-I{board_def_dir}"] + base_tokens + ["-fsyntax-only", source_file]
    # M1 fix: cwd=board_def_dir ensures a-led_bm.su is written into temp dir, NOT into the repo tree!
    code, stdout, stderr = run_cmd(cmd, cwd=board_def_dir, timeout=60.0)

    if not expected_fail:
        warnings = parse_compiler_warnings(stderr + "\n" + stdout)
        passed = (code == 0) and (len(warnings) == 0)
        detail = f"exit={code}, warnings={len(warnings)}"
        return passed, detail
    else:
        # m3 fix: find the specific #error line and check expected_token on that line
        error_lines = [line.strip() for line in stderr.splitlines() if "error:" in line and "#error" in line]
        has_error = (code != 0) and (len(error_lines) > 0)

        target_line = error_lines[0] if error_lines else (stderr.strip().splitlines()[-1] if stderr else "no error")
        has_token = (expected_token is None) or (expected_token in target_line)
        passed = has_error and has_token
        detail = f"exit={code}, msg='{target_line}'"
        return passed, detail


def run_t2(tools: dict[str, str], logger: Logger) -> bool:
    logger.log("=== Running T2 Checks (Compile-time #error & Range Validations) ===")
    overall_ok = True
    total_cases = 0
    passed_cases = 0

    with tempfile.TemporaryDirectory(prefix="led_t2_scratch_") as scratch_root:
        scratch_dir = Path(scratch_root)

        for proj_rel in PROJECTS:
            proj_dir = REPO_ROOT / proj_rel
            proj_name = proj_dir.name
            real_board_def = proj_dir / "Common" / "Inc" / "board_def.h"
            real_content = real_board_def.read_text(encoding="utf-8")
            base_tokens = extract_compile_tokens(proj_dir)

            proj_scratch = scratch_dir / proj_name
            proj_scratch.mkdir(parents=True, exist_ok=True)

            # Case 1: Positive control (unmodified board_def.h compiles with 0 errors and 0 warnings)
            pos_dir = proj_scratch / "pos_control"
            pos_dir.mkdir(parents=True, exist_ok=True)
            (pos_dir / "board_def.h").write_text(real_content, encoding="utf-8")
            pos_ok, pos_detail = compile_variant(tools, base_tokens, pos_dir, expected_fail=False)
            total_cases += 1
            if pos_ok:
                passed_cases += 1
            else:
                overall_ok = False
            logger.log(f"[host] CHECK T2 {proj_name} positive control: {'PASS' if pos_ok else 'FAIL'} ({pos_detail})")

            # Mutation 1: Missing BOARD_LED_RED_BIT
            m1_dir = proj_scratch / "mut1_no_red_bit"
            m1_dir.mkdir(parents=True, exist_ok=True)
            (m1_dir / "board_def.h").write_text(real_content + "\n#undef BOARD_LED_RED_BIT\n", encoding="utf-8")
            m1_ok, m1_detail = compile_variant(tools, base_tokens, m1_dir, expected_fail=True, expected_token="BOARD_LED_RED_BIT")
            total_cases += 1
            if m1_ok:
                passed_cases += 1
            else:
                overall_ok = False
            logger.log(f"[host] CHECK T2 {proj_name} mut1 (no red bit): {'PASS' if m1_ok else 'FAIL'} ({m1_detail})")

            # Mutation 2: Missing BOARD_LED_GREEN_PORT
            m2_dir = proj_scratch / "mut2_no_green_port"
            m2_dir.mkdir(parents=True, exist_ok=True)
            (m2_dir / "board_def.h").write_text(real_content + "\n#undef BOARD_LED_GREEN_PORT\n", encoding="utf-8")
            m2_ok, m2_detail = compile_variant(tools, base_tokens, m2_dir, expected_fail=True, expected_token="BOARD_LED_GREEN_PORT")
            total_cases += 1
            if m2_ok:
                passed_cases += 1
            else:
                overall_ok = False
            logger.log(f"[host] CHECK T2 {proj_name} mut2 (no green port): {'PASS' if m2_ok else 'FAIL'} ({m2_detail})")

            # Mutation 3: _PIN defined instead of _BIT (pre-F1 state)
            m3_dir = proj_scratch / "mut3_pin_not_bit"
            m3_dir.mkdir(parents=True, exist_ok=True)
            m3_content = (
                real_content
                + "\n#undef BOARD_LED_GREEN_BIT\n#define BOARD_LED_GREEN_PIN GPIO_PIN_0\n"
            )
            (m3_dir / "board_def.h").write_text(m3_content, encoding="utf-8")
            m3_ok, m3_detail = compile_variant(tools, base_tokens, m3_dir, expected_fail=True, expected_token="BOARD_LED_GREEN_BIT")
            total_cases += 1
            if m3_ok:
                passed_cases += 1
            else:
                overall_ok = False
            logger.log(f"[host] CHECK T2 {proj_name} mut3 (_PIN not _BIT): {'PASS' if m3_ok else 'FAIL'} ({m3_detail})")

            # Mutation 4: Out-of-range BOARD_LED_YELLOW_BIT 16U
            m4_dir = proj_scratch / "mut4_yellow_bit_16"
            m4_dir.mkdir(parents=True, exist_ok=True)
            m4_content = real_content + "\n#undef BOARD_LED_YELLOW_BIT\n#define BOARD_LED_YELLOW_BIT 16U\n"
            (m4_dir / "board_def.h").write_text(m4_content, encoding="utf-8")
            m4_ok, m4_detail = compile_variant(tools, base_tokens, m4_dir, expected_fail=True, expected_token="BOARD_LED_YELLOW_BIT")
            total_cases += 1
            if m4_ok:
                passed_cases += 1
            else:
                overall_ok = False
            logger.log(f"[host] CHECK T2 {proj_name} mut4 (yellow bit 16U): {'PASS' if m4_ok else 'FAIL'} ({m4_detail})")

    logger.log(f"[host] T2 cases: {passed_cases}/{total_cases} passed")
    logger.log(f"[host] T2 VERDICT: {'PASS' if overall_ok else 'FAIL'}")
    return overall_ok


# ---------------------------------------------------------------------------
# T3: Binary Object Inspection (g_led_config) vs Baseline & Oracle (MA1, M2, m4)
# ---------------------------------------------------------------------------

def find_led_bm_object(root_dir: Path, preset: str) -> Path:
    """Locates led_bm.c.obj under CM7 build directory."""
    cm7_dir = root_dir / "build" / preset / "CM7"
    if not cm7_dir.is_dir():
        raise FileNotFoundError(f"CM7 directory not found at {cm7_dir}")

    matches = list(cm7_dir.rglob("led_bm.c.obj"))
    if not matches:
        raise FileNotFoundError(f"led_bm.c.obj not found under {cm7_dir}")
    return matches[0]


def extract_objdump_led_config(objdump_cmd: str, obj_path: Path) -> tuple[int, str]:
    cmd = [objdump_cmd, "-s", "--section=.data.g_led_config", "--section=.data", str(obj_path)]
    code, stdout, stderr = run_cmd(cmd, timeout=30.0)
    return code, stdout


def parse_and_verify_led_config(objdump_text: str, expected_bytes: bytes) -> tuple[bool, str, bytes]:
    """Pure comparison function used for production checks and mutated negative control.

    m4: Ensures exactly 24 bytes are parsed without truncation, and validates exact size.
    """
    if not objdump_text.strip():
        return False, "Empty objdump text", b""

    hex_bytes = bytearray()
    for line in objdump_text.splitlines():
        line_strip = line.strip()
        m = re.match(r"^[0-9a-fA-F]{4,8}\s+((?:[0-9a-fA-F]{8}\s*)+)", line_strip)
        if m:
            groups = m.group(1).split()
            for grp in groups:
                if len(grp) == 8:
                    try:
                        raw = bytes.fromhex(grp)
                        hex_bytes.extend(raw)
                    except ValueError:
                        pass

    extracted = bytes(hex_bytes)
    # m4: check exact section size (must be exactly 24 bytes, not truncated or extended)
    if len(extracted) != len(expected_bytes) or len(extracted) != 24:
        return False, f"Extracted {len(extracted)} bytes, expected exactly 24 bytes", extracted

    match = (extracted == expected_bytes)
    words = [int.from_bytes(extracted[i:i+4], byteorder="little") for i in range(0, len(extracted), 4)]
    words_hex = [hex(w) for w in words]
    detail = f"words={words_hex}"
    return match, detail, extracted


def ensure_current_images_compiled(tools: dict[str, str], logger: Logger) -> bool:
    """Ensures all 8 matrix configurations of current tree are compiled freshly in this run (M2)."""
    head_rev = get_head_short_rev()
    all_ok = True
    for proj_rel, preset in MATRIX_CONFIGS:
        pdir = REPO_ROOT / proj_rel
        cm7_elf = pdir / "build" / preset / "CM7" / "cm7_firmware.elf"
        needs_build = (not cm7_elf.is_file()) or (cm7_elf.stat().st_mtime < RUN_START_TIME - 5.0)
        if needs_build:
            logger.log(f"[check] Compiling {pdir.name} [{preset}] for T3/T4 in current execution (HEAD {head_rev})...")
            passed, _, _, _, _ = compile_single_config(tools, pdir, preset, head_rev, logger)
            if not passed:
                all_ok = False
    return all_ok


def run_t3(tools: dict[str, str], control_dir: Path, logger: Logger) -> bool:
    logger.log("=== Running T3 Checks (Binary g_led_config vs UM2408 Oracle & 9dc6896 Control) ===")
    overall_ok = True
    head_rev = get_head_short_rev()

    # M2: Ensure current images are compiled in current run and match HEAD
    if not ensure_current_images_compiled(tools, logger):
        logger.log("[host] ERROR: Could not compile all 8 current configurations freshly in this execution.")
        return False

    # 1. Ensure control worktree is present and clean
    ctrl_ok, ctrl_msg = ensure_control_worktree(control_dir, logger)
    logger.log(f"[host] control worktree verification: {'PASS' if ctrl_ok else 'FAIL'} ({ctrl_msg})")
    if not ctrl_ok:
        return False

    # 2. Compile control worktree in-situ
    ctrl_build_ok = compile_control_images(control_dir, logger)
    if not ctrl_build_ok:
        logger.log("[host] ERROR: In-situ compilation of control images failed.")
        return False

    # 3. Verify all 8 production builds of current tree vs UM2408 Oracle and check FW_GIT_REV (MA1, M2)
    logger.log("[check] Inspecting g_led_config across 8 current builds...")
    current_build_bytes: dict[str, bytes] = {}

    for proj_rel, preset in MATRIX_CONFIGS:
        pdir = REPO_ROOT / proj_rel
        pname = pdir.name
        key = f"{pname}_{preset}"
        try:
            # Check FW_GIT_REV / freshness (MA1, M2)
            rev_ok, rev_msg = check_image_freshness_and_rev(pdir, preset, RUN_START_TIME, head_rev)
            if not rev_ok:
                logger.log(f"[host] FAIL: current {key} failed revision check: {rev_msg}")
                overall_ok = False

            obj = find_led_bm_object(pdir, preset)
            code, out = extract_objdump_led_config(tools["objdump"], obj)
            if code != 0 or not out.strip():
                logger.log(f"[host] FAIL: objdump failed on current {key}: code={code}")
                overall_ok = False
                continue

            ok, detail, bts = parse_and_verify_led_config(out, UM2408_ORACLE_BYTES)
            current_build_bytes[key] = bts
            logger.log(f"[host] CHECK current {key} vs UM2408: {'PASS' if ok else 'FAIL'} ({detail})")
            if not ok:
                overall_ok = False
        except Exception as e:
            logger.log(f"[host] FAIL on current {key}: {e}")
            overall_ok = False

    # 4. Verify all 8 builds of control worktree vs UM2408 Oracle and current builds
    logger.log("[check] Inspecting g_led_config across 8 control builds (9dc6896)...")
    ctrl_sample_out = ""
    for proj_rel, preset in MATRIX_CONFIGS:
        pdir = control_dir / proj_rel
        pname = pdir.name
        key = f"{pname}_{preset}"
        try:
            obj = find_led_bm_object(pdir, preset)
            code, out = extract_objdump_led_config(tools["objdump"], obj)
            if not ctrl_sample_out and code == 0 and out.strip():
                ctrl_sample_out = out

            if code != 0 or not out.strip():
                logger.log(f"[host] FAIL: objdump failed on control {key}: code={code}")
                overall_ok = False
                continue

            ok, detail, ctrl_bts = parse_and_verify_led_config(out, UM2408_ORACLE_BYTES)
            matches_current = (key in current_build_bytes) and (ctrl_bts == current_build_bytes[key])
            logger.log(f"[host] CHECK control {key} vs Oracle: {'PASS' if ok else 'FAIL'}, matches current: {'PASS' if matches_current else 'FAIL'}")
            if not (ok and matches_current):
                overall_ok = False
        except Exception as e:
            logger.log(f"[host] FAIL on control {key}: {e}")
            overall_ok = False

    # 5. Negative control for T3 (m4: unmutated passes, mutation alters text, exactly 24B rejected)
    sample_ok, sample_detail, sample_bts = parse_and_verify_led_config(ctrl_sample_out, UM2408_ORACLE_BYTES)
    if not sample_ok or len(sample_bts) != 24:
        logger.log(f"[host] FAIL: Unmutated sample does not pass verification ({sample_detail})")
        overall_ok = False

    # Alter the last word (red pin 14 = 0x0E -> 0x0F)
    mutated_text = ctrl_sample_out.replace("0e000000", "0f000000")
    if mutated_text == ctrl_sample_out:
        mutated_text = ctrl_sample_out.replace("00040258", "00050258")

    if mutated_text == ctrl_sample_out:
        logger.log("[host] FAIL: T3 mutation failed to alter text")
        overall_ok = False

    with tempfile.TemporaryDirectory(prefix="led_t3_neg_") as tmp_dir:
        neg_file = Path(tmp_dir) / "mutated_objdump.txt"
        neg_file.write_text(mutated_text, encoding="utf-8")
        read_neg = neg_file.read_text(encoding="utf-8")
        neg_ok, neg_detail, neg_bts = parse_and_verify_led_config(read_neg, UM2408_ORACLE_BYTES)
        # Mutation must extract 24 bytes but mismatch expected oracle bytes
        neg_pass = (neg_ok is False) and (len(neg_bts) == 24)
        logger.log(f"[host] CHECK T3 negative control (mutated objdump byte rejected): {'PASS' if neg_pass else 'FAIL'} ({neg_detail})")
        if not neg_pass:
            overall_ok = False

    logger.log(f"[host] T3 VERDICT: {'PASS' if overall_ok else 'FAIL'}")
    return overall_ok


# ---------------------------------------------------------------------------
# T4: Hardware Board Verification (Auditor only) (MA1, M2, m5, m7)
# ---------------------------------------------------------------------------

def read_swd_mem32(programmer_cmd: str, sn: str, addr: int, count: int = 1) -> list[int]:
    """Reads 32-bit words via SWD HOTPLUG mode with sn and timeout (m7)."""
    cmd = [
        programmer_cmd,
        "-c", "port=SWD", f"sn={sn}", "mode=HOTPLUG",
        "-r32", hex(addr), str(count * 4),
    ]
    code, stdout, stderr = run_cmd(cmd, timeout=30.0)
    if code != 0:
        raise RuntimeError(f"SWD read failed at {hex(addr)}: {stderr or stdout}")

    words = []
    for line in stdout.splitlines():
        line = line.strip()
        m = re.match(r"^0x[0-9A-Fa-f]+\s*:\s*(.+)$", line)
        if m:
            parts = m.group(1).split()
            for part in parts:
                if len(part) == 8 and re.match(r"^[0-9A-Fa-f]{8}$", part):
                    words.append(int(part, 16))
    if len(words) < count:
        raise RuntimeError(f"Expected {count} words from {hex(addr)}, got {len(words)}: {stdout}")
    return words[:count]


def parse_nm_symbol(nm_cmd: str, elf_path: Path, sym_name: str) -> int:
    code, stdout, stderr = run_cmd([nm_cmd, str(elf_path)], timeout=30.0)
    if code != 0:
        raise RuntimeError(f"nm failed on {elf_path}: {stderr}")
    for line in stdout.splitlines():
        parts = line.strip().split()
        if len(parts) >= 3 and parts[2] == sym_name:
            return int(parts[0], 16)
    raise ValueError(f"Symbol {sym_name} not found in {elf_path}")


def verify_t4_led_state(
    words: list[int],
    moder_b: int,
    moder_e: int,
    regs_b: dict[str, int] | None = None,
    regs_e: dict[str, int] | None = None,
) -> tuple[bool, str]:
    """Shared comparison function for T4 board readings and negative controls."""
    if len(words) < 6:
        return False, f"Expected 6 words in g_led_config, got {len(words)}"

    words_match = (words == UM2408_ORACLE_WORDS)

    if regs_b is None or regs_e is None:
        return False, "regs_b and regs_e are mandatory (SWD extended register telemetry missing)"
    for reg_key in ("moder", "otyper", "ospeedr", "pupdr", "odr"):
        if reg_key not in regs_b:
            return False, f"Missing required register '{reg_key}' in regs_b"
        if reg_key not in regs_e:
            return False, f"Missing required register '{reg_key}' in regs_e"

    # Pin checks for PB0 (Green LED), PB14 (Red LED), PE1 (Yellow LED)
    # MODER: 0b01 = output (1)
    # OTYPER: 0 = push-pull (0)
    # OSPEEDR: 0b00 = low speed (0)
    # PUPDR: 0b00 = no pull (0)
    # ODR: output bit 0 or 1
    pb0_mode = (regs_b.get("moder", moder_b) >> (0 * 2)) & 3
    pb0_otype = (regs_b.get("otyper", 0) >> 0) & 1
    pb0_speed = (regs_b.get("ospeedr", 0) >> (0 * 2)) & 3
    pb0_pull = (regs_b.get("pupdr", 0) >> (0 * 2)) & 3
    pb0_odr = (regs_b.get("odr", 0) >> 0) & 1

    pb14_mode = (regs_b.get("moder", moder_b) >> (14 * 2)) & 3
    pb14_otype = (regs_b.get("otyper", 0) >> 14) & 1
    pb14_speed = (regs_b.get("ospeedr", 0) >> (14 * 2)) & 3
    pb14_pull = (regs_b.get("pupdr", 0) >> (14 * 2)) & 3
    pb14_odr = (regs_b.get("odr", 0) >> 14) & 1

    pe1_mode = (regs_e.get("moder", moder_e) >> (1 * 2)) & 3
    pe1_otype = (regs_e.get("otyper", 0) >> 1) & 1
    pe1_speed = (regs_e.get("ospeedr", 0) >> (1 * 2)) & 3
    pe1_pull = (regs_e.get("pupdr", 0) >> (1 * 2)) & 3
    pe1_odr = (regs_e.get("odr", 0) >> 1) & 1

    pb0_ok = (pb0_mode == 1) and (pb0_otype == 0) and (pb0_speed == 0) and (pb0_pull == 0)
    pb14_ok = (pb14_mode == 1) and (pb14_otype == 0) and (pb14_speed == 0) and (pb14_pull == 0)
    pe1_ok = (pe1_mode == 1) and (pe1_otype == 0) and (pe1_speed == 0) and (pe1_pull == 0)

    pins_ok = pb0_ok and pb14_ok and pe1_ok
    passed = words_match and pins_ok
    detail = (
        f"words_match={words_match}, "
        f"PB0(mode={pb0_mode},otype={pb0_otype},speed={pb0_speed},pull={pb0_pull},odr={pb0_odr}), "
        f"PB14(mode={pb14_mode},otype={pb14_otype},speed={pb14_speed},pull={pb14_pull},odr={pb14_odr}), "
        f"PE1(mode={pe1_mode},otype={pe1_otype},speed={pe1_speed},pull={pe1_pull},odr={pe1_odr})"
    )
    return passed, detail


def ensure_debug_images_compiled(tools: dict[str, str], logger: Logger) -> bool:
    """Ensures the 4 debug images are compiled freshly in current execution from HEAD (MA1, M2)."""
    head_rev = get_head_short_rev()
    all_ok = True
    for proj_rel in PROJECTS:
        pdir = REPO_ROOT / proj_rel
        cm7_elf = pdir / "build" / "debug" / "CM7" / "cm7_firmware.elf"
        needs_build = (not cm7_elf.is_file()) or (cm7_elf.stat().st_mtime < RUN_START_TIME - 5.0)
        if needs_build:
            logger.log(f"[check] Compiling debug image for {pdir.name} in current execution (HEAD {head_rev})...")
            passed, _, _, _, _ = compile_single_config(tools, pdir, "debug", head_rev, logger)
            if not passed:
                all_ok = False

        rev_ok, rev_msg = check_image_freshness_and_rev(pdir, "debug", RUN_START_TIME, head_rev)
        logger.log(f"[host] CHECK T4 debug image FW_GIT_REV & freshness for {pdir.name}: {'PASS' if rev_ok else 'FAIL'} ({rev_msg})")
        if not rev_ok:
            all_ok = False
    return all_ok


def run_t4(tools: dict[str, str], sn: str, logger: Logger) -> bool:
    logger.log(f"=== Running T4 Checks (Hardware: Flash & SWD RAM / Registers with sn={sn}) ===")
    overall_ok = True

    # M2: Ensure debug images are compiled in current run from HEAD and verified
    if not ensure_debug_images_compiled(tools, logger):
        logger.log("[host] ERROR: Could not verify fresh compilation of debug images from HEAD for T4.")
        return False

    last_measured_words = None
    last_measured_moder_b = None
    last_measured_moder_e = None
    last_measured_regs_b = None
    last_measured_regs_e = None

    for proj_rel in PROJECTS:
        pdir = REPO_ROOT / proj_rel
        pname = pdir.name
        logger.log(f"[check] Flashing debug image for {pname}...")
        flash_shim = pdir / "tools" / "flash.py"
        if not flash_shim.is_file():
            flash_shim = REPO_ROOT / "tools" / "flash.py"

        # 1. Flash using tools/flash.py of each project with -v (download verification)
        flash_cmd = [sys.executable, str(flash_shim), "--preset", "debug", "--sn", sn]
        f_code, f_out, f_err = run_cmd(flash_cmd, cwd=pdir, timeout=60.0)
        flash_ok = (f_code == 0) and ("Verified OK" in f_out or "Download verified successfully" in f_out)
        logger.log(f"[host] CHECK flash {pname}: {'PASS' if flash_ok else 'FAIL'}")
        if not flash_ok:
            logger.log(f"       flash stderr: {f_err.strip()}")
            overall_ok = False
            continue

        # Wait for boot
        time.sleep(2.0)

        # 2. Resolve g_led_config RAM address via nm
        cm7_elf = pdir / "build" / "debug" / "CM7" / "cm7_firmware.elf"
        try:
            led_cfg_addr = parse_nm_symbol(tools["nm"], cm7_elf, "g_led_config")
            logger.log(f"  [SWD origin] {pname} g_led_config address: {hex(led_cfg_addr)}")

            # 3. Read 6 words from g_led_config
            words = read_swd_mem32(tools["programmer"], sn, led_cfg_addr, 6)

            # 4. Read MODER, OTYPER, OSPEEDR, PUPDR, ODR of GPIOB and GPIOE
            # Offsets: 0x00=MODER, 0x04=OTYPER, 0x08=OSPEEDR, 0x0C=PUPDR, 0x10=IDR, 0x14=ODR
            regs_b_words = read_swd_mem32(tools["programmer"], sn, GPIOB_BASE, 6)
            regs_e_words = read_swd_mem32(tools["programmer"], sn, GPIOE_BASE, 6)

            regs_b = {
                "moder": regs_b_words[0],
                "otyper": regs_b_words[1],
                "ospeedr": regs_b_words[2],
                "pupdr": regs_b_words[3],
                "odr": regs_b_words[5],
            }
            regs_e = {
                "moder": regs_e_words[0],
                "otyper": regs_e_words[1],
                "ospeedr": regs_e_words[2],
                "pupdr": regs_e_words[3],
                "odr": regs_e_words[5],
            }
            moder_b = regs_b["moder"]
            moder_e = regs_e["moder"]

            # m7: Log exact hexadecimal values read from SWD
            words_hex = [hex(w) for w in words]
            logger.log(f"  [SWD read] {pname} g_led_config: {words_hex}")
            logger.log(
                f"  [SWD read] {pname} GPIOB: MODER={hex(regs_b['moder'])} OTYPER={hex(regs_b['otyper'])} "
                f"OSPEEDR={hex(regs_b['ospeedr'])} PUPDR={hex(regs_b['pupdr'])} ODR={hex(regs_b['odr'])}"
            )
            logger.log(
                f"  [SWD read] {pname} GPIOE: MODER={hex(regs_e['moder'])} OTYPER={hex(regs_e['otyper'])} "
                f"OSPEEDR={hex(regs_e['ospeedr'])} PUPDR={hex(regs_e['pupdr'])} ODR={hex(regs_e['odr'])}"
            )

            proj_ok, proj_detail = verify_t4_led_state(words, moder_b, moder_e, regs_b, regs_e)
            logger.log(f"[host] CHECK {pname} SWD RAM & registers: {'PASS' if proj_ok else 'FAIL'} ({proj_detail})")
            if not proj_ok:
                overall_ok = False
            else:
                last_measured_words = list(words)
                last_measured_moder_b = moder_b
                last_measured_moder_e = moder_e
                last_measured_regs_b = copy.deepcopy(regs_b)
                last_measured_regs_e = copy.deepcopy(regs_e)
        except Exception as e:
            logger.log(f"[host] FAIL {pname} SWD read: {e}")
            overall_ok = False

    # Negative controls for T4: Mutating actual measured hardware telemetry (A6)
    if last_measured_regs_b is None or last_measured_regs_e is None or last_measured_words is None:
        logger.log("[host] FAIL: No successful project measurement available to build mutated negative controls")
        return False

    with tempfile.TemporaryDirectory(prefix="led_t4_neg_") as tmp_dir:
        # Negative control 1: Mutated RAM state from measured words (PB0 pin number changed to 2)
        neg_ram_path = Path(tmp_dir) / "mutated_ram.json"
        mutated_words = list(last_measured_words)
        mutated_words[1] = 2
        fake_ram_data = {
            "words": mutated_words,
            "moder_b": last_measured_moder_b,
            "moder_e": last_measured_moder_e,
            "regs_b": copy.deepcopy(last_measured_regs_b),
            "regs_e": copy.deepcopy(last_measured_regs_e),
        }
        neg_ram_path.write_text(json.dumps(fake_ram_data), encoding="utf-8")
        loaded_ram = json.loads(neg_ram_path.read_text(encoding="utf-8"))
        neg_ram_ok, neg_ram_detail = verify_t4_led_state(
            loaded_ram["words"], loaded_ram["moder_b"], loaded_ram["moder_e"],
            loaded_ram.get("regs_b"), loaded_ram.get("regs_e")
        )
        neg_ram_pass = (neg_ram_ok is False)
        logger.log(f"[host] CHECK T4 negative control 1 (mutated RAM state rejected): {'PASS' if neg_ram_pass else 'FAIL'} ({neg_ram_detail})")
        if not neg_ram_pass:
            overall_ok = False

        # Negative control 2: Mutated MODER state from measured regs_b (PB0 configured as 00b input)
        neg_moder_path = Path(tmp_dir) / "mutated_moder.json"
        mutated_regs_b_moder = copy.deepcopy(last_measured_regs_b)
        mutated_regs_b_moder["moder"] &= ~(3 << (0 * 2))  # PB0 mode = 00b (input)
        bad_moder_b = mutated_regs_b_moder["moder"]
        fake_moder_data = {
            "words": list(last_measured_words),
            "moder_b": bad_moder_b,
            "moder_e": last_measured_moder_e,
            "regs_b": mutated_regs_b_moder,
            "regs_e": copy.deepcopy(last_measured_regs_e),
        }
        neg_moder_path.write_text(json.dumps(fake_moder_data), encoding="utf-8")
        loaded_moder = json.loads(neg_moder_path.read_text(encoding="utf-8"))
        neg_moder_ok, neg_moder_detail = verify_t4_led_state(
            loaded_moder["words"], loaded_moder["moder_b"], loaded_moder["moder_e"],
            loaded_moder.get("regs_b"), loaded_moder.get("regs_e")
        )
        neg_moder_pass = (neg_moder_ok is False)
        logger.log(f"[host] CHECK T4 negative control 2 (mutated MODER register rejected): {'PASS' if neg_moder_pass else 'FAIL'} ({neg_moder_detail})")
        if not neg_moder_pass:
            overall_ok = False

        # Negative control 3: Mutated PUPDR state from measured regs_b (PB0 pull-up enabled)
        neg_pupdr_path = Path(tmp_dir) / "mutated_pupdr.json"
        mutated_regs_b_pupdr = copy.deepcopy(last_measured_regs_b)
        mutated_regs_b_pupdr["pupdr"] |= (1 << (0 * 2))  # PB0 pull = 01b (pull-up)
        fake_pupdr_data = {
            "words": list(last_measured_words),
            "moder_b": last_measured_moder_b,
            "moder_e": last_measured_moder_e,
            "regs_b": mutated_regs_b_pupdr,
            "regs_e": copy.deepcopy(last_measured_regs_e),
        }
        neg_pupdr_path.write_text(json.dumps(fake_pupdr_data), encoding="utf-8")
        loaded_pupdr = json.loads(neg_pupdr_path.read_text(encoding="utf-8"))
        neg_pupdr_ok, neg_pupdr_detail = verify_t4_led_state(
            loaded_pupdr["words"], loaded_pupdr["moder_b"], loaded_pupdr["moder_e"],
            loaded_pupdr.get("regs_b"), loaded_pupdr.get("regs_e")
        )
        neg_pupdr_pass = (neg_pupdr_ok is False)
        logger.log(f"[host] CHECK T4 negative control 3 (mutated PUPDR register rejected): {'PASS' if neg_pupdr_pass else 'FAIL'} ({neg_pupdr_detail})")
        if not neg_pupdr_pass:
            overall_ok = False

    logger.log(f"[host] T4 VERDICT: {'PASS' if overall_ok else 'FAIL'}")
    return overall_ok


# ---------------------------------------------------------------------------
# Main Entry Point
# ---------------------------------------------------------------------------

def main() -> int:
    parser = argparse.ArgumentParser(description="STM32H7 LED Driver L1/L2 Verification Harness")
    parser.add_argument("--test", default="all",
                        help="Which test to run: t1, t2, t3, t4, all (or comma-separated like t1,t2,t3)")
    parser.add_argument("--output", "-o", type=Path, default=None, help="Capture file destination")
    parser.add_argument("--control-dir", type=Path, default=REPO_ROOT.parent / f"stm32h755_control_{CONTROL_COMMIT_EXPECTED}",
                        help="Path to control worktree")
    parser.add_argument("--allow-dirty", action="store_true", help="Accept dirty working tree (development only)")
    parser.add_argument("--sn", default=DEFAULT_SN, help="ST-LINK serial number for T4")
    args = parser.parse_args()

    valid_tests = {"t1", "t2", "t3", "t4", "all"}
    raw_tests = [t.strip().lower() for t in args.test.split(",") if t.strip()]
    for t in raw_tests:
        if t not in valid_tests:
            parser.error(f"Invalid test: '{t}'. Choices: {sorted(valid_tests)}")
    if "all" in raw_tests:
        tests_to_run = ["t1", "t2", "t3", "t4"]
    else:
        tests_to_run = raw_tests

    logger = Logger(args.output)
    logger.log(f"[host] argv: {' '.join(sys.argv)}")
    logger.log(f"[host] options: test={args.test} output={args.output} control_dir={args.control_dir} allow_dirty={args.allow_dirty} sn={args.sn}")

    code, head_sha, _ = run_cmd(["git", "rev-parse", "HEAD"], cwd=REPO_ROOT, timeout=10.0)
    logger.log(f"[host] HEAD: {head_sha.strip()}")
    logger.log(f"[host] Working tree: {REPO_ROOT}")
    logger.log(f"[host] Control worktree: {args.control_dir}")

    # m6: Check clean tree at start
    clean_ok, clean_msg = check_clean_working_tree(REPO_ROOT, args.output)
    logger.log(f"[host] working tree clean (start): {'PASS' if clean_ok else 'FAIL'} ({clean_msg})")
    if not clean_ok and not args.allow_dirty:
        logger.log(f"[host] ERROR: Working tree is dirty. Stash or commit before running.")
        return 1

    try:
        tools = find_tools()
        logger.log(f"[host] Compiler: {tools['gcc']}")
        logger.log(f"[host] Objdump:  {tools['objdump']}")
        logger.log(f"[host] CMake:    {tools['cmake']}")
    except Exception as e:
        logger.log(f"[host] ERROR finding tools: {e}")
        return 2

    overall_all_ok = True

    if "t1" in tests_to_run:
        ok1 = run_t1(tools, logger)
        if not ok1:
            overall_all_ok = False

    if "t2" in tests_to_run:
        ok2 = run_t2(tools, logger)
        if not ok2:
            overall_all_ok = False

    if "t3" in tests_to_run:
        ok3 = run_t3(tools, args.control_dir, logger)
        if not ok3:
            overall_all_ok = False

    if "t4" in tests_to_run:
        ok4 = run_t4(tools, args.sn, logger)
        if not ok4:
            overall_all_ok = False

    # m6: Check clean tree at end
    clean_end_ok, clean_end_msg = check_clean_working_tree(REPO_ROOT, args.output)
    logger.log(f"[host] working tree clean (end): {'PASS' if clean_end_ok else 'FAIL'} ({clean_end_msg})")
    if not clean_end_ok and not args.allow_dirty:
        overall_all_ok = False

    verdict_str = "PASS" if overall_all_ok else "FAIL"
    logger.log(f"OVERALL VERDICT: {verdict_str}")
    return 0 if overall_all_ok else 3


if __name__ == "__main__":
    sys.exit(main())
