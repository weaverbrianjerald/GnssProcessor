#!/usr/bin/env python3
"""Card ARL: would a Melbourne-Wubbena wide lane fix the pairs the producer's search leaves free?

For each GPS L1/L2 line of sight of a simulator fixture, the Melbourne-Wubbena combination
    MW = (f1 L1 - f2 L2)/(f1 - f2) - (f1 P1 + f2 P2)/(f1 + f2)      (metres; L in metres)
is divided by the wide-lane wavelength c/(f1 - f2) and averaged causally over the producer's
arc (start from logs/network_slip_raw.csv, a gap over 300 s starts a new arc), from the arc's
first epoch to the epoch graded.  Geometry, clocks, troposphere, ionosphere and wind-up cancel;
the satellite and receiver code and phase biases cancel in the double difference.

At every pair-epoch of the run's producer-AR trace (decorrelated_partial pair columns) the
double-differenced MW wide lane is compared with truth (the simulator's DD L1 minus L2 integer;
the arcs' wind-up branch is band-common and cancels), next to the producer float's own wide
lane, per pair class (simnet_producer_ar_pairs.py's: singles fixed, WL fixed only, WL not
fixed).  The MW formal sigma is the per-epoch DD noise over the square root of the shorter of
the four arcs' epoch counts, the per-epoch noise taken from the data (the scatter of the
undifferenced MW about its arc mean).

Usage: python scripts/simnet_mw_widelane.py <fixture_dir> --run <run_dir> [--settle-h 1]
"""
import argparse
import bisect
import collections
import csv
import glob
import math
import os
import statistics as st
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import simnet_producer_ar_grade as g  # noqa: E402
from simnet_amb_integer import load_truth, n_at  # noqa: E402

C = 299792458.0
F1, F2 = 1575.42e6, 1227.60e6
L1W, L2W = C / F1, C / F2
LWL = C / (F1 - F2)


def read_rinex_mw(path):
    """{(sat): [(tow, mw_cycles)]} for GPS records with C1C, L1C, C2W, L2W."""
    out = collections.defaultdict(list)
    types = None
    with open(path) as f:
        for line in f:
            if "SYS / # / OBS TYPES" in line and line.startswith("G"):
                types = line[7:60].split()
            if "END OF HEADER" in line:
                break
        ix = {t: i for i, t in enumerate(types)}
        tow = None
        for line in f:
            if line.startswith(">"):
                p = line[1:].split()
                y, mo, d, hh, mi, ss = int(p[0]), int(p[1]), int(p[2]), int(p[3]), int(p[4]), float(p[5])
                import datetime
                dt = datetime.datetime(y, mo, d, hh, mi) - datetime.datetime(1980, 1, 6)
                tow = (dt.days % 7) * 86400.0 + dt.seconds + ss
                continue
            if not line.startswith("G") or tow is None:
                continue
            sat = line[:3]

            def val(t):
                k = ix[t]
                s = line[3 + 16 * k: 3 + 16 * k + 14].strip()
                return float(s) if s else None
            P1, Lc1, P2, Lc2 = val("C1C"), val("L1C"), val("C2W"), val("L2W")
            if None in (P1, Lc1, P2, Lc2):
                continue
            mw_m = (F1 * Lc1 * L1W - F2 * Lc2 * L2W) / (F1 - F2) - (F1 * P1 + F2 * P2) / (F1 + F2)
            out[sat].append((tow, mw_m / LWL))
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("fixture_dir")
    ap.add_argument("--run", required=True)
    ap.add_argument("--settle-h", type=float, default=1.0)
    a = ap.parse_args()

    stations = sorted(d for d in os.listdir(a.fixture_dir)
                      if os.path.isfile(os.path.join(a.fixture_dir, d, "truth_ambiguities.csv")))
    n_int, slips = load_truth(a.fixture_dir, stations)
    arcs, _ = g.load_windup_branches(a.fixture_dir, a.run, stations)   # arc starts (tows)

    # Per (sta, sat): cumulative sums over the producer's arcs, for causal arc means.
    cum = {}
    resid = []
    for sta in stations:
        rnx = glob.glob(os.path.join(a.fixture_dir, sta, "*.rnx"))
        if not rnx:
            continue
        for sat, series in read_rinex_mw(rnx[0]).items():
            starts = [t0 for t0, _t1, _k in arcs.get((sta, sat), ())]
            if not starts:
                continue
            tows, means, counts = [], [], []
            s = n = 0
            arc_id = None
            vals = []
            for t, v in series:
                i = bisect.bisect_right(starts, t) - 1
                if i < 0:
                    continue
                if i != arc_id:
                    if len(vals) > 5:
                        m = st.mean(vals)
                        resid.extend(x - m for x in vals)
                    arc_id, s, n, vals = i, 0.0, 0, []
                s += v
                n += 1
                vals.append(v)
                tows.append(t)
                means.append(s / n)
                counts.append(n)
            if len(vals) > 5:
                m = st.mean(vals)
                resid.extend(x - m for x in vals)
            cum[(sta, sat)] = (tows, means, counts)
    sig_ep = st.pstdev(resid) if resid else float("nan")

    def mw_at(sta, sat, t):
        c = cum.get((sta, sat))
        if not c:
            return None
        i = bisect.bisect_right(c[0], t + 1e-6) - 1
        if i < 0 or abs(c[0][i] - t) > 1e-3:
            return None
        return c[1][i], c[2][i]

    tow0 = None
    pairs = collections.defaultdict(dict)
    with open(os.path.join(a.run, "logs", "network_producer_ar.csv"), newline="") as f:
        rd = csv.DictReader(f)
        if "wl_cond_sigma_cyc" not in rd.fieldnames:
            raise SystemExit("trace has no pair columns: arm decorrelated_partial")
        for r in rd:
            t = float(r["tow"])
            tow0 = t if tow0 is None else tow0
            if t - tow0 < a.settle_h * 3600.0:
                continue
            pairs[(t, (r["sat_j"], r["ref_sat"], r["sta_r"], r["ref_sta"]))][r["signal"]] = r

    res = collections.defaultdict(lambda: {"mw": [], "flt": [], "mw_sig": [], "mw_ok": 0})
    for (t, (sj, sp, sr, sf)), rows in pairs.items():
        if set(rows) != {"L1C", "L2W"}:
            continue
        r1, r2 = rows["L1C"], rows["L2W"]
        if r1["kept"] == "1" and r2["kept"] == "1":
            k = "singles fixed"
        elif r1["wl_cond_sigma_cyc"] == "":
            continue
        elif float(r1["wl_cond_sigma_cyc"]) <= 0.05:
            k = "WL fixed only"
        else:
            k = "WL not fixed"
        q4 = [mw_at(sr, sj, t), mw_at(sr, sp, t), mw_at(sf, sj, t), mw_at(sf, sp, t)]
        if None in q4:
            continue
        mw_dd = (q4[0][0] - q4[1][0]) - (q4[2][0] - q4[3][0])
        n_min = min(x[1] for x in q4)

        def T(sig):
            v = [n_at(n_int, slips, sr, sj, sig, t), n_at(n_int, slips, sr, sp, sig, t),
                 n_at(n_int, slips, sf, sj, sig, t), n_at(n_int, slips, sf, sp, sig, t)]
            return None if None in v else (v[0] - v[1]) - (v[2] - v[3])
        t1, t2 = T("L1C"), T("L2W")
        if t1 is None or t2 is None:
            continue
        wl_true = t1 - t2
        R = res[k]
        e_mw = mw_dd - wl_true
        R["mw"].append(abs(e_mw))
        R["mw_ok"] += int(round(mw_dd) == wl_true)
        # The float's wide lane is graded on the producer's branch; the branch is band-common.
        R["flt"].append(abs(float(r1["float_cyc"]) - float(r2["float_cyc"]) - wl_true))
        R["mw_sig"].append(2.0 * sig_ep / math.sqrt(n_min))

    def q(v, p):
        s = sorted(v)
        return s[min(len(s) - 1, int(p * len(s)))] if s else float("nan")

    print("MW WIDE LANE vs the producer float's wide lane  run=%s  settled >= h%g" % (a.run, a.settle_h))
    print("  undifferenced MW scatter about its arc mean: %.3f WL cycle per epoch" % sig_ep)
    for k in ("singles fixed", "WL fixed only", "WL not fixed"):
        R = res[k]
        n = len(R["mw"])
        if not n:
            continue
        print("  %-14s n %5d   MW |e| p50 %.3f p90 %.3f, rounds to truth %5.1f %%, formal sigma p50 %.3f"
              "   |  float WL |e| p50 %.3f p90 %.3f"
              % (k, n, q(R["mw"], .5), q(R["mw"], .9), 100.0 * R["mw_ok"] / n, q(R["mw_sig"], .5),
                 q(R["flt"], .5), q(R["flt"], .9)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
