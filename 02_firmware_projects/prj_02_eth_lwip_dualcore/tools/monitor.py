#!/usr/bin/env python3
"""Serial console for the firmware log on the ST-LINK Virtual COM Port.

The firmware logs through USART3 at 115200 8N1 (PD8 = TX, PD9 = RX, AF7), which
the on-board ST-LINK/V3 of the Nucleo-H755ZI-Q exposes to the host as a virtual
COM port. `app_log.c` writes whole lines terminated with CRLF; this reads them
back.

No dependencies
---------------
This talks to the port through the Win32 API with ctypes rather than pyserial,
because pyserial is not installed and this workspace has no Python dependencies
to keep installed. The cost is that it is Windows-only, which the rest of the
tooling already is (tools/env.ps1 is PowerShell, the toolchain paths are .exe).

Usage
-----
Use the `py` launcher; the bare `python` on this machine is the Microsoft Store
alias stub and does not run.

    py tools/monitor.py                  # find the ST-LINK VCP and follow it
    py tools/monitor.py --list           # show the serial ports it can see
    py tools/monitor.py --port COM4      # skip the autodetection
    py tools/monitor.py --timestamps     # prefix each line with elapsed time
    py tools/monitor.py --log boot.txt   # also append to a file

Ctrl-C exits. The port is reopened automatically when it disappears - resetting
or reflashing the board drops the VCP for a moment - so the monitor can be left
running in one window while you flash from another.

This is a viewer: it never writes to the target. Nothing in the firmware reads
from the console, so there is nothing to type at.

See also
--------
01_shared_drivers/04_uart/uart_hal.h  the UART contract this mirrors
docs/ETHERNET_BRINGUP.md              what the banner lines should read
"""

from __future__ import annotations

import argparse
import ctypes
import sys
import time
from ctypes import wintypes
from pathlib import Path

if sys.platform != "win32":
    sys.exit("monitor.py uses the Win32 serial API and only runs on Windows.")

import winreg

SCRIPT_NAME = f"{Path(__file__).parent.name}/{Path(__file__).name}"

# 01_shared_drivers/04_uart/uart_hal.h: UART_CONSOLE_DEFAULT_BAUDRATE, 8N1.
DEFAULT_BAUD = 115200

# ST-LINK USB vendor id. The PID varies with the ST-LINK generation
# (V2 = 0x3748, V2-1 = 0x374B, V3 = 0x374E/0x374F/...), so match on vendor and
# on the port simply existing, rather than maintaining a PID list.
STLINK_VID = "VID_0483"

# --- Win32 -----------------------------------------------------------------
GENERIC_READ = 0x80000000
OPEN_EXISTING = 3
INVALID_HANDLE_VALUE = wintypes.HANDLE(-1).value
MAXDWORD = 0xFFFFFFFF
PURGE_RXCLEAR = 0x0008
PURGE_TXCLEAR = 0x0004
ERROR_ACCESS_DENIED = 5
ERROR_FILE_NOT_FOUND = 2


class DCB(ctypes.Structure):
    _fields_ = [
        ("DCBlength", wintypes.DWORD),
        ("BaudRate", wintypes.DWORD),
        ("_bitfield", wintypes.DWORD),
        ("wReserved", wintypes.WORD),
        ("XonLim", wintypes.WORD),
        ("XoffLim", wintypes.WORD),
        ("ByteSize", ctypes.c_byte),
        ("Parity", ctypes.c_byte),
        ("StopBits", ctypes.c_byte),
        ("XonChar", ctypes.c_char),
        ("XoffChar", ctypes.c_char),
        ("ErrorChar", ctypes.c_char),
        ("EofChar", ctypes.c_char),
        ("EvtChar", ctypes.c_char),
        ("wReserved1", wintypes.WORD),
    ]


class COMMTIMEOUTS(ctypes.Structure):
    _fields_ = [
        ("ReadIntervalTimeout", wintypes.DWORD),
        ("ReadTotalTimeoutMultiplier", wintypes.DWORD),
        ("ReadTotalTimeoutConstant", wintypes.DWORD),
        ("WriteTotalTimeoutMultiplier", wintypes.DWORD),
        ("WriteTotalTimeoutConstant", wintypes.DWORD),
    ]


_k32 = ctypes.WinDLL("kernel32", use_last_error=True)
_k32.CreateFileW.restype = wintypes.HANDLE
_k32.CreateFileW.argtypes = [wintypes.LPCWSTR, wintypes.DWORD, wintypes.DWORD,
                             wintypes.LPVOID, wintypes.DWORD, wintypes.DWORD, wintypes.HANDLE]
_k32.ReadFile.argtypes = [wintypes.HANDLE, wintypes.LPVOID, wintypes.DWORD,
                          ctypes.POINTER(wintypes.DWORD), wintypes.LPVOID]


class SerialPort:
    """The smallest read-only Win32 serial port that does the job."""

    def __init__(self, port: str, baud: int) -> None:
        # \\.\COMn is required for COM10 and above; it is also valid below it.
        self.name = port
        path = f"\\\\.\\{port}"
        self.handle = _k32.CreateFileW(path, GENERIC_READ, 0, None, OPEN_EXISTING, 0, None)
        if self.handle == INVALID_HANDLE_VALUE:
            err = ctypes.get_last_error()
            if err == ERROR_ACCESS_DENIED:
                raise OSError(f"{port} is busy - another terminal, or a debugger, already has it open")
            if err == ERROR_FILE_NOT_FOUND:
                raise OSError(f"{port} does not exist")
            raise OSError(f"cannot open {port} (Win32 error {err})")

        try:
            self._configure(baud)
        except Exception:
            self.close()
            raise

    def _configure(self, baud: int) -> None:
        dcb = DCB()
        dcb.DCBlength = ctypes.sizeof(DCB)
        if not _k32.GetCommState(self.handle, ctypes.byref(dcb)):
            raise OSError(f"GetCommState failed (Win32 error {ctypes.get_last_error()})")

        # BuildCommDCBW parses the same mode string as the `mode` command and
        # fills the bitfield correctly, which is far less error-prone than
        # setting fBinary / fParity / fDtrControl by hand.
        mode = f"baud={baud} parity=N data=8 stop=1"
        if not _k32.BuildCommDCBW(mode, ctypes.byref(dcb)):
            raise OSError(f"BuildCommDCBW rejected {mode!r}")
        if not _k32.SetCommState(self.handle, ctypes.byref(dcb)):
            raise OSError(f"SetCommState failed (Win32 error {ctypes.get_last_error()})")

        # Return from ReadFile as soon as bytes arrive, and in any case after
        # 100 ms, so Ctrl-C is honoured promptly instead of blocking forever on
        # a silent port.
        timeouts = COMMTIMEOUTS(MAXDWORD, MAXDWORD, 100, 0, 0)
        if not _k32.SetCommTimeouts(self.handle, ctypes.byref(timeouts)):
            raise OSError(f"SetCommTimeouts failed (Win32 error {ctypes.get_last_error()})")

        # Drop whatever accumulated in the driver buffer before we attached; it
        # is output from before this session and only confuses the log.
        _k32.PurgeComm(self.handle, PURGE_RXCLEAR | PURGE_TXCLEAR)

    def read(self, size: int = 4096) -> bytes:
        buf = ctypes.create_string_buffer(size)
        got = wintypes.DWORD(0)
        if not _k32.ReadFile(self.handle, buf, size, ctypes.byref(got), None):
            raise OSError(f"read from {self.name} failed (Win32 error {ctypes.get_last_error()})")
        return buf.raw[: got.value]

    def close(self) -> None:
        if getattr(self, "handle", None) not in (None, INVALID_HANDLE_VALUE):
            _k32.CloseHandle(self.handle)
            self.handle = INVALID_HANDLE_VALUE

    def __enter__(self) -> "SerialPort":
        return self

    def __exit__(self, *exc: object) -> None:
        self.close()


# --- Finding the port ------------------------------------------------------

def _serialcomm_ports() -> list[str]:
    """Every COM port the system currently has, ST-LINK or not."""
    ports: list[str] = []
    try:
        with winreg.OpenKey(winreg.HKEY_LOCAL_MACHINE, r"HARDWARE\DEVICEMAP\SERIALCOMM") as key:
            for i in range(winreg.QueryInfoKey(key)[1]):
                _name, value, _type = winreg.EnumValue(key, i)
                ports.append(value)
    except OSError:
        pass
    return sorted(set(ports))


def _usb_serial_ports() -> dict[str, str]:
    """Map COM port -> USB device key, for the ports that are USB CDC devices.

    Walks HKLM\\SYSTEM\\CurrentControlSet\\Enum\\USB. Each device instance that
    exposes a serial port carries its name under `Device Parameters\\PortName`.
    """
    found: dict[str, str] = {}
    try:
        with winreg.OpenKey(winreg.HKEY_LOCAL_MACHINE, r"SYSTEM\CurrentControlSet\Enum\USB") as usb:
            for i in range(winreg.QueryInfoKey(usb)[0]):
                vid_pid = winreg.EnumKey(usb, i)
                try:
                    with winreg.OpenKey(usb, vid_pid) as dev:
                        for j in range(winreg.QueryInfoKey(dev)[0]):
                            instance = winreg.EnumKey(dev, j)
                            try:
                                with winreg.OpenKey(dev, instance + r"\Device Parameters") as params:
                                    port, _ = winreg.QueryValueEx(params, "PortName")
                                    found[str(port)] = vid_pid
                            except OSError:
                                continue
                except OSError:
                    continue
    except OSError:
        pass
    return found


def list_ports() -> list[tuple[str, str]]:
    """[(port, description)] with the ST-LINK ones identified."""
    usb = _usb_serial_ports()
    rows: list[tuple[str, str]] = []
    for port in _serialcomm_ports():
        key = usb.get(port)
        if key and key.startswith(STLINK_VID):
            rows.append((port, f"ST-LINK Virtual COM Port  ({key})"))
        elif key:
            rows.append((port, f"USB serial  ({key})"))
        else:
            rows.append((port, "on-board or virtual serial port"))
    # ST-LINK ports first: that is what anyone running this wants.
    rows.sort(key=lambda r: (0 if "ST-LINK" in r[1] else 1, r[0]))
    return rows


def autodetect() -> str | None:
    for port, desc in list_ports():
        if "ST-LINK" in desc:
            return port
    return None


# --- Following the log -----------------------------------------------------

def follow(port_name: str, baud: int, args: argparse.Namespace) -> int:
    log_file = None
    if args.log:
        log_path = args.log if args.log.is_absolute() else (Path.cwd() / args.log)
        log_file = open(log_path, "a", encoding="utf-8", buffering=1)
        print(f"Appending to {log_path}")

    started = time.monotonic()
    pending = b""
    # The hint below is for "the board never said anything", not for the gaps
    # between log lines - a firmware that prints a stats line every few seconds
    # is silent most of the time and must not be nagged about it.
    received_any = False
    announced_silence = False
    connected = False

    try:
        while True:
            try:
                with SerialPort(port_name, baud) as port:
                    if not connected:
                        print(f"Connected to {port_name} at {baud} 8N1. Ctrl-C to stop.\n")
                        connected = True
                    while True:
                        if args.timeout and (time.monotonic() - started >= args.timeout):
                            return 0
                        chunk = port.read()
                        if chunk:
                            pending += chunk
                            received_any = True
                            *lines, pending = pending.split(b"\n")
                            for raw in lines:
                                text = raw.rstrip(b"\r").decode("utf-8", errors="replace")
                                stamp = f"[{time.monotonic() - started:8.3f}] " if args.timestamps else ""
                                print(stamp + text, flush=True)
                                if log_file:
                                    log_file.write(stamp + text + "\n")
                        elif not received_any and not announced_silence and time.monotonic() - started > 5.0:
                            announced_silence = True
                            print("(nothing received yet - press the black RESET button on the board,"
                                  " or check the LED codes in README.md section 8)", flush=True)
            except OSError as exc:
                if not args.reconnect:
                    print(f"ERROR: {exc}", file=sys.stderr)
                    return 1
                if connected:
                    print(f"\n-- {exc}; waiting for the port to come back --", flush=True)
                    connected = False
                time.sleep(0.5)
    except KeyboardInterrupt:
        if pending:
            print(pending.decode("utf-8", errors="replace"), flush=True)
        print("\nStopped.")
        return 0
    finally:
        if log_file:
            log_file.close()


def build_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(
        prog=SCRIPT_NAME,
        description="Follow the firmware log on the ST-LINK Virtual COM Port.",
        epilog="Read-only: nothing is ever written to the target.",
    )
    p.add_argument("--port", help="serial port to open (default: the ST-LINK VCP, autodetected)")
    p.add_argument("--baud", type=int, default=DEFAULT_BAUD,
                   help=f"baud rate (default: {DEFAULT_BAUD}, matching uart_hal.h)")
    p.add_argument("--timestamps", action="store_true",
                   help="prefix each line with seconds since the monitor started")
    p.add_argument("--log", type=Path, metavar="FILE", help="append the output to FILE as well")
    p.add_argument("--list", action="store_true", help="list serial ports and exit")
    p.add_argument("--no-reconnect", dest="reconnect", action="store_false",
                   help="exit instead of waiting when the port disappears")
    p.add_argument("--timeout", type=float, default=None, metavar="SECONDS",
                   help="exit automatically after SECONDS (useful for automated testing)")
    return p


def main(argv: list[str] | None = None) -> int:
    args = build_parser().parse_args(argv)

    if args.list:
        rows = list_ports()
        if not rows:
            print("No serial ports found.")
            return 1
        width = max(len(r[0]) for r in rows)
        for port, desc in rows:
            print(f"  {port:<{width}}  {desc}")
        return 0

    port_name = args.port or autodetect()
    if not port_name:
        print("ERROR: no ST-LINK Virtual COM Port found.", file=sys.stderr)
        print("  Is the board plugged in? Run --list to see what is there,", file=sys.stderr)
        print("  then pass one with --port COMn.", file=sys.stderr)
        return 1

    return follow(port_name, args.baud, args)


if __name__ == "__main__":
    sys.exit(main())
