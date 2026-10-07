#!/usr/bin/env python3
"""Flash the STM32H755ZI on the Nucleo-H755ZI-Q, one core or both.

Why this exists
---------------
The raw command is short enough to type:

    STM32_Programmer_CLI -c port=SWD mode=UR -w build/debug/CM7/cm7_firmware.elf -rst

What it does not do is find the tool (it lives inside an STM32CubeIDE plugin
directory whose name carries a build stamp that changes on every update),
pick up the CPU2 image when there is one, or notice that the ELF you are about
to program is older than the sources it was built from. That is the whole job
here.

Usage
-----
On Windows use the `py` launcher: the bare `python` on this machine is the
Microsoft Store alias stub and does not run.

    py tools/flash.py                    # CPU1 (+ CPU2 if built), debug preset
    py tools/flash.py --preset release
    py tools/flash.py --preset selftest  # any preset of the project's CMakePresets.json
    py tools/flash.py --core cm7         # only CPU1, even if CPU2 is built
    py tools/flash.py --tool openocd
    py tools/flash.py --dry-run          # print the commands, run nothing

    py tools/flash.py --sn 0016000C4142501220353451   # this ST-LINK only (sn=)

    py tools/flash.py --list             # probes the ST-LINK can see
    py tools/flash.py --read-ob          # dump option bytes
    py tools/flash.py --erase            # full chip erase, then flash

Option bytes
------------
`--read-ob` is safe and needs no confirmation. Writing them is different: a
wrong boot address leaves a board that no longer starts, so there is no
"configure dual-core boot for me" shortcut. Pass the fields explicitly and
confirm:

    py tools/flash.py --ob BCM7=1 --ob BOOT_CM7_ADD0=0x0800

Read `--read-ob` output first and use the names it prints. On the board tested
here (device 0x450 Rev V, CubeProgrammer 2.23) the CLI reports the single-core
H7 option-byte map - `BOOT_ADD0` / `BOOT_ADD1`, with no `BCM7` / `BCM4` - even
though it identifies the CPU as Cortex-M7/M4. Names that the tool does not
expose for the connected device are rejected by CubeProgrammer, not silently
applied, but check before assuming a name from a manual is the right one.

See also
--------
docs/ETHERNET_BRINGUP.md              bring-up and verification order
00_docs_stm32h7/docs/00_core_and_system/dual_core_boot_ipc.md
                                      what BCM7/BCM4 actually do at reset
"""

from __future__ import annotations

import argparse
import json
import os
import subprocess
import sys
from pathlib import Path

TOOLS_ROOT = Path(__file__).resolve().parent


def _project_root() -> Path:
    """The firmware tree this run operates on.

    One copy of this script serves every project, so the project cannot come
    from __file__ any more. A project's tools/<name>.py shim exports
    STM32_PROJECT_ROOT before handing over; running this file directly instead
    falls back to whichever project the current directory sits in.
    """
    injected = os.environ.get("STM32_PROJECT_ROOT")
    if injected:
        return Path(injected).resolve()

    cwd = Path.cwd().resolve()
    for base in (cwd, *cwd.parents):
        if (base / "CMakePresets.json").is_file():
            return base

    # Not die(): that helper is defined further down, and this runs at import.
    sys.exit(
        "ERROR: no firmware project here. Run this from inside one, or through"
        " its tools/flash.py shim, or set STM32_PROJECT_ROOT."
    )


PROJECT_ROOT = _project_root()
SCRIPT_NAME = f"{Path(__file__).parent.name}/{Path(__file__).name}"

# Used when the project's CMakePresets.json cannot be read.
DEFAULT_PRESETS = ("debug", "release", "selftest")


def _project_presets() -> tuple[str, ...]:
    """The configure presets of this project that --preset may name.

    Read from the project's own CMakePresets.json, so a project can add a preset
    without this shared script learning its name. Only presets whose binaryDir
    is ${sourceDir}/build/<name> qualify: artifact() looks for the images there,
    and a preset built anywhere else would be offered and then not found.
    """
    try:
        data = json.loads((PROJECT_ROOT / "CMakePresets.json").read_text(encoding="utf-8"))
        names = tuple(
            p["name"] for p in data.get("configurePresets", [])
            if not p.get("hidden") and p.get("binaryDir") == f"${{sourceDir}}/build/{p['name']}"
        )
    except (OSError, ValueError, KeyError, TypeError, AttributeError):
        names = ()
    return names or DEFAULT_PRESETS

# Flash bank layout, mirroring CM7/linker/stm32h755zi_cm7.ld and the CM4 script
# in 00_docs_stm32h7/docs/00_core_and_system/linker_templates/.
CORES = {
    "cm7": {"image": "cm7_firmware", "base": 0x08000000, "bank": 1, "cpu": "CPU1 (Cortex-M7)"},
    "cm4": {"image": "cm4_firmware", "base": 0x08100000, "bank": 2, "cpu": "CPU2 (Cortex-M4)"},
}

# Must match `set CHIPNAME` in tools/openocd/stm32h755_dualcore.cfg: it prefixes
# every target name OpenOCD creates (stm32h755.cm7, stm32h755.cm4, ...).
OPENOCD_CHIPNAME = "stm32h755"

# Directories whose sources feed the firmware, for the staleness check.
# Directories are skipped when a project does not have them, so one list
# serves a CM7-only product and a project-local Custom_Drivers/ alike.
SOURCE_DIRS = ("CM7", "CM4", "Common", "Custom_Drivers")
SOURCE_SUFFIXES = (".c", ".h", ".s", ".ld")

SHARED_DRIVERS_DIR = Path(
    os.environ.get("STM32_SHARED_DRIVERS", TOOLS_ROOT.parent / "01_shared_drivers")
).resolve()


# --------------------------------------------------------------------------
# Locating the tools
# --------------------------------------------------------------------------
# Same problem tools/env.ps1 solves, solved again here so flash.py works in a
# shell where env.ps1 was never run.

def _install_roots() -> list[Path]:
    roots = []
    for base in (
        Path(r"C:\ST"),
        Path(os.environ.get("ProgramFiles", r"C:\Program Files")) / "STMicroelectronics",
        Path(os.environ.get("LOCALAPPDATA", "")) / "Programs",
    ):
        if base.is_dir():
            roots.append(base)
    return roots


def _from_path(exe: str) -> Path | None:
    from shutil import which

    hit = which(exe)
    return Path(hit) if hit else None


def find_tool(exe: str, plugin: str) -> Path | None:
    """Return the path to `exe`: PATH first, then CubeCLT, then CubeIDE plugins."""
    found = _from_path(exe)
    if found:
        return found

    for base in _install_roots():
        # STM32CubeCLT: stable layout, but do not assume it - just look.
        for clt in sorted(base.glob("STM32CubeCLT*"), reverse=True):
            for hit in clt.rglob(exe):
                return hit

        # STM32CubeIDE: plugins/com.st...externaltools.<plugin>.win32_<stamp>/tools/bin/
        for ide in sorted(base.glob("STM32CubeIDE*"), reverse=True):
            plugins = ide / "STM32CubeIDE" / "plugins"
            if not plugins.is_dir():
                plugins = ide / "plugins"
            pattern = f"com.st.stm32cube.ide.mcu.externaltools.{plugin}*"
            for pdir in sorted(plugins.glob(pattern), reverse=True):
                candidate = pdir / "tools" / "bin" / exe
                if candidate.is_file():
                    return candidate
    return None


def find_openocd_scripts() -> Path | None:
    """ST's OpenOCD script tree, which lives in a *different* plugin than the binary."""
    for base in _install_roots():
        for clt in sorted(base.glob("STM32CubeCLT*"), reverse=True):
            for hit in clt.rglob("st_scripts"):
                if (hit / "target").is_dir():
                    return hit
        for ide in sorted(base.glob("STM32CubeIDE*"), reverse=True):
            plugins = ide / "STM32CubeIDE" / "plugins"
            if not plugins.is_dir():
                plugins = ide / "plugins"
            for pdir in sorted(plugins.glob("com.st.stm32cube.ide.mcu.debug.openocd*"), reverse=True):
                scripts = pdir / "resources" / "openocd" / "st_scripts"
                if (scripts / "target").is_dir():
                    return scripts
    return None


def require(tool: Path | None, exe: str) -> Path:
    if tool is None:
        die(
            f"{exe} not found.\n"
            f"  Looked on PATH and under: {', '.join(str(r) for r in _install_roots()) or '(nothing)'}\n"
            f"  Run tools\\env.ps1 first, or install STM32CubeCLT."
        )
    return tool  # type: ignore[return-value]


# --------------------------------------------------------------------------
# Artifacts
# --------------------------------------------------------------------------

def artifact(preset: str, core: str, explicit: Path | None = None) -> Path | None:
    if explicit:
        return explicit if explicit.is_file() else None
    core_dir = core.upper()
    elf = PROJECT_ROOT / "build" / preset / core_dir / f"{CORES[core]['image']}.elf"
    return elf if elf.is_file() else None


def newest_source_mtime() -> float:
    newest = 0.0
    dirs = [PROJECT_ROOT / name for name in SOURCE_DIRS]
    if SHARED_DRIVERS_DIR.is_dir():
        dirs.append(SHARED_DRIVERS_DIR)
    for root in dirs:
        if not root.is_dir():
            continue
        for path in root.rglob("*"):
            if path.suffix in SOURCE_SUFFIXES and path.is_file():
                newest = max(newest, path.stat().st_mtime)
    return newest


def warn_if_stale(elf: Path) -> None:
    """A stale ELF is the classic 'my fix did nothing' afternoon."""
    newest = newest_source_mtime()
    if newest and elf.stat().st_mtime < newest:
        warn(
            f"{elf.relative_to(PROJECT_ROOT)} is older than the newest source file.\n"
            f"  Build first:  cmake --build --preset {elf.parent.parent.name}"
        )


# --------------------------------------------------------------------------
# Output helpers
# --------------------------------------------------------------------------

def info(msg: str) -> None:
    print(f"  {msg}")


def warn(msg: str) -> None:
    print(f"WARNING: {msg}", file=sys.stderr)


def die(msg: str, code: int = 1) -> "typing.NoReturn":  # noqa: F821
    print(f"ERROR: {msg}", file=sys.stderr)
    sys.exit(code)


def run(cmd: list[str], dry_run: bool) -> int:
    printable = " ".join(f'"{c}"' if " " in c else c for c in cmd)
    print(f"\n$ {printable}\n")
    # The child writes straight to the console. Without this flush our buffered
    # output lands *after* it and the log reads back to front.
    sys.stdout.flush()
    if dry_run:
        return 0
    try:
        return subprocess.call(cmd)
    except FileNotFoundError:
        die(f"could not execute {cmd[0]}")


def confirm(question: str, assume_yes: bool) -> bool:
    """Ask, and treat 'cannot ask' as 'no'.

    isatty() is not enough: a PowerShell task runner or a CI step can report a
    terminal and still hand input() an immediate EOF. Both cases mean nobody is
    there to answer, and the safe reading of silence is no.
    """
    if assume_yes:
        return True
    try:
        answer = input(f"{question} [y/N] ")
    except (EOFError, KeyboardInterrupt):
        print()
        die("needs confirmation but no input is available; pass --yes to proceed")
    return answer.strip().lower() in ("y", "yes")


# --------------------------------------------------------------------------
# Backends
# --------------------------------------------------------------------------

def cubeprog_connect(args: argparse.Namespace) -> list[str]:
    # sn= picks the probe when two ST-LINKs are connected; without it the CLI
    # takes the first one it enumerates (05_i2c plan D7, S18).
    sn = getattr(args, "sn", None)
    return ["-c", "port=SWD", f"mode={args.mode}"] + ([f"sn={sn}"] if sn else [])


def cmd_cubeprog(args: argparse.Namespace, images: list[tuple[str, Path]]) -> list[str]:
    cli = str(require(find_tool("STM32_Programmer_CLI.exe", "cubeprogrammer"), "STM32_Programmer_CLI.exe"))
    cmd = [cli] + cubeprog_connect(args)

    if args.erase:
        cmd += ["-e", "all"]

    for _core, elf in images:
        cmd += ["-w", str(elf)]

    if not args.no_verify:
        cmd += ["-v"]
    if not args.no_reset:
        cmd += ["-rst"]
    return cmd


def cmd_openocd(args: argparse.Namespace, images: list[tuple[str, Path]]) -> list[str]:
    ocd = str(require(find_tool("openocd.exe", "openocd"), "openocd.exe"))
    scripts = find_openocd_scripts()
    if scripts is None:
        die("OpenOCD script directory (st_scripts) not found; use --tool cubeprog")

    board_cfg = TOOLS_ROOT / "openocd" / "stm32h755_dualcore.cfg"
    if not board_cfg.is_file():
        die(f"missing {board_cfg}")

    # Both flash banks are reachable through the CPU1 target (bank1.cm7 at
    # 0x08000000, bank2.cm7 at 0x08100000), so everything is programmed from
    # there. Selecting it explicitly matters: the config declares CPU2 too, and
    # target creation leaves *that* one current. While CPU2 is not booting its
    # examination fails, and a program command aimed at it would fail with it.
    # Declaring CPU2 is only honest once it has an image and BCM4 is set. While
    # it is held in reset its access port is unreachable and every reset dies
    # with "Debug AP not available, reset NOT asserted!", so the config is told
    # to stay single-core unless a CPU2 image is actually going on the board.
    programming_cm4 = any(core == "cm4" for core, _ in images)
    cmd = [ocd, "-s", str(scripts)]
    if not programming_cm4:
        cmd += ["-c", "set DUAL_CORE 0"]
    cmd += ["-f", str(board_cfg),
            "-c", "init",
            "-c", f"targets {OPENOCD_CHIPNAME}.cm7",
            "-c", "reset halt"]
    if args.erase:
        cmd += ["-c", "flash erase_sector 0 0 last", "-c", "flash erase_sector 1 0 last"]
    for _core, elf in images:
        verify = "" if args.no_verify else " verify"
        cmd += ["-c", f"program {elf.as_posix()}{verify}"]
    cmd += ["-c", "reset run" if not args.no_reset else "reset halt", "-c", "shutdown"]
    return cmd


# --------------------------------------------------------------------------
# Sub-commands that do not flash
# --------------------------------------------------------------------------

def do_list(args: argparse.Namespace) -> int:
    cli = str(require(find_tool("STM32_Programmer_CLI.exe", "cubeprogrammer"), "STM32_Programmer_CLI.exe"))
    return run([cli, "--list"], args.dry_run)


def do_read_ob(args: argparse.Namespace) -> int:
    cli = str(require(find_tool("STM32_Programmer_CLI.exe", "cubeprogrammer"), "STM32_Programmer_CLI.exe"))
    return run([cli] + cubeprog_connect(args) + ["-ob", "displ"], args.dry_run)


def do_write_ob(args: argparse.Namespace) -> int:
    cli = str(require(find_tool("STM32_Programmer_CLI.exe", "cubeprogrammer"), "STM32_Programmer_CLI.exe"))

    for pair in args.ob:
        if "=" not in pair:
            die(f"--ob expects NAME=VALUE, got {pair!r}")

    print("\nAbout to write option bytes. These persist across resets and a wrong")
    print("boot address leaves a board that does not start.\n")
    for pair in args.ob:
        name, value = pair.split("=", 1)
        print(f"    {name:<20} = {value}")
    print("\nRun --read-ob first if you have not checked the current values.")

    if not confirm("\nWrite these option bytes?", args.yes):
        print("Aborted; nothing written.")
        return 1

    return run([cli] + cubeprog_connect(args) + ["-ob"] + list(args.ob), args.dry_run)


# --------------------------------------------------------------------------

def build_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(
        prog=SCRIPT_NAME,
        description="Flash the STM32H755ZI on the Nucleo-H755ZI-Q.",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="Run with --dry-run to see the exact command without touching the board.",
    )
    p.add_argument("--preset", default="debug", choices=_project_presets(),
                   help="CMake preset whose build/ output to flash, one of this project's "
                        "CMakePresets.json (default: debug)")
    p.add_argument("--core", default="auto", choices=("auto", "cm7", "cm4", "both"),
                   help="which core to program; 'auto' takes whatever is built (default)")
    p.add_argument("--tool", default="cubeprog", choices=("cubeprog", "openocd"),
                   help="programming backend (default: cubeprog)")
    p.add_argument("--mode", default="UR", choices=("UR", "HOTPLUG", "NORMAL"),
                   help="CubeProgrammer connect mode; UR = under reset (default)")
    p.add_argument("--elf", type=Path, metavar="PATH",
                   help="program this ELF instead of the build tree (implies a single core)")
    p.add_argument("--erase", action="store_true", help="full chip erase before programming")
    p.add_argument("--no-verify", action="store_true", help="skip readback verification")
    p.add_argument("--no-reset", action="store_true", help="leave the target halted after programming")
    p.add_argument("--dry-run", action="store_true", help="print commands, execute nothing")
    p.add_argument("--yes", action="store_true", help="skip confirmation prompts")
    p.add_argument("--sn", metavar="SN",
                   help="serial number of the ST-LINK to use (passed as sn=; see --list). Without it "
                        "CubeProgrammer takes the first probe it finds. cubeprog backend only")

    g = p.add_argument_group("inspection (these never write flash)")
    g.add_argument("--list", action="store_true", help="list connected ST-LINK probes and exit")
    g.add_argument("--read-ob", action="store_true", help="dump option bytes and exit")
    g.add_argument("--ob", action="append", metavar="NAME=VALUE", default=[],
                   help="write an option byte; repeatable, always confirmed")
    return p


def main(argv: list[str] | None = None) -> int:
    args = build_parser().parse_args(argv)

    if args.sn is not None:
        if not args.sn.isalnum():
            die(f"--sn expects the ST-LINK serial number as --list prints it, got {args.sn!r}")
        if args.tool != "cubeprog":
            die("--sn is implemented for the cubeprog backend only; this OpenOCD setup "
                "has no probe selection, so it would use whichever ST-LINK it finds first")

    if args.list:
        return do_list(args)
    if args.read_ob:
        return do_read_ob(args)
    if args.ob:
        return do_write_ob(args)

    # --- decide what to program ---
    images: list[tuple[str, Path]] = []

    if args.elf:
        elf = args.elf if args.elf.is_absolute() else (PROJECT_ROOT / args.elf)
        if not elf.is_file():
            die(f"no such file: {elf}")
        core = "cm4" if "cm4" in elf.name.lower() else "cm7"
        images.append((core, elf))
    else:
        wanted = ("cm7", "cm4") if args.core in ("auto", "both") else (args.core,)
        for core in wanted:
            elf = artifact(args.preset, core)
            if elf:
                images.append((core, elf))
            elif args.core in ("cm4", "cm7", "both"):
                die(
                    f"no {core.upper()} image for preset '{args.preset}'.\n"
                    f"  Expected: build/{args.preset}/{core.upper()}/{CORES[core]['image']}.elf\n"
                    f"  Build it:  cmake --build --preset {args.preset}"
                )

    if not images:
        die(
            f"nothing built for preset '{args.preset}'.\n"
            f"  Expected at least: build/{args.preset}/CM7/cm7_firmware.elf\n"
            f"  Build it:  cmake --preset {args.preset} && cmake --build --preset {args.preset}"
        )

    print(f"\nTarget   : STM32H755ZI / Nucleo-H755ZI-Q")
    print(f"Backend  : {args.tool}" + (f"  (connect mode {args.mode})" if args.tool == "cubeprog" else ""))
    print(f"Preset   : {args.preset}")
    if args.sn:
        print(f"Probe    : ST-LINK SN {args.sn}")
    for core, elf in images:
        size = elf.stat().st_size
        print(f"  {CORES[core]['cpu']:<22} bank {CORES[core]['bank']} @ 0x{CORES[core]['base']:08X}"
              f"  {elf.relative_to(PROJECT_ROOT)}  ({size:,} B)")
        warn_if_stale(elf)

    # CPU2 boots from bank 2 only when BCM4 is set. Flashing CPU1 alone is fine;
    # the hazard is the opposite case, documented in docs/ETHERNET_BRINGUP.md.
    if len(images) == 1 and images[0][0] == "cm7":
        info("CPU2 image not programmed. If BCM4 is set, CPU2 will execute erased flash"
             " - see --read-ob and docs/ETHERNET_BRINGUP.md.")

    if args.erase and not confirm("\nFull chip erase wipes both banks. Continue?", args.yes):
        print("Aborted; nothing erased.")
        return 1

    cmd = cmd_cubeprog(args, images) if args.tool == "cubeprog" else cmd_openocd(args, images)
    rc = run(cmd, args.dry_run)

    if args.dry_run:
        print("Dry run: nothing was sent to the board.")
    elif rc == 0:
        print("\nDone.")
    else:
        print(f"\n{args.tool} exited with {rc}.", file=sys.stderr)
    return rc


if __name__ == "__main__":
    try:
        sys.exit(main())
    except KeyboardInterrupt:
        sys.exit(130)
