#!/usr/bin/env python3
"""Check that two runs share a byte-identical SOLVE (standing invariant #6).

An OUTPUTS-only knob (an emission filter, a trace, a legend) must leave the
estimator untouched.  Two gotchas this encodes:

  * product files must be compared with ``^#`` lines STRIPPED -- a legend or
    banner delta is not a wire regression, and a whole-file hash false-positives
    on it;
  * ``network_solution.csv`` CLK/CB values come straight from the solve and must
    match to the digit, whereas PB values are re-based at EMIT and are exactly
    what an emission filter is allowed to change.  Comparing all three together
    would hide a real solve change behind an expected PB change, or flag an
    intended PB change as a solve regression.

Usage:
  python scripts/check_solve_identity.py runs/agu63 runs/agu63_pbv2
"""
from __future__ import annotations

import argparse
import hashlib
from pathlib import Path

import pandas as pd

REPO = Path(__file__).resolve().parent.parent


def data_hash(products: Path) -> str | None:
    """MD5 of a product file's non-comment lines."""
    if not products.is_file():
        return None
    h = hashlib.md5()
    with products.open("rb") as fh:
        for raw in fh:
            if not raw.startswith(b"#"):
                h.update(raw)
    return h.hexdigest()


def compare_kind(a: Path, b: Path, kind: str) -> str:
    ca, cb = (pd.read_csv(p / "logs" / "network_solution.csv") for p in (a, b))
    key = ["tow", "sat", "signal", "kind"]
    ca, cb = (c[c["kind"] == kind].dropna(subset=["tow", "value_m"]) for c in (ca, cb))
    ca, cb = (c.assign(signal=c["signal"].fillna("")) for c in (ca, cb))
    m = ca.merge(cb, on=key, suffixes=("_a", "_b"))
    if m.empty:
        return f"{kind}: NO OVERLAP ({len(ca)} vs {len(cb)} rows)"
    d = (m["value_m_a"] - m["value_m_b"]).abs()
    return (f"{kind}: {len(m)} common rows, max |delta| = {d.max():.6e} m, "
            f"{'IDENTICAL' if d.max() == 0 else 'DIFFERS'}"
            f"  (rows a={len(ca)} b={len(cb)})")


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("run_a")
    ap.add_argument("run_b")
    args = ap.parse_args()
    a, b = (REPO / args.run_a, REPO / args.run_b)

    ha, hb = data_hash(a / "products.txt"), data_hash(b / "products.txt")
    print(f"products.txt data-line MD5 (comments stripped):")
    print(f"  {a.name}: {ha}")
    print(f"  {b.name}: {hb}")
    print(f"  => {'IDENTICAL' if ha and ha == hb else 'DIFFER (expected if the knob is an EMISSION filter)'}\n")

    for kind in ("CLK", "CB", "PB"):
        print(" ", compare_kind(a, b, kind))
    print("\nCLK/CB come from the solve and MUST match for an outputs-only knob.\n"
          "PB is re-based at emit, so a PB delta is the knob working, not a regression.")


if __name__ == "__main__":
    main()
