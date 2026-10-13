#!/usr/bin/env python3
"""Card IRD section 12 (IRD-0, gate 2 reading), promoted at IRD-0r (section 13.5, item 7): the
per-station circular concentration of the chord wide-lane fractions at one epoch.

For every station with at least 4 chords at the epoch, R = |mean(exp(2 pi i wl))| over the
station's arcs (scripts/ird_chord_census.py owns the chord selection and the wide lane).  A
receiver whose fractional phase offset is shared by its chords reads R near 1; independent
fractions read the null, the p50 of R over uniform draws with the same arc counts (500 draws).
A one-off diagnostic, not a graded instrument.

Usage:
    python scripts/ird0_station_R.py <run_dir> <tow>
"""
import argparse
import os
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ird_chord_census as CC  # noqa: E402  (chord selection and the wide lane)

MIN_ARCS = 4


def station_R(run, tow, seed=5, draws=500):
    """(stations, arcs p50, R p50, null mean, null p2.5, null p97.5, share R > 0.8)."""
    df, _ = CC.read_chords(run, tow - 1, tow + 1)
    wl = CC.wide_lanes(df)
    Rs, ns = [], []
    for _st, g in wl.groupby("station"):
        if len(g) < MIN_ARCS:
            continue
        Rs.append(abs(np.mean(np.exp(2j * np.pi * g["wl"].values))))
        ns.append(len(g))
    if not Rs:
        raise SystemExit("no station with %d chords at TOW %.0f in %s" % (MIN_ARCS, tow, run))
    rng = np.random.default_rng(seed)
    nul = [np.median([abs(np.mean(np.exp(2j * np.pi * rng.uniform(size=n)))) for n in ns])
           for _ in range(draws)]
    lo, hi = np.percentile(nul, [2.5, 97.5])
    return (len(Rs), float(np.median(ns)), float(np.median(Rs)), float(np.mean(nul)), float(lo),
            float(hi), float(np.mean(np.array(Rs) > 0.8)))


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("run")
    ap.add_argument("tow", type=float)
    a = ap.parse_args()
    n, arcs, r, nm, lo, hi, share = station_R(a.run, a.tow)
    print("%s TOW %.0f: %d stations, arcs per station p50 %d; per-station R p50 %.3f (null %.3f "
          "[%.3f, %.3f]); share R > 0.8: %.2f"
          % (os.path.basename(a.run.rstrip("/\\")), a.tow, n, arcs, r, nm, lo, hi, share))
    return 0


if __name__ == "__main__":
    sys.exit(main())
