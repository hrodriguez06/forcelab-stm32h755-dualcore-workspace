#!/usr/bin/env python3
"""Per-object size diff between two GNU ld .map files.

Used for the F1a production check (F1A_INSTRUCTIONS.md §2.6): between the
production image of F0 and that of F1a only gpio_bm.c.obj and the totals may
change. Byte-for-byte parity no longer applies once gpio_bm.c changes.

Method
------
Only the "Linker script and memory map" part of each map is read, so the
"Discarded input sections" list never counts. Every input section placed in
the image is attributed to its object file and summed per class:
  text  every allocated section that is not .data*/.bss*/COMMON
        (.text*, .rodata*, .isr_vector, .ARM.exidx*, ...), as `size` does;
  data  .data*;
  bss   .bss* and COMMON.
Debug sections (.debug*, .comment, .ARM.attributes) and *fill* are ignored.
Object paths are normalised by dropping CMake's "CMakeFiles/<target>.dir/<hash>/"
prefix, so the same source compiled in two trees has the same name.

Usage
-----
    py tools/map_diff.py OLD.map NEW.map

Prints every object whose text, data or bss differ, then the totals, and exits
with 0. It reports; it does not judge.
"""

from __future__ import annotations

import re
import sys
from collections import defaultdict
from pathlib import Path

IGNORED = (".debug", ".comment", ".ARM.attributes", ".stab", ".note.gnu", ".gnu.attributes")
SECTION_ONE_LINE = re.compile(r"^ (\S+)\s+0x([0-9a-fA-F]+)\s+0x([0-9a-fA-F]+)\s+(\S.*)$")
SECTION_NAME_ONLY = re.compile(r"^ (\.\S+|COMMON)\s*$")
SECTION_CONT = re.compile(r"^\s+0x([0-9a-fA-F]+)\s+0x([0-9a-fA-F]+)\s+(\S.*)$")
CMAKE_PREFIX = re.compile(r"CMakeFiles/[^/]+\.dir/(?:[0-9a-f]{32}/)?")


def classify(section: str) -> str | None:
    if section.startswith(IGNORED) or section == "*fill*":
        return None
    if section.startswith(".bss") or section == "COMMON":
        return "bss"
    if section.startswith(".data"):
        return "data"
    return "text"


def normalise(obj: str) -> str:
    obj = obj.strip().replace("\\", "/")
    return CMAKE_PREFIX.sub("", obj)


def parse(path: Path) -> dict[str, dict[str, int]]:
    text = path.read_text(encoding="utf-8", errors="replace").splitlines()
    try:
        start = next(i for i, l in enumerate(text) if l.startswith("Linker script and memory map"))
    except StopIteration:
        raise SystemExit(f"{path}: no 'Linker script and memory map' section")

    sizes: dict[str, dict[str, int]] = defaultdict(lambda: {"text": 0, "data": 0, "bss": 0})
    pending: str | None = None
    for line in text[start:]:
        m = SECTION_ONE_LINE.match(line)
        if m and m.group(1).startswith((".", "COMMON")):
            section, addr, size, obj = m.groups()
            pending = None
        else:
            m2 = SECTION_NAME_ONLY.match(line)
            if m2:
                pending = m2.group(1)
                continue
            m3 = SECTION_CONT.match(line) if pending else None
            if not m3:
                pending = None
                continue
            section = pending
            addr, size, obj = m3.groups()
            pending = None
        cls = classify(section)
        if cls is None or obj.startswith("load address"):
            # output-section summary lines ("load address 0x...") carry no object
            continue
        sizes[normalise(obj)][cls] += int(size, 16)
    return sizes


def main(argv: list[str]) -> int:
    if len(argv) != 3:
        print(__doc__)
        return 2
    old_p, new_p = Path(argv[1]), Path(argv[2])
    old, new = parse(old_p), parse(new_p)
    print(f"old: {old_p}")
    print(f"new: {new_p}")
    print(f"{'object':70} {'text':>15} {'data':>13} {'bss':>13}")
    changed = 0
    for obj in sorted(set(old) | set(new)):
        a = old.get(obj, {"text": 0, "data": 0, "bss": 0})
        b = new.get(obj, {"text": 0, "data": 0, "bss": 0})
        if a != b:
            changed += 1
            cols = [f"{a[k]}->{b[k]}" if a[k] != b[k] else f"{a[k]}" for k in ("text", "data", "bss")]
            print(f"{obj[-70:]:70} {cols[0]:>15} {cols[1]:>13} {cols[2]:>13}")
    tot_a = {k: sum(v[k] for v in old.values()) for k in ("text", "data", "bss")}
    tot_b = {k: sum(v[k] for v in new.values()) for k in ("text", "data", "bss")}
    print(f"{'TOTAL':70} {tot_a['text']}->{tot_b['text']:>7} {tot_a['data']}->{tot_b['data']:>5} {tot_a['bss']}->{tot_b['bss']:>5}")
    print(f"objects changed: {changed}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
