#!/usr/bin/env python3
"""Execute 20 consecutive warm resets and record monitor output for H6 test."""

from __future__ import annotations

import argparse
import subprocess
import sys
import time
from pathlib import Path

# Add repo root to path to import monitor
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


def run_warm_resets(
    port_name: str,
    out_file: str,
    title: str,
    target_count: int = 20,
    stop_phrase: str = "(CPU2 alive)",
) -> None:
    print(f"Opening {port_name}...")
    with SerialPort(port_name, 115200) as ser:
        records: list[str] = []
        header = (
            "================================================================================\n"
            f"{title}\n"
            "Target: NUCLEO-H755ZI-Q (Silicon Rev V, ID 0x450, ST-LINK V3)\n"
            "Reset method: STM32_Programmer_CLI -c port=SWD mode=HOTPLUG -rst\n"
            "================================================================================\n\n"
        )
        records.append(header)

        for i in range(1, target_count + 1):
            print(f"[{i}/{target_count}] Triggering warm reset...", end="", flush=True)
            records.append(f"--- WARM RESET {i}/{target_count} ---\n")

            # Drain any buffered bytes from previous run
            ser.read(4096)

            do_reset()

            # Collect output until stop_phrase or timeout
            t0 = time.monotonic()
            raw_buf = bytearray()
            found_alive = False

            while time.monotonic() - t0 < 6.0:
                chunk = ser.read(1024)
                if chunk:
                    raw_buf.extend(chunk)
                    if stop_phrase.encode("utf-8") in raw_buf:
                        found_alive = True
                        break
                else:
                    time.sleep(0.02)

            text = raw_buf.decode("utf-8", errors="replace").replace("\r\n", "\n")

            # Find the banner start
            banner_idx = text.find("=== STM32H755ZI")
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
            records.append(entry)

            status = "PASS" if found_alive else "TIMEOUT/FAIL"
            print(f" {status}")
            time.sleep(0.2)

        out_path = Path(out_file)
        out_path.parent.mkdir(parents=True, exist_ok=True)
        out_path.write_text("".join(records), encoding="utf-8")
        print(f"All {target_count} resets complete. Output saved to: {out_file}")


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--port", default=None, help="COM port (default autodetect)")
    parser.add_argument("--out", required=True, help="Output file path")
    parser.add_argument("--title", required=True, help="Header title")
    args = parser.parse_args()

    port = args.port or autodetect()
    if not port:
        sys.exit("Error: no ST-LINK serial port found")

    run_warm_resets(port, args.out, args.title)


if __name__ == "__main__":
    main()
