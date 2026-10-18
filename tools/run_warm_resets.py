#!/usr/bin/env python3
"""Execute 20 consecutive warm resets and record monitor output for H6 test.

Without --project/--preset the tool behaves as it always did (10_hsem uses it
that way): a free --title, the host's HEAD as "Firmware Git Revision", and a
PASS/TIMEOUT per reset on the stop phrase. The one difference with the format
before 00_gpio R4-b is the first line of the capture, "[host] argv: <the
arguments, shell-quoted>". --count (default 20) sets the number of resets.

With --project <dir> --preset <name> (00_gpio R3-c) it also identifies the
image in every reset: CPU1 prints "[sys] build rev=<FW_GIT_REV> id=<FW_BUILD_ID>"
and "[sys] cpu2 build rev=... id=..." (prj_01 CM7/App/Src/app_main.c), and both
must equal the revision and the build id this checkout computes for
build/<preset>. The id is computed by host_build_id() of <project>/tools/
gpio_hw_check.py (imported, one definition; see cmake/fw_build_id.cmake), the
tree state is recorded as there (tree_record), and the title is composed from
the host revision and the id, with --title as a subtitle. The first reset that
differs fails the run, with its reason, and the exit code is 1. In this mode the
capture starts with "[host] argv: ..." and "[host] options: project=...
preset=... count=..." (R4-b).

With --sn <serial> (default None), STM32_Programmer_CLI targets that specific
ST-LINK probe. If --port is omitted and --sn is provided, the port is resolved
via monitor.port_for_sn(sn); otherwise it falls back to autodetect().
"""

from __future__ import annotations

import argparse
import importlib.util
import re
import shlex
import subprocess
import sys
import time
from pathlib import Path
from typing import Optional

# Add repo root to path to import monitor
REPO_ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO_ROOT))

from tools.monitor import SerialPort, autodetect

CLI_PATH = r"C:\ST\STM32CubeIDE_2.2.0\STM32CubeIDE\plugins\com.st.stm32cube.ide.mcu.externaltools.cubeprogrammer.win32_2.2.500.202603051304\tools\bin\STM32_Programmer_CLI.exe"


def do_reset(sn: Optional[str] = None) -> None:
    cmd = [CLI_PATH, "-c", "port=SWD"]
    if sn:
        cmd.append(f"sn={sn}")
    cmd.extend(["mode=HOTPLUG", "-rst"])
    res = subprocess.run(
        cmd,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    if res.returncode != 0:
        raise RuntimeError(f"Reset failed: {res.stderr or res.stdout}")


class Identity:
    """--project/--preset: what the image must say it is (R3-c)."""

    def __init__(self, project: Path, preset: str) -> None:
        spec = importlib.util.spec_from_file_location("gpio_hw_check", project / "tools" / "gpio_hw_check.py")
        if spec is None or spec.loader is None:
            raise SystemExit(f"Error: no tools/gpio_hw_check.py in {project}")
        self.ghc = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(self.ghc)
        self.project = project
        self.preset = preset
        self.build_id = self.ghc.host_build_id(preset, project)
        self.rev = self._rev()

    def _rev(self) -> str:
        """FW_GIT_REV's rule (cmake/fw_build_id.cmake): short HEAD, -dirty when
        git status --porcelain -uno lists anything."""
        try:
            head = subprocess.check_output(["git", "rev-parse", "--short", "HEAD"], cwd=str(self.project),
                                           text=True).strip()
            status = subprocess.check_output(["git", "status", "--porcelain", "-uno"], cwd=str(self.project),
                                             text=True).strip()
        except (OSError, subprocess.CalledProcessError):
            return "unknown"
        return f"{head}-dirty" if status else head

    def tree_lines(self) -> list[str]:
        lines: list[str] = []
        self.ghc.tree_record(lines.append)
        return lines

    def check(self, text: str) -> tuple[bool, str]:
        m7 = re.search(r"\[sys\] build rev=(\S+) id=(\S+)", text)
        m4 = re.search(r"\[sys\] cpu2 build rev=(\S+) id=(\S+)", text)
        got = (f"cpu1 rev={m7.group(1) if m7 else '(none)'} id={m7.group(2) if m7 else '(none)'}, "
               f"cpu2 rev={m4.group(1) if m4 else '(none)'} id={m4.group(2) if m4 else '(none)'}, "
               f"host rev={self.rev} id={self.build_id}")
        if self.build_id == "unknown" or self.rev == "unknown":
            return False, f"{got}: the host cannot compute its identity"
        if not m7 or not m4:
            return False, f"{got}: a core did not print its build line"
        ok = all(v == (self.rev, self.build_id) for v in ((m7.group(1), m7.group(2)), (m4.group(1), m4.group(2))))
        return ok, got if ok else f"{got}: the image is not the one this checkout builds"


def run_warm_resets(
    port_name: str,
    out_file: str,
    title: str,
    target_count: int = 20,
    stop_phrase: str = "(CPU2 alive)",
    identity: Optional[Identity] = None,
    argv_line: str = "",
    options_line: str = "",
    banner: str = "=== STM32H755ZI",
    sn: Optional[str] = None,
) -> int:
    print(f"Opening {port_name}...")
    git_rev = "unknown"
    try:
        git_rev = subprocess.check_output(["git", "rev-parse", "--short", "HEAD"], cwd=str(REPO_ROOT), text=True).strip()
    except Exception:
        pass

    failed: Optional[str] = None
    alive = 0
    with SerialPort(port_name, 115200) as ser:
        records: list[str] = []
        if argv_line:
            records.append(argv_line + "\n")
        if options_line:
            records.append(options_line + "\n")
        sn_cmd = f" sn={sn}" if sn else ""
        if identity is None:
            header = (
                "================================================================================\n"
                f"{title}\n"
                f"Firmware Git Revision: {git_rev}\n"
                f"Expected boot banner: {banner}\n"
                "Target: NUCLEO-H755ZI-Q (Silicon Rev V, ID 0x450, ST-LINK V3)\n"
                f"Reset method: STM32_Programmer_CLI -c port=SWD{sn_cmd} mode=HOTPLUG -rst\n"
                "================================================================================\n\n"
            )
        else:
            header = (
                "================================================================================\n"
                f"{identity.project.name} {identity.preset} at {identity.rev}, build id {identity.build_id}: "
                f"{target_count} warm resets\n"
                f"{title}\n"
                f"Host checkout: {identity.rev}; build id of build/{identity.preset}: {identity.build_id} "
                "(host_build_id of tools/gpio_hw_check.py)\n"
                "Each reset must print '[sys] build' and '[sys] cpu2 build' with this rev and id.\n"
                f"Expected boot banner: {banner}\n"
                + "".join(f"{line}\n" for line in identity.tree_lines())
                + "Target: NUCLEO-H755ZI-Q (Silicon Rev V, ID 0x450, ST-LINK V3)\n"
                f"Reset method: STM32_Programmer_CLI -c port=SWD{sn_cmd} mode=HOTPLUG -rst\n"
                "================================================================================\n\n"
            )
        records.append(header)

        for i in range(1, target_count + 1):
            print(f"[{i}/{target_count}] Triggering warm reset...", end="", flush=True)
            records.append(f"--- WARM RESET {i}/{target_count} ---\n")

            # Drain any buffered bytes from previous run (bounded by 1.0 s timeout, F-m1)
            time.sleep(0.05)
            t_drain = time.monotonic()
            while time.monotonic() - t_drain < 1.0 and ser.read(4096):
                pass

            do_reset(sn)

            # Collect output until stop_phrase or timeout
            t0 = time.monotonic()
            raw_buf = bytearray()
            found_alive = False

            while time.monotonic() - t0 < 6.0:
                chunk = ser.read(1024)
                if chunk:
                    raw_buf.extend(chunk)
                    banner_pos = raw_buf.find(banner.encode("utf-8"))
                    if banner_pos >= 0 and stop_phrase.encode("utf-8") in raw_buf[banner_pos:]:
                        found_alive = True
                        break
                else:
                    time.sleep(0.02)

            text = raw_buf.decode("utf-8", errors="replace").replace("\r\n", "\n")

            # Find the banner start
            banner_idx = text.find(banner)
            if banner_idx >= 0:
                text = text[banner_idx:]

            # Keep lines up to the stop_phrase
            lines = text.splitlines(keepends=True)
            run_lines = []
            for line in lines:
                run_lines.append(line)
                if stop_phrase in line:
                    break

            entry = "".join(run_lines).strip() + "\n\n"
            status = "PASS" if found_alive else "TIMEOUT/FAIL"
            alive += 1 if found_alive else 0
            if identity is not None:
                ok, why = identity.check("".join(run_lines))
                entry = entry.rstrip("\n") + f"\n[check] {why} -> {'PASS' if ok else 'FAIL'}\n\n"
                if not ok:
                    status = "IDENTITY FAIL"
                    failed = f"reset {i}: {why}"
            records.append(entry)

            print(f" {status}")
            if failed:
                break
            time.sleep(0.2)

        if identity is not None:
            done = i if failed else target_count
            records.append(f"RESULT: {alive}/{done} resets reached '{stop_phrase}'; identity "
                           f"{'FAIL (' + failed + ')' if failed else 'PASS in every reset'}\n")
        out_path = Path(out_file)
        out_path.parent.mkdir(parents=True, exist_ok=True)
        out_path.write_text("".join(records), encoding="utf-8")
        if failed:
            print(f"FAILED at {failed}. Output saved to: {out_file}")
            return 1
        print(f"All {target_count} resets complete. Output saved to: {out_file}")
    return 0


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--port", default=None, help="COM port (default autodetect)")
    parser.add_argument("--out", required=True, help="Output file path")
    parser.add_argument("--title", required=True, help="Header title (a subtitle with --project/--preset)")
    parser.add_argument("--project", type=Path, default=None,
                        help="project dir whose build/<preset> the image must match (with --preset)")
    parser.add_argument("--preset", default=None, help="configure preset of the flashed image (with --project)")
    parser.add_argument("--count", type=int, default=20, help="number of warm resets (default 20)")
    parser.add_argument("--banner", default="=== STM32H755ZI", help="expected boot banner prefix (default '=== STM32H755ZI')")
    parser.add_argument("--sn", default=None, help="ST-LINK serial number (optional; defaults to None; determines port via port_for_sn when --port is not given)")
    args = parser.parse_args()
    if args.count < 1:
        sys.exit("Error: --count must be at least 1")
    if (args.project is None) != (args.preset is None):
        sys.exit("Error: --project and --preset go together")

    if args.port:
        port = args.port
    elif args.sn:
        from tools.monitor import port_for_sn
        port = port_for_sn(args.sn)
        if not port:
            sys.exit(f"Error: no ST-LINK serial port found for SN {args.sn}")
    else:
        port = autodetect()
    if not port:
        sys.exit("Error: no ST-LINK serial port found")

    identity = Identity(args.project.resolve(), args.preset) if args.project is not None else None
    # R4-b: the arguments first; the resolved options only in the identity mode,
    # so that without the new options argv is the one line added to the old format.
    argv_line = f"[host] argv: {shlex.join(sys.argv[1:])}"
    options_line = (f"[host] options: project={identity.project} preset={identity.preset} count={args.count} banner={args.banner}"
                    if identity is not None else "")
    sys.exit(run_warm_resets(port, args.out, args.title, target_count=args.count, identity=identity,
                             argv_line=argv_line, options_line=options_line, banner=args.banner,
                             sn=args.sn))


if __name__ == "__main__":
    main()
