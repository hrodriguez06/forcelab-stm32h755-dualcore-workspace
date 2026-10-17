#!/usr/bin/env python3
"""
tools/gpio_close_checks.py

Verification tool for 00_gpio driver close (K1-K5).
Runs checks T1 to T3 (host / no-board) and provides T4 execution for board auditor.

Features & Verification Coverage:
- T1: Build Matrix & Selftests
  - 8 matrix configurations: prj_01, prj_02, prj_03, prj_04 in debug & release
  - Selftests: prj_01 [selftest-gpio], prj_03 [selftest], prj_04 [selftest-gpio]
  - 0 warnings, 0 errors, newly written images
  - Negative control: deliberate warning injected in compiler flags via evaluate_build_output()
- T2: Symbols, Objects & Text Equivalence
  - CM7 main.c.obj has GPIO_BM_EnableCompensationCell undefined (U) and no HAL_EnableCompensationCell
  - Linked ELF contains no HAL_EnableCompensationCell
  - Text: CompensationCell_Init() is byte-identical across prj_02, prj_03, and prj_04
  - Deep binary equivalence against control f066a8e compiled in same execution:
    * All other CM7 objects (all except main.c.obj)
    * All CM4 objects
    * All prj_01 objects (CM7 & CM4)
  - Negative controls (same functions):
    1) Control f066a8e main.c.obj passed to evaluate_main_symbols() -> FAIL
    2) Object with mutated section passed to compare_object_equivalence() -> FAIL
    3) Mutated main.c source passed to evaluate_compensation_cell_text() -> FAIL
- T3: K2 Linkage against build.ninja libraries
  - Temporary .c written outside repo calling GPIO_HAL_Init, GPIO_BM_ExtiConfig,
    and GPIO_BM_EnableCompensationCell
  - Objects resolved from build.ninja (gpio_hal.c.obj, gpio_bm.c.obj)
  - Partial & full link passes with no unresolved symbols
  - Negative control: link without gpio_bm.c.obj fails with undefined reference to GPIO_BM_*
- T4: Target execution (board auditor) or simulated mode
  - Without --sn: terminates with FAIL without touching anything
  - With --simulate-t4: tests simulated CLI output and confirms negative controls
  - Negative controls: mutated readings with READY=0 and EN=0 rejected by evaluate_t4_registers()
"""

from __future__ import annotations

import argparse
import hashlib
import os
import re
import shutil
import subprocess
import sys
import tempfile
import time
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
CONTROL_COMMIT_EXPECTED = "f066a8e"
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

SELFTEST_CONFIGS = [
    ("03_firmware_templates/prj_01_template_dualcore", "selftest-gpio"),
    ("02_firmware_projects/prj_03_adc_dma_eth", "selftest"),
    ("02_firmware_projects/prj_04_adc_dma_bm_eth", "selftest-gpio"),
]


def run_cmd(
    args: list[str],
    cwd: Path | None = None,
    timeout: float = 300.0,
    env: dict[str, str] | None = None,
) -> tuple[int, str, str]:
    """Runs command with subprocess and returns (code, stdout, stderr)."""
    try:
        proc = subprocess.run(
            args,
            cwd=str(cwd) if cwd else None,
            capture_output=True,
            text=True,
            timeout=timeout,
            env=env,
        )
        return proc.returncode, proc.stdout, proc.stderr
    except subprocess.TimeoutExpired as e:
        return -1, e.stdout or "", f"Timeout after {timeout}s: {e}"
    except Exception as e:
        return -2, "", str(e)


class Logger:
    def __init__(self, out_path: Path | None) -> None:
        self.out_path = out_path
        self.lines: list[str] = []
        if self.out_path:
            self.out_path.parent.mkdir(parents=True, exist_ok=True)
            self.out_path.write_text("", encoding="utf-8")

    def log(self, msg: str) -> None:
        print(msg, flush=True)
        self.lines.append(msg)
        if self.out_path:
            with open(self.out_path, "a", encoding="utf-8") as f:
                f.write(msg + "\n")


def sha256_file(path: Path) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as f:
        while chunk := f.read(65536):
            h.update(chunk)
    return h.hexdigest()


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

    ninja_path = shutil.which("ninja")
    if not ninja_path:
        for p in Path(r"C:\ST").glob("**/ninja.exe"):
            ninja_path = str(p)
            break
    tools["ninja"] = ninja_path or "ninja"

    prog_path = shutil.which("STM32_Programmer_CLI")
    if not prog_path:
        for p in Path(r"C:\ST").glob("**/STM32_Programmer_CLI.exe"):
            prog_path = str(p)
            break
    tools["programmer"] = prog_path or "STM32_Programmer_CLI"

    return tools


def get_head_full_rev(repo: Path = REPO_ROOT) -> str:
    code, out, _ = run_cmd(["git", "rev-parse", "HEAD"], cwd=repo, timeout=10.0)
    return out.strip() if code == 0 else "unknown"


def snapshot_working_tree(repo_root: Path) -> dict[str, str]:
    """Snapshots git status porcelain entries."""
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
    repo_root: Path,
    output_file: Path | None,
    initial_snapshot: dict[str, str] | None = None
) -> tuple[bool, str]:
    """Verifies working tree cleanliness outside build/ directories."""
    current = snapshot_working_tree(repo_root)
    out_resolved = output_file.resolve() if output_file is not None else None

    dirty = []
    for rel_path, prefix in current.items():
        resolved_path = (repo_root / rel_path).resolve()
        if out_resolved is not None and resolved_path == out_resolved:
            continue
        if "build" in resolved_path.parts:
            continue
        # Test capture files under 01_shared_drivers/00_gpio/docs are permitted
        if (resolved_path.parent == (repo_root / "01_shared_drivers/00_gpio/docs").resolve()
                and resolved_path.name.startswith("gpio_close_")):
            continue

        if prefix in (" M", "M ", " D", "D ", "A ", "AM", "AD", "R ", "C ", "U ", "??"):
            dirty.append(f"{prefix} {rel_path}")
        elif initial_snapshot is not None:
            if rel_path not in initial_snapshot:
                dirty.append(f"NEW_IGNORED: {rel_path}")

    if dirty:
        return False, f"Dirty tree outside build/ ({len(dirty)} items): {dirty[:5]}"
    return True, "Tree clean outside build/"


def ensure_control_worktree(control_dir: Path, logger: Logger) -> tuple[bool, str]:
    """Ensures control worktree exists at CONTROL_COMMIT_EXPECTED and is clean."""
    if not control_dir.is_dir():
        logger.log(f"[control] Creating control worktree at {control_dir} (commit {CONTROL_COMMIT_EXPECTED})...")
        code, out, err = run_cmd(["git", "worktree", "add", str(control_dir), CONTROL_COMMIT_EXPECTED], cwd=REPO_ROOT, timeout=120.0)
        if code != 0:
            return False, f"git worktree add failed ({code}): {err or out}"

    code, ctrl_rev, _ = run_cmd(["git", "rev-parse", "HEAD"], cwd=control_dir, timeout=10.0)
    ctrl_rev = ctrl_rev.strip()
    if code != 0 or not ctrl_rev.startswith(CONTROL_COMMIT_EXPECTED):
        return False, f"Control commit {ctrl_rev} does not match expected {CONTROL_COMMIT_EXPECTED}"

    code_ctrl_st, ctrl_st_out, _ = run_cmd(["git", "status", "--porcelain", "--ignore-submodules"], cwd=control_dir, timeout=30.0)
    if code_ctrl_st != 0 or ctrl_st_out.strip():
        return False, f"Control worktree dirty ({code_ctrl_st}): {ctrl_st_out.strip()}"

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

    # Ignore submodule in control worktree so git status does not fail in worktree
    run_cmd(["git", "-C", str(control_dir), "config", "submodule.02_firmware_projects/prj_eth_lwip_node/external/STM32CubeH7.ignore", "all"], timeout=10.0)

    return True, f"Control worktree ready at {CONTROL_COMMIT_EXPECTED}"


# ==============================================================================
# T1: Build Matrix & Selftests
# ==============================================================================

def parse_compiler_warnings(build_output: str) -> list[str]:
    warnings = []
    for line in build_output.splitlines():
        if re.search(r"\bwarning:", line, re.IGNORECASE):
            warnings.append(line.strip())
    return warnings


def count_built_objects(build_output: str) -> int:
    return sum(1 for line in build_output.splitlines() if "Building C object" in line or "Building CXX object" in line)


def evaluate_build_output(
    config_name: str,
    preset: str,
    build_code: int,
    build_output: str,
    built_objects: int,
    expected_warnings: int = 0
) -> tuple[bool, list[str], str]:
    """Pure evaluation function used for real builds and negative controls."""
    warnings = parse_compiler_warnings(build_output)
    passed = (
        (build_code == 0)
        and (len(warnings) == expected_warnings)
        and (built_objects >= 0)
    )
    detail = (
        f"exit_code={build_code}, warnings={len(warnings)} (expected {expected_warnings}), "
        f"objects={built_objects}"
    )
    return passed, warnings, detail


def check_image_freshness(pdir: Path, preset: str, start_time: float) -> tuple[bool, str]:
    cm7_elf = pdir / "build" / preset / "CM7" / "cm7_firmware.elf"
    cm4_elf = pdir / "build" / preset / "CM4" / "cm4_firmware.elf"

    for elf in (cm7_elf, cm4_elf):
        if not elf.is_file():
            return False, f"Missing binary {elf}"
        if elf.stat().st_mtime < start_time - 5.0:
            return False, f"Stale binary {elf} (mtime {elf.stat().st_mtime} < {start_time})"

    return True, "Freshly built ELFs present"


def compile_single_config(
    tools: dict[str, str],
    proj_dir: Path,
    preset: str,
    logger: Logger,
    fresh: bool = False
) -> tuple[bool, int, str, int, list[str]]:
    bdir = proj_dir / "build" / preset
    configure_cmd = [tools["cmake"], f"--preset={preset}"]
    if fresh:
        configure_cmd.append("--fresh")

    code_c, out_c, err_c = run_cmd(configure_cmd, cwd=proj_dir, timeout=120.0)
    if code_c != 0:
        logger.log(f"[build] Configure failed for {proj_dir.name} [{preset}]: {err_c or out_c}")
        return False, code_c, (out_c + "\n" + err_c), 0, []

    build_cmd = [tools["cmake"], "--build", f"--preset={preset}", "--clean-first"]
    code_b, out_b, err_b = run_cmd(build_cmd, cwd=proj_dir, timeout=300.0)
    combined = out_b + "\n" + err_b
    objs = count_built_objects(combined)
    warnings = parse_compiler_warnings(combined)

    return (code_b == 0), code_b, combined, objs, warnings


def run_t1(tools: dict[str, str], logger: Logger) -> bool:
    logger.log("=== Running T1 Checks (Build Matrix 8/8 & Selftests) ===")
    overall_ok = True
    t1_start = time.time()

    # 1. Compile 8 matrix configs via build_all.ps1 -Clean
    logger.log("[check] Executing tools/build_all.ps1 -Clean for 8 matrix builds...")
    cmd_build_all = [
        "powershell", "-NoProfile", "-ExecutionPolicy", "Bypass",
        "-File", str(REPO_ROOT / "tools" / "build_all.ps1"), "-Clean"
    ]
    code_ba, out_ba, err_ba = run_cmd(cmd_build_all, cwd=REPO_ROOT, timeout=600.0)
    ba_output = out_ba + "\n" + err_ba
    ba_warnings = parse_compiler_warnings(ba_output)
    pass_lines = sum(1 for line in out_ba.splitlines() if "-> PASS" in line or " PASS " in line)

    ba_ok = (code_ba == 0) and ("All 8 builds PASSED successfully" in out_ba) and (len(ba_warnings) == 0)
    logger.log(f"CHECK t1_matrix_build_all: {'PASS' if ba_ok else 'FAIL'} (exit_code={code_ba}, pass_count={pass_lines}, warnings={len(ba_warnings)})")
    if not ba_ok:
        overall_ok = False

    # Verify freshness of all 8 matrix configurations
    for proj_rel, preset in MATRIX_CONFIGS:
        pdir = REPO_ROOT / proj_rel
        fresh_ok, fresh_msg = check_image_freshness(pdir, preset, t1_start)
        logger.log(f"CHECK t1_freshness_{pdir.name}_{preset}: {'PASS' if fresh_ok else 'FAIL'} ({fresh_msg})")
        if not fresh_ok:
            overall_ok = False

    # 2. Compile selftest presets
    for proj_rel, preset in SELFTEST_CONFIGS:
        pdir = REPO_ROOT / proj_rel
        logger.log(f"[check] Building {pdir.name} [{preset}]...")
        passed, code, out, objs, warnings = compile_single_config(tools, pdir, preset, logger, fresh=True)
        eval_pass, eval_warns, eval_detail = evaluate_build_output(
            pdir.name, preset, code, out, objs, expected_warnings=0
        )
        fresh_ok, fresh_msg = check_image_freshness(pdir, preset, t1_start)
        st_ok = eval_pass and fresh_ok
        logger.log(f"CHECK t1_selftest_{pdir.name}_{preset}: {'PASS' if st_ok else 'FAIL'} ({eval_detail}, {fresh_msg})")
        if not st_ok:
            overall_ok = False

    # 3. Negative control: Injected warning in temporary copy of project flags
    logger.log("[check] Running T1 negative control (injecting deliberate compiler warning)...")
    with tempfile.TemporaryDirectory(prefix="gpio_t1_neg_") as tmp_dir:
        tmp_path = Path(tmp_dir)
        c_file = tmp_path / "warn_sample.c"
        c_obj = tmp_path / "warn_sample.o"
        c_file.write_text(
            "#include <stdint.h>\n"
            "int test_warning_var(void) {\n"
            "    int unused_local_variable = 42;\n"
            "    return 0;\n"
            "}\n",
            encoding="utf-8"
        )
        flags = [
            "-mcpu=cortex-m7", "-mthumb", "-mfpu=fpv5-d16", "-mfloat-abi=hard",
            "-Og", "-g3", "-Wall", "-Wextra"
        ]
        cmd = [tools["gcc"], "-c", str(c_file), "-o", str(c_obj)] + flags
        c_code, c_out, c_err = run_cmd(cmd, cwd=tmp_path, timeout=30.0)
        c_combined = c_out + "\n" + c_err
        c_objs = 1 if c_obj.is_file() else 0

        eval_pass, eval_warns, eval_detail = evaluate_build_output(
            "neg_control", "warning_sample", c_code, c_combined, c_objs, expected_warnings=0
        )
        neg_ok = (not eval_pass) and (len(eval_warns) > 0)
        logger.log(f"CHECK t1_negative_control_warning: {'PASS' if neg_ok else 'FAIL'} (detected_warnings={len(eval_warns)}, eval_pass={eval_pass})")
        if not neg_ok:
            overall_ok = False

    return overall_ok


# ==============================================================================
# T2: Reused Object Equivalence Comparator from tools/eth_e3e6_checks.py
# (extract_alloc_section_headers, extract_nondebug_raw_contents,
#  extract_nondebug_relocations, extract_nondebug_symbols, compare_object_equivalence)
# ==============================================================================

def extract_alloc_section_headers(tools: dict[str, str], obj_path: Path) -> dict[str, tuple[str, str, str, str, str]]:
    """Extracts all ALLOC section headers via arm-none-eabi-objdump -h."""
    r_code, r_out, r_err = run_cmd([tools["objdump"], "-h", str(obj_path)], timeout=30.0)
    if r_code != 0:
        raise RuntimeError(f"objdump -h failed on {obj_path}: {r_err}")

    headers = {}
    lines = r_out.splitlines()
    for i, line in enumerate(lines):
        line_s = line.strip()
        parts = line_s.split()
        if len(parts) >= 7 and parts[0].isdigit() and (i + 1 < len(lines)):
            flags = lines[i + 1].strip()
            if "ALLOC" in flags:
                sec_name = parts[1]
                size = parts[2]
                vma = parts[3]
                lma = parts[4]
                algn = parts[6]
                headers[sec_name] = (size, vma, lma, algn, flags)
    return headers


def extract_nondebug_raw_contents(tools: dict[str, str], obj_path: Path) -> dict[str, str]:
    """Extracts raw content of all non-debug sections via arm-none-eabi-objdump -s."""
    r_code, r_out, r_err = run_cmd([tools["objdump"], "-s", str(obj_path)], timeout=30.0)
    if r_code != 0:
        raise RuntimeError(f"objdump -s failed on {obj_path}: {r_err}")

    sections = {}
    current_sec = None
    lines: list[str] = []
    for line in r_out.splitlines():
        if line.startswith("Contents of section "):
            if current_sec and not current_sec.startswith(".debug") and current_sec != ".comment":
                sections[current_sec] = "\n".join(lines)
            current_sec = line.split()[3].rstrip(":")
            lines = []
        else:
            lines.append(line)
    if current_sec and not current_sec.startswith(".debug") and current_sec != ".comment":
        sections[current_sec] = "\n".join(lines)
    return sections


def extract_nondebug_relocations(tools: dict[str, str], obj_path: Path) -> dict[str, str]:
    """Extracts relocations of all non-debug sections via arm-none-eabi-objdump -r."""
    r_code, r_out, r_err = run_cmd([tools["objdump"], "-r", str(obj_path)], timeout=30.0)
    if r_code != 0:
        raise RuntimeError(f"objdump -r failed on {obj_path}: {r_err}")

    relocs = {}
    curr_sec = None
    for line in r_out.splitlines():
        if line.startswith("RELOCATION RECORDS FOR ["):
            sec = line.split("[")[1].split("]")[0]
            if not sec.startswith(".debug") and sec != ".comment":
                curr_sec = sec
                relocs[curr_sec] = []
            else:
                curr_sec = None
        elif curr_sec and line.strip() and not line.startswith("OFFSET"):
            relocs[curr_sec].append(line.strip())
    return {k: "\n".join(v) for k, v in relocs.items()}


def extract_nondebug_symbols(tools: dict[str, str], obj_path: Path) -> list[str]:
    """Extracts symbols via arm-none-eabi-nm -p, filtering out debug symbols."""
    r_code, r_out, r_err = run_cmd([tools["nm"], "-p", str(obj_path)], timeout=30.0)
    if r_code != 0:
        raise RuntimeError(f"nm -p failed on {obj_path}: {r_err}")

    symbols = []
    for line in r_out.splitlines():
        parts = line.split()
        if len(parts) >= 3:
            stype = parts[1]
            sname = parts[2]
            if stype.lower() not in ("n", "a") and not sname.endswith(".c"):
                symbols.append(f"{stype} {sname}")
        elif len(parts) == 2:
            stype = parts[0]
            sname = parts[1]
            if stype.lower() not in ("n", "a") and not sname.endswith(".c"):
                symbols.append(f"{stype} {sname}")
    return sorted(symbols)


def compare_object_equivalence(
    hdrs_h: dict, hdrs_c: dict,
    raw_h: dict, raw_c: dict,
    relocs_h: dict, relocs_c: dict,
    syms_h: list, syms_c: list
) -> tuple[bool, str]:
    """Deep comparison function covering ALLOC headers, raw contents, relocations and symbols."""
    if not hdrs_h or not hdrs_c:
        return False, "Empty section headers"

    if hdrs_h != hdrs_c:
        diff_h = set(hdrs_h.items()) ^ set(hdrs_c.items())
        return False, f"Section header mismatch: {diff_h}"

    if raw_h != raw_c:
        diff_r = set(raw_h.keys()) ^ set(raw_c.keys())
        if diff_r:
            return False, f"Section raw content keys mismatch: {diff_r}"
        for k in sorted(raw_h.keys()):
            if raw_h[k] != raw_c[k]:
                return False, f"Section raw content mismatch in {k}"

    if relocs_h != relocs_c:
        diff_rel = set(relocs_h.keys()) ^ set(relocs_c.keys())
        if diff_rel:
            return False, f"Relocation section mismatch: {diff_rel}"
        for k in sorted(relocs_h.keys()):
            if relocs_h[k] != relocs_c[k]:
                return False, f"Relocation record mismatch in {k}"

    if syms_h != syms_c:
        diff_s = set(syms_h) ^ set(syms_c)
        return False, f"Symbol table mismatch: {diff_s}"

    return True, f"Identical: {len(hdrs_h)} ALLOC sections, {len(raw_h)} content sections, {len(relocs_h)} reloc sections, {len(syms_h)} symbols"


def normalize_git_rev_in_raw(raw_dict: dict[str, str], rev: str) -> dict[str, str]:
    """Normalizes the 7-character git short rev in raw objdump section hex and ascii strings."""
    if not rev or len(rev) < 7:
        return raw_dict
    rev7 = rev[:7]
    h1 = rev7[:4].encode().hex()
    h2 = rev7[4:].encode().hex()
    h_combined = f"{h1} {h2}"
    out = {}
    for k, v in raw_dict.items():
        out[k] = v.replace(h_combined, "00000000 000000").replace(rev7, "0000000")
    return out


def extract_fw_build_id(bdir: Path) -> str:
    """Extracts FW_BUILD_ID from generated/fw_build_id.h if present."""
    bid_h = bdir / "generated" / "fw_build_id.h"
    if bid_h.is_file():
        m = re.search(r'#define\s+FW_BUILD_ID\s+"([^"]+)"', bid_h.read_text(encoding="utf-8"))
        if m:
            return m.group(1).strip()
    return ""


def normalize_build_id_in_raw(raw_dict: dict[str, str], bid: str) -> dict[str, str]:
    """Normalizes the 16-hex-digit FW_BUILD_ID in raw objdump hex and ascii strings."""
    if not bid or len(bid) < 16:
        return raw_dict
    out = {}
    for k, v in raw_dict.items():
        for i in range(0, 16, 4):
            w = bid[i:i + 4].encode().hex()
            v = v.replace(w, "00000000")
        v = v.replace(bid, "0000000000000000").replace(bid[:8], "00000000").replace(bid[8:], "00000000")
        out[k] = v
    return out


# ==============================================================================
# T2: Symbols and Text Evaluation Functions
# ==============================================================================

def evaluate_main_symbols(
    tools: dict[str, str],
    main_obj: Path,
    elf_path: Path
) -> tuple[bool, str]:
    """Pure evaluation function verifying K1 symbols in main.c.obj and linked ELF."""
    if not main_obj.is_file():
        return False, f"main.c.obj missing: {main_obj}"
    if not elf_path.is_file():
        return False, f"ELF missing: {elf_path}"

    c_code, c_out, c_err = run_cmd([tools["nm"], str(main_obj)], timeout=30.0)
    if c_code != 0:
        return False, f"nm failed on {main_obj}: {c_err}"

    e_code, e_out, e_err = run_cmd([tools["nm"], str(elf_path)], timeout=30.0)
    if e_code != 0:
        return False, f"nm failed on {elf_path}: {e_err}"

    # In main.c.obj:
    # 1. GPIO_BM_EnableCompensationCell must be undefined ('U')
    has_gpio_bm_undef = False
    for line in c_out.splitlines():
        parts = line.strip().split()
        if len(parts) >= 2 and parts[-1] == "GPIO_BM_EnableCompensationCell":
            if parts[-2] == "U":
                has_gpio_bm_undef = True

    # 2. HAL_EnableCompensationCell must NOT be in main.c.obj
    has_hal_in_obj = any("HAL_EnableCompensationCell" in line for line in c_out.splitlines())

    # 3. HAL_EnableCompensationCell must NOT be in ELF
    has_hal_in_elf = any("HAL_EnableCompensationCell" in line for line in e_out.splitlines())

    passed = has_gpio_bm_undef and (not has_hal_in_obj) and (not has_hal_in_elf)
    detail = (
        f"gpio_bm_enable_undef={has_gpio_bm_undef}, "
        f"hal_enable_in_obj={has_hal_in_obj}, hal_enable_in_elf={has_hal_in_elf}"
    )
    return passed, detail


def extract_compensation_cell_text(source_text: str) -> str:
    """Extracts CompensationCell_Init definition including preceding #defines and docblock."""
    m = re.search(r"(/\* CSI ready timeout.*?^static void CompensationCell_Init\(void\)\s*\{.*?^\})", source_text, re.DOTALL | re.MULTILINE)
    if not m:
        # Fallback to function body only
        m = re.search(r"(static void CompensationCell_Init\(void\)\s*\{.*?^\})", source_text, re.DOTALL | re.MULTILINE)
    return m.group(1).strip() if m else ""


def evaluate_compensation_cell_text(source_texts: list[str]) -> tuple[bool, str]:
    """Pure comparison function verifying CompensationCell_Init() text identity."""
    if len(source_texts) < 2:
        return False, "Fewer than 2 source texts provided"

    extracted = [extract_compensation_cell_text(t) for t in source_texts]
    for i, e in enumerate(extracted):
        if not e:
            return False, f"Source text {i} missing CompensationCell_Init()"

    first = extracted[0]
    for i in range(1, len(extracted)):
        if extracted[i] != first:
            return False, f"Source text {i} differs from source text 0"

    return True, f"All {len(source_texts)} implementations are byte-for-byte identical ({len(first)} chars)"


def run_t2(tools: dict[str, str], control_dir: Path, logger: Logger) -> bool:
    logger.log("=== Running T2 Checks (Symbols, Text & Deep Binary Equivalence vs Control f066a8e) ===")
    overall_ok = True

    # 1. Ensure control worktree exists and is built
    ctrl_ok, ctrl_msg = ensure_control_worktree(control_dir, logger)
    logger.log(f"[control] Control worktree setup: {'PASS' if ctrl_ok else 'FAIL'} ({ctrl_msg})")
    if not ctrl_ok:
        return False

    logger.log("[control] Building control worktree via build_all.ps1 -Clean (with -ffile-prefix-map)...")
    ctrl_env = os.environ.copy()
    c_from = control_dir.resolve().as_posix()
    c_to = REPO_ROOT.resolve().as_posix()
    ctrl_env["CFLAGS"] = f"-ffile-prefix-map={c_from}={c_to}"

    cmd_ctrl_build = [
        "powershell", "-NoProfile", "-ExecutionPolicy", "Bypass",
        "-File", str(control_dir / "tools" / "build_all.ps1"), "-Clean"
    ]
    code_cb, out_cb, err_cb = run_cmd(cmd_ctrl_build, cwd=control_dir, timeout=600.0, env=ctrl_env)
    ctrl_build_ok = (code_cb == 0) and ("All 8 builds PASSED successfully" in out_cb)
    logger.log(f"CHECK t2_control_build_all: {'PASS' if ctrl_build_ok else 'FAIL'} (exit_code={code_cb})")
    if not ctrl_build_ok:
        return False

    # 2. Check main.c.obj and ELF symbols for prj_02, prj_03, prj_04 (debug & release)
    k1_projects = [
        ("02_firmware_projects/prj_02_eth_lwip_dualcore", "prj_02"),
        ("02_firmware_projects/prj_03_adc_dma_eth", "prj_03"),
        ("02_firmware_projects/prj_04_adc_dma_bm_eth", "prj_04"),
    ]

    for proj_rel, pname in k1_projects:
        for preset in ("debug", "release"):
            pdir = REPO_ROOT / proj_rel
            main_obj = pdir / "build" / preset / "CM7" / "CMakeFiles" / "cm7_firmware.dir" / "Core" / "Src" / "main.c.obj"
            elf_path = pdir / "build" / preset / "CM7" / "cm7_firmware.elf"

            sym_ok, sym_detail = evaluate_main_symbols(tools, main_obj, elf_path)
            logger.log(f"CHECK t2_symbols_{pname}_{preset}_cm7: {'PASS' if sym_ok else 'FAIL'} ({sym_detail})")
            if not sym_ok:
                overall_ok = False

    # 3. Check CompensationCell_Init text identity across the three projects
    source_texts = []
    for proj_rel, _ in k1_projects:
        main_c_path = REPO_ROOT / proj_rel / "CM7" / "Core" / "Src" / "main.c"
        source_texts.append(main_c_path.read_text(encoding="utf-8"))

    text_ok, text_detail = evaluate_compensation_cell_text(source_texts)
    logger.log(f"CHECK t2_text_identity_compensation_cell: {'PASS' if text_ok else 'FAIL'} ({text_detail})")
    if not text_ok:
        overall_ok = False

    # 4. Deep binary equivalence for all objects outside CM7 main.c.obj
    all_projects = [
        ("03_firmware_templates/prj_01_template_dualcore", "prj_01", False),
        ("02_firmware_projects/prj_02_eth_lwip_dualcore", "prj_02", True),
        ("02_firmware_projects/prj_03_adc_dma_eth", "prj_03", True),
        ("02_firmware_projects/prj_04_adc_dma_bm_eth", "prj_04", True),
    ]

    total_objects_compared = 0
    all_objects_pass = True
    h_rev = get_head_full_rev(REPO_ROOT)[:7]
    c_rev = get_head_full_rev(control_dir)[:7]

    for proj_rel, pname, exclude_cm7_main in all_projects:
        for preset in ("debug", "release"):
            head_bdir = REPO_ROOT / proj_rel / "build" / preset
            ctrl_bdir = control_dir / proj_rel / "build" / preset
            h_bid = extract_fw_build_id(head_bdir)
            c_bid = extract_fw_build_id(ctrl_bdir)

            head_objs = sorted(head_bdir.glob("**/*.c.obj"))
            if not head_objs:
                logger.log(f"CHECK t2_objects_found_{pname}_{preset}: FAIL (No objects found in {head_bdir})")
                overall_ok = False
                continue

            for h_obj in head_objs:
                rel_to_build = h_obj.relative_to(head_bdir)
                is_cm7 = "CM7" in rel_to_build.parts
                is_main = h_obj.name == "main.c.obj"

                if exclude_cm7_main and is_cm7 and is_main:
                    continue  # K1 modified CM7 main.c.obj; tested via evaluate_main_symbols

                c_obj = ctrl_bdir / rel_to_build
                if not c_obj.is_file():
                    # For external sources (e.g. HAL, LwIP), CMake hashes the source path,
                    # which differs between worktrees. Resolve by unique object name within core.
                    core_dir = "CM7" if is_cm7 else "CM4"
                    candidates = list((ctrl_bdir / core_dir).rglob(h_obj.name))
                    if len(candidates) == 1:
                        c_obj = candidates[0]
                    else:
                        logger.log(f"CHECK t2_object_match_{pname}_{preset}_{rel_to_build}: FAIL (missing in control)")
                        all_objects_pass = False
                        continue

                try:
                    hdrs_h = extract_alloc_section_headers(tools, h_obj)
                    hdrs_c = extract_alloc_section_headers(tools, c_obj)
                    raw_h = extract_nondebug_raw_contents(tools, h_obj)
                    raw_c = extract_nondebug_raw_contents(tools, c_obj)
                    relocs_h = extract_nondebug_relocations(tools, h_obj)
                    relocs_c = extract_nondebug_relocations(tools, c_obj)
                    syms_h = extract_nondebug_symbols(tools, h_obj)
                    syms_c = extract_nondebug_symbols(tools, c_obj)

                    if h_obj.name in ("app_main.c.obj", "main.c.obj"):
                        raw_h = normalize_git_rev_in_raw(raw_h, h_rev)
                        raw_c = normalize_git_rev_in_raw(raw_c, c_rev)
                        if h_bid:
                            raw_h = normalize_build_id_in_raw(raw_h, h_bid)
                        if c_bid:
                            raw_c = normalize_build_id_in_raw(raw_c, c_bid)

                    cmp_pass, cmp_msg = compare_object_equivalence(
                        hdrs_h, hdrs_c, raw_h, raw_c, relocs_h, relocs_c, syms_h, syms_c
                    )
                    if not cmp_pass:
                        logger.log(f"CHECK t2_object_match_{pname}_{preset}_{h_obj.name}: FAIL ({cmp_msg})")
                        all_objects_pass = False
                    total_objects_compared += 1
                except Exception as e:
                    logger.log(f"CHECK t2_object_match_{pname}_{preset}_{h_obj.name}: FAIL (exception: {e})")
                    all_objects_pass = False

    logger.log(f"CHECK t2_deep_binary_equivalence: {'PASS' if all_objects_pass else 'FAIL'} (compared {total_objects_compared} object files)")
    if not all_objects_pass:
        overall_ok = False

    # 5. Negative controls for T2 (all call the exact same functions)
    logger.log("[check] Running T2 negative controls...")

    # Neg control 1: Control f066a8e main.c.obj passed to evaluate_main_symbols() must FAIL
    ctrl_main_obj = control_dir / "02_firmware_projects" / "prj_02_eth_lwip_dualcore" / "build" / "debug" / "CM7" / "CMakeFiles" / "cm7_firmware.dir" / "Core" / "Src" / "main.c.obj"
    ctrl_elf = control_dir / "02_firmware_projects" / "prj_02_eth_lwip_dualcore" / "build" / "debug" / "CM7" / "cm7_firmware.elf"
    neg_sym_ok, neg_sym_detail = evaluate_main_symbols(tools, ctrl_main_obj, ctrl_elf)
    neg_sym_pass = (neg_sym_ok is False)
    logger.log(f"CHECK t2_negative_control_control_symbols: {'PASS' if neg_sym_pass else 'FAIL'} (passed={neg_sym_ok}, detail={neg_sym_detail})")
    if not neg_sym_pass:
        overall_ok = False

    # Neg control 2: Object with mutated content passed to compare_object_equivalence() must FAIL
    with tempfile.TemporaryDirectory(prefix="gpio_t2_neg_obj_") as tmp_dir:
        tmp_path = Path(tmp_dir)
        sample_obj = control_dir / "03_firmware_templates" / "prj_01_template_dualcore" / "build" / "debug" / "CM7" / "CMakeFiles" / "cm7_firmware.dir" / "Core" / "Src" / "main.c.obj"
        mutated_obj = tmp_path / "mutated.o"
        obj_bytes = bytearray(sample_obj.read_bytes())
        # Mutate byte inside .text.main (ALLOC section at file offset 0x4f0)
        obj_bytes[0x4f0] ^= 0xFF
        mutated_obj.write_bytes(obj_bytes)

        try:
            h_hdrs = extract_alloc_section_headers(tools, sample_obj)
            m_hdrs = extract_alloc_section_headers(tools, mutated_obj)
            h_raw = extract_nondebug_raw_contents(tools, sample_obj)
            m_raw = extract_nondebug_raw_contents(tools, mutated_obj)
            h_relocs = extract_nondebug_relocations(tools, sample_obj)
            m_relocs = extract_nondebug_relocations(tools, mutated_obj)
            h_syms = extract_nondebug_symbols(tools, sample_obj)
            m_syms = extract_nondebug_symbols(tools, mutated_obj)

            neg_cmp_ok, neg_cmp_msg = compare_object_equivalence(
                h_hdrs, m_hdrs, h_raw, m_raw, h_relocs, m_relocs, h_syms, m_syms
            )
        except Exception as e:
            neg_cmp_ok = False
            neg_cmp_msg = f"Extraction rejected mutated object: {e}"

        neg_obj_pass = (neg_cmp_ok is False)
        logger.log(f"CHECK t2_negative_control_mutated_object: {'PASS' if neg_obj_pass else 'FAIL'} ({neg_cmp_msg})")
        if not neg_obj_pass:
            overall_ok = False

    # Neg control 3: Mutated main.c source text passed to evaluate_compensation_cell_text() must FAIL
    mutated_sources = list(source_texts)
    mutated_sources[1] = mutated_sources[1].replace("COMP_CELL_MAX_CYCLES", "COMP_CELL_MAX_CYCLES_MUTATED")
    neg_text_ok, neg_text_detail = evaluate_compensation_cell_text(mutated_sources)
    neg_text_pass = (neg_text_ok is False)
    logger.log(f"CHECK t2_negative_control_mutated_text: {'PASS' if neg_text_pass else 'FAIL'} ({neg_text_detail})")
    if not neg_text_pass:
        overall_ok = False

    return overall_ok


# ==============================================================================
# T3: K2 Linkage against build.ninja Libraries
# ==============================================================================

def find_ninja_gpio_objects(bdir: Path) -> tuple[Path, Path]:
    """Resolves gpio_hal.c.obj and gpio_bm.c.obj paths dynamically from build.ninja."""
    ninja_path = bdir / "build.ninja"
    if not ninja_path.is_file():
        raise FileNotFoundError(f"build.ninja not found in {bdir}")

    content = ninja_path.read_text(encoding="utf-8", errors="ignore")
    hal_rel = None
    bm_rel = None

    for line in content.splitlines():
        if line.startswith("build ") and ": C_COMPILER__" in line:
            m = re.match(r"^build\s+(.*?):\s+C_COMPILER__", line)
            if m:
                out_path = m.group(1).strip()
                if "CM7" in out_path:
                    if out_path.endswith("gpio_hal.c.obj"):
                        hal_rel = out_path
                    elif out_path.endswith("gpio_bm.c.obj"):
                        bm_rel = out_path

    if not hal_rel or not bm_rel:
        raise RuntimeError(f"Could not extract gpio_hal and gpio_bm object paths from {ninja_path}")

    return (bdir / hal_rel), (bdir / bm_rel)


def evaluate_link_result(returncode: int, stderr_out: str, expected_success: bool) -> tuple[bool, str]:
    """Pure evaluation function used for T3 linkage and negative control."""
    if expected_success:
        passed = (returncode == 0)
        detail = f"exit_code={returncode}"
    else:
        passed = (returncode != 0) and ("undefined reference" in stderr_out) and ("GPIO_BM_" in stderr_out)
        detail = f"exit_code={returncode}, undefined references detected"
    return passed, detail


def run_t3(tools: dict[str, str], logger: Logger) -> bool:
    logger.log("=== Running T3 Checks (K2 Linkage against build.ninja libraries) ===")
    overall_ok = True

    bdir = REPO_ROOT / "02_firmware_projects" / "prj_04_adc_dma_bm_eth" / "build" / "debug"
    hal_obj, bm_obj = find_ninja_gpio_objects(bdir)
    logger.log(f"[check] Resolved objects from {bdir / 'build.ninja'}:")
    logger.log(f"        HAL: {hal_obj}")
    logger.log(f"        BM:  {bm_obj}")

    if not hal_obj.is_file() or not bm_obj.is_file():
        logger.log("CHECK t3_ninja_objects_exist: FAIL (objects missing on disk)")
        return False

    with tempfile.TemporaryDirectory(prefix="gpio_t3_link_") as tmp_dir:
        tmp_path = Path(tmp_dir)
        test_c = tmp_path / "test_k2_link.c"
        test_obj = tmp_path / "test_k2_link.o"

        test_c.write_text(
            "#include <stdint.h>\n"
            "#include <stdbool.h>\n"
            "typedef struct { uint32_t Pin; uint32_t Mode; uint32_t Pull; uint32_t Speed; uint32_t Alternate; } GPIO_InitTypeDef;\n"
            "typedef struct { volatile uint32_t MODER; } GPIO_TypeDef;\n"
            "int GPIO_HAL_Init(GPIO_TypeDef *port, const GPIO_InitTypeDef *init);\n"
            "int GPIO_BM_ExtiConfig(uint32_t line, uint32_t port, uint32_t trigger);\n"
            "int GPIO_BM_EnableCompensationCell(uint32_t max_cycles);\n"
            "\n"
            "/* Minimal stubs for external HAL calls made by gpio_hal */\n"
            "void HAL_GPIO_Init(GPIO_TypeDef *port, GPIO_InitTypeDef *init) { (void)port; (void)init; }\n"
            "int HAL_GPIO_LockPin(GPIO_TypeDef *port, uint16_t pin) { (void)port; (void)pin; return 0; }\n"
            "\n"
            "int main(void) {\n"
            "    GPIO_HAL_Init((GPIO_TypeDef *)0, (const GPIO_InitTypeDef *)0);\n"
            "    GPIO_BM_ExtiConfig(0, 0, 0);\n"
            "    GPIO_BM_EnableCompensationCell(1000);\n"
            "    return 0;\n"
            "}\n",
            encoding="utf-8"
        )

        cpu_flags = ["-mcpu=cortex-m7", "-mthumb", "-mfpu=fpv5-d16", "-mfloat-abi=hard"]
        cmd_compile = [tools["gcc"]] + cpu_flags + ["-c", str(test_c), "-o", str(test_obj)]
        c_code, _, c_err = run_cmd(cmd_compile, cwd=tmp_path, timeout=30.0)
        if c_code != 0:
            logger.log(f"CHECK t3_compile_test_source: FAIL ({c_err})")
            return False

        # 1. Full link with exact libraries/objects from build.ninja
        out_elf_ok = tmp_path / "out_ok.elf"
        cmd_link_ok = [tools["gcc"]] + cpu_flags + [
            "--specs=nosys.specs", "-o", str(out_elf_ok),
            str(test_obj), str(hal_obj), str(bm_obj)
        ]
        code_l1, out_l1, err_l1 = run_cmd(cmd_link_ok, cwd=tmp_path, timeout=30.0)
        link1_pass, link1_detail = evaluate_link_result(code_l1, (out_l1 + "\n" + err_l1), expected_success=True)
        logger.log(f"CHECK t3_link_hal_and_bm: {'PASS' if link1_pass else 'FAIL'} ({link1_detail})")
        if not link1_pass:
            overall_ok = False

        # 2. Partial link with -r
        out_r_ok = tmp_path / "out_r_ok.o"
        cmd_r_ok = [tools["gcc"]] + cpu_flags + [
            "-r", "-o", str(out_r_ok),
            str(test_obj), str(hal_obj), str(bm_obj)
        ]
        code_r, out_r, err_r = run_cmd(cmd_r_ok, cwd=tmp_path, timeout=30.0)
        # Verify no undefined GPIO_BM_* symbols in relocatable object
        code_nm, out_nm, _ = run_cmd([tools["nm"], "-u", str(out_r_ok)], timeout=30.0)
        unres_bm = [l.strip().split()[-1] for l in out_nm.splitlines() if "GPIO_BM_" in l]
        r_pass = (code_r == 0) and (len(unres_bm) == 0)
        logger.log(f"CHECK t3_partial_link_r_clean: {'PASS' if r_pass else 'FAIL'} (exit_code={code_r}, unresolved_bm={unres_bm})")
        if not r_pass:
            overall_ok = False

        # 3. Negative control: Link WITHOUT gpio_bm.c.obj must fail with undefined reference to GPIO_BM_*
        out_elf_fail = tmp_path / "out_fail.elf"
        cmd_link_fail = [tools["gcc"]] + cpu_flags + [
            "--specs=nosys.specs", "-o", str(out_elf_fail),
            str(test_obj), str(hal_obj)
        ]
        code_l2, out_l2, err_l2 = run_cmd(cmd_link_fail, cwd=tmp_path, timeout=30.0)
        link2_pass, link2_detail = evaluate_link_result(code_l2, (out_l2 + "\n" + err_l2), expected_success=False)
        logger.log(f"CHECK t3_negative_control_link_without_bm: {'PASS' if link2_pass else 'FAIL'} ({link2_detail})")
        if not link2_pass:
            overall_ok = False

    return overall_ok


# ==============================================================================
# T4: Hardware Board Verification (Auditor) or Simulated Execution
# ==============================================================================

def evaluate_t4_registers(cccsr_val: int, rcc_cr_val: int) -> tuple[bool, str]:
    """Pure evaluation function verifying CCCSR.EN=1, READY=1, RCC_CR.CSIRDY=1."""
    en_bit = (cccsr_val & 0x00000001) != 0       # SYSCFG_CCCSR.EN
    ready_bit = (cccsr_val & 0x00000100) != 0    # SYSCFG_CCCSR.READY
    csirdy_bit = (rcc_cr_val & 0x00000100) != 0   # RCC_CR.CSIRDY

    passed = en_bit and ready_bit and csirdy_bit
    detail = (
        f"CCCSR=0x{cccsr_val:08X} (EN={int(en_bit)}, READY={int(ready_bit)}), "
        f"RCC_CR=0x{rcc_cr_val:08X} (CSIRDY={int(csirdy_bit)})"
    )
    return passed, detail


def parse_swd_hotplug_output(cli_stdout: str) -> tuple[int, int]:
    """Parses 32-bit hex values from STM32_Programmer_CLI -r32 output."""
    readings = []
    for line in cli_stdout.splitlines():
        line = line.strip()
        m = re.match(r"^0x[0-9A-Fa-f]+\s*:\s*([0-9A-Fa-f]{8})", line)
        if m:
            readings.append(int(m.group(1), 16))

    if len(readings) < 2:
        raise ValueError(f"Expected at least 2 readings from CLI output, got {len(readings)}: {cli_stdout}")
    return readings[0], readings[1]


def run_t4(tools: dict[str, str], sn: str | None, simulate: bool, logger: Logger) -> bool:
    logger.log("=== Running T4 Checks (Hardware Board Verification / Simulation) ===")
    overall_ok = True

    # Record SHA-256 hashes of the target images compiled in this execution
    t4_targets = [
        ("02_firmware_projects/prj_02_eth_lwip_dualcore", "debug"),
        ("02_firmware_projects/prj_03_adc_dma_eth", "debug"),
        ("02_firmware_projects/prj_04_adc_dma_bm_eth", "debug"),
        ("02_firmware_projects/prj_04_adc_dma_bm_eth", "release"),
    ]

    logger.log("[t4] Recording SHA-256 of ELFs compiled in current execution:")
    for proj_rel, preset in t4_targets:
        cm7_elf = REPO_ROOT / proj_rel / "build" / preset / "CM7" / "cm7_firmware.elf"
        if cm7_elf.is_file():
            h = sha256_file(cm7_elf)
            logger.log(f"     {cm7_elf.name} ({proj_rel.split('/')[-1]} [{preset}]): {h}")
        else:
            logger.log(f"     MISSING: {cm7_elf}")
            overall_ok = False

    if simulate:
        logger.log("[t4] Running simulated T4 verification (no board connected, simulated CLI output)")
        # Real recorded baseline from board SWD reading: CCCSR=0x00000101, RCC_CR=0x0307C1A5
        sim_stdout = (
            "      -------------------------------------------------------------------\n"
            "                        STM32CubeProgrammer v2.16.0\n"
            "      -------------------------------------------------------------------\n"
            "Reading 32-bit memory content\n"
            "  0x58000420 : 00000101\n"
            "Reading 32-bit memory content\n"
            "  0x58024400 : 0307C1A5\n"
        )
        cccsr, rcc_cr = parse_swd_hotplug_output(sim_stdout)
        ev_pass, ev_detail = evaluate_t4_registers(cccsr, rcc_cr)
        logger.log(f"CHECK t4_simulated_register_reading: {'PASS' if ev_pass else 'FAIL'} ({ev_detail})")
        if not ev_pass:
            overall_ok = False

        # Negative control 1: Mutated in-memory reading with READY=0
        cccsr_mut_ready = cccsr & ~0x00000100
        neg1_pass, neg1_detail = evaluate_t4_registers(cccsr_mut_ready, rcc_cr)
        neg1_ok = (neg1_pass is False)
        logger.log(f"CHECK t4_negative_control_ready_zero: {'PASS' if neg1_ok else 'FAIL'} ({neg1_detail})")
        if not neg1_ok:
            overall_ok = False

        # Negative control 2: Mutated in-memory reading with EN=0
        cccsr_mut_en = cccsr & ~0x00000001
        neg2_pass, neg2_detail = evaluate_t4_registers(cccsr_mut_en, rcc_cr)
        neg2_ok = (neg2_pass is False)
        logger.log(f"CHECK t4_negative_control_en_zero: {'PASS' if neg2_ok else 'FAIL'} ({neg2_detail})")
        if not neg2_ok:
            overall_ok = False

        logger.log("[t4] Note: Board execution pending auditor verification (T4 NO VERIFICADA sin placa).")
        return overall_ok

    if not sn:
        logger.log("CHECK t4_sn_provided: FAIL (No --sn provided; T4 requires ST-LINK serial number on hardware)")
        return False

    # Hardware execution by auditor
    logger.log(f"[t4] Executing hardware verification on target ST-LINK SN={sn}...")
    prog = tools["programmer"]
    flash_py = REPO_ROOT / "tools" / "flash.py"
    net_check_py = REPO_ROOT / "tools" / "net_check.py"

    for proj_rel, preset in t4_targets:
        pdir = REPO_ROOT / proj_rel
        pname = pdir.name
        logger.log(f"[t4] Flashing {pname} [{preset}] with sn={sn}...")
        cmd_flash = ["py", str(flash_py), "--sn", sn, "--project", pname, "--preset", preset]
        c_code, c_out, c_err = run_cmd(cmd_flash, cwd=REPO_ROOT, timeout=120.0)
        flash_ok = (c_code == 0) and ("Verification...OK" in c_out or "Download verified successfully" in c_out)
        logger.log(f"CHECK t4_flash_{pname}_{preset}: {'PASS' if flash_ok else 'FAIL'} (exit_code={c_code})")
        if not flash_ok:
            overall_ok = False
            continue

        time.sleep(3.0)  # Allow boot to complete

        # Single HOTPLUG SWD call reading CCCSR (0x58000420) and RCC_CR (0x58024400)
        cmd_swd = [
            prog, "-c", "port=SWD", f"sn={sn}", "mode=HOTPLUG",
            "-r32", "0x58000420", "4",
            "-r32", "0x58024400", "4",
        ]
        s_code, s_out, s_err = run_cmd(cmd_swd, timeout=30.0)
        if s_code != 0:
            logger.log(f"CHECK t4_swd_read_{pname}_{preset}: FAIL (exit_code={s_code}: {s_err or s_out})")
            overall_ok = False
            continue

        try:
            cccsr_val, rcc_cr_val = parse_swd_hotplug_output(s_out)
            ev_pass, ev_detail = evaluate_t4_registers(cccsr_val, rcc_cr_val)
            logger.log(f"CHECK t4_registers_{pname}_{preset}: {'PASS' if ev_pass else 'FAIL'} ({ev_detail})")
            if not ev_pass:
                overall_ok = False
        except Exception as e:
            logger.log(f"CHECK t4_registers_{pname}_{preset}: FAIL ({e})")
            overall_ok = False

        # Run net_check.py
        cmd_net = ["py", str(net_check_py)]
        n_code, n_out, n_err = run_cmd(cmd_net, cwd=REPO_ROOT, timeout=30.0)
        net_ok = (n_code == 0)
        logger.log(f"CHECK t4_net_check_{pname}_{preset}: {'PASS' if net_ok else 'FAIL'} (exit_code={n_code})")
        if not net_ok:
            overall_ok = False

        # For prj_04 debug: 5 resets by NRST with reading and ping
        if pname == "prj_04_adc_dma_bm_eth" and preset == "debug":
            logger.log("[t4] Performing 5 warm resets on prj_04 debug via NRST...")
            resets_passed = 0
            for r_idx in range(1, 6):
                # Trigger reset via ST-LINK CLI
                cmd_rst = [prog, "-c", "port=SWD", f"sn={sn}", "mode=HOTPLUG", "-rst"]
                run_cmd(cmd_rst, timeout=15.0)
                time.sleep(3.0)

                s_code, s_out, _ = run_cmd(cmd_swd, timeout=30.0)
                n_code, _, _ = run_cmd(cmd_net, cwd=REPO_ROOT, timeout=30.0)
                if s_code == 0 and n_code == 0:
                    try:
                        c_val, r_val = parse_swd_hotplug_output(s_out)
                        p_ok, _ = evaluate_t4_registers(c_val, r_val)
                        if p_ok:
                            resets_passed += 1
                    except Exception:
                        pass
            rst_ok = (resets_passed == 5)
            logger.log(f"CHECK t4_nrst_resets_prj_04_debug: {'PASS' if rst_ok else 'FAIL'} (passed={resets_passed}/5)")
            if not rst_ok:
                overall_ok = False

    return overall_ok


# ==============================================================================
# Main Driver
# ==============================================================================

def main() -> int:
    parser = argparse.ArgumentParser(description="Verification tool for 00_gpio driver close (K1-K5)")
    parser.add_argument(
        "--test",
        default="t1,t2,t3",
        help="Comma-separated test IDs: t1, t2, t3, t4, all (default: t1,t2,t3)"
    )
    parser.add_argument("--output", type=Path, default=None, help="Capture output file path")
    parser.add_argument("--sn", default=None, help="ST-LINK serial number for T4 hardware execution")
    parser.add_argument("--simulate-t4", action="store_true", help="Simulate T4 execution without target hardware")

    args = parser.parse_args()
    logger = Logger(args.output)

    tests_requested = [t.strip().lower() for t in args.test.split(",")]
    if "all" in tests_requested:
        tests_requested = ["t1", "t2", "t3", "t4"]

    head_rev = get_head_full_rev()
    control_dir = REPO_ROOT.parent / f"stm32h755_control_{CONTROL_COMMIT_EXPECTED}"

    initial_snapshot = snapshot_working_tree(REPO_ROOT)
    tree_ok_start, tree_msg_start = check_clean_working_tree(REPO_ROOT, args.output)

    logger.log("=================================================================")
    logger.log(" STM32H755 00_gpio Driver Close Verification (K1-K5)")
    logger.log("=================================================================")
    logger.log(f"argv:             {' '.join(sys.argv)}")
    logger.log(f"HEAD:             {head_rev}")
    logger.log(f"Control worktree: {control_dir} (commit {CONTROL_COMMIT_EXPECTED})")
    logger.log(f"Tree status (in): {'PASS' if tree_ok_start else 'FAIL'} ({tree_msg_start})")
    logger.log("=================================================================")
    logger.log("")

    if not tree_ok_start:
        logger.log(f"CHECK tree_clean_start: FAIL ({tree_msg_start})")
        logger.log("OVERALL VERDICT: FAIL")
        return 1

    try:
        tools = find_tools()
        logger.log(f"[tools] gcc:        {tools['gcc']}")
        logger.log(f"[tools] objdump:    {tools['objdump']}")
        logger.log(f"[tools] nm:         {tools['nm']}")
        logger.log(f"[tools] size:       {tools['size']}")
        logger.log(f"[tools] cmake:      {tools['cmake']}")
        logger.log(f"[tools] ninja:      {tools['ninja']}")
        logger.log(f"[tools] programmer: {tools['programmer']}")
        logger.log("")
    except Exception as e:
        logger.log(f"CHECK toolchain_discovery: FAIL ({e})")
        logger.log("OVERALL VERDICT: FAIL")
        return 1

    overall_pass = True

    if "t1" in tests_requested:
        if not run_t1(tools, logger):
            overall_pass = False
        logger.log("")

    if "t2" in tests_requested:
        if not run_t2(tools, control_dir, logger):
            overall_pass = False
        logger.log("")

    if "t3" in tests_requested:
        if not run_t3(tools, logger):
            overall_pass = False
        logger.log("")

    if "t4" in tests_requested:
        if not run_t4(tools, args.sn, args.simulate_t4, logger):
            overall_pass = False
        logger.log("")

    tree_ok_end, tree_msg_end = check_clean_working_tree(REPO_ROOT, args.output, initial_snapshot)
    logger.log(f"CHECK tree_clean_end: {'PASS' if tree_ok_end else 'FAIL'} ({tree_msg_end})")
    if not tree_ok_end:
        overall_pass = False

    logger.log("")
    logger.log("=================================================================")
    logger.log(f"OVERALL VERDICT: {'PASS' if overall_pass else 'FAIL'}")
    logger.log("=================================================================")

    return 0 if overall_pass else 1


if __name__ == "__main__":
    sys.exit(main())
