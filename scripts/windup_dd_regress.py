#!/usr/bin/env python3
"""THE WIND-UP DISCRIMINATOR for the band-common DD error (SPEC_synthetic §13.8.2 finding 4, owed since 2026-08).

Finding 4: the per-band DDs of the posture-C arm are ~5.0 cyc med|e| on L1C, L2W AND L5Q alike, while
WL(1,2) is 0.386 and EWL(2,5) is 0.081 — so the dominant term is EQUAL IN CYCLES across bands and cancels
exactly in every between-band difference.  The registered leading candidate is residual carrier phase
wind-up, which is band-common in cycles by construction.  This script is the owed regression.

WHY THE CANDIDATE IS PLAUSIBLE AT ALL — and the cheaper test that comes first.  A range-domain error
(orbit, clock, troposphere, antenna offset) is common in METRES, so in cycles it scales as 1/lambda and L5
would read 25 % SMALLER than L1.  Measured: 4.956 / 5.034 / 5.076, a 2.4 % spread with L5 slightly LARGER.
So the term is a CYCLE-domain quantity, and wind-up is the only standard one.  That is what makes the
regression worth running rather than a formality.

THE REGRESSOR IS TRUTH, NOT A MODEL.  The simulator injects wind-up per (station, satellite, epoch) via
EphemerisProvider::phaseWindupMetres(sat, r_rx, ...) and writes the accumulated band-common cycle count to
<fixture>/<station>/truth_windup.csv.  So the DD regressor is exact:

    wu_dd = [wu(a,sat) - wu(a,ref)] - [wu(b,sat) - wu(b,ref)]        (cycles)

and the graded quantity is the same e the integer grader uses:

    e = dd_cyc - DD_true,    DD_true = [N(a,sat) - N(a,ref)] - [N(b,sat) - N(b,ref)]

PRE-REGISTERED PREDICTIONS (write them down before reading the output):
  H1 wind-up IS the band-common term  =>  |slope| ~ 1, R^2 large, the SAME slope on all three bands, and
     med|e - wu_dd| collapses well below med|e|.
  H0 it is not                        =>  |slope| small and/or R^2 ~ 0, and sd(e - wu_dd) > sd(e)
     (variances add under independence).  ⚠ State that in VARIANCE, not in med| |: for a broad e the
     median absolute value can fall slightly when a small independent regressor is subtracted, so a
     "med|e-w| went down" reading is NOT evidence of wind-up content.
  ⚠ MAGNITUDE GATE, independent of any fit: the injected wind-up accumulates to at most ~1 cycle, so
     |wu_dd| is bounded by ~2 cyc.  If med|e| is ~5 cyc with a ~20 cyc p90, wind-up cannot be the whole
     term no matter what the correlation says.  Report the bound BESIDE the fit and let it veto.

Usage:  python scripts/windup_dd_regress.py <fixture_dir> <run_dir> [--stride 8] [--signals L1C,L2W,L5Q]
        python scripts/windup_dd_regress.py --selftest
"""
import argparse
import collections
import csv
import io
import math
import os
import statistics as st
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from simnet_amb_integer import load_truth, n_at  # noqa: E402


def med(x):
    return st.median(abs(v) for v in x) if x else float("nan")


def ols(x, y):
    """(slope, intercept, r, R^2) for y = a*x + b.  Returns nans on a degenerate x."""
    n = len(x)
    if n < 3:
        return (float("nan"),) * 4
    mx, my = sum(x) / n, sum(y) / n
    sxx = sum((v - mx) ** 2 for v in x)
    syy = sum((v - my) ** 2 for v in y)
    sxy = sum((x[i] - mx) * (y[i] - my) for i in range(n))
    if sxx <= 0 or syy <= 0:
        return (float("nan"),) * 4
    a = sxy / sxx
    b = my - a * mx
    r = sxy / math.sqrt(sxx * syy)
    return a, b, r, r * r


def load_windup(fixture, tows):
    """{(sta, sat, tow): accumulated cycles} restricted to the graded tows."""
    wu = {}
    stations = sorted(d for d in os.listdir(fixture)
                      if os.path.isfile(os.path.join(fixture, d, "truth_windup.csv")))
    for sta in stations:
        with io.open(os.path.join(fixture, sta, "truth_windup.csv"), encoding="utf-8") as fh:
            fh.readline()
            for line in fh:
                p = line.rstrip("\n").split(",")
                if len(p) < 4 or p[1] not in tows:
                    continue
                wu[(sta, p[2], p[1])] = float(p[3])
    return wu, stations


def regress(fixture, run, stride=8, signals=("L1C", "L2W", "L5Q"), out=sys.stdout):
    path = os.path.join(run, "logs", "network_dd_fractional.csv")

    # pass 1 - which tows will be graded (so the wind-up load stays bounded)
    tows = []
    with io.open(path, newline="", encoding="utf-8", errors="replace") as f:
        rd = csv.reader(f)
        hdr = next(rd)
        it = hdr.index("tow")
        last = None
        for row in rd:
            if row[it] != last:
                last = row[it]
                tows.append(last)
    graded = set(tows[::stride])

    wu, wu_stations = load_windup(fixture, graded)
    stations = sorted(d for d in os.listdir(fixture)
                      if os.path.isfile(os.path.join(fixture, d, "truth_ambiguities.csv")))
    n_int, slips = load_truth(fixture, stations)

    B = collections.defaultdict(lambda: {"e": [], "w": []})
    chord = collections.defaultdict(dict)   # (tow,sat,ref,a,b) -> {signal: e}, the band-common premise
    chord_w = {}                            # same key -> wind-up DD (cycles)
    lam_of = {}                             # signal -> wavelength (m), read from the trace
    n_rows = n_nowu = n_notruth = 0
    with io.open(path, newline="", encoding="utf-8", errors="replace") as f:
        rd = csv.reader(f)
        hdr = next(rd)
        ix = {c: hdr.index(c) for c in ("tow", "signal", "sat_i", "ref_sat", "sta_a", "sta_b",
                                        "dd_cyc", "lambda_m")}
        for row in rd:
            tow = row[ix["tow"]]
            if tow not in graded:
                continue
            sig = row[ix["signal"]]
            if sig not in signals:
                continue
            sat, ref = row[ix["sat_i"]], row[ix["ref_sat"]]
            a, b = row[ix["sta_a"]], row[ix["sta_b"]]
            n_rows += 1
            try:
                w = ((wu[(a, sat, tow)] - wu[(a, ref, tow)])
                     - (wu[(b, sat, tow)] - wu[(b, ref, tow)]))
            except KeyError:
                n_nowu += 1
                continue
            t = float(tow)
            N = [n_at(n_int, slips, s_, sv, sig, t) for s_, sv in ((a, sat), (a, ref), (b, sat), (b, ref))]
            if any(v is None for v in N):
                n_notruth += 1
                continue
            e = float(row[ix["dd_cyc"]]) - ((N[0] - N[1]) - (N[2] - N[3]))
            B[sig]["e"].append(e)
            B[sig]["w"].append(w)
            chord[(tow, sat, ref, a, b)][sig] = e
            chord_w[(tow, sat, ref, a, b)] = w
            try:
                lam_of.setdefault(sig, float(row[ix["lambda_m"]]))
            except ValueError:
                pass

    def P(*a):
        print(*a, file=out)

    P(f"fixture {fixture}\nrun      {run}")
    P(f"stride {stride}: {len(graded)} of {len(tows)} traced epochs; {len(wu_stations)} stations carry "
      f"truth_windup.csv")
    P(f"rows considered {n_rows}, dropped {n_nowu} (no wind-up truth) + {n_notruth} (no ambiguity truth)")
    P("")
    P("PREMISE CHECK — is the error actually BAND-COMMON in THIS arm? (finding 4's claim)")
    P("  If e is common in cycles across bands it cancels in a between-band difference of the SAME chord;")
    P("  if it were common in METRES it would scale as 1/lambda and NOT cancel.")
    sigs = [x for x in signals if any(x in v for v in chord.values())]
    for i in range(len(sigs) - 1):
        s1, s2 = sigs[i], sigs[i + 1]
        d = [v[s1] - v[s2] for v in chord.values() if s1 in v and s2 in v]
        if not d:
            continue
        m1 = med([v[s1] for v in chord.values() if s1 in v])
        P(f"  med|e({s1}) - e({s2})| = {med(d):7.3f} cyc   vs per-band med|e({s1})| = {m1:7.3f} cyc"
          f"   -> {med(d)/m1:.1%} of the per-band level survives the difference")
    P("")
    P("MAGNITUDE GATE — can wind-up be this large at all?")
    P(f"{'band':>5} {'n':>7} {'med|e|':>8} {'p90|e|':>8} | {'med|wu_dd|':>10} {'p90|wu_dd|':>10} "
      f"{'max|wu_dd|':>10} | {'share':>7}")
    for sig in signals:
        d = B.get(sig)
        if not d or not d["e"]:
            continue
        e, w = d["e"], d["w"]
        es, ws = sorted(abs(v) for v in e), sorted(abs(v) for v in w)
        p90e, p90w = es[int(0.9 * len(es))], ws[int(0.9 * len(ws))]
        P(f"{sig:>5} {len(e):7d} {med(e):8.3f} {p90e:8.3f} | {med(w):10.3f} {p90w:10.3f} {max(ws):10.3f} | "
          f"{med(w) / med(e) if med(e) else float('nan'):6.1%}")
    P("")
    P("REGRESSION  e = a*wu_dd + b   (H1: a ~ 1, large R^2, same a on every band)")
    P(f"{'band':>5} {'n':>7} {'slope a':>9} {'inter b':>9} {'r':>7} {'R^2':>7} | "
      f"{'med|e|':>8} {'med|e-w|':>9} | {'sd(e)':>8} {'sd(e-w)':>8}")
    for sig in signals:
        d = B.get(sig)
        if not d or len(d["e"]) < 3:
            continue
        e, w = d["e"], d["w"]
        a, b, r, r2 = ols(w, e)
        res_one = [e[i] - w[i] for i in range(len(e))]
        P(f"{sig:>5} {len(e):7d} {a:9.4f} {b:9.4f} {r:7.4f} {r2:7.4f} | "
          f"{med(e):8.3f} {med(res_one):9.3f} | {st.pstdev(e):8.3f} {st.pstdev(res_one):8.3f}")

    # --- Two-component decomposition, then the SHARPEST form of the discriminator ----------
    # p ~ +0.39 above says the per-band error is a MIXTURE, so testing wind-up against the RAW
    # per-band e dilutes it.  Split each chord's three-band error into
    #     e_i = A + B * (lambda_i / lambda_1)
    # A = the CYCLE-common part (what wind-up would be) and B = the part proportional to lambda
    # (iono enters exactly this way: -dI1 * lambda_i / lambda_1^2 cycles).  Then regress the
    # EXTRACTED A against the wind-up DD — if wind-up hides anywhere, it is in A.
    if len(lam_of) >= 3 and all(x in lam_of for x in signals):
        base = lam_of[signals[0]]
        r = [lam_of[x] / base for x in signals]
        n_ = len(r)
        s11, s1r, srr = float(n_), sum(r), sum(v * v for v in r)
        det = s11 * srr - s1r * s1r
        A_l, B_l, W_l = [], [], []
        for k, v in chord.items():
            if not all(x in v for x in signals) or k not in chord_w:
                continue
            y = [v[x] for x in signals]
            sy, sry = sum(y), sum(r[i] * y[i] for i in range(n_))
            A_l.append((srr * sy - s1r * sry) / det)
            B_l.append((s11 * sry - s1r * sy) / det)
            W_l.append(chord_w[k])
        if len(A_l) > 3:
            P("")
            P("DECOMPOSITION  e_i = A + B*(lambda_i/lambda_1)   over chords with all three bands")
            P(f"  n chords {len(A_l)}   lambda ratios " + " ".join(f"{x}={v:.3f}" for x, v in zip(signals, r)))
            P(f"  A  (CYCLE-common, the wind-up-shaped part) : med|A| {med(A_l):8.3f} cyc   sd {st.pstdev(A_l):8.3f}")
            P(f"  B  (proportional to lambda, iono-shaped)   : med|B| {med(B_l):8.3f} cyc   sd {st.pstdev(B_l):8.3f}")
            aa, bb, rr, r2 = ols(W_l, A_l)
            P("")
            P("  ⭑ THE DECISIVE FIT — the extracted cycle-common part against the wind-up DD:")
            P(f"      A = {aa:.4f}*wu_dd + {bb:.4f}   r {rr:+.4f}   R^2 {r2:.5f}   (H1 predicts slope ~ 1, R^2 large)")
            P(f"      med|A| {med(A_l):.3f} -> med|A - wu_dd| {med([A_l[i] - W_l[i] for i in range(len(A_l))]):.3f}"
              f"   sd {st.pstdev(A_l):.3f} -> {st.pstdev([A_l[i] - W_l[i] for i in range(len(A_l))]):.3f}")
            P(f"      wind-up's share of the cycle-common variance: {100.0 * r2:.3f} %")
    return B


def selftest():
    """Two beds through the real path: one where e IS the wind-up DD (slope 1 must come back), one where
    e is independent of it (slope ~ 0, and subtracting the regressor must make the residual WORSE)."""
    ok = True

    def chk(name, cond, detail=""):
        nonlocal ok
        ok &= bool(cond)
        print(f"  {'PASS' if cond else 'FAIL'} {name} {detail}")

    import random
    stations = [f"S{i:03d}" for i in range(5)]
    sats = ["G01", "G02", "G03", "G04"]
    lam = 0.190293672798
    for mode in ("windup", "independent"):
        td = tempfile.mkdtemp(prefix="wudd_")
        fx, run = os.path.join(td, "fx"), os.path.join(td, "run")
        os.makedirs(os.path.join(run, "logs"))
        rng = random.Random(5 if mode == "windup" else 9)
        wu = {}
        for k in range(6):
            for s in stations:
                for sv in sats:
                    wu[(s, sv, f"{30.0*k:.3f}")] = rng.uniform(-0.9, 0.9)
        for s in stations:
            os.makedirs(os.path.join(fx, s))
            with open(os.path.join(fx, s, "truth_ambiguities.csv"), "w") as f:
                f.write("satellite,signal,N_int,wavelength_m\n")
                for sv in sats:
                    f.write(f"{sv},L1C,{100000},{lam:.12f}\n")
            with open(os.path.join(fx, s, "truth_slips.csv"), "w") as f:
                f.write("week,tow_s,satellite,signal,delta_cycles,flagged,elev_deg\n")
            with open(os.path.join(fx, s, "truth_windup.csv"), "w") as f:
                f.write("week,tow_s,satellite,windup_cycles\n")
                for k in range(6):
                    for sv in sats:
                        f.write(f"2313,{30.0*k:.3f},{sv},{wu[(s, sv, f'{30.0*k:.3f}')]:.6f}\n")
        with open(os.path.join(run, "logs", "network_dd_fractional.csv"), "w") as fd:
            fd.write("week,tow,cons,signal,sat_i,ref_sat,sta_a,sta_b,dd_m,lambda_m,dd_cyc,frac_cyc,"
                     "dd_sigma_cyc,sigma_carry_aware\n")
            for k in range(6):
                tow = f"{30.0*k:.3f}"
                for i, a in enumerate(stations):
                    for b in stations[i + 1:]:
                        for sv in sats[1:]:
                            w = ((wu[(a, sv, tow)] - wu[(a, "G01", tow)])
                                 - (wu[(b, sv, tow)] - wu[(b, "G01", tow)]))
                            e = w if mode == "windup" else rng.uniform(-5, 5)
                            fd.write(f"2313,{tow},G,L1C,{sv},G01,{a},{b},0,{lam:.6f},{e:.6f},0,0.01,0\n")
        buf = io.StringIO()
        B = regress(fx, run, stride=1, signals=("L1C",), out=buf)
        e, w = B["L1C"]["e"], B["L1C"]["w"]
        a, b, r, r2 = ols(w, e)
        res = med([e[i] - w[i] for i in range(len(e))])
        print(f"  [{mode}] n={len(e)} slope={a:.4f} R2={r2:.4f} med|e|={med(e):.3f} med|e-w|={res:.3f}")
        if mode == "windup":
            chk("windup bed: slope ~ 1", abs(a - 1.0) < 1e-6, f"{a:.6f}")
            chk("windup bed: R^2 ~ 1", r2 > 0.999, f"{r2:.4f}")
            chk("windup bed: residual collapses", res < 1e-6, f"{res:.2e}")
        else:
            chk("independent bed: slope ~ 0", abs(a) < 0.35, f"{a:.4f}")
            chk("independent bed: R^2 ~ 0", r2 < 0.05, f"{r2:.4f}")
            v0, v1 = st.pvariance(e), st.pvariance([e[i] - w[i] for i in range(len(e))])
            chk("independent bed: variance ADDS when an uncorrelated regressor is subtracted",
                v1 > v0, f"var {v0:.3f} -> {v1:.3f}")
            chk("independent bed: ⚠ med| | need NOT rise (why the H0 test is stated in variance)",
                True, f"med {med(e):.3f} -> {res:.3f}")
    print("SELFTEST", "PASS" if ok else "FAIL")
    return 0 if ok else 1


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("fixture_dir", nargs="?")
    ap.add_argument("run_dir", nargs="?")
    ap.add_argument("--stride", type=int, default=8)
    ap.add_argument("--signals", default="L1C,L2W,L5Q")
    ap.add_argument("--selftest", action="store_true")
    a = ap.parse_args()
    if a.selftest:
        return selftest()
    if not a.fixture_dir or not a.run_dir:
        ap.error("fixture_dir and run_dir are required (or --selftest)")
    regress(a.fixture_dir, a.run_dir, stride=max(1, a.stride),
            signals=tuple(s.strip() for s in a.signals.split(",") if s.strip()))
    return 0


if __name__ == "__main__":
    sys.exit(main())
