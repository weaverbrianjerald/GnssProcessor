#!/usr/bin/env python3
"""Mechanically diff the IAU 1980 106-term nutation table between
GnssProcessor (src/core/model/Planetary.cpp) and cssrlib
(reference_software/cssrlib/src/cssrlib/peph.py).

Both are ports of the same RTKLIB table, so they should be identical to
the digit.  Any row that differs is either a transcription bug in one of
them or a deliberate revision — either way worth surfacing.  This is the
kind of artifact eyeballing can't be trusted on.
"""

from __future__ import annotations
import re
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
GP = REPO / "src" / "core" / "model" / "Planetary.cpp"
CSS = REPO / "reference_software" / "cssrlib" / "src" / "cssrlib" / "peph.py"

NUM = r"-?\d+(?:\.\d+)?"


def parse_gp(text: str):
    """Rows look like:  {  0,  0,  2, -2,  2,   182.6,  -13187, ... }"""
    m = re.search(r"nut\[106\]\[10\]\s*=\s*\{(.*?)\n\s*\};", text, re.DOTALL)
    if not m:
        raise RuntimeError("GnssProcessor nut table not found")
    body = m.group(1)
    rows = re.findall(r"\{\s*(" + NUM + r"(?:\s*,\s*" + NUM + r"){9})\s*\}", body)
    return [[float(x) for x in re.split(r"\s*,\s*", r.strip())] for r in rows]


def parse_css(text: str):
    """Rows look like:  [0,   0,   0,   0,   1, -6798.4, -171996, ...],"""
    m = re.search(r"nut\s*=\s*np\.array\(\[(.*?)\]\)", text, re.DOTALL)
    if not m:
        raise RuntimeError("cssrlib nut table not found")
    body = m.group(1)
    rows = re.findall(r"\[\s*(" + NUM + r"(?:\s*,\s*" + NUM + r"){9})\s*\]", body)
    return [[float(x) for x in re.split(r"\s*,\s*", r.strip())] for r in rows]


def main() -> int:
    gp = parse_gp(GP.read_text(encoding="utf-8", errors="replace"))
    css = parse_css(CSS.read_text(encoding="utf-8", errors="replace"))
    print(f"GnssProcessor rows: {len(gp)}   cssrlib rows: {len(css)}")
    if len(gp) != 106 or len(css) != 106:
        print("!! row count mismatch")
    n = min(len(gp), len(css))
    diffs = 0
    for i in range(n):
        if gp[i] != css[i]:
            # Report only columns that differ.
            cols = [j for j in range(10) if gp[i][j] != css[i][j]]
            print(f"row {i}: differ in cols {cols}")
            print(f"   GP : {gp[i]}")
            print(f"   CSS: {css[i]}")
            diffs += 1
    if diffs == 0:
        print("OK — all 106 rows identical between GnssProcessor and cssrlib.")
    else:
        print(f"\n{diffs} differing row(s).")
    return 0


if __name__ == "__main__":
    sys.exit(main())
