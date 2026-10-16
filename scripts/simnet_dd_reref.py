#!/usr/bin/env python3
"""Reference-satellite re-referencing of the DD-integer grade (SPEC_amb_iono_split_overcount §6.12, 2026-09-06).

The producer's DD trace (network_dd_fractional.csv, filter/DdFrac.cpp) forms every chord against a
single reference satellite per (constellation, signal): the lexically-lowest satellite present at >= 2 stations,
i.e. G01 / E02 for the whole day.  Every graded chord therefore inherits that satellite's own between-station SD
ambiguity error, and when the reference leaves and returns (eclipse exclusion: G01 twice a day at the GPS
period, E02 at the Galileo period), every chord in the trace is re-referenced to a satellite whose legs were
just reborn at every station.  §6.11's "post-eclipse network-wide DD episode" is this re-referencing artifact.

Per (epoch, constellation, signal, sta_a, sta_b) the trace gives e_i = DD error of sat_i against the ref, and
e_i = s_i - s_ref with s the between-station SD ambiguity error of a satellite, so the ref's share is common to
the group and cancels in any between-satellite difference.  Three grades per hour and constellation:

  as-traced   med|e_i|                        what every grade before 2026-09-06 reported
  common      med|c|,  c = median_i e_i        ~ -s_ref when most legs are good: the reference's share
  ref-free    med|e_i - c| and round0 %        the reference cancelled robustly (groups with >= 3 sats)
  oldest-ref  med|e_i - e_j|, j = the sat with the longest continuous presence at (a,b) on this signal

plus the ref-free error by the graded satellite's own leg age (epochs of continuous presence at the pair),
which is the per-satellite convergence that remains once the instrument's reference is removed.

Reads:  <run>/logs/network_dd_fractional.csv (streamed at --stride), <fixture>/<sta>/truth_ambiguities.csv +
        truth_slips.csv (via simnet_amb_integer.load_truth / n_at).
Usage:  python scripts/simnet_dd_reref.py <fixture_dir> <run_dir> [--stride 8] [--signal L1C]
        python scripts/simnet_dd_reref.py --selftest
"""
import argparse
import collections
import csv
import io
import os
import statistics as st
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from simnet_amb_integer import load_truth, n_at  # noqa: E402

AGE_BINS = ((1, 10, "[1,10)"), (10, 30, "[10,30)"), (30, 100, "[30,100)"), (100, 300, "[100,300)"),
            (300, 1000, "[300,1000)"), (1000, 10 ** 9, "1000+"))


def med(x):
    return st.median(abs(v) for v in x) if x else float("nan")


def r0(x):
    return 100.0 * sum(1 for v in x if round(v) == 0) / len(x) if x else float("nan")


def reref(fixture, run, stride=8, sig="L1C", out=sys.stdout):
    """Stream the trace; print and return ({(hour, cons): {...}}, age-bin dict)."""
    stations = sorted(d for d in os.listdir(fixture)
                      if os.path.isdir(os.path.join(fixture, d))
                      and os.path.isfile(os.path.join(fixture, d, "truth_ambiguities.csv")))
    n_int, slips = load_truth(fixture, stations)
    path = os.path.join(run, "logs", "network_dd_fractional.csv")
    f = io.open(path, newline="", encoding="utf-8", errors="replace")
    rd = csv.reader(f)
    hdr = next(rd)
    ix = {c: hdr.index(c) for c in ("tow", "cons", "signal", "sat_i", "ref_sat", "sta_a", "sta_b", "dd_cyc")}
    tow0 = None
    k = -1
    last = None
    runlen = collections.Counter()      # (a, b, sat) -> epochs of continuous presence (every epoch, not strided)
    seen = set()
    group = collections.defaultdict(dict)   # (cons, a, b) -> {sat: (e, runlen)}
    H = collections.defaultdict(lambda: {"e": [], "c": [], "u": [], "o": [], "ref": collections.Counter()})
    age = collections.defaultdict(list)

    def flush(hour):
        for (cons, a, b), d in group.items():
            if len(d) < 3:
                continue
            c = st.median(v[0] for v in d.values())
            oldest = max(d.items(), key=lambda kv: (kv[1][1], kv[0]))[0]
            eo = d[oldest][0]
            h = H[(hour, cons)]
            h["c"].append(c)
            for sat, (e, rl) in d.items():
                h["e"].append(e)
                h["u"].append(e - c)
                if sat != oldest:
                    h["o"].append(e - eo)
                for lo, hi, nm in AGE_BINS:
                    if lo <= rl < hi:
                        age[nm].append(abs(e - c))
                        break

    def advance_runs():
        for key in list(runlen):
            if key not in seen:
                del runlen[key]
        for key in seen:
            runlen[key] += 1

    for row in rd:
        tow = float(row[ix["tow"]])
        if tow != last:
            if last is not None:
                advance_runs()
                if k % stride == 0:
                    flush(int((last - tow0) // 3600))
            group.clear()
            seen = set()
            k += 1
            last = tow
            if tow0 is None:
                tow0 = tow
        if row[ix["signal"]] != sig:
            continue
        sat, ref = row[ix["sat_i"]], row[ix["ref_sat"]]
        a, b, cons = row[ix["sta_a"]], row[ix["sta_b"]], row[ix["cons"]]
        seen.add((a, b, sat))
        seen.add((a, b, ref))
        if k % stride:
            continue
        N = [n_at(n_int, slips, s_, sv, sig, tow) for s_, sv in ((a, sat), (a, ref), (b, sat), (b, ref))]
        if any(v is None for v in N):
            continue
        e = float(row[ix["dd_cyc"]]) - ((N[0] - N[1]) - (N[2] - N[3]))
        group[(cons, a, b)][sat] = (e, runlen[(a, b, sat)])
        H[(int((tow - tow0) // 3600), cons)]["ref"][ref] += 1
    if last is not None:
        advance_runs()
        if k % stride == 0:
            flush(int((last - tow0) // 3600))
    f.close()

    def P(*a):
        print(*a, file=out)

    P(f"signal {sig}, stride {stride}, groups need >= 3 satellites")
    P(f"{'h':>2} {'cons':>4} {'n':>7} {'as-traced':>9} {'r0%':>5} | {'common':>6} | {'ref-free':>8} {'r0%':>5} | "
      f"{'oldest-ref':>10} {'r0%':>5} | ref (rows)")
    for (h, cons) in sorted(H):
        d = H[(h, cons)]
        refs = " ".join(f"{s}:{n}" for s, n in d["ref"].most_common(2))
        P(f"{h:2d} {cons:>4} {len(d['e']):7d} {med(d['e']):9.3f} {r0(d['e']):5.1f} | {med(d['c']):6.3f} | "
          f"{med(d['u']):8.3f} {r0(d['u']):5.1f} | {med(d['o']):10.3f} {r0(d['o']):5.1f} | {refs}")
    P(f"\nref-free {sig} med|u| by the graded satellite's leg age (epochs of continuous presence at the pair):")
    for _, _, nm in AGE_BINS:
        x = age.get(nm, [])
        P(f"  {nm:>11} {st.median(x) if x else float('nan'):7.3f}  n={len(x)}")
    return H, age


def selftest():
    """Two stations, one constellation, ref G01 carrying a between-station SD error of +0.70 cyc, three graded
    sats: G02/G03 exact, G04 at +0.30.  As-traced med|e| must read the ref's 0.70; ref-free must read G04's 0.30
    only at G04 and 0 elsewhere (median over three = 0), so med|u| = 0 and round0 = 100 %."""
    td = tempfile.mkdtemp(prefix="ddreref_")
    fx = os.path.join(td, "fixture")
    run = os.path.join(td, "run")
    os.makedirs(os.path.join(run, "logs"))
    lam = 0.190293672798
    N = {"G01": 100, "G02": 200, "G03": 300, "G04": 400}
    for sta, base in (("SAAA", 100000), ("SBBB", 200000)):
        os.makedirs(os.path.join(fx, sta))
        with open(os.path.join(fx, sta, "truth_ambiguities.csv"), "w") as f:
            f.write("satellite,signal,N_int,wavelength_m\n")
            for s, n in N.items():
                f.write(f"{s},L1C,{base + n},{lam:.12f}\n")
        with open(os.path.join(fx, sta, "truth_slips.csv"), "w") as f:
            f.write("week,tow_s,satellite,signal,delta_cycles,flagged,elev_deg\n")
    s_err = {"G01": 0.70, "G02": 0.0, "G03": 0.0, "G04": 0.30}     # between-station SD errors (cycles)
    with open(os.path.join(run, "logs", "network_dd_fractional.csv"), "w") as fd:
        fd.write("week,tow,cons,signal,sat_i,ref_sat,sta_a,sta_b,dd_m,lambda_m,dd_cyc,frac_cyc,"
                 "dd_sigma_cyc,sigma_carry_aware\n")
        for k in range(16):
            tow = 30.0 * k
            for sat in ("G02", "G03", "G04"):
                dd_true = 0                          # same per-sat offsets at both stations: DD of truth = 0
                e = s_err[sat] - s_err["G01"]
                dd = dd_true + e
                fd.write(f"2313,{tow:.3f},G,L1C,{sat},G01,SAAA,SBBB,{dd * lam:.6f},{lam:.12f},{dd:.6f},"
                         f"{e - round(e):.6f},0.01,0\n")
    buf = io.StringIO()
    H, age = reref(fx, run, stride=1, sig="L1C", out=buf)
    d = H[(0, "G")]
    ok = True

    def chk(name, got, want, tol=1e-9):
        nonlocal ok
        good = abs(got - want) <= tol
        ok &= good
        print(f"  {'PASS' if good else 'FAIL'} {name}: got {got:.6f} want {want:.6f}")

    chk("as-traced med|e| (the ref's 0.70 leaks into every chord)", med(d["e"]), 0.70)
    chk("common = -s_ref", st.median(d["c"]), -0.70)
    chk("ref-free med|u| (G02/G03 exact, G04 0.30 -> median 0)", med(d["u"]), 0.0)
    chk("ref-free round0 %", r0(d["u"]), 100.0)
    chk("oldest-ref med|e_i - e_j| (G04 oldest by tie-break; G02/G03 - G04 = -0.30)", med(d["o"]), 0.30)
    chk("n graded", float(len(d["e"])), 48.0)
    chk("leg-age bin [10,30) holds the rows from epoch 10 on", float(len(age.get("[10,30)", []))), 18.0)
    print("SELFTEST", "PASS" if ok else "FAIL")
    return 0 if ok else 1


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("fixture_dir", nargs="?")
    ap.add_argument("run_dir", nargs="?")
    ap.add_argument("--stride", type=int, default=8)
    ap.add_argument("--signal", default="L1C")
    ap.add_argument("--selftest", action="store_true")
    a = ap.parse_args()
    if a.selftest:
        return selftest()
    if not a.fixture_dir or not a.run_dir:
        ap.error("fixture_dir and run_dir are required (or --selftest)")
    reref(a.fixture_dir, a.run_dir, stride=max(1, a.stride), sig=a.signal)
    return 0


if __name__ == "__main__":
    sys.exit(main())
