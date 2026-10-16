#!/usr/bin/env python3
"""Verification harness for STM32H7 LED Driver L1/L2 phase.

Covers tests T1, T2, T3 (off-board) and T4 (on-board SWD verification):
  - T1: Multi-project CI matrix build (8/8) via build_all.ps1 -Clean,
        plus 9 selftest presets linking led_bm with --fresh and --clean-first.
        Oracle: 0 errors and 0 compiler warnings.
        Negative control: synthetic warning rejected by warning parser.
  - T2: Compile led_bm.c with -fsyntax-only against mutated board_def.h copies
        in temporary directories outside the repository:
        1 positive control and 4 failing mutations per project (4 x 4 = 16 mutations).
        Oracle: exit code and specific #error message matching missing/out-of-range macro.
  - T3: Initial content of g_led_config in .data of CM7 led_bm.c.obj across all 8 production builds
        contrasted against UM2408 hardware oracle (PB0, PE1, PB14) and against control commit 9dc6896
        compiled in-situ during execution.
        Negative control: mutated objdump output rejected by comparison function.
  - T4: Hardware board test (executed by auditor): flashes 4 projects with tools/flash.py,
        reads g_led_config in RAM and MODER registers via SWD HOTPLUG -r32, checks output modes.
        Negative control: mutated RAM/register state rejected by verification function.

Usage:
    py tools/led_l1l2_checks.py [--test {t1,t2,t3,t4,all}] [--output <file>] [--control-dir <path>] [--allow-dirty] [--sn <sn>]

Standard library only.
"""

from __future__ import annotations

import argparse
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

PROJECTS = [
    "03_firmware_templates/prj_01_template_dualcore",
    "02_firmware_projects/prj_02_eth_lwip_dualcore",
    "02_firmware_projects/prj_03_adc_dma_eth",
    "02_firmware_projects/prj_04_adc_dma_bm_eth",
]

SELFTEST_PRESETS = {
    "03_firmware_templates/prj_01_template_dualcore": [
        "selftest-gpio",
        "selftest-tim",
        "selftest-spi",
        "selftest-i2c",
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


def run_cmd(args: list[str], cwd: Path | None = None) -> tuple[int, str, str]:
    res = subprocess.run(args, cwd=str(cwd) if cwd else None, capture_output=True, text=True)
    return res.returncode, res.stdout, res.stderr


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
        ])
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


def check_clean_working_tree(repo_root: Path, output_file: Path | None) -> tuple[bool, str]:
    """Verifies git status --porcelain -u is clean, ignoring only the output file."""
    code, status_out, err = run_cmd(["git", "status", "--porcelain", "-u"], cwd=repo_root)
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
            if untracked_rel.replace("\\", "/").startswith("01_shared_drivers/01_led/docs/led_l1l2_"):
                continue
        dirty_lines.append(line)

    if dirty_lines:
        return False, f"Dirty tree ({len(dirty_lines)} files): {dirty_lines}"
    return True, "Tree is clean"


def ensure_control_worktree(control_dir: Path, logger: Logger) -> tuple[bool, str]:
    """Ensures control worktree exists at CONTROL_COMMIT_EXPECTED and is clean."""
    if not control_dir.is_dir():
        logger.log(f"[control] Creating control worktree at {control_dir} (commit {CONTROL_COMMIT_EXPECTED})...")
        code, out, err = run_cmd(["git", "worktree", "add", str(control_dir), CONTROL_COMMIT_EXPECTED], cwd=REPO_ROOT)
        if code != 0:
            return False, f"git worktree add failed ({code}): {err or out}"

    code, ctrl_rev, _ = run_cmd(["git", "rev-parse", "HEAD"], cwd=control_dir)
    ctrl_rev = ctrl_rev.strip()
    if code != 0 or not ctrl_rev.startswith(CONTROL_COMMIT_EXPECTED):
        return False, f"Control commit {ctrl_rev} does not match expected {CONTROL_COMMIT_EXPECTED}"

    code_ctrl_st, ctrl_st_out, _ = run_cmd(["git", "status", "--porcelain", "--ignore-submodules"], cwd=control_dir)
    if code_ctrl_st != 0:
        return False, f"git status failed on control worktree ({code_ctrl_st})"

    # Check for modified tracked files
    tracked_dirty = [ln for ln in ctrl_st_out.splitlines() if not ln.strip().startswith("??")]
    if tracked_dirty:
        return False, f"Control worktree has modified files: {tracked_dirty}"

    # Ensure STM32CubeH7 junction exists and is valid
    sentinel = control_dir / "02_firmware_projects" / "prj_eth_lwip_node" / "external" / "STM32CubeH7" / "Drivers" / "STM32H7xx_HAL_Driver" / "Src" / "stm32h7xx_hal_rcc.c"
    cube_junction = control_dir / "02_firmware_projects" / "prj_eth_lwip_node" / "external" / "STM32CubeH7"
    if not sentinel.exists():
        src_cube = REPO_ROOT / "02_firmware_projects" / "prj_eth_lwip_node" / "external" / "STM32CubeH7"
        if src_cube.exists():
            if cube_junction.exists():
                try:
                    cube_junction.rmdir()
                except Exception:
                    run_cmd(["cmd", "/c", "rmdir", "/s", "/q", str(cube_junction)])
            cube_junction.parent.mkdir(parents=True, exist_ok=True)
            logger.log(f"[control] Creating junction for STM32CubeH7 at {cube_junction}...")
            run_cmd(["cmd", "/c", "mklink", "/J", str(cube_junction), str(src_cube)])

    if not sentinel.exists():
        return False, f"STM32CubeH7 incomplete in control worktree (sentinel missing: {sentinel})"

    return True, f"Control worktree at {CONTROL_COMMIT_EXPECTED} ready"


def compile_control_images(control_dir: Path, logger: Logger) -> bool:
    """Compiles the 16 control images via build_all.ps1 -Clean in control worktree."""
    logger.log(f"[check] Compiling control images via build_all.ps1 -Clean in {control_dir}...")
    cmd = [
        "powershell", "-NoProfile", "-ExecutionPolicy", "Bypass",
        "-File", str(control_dir / "tools" / "build_all.ps1"), "-Clean"
    ]
    t_start = time.time()
    code, stdout, stderr = run_cmd(cmd, cwd=control_dir)
    duration = time.time() - t_start

    passed = (code == 0) and ("All 8 builds PASSED successfully" in stdout)
    logger.log(f"[build_all_control] Finished in {duration:.1f}s, exit code {code}")
    for line in stdout.splitlines():
        if "==>" in line or "PASSED" in line or "FAILED" in line:
            logger.log(f"[build_all_control] {line.strip()}")
    logger.log(f"[host] CHECK control build_all 8/8 (-Clean): {'PASS' if passed else 'FAIL'}")
    return passed


# ---------------------------------------------------------------------------
# T1: Build Matrix & Selftests
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


def run_t1(tools: dict[str, str], logger: Logger) -> bool:
    logger.log("=== Running T1 Checks (Build Matrix 8/8 & Selftest Presets) ===")
    overall_ok = True

    # 1. Production build matrix 8/8 via build_all.ps1 -Clean
    logger.log("[check] Running build_all.ps1 -Clean across 8 configurations...")
    cmd = [
        "powershell", "-NoProfile", "-ExecutionPolicy", "Bypass",
        "-File", str(REPO_ROOT / "tools" / "build_all.ps1"), "-Clean"
    ]
    t0 = time.time()
    code, stdout, stderr = run_cmd(cmd, cwd=REPO_ROOT)
    dur = time.time() - t0

    full_matrix_out = stdout + "\n" + stderr
    matrix_warnings = parse_compiler_warnings(full_matrix_out)
    matrix_pass = (code == 0) and ("All 8 builds PASSED successfully" in stdout) and (len(matrix_warnings) == 0)

    logger.log(f"[build_all] Finished in {dur:.1f}s, exit code {code}, warnings={len(matrix_warnings)}")
    for line in stdout.splitlines():
        if "==>" in line or "PASSED" in line or "FAILED" in line:
            logger.log(f"[build_all] {line.strip()}")
    logger.log(f"[host] CHECK T1 build_all 8/8 (-Clean, 0 warnings): {'PASS' if matrix_pass else 'FAIL'}")
    if not matrix_pass:
        overall_ok = False

    # 2. Selftest presets linking led_bm
    logger.log("[check] Checking selftest presets linking led_bm (--fresh and --clean-first)...")
    for proj_rel, presets in SELFTEST_PRESETS.items():
        proj_dir = REPO_ROOT / proj_rel
        for preset in presets:
            cfg_code, cfg_out, cfg_err = run_cmd([tools["cmake"], "--preset", preset, "--fresh"], cwd=proj_dir)
            if cfg_code != 0:
                logger.log(f"[host] FAIL: cmake configure --preset {preset} on {proj_dir.name} failed (code={cfg_code}): {cfg_err}")
                overall_ok = False
                continue

            bld_code, bld_out, bld_err = run_cmd([tools["cmake"], "--build", "--preset", preset, "--clean-first"], cwd=proj_dir)
            full_out = bld_out + "\n" + bld_err
            warnings = parse_compiler_warnings(full_out)
            objs = count_built_objects(full_out)

            preset_ok = (bld_code == 0) and (len(warnings) == 0) and (objs > 0)
            logger.log(f"[host] CHECK {proj_dir.name} [{preset}]: {'PASS' if preset_ok else 'FAIL'} "
                       f"(code={bld_code}, objs={objs}, warnings={len(warnings)})")
            if not preset_ok:
                overall_ok = False

    # 3. Negative control for warning counter using mutated build log in temporary dir
    with tempfile.TemporaryDirectory(prefix="led_t1_neg_") as tmp_dir:
        fake_log_path = Path(tmp_dir) / "synthetic_build.log"
        fake_content = (
            "Building C object CM7/CMakeFiles/cm7_firmware.dir/main.c.obj\n"
            "main.c:42:5: warning: unused variable 'synthetic_control' [-Wunused-variable]\n"
        )
        fake_log_path.write_text(fake_content, encoding="utf-8")
        read_back = fake_log_path.read_text(encoding="utf-8")
        neg_warnings = parse_compiler_warnings(read_back)
        neg_warn_ok = (len(neg_warnings) == 1) and ("unused variable" in neg_warnings[0])
        logger.log(f"[host] CHECK warning counter negative control (synthetic warning flagged): {'PASS' if neg_warn_ok else 'FAIL'}")
        if not neg_warn_ok:
            overall_ok = False

    logger.log(f"[host] T1 VERDICT: {'PASS' if overall_ok else 'FAIL'}")
    return overall_ok


# ---------------------------------------------------------------------------
# T2: Compile-Time #error & Range Checks
# ---------------------------------------------------------------------------

def extract_compile_tokens(project_dir: Path) -> list[str]:
    """Loads compile_commands.json and returns compiler flags for led_bm.c."""
    comp_json = project_dir / "build" / "debug" / "compile_commands.json"
    if not comp_json.is_file():
        raise FileNotFoundError(f"compile_commands.json missing at {comp_json}")

    with open(comp_json, "r", encoding="utf-8") as f:
        data = json.load(f)

    for entry in data:
        if "led_bm.c" in entry.get("file", ""):
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

    raise ValueError(f"No entry for led_bm.c in {comp_json}")


def compile_variant(
    tools: dict[str, str],
    base_tokens: list[str],
    work_dir: Path,
    board_def_dir: Path,
    expected_fail: bool,
    expected_token: Optional[str] = None
) -> tuple[bool, str]:
    """Shared pure function for positive check and negative mutations."""
    source_file = str(REPO_ROOT / "01_shared_drivers" / "01_led" / "led_bm.c")
    cmd = [tools["gcc"], f"-I{board_def_dir}"] + base_tokens + ["-fsyntax-only", source_file]
    proc = subprocess.run(cmd, cwd=str(work_dir), capture_output=True, text=True)

    if not expected_fail:
        passed = (proc.returncode == 0)
        detail = f"exit={proc.returncode}" if passed else f"exit={proc.returncode}: {proc.stderr.strip().splitlines()[-1] if proc.stderr else 'no error'}"
        return passed, detail
    else:
        err = proc.stderr
        has_error = (proc.returncode != 0) and ("#error" in err)
        has_token = (expected_token is None) or (expected_token in err)
        passed = has_error and has_token
        # Extract first #error line
        first_err = "no #error found"
        for line in err.splitlines():
            if "#error" in line:
                first_err = line.strip()
                break
        detail = f"exit={proc.returncode}, msg='{first_err}'"
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

            # Case 1: Positive control (unmodified board_def.h compiles with 0 errors)
            pos_dir = proj_scratch / "pos_control"
            pos_dir.mkdir(parents=True, exist_ok=True)
            (pos_dir / "board_def.h").write_text(real_content, encoding="utf-8")
            pos_ok, pos_detail = compile_variant(tools, base_tokens, proj_dir, pos_dir, expected_fail=False)
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
            m1_ok, m1_detail = compile_variant(tools, base_tokens, proj_dir, m1_dir, expected_fail=True, expected_token="BOARD_LED_RED_BIT")
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
            m2_ok, m2_detail = compile_variant(tools, base_tokens, proj_dir, m2_dir, expected_fail=True, expected_token="BOARD_LED_GREEN_PORT")
            total_cases += 1
            if m2_ok:
                passed_cases += 1
            else:
                overall_ok = False
            logger.log(f"[host] CHECK T2 {proj_name} mut2 (no green port): {'PASS' if m2_ok else 'FAIL'} ({m2_detail})")

            # Mutation 3: Prior state (_PIN masks in place of _BIT)
            m3_dir = proj_scratch / "mut3_pin_not_bit"
            m3_dir.mkdir(parents=True, exist_ok=True)
            m3_content = (
                real_content +
                "\n#undef BOARD_LED_GREEN_BIT\n#undef BOARD_LED_YELLOW_BIT\n#undef BOARD_LED_RED_BIT\n" +
                "#define BOARD_LED_GREEN_PIN GPIO_PIN_0\n" +
                "#define BOARD_LED_YELLOW_PIN GPIO_PIN_1\n" +
                "#define BOARD_LED_RED_PIN GPIO_PIN_14\n"
            )
            (m3_dir / "board_def.h").write_text(m3_content, encoding="utf-8")
            m3_ok, m3_detail = compile_variant(tools, base_tokens, proj_dir, m3_dir, expected_fail=True, expected_token="BOARD_LED_GREEN_BIT")
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
            m4_ok, m4_detail = compile_variant(tools, base_tokens, proj_dir, m4_dir, expected_fail=True, expected_token="BOARD_LED_YELLOW_BIT")
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
# T3: Binary Object Inspection (g_led_config) vs Baseline & Oracle
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
    proc = subprocess.run(cmd, capture_output=True, text=True)
    return proc.returncode, proc.stdout


def parse_and_verify_led_config(objdump_text: str, expected_bytes: bytes) -> tuple[bool, str, bytes]:
    """Pure comparison function used for production checks and mutated negative control."""
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

    extracted = bytes(hex_bytes[:len(expected_bytes)])
    if len(extracted) < len(expected_bytes):
        return False, f"Extracted only {len(extracted)} bytes, expected {len(expected_bytes)}", extracted

    match = (extracted == expected_bytes)
    words = [int.from_bytes(extracted[i:i+4], byteorder="little") for i in range(0, len(extracted), 4)]
    words_hex = [hex(w) for w in words]
    detail = f"words={words_hex}"
    return match, detail, extracted


def run_t3(tools: dict[str, str], control_dir: Path, logger: Logger) -> bool:
    logger.log("=== Running T3 Checks (Binary g_led_config vs UM2408 Oracle & 9dc6896 Control) ===")
    overall_ok = True

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

    # 3. Verify all 8 production builds of current tree vs UM2408 Oracle
    logger.log("[check] Inspecting g_led_config across 8 current builds...")
    current_build_bytes: dict[str, bytes] = {}

    for proj_rel in PROJECTS:
        pdir = REPO_ROOT / proj_rel
        pname = pdir.name
        for preset in ("debug", "release"):
            key = f"{pname}_{preset}"
            try:
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
    for proj_rel in PROJECTS:
        pdir = control_dir / proj_rel
        pname = pdir.name
        for preset in ("debug", "release"):
            key = f"{pname}_{preset}"
            try:
                obj = find_led_bm_object(pdir, preset)
                code, out = extract_objdump_led_config(tools["objdump"], obj)
                if not ctrl_sample_out:
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

    # 5. Negative control for T3: mutate a byte in objdump text in temp directory
    with tempfile.TemporaryDirectory(prefix="led_t3_neg_") as tmp_dir:
        neg_file = Path(tmp_dir) / "mutated_objdump.txt"
        # Alter the last word (red pin 14 = 0x0E -> 0x0F)
        mutated_text = ctrl_sample_out.replace("0e000000", "0f000000")
        if mutated_text == ctrl_sample_out:
            # Fallback mutation
            mutated_text = ctrl_sample_out.replace("00040258", "00050258")

        neg_file.write_text(mutated_text, encoding="utf-8")
        read_neg = neg_file.read_text(encoding="utf-8")
        neg_ok, neg_detail, _ = parse_and_verify_led_config(read_neg, UM2408_ORACLE_BYTES)
        # Mutation must be rejected
        neg_pass = (neg_ok is False)
        logger.log(f"[host] CHECK T3 negative control (mutated objdump byte rejected): {'PASS' if neg_pass else 'FAIL'} ({neg_detail})")
        if not neg_pass:
            overall_ok = False

    logger.log(f"[host] T3 VERDICT: {'PASS' if overall_ok else 'FAIL'}")
    return overall_ok


# ---------------------------------------------------------------------------
# T4: Hardware Board Verification (Auditor only)
# ---------------------------------------------------------------------------

def read_swd_mem32(programmer_cmd: str, sn: str, addr: int, count: int = 1) -> list[int]:
    """Reads 32-bit words via SWD HOTPLUG mode."""
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


def parse_nm_symbol(nm_cmd: str, elf_path: Path, sym_name: str) -> int:
    proc = subprocess.run([nm_cmd, str(elf_path)], capture_output=True, text=True)
    if proc.returncode != 0:
        raise RuntimeError(f"nm failed on {elf_path}: {proc.stderr}")
    for line in proc.stdout.splitlines():
        parts = line.strip().split()
        if len(parts) >= 3 and parts[2] == sym_name:
            return int(parts[0], 16)
    raise ValueError(f"Symbol {sym_name} not found in {elf_path}")


def verify_t4_led_state(words: list[int], moder_b: int, moder_e: int) -> tuple[bool, str]:
    """Shared comparison function for T4 board readings and negative control."""
    if len(words) < 6:
        return False, f"Expected 6 words in g_led_config, got {len(words)}"

    words_match = (words == UM2408_ORACLE_WORDS)

    # MODER output mode is 0b01
    pb0_mode = (moder_b >> (0 * 2)) & 3
    pb14_mode = (moder_b >> (14 * 2)) & 3
    pe1_mode = (moder_e >> (1 * 2)) & 3

    pins_ok = (pb0_mode == 1) and (pb14_mode == 1) and (pe1_mode == 1)
    passed = words_match and pins_ok
    detail = f"words_match={words_match}, PB0_mode={pb0_mode}, PB14_mode={pb14_mode}, PE1_mode={pe1_mode}"
    return passed, detail


def run_t4(tools: dict[str, str], sn: str, logger: Logger) -> bool:
    logger.log(f"=== Running T4 Checks (Hardware: Flash & SWD RAM / MODER with sn={sn}) ===")
    overall_ok = True

    for proj_rel in PROJECTS:
        pdir = REPO_ROOT / proj_rel
        pname = pdir.name
        logger.log(f"[check] Flashing debug image for {pname}...")
        flash_shim = pdir / "tools" / "flash.py"
        if not flash_shim.is_file():
            flash_shim = REPO_ROOT / "tools" / "flash.py"

        # 1. Flash using tools/flash.py of each project
        flash_cmd = [sys.executable, str(flash_shim), "--preset", "debug", "--sn", sn]
        f_code, f_out, f_err = run_cmd(flash_cmd, cwd=pdir)
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

            # 4. Read MODER of GPIOB and GPIOE
            moder_b = read_swd_mem32(tools["programmer"], sn, GPIOB_BASE, 1)[0]
            moder_e = read_swd_mem32(tools["programmer"], sn, GPIOE_BASE, 1)[0]

            proj_ok, proj_detail = verify_t4_led_state(words, moder_b, moder_e)
            logger.log(f"[host] CHECK {pname} SWD RAM & MODER: {'PASS' if proj_ok else 'FAIL'} ({proj_detail})")
            if not proj_ok:
                overall_ok = False
        except Exception as e:
            logger.log(f"[host] FAIL {pname} SWD read: {e}")
            overall_ok = False

    # Negative control for T4: mutated readings passed to verify_t4_led_state in temp copy
    with tempfile.TemporaryDirectory(prefix="led_t4_neg_") as tmp_dir:
        neg_json_path = Path(tmp_dir) / "mutated_t4.json"
        # Alter PB0 pin number to 2
        mutated_words = list(UM2408_ORACLE_WORDS)
        mutated_words[1] = 2
        fake_data = {"words": mutated_words, "moder_b": 0x10000001, "moder_e": 0x00000004}
        neg_json_path.write_text(json.dumps(fake_data), encoding="utf-8")
        loaded = json.loads(neg_json_path.read_text(encoding="utf-8"))
        neg_ok, neg_detail = verify_t4_led_state(loaded["words"], loaded["moder_b"], loaded["moder_e"])
        neg_pass = (neg_ok is False)
        logger.log(f"[host] CHECK T4 negative control (mutated RAM state rejected): {'PASS' if neg_pass else 'FAIL'} ({neg_detail})")
        if not neg_pass:
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
    logger.log(f"[host] argv: {shlex.join(sys.argv)}")
    logger.log(f"[host] options: test={args.test} output={args.output} control_dir={args.control_dir} allow_dirty={args.allow_dirty} sn={args.sn}")

    try:
        code_head, head_rev, _ = run_cmd(["git", "rev-parse", "HEAD"], cwd=REPO_ROOT)
        logger.log(f"[host] HEAD: {head_rev.strip()}")
        logger.log(f"[host] Working tree: {REPO_ROOT}")
        logger.log(f"[host] Control worktree: {args.control_dir}")

        tree_clean, tree_msg = check_clean_working_tree(REPO_ROOT, args.output)
        logger.log(f"[host] working tree clean: {'PASS' if tree_clean else 'DIRTY'} ({tree_msg})")
        if not tree_clean:
            if not args.allow_dirty:
                logger.log("[host] ERROR: Working tree is dirty. Aborting.")
                logger.log("OVERALL VERDICT: FAIL")
                return 1
            logger.log("[host] WARNING: --allow-dirty specified. Audit certification requires clean tree.")

        tools = find_tools()
        logger.log(f"[host] Compiler: {tools['gcc']}")
        logger.log(f"[host] Objdump:  {tools['objdump']}")
        logger.log(f"[host] CMake:    {tools['cmake']}")

        overall_ok = True if tree_clean else False

        if "t1" in tests_to_run:
            t1_ok = run_t1(tools, logger)
            if not t1_ok:
                overall_ok = False

        if "t2" in tests_to_run:
            t2_ok = run_t2(tools, logger)
            if not t2_ok:
                overall_ok = False

        if "t3" in tests_to_run:
            t3_ok = run_t3(tools, args.control_dir, logger)
            if not t3_ok:
                overall_ok = False

        if "t4" in tests_to_run:
            t4_ok = run_t4(tools, args.sn, logger)
            if not t4_ok:
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
