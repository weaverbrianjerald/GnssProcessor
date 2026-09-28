#!/usr/bin/env python3
"""Check that two runs share a byte-identical solve (standing invariant #6).

An outputs-only knob (an emission filter, a trace, a legend) must leave the
estimator untouched. Two conditions this encodes:

  * Product files must be compared on their records only: the ``#`` legend and
    banner lines and the ``S`` contract lines are header, not wire content, and a
    whole-file hash gives a false positive on them (card OWD made the S line part
    of every wire, so a pre-OWD run against a post-OWD run differs in the header
    alone). The header is hashed and reported separately.
  * ``network_solution.csv`` CLK/CB values come straight from the solve and must
    match to the digit, whereas PB values are re-based at emit and are exactly
    what an emission filter is allowed to change. Comparing all three together
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
    """MD5 of a product file's records: every line that is neither a ``#`` legend or
    banner line nor an ``S`` contract line (card OWD: the S line is header)."""
    if not products.is_file():
        return None
    h = hashlib.md5()
    with products.open("rb") as fh:
        for raw in fh:
            if not raw.startswith(b"#") and not raw.startswith(b"S,"):
                h.update(raw)
    return h.hexdigest()


def header_digest(products: Path) -> str | None:
    """The header, reported apart from the records: the banner, the number of S
    contract lines and an MD5 over the ``#`` and ``S`` lines."""
    if not products.is_file():
        return None
    h = hashlib.md5()
    banner, n_s = "", 0
    with products.open("rb") as fh:
        for raw in fh:
            if raw.startswith(b"#") or raw.startswith(b"S,"):
                h.update(raw)
                if raw.startswith(b"# gnss_network_product") and not banner:
                    banner = raw.decode("ascii", "replace").strip("# \r\n")
                n_s += raw.startswith(b"S,")
    return f"{banner or 'no banner'}, {n_s} S line(s), md5 {h.hexdigest()}"


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
    print(f"products.txt record MD5 (the # lines and the S contract lines excluded):")
    print(f"  {a.name}: {ha}")
    print(f"  {b.name}: {hb}")
    print(f"  => {'IDENTICAL' if ha and ha == hb else 'DIFFER (expected if the knob is an EMISSION filter)'}")
    ga, gb = header_digest(a / "products.txt"), header_digest(b / "products.txt")
    print(f"products.txt header, reported apart (a header difference is not a solve difference):")
    print(f"  {a.name}: {ga}")
    print(f"  {b.name}: {gb}")
    print(f"  => {'same' if ga == gb else 'differs'}\n")

    for kind in ("CLK", "CB", "PB"):
        print(" ", compare_kind(a, b, kind))
    print("\nCLK/CB come from the solve and MUST match for an outputs-only knob.\n"
          "PB is re-based at emit, so a PB delta is the knob working, not a regression.")


if __name__ == "__main__":
    main()
