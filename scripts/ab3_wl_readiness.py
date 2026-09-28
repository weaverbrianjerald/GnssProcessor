#!/usr/bin/env python3
"""AB3-0 wide-lane readiness read (docs/refound/AB3_lanes_2026-09-20.md section 3): trace only.

Question: under the float ambiguity vehicle, do wide-lane integers exist on the field bed?
On the retired vehicle the field DD fractions were uniform on [0, 0.5] (zero integer
information) while the sim was integer.

Input: <run>/logs/network_dd_fractional.csv alone (no truth, no fixture, no products), one row
per (epoch, cons, signal, sat_i, ref_sat, sta_a, sta_b) with dd_cyc / frac_cyc / dd_sigma_cyc.
The wide lane is formed in cycle-difference form on the matched quadruple, the same form
scripts/simnet_amb_integer.py uses for its truth-graded lanes:

    WL_cyc = dd_cyc(L1 signal) - dd_cyc(L2W)      on the same (tow, sat_i, ref_sat, sta_a, sta_b)

ref_sat is part of the join key, so a chord whose two bands sit on different reference
satellites is never combined (that happens on the field: L1W runs its own reference family).

The reference satellite is in every chord (project instructions; SPEC_amb_iono section 6.12).
Every chord of an (epoch, cons, ref_sat, sta_a, sta_b) group carries -s_ref, the reference's own
between-station wide-lane error, as a common additive term.  Two reference-free readouts, both
reported, neither of them the raw as-traced value:

  R   = |mean_i exp(2*pi*i*f_i)| per group, averaged over groups.  A common additive term
        rotates the whole group on the unit circle and leaves |R| unchanged, so R is
        reference-free without subtracting anything.  This is the primary statistic and it is
        immune to null trap (b).
  u_i = wrap(f_i - circ_median_j f_j), the re-referencing of scripts/simnet_dd_reref.py and
        scripts/dd_frac_reref.py.  Quoted only as a gap to its own size-matched null, because
        the subtraction shrinks |residual| about 42 % on pure uniform noise.

Null traps (both have produced false findings on this project; see the project instructions):
  (a) every concentration number is printed beside a size-matched Monte-Carlo uniform null with
      the same group sizes and the same estimator, and the verdict is the gap;
  (b) a group's own circular median is never subtracted before grading a headline number;
  (c) a wrapped statistic is meaningless when the underlying errors span many cycles: the
      formal dd_sigma_cyc census in section M4 is printed so the reader can check that first.

Usage:
    set PYTHONIOENCODING=utf-8
    python scripts/ab3_wl_readiness.py runs/_rf0hv3_field6 --label FIELD --out runs/_ab3_read_20260920/field.txt
    python scripts/ab3_wl_readiness.py runs/_rf0hv3_sim6   --label SIM   --out runs/_ab3_read_20260920/sim.txt
    python scripts/ab3_wl_readiness.py --selftest
"""
import argparse
import collections
import csv
import io
import math
import os
import random
import statistics as st
import sys
import tempfile

NEAR = 0.15         # the AB3 near-integer band, cycles (uniform gives 30 %)
MIN_GROUP = 3       # a group needs >= 3 satellites before a circular median means anything
SEED = 20260920     # fixed: every null in this script is reproducible
GRADE_H0 = 3        # the decision-rule window starts at this 1-based hour
MINLEN = 120        # M3: minimum consecutive epochs for a stability chord


def wrap(x):
    """Map a cycle count to (-0.5, +0.5]."""
    return x - round(x)


def circ_median(vals):
    """The value minimising sum_i |wrap(f_i - c)|.  Attained at a data point; O(n^2)."""
    best, bestcost = vals[0], None
    for c in vals:
        cost = 0.0
        for v in vals:
            cost += abs(wrap(v - c))
        if bestcost is None or cost < bestcost:
            best, bestcost = c, cost
    return best


def resultant(vals):
    """|mean exp(2*pi*i*f)|.  1 = concentrated, ~0.886/sqrt(n) under uniform noise.
    Invariant to a common additive shift, i.e. to the reference satellite's own error."""
    c = sum(math.cos(2 * math.pi * v) for v in vals) / len(vals)
    s = sum(math.sin(2 * math.pi * v) for v in vals) / len(vals)
    return math.hypot(c, s)


def med(x):
    return st.median(abs(v) for v in x) if x else float("nan")


def near(x, band=NEAR):
    return 100.0 * sum(1 for v in x if abs(v) <= band) / len(x) if x else float("nan")


class Null:
    """Size-matched Monte-Carlo uniform null: same group sizes, same estimators, fixed seed."""

    def __init__(self, draws=400, seed=SEED):
        self.draws = draws
        self.rng = random.Random(seed)
        self.cache = {}

    def of(self, n):
        """(E med|u|, E R, E med|f|, E near%(u), E near%(f)) for a uniform group of size n."""
        if n in self.cache:
            return self.cache[n]
        u, f, r = [], [], []
        for _ in range(self.draws):
            v = [self.rng.random() - 0.5 for _ in range(n)]
            c = circ_median(v)
            u.extend(wrap(x - c) for x in v)
            f.extend(v)
            r.append(resultant(v))
        out = (med(u), st.mean(r), med(f), near(u), near(f))
        self.cache[n] = out
        return out


class Acc:
    """One graded population (a signal or a lane, in one hour or pooled)."""

    def __init__(self):
        self.f, self.u, self.R = [], [], []
        self.nf, self.nu, self.nR, self.nnu, self.nnf = [], [], [], [], []
        self.ref = collections.Counter()
        self.sig = []                  # formal sigma proxy in cycles, one per graded row

    def add_group(self, vals, nul):
        n = len(vals)
        nu, nR, nf, nnu, nnf = nul.of(n)
        c = circ_median(vals)
        self.R.append(resultant(vals))
        self.nR.append(nR)
        self.nf.append(nf)
        self.nu.append(nu)
        self.nnu.append(nnu)
        self.nnf.append(nnf)
        for v in vals:
            self.f.append(v)
            self.u.append(wrap(v - c))

    def merge(self, o):
        for k in ("f", "u", "R", "nf", "nu", "nR", "nnu", "nnf", "sig"):
            getattr(self, k).extend(getattr(o, k))
        self.ref.update(o.ref)

    def row(self, label):
        if not self.f:
            return f"  {label:<12} (no rows)"
        gapR = st.mean(self.R) - st.mean(self.nR)
        gapu = med(self.u) - st.mean(self.nu)
        refs = " ".join(f"{s}:{n}" for s, n in self.ref.most_common(3))
        return (f"  {label:<12} n={len(self.f):<7d} grp={len(self.R):<6d} | "
                f"med|f| {med(self.f):.3f} (null {st.mean(self.nf):.3f}) near15 {near(self.f):5.1f}% "
                f"(null {st.mean(self.nnf):4.1f}) | R {st.mean(self.R):.3f} (null {st.mean(self.nR):.3f}, "
                f"gap {gapR:+.3f}) | reffree med|u| {med(self.u):.3f} (null {st.mean(self.nu):.3f}, "
                f"gap {gapu:+.3f}) near15 {near(self.u):5.1f}% (null {st.mean(self.nnu):4.1f}) | {refs}")


HEADER = ("  (med|f| and near15 are AS-TRACED -- the reference is still in them.  R is the "
          "reference-free concentration\n   (invariant to a common rotation, no subtraction).  "
          "med|u| subtracts the group circular median: read its GAP only.)")


def grade(run, label, l1_signals, l2_signal, cons_filter, null_draws, out=sys.stdout):
    path = os.path.join(run, "logs", "network_dd_fractional.csv")
    f = io.open(path, newline="", encoding="utf-8", errors="replace")
    rd = csv.reader(f)
    hdr = next(rd)
    ix = {c: hdr.index(c) for c in ("tow", "cons", "signal", "sat_i", "ref_sat", "sta_a", "sta_b",
                                    "dd_cyc", "frac_cyc", "dd_sigma_cyc")}
    nul = Null(draws=null_draws, seed=SEED)

    lanes = [f"{s}-{l2_signal}" for s in l1_signals]
    bands = list(l1_signals) + [l2_signal]
    series = bands + lanes + [f"NLrem({s})" for s in l1_signals]

    H = collections.defaultdict(Acc)          # (hour, series) -> Acc
    epoch = {}                                # (cons, sat, ref, a, b) -> {signal: (dd, sigma)}
    tow0 = None
    last = None
    k = -1
    nrows = 0
    zeroed = 0
    epochs_seen = 0
    sigcensus = collections.defaultdict(list)  # (hour, series) -> formal sigma cycles
    # M3 stability bookkeeping: chord key -> [last_k, [values...], first_k]
    runs_state = {}
    runs_done = collections.defaultdict(list)  # series -> [(n, stdev, med_value)]
    joinable = collections.Counter()
    refmismatch = collections.Counter()

    def hour_of(tow):
        return int((tow - tow0) // 3600) + 1      # 1-based

    def close_runs(kk, force=False):
        for key in list(runs_state):
            lk, vals, fk = runs_state[key]
            if force or lk < kk - 1:
                if len(vals) >= MINLEN:
                    runs_done[key[0]].append((len(vals), st.pstdev(vals), st.median(vals)))
                del runs_state[key]

    def flush(tow):
        nonlocal epochs_seen
        epochs_seen += 1
        hour = hour_of(tow)
        # ---- assemble per-series values on this epoch, keyed by (cons, ref, a, b) group ----
        groups = collections.defaultdict(lambda: collections.defaultdict(dict))  # series -> grp -> sat -> frac
        for (cons, sat, ref, a, b), d in epoch.items():
            grp = (cons, ref, a, b)
            for sg in bands:
                if sg in d:
                    dd, sd = d[sg]
                    groups[sg][grp][sat] = wrap(dd)
                    sigcensus[(hour, sg)].append(sd)
            for s1 in l1_signals:
                if s1 in d and l2_signal in d:
                    joinable[s1] += 1
                    dd1, sd1 = d[s1]
                    dd2, sd2 = d[l2_signal]
                    wl = dd1 - dd2
                    lane = f"{s1}-{l2_signal}"
                    groups[lane][grp][sat] = wrap(wl)
                    # conservative formal bound (the two bands are correlated; no rho in the trace)
                    sigcensus[(hour, lane)].append(math.hypot(sd1, sd2))
                    # narrow-lane-like remainder: the L1 DD with the WL integer removed.
                    # identity: wrap(dd1 - round(wl)) == wrap(f_L2 + f_WL); context only.
                    groups[f"NLrem({s1})"][grp][sat] = wrap(dd1 - round(wl))
                    # M3 chord runs on the lane
                    ckey = (lane, sat, ref, a, b)
                    if hour >= GRADE_H0:
                        prev = runs_state.get(ckey)
                        if prev is not None and prev[0] == k - 1:
                            prev[1].append(wl)
                            prev[0] = k
                        else:
                            if prev is not None and len(prev[1]) >= MINLEN:
                                runs_done[lane].append((len(prev[1]), st.pstdev(prev[1]),
                                                        st.median(prev[1])))
                            runs_state[ckey] = [k, [wl], k]
        for ser, gs in groups.items():
            for grp, d in gs.items():
                if len(d) < MIN_GROUP:
                    continue
                acc = H[(hour, ser)]
                acc.ref[grp[1]] += len(d)
                acc.add_group(list(d.values()), nul)
        close_runs(k)

    for row in rd:
        tow = float(row[ix["tow"]])
        if tow != last:
            if last is not None:
                flush(last)
                epoch.clear()
            k += 1
            last = tow
            if tow0 is None:
                tow0 = tow
        cons = row[ix["cons"]]
        if cons_filter and cons != cons_filter:
            continue
        dd = float(row[ix["dd_cyc"]])
        fr = float(row[ix["frac_cyc"]])
        if dd == 0.0 and fr == 0.0:
            zeroed += 1                       # lambda unknown -> both columns zeroed
            continue
        nrows += 1
        key = (cons, row[ix["sat_i"]], row[ix["ref_sat"]], row[ix["sta_a"]], row[ix["sta_b"]])
        epoch[key] = epoch.get(key, {})
        epoch[key][row[ix["signal"]]] = (dd, float(row[ix["dd_sigma_cyc"]]))
        # ref-mismatch census: the same (sat, a, b) on the two bands with different references
        nk = (cons, row[ix["sat_i"]], row[ix["sta_a"]], row[ix["sta_b"]])
        refmismatch[(nk, row[ix["signal"]])] = row[ix["ref_sat"]]
    if last is not None:
        flush(last)
    close_runs(0, force=True)
    f.close()

    def P(*a):
        print(*a, file=out)

    hours = sorted({h for (h, _) in H})
    pooled = {}
    for ser in series:
        a = Acc()
        for h in hours:
            if h >= GRADE_H0 and (h, ser) in H:
                a.merge(H[(h, ser)])
        pooled[ser] = a

    P("=" * 110)
    P(f"AB3-0 WIDE-LANE READINESS READ -- {label}")
    P("=" * 110)
    P(f"  run          : {run}")
    P(f"  rows graded  : {nrows}   (zeroed dd/frac rows skipped: {zeroed})")
    P(f"  epochs       : {epochs_seen}   tow {tow0:.0f} .. {last:.0f}   "
      f"span {(last - tow0) / 3600.0:.2f} h")
    P(f"  bands        : {', '.join(bands)}      lanes: {', '.join(lanes)}")
    P(f"  null         : size-matched Monte-Carlo uniform, {null_draws} draws, seed {SEED}, "
      f"groups need >= {MIN_GROUP} sats")
    P(f"  near-band    : |.| <= {NEAR} cyc (uniform gives 30.0 %)")
    P("")
    P("-- M0  WIDE-LANE JOIN CENSUS (the join key carries ref_sat, so cross-reference pairs never combine) --")
    for s1 in l1_signals:
        P(f"  {s1}-{l2_signal}: {joinable[s1]} matched quadruples over the run "
          f"({joinable[s1] / max(1, epochs_seen):.1f} per epoch)")
    # how many (sat, a, b) had both bands but on different references
    mm = collections.defaultdict(dict)
    for (nk, sg), ref in refmismatch.items():
        mm[nk][sg] = ref
    for s1 in l1_signals:
        diff = sum(1 for nk, d in mm.items()
                   if s1 in d and l2_signal in d and d[s1] != d[l2_signal])
        P(f"  {s1}-{l2_signal}: {diff} distinct (sat, sta_a, sta_b) keys where the two bands "
          f"sat on DIFFERENT reference satellites at the last sighting (not joined)")
    P("")

    for title, sers in (("M1  PER-SIGNAL BAND FRACTIONS", bands),
                        ("M2  WIDE LANE  WL_cyc = dd_cyc(L1) - dd_cyc(L2W)", lanes),
                        ("M2b NARROW-LANE-LIKE REMAINDER (context only; "
                         "identically wrap(f_L2 + f_WL))", [f"NLrem({s})" for s in l1_signals])):
        P(f"-- {title} --")
        P(HEADER)
        for ser in sers:
            P(f"  [{ser}]")
            for h in hours:
                if (h, ser) in H and H[(h, ser)].f:
                    P("  " + H[(h, ser)].row(f"h{h}"))
            P("  " + pooled[ser].row(f"h{GRADE_H0}-{max(hours) if hours else GRADE_H0} POOLED"))
        P("")

    P("-- M3  STABILITY: chords with >= %d CONSECUTIVE epochs inside hours %d-%d --"
      % (MINLEN, GRADE_H0, max(hours) if hours else GRADE_H0))
    P("  (within-chord stdev of the RAW WL_cyc: a real integer ambiguity gives a small constant "
      "scatter.\n   chord key = (lane, sat_i, ref_sat, sta_a, sta_b); a one-epoch gap or a "
      "reference switch starts a new run.)")
    for lane in lanes:
        rr = runs_done.get(lane, [])
        if not rr:
            P(f"  {lane:<12} 0 qualifying chords")
            continue
        sd = sorted(x[1] for x in rr)
        fr = [wrap(x[2]) for x in rr]
        ln = sorted(x[0] for x in rr)
        P(f"  {lane:<12} chords {len(rr):5d}  len p50 {ln[len(ln) // 2]:4d}  "
          f"within-chord sd(WL_cyc) p50 {sd[len(sd) // 2]:.3f} cyc  p90 {sd[int(.9 * (len(sd) - 1))]:.3f}  "
          f"| chord-median WL fraction: med|f| {med(fr):.3f}  within {NEAR} cyc {near(fr):5.1f} % "
          f"(uniform 30.0 %)")
    P("")

    P("-- M4c  ARE THE UNDERLYING ERRORS SUB-CYCLE?  formal dd_sigma_cyc census (cycles) --")
    P("  (a wrapped statistic is meaningless when the error spans many cycles.  The lane figure is "
      "hypot(s1, s2),\n   an UPPER bound: the two bands are correlated and the trace carries no "
      "correlation.)")
    P(f"  {'series':<14}{'hour':>5}{'n':>9}{'p50':>9}{'p90':>9}{'p99':>9}")
    for ser in bands + lanes:
        for h in hours:
            v = sorted(sigcensus.get((h, ser), []))
            if not v:
                continue
            if h < GRADE_H0:
                continue
            P(f"  {ser:<14}{h:>5}{len(v):>9}{v[len(v) // 2]:>9.3f}"
              f"{v[int(.9 * (len(v) - 1))]:>9.3f}{v[int(.99 * (len(v) - 1))]:>9.3f}")
    P("")

    P("-- DECISION RULE (frozen, AB3 section 3): open a lane build card only if, in hours %d-%d, "
      "the wide-lane\n   concentration exceeds its null by >= 0.10 AND >= 50 %% of wide-lane "
      "fractions lie within %.2f cyc --" % (GRADE_H0, max(hours) if hours else GRADE_H0, NEAR))
    for lane in lanes:
        a = pooled[lane]
        if not a.f:
            P(f"  {lane:<12} NO DATA -- rule not evaluable")
            continue
        gapR = st.mean(a.R) - st.mean(a.nR)
        n_as = near(a.f)
        n_rf = near(a.u)
        n_rf_null = st.mean(a.nnu)
        for nm, share, ref in (("as-traced", n_as, 30.0), ("ref-free ", n_rf, n_rf_null)):
            verdict = "PASS" if (gapR >= 0.10 and share >= 50.0) else "FAIL"
            P(f"  {lane:<12} [{nm}] concentration gap {gapR:+.3f} (>= +0.10?  "
              f"{'yes' if gapR >= 0.10 else 'NO'})   share within {NEAR} cyc {share:.1f} % "
              f"(null {ref:.1f} %) (>= 50 %?  {'yes' if share >= 50.0 else 'NO'})   -> {verdict}")
    P("=" * 110)
    return {"H": H, "pooled": pooled, "runs": runs_done, "joinable": joinable}


def selftest():
    """Three synthetic beds through the real path.  The point of bed INTREF is the instrument
    lesson this read turns on.

    BED INTREF : the wide lane is an integer (N + 0.06 cyc noise) but the reference satellite's
                 own wide-lane error is large (+-0.5 cyc per station, so the reference DD term
                 wraps uniformly).  R and the ref-free readouts must fire; the as-traced near15
                 share must sit at ~30 %, because the as-traced fraction is a test of the
                 reference, not of the lane.
    BED INTCLN : the same integer lane with a small reference term.  Every readout fires,
                 including the as-traced share.
    BED UNIF   : the wide lane is uniform noise.  Every readout must sit at its null: the guard
                 that makes an INT win meaningful rather than a subtraction artifact.
    """
    ok = True

    def chk(name, cond, detail=""):
        nonlocal ok
        ok &= bool(cond)
        print(f"  {'PASS' if cond else 'FAIL'} {name} {detail}")

    stations = [f"S{i:03d}" for i in range(6)]
    sats = [f"G{i:02d}" for i in range(2, 9)]
    lam1, lam2 = 0.190294, 0.244210

    def write(path, mode, seed):
        rng = random.Random(seed)
        os.makedirs(os.path.join(path, "logs"))
        refamp = {"intref": 0.5, "intcln": 0.02, "unif": 0.5}[mode]
        with open(os.path.join(path, "logs", "network_dd_fractional.csv"), "w") as fd:
            fd.write("week,tow,cons,signal,sat_i,ref_sat,sta_a,sta_b,dd_m,lambda_m,dd_cyc,"
                     "frac_cyc,dd_sigma_cyc,sigma_carry_aware\n")
            for kk in range(400):       # 3.3 h at 30 s: the pooled h3+ window exists
                tow = 259200.0 + 30.0 * kk
                rho = {s: (rng.random() - 0.5) * 2 * refamp for s in stations}   # ref sat's WL error
                for i, a in enumerate(stations):
                    for b in stations[i + 1:]:
                        for sat in sats:
                            if mode.startswith("int"):
                                wl = 11 + (rng.random() - 0.5) * 0.06 - (rho[a] - rho[b])
                            else:
                                wl = 11 + rng.random() - 0.5
                            dd2 = 37 + (rng.random() - 0.5) * 0.4
                            dd1 = dd2 + wl
                            for sg, dd, lam in (("L1C", dd1, lam1), ("L2W", dd2, lam2)):
                                fd.write(f"2313,{tow:.3f},G,{sg},{sat},G01,{a},{b},"
                                         f"{dd * lam:.4f},{lam:.6f},{dd:.4f},{wrap(dd):.4f},"
                                         f"0.05,0\n")

    td = tempfile.mkdtemp(prefix="ab3wl_")
    res = {}
    for mode, seed in (("intref", 3), ("intcln", 4), ("unif", 5)):
        run = os.path.join(td, mode)
        write(run, mode, seed=seed)
        buf = io.StringIO()
        r = grade(run, mode.upper(), ["L1C"], "L2W", "G", 120, out=buf)
        a = r["pooled"]["L1C-L2W"]
        R, nR = st.mean(a.R), st.mean(a.nR)
        res[mode] = (R - nR, near(a.f), near(a.u), st.mean(a.nnu), med(a.u), st.mean(a.nu))
        print(f"  [{mode:>6}] R gap {R - nR:+.3f} | as-traced near15 {near(a.f):5.1f} % | "
              f"ref-free near15 {near(a.u):5.1f} % (null {st.mean(a.nnu):.1f}) | "
              f"ref-free med|u| {med(a.u):.3f} (null {st.mean(a.nu):.3f})")
    chk("INTREF: the concentration fires", res["intref"][0] > 0.30,
        f"gap {res['intref'][0]:+.3f} > 0.30")
    chk("INTREF: ref-free near15 fires", res["intref"][2] > 90.0, f"{res['intref'][2]:.1f} % > 90 %")
    chk("INTREF: AS-TRACED near15 stays at ~30 % (it tests the REFERENCE, not the lane)",
        abs(res["intref"][1] - 30.0) < 5.0, f"{res['intref'][1]:.1f} % ~ 30 %")
    chk("INTREF: ref-free med|u| collapses below its null", res["intref"][4] < 0.5 * res["intref"][5],
        f"{res['intref'][4]:.3f} vs null {res['intref'][5]:.3f}")
    chk("INTCLN: as-traced near15 fires when the reference is near-integer too",
        res["intcln"][1] > 90.0, f"{res['intcln'][1]:.1f} % > 90 %")
    chk("UNIF: concentration AT its null", abs(res["unif"][0]) < 0.05, f"gap {res['unif'][0]:+.3f}")
    chk("UNIF: as-traced near15 at 30 %", abs(res["unif"][1] - 30.0) < 4.0, f"{res['unif'][1]:.1f} %")
    chk("UNIF: ref-free near15 NO better than its null", res["unif"][2] < 1.1 * res["unif"][3],
        f"{res['unif'][2]:.1f} % vs null {res['unif'][3]:.1f} %")
    chk("UNIF: ref-free med|u| NO better than its null", res["unif"][4] > 0.8 * res["unif"][5],
        f"{res['unif'][4]:.3f} vs null {res['unif'][5]:.3f}")
    chk("UNIF: the ref-free null is BELOW 0.25 (the trap (b) artifact)", res["unif"][5] < 0.235,
        f"null {res['unif'][5]:.3f} < 0.235")
    print("SELFTEST", "PASS" if ok else "FAIL")
    return 0 if ok else 1


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("run_dir", nargs="?")
    ap.add_argument("--label", default="RUN")
    ap.add_argument("--l1", default="L1C,L1W", help="comma list of L1 signals to pair with --l2")
    ap.add_argument("--l2", default="L2W")
    ap.add_argument("--cons", default="G")
    ap.add_argument("--null-draws", type=int, default=400)
    ap.add_argument("--out", default=None)
    ap.add_argument("--selftest", action="store_true")
    a = ap.parse_args()
    if a.selftest:
        return selftest()
    if not a.run_dir:
        ap.error("run_dir is required (or --selftest)")
    l1 = [s for s in a.l1.split(",") if s]
    if a.out:
        os.makedirs(os.path.dirname(os.path.abspath(a.out)), exist_ok=True)
        with io.open(a.out, "w", encoding="utf-8") as fh:
            grade(a.run_dir, a.label, l1, a.l2, a.cons, a.null_draws, out=fh)
        with io.open(a.out, encoding="utf-8") as fh:
            sys.stdout.write(fh.read())
    else:
        grade(a.run_dir, a.label, l1, a.l2, a.cons, a.null_draws)
    return 0


if __name__ == "__main__":
    sys.exit(main())
