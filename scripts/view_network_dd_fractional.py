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
    """Return (frac, sigma, series) keyed by (const, signal).

    frac[key]   -> list of frac_cyc (wrapped to (-0.5, 0.5])
    sigma[key]  -> list of dd_sigma_cyc (T2a formal DD σ, cycles; only rows with a
                   positive value — −1 marks "covariance unavailable")
    series[key] -> {(sat, ref, sta_a, sta_b): [dd_cyc over epochs]} for time-stability
    """
    frac = defaultdict(list)
    sigma = defaultdict(list)
    series = defaultdict(lambda: defaultdict(list))
    with open(path, newline="") as f:
        for row in csv.DictReader(f):
            try:
                lam = float(row["lambda_m"])
                fc = float(row["frac_cyc"])
                dc = float(row["dd_cyc"])
            except (KeyError, ValueError, TypeError):
                continue  # short/blank/malformed row (e.g. a truncated tail line)
            if lam <= 0.0:  # no wavelength (unknown signal) -> cycle columns are 0, skip
                continue
            key = (row["cons"], row["signal"])
            frac[key].append(wrap_half(fc))
            # T2a formal DD σ (cycles) — optional column; −1/absent = unavailable.
            try:
                ds = float(row["dd_sigma_cyc"])
                if ds > 0.0:
                    sigma[key].append(ds)
            except (KeyError, ValueError, TypeError):
                pass
            skey = (row.get("sat_i", ""), row.get("ref_sat", ""),
                    row.get("sta_a", ""), row.get("sta_b", ""))
            series[key][skey].append(dc)
    return frac, sigma, series


def robust_std(vals):
    if not vals:
        return 0.0
    s = sorted(abs(v) for v in vals)
    mad = s[len(s) // 2]
    return 1.4826 * mad


def circular_R(vals):
    """Circular resultant length of the fractional phases mapped to angles 2*pi*frac.

    R=0 for a uniform (no-integer-information) distribution; R->1 as the DDs pile
    onto integers.  A scale-free companion to mean|frac| / P(<0.1)."""
    if not vals:
        return 0.0
    c = sum(math.cos(2.0 * math.pi * v) for v in vals)
    s = sum(math.sin(2.0 * math.pi * v) for v in vals)
    return math.hypot(c, s) / len(vals)


def series_stability(smap):
    """Median (and 90th-pct) over series of the per-series std of dd_cyc — how much a
    fixed (sat,pair,signal) DD wanders in time.  A drifting float (root-cause signature)
    shows a large per-series std even when the pooled histogram looks tight."""
    stds = []
    for vals in smap.values():
        if len(vals) < 2:
            continue
        m = sum(vals) / len(vals)
        var = sum((v - m) ** 2 for v in vals) / (len(vals) - 1)
        stds.append(math.sqrt(var))
    if not stds:
        return (0.0, 0.0, 0)
    stds.sort()
    med = stds[len(stds) // 2]
    p90 = stds[min(len(stds) - 1, int(0.9 * len(stds)))]
    return (med, p90, len(stds))


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

    frac, sigma, series = load(path)
    keys = sorted(frac)
    if args.signal:
        keys = [k for k in keys if k[1] == args.signal]
    if not keys:
        sys.exit("no (const,signal) samples with a known wavelength to plot")

    print(f"Network DD fractional-cycle histogram: {path}")
    print("UNIFORM BENCHMARK (no integer information): mean|frac|=0.250  P(<0.1)=0.200  R=0.00")
    print("  AR-grade biases pull mean|frac| << 0.25, P(<0.1) >> 0.20, R >> 0, and calib ~ 1.")
    # Pooled (all (const,signal)) headline, so a single number is comparable to the audit
    # benchmark (mean|frac|=0.247, P(<0.1)=0.209 on runs/net305_10ep_audit).
    pooled = [v for k in keys for v in frac[k]]
    if pooled:
        pmean = sum(abs(v) for v in pooled) / len(pooled)
        pp01 = sum(1 for v in pooled if abs(v) < 0.1) / len(pooled)
        print(f"POOLED  n={len(pooled)}  mean|frac|={pmean:.3f}  P(<0.1)={pp01:.3f}  "
              f"R={circular_R(pooled):.3f}")
    for cst, sig in keys:
        vals = frac[(cst, sig)]
        near = sum(1 for v in vals if abs(v) < 0.1) / len(vals)
        mean_abs = sum(abs(v) for v in vals) / len(vals)
        emp = robust_std(vals)                       # robust std of the FRACTIONS (unif ~0.29)
        smed, sp90, ns = series_stability(series.get((cst, sig), {}))
        # σ calibration (the doc's χ² intent): compare the per-series TEMPORAL scatter of
        # dd_cyc (how much a fixed (sat,pair,signal) DD wanders epoch-to-epoch) to the formal
        # DD σ.  calib = temporal_scatter / formal_σ: ~1 = honest; >>1 = formal σ overconfident
        # (the σ-incest signature); <<1 = conservative (typical during warmup).
        svals = sigma.get((cst, sig), [])
        med_sig = (sorted(svals)[len(svals) // 2] if svals else 0.0)
        calib = (smed / med_sig) if med_sig > 0.0 else float("nan")
        print(f"\n[{SYS_NAME.get(cst, cst)} {sig}]  n={len(vals)}")
        print(f"  mean|frac|={mean_abs:.3f} (unif 0.25)   P(<0.1)={near:.3f} (unif 0.20)"
              f"   R={circular_R(vals):.3f} (unif 0.00)")
        print(f"  robust-sigma(frac)={emp:.3f} cyc   median formal dd_sigma_cyc="
              f"{med_sig:.3f} cyc" + ("" if svals else "  [no dd_sigma_cyc column]"))
        print(f"  per-series std(dd_cyc): median={smed:.3f} p90={sp90:.3f} cyc "
              f"over {ns} series   calib(scatter/formal)={calib:.2f}")
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
