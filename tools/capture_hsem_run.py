#!/usr/bin/env python3
"""Capture a single, unedited hardware run of HSEM self-test with timestamps."""

from __future__ import annotations

import argparse
import subprocess
import sys
import time
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO_ROOT))

from tools.monitor import SerialPort, autodetect

CLI_PATH = r"C:\ST\STM32CubeIDE_2.2.0\STM32CubeIDE\plugins\com.st.stm32cube.ide.mcu.externaltools.cubeprogrammer.win32_2.2.500.202603051304\tools\bin\STM32_Programmer_CLI.exe"


def do_reset() -> None:
    res = subprocess.run(
        [CLI_PATH, "-c", "port=SWD", "mode=HOTPLUG", "-rst"],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    if res.returncode != 0:
        raise RuntimeError(f"Reset failed: {res.stderr or res.stdout}")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--out",
        type=Path,
        default=REPO_ROOT / "02_firmware_projects" / "prj_04_adc_dma_bm_eth" / "docs" / "hsem_selftest_capture.txt",
        help="Output capture file",
    )
    parser.add_argument("--timeout", type=float, default=25.0, help="Capture timeout in seconds")
    parser.add_argument("--port", type=str, default=None, help="Serial port")
    args = parser.parse_args()

    port_name = args.port or autodetect()
    if not port_name:
        sys.exit("ERROR: Could not autodetect ST-LINK virtual COM port.")

    print(f"Connecting to {port_name}...")
    with SerialPort(port_name, 115200) as ser:
        # Drain previous bytes
        ser.read(4096)
        time.sleep(0.1)
        ser.read(4096)

        print("Triggering hardware reset via SWD...")
        do_reset()
        start_time = time.monotonic()

        lines: list[str] = []
        buf = bytearray()
        cm4_verdict_seen = False
        post_verdict_lines = 0

        while True:
            elapsed = time.monotonic() - start_time
            if elapsed > args.timeout:
                print(f"\nTimeout ({args.timeout}s) reached.")
                break

            chunk = ser.read(512)
            if chunk:
                buf.extend(chunk)
                while b"\n" in buf:
                    raw_line, buf = buf.split(b"\n", 1)
                    line_str = raw_line.decode("utf-8", errors="replace").rstrip("\r")
                    timestamp = time.monotonic() - start_time
                    formatted = f"[+{timestamp:6.3f}s] {line_str}"
                    print(formatted)
                    lines.append(formatted)

                    if "CM4 VERDICT=" in line_str:
                        cm4_verdict_seen = True

                    if cm4_verdict_seen:
                        if "(CPU2 alive)" in line_str or "CPU2 NOT RESPONDING" in line_str:
                            post_verdict_lines += 1
                            if post_verdict_lines >= 4:
                                break
            else:
                time.sleep(0.01)

            if cm4_verdict_seen and post_verdict_lines >= 4:
                break

    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text("\n".join(lines) + "\n", encoding="utf-8")
    print(f"\nSaved {len(lines)} lines to {args.out}")


if __name__ == "__main__":
    main()
