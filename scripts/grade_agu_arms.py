#!/usr/bin/env python3
"""Tabulate held-out consumer performance across runs / replay arms.

Reads each run's ``logs/closed_loop_trace.csv`` and reports the final-hour
median error, the converged-window convention this project grades network runs
on.  A whole-run mean would average the ignition transient into the verdict.

Usage:
  python scripts/grade_agu_arms.py runs/agu63 runs/agu63_rp_*
"""
from __future__ import annotations

import argparse
import glob
import re
from pathlib import Path

import numpy as np
import pandas as pd

REPO = Path(__file__).resolve().parent.parent


def product_span(products: Path) -> tuple[float, float] | None:
    """(min, max) TOW covered by a product file, or None."""
    if not products.is_file():
        return None
    tows = []
    with products.open(errors="ignore") as fh:
        for line in fh:
            if line[:1] in ("H", "E"):
                m = re.search(r"(\d{6}\.\d)", line)
                if m:
                    tows.append(float(m.group(1)))
    return (min(tows), max(tows)) if tows else None


def grade(run_dir: Path, products: Path | None = None) -> dict | None:
    f = run_dir / "logs" / "closed_loop_trace.csv"
    if not f.is_file():
        return None
    df = pd.read_csv(f)
    if df.empty:
        return None
    df = df.sort_values("tow")

    # A consumer epoch past the product file's last record does not measure
    # product quality: with no fresh correction the consumer coasts on a frozen
    # set (or falls back to the external precise products it also has loaded),
    # and its error can improve sharply once coverage runs out (observed here:
    # 0 % to 96 % fixed and 3.9 m to 0.10 m). Grading that would report the
    # absence of the product as its success, so those epochs are excluded here,
    # and the count is surfaced in the 'cut' column rather than absorbed silently.
    n_all = len(df)
    if products is not None and (span := product_span(products)):
        df = df[(df["tow"] >= span[0]) & (df["tow"] <= span[1])]
        if df.empty:
            return None
    h = (df["tow"].to_numpy(float) - float(df["tow"].min())) / 3600.0
    tail = df[h >= max(0.0, h.max() - 1.0)] if h.max() > 1.5 else df
    conv = df[h >= 2.0] if h.max() > 3.0 else df
    return {
        "arm": run_dir.name,
        "ep": len(df),
        "cut": n_all - len(df),
        "hrs": round(float(h.max()), 2),
        "fix%": round(100.0 * float((df["ar_success"] > 0).mean()), 1),
        "E": round(float(tail["east_m"].median()), 3),
        "N": round(float(tail["north_m"].median()), 3),
        "U": round(float(tail["up_m"].median()), 3),
        "3D_h1": round(float(tail["pos_err_3d_m"].median()), 3),
        "3D_conv": round(float(conv["pos_err_3d_m"].median()), 3),
        "3D_p90": round(float(np.percentile(conv["pos_err_3d_m"], 90)), 3),
        "nsat": round(float(df["n_sats"].median()), 1),
    }


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("runs", nargs="+")
    ap.add_argument("--products", default=None,
                    help="product file the replay arms consumed; consumer epochs "
                         "outside its coverage are DROPPED (see grade()).")
    args = ap.parse_args()

    products = Path(args.products) if args.products else None
    if products and not products.is_absolute():
        products = REPO / products
    if products:
        span = product_span(products)
        print(f"product coverage: tow {span[0]:.1f}..{span[1]:.1f} "
              f"({(span[1]-span[0])/3600:.2f} h)\n" if span else "no product span\n")

    paths: list[Path] = []
    for pat in args.runs:
        hits = sorted(glob.glob(str(REPO / pat))) or sorted(glob.glob(pat))
        paths += [Path(p) for p in hits]

    rows = [r for p in paths if (r := grade(p, products))]
    if not rows:
        raise SystemExit("no gradeable runs found")
    out = pd.DataFrame(rows)
    print(out.to_string(index=False))
    print("\nE/N/U and 3D_h1 are FINAL-HOUR medians (m); 3D_conv/p90 cover "
          "everything past 2 h.\n'cut' = consumer epochs dropped for falling "
          "outside product coverage (must be 0 for a valid grade).\n"
          "Truth: ITRF2020 SSC, consumed nowhere in the engine.")


if __name__ == "__main__":
    main()
