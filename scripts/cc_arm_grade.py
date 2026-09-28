#!/usr/bin/env python3
"""Card CC-arm: the consumer position read the CST §7.1 bars are written against.

    python scripts/cc_arm_grade.py <run_dir> [<run_dir> ...]
           [--bin-s 1800] [--window TOW_MIN TOW_MAX] [--window-bin-s 300]

Three reads of `logs/closed_loop_trace.csv`, all on `pos_err_3d_m`, all p50 unless
labelled otherwise, so a baseline and an arm are binned identically:

  * per-hour p50 / p90 / n, delegated to refound_grade.consumer_table (the same
    statistic rf2d_grade.py prints, not re-implemented here)
  * per-bin p50 (default 30 min), binned from the consumer's own first epoch,
    the same t0 consumer_table uses, so bin k of two arms covers the same elapsed
    time even if the two runs start a sample apart
  * a window read in absolute tow: n / p50 / first / last / max, plus the
    window's own finer bins (default 5 min) and two drift numbers:
      `end-start`  = last bin p50 - first bin p50
      `max-rise`   = max over j > i of (p50_j - p50_i), the largest monotone climb
                     any later bin makes over any earlier one

`max-rise` is the conservative statistic and is what a "no monotone drift > X m" bar
should be graded on: `end-start` alone reads 0 for an excursion that returns.

Read-only: launches nothing, writes nothing but stdout.
"""
from __future__ import annotations

import argparse
import os
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import refound_grade as rg  # noqa: E402


def p50(a):
    return float(np.percentile(a, 50)) if len(a) else float("nan")


def binned(rows, t0, bin_s, lo=None, hi=None):
    """-> [(k, tow_lo, n, p50)] binned from t0, optionally clipped to [lo, hi)."""
    sel = [r for r in rows if (lo is None or r["tow"] >= lo) and (hi is None or r["tow"] < hi)]
    if not sel:
        return []
    kmax = int((max(r["tow"] for r in sel) - t0) // bin_s)
    kmin = int((min(r["tow"] for r in sel) - t0) // bin_s)
    out = []
    for k in range(kmin, kmax + 1):
        a = [r["err3d"] for r in sel if k * bin_s <= r["tow"] - t0 < (k + 1) * bin_s]
        if a:
            out.append((k, t0 + k * bin_s, len(a), p50(a)))
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("runs", nargs="+")
    ap.add_argument("--hours", type=int, default=6)
    ap.add_argument("--bin-s", type=float, default=1800.0)
    ap.add_argument("--window", type=float, nargs=2, action="append", default=None,
                    help="absolute TOW_MIN TOW_MAX; repeatable")
    ap.add_argument("--window-bin-s", type=float, default=300.0)
    args = ap.parse_args()

    for run in args.runs:
        rows = rg.read_consumer(run)
        print(f"\n##### {run}   consumer rows {len(rows)}")
        if not rows:
            continue
        tab, t0 = rg.consumer_table(rows, args.hours)
        print(f"  t0 (consumer first tow) {t0:.1f}")
        print("  per-hour n   : " + " / ".join(f"{t['n']:d}" for t in tab))
        print("  per-hour p50 : " + " / ".join(f"{t['p50']:.2f}" for t in tab))
        print("  per-hour p90 : " + " / ".join(f"{t['p90']:.2f}" for t in tab))
        print("  per-hour max : " + " / ".join(f"{t['max']:.2f}" for t in tab))
        b = binned(rows, t0, args.bin_s)
        print(f"  per-{args.bin_s / 60:.0f}min p50 : " + " ".join(f"{v:.2f}" for _, _, _, v in b))
        print(f"  per-{args.bin_s / 60:.0f}min n   : " + " ".join(f"{n:d}" for _, _, n, _ in b))
        for lo, hi in (args.window or []):
            sel = [r for r in rows if lo <= r["tow"] <= hi]
            print(f"  -- window [{lo:.0f}, {hi:.0f}]  n {len(sel)}")
            if not sel:
                continue
            a = [r["err3d"] for r in sel]
            print(f"     p50 {p50(a):.3f}   first {sel[0]['err3d']:.3f} @ {sel[0]['tow']:.0f}"
                  f"   last {sel[-1]['err3d']:.3f} @ {sel[-1]['tow']:.0f}"
                  f"   max {max(a):.3f} @ {sel[int(np.argmax(a))]['tow']:.0f}")
            wb = binned(rows, lo, args.window_bin_s, lo, hi + 1e-6)
            vals = [v for _, _, _, v in wb]
            print(f"     per-{args.window_bin_s / 60:.0f}min p50 : "
                  + " ".join(f"{v:.2f}" for v in vals))
            if len(vals) > 1:
                rise = max(vals[j] - vals[i] for i in range(len(vals)) for j in range(i + 1, len(vals)))
                print(f"     drift: end-start {vals[-1] - vals[0]:+.3f} m   max-rise {rise:+.3f} m")


if __name__ == "__main__":
    main()
