#!/usr/bin/env python
"""
Exercises the Ethernet data path of a running board from the host side.

Why this exists
---------------
The firmware's own counters stay flat while nothing talks to the board, so
`rx=0` after boot proves nothing. Something on the network has to drive the
receive path before the numbers mean anything. This is that something.

What it actually tests, beyond "it replies"
-------------------------------------------
  ICMP          the host resolves the station address and gets answers. The
                first reply is slow because of the ARP, not the firmware.

  Size sweep    1 B up to 1472 B, the largest UDP payload that still fits one
                frame (1500 MTU - 20 IP - 8 UDP). That last size is the point
                of the sweep: it is what exercises the ETH_DRIVER_BUFFER_SIZE
                buffers in .eth_ram. A pool mis-sized, or not 32-byte
                aligned, fails there and never on small packets.

  Burst         datagrams back to back with no pause. The Rx pool is 8
                buffers and the Tx pool 4 (see eth_hal.h), so they have to
                turn over faster than frames arrive. A leak in
                ETH_Driver_ReclaimTxBuffers(), or an Rx segment that is never
                released, shows up here as losses - and on the board as
                rx_nobuf / tx_nobuf climbing off zero.

Cross-check the board's side while this runs, or right after:

    py tools/monitor.py --timeout 15

A pass here with rx_nobuf, tx_nobuf and dma_err still at zero there is what
makes the result conclusive: both ends agree, so the host is not talking to
something else on the segment.

Read-only with respect to the target: nothing is flashed and nothing is
written over SWD. The board only has to be running an image that answers
ICMP and echoes UDP on port 7.

Related:
    01_shared_drivers/08_eth/eth_hal.h    the buffer pools and their sizes
    <project>/CM7/App/Inc/net_config.h    the address this defaults to
    <project>/docs/ETHERNET_BRINGUP.md    the full bring-up guide
"""

from __future__ import annotations

import argparse
import os
import re
import socket
import subprocess
import sys
import time
from pathlib import Path

TOOLS_ROOT = Path(__file__).resolve().parent

# 1500 MTU - 20 IP header - 8 UDP header. Anything larger fragments, which
# tests the host's IP stack rather than the MAC.
MAX_SINGLE_FRAME_PAYLOAD = 1472
DEFAULT_SIZES = (1, 25, 512, 1024, MAX_SINGLE_FRAME_PAYLOAD)
ECHO_PORT = 7


# --------------------------------------------------------------------------
# Output
# --------------------------------------------------------------------------

def info(msg: str) -> None:
    print(f"  {msg}")


def warn(msg: str) -> None:
    print(f"WARNING: {msg}", file=sys.stderr)


def die(msg: str, code: int = 1):
    print(f"ERROR: {msg}", file=sys.stderr)
    sys.exit(code)


# --------------------------------------------------------------------------
# Which project, and therefore which address
# --------------------------------------------------------------------------

def project_root() -> Path | None:
    """The firmware tree this run belongs to, if there is one.

    A project's tools/net_check.py shim exports STM32_PROJECT_ROOT; run
    directly, walk up from the current directory. Unlike flash.py this is not
    fatal when missing - the board can always be named with --host.
    """
    injected = os.environ.get("STM32_PROJECT_ROOT")
    if injected:
        return Path(injected).resolve()

    cwd = Path.cwd().resolve()
    for base in (cwd, *cwd.parents):
        if (base / "CMakePresets.json").is_file():
            return base
    return None


def address_from_net_config(root: Path | None) -> str | None:
    """The static IP the project's net_config.h sets, if it sets one.

    Returns None when the project uses DHCP, since the leased address is not
    knowable from the source. --host covers that case.
    """
    if root is None:
        return None

    header = root / "CM7" / "App" / "Inc" / "net_config.h"
    if not header.is_file():
        return None

    text = header.read_text(encoding="utf-8", errors="replace")

    dhcp = re.search(r"#define\s+NET_USE_DHCP\s+(\d+)", text)
    if dhcp and dhcp.group(1) != "0":
        warn(f"{header.name} sets NET_USE_DHCP, so the address is a lease.\n"
             "         Pass --host with whatever the board actually got.")
        return None

    octets = []
    for i in range(4):
        m = re.search(rf"#define\s+NET_STATIC_IP_ADDR{i}\s+(\d+)", text)
        if not m:
            return None
        octets.append(m.group(1))
    return ".".join(octets)


# --------------------------------------------------------------------------
# Checks
# --------------------------------------------------------------------------

def check_icmp(host: str, count: int) -> bool:
    flag = "-n" if os.name == "nt" else "-c"
    proc = subprocess.run(["ping", flag, str(count), host],
                          capture_output=True, text=True)
    replies = len(re.findall(r"(?i)\bttl=", proc.stdout))
    ok = replies == count
    info(f"{'OK  ' if ok else 'FAIL'}  ICMP  {replies}/{count} replies")
    if not ok:
        info("      the board is not answering: check the link LED, the")
        info("      address, and that the host is on the same segment")
    return ok


def _echo(sock: socket.socket, host: str, payload: bytes) -> tuple[bool, float]:
    start = time.perf_counter()
    sock.sendto(payload, (host, ECHO_PORT))
    try:
        data, _ = sock.recvfrom(len(payload) + 512)
    except socket.timeout:
        return False, 0.0
    return data == payload, (time.perf_counter() - start) * 1000.0


def check_sizes(host: str, sizes: tuple[int, ...], timeout: float) -> bool:
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.settimeout(timeout)
    all_ok = True

    for size in sizes:
        # A varying pattern rather than a constant byte, so a driver that
        # returns a stale buffer of the right length still fails.
        payload = bytes((i * 7 + size) & 0xFF for i in range(size))
        ok, rtt = _echo(sock, host, payload)
        all_ok &= ok
        verdict = f"rtt={rtt:5.2f} ms" if ok else "no reply, or bytes differ"
        info(f"{'OK  ' if ok else 'FAIL'}  UDP {size:5d} B  {verdict}")
        time.sleep(0.15)

    sock.close()
    return all_ok


def check_burst(host: str, count: int, timeout: float) -> bool:
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.settimeout(timeout)
    lost = 0

    start = time.perf_counter()
    for i in range(count):
        payload = f"burst-{i:05d}-".encode() * 8
        ok, _ = _echo(sock, host, payload)
        if not ok:
            lost += 1
    elapsed = time.perf_counter() - start
    sock.close()

    returned = count - lost
    info(f"{'OK  ' if lost == 0 else 'FAIL'}  burst  {returned}/{count} returned"
         f"  ({elapsed:.1f} s)")
    if lost:
        info("      buffer recycling: check rx_nobuf / tx_nobuf on the board")
    return lost == 0


# --------------------------------------------------------------------------

def build_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(
        prog="tools/net_check.py",
        description="Exercise the Ethernet data path of a running board.",
        epilog="Cross-check the board's counters with: py tools/monitor.py --timeout 15",
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    p.add_argument("--host", help="board address (default: the static IP in "
                                  "the project's net_config.h)")
    p.add_argument("--ping", type=int, default=4, metavar="N",
                   help="ICMP echo requests to send (default: 4, 0 skips)")
    p.add_argument("--burst", type=int, default=100, metavar="N",
                   help="back-to-back datagrams (default: 100, 0 skips)")
    p.add_argument("--timeout", type=float, default=2.0, metavar="SECONDS",
                   help="per-datagram reply timeout (default: 2.0)")
    p.add_argument("--sizes", help="comma-separated payload sizes to sweep "
                                   f"(default: {','.join(map(str, DEFAULT_SIZES))})")
    return p


def main() -> int:
    args = build_parser().parse_args()

    root = project_root()
    host = args.host or address_from_net_config(root)
    if not host:
        die("no board address: pass --host, or run this from a project whose "
            "net_config.h sets a static IP")

    if args.sizes:
        try:
            sizes = tuple(int(s) for s in args.sizes.split(",") if s.strip())
        except ValueError:
            die(f"--sizes takes comma-separated integers, got {args.sizes!r}")
        oversized = [s for s in sizes if s > MAX_SINGLE_FRAME_PAYLOAD]
        if oversized:
            warn(f"{oversized} exceed {MAX_SINGLE_FRAME_PAYLOAD} B and will "
                 "fragment, which tests the host IP stack more than the MAC")
    else:
        sizes = DEFAULT_SIZES

    where = f" ({root.name})" if root else ""
    print(f"Board    : {host}{where}")
    print(f"UDP echo : port {ECHO_PORT}\n")

    results = []
    if args.ping:
        results.append(check_icmp(host, args.ping))
    if sizes:
        results.append(check_sizes(host, sizes, args.timeout))
    if args.burst:
        results.append(check_burst(host, args.burst, args.timeout))

    if not results:
        die("nothing to do: --ping 0 --burst 0 with no sizes")

    passed = all(results)
    print(f"\n{'PASS' if passed else 'FAIL'}  "
          f"{sum(results)}/{len(results)} checks")
    if passed:
        print("Now confirm the board agrees: rx_nobuf, tx_nobuf and dma_err\n"
              "should all still read 0 in  py tools/monitor.py --timeout 15")
    return 0 if passed else 1


if __name__ == "__main__":
    try:
        sys.exit(main())
    except KeyboardInterrupt:
        sys.exit(130)
