#!/usr/bin/env python3
"""Fractional-cycle histogram of the network cross-station DD ambiguities (B5).

The network producer, with ``network.trace_dd_fractional`` on, writes
``logs/network_dd_fractional.csv`` — the between-station double-differenced
undifferenced float ambiguities per (constellation, signal), now including the
fractional-cycle residual ``frac_cyc = dd_cyc - round(dd_cyc)`` (P1-2).  This is
the go/no-go instrument for float-bias user AR: if the emitted phase biases are
AR-grade, the DD ambiguities cluster on integers and ``frac_cyc`` piles up near
0 (equivalently ±0.5 is empty).  A smeared, flat histogram means the bias
product does not yet support <0.1-cycle user AR.

Usage:
  python view_network_dd_fractional.py <run_dir> [--signal L1C] [--save out.png]
  <run_dir> contains logs/network_dd_fractional.csv

Prints a per-(const,signal) text histogram + robust spread always; renders a
matplotlib figure when matplotlib is available (or is asked to --save).
GLONASS and the pivot station are already excluded producer-side.
"""
import argparse
import csv
import math
import os
import sys
from collections import defaultdict

SYS_NAME = {"G": "GPS", "E": "Galileo", "C": "BeiDou", "J": "QZSS", "S": "SBAS"}


def wrap_half(x):
    """Wrap a fractional cycle into (-0.5, 0.5]."""
    return x - round(x)


def load(path):
    # (const, signal) -> list of frac_cyc
    frac = defaultdict(list)
    with open(path, newline="") as f:
        for row in csv.DictReader(f):
            try:
                lam = float(row["lambda_m"])
                fc = float(row["frac_cyc"])
            except (KeyError, ValueError):
                continue
            if lam <= 0.0:  # no wavelength (unknown signal) -> cycle columns are 0, skip
                continue
            key = (row["cons"], row["signal"])
            frac[key].append(wrap_half(fc))
    return frac


def robust_std(vals):
    if not vals:
        return 0.0
    s = sorted(abs(v) for v in vals)
    mad = s[len(s) // 2]
    return 1.4826 * mad


def text_hist(vals, bins=21, width=50):
    if not vals:
        return "  (no samples)"
    counts = [0] * bins
    for v in vals:
        # v in (-0.5, 0.5]; map to [0, bins)
        idx = min(bins - 1, max(0, int((v + 0.5) * bins)))
        counts[idx] += 1
    top = max(counts) or 1
    lines = []
    for i, c in enumerate(counts):
        lo = -0.5 + i / bins
        bar = "#" * int(round(width * c / top))
        lines.append(f"  {lo:+.3f} | {bar} {c}")
    return "\n".join(lines)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("run_dir")
    ap.add_argument("--signal", default=None, help="filter to one signal (e.g. L1C)")
    ap.add_argument("--save", default=None, help="write the figure to this PNG")
    args = ap.parse_args()

    path = os.path.join(args.run_dir, "logs", "network_dd_fractional.csv")
    if not os.path.exists(path):
        sys.exit(f"not found: {path}\n(enable network.trace_dd_fractional and re-run)")

    frac = load(path)
    keys = sorted(frac)
    if args.signal:
        keys = [k for k in keys if k[1] == args.signal]
    if not keys:
        sys.exit("no (const,signal) samples with a known wavelength to plot")

    print(f"Network DD fractional-cycle histogram: {path}")
    for cst, sig in keys:
        vals = frac[(cst, sig)]
        near = sum(1 for v in vals if abs(v) < 0.1) / len(vals) * 100.0
        print(f"\n[{SYS_NAME.get(cst, cst)} {sig}]  n={len(vals)}  "
              f"robust-sigma={robust_std(vals):.3f} cyc  within 0.1 cyc={near:.1f}%")
        print(text_hist(vals))

    if not args.save:
        try:
            import matplotlib  # noqa: F401
        except ImportError:
            return
    try:
        import matplotlib
        if args.save:
            matplotlib.use("Agg")
        import matplotlib.pyplot as plt
    except ImportError:
        return

    n = len(keys)
    ncol = min(3, n)
    nrow = math.ceil(n / ncol)
    fig, axes = plt.subplots(nrow, ncol, figsize=(4.5 * ncol, 3.2 * nrow), squeeze=False)
    for ax, (cst, sig) in zip([a for r in axes for a in r], keys):
        vals = frac[(cst, sig)]
        ax.hist(vals, bins=41, range=(-0.5, 0.5), color="#440154")  # viridis dark
        ax.axvline(0.0, color="#21918c", lw=1)
        ax.set_title(f"{SYS_NAME.get(cst, cst)} {sig}  (n={len(vals)})", fontsize=9)
        ax.set_xlabel("frac cycle")
    for ax in [a for r in axes for a in r][n:]:
        ax.axis("off")
    fig.suptitle("Cross-station DD fractional cycle (tight near 0 = AR-grade biases)")
    fig.tight_layout()
    if args.save:
        fig.savefig(args.save, dpi=120)
        print(f"\nsaved {args.save}")
    else:
        plt.show()


if __name__ == "__main__":
    main()
