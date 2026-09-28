#!/usr/bin/env python3
"""Score the engine's MW widelane integers against the estimator-free MW census.

Why
---
``AmbiguityResolver::resolveWlConstrained`` anchors the narrow-lane search on a
hard widelane integer taken from the MW cycle-slip detector:

    dd_wl = SD_WL(sat_i) - SD_WL(sat_ref)          (AmbiguityResolver.cpp ~:1183)
    y     = N1 - f2/(f1+f2) * dd_wl                (the N1-aligned search target)

For GPS L1/L2, ``f2/(f1+f2) = 0.4379``.  A 1-cycle error in ``dd_wl`` therefore
shifts the N1 target by 0.438 cycles, almost exactly half a cycle, the
maximally ambiguous displacement.  A wrong widelane deterministically produces a
wrong narrow-lane.  This script measures how often that happens.

Method (bias-immune)
---------------------
The census MW comes straight from raw RINEX and still carries the receiver MW
bias (common to all satellites, cancels in the between-satellite SD) and the
satellite MW bias (per-satellite, does not cancel in an SD).  The engine's MW is
OSB-corrected, so the two cannot be compared in absolute terms.

They can be compared in time.  For a continuous arc the true widelane ambiguity
is a constant, so for each satellite pair

    resid(t) = SD_MW_census(t) - dd_wl_engine(t)

must be constant (it equals the fixed satellite-MW-bias difference plus MW
noise).  Any integer step in resid is a disagreement: either the engine changed
its integer when the data did not, or the data stepped and the engine did not.

Reported per pair, against the pair's own median resid:
  * n_bad   -- epochs with |resid - median| >= 0.5 cyc (an integer disagreement)
  * scatter -- robust sigma of resid (should be MW noise, ~0.1-0.3 cyc)
  * n_flip  -- times the engine's dd_wl changed value within the arc

Usage
-----
    python scripts/compare_wl_vs_census.py <run_dir> [obs_rnx] [tow0]

``tow0`` (default 259200 = Wednesday 00:00, i.e. 2024 DOY-129) converts the
engine's GPS TOW to the census's seconds-of-day.
"""
import importlib.util
import math
import os
import re
import statistics as st
import sys
from collections import defaultdict

HERE = os.path.dirname(os.path.abspath(__file__))


def _load_census():
    """Import the census module without triggering its __main__ scan."""
    path = os.path.join(HERE, "mw_census_allpairs.py")
    spec = importlib.util.spec_from_file_location("mw_census_allpairs", path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def read_wldump(run_dir):
    """-> {(sat_i, sat_ref, sig_hi, sig_lo): {sod: dd_wl}}, tow0-relative later."""
    out = defaultdict(dict)
    pat = re.compile(
        r"WLDUMP tow=([\d.]+) sat_i=(\S+) sat_ref=(\S+) sig_hi=(\S+) sig_lo=(\S+) "
        r"wl_i=(-?\d+) wl_ref=(-?\d+) dd_wl=(-?\d+)")
    src = os.path.join(run_dir, "run_stdout.txt")
    with open(src, encoding="utf-8", errors="ignore") as fh:
        for ln in fh:
            m = pat.search(ln)
            if not m:
                continue
            tow, si, sr, hi, lo, wi, wr, dd = m.groups()
            out[(si, sr, hi, lo)][float(tow)] = int(dd)
    return out


def main():
    run_dir = sys.argv[1]
    obs = (sys.argv[2] if len(sys.argv) > 2 else
           "C:/GnssProcessor/data/2024-129/obs/algo00can/"
           "ALGO00CAN_R_20241290000_01D_30S_MO.rnx")
    tow0 = float(sys.argv[3]) if len(sys.argv) > 3 else 259200.0

    eng = read_wldump(run_dir)
    if not eng:
        print("no WLDUMP lines found")
        return
    sig_hi, sig_lo = next(iter(eng))[2:4]
    pairkey = ("G", f"{sig_hi}/{sig_lo}")
    print(f"engine pairs: {len(eng)}   signal pair: {pairkey[1]}")

    census = _load_census()
    ser = census.parse_station_tow(obs)          # {(sat, pairkey): [(sod, mw_cyc)]}
    mw = defaultdict(dict)
    for (sat, pk), vals in ser.items():
        if pk != pairkey:
            continue
        for sod, v in vals:
            mw[sat][round(sod)] = v
    print(f"census satellites on {pairkey[1]}: {len(mw)}")
    if not mw:
        print("NO CENSUS DATA for that signal pair — check the RINEX observables.")
        return

    tot = bad = 0
    per_pair = []
    for (si, sr, _hi, _lo), epochs in eng.items():
        resid, dds = [], []
        for tow, dd in sorted(epochs.items()):
            sod = round(tow - tow0)
            a, b = mw.get(si, {}).get(sod), mw.get(sr, {}).get(sod)
            if a is None or b is None:
                continue
            resid.append((a - b) - dd)
            dds.append(dd)
        if len(resid) < 10:
            continue
        med = st.median(resid)
        nb = sum(1 for r in resid if abs(r - med) >= 0.5)
        # robust sigma via MAD
        mad = st.median([abs(r - med) for r in resid]) * 1.4826
        flips = sum(1 for u, v in zip(dds, dds[1:]) if u != v)
        per_pair.append((f"{si}-{sr}", len(resid), nb, 100.0 * nb / len(resid), mad, flips))
        tot += len(resid)
        bad += nb

    per_pair.sort(key=lambda r: -r[3])
    print(f"\n{'pair':<10}{'n':>6}{'n_bad':>7}{'bad%':>8}{'MAD sig':>9}{'flips':>7}")
    for r in per_pair[:15]:
        print(f"{r[0]:<10}{r[1]:>6}{r[2]:>7}{r[3]:>8.1f}{r[4]:>9.3f}{r[5]:>7}")
    if len(per_pair) > 15:
        print(f"... {len(per_pair)-15} more pairs")

    if tot:
        mads = [r[4] for r in per_pair]
        print(f"\nTOTAL matched epoch-pairs : {tot}")
        print(f"integer DISAGREEMENTS     : {bad}  ({100.0*bad/tot:.1f}%)")
        print(f"median per-pair MAD sigma : {st.median(mads):.3f} cyc  "
              f"(MW noise expectation ~0.1-0.3)")
        sig = st.median(mads)
        # A 0.5-cyc threshold against sigma this size is only about 2 sigma, so
        # pure MW noise already crosses it this often; quote the null so the raw
        # rate is not over-read.
        z = 0.5 / sig if sig > 0 else float("inf")
        null = 100.0 * math.erfc(z / math.sqrt(2.0))
        print(f"  ...but the NOISE-ONLY null at that sigma is {null:.1f}% "
              f"(0.5 cyc = {z:.2f} sigma) — excess = {100.0*bad/tot - null:+.1f} pts")
        print(f"pairs with any disagreement: "
              f"{sum(1 for r in per_pair if r[2] > 0)}/{len(per_pair)}")
        print(f"total engine WL flips      : {sum(r[5] for r in per_pair)}")

    # ---- zero-difference arc medians: diagnostic only, not a validity test ----
    # The absolute ZD widelane level is not observable, and the engine's
    # per-satellite wl_i is arc-relative (the MW detector latches a level at arc
    # start), so a large per-satellite offset here is expected and is not
    # evidence of a wrong integer.  Only the between-satellite SD comparison
    # above is a validity test: taking the per-pair median absorbs any
    # per-satellite constant, leaving noise.  This block is kept because the
    # spread is a useful sanity read on whether the engine and the census are
    # on the same scale.
    zd = defaultdict(list)
    pat = re.compile(r"WLDUMP tow=([\d.]+) sat_i=(\S+) sat_ref=(\S+) \S+ \S+ "
                     r"wl_i=(-?\d+) wl_ref=(-?\d+)")
    with open(os.path.join(run_dir, "run_stdout.txt"), encoding="utf-8",
              errors="ignore") as fh:
        for ln in fh:
            m = pat.search(ln)
            if not m:
                continue
            tow, si, sr, wi, wr = m.groups()
            sod = round(float(tow) - tow0)
            for sat, wl in ((si, int(wi)), (sr, int(wr))):
                v = mw.get(sat, {}).get(sod)
                if v is not None:
                    zd[sat].append(v - wl)
    if zd:
        meds = {s: st.median(v) for s, v in zd.items() if len(v) >= 20}
        centre = st.median(list(meds.values()))
        offs = [meds[s] - centre for s in meds]
        spread = st.median([abs(d) for d in offs]) * 1.4826
        print(f"\n--- ZD arc medians ({len(meds)} sats) — DIAGNOSTIC, NOT a verdict ---")
        print(f"common level {centre:+.3f} cyc, robust spread {spread:.3f} cyc")
        print("The ZD widelane level is UNOBSERVABLE and the engine's wl_i is\n"
              "arc-relative, so this spread is expected. It does NOT indicate\n"
              "wrong integers. Read the SD table above for the validity verdict.")


if __name__ == "__main__":
    main()
