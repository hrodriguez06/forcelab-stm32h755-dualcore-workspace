#!/usr/bin/env python3
"""Verification harness for STM32H7 Ethernet Driver E3-E7 phase (Revision 2).

Covers off-board tests T1, T2, T3, T4 and on-board auditor tests T5, T6:
  - T1: CI matrix build (8 configurations: prj_01..04 debug/release) compiled directly
        via cmake --preset and cmake --build --preset --clean-first.
        Checks: exit code 0, 0 compiler warnings, built objects count > 0, image freshness.
        Negative control: temporary C file outside repo compiled with real project flags
        producing a deliberate warning, routed through parse_compiler_warnings and evaluate_build_output.
  - T2: prj_03 and prj_04 unchanged:
        Ensures HEAD configs are fresh. Control 69179d7 compiled in-situ with canonical prefix map.
        Deep binary equivalence across 4 builds (prj_03 & prj_04, debug & release) for
        eth_hal.c.obj and ethernetif.c.obj:
          1) objdump -h: all ALLOC section headers (names, sizes, flags, including NOBITS .bss,
             .RxArraySection, .RxDescripSection, .TxDescripSection, .eth_ram).
          2) objdump -s: raw section contents for all non-debug sections.
          3) objdump -r: relocations for all non-debug code and data sections.
          4) nm -p: symbol table filtered to remove debug symbols.
        Negative control: mutated section entry detected and rejected.
  - T3: E3 compile-time callbacks opt-out:
        eth_hal.c compiled with -DETH_HAL_CUSTOM_CALLBACKS in a temporary directory outside repo.
        nm exit code verified == 0 and driver symbol ETH_Driver_Init confirmed present.
        nm verifies driver object defines none of the 6 HAL ETH callbacks.
        Partial link with a custom callbacks object succeeds (exit code 0).
        Negative control: without macro, partial link fails with duplicate symbol errors.
  - T4: prj_02 memory budget and zero-copy verification:
        From cm7_firmware.map: D2 SRAM3 (.eth_ram) total size <= 32 KB (32768 bytes).
        MPU Region 6 base address and size parsed dynamically from mpu_config.c.
        In both debug and release: g_rx_pool is 32-byte aligned and fully within Region 6.
        Disassembly of ethernetif_receive_one and static callees: zero calls to
        memcpy, __aeabi_memcpy*, pbuf_take, or pbuf_take_at.
        Negative controls: mutated .map with 33 KB section and mutated disassembly with
        injected __aeabi_memcpy call both fail through the respective evaluation functions.
  - T5: Hardware data path & throughput test for auditor (with local loopback mode for implementer):
        Compiles prj_02 debug in control worktree and HEAD, logging ELF SHA-256.
        Flashes each using its own flash.py.
        Runs monitor.py in background checking rx_nobuf=0, tx_nobuf=0, dma_err=0 across all [net] lines.
        net_check.py (ping 4/4, UDP sweep 1..1472 B, burst 100/100) with exit code check.
        UDP load test: sequential load (1-in-flight) and windowed load (32 datagrams in flight).
        Throughput measured as median of 3 repetitions; criterion: HEAD >= 0.98 * control.
        Negative controls: mutated payload byte rejected; synthetic non-zero monitor counters rejected.
  - T6: Hardware reset stress test for auditor:
        10 consecutive SWD resets (-rst), 5.0s link settling time, ping 4/4 and burst 100/100 echo datagrams.

Clean working tree (Revision 2):
  Only files inside build/ directories are permitted to be ignored or untracked.
  Outside build/, any new/untracked/ignored file or tracked modification is FAIL
  (ignoring only the exact output file if specified).
  All subprocesses enforce execution timeouts.
  Standard library only.
"""

from __future__ import annotations

import argparse
import hashlib
import os
import re
import shutil
import socket
import subprocess
import sys
sys.dont_write_bytecode = True
import tempfile
import threading
import time
from pathlib import Path
from typing import Optional

REPO_ROOT = Path(__file__).resolve().parent.parent
CONTROL_COMMIT_EXPECTED = "69179d7"
DEFAULT_SN = "0016000C4142501220353451"
DEFAULT_TARGET_IP = "192.168.1.150"

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

T2_CONFIGS = [
    ("02_firmware_projects/prj_03_adc_dma_eth", "debug"),
    ("02_firmware_projects/prj_03_adc_dma_eth", "release"),
    ("02_firmware_projects/prj_04_adc_dma_bm_eth", "debug"),
    ("02_firmware_projects/prj_04_adc_dma_bm_eth", "release"),
]

HAL_ETH_CALLBACKS_6 = [
    "HAL_ETH_RxAllocateCallback",
    "HAL_ETH_RxLinkCallback",
    "HAL_ETH_TxFreeCallback",
    "HAL_ETH_RxCpltCallback",
    "HAL_ETH_TxCpltCallback",
    "HAL_ETH_ErrorCallback",
]


def run_cmd(args: list[str], cwd: Path | None = None, timeout: float = 300.0, env: dict[str, str] | None = None) -> tuple[int, str, str]:
    """Runs a command with an explicit timeout."""
    try:
        res = subprocess.run(
            args,
            cwd=str(cwd) if cwd else None,
            capture_output=True,
            text=True,
            timeout=timeout,
            env=env,
        )
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


def sha256_file(path: Path) -> str:
    """Computes SHA-256 hexadecimal hash of a file."""
    if not path.is_file():
        return "not_found"
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


def get_head_full_rev(repo: Path = REPO_ROOT) -> str:
    """Returns full SHA for HEAD in the given repository."""
    code, out, _ = run_cmd(["git", "rev-parse", "HEAD"], cwd=repo, timeout=10.0)
    return out.strip() if code == 0 else "unknown"


def snapshot_working_tree(repo_root: Path) -> dict[str, str]:
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
    repo_root: Path,
    output_file: Path | None,
    initial_snapshot: dict[str, str] | None = None
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
        # Test capture files in 01_shared_drivers/08_eth/docs/ are permitted
        if (resolved_path.parent == (repo_root / "01_shared_drivers/08_eth/docs").resolve()
                and resolved_path.name.startswith("eth_e3e6_")):
            continue

        # Tracked modifications or untracked files outside build/
        if prefix in (" M", "M ", " D", "D ", "A ", "AM", "AD", "R ", "C ", "U ", "??"):
            dirty.append(f"{prefix} {rel_path}")
        elif initial_snapshot is not None:
            # If it's an ignored file (!!) or anything else that was NOT in the initial snapshot,
            # it was created during the test run outside build/!
            if rel_path not in initial_snapshot:
                dirty.append(f"NEW({prefix}) {rel_path}")

    if dirty:
        return False, f"Dirty tree outside build/ ({len(dirty)} items): {dirty}"
    return True, "Tree is clean outside build/"


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


# ==============================================================================
# T1: Matrix Compilation 8/8
# ==============================================================================

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
    expected_warnings: int = 0
) -> tuple[bool, list[str], str]:
    """Pure evaluation function used for real builds and synthetic warning negative control."""
    warnings = parse_compiler_warnings(build_output)
    passed = (
        (build_code == 0)
        and (len(warnings) == expected_warnings)
        and (built_objects > 0)
    )
    detail = f"code={build_code}, objs={built_objects}, warnings={len(warnings)}"
    return passed, warnings, detail


def check_image_freshness(pdir: Path, preset: str, start_time: float) -> tuple[bool, str]:
    """Verifies that an image was built during this execution."""
    cm7_elf = pdir / "build" / preset / "CM7" / "cm7_firmware.elf"
    if not cm7_elf.is_file():
        return False, f"ELF file not found at {cm7_elf}"

    mtime = cm7_elf.stat().st_mtime
    if mtime < start_time - 5.0:
        return False, f"ELF file is stale (mtime {mtime:.0f} < start {start_time:.0f})"

    return True, f"fresh (mtime={mtime:.0f})"


def compile_single_config(
    tools: dict[str, str],
    pdir: Path,
    preset: str,
    logger: Logger,
    env: dict[str, str] | None = None,
) -> tuple[bool, int, str, int]:
    """Configures and builds a single project preset directly via cmake."""
    pname = pdir.name
    cfg_cmd = [tools["cmake"], "--preset", preset, "--fresh"]
    cfg_code, cfg_out, cfg_err = run_cmd(cfg_cmd, cwd=pdir, timeout=120.0, env=env)
    if cfg_code != 0:
        logger.log(f"[host] FAIL: configure {pname} [{preset}] failed (code={cfg_code}): {cfg_err or cfg_out}")
        return False, cfg_code, cfg_out + "\n" + cfg_err, 0
    if "CMake Error" in (cfg_out + cfg_err):
        logger.log(f"[host] FAIL: configure {pname} [{preset}] reported error: {cfg_err or cfg_out}")
        return False, 1, cfg_out + "\n" + cfg_err, 0

    bld_code, bld_out, bld_err = run_cmd(
        [tools["cmake"], "--build", "--preset", preset, "--clean-first"],
        cwd=pdir,
        timeout=300.0,
        env=env,
    )
    full_out = bld_out + "\n" + bld_err
    objs = count_built_objects(full_out)
    passed, warnings, detail = evaluate_build_output(pname, preset, bld_code, full_out, objs)
    return passed, bld_code, full_out, objs


def run_t1(tools: dict[str, str], logger: Logger) -> bool:
    logger.log("=== Running T1 Checks (Build Matrix 8/8) ===")
    overall_ok = True
    matrix_passed_count = 0

    logger.log("[check] Compiling 8 matrix configurations directly via cmake --clean-first...")

    for proj_rel, preset in MATRIX_CONFIGS:
        pdir = REPO_ROOT / proj_rel
        pname = pdir.name
        passed, bld_code, full_out, objs = compile_single_config(tools, pdir, preset, logger)
        warnings = parse_compiler_warnings(full_out)
        fresh_ok, fresh_msg = check_image_freshness(pdir, preset, RUN_START_TIME)

        config_ok = passed and fresh_ok
        detail = f"code={bld_code}, objs={objs}, warnings={len(warnings)}, image={fresh_msg}"
        logger.log(f"[host] CHECK matrix {pname} [{preset}]: {'PASS' if config_ok else 'FAIL'} ({detail})")

        if config_ok:
            matrix_passed_count += 1
        else:
            overall_ok = False

    matrix_ok = (matrix_passed_count == 8)
    logger.log(f"[host] CHECK T1 matrix 8/8 (clean-first, 0 warnings): {'PASS' if matrix_ok else 'FAIL'} ({matrix_passed_count}/8 passed)")
    if not matrix_ok:
        overall_ok = False

    # Negative control: deliberate compiler warning in temporary C file compiled outside repo using real project flags
    with tempfile.TemporaryDirectory(prefix="eth_t1_neg_") as tmp_dir:
        tmp_path = Path(tmp_dir)
        c_file = tmp_path / "warning_sample.c"
        c_obj = tmp_path / "warning_sample.o"
        c_file.write_text(
            "#include <stdint.h>\n"
            "int test_deliberate_warning(void) {\n"
            "    int unused_warning_var = 1234;\n"
            "    return 0;\n"
            "}\n",
            encoding="utf-8"
        )
        prj_root = REPO_ROOT / "02_firmware_projects" / "prj_02_eth_lwip_dualcore"
        cube = REPO_ROOT / "02_firmware_projects" / "prj_eth_lwip_node" / "external" / "STM32CubeH7"
        project_cflags = [
            "-mcpu=cortex-m7", "-mthumb", "-mfpu=fpv5-d16", "-mfloat-abi=hard",
            "-Og", "-g3",
            "-Wall", "-Wextra",
            f"-I{prj_root}/CM7/Core/Inc",
            f"-I{cube}/Drivers/CMSIS/Device/ST/STM32H7xx/Include",
            f"-I{cube}/Drivers/CMSIS/Core/Include",
            f"-I{cube}/Drivers/STM32H7xx_HAL_Driver/Inc",
            "-DSTM32H755xx", "-DCORE_CM7", "-DUSE_HAL_DRIVER",
        ]
        cmd = [tools["gcc"], "-c", str(c_file), "-o", str(c_obj)] + project_cflags
        comp_code, comp_out, comp_err = run_cmd(cmd, cwd=tmp_path, timeout=30.0)
        full_neg_out = comp_out + "\n" + comp_err
        neg_objs = 1 if c_obj.is_file() else 0
        neg_pass, neg_warnings, neg_detail = evaluate_build_output(
            "neg_control", "warning_sample", comp_code, full_neg_out, neg_objs, expected_warnings=0
        )
        neg_ok = (not neg_pass) and (len(neg_warnings) > 0)
        logger.log(f"[host] CHECK T1 negative control (project flags deliberate warning detected & rejected): {'PASS' if neg_ok else 'FAIL'} (warnings={len(neg_warnings)})")
        if not neg_ok:
            overall_ok = False

    return overall_ok


# ==============================================================================
# T2: prj_03 and prj_04 Unchanged vs Control 69179d7 (Deep Binary Equivalence)
# ==============================================================================

def extract_alloc_section_headers(tools: dict[str, str], obj_path: Path) -> dict[str, tuple[str, str, str, str, str]]:
    """Extracts all ALLOC section headers via arm-none-eabi-objdump -h:
    Includes .text*, .rodata*, .data*, .bss*, .RxArraySection, .RxDescripSection,
    .TxDescripSection, .eth_ram with exact sizes, VMAs, LMAs, alignments, and flags.
    """
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


def run_t2(tools: dict[str, str], control_dir: Path, logger: Logger) -> bool:
    logger.log("=== Running T2 Checks (prj_03 & prj_04 Unchanged vs Control 69179d7) ===")
    ctrl_ok, ctrl_msg = ensure_control_worktree(control_dir, logger)
    logger.log(f"[host] Control worktree setup: {'PASS' if ctrl_ok else 'FAIL'} ({ctrl_msg})")
    if not ctrl_ok:
        return False

    overall_ok = True

    # 1. Compile HEAD prj_03 and prj_04 in-situ if not already built fresh
    logger.log("[check] Ensuring HEAD prj_03 and prj_04 are compiled fresh in this execution...")
    for proj_rel, preset in T2_CONFIGS:
        pdir_head = REPO_ROOT / proj_rel
        fresh_ok, _ = check_image_freshness(pdir_head, preset, RUN_START_TIME)
        if not fresh_ok:
            passed_h, _, _, _ = compile_single_config(tools, pdir_head, preset, logger)
            if not passed_h:
                logger.log(f"[host] ERROR compiling HEAD {pdir_head.name} [{preset}]")
                overall_ok = False

    if not overall_ok:
        return False

    # 2. Compile control configurations in-situ with canonical prefix map
    logger.log("[check] Compiling 4 control configurations (prj_03 & prj_04, debug & release) in-situ...")
    ctrl_env = os.environ.copy()
    c_from = control_dir.as_posix()
    c_to = REPO_ROOT.as_posix()
    ctrl_env["CFLAGS"] = f"-ffile-prefix-map={c_from}={c_to}"

    for proj_rel, preset in T2_CONFIGS:
        pdir_ctrl = control_dir / proj_rel
        passed_ctrl, _, _, _ = compile_single_config(tools, pdir_ctrl, preset, logger, env=ctrl_env)
        if not passed_ctrl:
            logger.log(f"[host] ERROR: Control compilation failed for {pdir_ctrl.name} [{preset}]")
            overall_ok = False

    if not overall_ok:
        return False

    # 3. Deep compare 4 configs x 2 objects = 8 objects
    logger.log("[check] Deep comparing ALLOC headers, raw contents, relocations and symbols across 8 objects...")
    matches_count = 0

    sample_hdrs_h = None
    sample_hdrs_c = None
    sample_raw_h = None
    sample_raw_c = None
    sample_relocs_h = None
    sample_relocs_c = None
    sample_syms_h = None
    sample_syms_c = None

    for proj_rel, preset in T2_CONFIGS:
        pdir_head = REPO_ROOT / proj_rel
        pdir_ctrl = control_dir / proj_rel
        pname = pdir_head.name

        for obj_name in ("eth_hal.c.obj", "ethernetif.c.obj"):
            try:
                head_objs = list((pdir_head / "build" / preset / "CM7").rglob(obj_name))
                ctrl_objs = list((pdir_ctrl / "build" / preset / "CM7").rglob(obj_name))

                if not head_objs or not ctrl_objs:
                    logger.log(f"[host] CHECK {pname} [{preset}] {obj_name}: FAIL (object file missing)")
                    overall_ok = False
                    continue

                hdrs_h = extract_alloc_section_headers(tools, head_objs[0])
                hdrs_c = extract_alloc_section_headers(tools, ctrl_objs[0])

                raw_h = extract_nondebug_raw_contents(tools, head_objs[0])
                raw_c = extract_nondebug_raw_contents(tools, ctrl_objs[0])

                relocs_h = extract_nondebug_relocations(tools, head_objs[0])
                relocs_c = extract_nondebug_relocations(tools, ctrl_objs[0])

                syms_h = extract_nondebug_symbols(tools, head_objs[0])
                syms_c = extract_nondebug_symbols(tools, ctrl_objs[0])

                if sample_hdrs_h is None:
                    sample_hdrs_h = dict(hdrs_h)
                    sample_hdrs_c = dict(hdrs_c)
                    sample_raw_h = dict(raw_h)
                    sample_raw_c = dict(raw_c)
                    sample_relocs_h = dict(relocs_h)
                    sample_relocs_c = dict(relocs_c)
                    sample_syms_h = list(syms_h)
                    sample_syms_c = list(syms_c)

                match_ok, match_detail = compare_object_equivalence(
                    hdrs_h, hdrs_c, raw_h, raw_c, relocs_h, relocs_c, syms_h, syms_c
                )
                logger.log(f"[host] CHECK {pname} [{preset}] {obj_name}: {'PASS' if match_ok else 'FAIL'} ({match_detail})")

                if match_ok:
                    matches_count += 1
                else:
                    overall_ok = False
            except Exception as e:
                logger.log(f"[host] CHECK {pname} [{preset}] {obj_name}: FAIL (exception: {e})")
                overall_ok = False

    t2_pass = (matches_count == 8)
    logger.log(f"[host] CHECK T2 all 8 objects identical to control: {'PASS' if t2_pass else 'FAIL'} ({matches_count}/8 passed)")
    if not t2_pass:
        overall_ok = False

    # Negative control for T2: mutate 1 section size in header dictionary
    if sample_hdrs_h and sample_hdrs_c:
        mutated_hdrs = dict(sample_hdrs_c)
        target_k = sorted(mutated_hdrs.keys())[0]
        orig_val = mutated_hdrs[target_k]
        mutated_hdrs[target_k] = ("0000ffff", orig_val[1], orig_val[2], orig_val[3], orig_val[4])

        neg_match, neg_detail = compare_object_equivalence(
            sample_hdrs_h, mutated_hdrs,
            sample_raw_h, sample_raw_c,
            sample_relocs_h, sample_relocs_c,
            sample_syms_h, sample_syms_c
        )
        neg_detected = (not neg_match) and ("mismatch" in neg_detail)
        logger.log(f"[host] CHECK T2 negative control (mutated section header size detected & rejected): {'PASS' if neg_detected else 'FAIL'} ({neg_detail})")
        if not neg_detected:
            overall_ok = False

    return overall_ok


# ==============================================================================
# T3: E3 Callbacks Opt-Out Compilation & Linkage
# ==============================================================================

def evaluate_link_result(returncode: int, stderr_out: str, expected_success: bool) -> tuple[bool, str]:
    """Pure evaluation of arm-none-eabi-gcc -r linkage."""
    if expected_success:
        ok = (returncode == 0)
        return ok, f"exit code {returncode}"
    else:
        ok = (returncode != 0) and ("multiple definition" in stderr_out)
        return ok, f"exit code {returncode}, multiple definition detected"


def run_t3(tools: dict[str, str], logger: Logger) -> bool:
    logger.log("=== Running T3 Checks (E3 Callbacks Opt-Out & Linkage) ===")
    overall_ok = True

    prj_root = REPO_ROOT / "02_firmware_projects" / "prj_02_eth_lwip_dualcore"
    cube = REPO_ROOT / "02_firmware_projects" / "prj_eth_lwip_node" / "external" / "STM32CubeH7"

    inc = [
        f"-I{REPO_ROOT}/01_shared_drivers/08_eth",
        f"-I{REPO_ROOT}/01_shared_drivers/00_gpio",
        f"-I{prj_root}/Common/Inc",
        f"-I{prj_root}/CM7/Core/Inc",
        f"-I{cube}/Drivers/CMSIS/Device/ST/STM32H7xx/Include",
        f"-I{cube}/Drivers/CMSIS/Core/Include",
        f"-I{cube}/Drivers/STM32H7xx_HAL_Driver/Inc",
    ]
    defs = [
        "-DSTM32H755xx",
        "-DCORE_CM7",
        "-DUSE_HAL_DRIVER",
        '-DCMSIS_device_header="stm32h7xx.h"',
    ]

    with tempfile.TemporaryDirectory(prefix="eth_t3_") as tmp_dir:
        tmp = Path(tmp_dir)

        # 1. Custom callbacks source defining all 6 callbacks
        cb_c = tmp / "custom_callbacks.c"
        cb_c.write_text(
            "#include <stdint.h>\n"
            "typedef void ETH_HandleTypeDef;\n"
            "void HAL_ETH_RxAllocateCallback(uint8_t **buff) { (void)buff; }\n"
            "void HAL_ETH_RxLinkCallback(void **pStart, void **pEnd, uint8_t *buff, uint16_t Length) { (void)pStart; (void)pEnd; (void)buff; (void)Length; }\n"
            "void HAL_ETH_TxFreeCallback(uint32_t *buff) { (void)buff; }\n"
            "void HAL_ETH_RxCpltCallback(ETH_HandleTypeDef *heth) { (void)heth; }\n"
            "void HAL_ETH_TxCpltCallback(ETH_HandleTypeDef *heth) { (void)heth; }\n"
            "void HAL_ETH_ErrorCallback(ETH_HandleTypeDef *heth) { (void)heth; }\n",
            encoding="utf-8"
        )
        cb_obj = tmp / "custom_callbacks.o"
        code_cb, _, err_cb = run_cmd([tools["gcc"], "-c", str(cb_c), "-o", str(cb_obj), "-mcpu=cortex-m7", "-mthumb"], cwd=tmp)
        if code_cb != 0:
            logger.log(f"[host] ERROR compiling custom callbacks: {err_cb}")
            return False

        # 2. Compile eth_hal.c with -DETH_HAL_CUSTOM_CALLBACKS
        obj_with = tmp / "eth_hal_with_macro.o"
        cmd_with = [
            tools["gcc"], "-c", str(REPO_ROOT / "01_shared_drivers/08_eth/eth_hal.c"),
            "-o", str(obj_with), "-mcpu=cortex-m7", "-mthumb"
        ] + inc + defs + ["-DETH_HAL_CUSTOM_CALLBACKS"]
        code_with, out_with, err_with = run_cmd(cmd_with, cwd=tmp)
        if code_with != 0:
            logger.log(f"[host] ERROR compiling eth_hal.c with macro: {err_with}")
            return False

        # Check nm output: exit code must be 0 and strong driver symbol ETH_Driver_Init present
        code_nm, out_nm, err_nm = run_cmd([tools["nm"], str(obj_with)], cwd=tmp)
        nm_valid = (code_nm == 0) and ("ETH_Driver_Init" in out_nm)
        logger.log(f"[host] CHECK nm on eth_hal.c object (rc==0 and ETH_Driver_Init present): {'PASS' if nm_valid else 'FAIL'}")
        if not nm_valid:
            overall_ok = False

        defined_callbacks = []
        for line in out_nm.splitlines():
            for cb in HAL_ETH_CALLBACKS_6:
                if re.search(rf"\b[TtDd]\s+{cb}\b", line):
                    defined_callbacks.append(cb)

        nm_ok = nm_valid and (len(defined_callbacks) == 0)
        logger.log(f"[host] CHECK eth_hal.c -DETH_HAL_CUSTOM_CALLBACKS (0 of 6 callbacks defined in object): {'PASS' if nm_ok else 'FAIL'} (defined={defined_callbacks})")
        if not nm_ok:
            overall_ok = False

        # Partial link between eth_hal_with_macro.o and custom_callbacks.o
        linked_ok_obj = tmp / "linked_ok.o"
        code_link_ok, out_link_ok, err_link_ok = run_cmd([
            tools["gcc"], "-r", "-o", str(linked_ok_obj),
            str(obj_with), str(cb_obj), "-mcpu=cortex-m7", "-mthumb"
        ], cwd=tmp)
        link_pass, link_detail = evaluate_link_result(code_link_ok, err_link_ok, expected_success=True)
        logger.log(f"[host] CHECK partial link with custom callbacks (exit code 0): {'PASS' if link_pass else 'FAIL'} ({link_detail})")
        if not link_pass:
            overall_ok = False

        # Negative control: compile eth_hal.c WITHOUT macro, partial link must fail with duplicate symbols
        obj_without = tmp / "eth_hal_without_macro.o"
        cmd_without = [
            tools["gcc"], "-c", str(REPO_ROOT / "01_shared_drivers/08_eth/eth_hal.c"),
            "-o", str(obj_without), "-mcpu=cortex-m7", "-mthumb"
        ] + inc + defs
        code_without, _, err_without = run_cmd(cmd_without, cwd=tmp)
        if code_without != 0:
            logger.log(f"[host] ERROR compiling eth_hal.c without macro: {err_without}")
            return False

        linked_fail_obj = tmp / "linked_fail.o"
        code_link_fail, out_link_fail, err_link_fail = run_cmd([
            tools["gcc"], "-r", "-o", str(linked_fail_obj),
            str(obj_without), str(cb_obj), "-mcpu=cortex-m7", "-mthumb"
        ], cwd=tmp)
        neg_pass, neg_detail = evaluate_link_result(code_link_fail, err_link_fail, expected_success=False)
        logger.log(f"[host] CHECK T3 negative control (link without macro fails with multiple definition): {'PASS' if neg_pass else 'FAIL'} ({neg_detail})")
        if not neg_pass:
            overall_ok = False

    return overall_ok


# ==============================================================================
# T4: Memory Budget of prj_02 and Zero-Copy Disassembly
# ==============================================================================

def evaluate_map_eth_ram_size(map_content: str, max_allowed: int = 32768) -> tuple[bool, int, str]:
    """Pure parser: extracts D2 SRAM3 (.eth_ram) total size from linker map file."""
    m = re.search(r"^\s*\.eth_ram\s+0x[0-9a-fA-F]+\s+0x([0-9a-fA-F]+)", map_content, re.MULTILINE)
    if m:
        size_found = int(m.group(1), 16)
    else:
        m2 = re.search(r"RAM_D2_ETH:\s+(\d+)\s+B", map_content)
        if m2:
            size_found = int(m2.group(1))
        else:
            return False, 0, "Could not determine .eth_ram size from map file"

    passed = (size_found <= max_allowed)
    detail = f"SRAM3 size = {size_found} B / {max_allowed} B ({size_found / max_allowed * 100:.2f}%)"
    return passed, size_found, detail


def parse_mpu_region6_bounds(mpu_c_text: str) -> tuple[int, int]:
    """Dynamically parses MPU Region 6 base address and size from mpu_config.c."""
    m = re.search(
        r"MPU_Region\s*\(\s*MPU_REGION_NUMBER6\s*,\s*(0x[0-9a-fA-F]+)\s*,\s*MPU_REGION_SIZE_(\d+)([KMG]B)",
        mpu_c_text
    )
    if not m:
        raise RuntimeError("Could not parse MPU Region 6 from mpu_config.c")

    base = int(m.group(1), 16)
    val = int(m.group(2))
    unit = m.group(3).upper()
    mult = 1024 if unit == "KB" else (1024 * 1024 if unit == "MB" else 1024 * 1024 * 1024)
    size = val * mult
    return base, size


def evaluate_rx_disassembly(disasm_text: str) -> tuple[bool, list[str]]:
    """Pure checker: ensures no calls to memcpy, __aeabi_memcpy*, pbuf_take, or pbuf_take_at
    exist in the receive path (ethernetif_receive_one, ethernetif_input_thread, ethernetif_rx_pbuf_free_custom).
    """
    rx_functions = {"ethernetif_receive_one", "ethernetif_input_thread", "ethernetif_rx_pbuf_free_custom"}
    forbidden = ["memcpy", "__aeabi_memcpy", "pbuf_take", "pbuf_take_at"]
    detected = []
    in_fn = False
    for line in disasm_text.splitlines():
        m_func = re.search(r"<([a-zA-Z0-9_]+)>:", line)
        if m_func:
            in_fn = m_func.group(1) in rx_functions

        if in_fn:
            for f in forbidden:
                if re.search(rf"\b(bl|b\.w)\b.*<{f}", line):
                    detected.append(line.strip())

    passed = (len(detected) == 0)
    return passed, detected


def run_t4(tools: dict[str, str], logger: Logger) -> bool:
    logger.log("=== Running T4 Checks (prj_02 Memory Budget & Zero-Copy Disassembly) ===")
    overall_ok = True

    prj_dir = REPO_ROOT / "02_firmware_projects/prj_02_eth_lwip_dualcore"

    # 1. Ensure prj_02 debug is fresh
    fresh_dbg, _ = check_image_freshness(prj_dir, "debug", RUN_START_TIME)
    if not fresh_dbg:
        compile_single_config(tools, prj_dir, "debug", logger)

    map_path = prj_dir / "build/debug/cm7_firmware.map"
    if not map_path.is_file():
        map_path = prj_dir / "build/debug/CM7/cm7_firmware.map"
    if not map_path.is_file():
        logger.log(f"[host] ERROR: Map file not found at {map_path}")
        return False

    map_text = map_path.read_text(encoding="utf-8", errors="replace")

    # SRAM3 usage <= 32 KB
    map_ok, ram_size, ram_detail = evaluate_map_eth_ram_size(map_text, max_allowed=32768)
    logger.log(f"[host] CHECK prj_02 D2 SRAM3 usage (<= 32 KB): {'PASS' if map_ok else 'FAIL'} ({ram_detail})")
    if not map_ok:
        overall_ok = False

    # 2. Dynamic MPU Region 6 bounds from mpu_config.c
    mpu_c_path = prj_dir / "CM7/App/Src/mpu_config.c"
    mpu_text = mpu_c_path.read_text(encoding="utf-8", errors="replace")
    r6_base, r6_size = parse_mpu_region6_bounds(mpu_text)
    logger.log(f"[host] Parsed MPU Region 6 from mpu_config.c: base=0x{r6_base:08x}, size={r6_size} B")

    # Check g_rx_pool alignment and start/end bounds in BOTH debug and release
    for cfg in ("debug", "release"):
        elf_path = prj_dir / f"build/{cfg}/CM7/cm7_firmware.elf"
        if not elf_path.is_file():
            compile_single_config(tools, prj_dir, cfg, logger)

        code_nm, nm_out, _ = run_cmd([tools["nm"], "-n", str(elf_path)])
        rx_pool_addr = None
        for line in nm_out.splitlines():
            if " g_rx_pool" in line:
                rx_pool_addr = int(line.split()[0], 16)
                break

        if rx_pool_addr is None:
            logger.log(f"[host] CHECK [{cfg}] g_rx_pool symbol in ELF: FAIL (not found)")
            overall_ok = False
        else:
            aligned_32 = (rx_pool_addr % 32 == 0)
            pool_bytes = 12 * 1536
            rx_pool_end = rx_pool_addr + pool_bytes
            in_r6 = (r6_base <= rx_pool_addr) and (rx_pool_end <= r6_base + r6_size)
            cfg_ok = aligned_32 and in_r6
            logger.log(f"[host] CHECK [{cfg}] g_rx_pool 32-byte aligned & in MPU Region 6: {'PASS' if cfg_ok else 'FAIL'} (addr=0x{rx_pool_addr:08x}..0x{rx_pool_end:08x}, align32={aligned_32}, in_r6={in_r6})")
            if not cfg_ok:
                overall_ok = False

    # 3. Disassembly of ethernetif_receive_one and static callees in ethernetif.c.obj
    for cfg in ("debug", "release"):
        ethif_objs = list((prj_dir / f"build/{cfg}/CM7").rglob("ethernetif.c.obj"))
        if not ethif_objs:
            logger.log(f"[host] CHECK [{cfg}] ethernetif.c.obj exists: FAIL")
            return False

        code_dis, dis_out, _ = run_cmd([tools["objdump"], "-d", str(ethif_objs[0])])
        dis_ok, forbidden_detected = evaluate_rx_disassembly(dis_out)
        logger.log(f"[host] CHECK [{cfg}] zero-copy disassembly (no memcpy/__aeabi_memcpy/pbuf_take calls): {'PASS' if dis_ok else 'FAIL'} (detected={forbidden_detected})")
        if not dis_ok:
            overall_ok = False

    # 4. Negative control for .map: mutated map with 33 KB section (0x8400 = 33792 B)
    mutated_map = re.sub(
        r"^(\s*\.eth_ram\s+0x[0-9a-fA-F]+\s+)0x[0-9a-fA-F]+",
        r"\g<1>0x8400",
        map_text,
        flags=re.MULTILINE
    )
    neg_map_pass, neg_sz, neg_detail = evaluate_map_eth_ram_size(mutated_map, max_allowed=32768)
    neg_map_detected = (not neg_map_pass) and (neg_sz > 32768)
    logger.log(f"[host] CHECK T4 negative control (.map 33 KB detected & rejected): {'PASS' if neg_map_detected else 'FAIL'} ({neg_detail})")
    if not neg_map_detected:
        overall_ok = False

    # 5. Negative control for disassembly: injected __aeabi_memcpy call in receive path
    injected_dis = dis_out.replace(
        "bl\t0 <ETH_Driver_ReleaseRxBuffer>",
        "bl\t0 <ETH_Driver_ReleaseRxBuffer>\n  12:\tf7ff fffe\tbl\t0 <__aeabi_memcpy>"
    )
    if injected_dis == dis_out:
        injected_dis = re.sub(
            r"(<(?:ethernetif_receive_one|ethernetif_input_thread|ethernetif_rx_pbuf_free_custom)>:)",
            r"\1\n  12:\tf7ff fffe\tbl\t0 <__aeabi_memcpy>",
            dis_out,
            count=1
        )
    neg_dis_pass, neg_calls = evaluate_rx_disassembly(injected_dis)
    neg_dis_detected = (not neg_dis_pass) and any("__aeabi_memcpy" in c for c in neg_calls)
    logger.log(f"[host] CHECK T4 negative control (injected __aeabi_memcpy in disasm detected & rejected): {'PASS' if neg_dis_detected else 'FAIL'} ({neg_calls})")
    if not neg_dis_detected:
        overall_ok = False

    return overall_ok


# ==============================================================================
# T5: UDP Load Test & Hardware Verification (Revision 2)
# ==============================================================================

def verify_udp_payload(sent: bytes, received: bytes) -> bool:
    """Pure comparison for sent and received UDP echo payload."""
    return sent == received


def evaluate_monitor_net_lines(log_text: str) -> tuple[bool, int, str]:
    """Pure evaluator: parses [net] status lines from monitor.py log,
    verifying rx_nobuf == 0, tx_nobuf == 0, and dma_err == 0 across all logged samples.
    """
    net_lines_count = 0
    errors = []
    pattern = re.compile(r"\[net\].*?\brx_nobuf=(\d+)\s+tx_nobuf=(\d+)\s+dma_err=(\d+)")

    for line in log_text.splitlines():
        m = pattern.search(line)
        if m:
            net_lines_count += 1
            rx_nobuf = int(m.group(1))
            tx_nobuf = int(m.group(2))
            dma_err = int(m.group(3))
            if rx_nobuf != 0 or tx_nobuf != 0 or dma_err != 0:
                errors.append(f"rx_nobuf={rx_nobuf}, tx_nobuf={tx_nobuf}, dma_err={dma_err}")

    if errors:
        return False, net_lines_count, f"Non-zero error counters in {len(errors)} samples: {errors[:3]}"
    return True, net_lines_count, f"All {net_lines_count} [net] samples clean (0 errors)"


def run_udp_sequential_load(
    host: str,
    port: int,
    count: int = 10000,
    payload_size: int = 1472,
    timeout: float = 1.0,
    max_consecutive_timeouts: int = 50,
    total_deadline: float = 60.0,
    logger: Logger | None = None
) -> tuple[bool, int, int, float, float]:
    """Sequential (1-in-flight) UDP echo load test with sequence numbering and timeout guard."""
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.settimeout(timeout)

    sent_count = 0
    received_count = 0
    corrupt_count = 0
    total_bytes = 0
    consec_timeouts = 0

    t0 = time.perf_counter()
    deadline = t0 + total_deadline

    for seq in range(count):
        if time.perf_counter() > deadline:
            if logger:
                logger.log(f"[udp_seq] Total deadline {total_deadline}s exceeded")
            break

        prefix = f"ETH-E6-SEQ-{seq:06d}-".encode("ascii")
        pad_len = payload_size - len(prefix)
        pattern = bytes((i * 11 + seq) & 0xFF for i in range(pad_len))
        payload = prefix + pattern

        try:
            sock.sendto(payload, (host, port))
            sent_count += 1
            reply, _ = sock.recvfrom(payload_size + 512)
            consec_timeouts = 0
            if verify_udp_payload(payload, reply):
                received_count += 1
                total_bytes += len(reply)
            else:
                corrupt_count += 1
        except socket.timeout:
            consec_timeouts += 1
            if consec_timeouts >= max_consecutive_timeouts:
                if logger:
                    logger.log(f"[udp_seq] Aborted after {consec_timeouts} consecutive timeouts")
                break

    t_elapsed = time.perf_counter() - t0
    sock.close()

    throughput_mbps = (total_bytes * 8.0) / (t_elapsed * 1_000_000.0) if t_elapsed > 0 else 0.0
    passed = (sent_count == count) and (received_count == count) and (corrupt_count == 0)

    if logger:
        logger.log(f"[udp_seq] Sent: {sent_count}/{count}, Recv: {received_count}/{count}, Corrupt: {corrupt_count}, "
                   f"Time: {t_elapsed:.2f}s, Throughput: {throughput_mbps:.2f} Mbit/s")

    return passed, sent_count, received_count, throughput_mbps, t_elapsed


def run_udp_windowed_load(
    host: str,
    port: int,
    count: int = 5000,
    window_size: int = 32,
    payload_size: int = 1472,
    timeout: float = 0.5,
    max_consecutive_timeouts: int = 50,
    total_deadline: float = 60.0,
    logger: Logger | None = None
) -> tuple[bool, int, int, float, float]:
    """Windowed (32 datagrams in flight) UDP echo load test to stress pool exhaustion and re-arming."""
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.settimeout(timeout)

    inflight: dict[int, bytes] = {}
    next_seq = 0
    received_count = 0
    corrupt_count = 0
    total_bytes = 0
    consec_timeouts = 0

    t0 = time.perf_counter()
    deadline = t0 + total_deadline

    while received_count < count:
        if time.perf_counter() > deadline:
            if logger:
                logger.log(f"[udp_win] Total deadline {total_deadline}s exceeded")
            break

        # Send packets up to window limit
        while len(inflight) < window_size and next_seq < count:
            prefix = f"ETH-E6-SEQ-{next_seq:06d}-".encode("ascii")
            pad_len = payload_size - len(prefix)
            pattern = bytes((i * 11 + next_seq) & 0xFF for i in range(pad_len))
            p = prefix + pattern
            try:
                sock.sendto(p, (host, port))
                inflight[next_seq] = p
                next_seq += 1
            except OSError:
                break

        try:
            reply, _ = sock.recvfrom(payload_size + 512)
            consec_timeouts = 0
            if len(reply) >= 21 and reply.startswith(b"ETH-E6-SEQ-"):
                seq = int(reply[11:17].decode("ascii"))
                if seq in inflight:
                    if reply == inflight[seq]:
                        received_count += 1
                        total_bytes += len(reply)
                    else:
                        corrupt_count += 1
                    del inflight[seq]
        except socket.timeout:
            consec_timeouts += 1
            if consec_timeouts >= max_consecutive_timeouts:
                if logger:
                    logger.log(f"[udp_win] Aborted after {consec_timeouts} consecutive timeouts")
                break

    t_elapsed = time.perf_counter() - t0
    sock.close()

    throughput_mbps = (total_bytes * 8.0) / (t_elapsed * 1_000_000.0) if t_elapsed > 0 else 0.0
    passed = (received_count == count) and (corrupt_count == 0)

    if logger:
        logger.log(f"[udp_win] In-flight={window_size}, Sent: {next_seq}/{count}, Recv: {received_count}/{count}, "
                   f"Corrupt: {corrupt_count}, Time: {t_elapsed:.2f}s, Throughput: {throughput_mbps:.2f} Mbit/s")

    return passed, next_seq, received_count, throughput_mbps, t_elapsed


class LocalUdpEchoServer:
    def __init__(self, host: str = "127.0.0.1", port: int = 0) -> None:
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sock.bind((host, port))
        self.host, self.port = self.sock.getsockname()
        self.running = True
        self.thread = threading.Thread(target=self._serve, daemon=True)
        self.thread.start()

    def _serve(self) -> None:
        while self.running:
            try:
                self.sock.settimeout(0.2)
                data, addr = self.sock.recvfrom(4096)
                if data:
                    self.sock.sendto(data, addr)
            except (socket.timeout, OSError):
                continue

    def stop(self) -> None:
        self.running = False
        try:
            self.sock.close()
        except Exception:
            pass
        self.thread.join(timeout=2.0)


def run_t5(
    tools: dict[str, str],
    control_dir: Path,
    sn: str,
    target_ip: str,
    local_only: bool,
    logger: Logger
) -> bool:
    logger.log(f"=== Running T5 Checks (UDP Load Test & Hardware Verification: {'LOCAL_LOOPBACK' if local_only else 'HARDWARE'}) ===")
    overall_ok = True

    # 1. Negative control for payload verification
    sample_sent = b"Hello STM32H7 Ethernet Zero-Copy Test Payload!"
    sample_recv_bad = bytearray(sample_sent)
    sample_recv_bad[-1] ^= 0xFF
    neg_verif = verify_udp_payload(sample_sent, bytes(sample_recv_bad))
    logger.log(f"[host] CHECK T5 negative control (mutated payload detected & rejected): {'PASS' if not neg_verif else 'FAIL'}")
    if neg_verif:
        overall_ok = False

    # 2. Negative control for monitor log evaluation
    synth_bad_log = (
        "[+   1.000s] [net] link=100M_FD ip=192.168.1.150 rx=100 tx=100 rx_nobuf=0 tx_nobuf=0 dma_err=0\n"
        "[+   2.000s] [net] link=100M_FD ip=192.168.1.150 rx=500 tx=500 rx_nobuf=5 tx_nobuf=0 dma_err=0\n"
    )
    mon_neg_pass, mon_neg_cnt, mon_neg_msg = evaluate_monitor_net_lines(synth_bad_log)
    mon_neg_ok = (not mon_neg_pass) and ("rx_nobuf=5" in mon_neg_msg)
    logger.log(f"[host] CHECK T5 negative control (synthetic non-zero monitor counters detected & rejected): {'PASS' if mon_neg_ok else 'FAIL'}")
    if not mon_neg_ok:
        overall_ok = False

    if local_only:
        # Off-board implementer test against local 127.0.0.1 UDP echo server
        logger.log("[check] Starting local UDP echo server on 127.0.0.1 to verify sequential and windowed load engines...")
        server = LocalUdpEchoServer()
        logger.log(f"[host] Local echo server running on {server.host}:{server.port}")
        try:
            # Test 1: Sequential 10,000 datagrams
            pass_seq, sent_seq, recv_seq, mbps_seq, _ = run_udp_sequential_load(
                server.host, server.port, count=10000, payload_size=1472, timeout=1.0, logger=logger
            )
            logger.log(f"[host] CHECK local sequential UDP load 10000/10000 @ 1472 B: {'PASS' if pass_seq else 'FAIL'} ({mbps_seq:.2f} Mbit/s)")
            if not pass_seq:
                overall_ok = False

            # Test 2: Windowed 32-in-flight 5,000 datagrams
            pass_win, sent_win, recv_win, mbps_win, _ = run_udp_windowed_load(
                server.host, server.port, count=5000, window_size=32, payload_size=1472, timeout=0.5, logger=logger
            )
            logger.log(f"[host] CHECK local windowed (32 in flight) UDP load 5000/5000 @ 1472 B: {'PASS' if pass_win else 'FAIL'} ({mbps_win:.2f} Mbit/s)")
            if not pass_win:
                overall_ok = False
        finally:
            server.stop()
        return overall_ok

    # On-board mode for auditor
    logger.log("[auditor] Compiling prj_02 debug in control worktree and HEAD...")
    pdir_ctrl = control_dir / "02_firmware_projects" / "prj_02_eth_lwip_dualcore"
    pdir_head = REPO_ROOT / "02_firmware_projects" / "prj_02_eth_lwip_dualcore"

    c_bld_ok, _, _, _ = compile_single_config(tools, pdir_ctrl, "debug", logger)
    h_bld_ok, _, _, _ = compile_single_config(tools, pdir_head, "debug", logger)
    if not (c_bld_ok and h_bld_ok):
        logger.log("[auditor] FAIL: Compilation of control or HEAD prj_02 failed")
        return False

    ctrl_elf = pdir_ctrl / "build" / "debug" / "CM7" / "cm7_firmware.elf"
    head_elf = pdir_head / "build" / "debug" / "CM7" / "cm7_firmware.elf"
    ctrl_sha = sha256_file(ctrl_elf)
    head_sha = sha256_file(head_elf)
    logger.log(f"[auditor] Control ELF SHA-256: {ctrl_sha} ({ctrl_elf})")
    logger.log(f"[auditor] HEAD ELF SHA-256:    {head_sha} ({head_elf})")

    # 1. Test Control Image on Board
    with tempfile.TemporaryDirectory(prefix="eth_t5_ctrl_") as tmp_ctrl_dir:
        tmp_ctrl_path = Path(tmp_ctrl_dir)
        ctrl_log = tmp_ctrl_path / "monitor_ctrl.log"

        logger.log(f"[auditor] Flashing control image to board {sn} using control's flash.py...")
        ctrl_flash_py = pdir_ctrl / "tools" / "flash.py"
        code_fc, out_fc, err_fc = run_cmd(["py", str(ctrl_flash_py), "--preset", "debug", "--sn", sn], cwd=pdir_ctrl)
        if code_fc != 0:
            logger.log(f"[auditor] FAIL: Flashing control image failed ({code_fc}): {err_fc or out_fc}")
            return False

        # Start background monitor.py
        monitor_py = REPO_ROOT / "tools" / "monitor.py"
        mon_proc_c = subprocess.Popen(["py", str(monitor_py), "--sn", sn, "--log", str(ctrl_log)])
        time.sleep(5.0)

        # Run net_check.py against control
        net_check_py = REPO_ROOT / "tools" / "net_check.py"
        code_nc1, out_nc1, err_nc1 = run_cmd(["py", str(net_check_py), "--host", target_ip], timeout=60.0)
        nc1_ok = (code_nc1 == 0) and ("FAIL" not in (out_nc1 + err_nc1))
        logger.log(f"[auditor] Control net_check: {'PASS' if nc1_ok else 'FAIL'} (exit code {code_nc1})")
        if not nc1_ok:
            overall_ok = False

        # Run 3 repetitions of throughput load test on control
        ctrl_tp_runs = []
        for rep in range(1, 4):
            logger.log(f"[auditor] Running control sequential load test rep {rep}/3...")
            ok_r, _, _, mbps_r, _ = run_udp_sequential_load(target_ip, 7, count=10000, payload_size=1472, logger=logger)
            if not ok_r:
                overall_ok = False
            ctrl_tp_runs.append(mbps_r)
        ctrl_median_tp = sorted(ctrl_tp_runs)[1]
        logger.log(f"[auditor] Control throughput median: {ctrl_median_tp:.2f} Mbit/s (runs: {ctrl_tp_runs})")

        # Run windowed load test on control (32 in flight)
        logger.log("[auditor] Running control windowed load test (32 in flight, 5000 datagrams)...")
        ok_win_c, _, _, _, _ = run_udp_windowed_load(target_ip, 7, count=5000, window_size=32, payload_size=1472, logger=logger)
        if not ok_win_c:
            overall_ok = False

        # Terminate monitor and evaluate log
        mon_proc_c.terminate()
        try:
            mon_proc_c.wait(timeout=5.0)
        except Exception:
            mon_proc_c.kill()

        ctrl_mon_text = ctrl_log.read_text(encoding="utf-8", errors="replace") if ctrl_log.is_file() else ""
        c_mon_ok, c_mon_samples, c_mon_msg = evaluate_monitor_net_lines(ctrl_mon_text)
        logger.log(f"[auditor] Control monitor check: {'PASS' if c_mon_ok else 'FAIL'} ({c_mon_msg})")
        if not c_mon_ok:
            overall_ok = False

    # 2. Test HEAD Image on Board
    with tempfile.TemporaryDirectory(prefix="eth_t5_head_") as tmp_head_dir:
        tmp_head_path = Path(tmp_head_dir)
        head_log = tmp_head_path / "monitor_head.log"

        logger.log(f"[auditor] Flashing HEAD image to board {sn} using HEAD's flash.py...")
        head_flash_py = pdir_head / "tools" / "flash.py"
        code_fh, out_fh, err_fh = run_cmd(["py", str(head_flash_py), "--preset", "debug", "--sn", sn], cwd=pdir_head)
        if code_fh != 0:
            logger.log(f"[auditor] FAIL: Flashing HEAD image failed ({code_fh}): {err_fh or out_fh}")
            return False

        # Start background monitor.py
        mon_proc_h = subprocess.Popen(["py", str(monitor_py), "--sn", sn, "--log", str(head_log)])
        time.sleep(5.0)

        # Run net_check.py against HEAD
        code_nc2, out_nc2, err_nc2 = run_cmd(["py", str(net_check_py), "--host", target_ip], timeout=60.0)
        nc2_ok = (code_nc2 == 0) and ("FAIL" not in (out_nc2 + err_nc2))
        logger.log(f"[auditor] HEAD net_check: {'PASS' if nc2_ok else 'FAIL'} (exit code {code_nc2})")
        if not nc2_ok:
            overall_ok = False

        # Run 3 repetitions of throughput load test on HEAD
        head_tp_runs = []
        for rep in range(1, 4):
            logger.log(f"[auditor] Running HEAD sequential load test rep {rep}/3...")
            ok_hr, _, _, mbps_hr, _ = run_udp_sequential_load(target_ip, 7, count=10000, payload_size=1472, logger=logger)
            if not ok_hr:
                overall_ok = False
            head_tp_runs.append(mbps_hr)
        head_median_tp = sorted(head_tp_runs)[1]
        logger.log(f"[auditor] HEAD throughput median: {head_median_tp:.2f} Mbit/s (runs: {head_tp_runs})")

        # Run windowed load test on HEAD (32 in flight)
        logger.log("[auditor] Running HEAD windowed load test (32 in flight, 5000 datagrams)...")
        ok_win_h, _, _, _, _ = run_udp_windowed_load(target_ip, 7, count=5000, window_size=32, payload_size=1472, logger=logger)
        if not ok_win_h:
            overall_ok = False

        # Terminate monitor and evaluate log
        mon_proc_h.terminate()
        try:
            mon_proc_h.wait(timeout=5.0)
        except Exception:
            mon_proc_h.kill()

        head_mon_text = head_log.read_text(encoding="utf-8", errors="replace") if head_log.is_file() else ""
        h_mon_ok, h_mon_samples, h_mon_msg = evaluate_monitor_net_lines(head_mon_text)
        logger.log(f"[auditor] HEAD monitor check: {'PASS' if h_mon_ok else 'FAIL'} ({h_mon_msg})")
        if not h_mon_ok:
            overall_ok = False

        # Revision 2 throughput criterion: median_head >= 0.98 * median_control
        tp_ratio = head_median_tp / ctrl_median_tp if ctrl_median_tp > 0 else 0.0
        tp_ok = (head_median_tp >= 0.98 * ctrl_median_tp)
        logger.log(f"[auditor] CHECK T5 throughput comparison (HEAD median {head_median_tp:.2f} >= 0.98 * control {ctrl_median_tp:.2f}, ratio={tp_ratio:.4f}): {'PASS' if tp_ok else 'FAIL'}")
        if not tp_ok:
            overall_ok = False

    return overall_ok


# ==============================================================================
# T6: Hardware Reset Stress Test (Auditor Only)
# ==============================================================================

def run_t6(
    tools: dict[str, str],
    sn: str,
    target_ip: str,
    reset_count: int,
    logger: Logger
) -> bool:
    logger.log(f"=== Running T6 Checks ({reset_count} SWD Resets on Board {sn}) ===")
    programmer = tools["programmer"]
    net_check_py = REPO_ROOT / "tools" / "net_check.py"

    resets_passed = 0
    for r in range(1, reset_count + 1):
        logger.log(f"[auditor] Reset {r}/{reset_count}: triggering SWD reset via STM32_Programmer_CLI...")
        cmd_rst = [
            programmer, "-c", "port=SWD", f"sn={sn}", "mode=HOTPLUG", "-rst"
        ]
        c_code, c_out, c_err = run_cmd(cmd_rst, timeout=15.0)
        if c_code != 0:
            logger.log(f"[auditor] FAIL: SWD reset {r} failed (code={c_code}): {c_err or c_out}")
            break

        time.sleep(5.0)  # Settling time for PHY auto-negotiation and link up

        cmd_nc = ["py", str(net_check_py), "--host", target_ip, "--ping", "4", "--burst", "100"]
        nc_code, nc_out, nc_err = run_cmd(cmd_nc, timeout=30.0)
        nc_ok = (nc_code == 0) and ("FAIL" not in (nc_out + nc_err))
        logger.log(f"[auditor] Reset {r}/{reset_count} verification: {'PASS' if nc_ok else 'FAIL'}")
        if nc_ok:
            resets_passed += 1

    t6_ok = (resets_passed == reset_count)
    logger.log(f"[auditor] CHECK T6 {reset_count} resets: {'PASS' if t6_ok else 'FAIL'} ({resets_passed}/{reset_count} passed)")
    return t6_ok


# ==============================================================================
# Main Entry Point & CLI
# ==============================================================================

def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--test", default="all",
                        help="Which test to run: t1, t2, t3, t4, t5, t6, all (or comma-separated like t1,t2,t3,t4)")
    parser.add_argument("--output", "-o", type=Path, default=None, help="Capture file destination")
    parser.add_argument("--control-dir", type=Path, default=REPO_ROOT.parent / f"stm32h755_control_{CONTROL_COMMIT_EXPECTED}",
                        help="Path to control worktree")
    parser.add_argument("--allow-dirty", action="store_true", help="Accept dirty working tree (development only)")
    parser.add_argument("--sn", default=DEFAULT_SN, help=f"ST-LINK serial number for T5/T6 (default: {DEFAULT_SN})")
    parser.add_argument("--target-ip", default=DEFAULT_TARGET_IP, help=f"Target IP address for T5/T6 (default: {DEFAULT_TARGET_IP})")
    parser.add_argument("--local-echo", action="store_true", help="Run T5 against a local 127.0.0.1 UDP echo server without hardware")
    parser.add_argument("--reset-count", type=int, default=10, help="Number of resets for T6 (default: 10)")
    args = parser.parse_args()

    valid_tests = {"t1", "t2", "t3", "t4", "t5", "t6", "all"}
    raw_tests = [t.strip().lower() for t in args.test.split(",") if t.strip()]
    for t in raw_tests:
        if t not in valid_tests:
            parser.error(f"Invalid test: '{t}'. Choices: {sorted(valid_tests)}")

    if "all" in raw_tests:
        if args.local_echo:
            tests_to_run = ["t1", "t2", "t3", "t4", "t5"]
        else:
            tests_to_run = ["t1", "t2", "t3", "t4"]
    else:
        tests_to_run = raw_tests

    logger = Logger(args.output)
    logger.log(f"[host] argv: {' '.join(sys.argv)}")
    logger.log(f"[host] options: test={args.test} output={args.output} control_dir={args.control_dir} allow_dirty={args.allow_dirty} sn={args.sn} target_ip={args.target_ip} local_echo={args.local_echo}")

    head_rev = get_head_full_rev()
    logger.log(f"[host] HEAD: {head_rev}")
    logger.log(f"[host] Working tree: {REPO_ROOT}")
    logger.log(f"[host] Control worktree: {args.control_dir}")

    # Snapshot working tree and check clean at start
    initial_snapshot = snapshot_working_tree(REPO_ROOT)
    clean_ok, clean_msg = check_clean_working_tree(REPO_ROOT, args.output, initial_snapshot=None)
    logger.log(f"[host] working tree clean (start): {'PASS' if clean_ok else 'FAIL'} ({clean_msg})")
    if not clean_ok and not args.allow_dirty:
        logger.log("[host] ERROR: Working tree is dirty. Stash or commit before running.")
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
        ok2 = run_t2(tools, args.control_dir, logger)
        if not ok2:
            overall_all_ok = False

    if "t3" in tests_to_run:
        ok3 = run_t3(tools, logger)
        if not ok3:
            overall_all_ok = False

    if "t4" in tests_to_run:
        ok4 = run_t4(tools, logger)
        if not ok4:
            overall_all_ok = False

    if "t5" in tests_to_run:
        ok5 = run_t5(tools, args.control_dir, args.sn, args.target_ip, args.local_echo, logger)
        if not ok5:
            overall_all_ok = False

    if "t6" in tests_to_run:
        ok6 = run_t6(tools, args.sn, args.target_ip, args.reset_count, logger)
        if not ok6:
            overall_all_ok = False

    # Check clean working tree at end against initial snapshot
    clean_end_ok, clean_end_msg = check_clean_working_tree(REPO_ROOT, args.output, initial_snapshot=initial_snapshot)
    logger.log(f"[host] working tree clean (end): {'PASS' if clean_end_ok else 'FAIL'} ({clean_end_msg})")
    if not clean_end_ok and not args.allow_dirty:
        overall_all_ok = False

    verdict_str = "PASS" if overall_all_ok else "FAIL"
    logger.log(f"OVERALL VERDICT: {verdict_str}")
    return 0 if overall_all_ok else 3


if __name__ == "__main__":
    sys.exit(main())
