#!/usr/bin/env python3
"""RTB-7(d) RS card: warm-resume divergence metric (between-satellite SD-of-SD).

Grades how far a resumed producer's emitted products drift from an inline
(never-interrupted) reference at the same epochs.

Metric (datum-invariant by construction).  For one epoch and one product kind,
each run's per-satellite values ``p_i`` are reduced to between-satellite single
differences ``d_i = p_i - p_ref`` against a common reference satellite.  The
score is ``RMS_i( d_i^ARM - d_i^REF )``.  Differencing between satellites cancels
the receiver-clock / per-constellation sum-to-zero datum common mode, so the two
runs' independent datum realizations do not contaminate the comparison
(Psychas single-realization mandate: grade datum-invariant quantities only).

CLK and CB are graded.  PB is deliberately not graded: the emitted phase bias is
integer-ambiguous, so its between-satellite SD is dominated by whole-cycle branch
offsets (tens of metres) and says nothing about drift.  Grade the phase branch
with ``diff_carry_blobs.py`` (the carry blob is integer- and datum-invariant).

Read levels and slopes together: a resumed arm pays a one-time restore transient
whose decay constants are the adaptive windows (D5 calibrate, iono deweight). A
higher level inside that window with a shallower slope is a transient, not
divergence, which is why this script fits and reports the post-transient slope.

Usage:
    python scripts/warm_resume_sd_metric.py --ref A.csv \
        --arm cold=B2off.csv --arm warm=B2on.csv \
        [--kind CLK --kind CB] [--slope-from 20 --slope-to 60] [--plot out.png]

Inputs are ``network_solution.csv`` files (week,tow,sat,kind,signal,value_m,...).
"""
import argparse, csv, math, os
from collections import defaultdict


def load(path, kinds):
    """kind -> tow -> {sat: value_m} from a network_solution.csv."""
    out = {k: defaultdict(dict) for k in kinds}
    with open(path, newline="") as f:
        for r in csv.DictReader(f):
            k = r.get("kind")
            if k not in out:
                continue
            try:
                out[k][r["tow"]][r["sat"] + "|" + r.get("signal", "")] = float(r["value_m"])
            except (ValueError, KeyError):
                continue
    return out


def sd_of_sd_mm(a_ep, b_ep):
    """RMS over satellites of the between-sat SD difference, in mm (None if <3 common)."""
    common = sorted(set(a_ep) & set(b_ep))
    if len(common) < 3:
        return None
    ref = common[0]
    ds = [(a_ep[s] - a_ep[ref]) - (b_ep[s] - b_ep[ref]) for s in common[1:]]
    return math.sqrt(sum(d * d for d in ds) / len(ds)) * 1000.0


def fit_slope(eps, vals, lo, hi):
    """Least-squares slope (mm/epoch) over [lo, hi]; None if <2 points."""
    pts = [(e, v) for e, v in zip(eps, vals) if v is not None and lo <= e <= hi]
    if len(pts) < 2:
        return None
    n = len(pts)
    mx = sum(p[0] for p in pts) / n
    my = sum(p[1] for p in pts) / n
    den = sum((p[0] - mx) ** 2 for p in pts)
    if den == 0:
        return None
    return sum((p[0] - mx) * (p[1] - my) for p in pts) / den


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--ref", required=True, help="inline reference network_solution.csv")
    ap.add_argument("--arm", action="append", default=[], metavar="LABEL=PATH")
    ap.add_argument("--kind", action="append", default=None, help="CLK and/or CB (default both)")
    ap.add_argument("--cadence", type=float, default=30.0)
    ap.add_argument("--slope-from", type=float, default=20.0)
    ap.add_argument("--slope-to", type=float, default=60.0)
    ap.add_argument("--plot", default=None)
    a = ap.parse_args()
    kinds = a.kind or ["CLK", "CB"]
    arms = [x.split("=", 1) for x in a.arm]

    ref = load(a.ref, kinds)
    loaded = [(lbl, load(p, kinds)) for lbl, p in arms]
    # epoch 0 = earliest reference tow
    anchor = min(float(t) for k in kinds for t in ref[k]) if any(ref[k] for k in kinds) else 0.0

    curves = {}
    for kind in kinds:
        print(f"\n=== {kind}: between-sat SD-of-SD vs reference [mm] ===")
        hdr = f"  {'epoch':>6}" + "".join(f"{lbl:>12}" for lbl, _ in loaded)
        print(hdr)
        tows = sorted(ref[kind], key=float)
        eps, series = [], {lbl: [] for lbl, _ in loaded}
        for t in tows:
            ep = int(round((float(t) - anchor) / a.cadence))
            row = []
            for lbl, arm in loaded:
                v = sd_of_sd_mm(ref[kind][t], arm[kind][t]) if t in arm[kind] else None
                series[lbl].append(v)
                row.append(v)
            if all(v is None for v in row):
                continue
            eps.append(ep)
            print(f"  {ep:>6}" + "".join(f"{(f'{v:.1f}' if v is not None else '-'):>12}" for v in row))
        # trim series to the epochs actually printed
        keep = [i for i, t in enumerate(tows)
                if int(round((float(t) - anchor) / a.cadence)) in eps]
        for lbl in series:
            series[lbl] = [series[lbl][i] for i in keep]
        curves[kind] = (eps, series)
        print(f"  -- fitted slope over ep{a.slope_from:.0f}..{a.slope_to:.0f} (mm/epoch) --")
        for lbl, _ in loaded:
            s = fit_slope(eps, series[lbl], a.slope_from, a.slope_to)
            print(f"     {lbl:>12}: {('%.2f' % s) if s is not None else 'n/a'}")

    if a.plot:
        try:
            import matplotlib
            matplotlib.use("Agg")
            import matplotlib.pyplot as plt
            fig, axes = plt.subplots(len(kinds), 1, figsize=(9, 4 * len(kinds)), squeeze=False)
            for ax, kind in zip(axes[:, 0], kinds):
                eps, series = curves[kind]
                for lbl in series:
                    ax.plot(eps, [v if v is not None else float("nan") for v in series[lbl]],
                            marker="o", ms=3, label=lbl)
                ax.set_title(f"{kind}: between-sat SD-of-SD vs inline")
                ax.set_xlabel("epoch"); ax.set_ylabel("RMS [mm]")
                ax.grid(alpha=.3); ax.legend()
            fig.tight_layout(); fig.savefig(a.plot, dpi=120)
            print(f"\nplot -> {a.plot}")
        except ImportError:
            print("\n(matplotlib unavailable — table only)")


if __name__ == "__main__":
    main()
