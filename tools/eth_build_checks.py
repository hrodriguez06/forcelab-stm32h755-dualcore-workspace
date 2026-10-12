#!/usr/bin/env python3
"""Build-time checks for the STM32H7 Ethernet driver (08_eth).

Covers tests T2, T3, and T4 from IMPLEMENTATION_PLAN.md:
  - T2 (E1): eth_hal.c compiled against 19 variants of board_def.h:
             missing file, and missing each of the 18 BOARD_ETH_* macros,
             plus positive controls (full board_def.h, and prj_01 without module).
  - T3 (E2): eth_hal.c compiled with USE_HAL_ETH_REGISTER_CALLBACKS != 0 (#error),
             plus controls (0U and undefined).
  - T4 (E5, E8): Object disassembly and symbol inspection of eth_hal.c.obj:
             5 atomic increments (ldrex/strex loops), GPIO_HAL_Init imported,
             HAL_GPIO_Init absent; contrasted against the 7154af6 baseline object.

All test variants are built in a dedicated scratch directory and never touch
project source directories.
"""

from __future__ import annotations

import argparse
import json
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent

RMII_SIGNALS = [
    "REF_CLK",
    "MDIO",
    "CRS_DV",
    "MDC",
    "RXD0",
    "RXD1",
    "TX_EN",
    "TXD0",
    "TXD1",
]

MACROS_18 = []
for sig in RMII_SIGNALS:
    MACROS_18.append(f"BOARD_ETH_{sig}_PORT")
    MACROS_18.append(f"BOARD_ETH_{sig}_PIN")


def find_toolchain(compiler_from_json: str | None = None) -> dict[str, str]:
    """Resolves arm-none-eabi-gcc, arm-none-eabi-objdump, and arm-none-eabi-nm."""
    tools = {}
    gcc_path = None

    if compiler_from_json and Path(compiler_from_json).is_file():
        gcc_path = Path(compiler_from_json)
    else:
        which_gcc = shutil.which("arm-none-eabi-gcc")
        if which_gcc:
            gcc_path = Path(which_gcc)

    if not gcc_path:
        # Fallback to searching C:\ST
        for p in Path("C:/ST").glob("**/arm-none-eabi-gcc.exe"):
            gcc_path = p
            break

    if not gcc_path or not gcc_path.is_file():
        raise RuntimeError("Could not find arm-none-eabi-gcc")

    bin_dir = gcc_path.parent
    tools["gcc"] = str(gcc_path)
    tools["objdump"] = str(bin_dir / "arm-none-eabi-objdump.exe")
    tools["nm"] = str(bin_dir / "arm-none-eabi-nm.exe")
    tools["size"] = str(bin_dir / "arm-none-eabi-size.exe")

    for name, path in tools.items():
        if not Path(path).is_file():
            which_t = shutil.which(f"arm-none-eabi-{name}")
            if which_t:
                tools[name] = which_t
            else:
                raise RuntimeError(f"Could not find tool {name} at {path}")

    return tools


def get_compile_entry(project_dir: Path, source_name: str = "eth_hal.c") -> tuple[str, list[str]]:
    """Loads compile_commands.json and returns (directory, command_tokens)."""
    comp_json = project_dir / "build" / "debug" / "compile_commands.json"
    if not comp_json.is_file():
        comp_json = project_dir / "build" / "release" / "compile_commands.json"
    if not comp_json.is_file():
        raise FileNotFoundError(f"compile_commands.json not found in {project_dir}/build/(debug|release)")

    with open(comp_json, "r", encoding="utf-8") as f:
        data = json.load(f)

    for entry in data:
        if source_name in entry.get("file", ""):
            cmd = entry.get("command") or entry.get("arguments")
            if isinstance(cmd, list):
                tokens = cmd
            else:
                import shlex
                tokens = shlex.split(cmd, posix=False)
            return entry.get("directory", str(project_dir)), tokens

    raise ValueError(f"No entry for {source_name} in {comp_json}")


def clean_tokens(tokens: list[str]) -> list[str]:
    """Removes output and input arguments (-o ..., -c ...) from gcc command."""
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
        cleaned.append(tok)
    return cleaned


def run_t2(tools: dict[str, str], base_dir: str, base_tokens: list[str], scratch_dir: Path) -> dict:
    """T2 (E1): 19 failing variants + controls."""
    results = {"pass": True, "cases": []}
    source_file = str(REPO_ROOT / "01_shared_drivers" / "08_eth" / "eth_hal.c")
    t2_scratch = scratch_dir / "t2"
    t2_scratch.mkdir(parents=True, exist_ok=True)

    # Variant 0: missing board_def.h completely
    # We remove any -I pointing to Common/Inc
    v0_tokens = [tok for tok in base_tokens if not tok.replace("\\", "/").endswith("/Common/Inc")]
    out_obj = str(t2_scratch / "v0_no_header.o")
    cmd = [tools["gcc"]] + v0_tokens + ["-c", source_file, "-o", out_obj]
    proc = subprocess.run(cmd, cwd=base_dir, capture_output=True, text=True)
    v0_ok = (proc.returncode != 0) and ("board_def.h" in proc.stderr)
    results["cases"].append({
        "id": "T2_var_missing_board_def",
        "description": "Variant 0: without board_def.h",
        "expected_fail": True,
        "passed": v0_ok,
        "detail": proc.stderr.strip().splitlines()[-1] if proc.stderr else ""
    })
    if not v0_ok:
        results["pass"] = False

    # Variants 1..18: missing each BOARD_ETH_* macro
    real_board_def = REPO_ROOT / "02_firmware_projects" / "prj_02_eth_lwip_dualcore" / "Common" / "Inc" / "board_def.h"
    real_content = real_board_def.read_text(encoding="utf-8")

    for macro in MACROS_18:
        var_dir = t2_scratch / f"missing_{macro}"
        var_dir.mkdir(parents=True, exist_ok=True)
        # Create a modified board_def.h that undefines this macro at the end
        mod_content = real_content + f"\n#undef {macro}\n"
        (var_dir / "board_def.h").write_text(mod_content, encoding="utf-8")

        out_obj = str(var_dir / "out.o")
        # Prepend -I<var_dir> so gcc finds this board_def.h first
        cmd = [tools["gcc"], f"-I{var_dir}"] + base_tokens + ["-c", source_file, "-o", out_obj]
        proc = subprocess.run(cmd, cwd=base_dir, capture_output=True, text=True)
        var_ok = (proc.returncode != 0) and ("#error" in proc.stderr) and (macro in proc.stderr)
        results["cases"].append({
            "id": f"T2_var_{macro}",
            "description": f"Missing {macro}",
            "expected_fail": True,
            "passed": var_ok,
            "detail": proc.stderr.strip().splitlines()[-1] if proc.stderr else ""
        })
        if not var_ok:
            results["pass"] = False

    # Negative Control 1: complete board_def.h compiles with 0 errors
    ctrl1_obj = str(t2_scratch / "ctrl1_full.o")
    cmd = [tools["gcc"]] + base_tokens + ["-c", source_file, "-o", ctrl1_obj]
    proc = subprocess.run(cmd, cwd=base_dir, capture_output=True, text=True)
    ctrl1_ok = (proc.returncode == 0)
    results["cases"].append({
        "id": "T2_ctrl_full_board_def",
        "description": "Control 1: full board_def.h compiles with 0 errors",
        "expected_fail": False,
        "passed": ctrl1_ok,
        "detail": f"exit={proc.returncode}"
    })
    if not ctrl1_ok:
        results["pass"] = False

    # Negative Control 2: prj_01 without Ethernet module compiles with 0 errors
    # In prj_01, HAL_ETH_MODULE_ENABLED is not defined. We undefine it.
    ctrl2_tokens = [tok for tok in base_tokens if not tok.startswith("-DHAL_ETH_MODULE_ENABLED")]
    ctrl2_tokens.append("-UHAL_ETH_MODULE_ENABLED")
    ctrl2_obj = str(t2_scratch / "ctrl2_prj01.o")
    cmd = [tools["gcc"]] + ctrl2_tokens + ["-c", source_file, "-o", ctrl2_obj]
    proc = subprocess.run(cmd, cwd=base_dir, capture_output=True, text=True)
    ctrl2_ok = (proc.returncode == 0)
    results["cases"].append({
        "id": "T2_ctrl_prj01_no_eth",
        "description": "Control 2: without HAL_ETH_MODULE_ENABLED compiles with 0 errors",
        "expected_fail": False,
        "passed": ctrl2_ok,
        "detail": f"exit={proc.returncode}"
    })
    if not ctrl2_ok:
        results["pass"] = False

    return results


def run_t3(tools: dict[str, str], base_dir: str, base_tokens: list[str], scratch_dir: Path) -> dict:
    """T3 (E2): USE_HAL_ETH_REGISTER_CALLBACKS != 0 check + controls."""
    results = {"pass": True, "cases": []}
    source_file = str(REPO_ROOT / "01_shared_drivers" / "08_eth" / "eth_hal.c")
    t3_scratch = scratch_dir / "t3"
    t3_scratch.mkdir(parents=True, exist_ok=True)

    real_hal_conf = REPO_ROOT / "02_firmware_projects" / "prj_02_eth_lwip_dualcore" / "CM7" / "Core" / "Inc" / "stm32h7xx_hal_conf.h"
    real_content = real_hal_conf.read_text(encoding="utf-8")

    # Case 1: USE_HAL_ETH_REGISTER_CALLBACKS == 1U -> must fail with #error
    dir_1 = t3_scratch / "callbacks_1"
    dir_1.mkdir(parents=True, exist_ok=True)
    content_1 = real_content + "\n#undef USE_HAL_ETH_REGISTER_CALLBACKS\n#define USE_HAL_ETH_REGISTER_CALLBACKS 1U\n"
    (dir_1 / "stm32h7xx_hal_conf.h").write_text(content_1, encoding="utf-8")

    out_1 = str(dir_1 / "out.o")
    cmd = [tools["gcc"], f"-I{dir_1}"] + base_tokens + ["-c", source_file, "-o", out_1]
    proc = subprocess.run(cmd, cwd=base_dir, capture_output=True, text=True)
    c1_ok = (proc.returncode != 0) and ("#error" in proc.stderr) and ("USE_HAL_ETH_REGISTER_CALLBACKS" in proc.stderr)
    results["cases"].append({
        "id": "T3_callbacks_1U",
        "description": "USE_HAL_ETH_REGISTER_CALLBACKS 1U fails with #error",
        "expected_fail": True,
        "passed": c1_ok,
        "detail": proc.stderr.strip().splitlines()[-1] if proc.stderr else ""
    })
    if not c1_ok:
        results["pass"] = False

    # Control 1: USE_HAL_ETH_REGISTER_CALLBACKS == 0U -> must compile cleanly
    dir_0 = t3_scratch / "callbacks_0"
    dir_0.mkdir(parents=True, exist_ok=True)
    content_0 = real_content + "\n#undef USE_HAL_ETH_REGISTER_CALLBACKS\n#define USE_HAL_ETH_REGISTER_CALLBACKS 0U\n"
    (dir_0 / "stm32h7xx_hal_conf.h").write_text(content_0, encoding="utf-8")

    out_0 = str(dir_0 / "out.o")
    cmd = [tools["gcc"], f"-I{dir_0}"] + base_tokens + ["-c", source_file, "-o", out_0]
    proc = subprocess.run(cmd, cwd=base_dir, capture_output=True, text=True)
    c0_ok = (proc.returncode == 0)
    results["cases"].append({
        "id": "T3_ctrl_callbacks_0U",
        "description": "Control 1: USE_HAL_ETH_REGISTER_CALLBACKS 0U compiles with 0 errors",
        "expected_fail": False,
        "passed": c0_ok,
        "detail": f"exit={proc.returncode}"
    })
    if not c0_ok:
        results["pass"] = False

    # Control 2: USE_HAL_ETH_REGISTER_CALLBACKS undefined -> must compile cleanly
    dir_undef = t3_scratch / "callbacks_undef"
    dir_undef.mkdir(parents=True, exist_ok=True)
    content_undef = real_content + "\n#undef USE_HAL_ETH_REGISTER_CALLBACKS\n"
    (dir_undef / "stm32h7xx_hal_conf.h").write_text(content_undef, encoding="utf-8")

    out_undef = str(dir_undef / "out.o")
    cmd = [tools["gcc"], f"-I{dir_undef}"] + base_tokens + ["-c", source_file, "-o", out_undef]
    proc = subprocess.run(cmd, cwd=base_dir, capture_output=True, text=True)
    cundef_ok = (proc.returncode == 0)
    results["cases"].append({
        "id": "T3_ctrl_callbacks_undef",
        "description": "Control 2: USE_HAL_ETH_REGISTER_CALLBACKS undefined compiles with 0 errors",
        "expected_fail": False,
        "passed": cundef_ok,
        "detail": f"exit={proc.returncode}"
    })
    if not cundef_ok:
        results["pass"] = False

    return results


def run_t4(tools: dict[str, str], base_dir: str, base_tokens: list[str], scratch_dir: Path) -> dict:
    """T4 (E5, E8): Object disassembly and symbol inspection against 7154af6."""
    results = {"pass": True, "cases": []}
    t4_scratch = scratch_dir / "t4"
    t4_scratch.mkdir(parents=True, exist_ok=True)

    # Compile current eth_hal.c into scratch
    curr_src = str(REPO_ROOT / "01_shared_drivers" / "08_eth" / "eth_hal.c")
    curr_obj = str(t4_scratch / "eth_hal_current.o")
    cmd = [tools["gcc"]] + base_tokens + ["-c", curr_src, "-o", curr_obj]
    proc = subprocess.run(cmd, cwd=base_dir, capture_output=True, text=True)
    if proc.returncode != 0:
        results["pass"] = False
        results["cases"].append({
            "id": "T4_compile_current",
            "description": "Compile current eth_hal.c",
            "passed": False,
            "detail": proc.stderr
        })
        return results

    # Obtain 7154af6 baseline eth_hal.c and compile into scratch
    baseline_src = t4_scratch / "eth_hal_7154af6.c"
    git_show = subprocess.run(["git", "show", "7154af6:01_shared_drivers/08_eth/eth_hal.c"],
                              cwd=str(REPO_ROOT), capture_output=True, text=True)
    if git_show.returncode != 0:
        raise RuntimeError("git show 7154af6:01_shared_drivers/08_eth/eth_hal.c failed")
    baseline_src.write_text(git_show.stdout, encoding="utf-8")

    baseline_obj = str(t4_scratch / "eth_hal_7154af6.o")
    cmd = [tools["gcc"]] + base_tokens + ["-c", str(baseline_src), "-o", baseline_obj]
    proc = subprocess.run(cmd, cwd=base_dir, capture_output=True, text=True)
    if proc.returncode != 0:
        results["pass"] = False
        results["cases"].append({
            "id": "T4_compile_baseline",
            "description": "Compile 7154af6 baseline eth_hal.c",
            "passed": False,
            "detail": proc.stderr
        })
        return results

    # 1. Undefined symbols check on current object:
    nm_curr = subprocess.run([tools["nm"], "-u", curr_obj], capture_output=True, text=True)
    curr_symbols = [line.strip().split()[-1] for line in nm_curr.stdout.strip().splitlines() if line.strip()]

    has_gpio_hal_init = "GPIO_HAL_Init" in curr_symbols
    has_hal_gpio_init = "HAL_GPIO_Init" in curr_symbols
    e8_curr_ok = has_gpio_hal_init and not has_hal_gpio_init

    results["cases"].append({
        "id": "T4_curr_symbols",
        "description": "Current object imports GPIO_HAL_Init and NOT HAL_GPIO_Init",
        "passed": e8_curr_ok,
        "detail": f"GPIO_HAL_Init={has_gpio_hal_init}, HAL_GPIO_Init={has_hal_gpio_init}"
    })
    if not e8_curr_ok:
        results["pass"] = False

    # 2. Undefined symbols check on baseline object:
    nm_base = subprocess.run([tools["nm"], "-u", baseline_obj], capture_output=True, text=True)
    base_symbols = [line.strip().split()[-1] for line in nm_base.stdout.strip().splitlines() if line.strip()]

    base_has_hal_gpio = "HAL_GPIO_Init" in base_symbols
    base_has_gpio_hal = "GPIO_HAL_Init" in base_symbols
    e8_base_ok = base_has_hal_gpio and not base_has_gpio_hal

    results["cases"].append({
        "id": "T4_control_baseline_symbols",
        "description": "Control: 7154af6 object imports HAL_GPIO_Init and NOT GPIO_HAL_Init",
        "passed": e8_base_ok,
        "detail": f"HAL_GPIO_Init={base_has_hal_gpio}, GPIO_HAL_Init={base_has_gpio_hal}"
    })
    if not e8_base_ok:
        results["pass"] = False

    # 3. Disassembly: atomic ldrex/strex in current object
    objdump_curr = subprocess.run([tools["objdump"], "-d", curr_obj], capture_output=True, text=True)
    curr_strex_count = len(re.findall(r"\bstrex\b", objdump_curr.stdout))
    curr_ldrex_count = len(re.findall(r"\bldrex\b", objdump_curr.stdout))
    e5_curr_ok = (curr_strex_count == 5) and (curr_ldrex_count == 5)

    results["cases"].append({
        "id": "T4_curr_atomic_stats",
        "description": "Current object has exactly 5 atomic ldrex/strex loops",
        "passed": e5_curr_ok,
        "detail": f"ldrex={curr_ldrex_count}, strex={curr_strex_count}"
    })
    if not e5_curr_ok:
        results["pass"] = False

    # 4. Disassembly: baseline object has 0 strex instructions
    objdump_base = subprocess.run([tools["objdump"], "-d", baseline_obj], capture_output=True, text=True)
    base_strex_count = len(re.findall(r"\bstrex\b", objdump_base.stdout))
    base_ldrex_count = len(re.findall(r"\bldrex\b", objdump_base.stdout))
    e5_base_ok = (base_strex_count == 0) and (base_ldrex_count == 0)

    results["cases"].append({
        "id": "T4_control_baseline_atomic",
        "description": "Control: 7154af6 object has 0 ldrex/strex (non-atomic ldr/add/str)",
        "passed": e5_base_ok,
        "detail": f"ldrex={base_ldrex_count}, strex={base_strex_count}"
    })
    if not e5_base_ok:
        results["pass"] = False

    return results


def main() -> int:
    parser = argparse.ArgumentParser(description="ETH driver build-time checks (T2, T3, T4)")
    parser.add_argument("--scratch-dir", type=Path, default=REPO_ROOT / "build" / "scratch_eth_checks",
                        help="Scratch directory for test variants")
    parser.add_argument("--check", choices=["T2", "T3", "T4", "all"], default="all",
                        help="Check to run (default: all)")
    args = parser.parse_args()

    scratch_dir: Path = args.scratch_dir
    scratch_dir.mkdir(parents=True, exist_ok=True)

    prj02_dir = REPO_ROOT / "02_firmware_projects" / "prj_02_eth_lwip_dualcore"
    try:
        base_dir, raw_tokens = get_compile_entry(prj02_dir, "eth_hal.c")
    except Exception as e:
        sys.exit(f"Error loading compile commands: {e}")

    tools = find_toolchain(raw_tokens[0])
    base_tokens = clean_tokens(raw_tokens)

    all_passed = True
    print(f"=== ETH Build Checks (T2, T3, T4) ===")
    print(f"Compiler: {tools['gcc']}")
    print(f"Scratch dir: {scratch_dir}")
    print()

    checks_to_run = ["T2", "T3", "T4"] if args.check == "all" else [args.check]

    if "T2" in checks_to_run:
        print("--- Running T2 (E1: 19 board_def.h variants + 2 controls) ---")
        res_t2 = run_t2(tools, base_dir, base_tokens, scratch_dir)
        for c in res_t2["cases"]:
            status = "PASS" if c["passed"] else "FAIL"
            print(f"  [{status}] {c['id']}: {c['description']} ({c['detail']})")
        if not res_t2["pass"]:
            all_passed = False
        print(f"T2 Verdict: {'PASS' if res_t2['pass'] else 'FAIL'}\n")

    if "T3" in checks_to_run:
        print("--- Running T3 (E2: USE_HAL_ETH_REGISTER_CALLBACKS != 0 + 2 controls) ---")
        res_t3 = run_t3(tools, base_dir, base_tokens, scratch_dir)
        for c in res_t3["cases"]:
            status = "PASS" if c["passed"] else "FAIL"
            print(f"  [{status}] {c['id']}: {c['description']} ({c['detail']})")
        if not res_t3["pass"]:
            all_passed = False
        print(f"T3 Verdict: {'PASS' if res_t3['pass'] else 'FAIL'}\n")

    if "T4" in checks_to_run:
        print("--- Running T4 (E5, E8: object disassembly and symbols vs 7154af6) ---")
        res_t4 = run_t4(tools, base_dir, base_tokens, scratch_dir)
        for c in res_t4["cases"]:
            status = "PASS" if c["passed"] else "FAIL"
            print(f"  [{status}] {c['id']}: {c['description']} ({c['detail']})")
        if not res_t4["pass"]:
            all_passed = False
        print(f"T4 Verdict: {'PASS' if res_t4['pass'] else 'FAIL'}\n")

    overall = "PASS" if all_passed else "FAIL"
    print(f"=== OVERALL BUILD CHECKS VERDICT: {overall} ===")
    return 0 if all_passed else 1


if __name__ == "__main__":
    sys.exit(main())
