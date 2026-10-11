#!/usr/bin/env python3
"""Truth grade of the producer's Phase-2 AR search (docs/network_ambiguity_resolution_design.md,
the 2026-09-10 note §4.1: "a fix count is not a grade").

`NetworkProducerAr` forms between-station double differences of the network's float ambiguities
(star topology per (constellation, signal): one reference satellite, one reference station), builds
their conditional covariance, and runs MLAMBDA on the `ils_max_dim` lowest-sigma of them.  With
`network.ambiguity_resolution.trace` armed it writes every DD of the basis to
`logs/network_producer_ar.csv` with its identity, float value, formal sigma, whether it entered the
search subset, and the integer the ILS returned.  This script joins that file to the simulator's
per-station truth and answers the three questions a fix count cannot:

  1. **Is the float right?**  e = float_cyc - DD_true, where
     DD_true = [N(sta_r, sat_j) - N(sta_r, ref_sat)] - [N(ref_sta, sat_j) - N(ref_sta, ref_sat)]
     at that epoch (truth slips accumulated via simnet_amb_integer.n_at; the quiet bed has none).
     Reported as wrapped |frac| p50, share < 0.15 / < 0.25, and the unwrapped std, for the whole
     basis and for the searched subset separately, per band.  The two populations are different by
     construction: the cap selects the tightest DDs, so a subset number is not a basis number.
  2. **Are the fixed integers right?**  The correct-fix rate among kept DDs (fixed_int == DD_true),
     the wrong-fix count with the |e| of each, per band and per sigma bin.  This is the gate that
     matters: a wrong fix accepted at p_succ >= 0.9999 falsifies the carry-blind covariance as a
     gate input, and no amount of fix rate compensates for it.
  3. **What is the ceiling?**  The share of searched DDs with |e| < 0.25, the ones plain rounding
     would already get right.  If the ceiling is high and the fix rate is low, the search is being
     refused by its gates; if the ceiling is low, the float is the problem and Phase 3 is premature.

**The wind-up branch of each arc (card ARL section 4).**  The simulator accumulates phase wind-up
from the satellite's rise at its own mask; the producer (estimation.accumulate_phase_windup) starts
each arc's accumulator at the arc's first epoch with the WRAPPED value, so where the simulator's
accumulated value at that epoch is beyond +-0.5 cycle the producer's integer differs from the
simulator's by k = round(wu_sim(t0)) on every band of that arc (N_producer = N_sim + k).  Both are
right; they count the same phase from different branches.  The grader therefore takes each arc's
first epoch from the producer's own trace (logs/network_slip_raw.csv, one row per dual-frequency
phase line of sight the producer models, from the run's first epoch; a gap
longer than the accumulator's 300 s continuity horizon starts a new arc) and the simulator's
wind-up there (<fixture>/<sta>/truth_windup.csv), and grades against N_sim + k.  --no-windup-branch
grades against the simulator's raw integers.

**The cycle basis (card AGT, network.ambiguity_resolution.cycle_basis).**  When the trace carries
the column `cycle_kind` (partner or borrowed, with `band_only` 1 on a chord that exists in one band
only), the kept and correct fixes, and every wrong fix, are also split by kind: "partner",
"borrowed", and the same with "/band_only".  Borrowed DDs sit on longer baselines than partner DDs,
so zero wrong fixes is graded per kind (card AGT section 6).

Plus the reference sat/station identity over time and its switch count, because every DD in this
file is differenced against one such pair and inherits its error (SPEC_amb_iono_split_overcount
§6.12: any grade built on a single reference element must report that element's share or difference
it away).  A high switch count with a settled-window verdict means the graded population is rotating.

Reads:  <run>/logs/network_producer_ar.csv, <run>/logs/network_slip_raw.csv,
        <fixture>/<sta>/truth_ambiguities.csv + truth_slips.csv + truth_windup.csv
Usage:  python scripts/simnet_producer_ar_grade.py <fixture_dir> --run <run_dir> [--settle-h 12]
                                                   [--stride N] [--json]
        python scripts/simnet_producer_ar_grade.py --selftest
"""
import argparse
import collections
import csv
import io
import json
import math
import os
import statistics as st
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from simnet_amb_integer import load_truth, n_at  # noqa: E402

TRACE_REL = os.path.join("logs", "network_producer_ar.csv")
ARC_TRACE_REL = os.path.join("logs", "network_slip_raw.csv")

# The producer's wind-up accumulator continuity horizon (GnssCorrections: a gap longer than this
# restarts the accumulator at the wrapped value, which is a new branch).
WINDUP_HORIZON_S = 300.0

# Formal-sigma bins (cycles) for the wrong-fix breakdown.  The ILS gates on FFR, not on sigma
# directly, so a wrong fix concentrated in one bin says the covariance is mis-scaled in that bin.
SIGMA_BINS = ((0.0, 0.01, "[0,.01)"), (0.01, 0.03, "[.01,.03)"), (0.03, 0.10, "[.03,.10)"),
              (0.10, 0.30, "[.10,.30)"), (0.30, 1.00, "[.30,1)"), (1.00, 1e9, "1+"))


def wrap(x):
    """Fractional part of x in [-0.5, 0.5]."""
    return x - round(x)


def round_half_up(x):
    """Nearest integer, halves away from the producer's wrap: wrap(x) = x - k lies in [-0.5, 0.5)."""
    return int(math.floor(x + 0.5))


def load_windup_branches(fixture, run, stations, horizon_s=WINDUP_HORIZON_S):
    """Per (station, satellite): a sorted list of (arc_start_tow, arc_end_tow, k), where k is the
    integer the producer's integer differs from the simulator's by on that arc (both bands).

    Arc starts are the producer's: the first epoch each (station, satellite) has a modelled phase
    pair in network_slip_raw.csv, a new arc after a gap > horizon_s.  k = round(wu_sim(start)), the
    simulator's accumulated wind-up at that epoch.  Returns (branches, info)."""
    path = os.path.join(run, ARC_TRACE_REL)
    if not os.path.isfile(path):
        raise SystemExit("missing " + path + " (the producer's slip trace, or pass "
                         "--no-windup-branch)")
    seen = collections.defaultdict(set)          # (sta, sat) -> {tow}
    with io.open(path, newline="", encoding="utf-8", errors="replace") as f:
        rd = csv.reader(l for l in f if not l.startswith("#"))
        hdr = next(rd)
        i_tow, i_sta, i_sat = hdr.index("tow"), hdr.index("station"), hdr.index("sat")
        for row in rd:
            seen[(row[i_sta], row[i_sat])].add(float(row[i_tow]))

    wu = {}                                      # (sta, sat) -> {tow: windup_cycles}
    for sta in stations:
        p = os.path.join(fixture, sta, "truth_windup.csv")
        if not os.path.isfile(p):
            continue
        with open(p, newline="") as f:
            for row in csv.DictReader(f):
                wu.setdefault((sta, row["satellite"]), {})[float(row["tow_s"])] = \
                    float(row["windup_cycles"])

    branches = {}
    info = {"arcs": 0, "arcs_shifted": 0, "arcs_no_truth": 0, "shifted": []}
    for key, tows in seen.items():
        ts = sorted(tows)
        starts = [0] + [i for i in range(1, len(ts)) if ts[i] - ts[i - 1] > horizon_s]
        ends = [i - 1 for i in starts[1:]] + [len(ts) - 1]
        arcs = []
        for a, b in zip(starts, ends):
            info["arcs"] += 1
            w0 = wu.get(key, {}).get(ts[a])
            if w0 is None:
                info["arcs_no_truth"] += 1
                k = 0
            else:
                k = round_half_up(w0)
            if k != 0:
                info["arcs_shifted"] += 1
                info["shifted"].append({"sta": key[0], "sat": key[1], "start_tow": ts[a],
                                        "end_tow": ts[b], "wu_sim_start": w0, "k": k})
            arcs.append((ts[a], ts[b], k))
        branches[key] = arcs
    return branches, info


def branch_at(branches, sta, sat, tow):
    """k of the arc holding `tow` (the latest arc started at or before it); 0 if none."""
    k = 0
    for t0, _t1, kk in branches.get((sta, sat), ()):
        if t0 <= tow:
            k = kk
        else:
            break
    return k


def cycle_kind_of(row, i_kind, i_band_only):
    """The cycle basis's kind of a trace row ("partner", "borrowed", "partner/band_only", ...),
    or None on a trace without the column (card AGT)."""
    if i_kind is None:
        return None
    kind = row[i_kind]
    if i_band_only is not None and row[i_band_only] == "1":
        kind += "/band_only"
    return kind


def sigma_bin(s):
    for lo, hi, nm in SIGMA_BINS:
        if lo <= s < hi:
            return nm
    return "n/a"


class Acc:
    """Error accumulator for one population (all / subset, per band or pooled)."""

    def __init__(self):
        self.e = []        # unwrapped float error, cycles
        self.w = []        # wrapped |error|

    def add(self, e):
        self.e.append(e)
        self.w.append(abs(wrap(e)))

    def summary(self):
        n = len(self.w)
        if n == 0:
            return None
        s = sorted(self.w)
        return {
            "n": n,
            "wrap_abs_p50": s[n // 2],
            "pct_lt_0p15": 100.0 * sum(1 for v in self.w if v < 0.15) / n,
            "pct_lt_0p25": 100.0 * sum(1 for v in self.w if v < 0.25) / n,
            "unwrapped_std": st.pstdev(self.e) if n > 1 else 0.0,
            "unwrapped_abs_p50": sorted(abs(v) for v in self.e)[n // 2],
        }


def fmt_acc(label, d):
    if d is None:
        return "  {:<22} (no rows)".format(label)
    return ("  {:<22} n {:>8}  |frac| p50 {:6.3f}  <0.15 {:5.1f}%  <0.25 {:5.1f}%  "
            "unwrapped std {:9.3f}  |e| p50 {:8.3f}").format(
        label, d["n"], d["wrap_abs_p50"], d["pct_lt_0p15"], d["pct_lt_0p25"],
        d["unwrapped_std"], d["unwrapped_abs_p50"])


def grade(fixture, run, settle_h=12.0, stride=1, out=sys.stdout, windup_branch=True):
    """Join the producer-AR trace to truth; print the grade and return it as a dict."""
    stations = sorted(d for d in os.listdir(fixture)
                      if os.path.isdir(os.path.join(fixture, d))
                      and os.path.isfile(os.path.join(fixture, d, "truth_ambiguities.csv")))
    if not stations:
        raise SystemExit("no truth stations under " + fixture)
    n_int, slips = load_truth(fixture, stations)
    if windup_branch:
        branches, wb_info = load_windup_branches(fixture, run, stations)
    else:
        branches, wb_info = {}, None

    path = os.path.join(run, TRACE_REL)
    if not os.path.isfile(path):
        raise SystemExit("missing producer-AR trace: " + path +
                         "\n(arm network.ambiguity_resolution.{enable,trace})")

    need = ("tow", "cons", "signal", "sat_j", "ref_sat", "sta_r", "ref_sta",
            "float_cyc", "sigma_cyc", "in_subset", "fixed_int", "kept",
            "ratio", "p_succ", "n_dd", "n_search", "n_fixed")
    f = io.open(path, newline="", encoding="utf-8", errors="replace")
    rd = csv.reader(f)
    hdr = next(rd)
    missing = [c for c in need if c not in hdr]
    if missing:
        raise SystemExit("trace is missing columns %s -- is this a pre-card file?" % missing)
    ix = {c: hdr.index(c) for c in need}
    # Card AGT: the cycle basis's kind columns, present only when it was armed.
    i_kind = hdr.index("cycle_kind") if "cycle_kind" in hdr else None
    i_band_only = hdr.index("band_only") if "band_only" in hdr else None

    # Two windows: the settled one (the verdict) and h0-h12 (ignition, reported for contrast --
    # SPEC_level_information_budget §14's lesson that a short window measures ignition).
    W = ("settled", "h0_12")
    acc = {w: {"all": collections.defaultdict(Acc), "subset": collections.defaultdict(Acc)}
           for w in W}
    pooled = {w: {"all": Acc(), "subset": Acc()} for w in W}
    fixed = {w: {"n": 0, "correct": 0, "wrong": [], "by_kind": collections.Counter(),
                 "by_kind_ok": collections.Counter(), "by_band": collections.Counter(),
                 "by_band_ok": collections.Counter(), "by_sigma": collections.Counter(),
                 "by_sigma_ok": collections.Counter()} for w in W}
    ceiling = {w: {"n": 0, "ok": 0, "by_band": collections.Counter(),
                   "by_band_ok": collections.Counter()} for w in W}
    ep = {w: {} for w in W}      # tow -> (ratio, p_succ, n_dd, n_search, n_fixed)
    refs = []                    # (tow, cons, signal, ref_sat, ref_sta) first row per epoch/family
    seen_ref = set()
    n_rows = n_join = n_nojoin = 0
    tow0 = None
    keep_ep = {}

    for row in rd:
        tow = float(row[ix["tow"]])
        if tow0 is None:
            tow0 = tow
        # Stride on epochs, not rows (a row is a DD; striding rows would sample a band).
        k = keep_ep.get(tow)
        if k is None:
            k = (len(keep_ep) % stride == 0) if stride > 1 else True
            keep_ep[tow] = k
        if not k:
            continue
        n_rows += 1
        w = "settled" if (tow - tow0) >= settle_h * 3600.0 else "h0_12"
        cons, sig = row[ix["cons"]], row[ix["signal"]]
        sat_j, ref_sat = row[ix["sat_j"]], row[ix["ref_sat"]]
        sta_r, ref_sta = row[ix["sta_r"]], row[ix["ref_sta"]]

        key = (tow, cons, sig)
        if key not in seen_ref:
            seen_ref.add(key)
            refs.append((tow, cons, sig, ref_sat, ref_sta))
        ep[w][tow] = (float(row[ix["ratio"]]), float(row[ix["p_succ"]]),
                      int(row[ix["n_dd"]]), int(row[ix["n_search"]]), int(row[ix["n_fixed"]]))

        n_rj = n_at(n_int, slips, sta_r, sat_j, sig, tow)
        n_rp = n_at(n_int, slips, sta_r, ref_sat, sig, tow)
        n_fj = n_at(n_int, slips, ref_sta, sat_j, sig, tow)
        n_fp = n_at(n_int, slips, ref_sta, ref_sat, sig, tow)
        if None in (n_rj, n_rp, n_fj, n_fp):
            n_nojoin += 1
            continue
        n_join += 1
        # The producer's integer: the simulator's plus the arc's wind-up branch.
        n_rj += branch_at(branches, sta_r, sat_j, tow)
        n_rp += branch_at(branches, sta_r, ref_sat, tow)
        n_fj += branch_at(branches, ref_sta, sat_j, tow)
        n_fp += branch_at(branches, ref_sta, ref_sat, tow)
        dd_true = (n_rj - n_rp) - (n_fj - n_fp)
        e = float(row[ix["float_cyc"]]) - dd_true
        band = cons + ":" + sig

        acc[w]["all"][band].add(e)
        pooled[w]["all"].add(e)
        in_sub = row[ix["in_subset"]] == "1"
        if in_sub:
            acc[w]["subset"][band].add(e)
            pooled[w]["subset"].add(e)
            ceiling[w]["n"] += 1
            ceiling[w]["by_band"][band] += 1
            if abs(wrap(e)) < 0.25:
                ceiling[w]["ok"] += 1
                ceiling[w]["by_band_ok"][band] += 1
        if row[ix["kept"]] == "1":
            sg = float(row[ix["sigma_cyc"]])
            sb = sigma_bin(sg)
            fx = int(round(float(row[ix["fixed_int"]])))
            kind = cycle_kind_of(row, i_kind, i_band_only)
            F = fixed[w]
            F["n"] += 1
            F["by_band"][band] += 1
            F["by_sigma"][sb] += 1
            if kind:
                F["by_kind"][kind] += 1
            if fx == dd_true:
                F["correct"] += 1
                F["by_band_ok"][band] += 1
                F["by_sigma_ok"][sb] += 1
                if kind:
                    F["by_kind_ok"][kind] += 1
            else:
                F["wrong"].append({"tow": tow, "band": band, "sat_j": sat_j, "ref_sat": ref_sat,
                                   "sta_r": sta_r, "ref_sta": ref_sta, "fixed": fx,
                                   "true": dd_true, "err_cyc": e, "sigma_cyc": sg,
                                   "sigma_bin": sb, "cycle_kind": kind})
    f.close()

    # Reference identity + switches (per (cons, signal) family, in time order).
    fam = collections.defaultdict(list)
    for tow, cons, sig, rsat, rsta in refs:
        fam[(cons, sig)].append((tow, rsat, rsta))
    ref_stats = {}
    for k2, seq in fam.items():
        seq.sort()
        sw_sat = sum(1 for a, b in zip(seq, seq[1:]) if a[1] != b[1])
        sw_sta = sum(1 for a, b in zip(seq, seq[1:]) if a[2] != b[2])
        ref_stats["%s:%s" % k2] = {
            "epochs": len(seq), "sat_switches": sw_sat, "sta_switches": sw_sta,
            "sats": sorted({s for _, s, _ in seq}), "stas": sorted({s for _, _, s in seq})}

    def p50(v):
        return sorted(v)[len(v) // 2] if v else float("nan")

    res = {"run": run, "fixture": fixture, "settle_h": settle_h, "stride": stride,
           "rows_read": n_rows, "rows_joined": n_join, "rows_unjoinable": n_nojoin,
           "reference": ref_stats, "windup_branch": wb_info, "windows": {}}

    print("=" * 108, file=out)
    print("PRODUCER-AR PHASE-2 TRUTH GRADE  run=%s  fixture=%s" % (run, fixture), file=out)
    print("  rows %d joined %d unjoinable %d   settle >= h%.0f   stride %d"
          % (n_rows, n_join, n_nojoin, settle_h, stride), file=out)
    print("=" * 108, file=out)

    if wb_info is None:
        print("\nWIND-UP BRANCH: not applied (--no-windup-branch): graded on the simulator's integers",
              file=out)
    else:
        print("\nWIND-UP BRANCH (card ARL section 4): %d producer arcs, %d on a shifted branch, "
              "%d without wind-up truth at their start"
              % (wb_info["arcs"], wb_info["arcs_shifted"], wb_info["arcs_no_truth"]), file=out)
        for x in wb_info["shifted"][:20]:
            print("  %s/%s  arc %.0f-%.0f  wu_sim at start %+.3f cyc  k %+d"
                  % (x["sta"], x["sat"], x["start_tow"], x["end_tow"], x["wu_sim_start"], x["k"]),
                  file=out)

    print("\nREFERENCE IDENTITY (every DD is differenced against this pair -- SPEC_amb_iono 6.12)",
          file=out)
    for k2 in sorted(ref_stats):
        r = ref_stats[k2]
        print("  %-10s epochs %5d   ref-sat switches %3d %-28s   ref-sta switches %3d (%d distinct)"
              % (k2, r["epochs"], r["sat_switches"], ",".join(r["sats"][:6]),
                 r["sta_switches"], len(r["stas"])), file=out)

    for w in W:
        E = ep[w]
        n_ep = len(E)
        n_fix_ep = sum(1 for v in E.values() if v[4] > 0)
        F = fixed[w]
        cw = 100.0 * F["correct"] / F["n"] if F["n"] else float("nan")
        cl = 100.0 * ceiling[w]["ok"] / ceiling[w]["n"] if ceiling[w]["n"] else float("nan")
        win = {
            "epochs": n_ep, "fix_epochs": n_fix_ep,
            "fix_epoch_pct": 100.0 * n_fix_ep / n_ep if n_ep else float("nan"),
            "n_dd_p50": p50([v[2] for v in E.values()]),
            "n_search_p50": p50([v[3] for v in E.values()]),
            "ratio_p50": p50([v[0] for v in E.values()]),
            "ratio_p50_fix_ep": p50([v[0] for v in E.values() if v[4] > 0]),
            "p_succ_p50": p50([v[1] for v in E.values()]),
            "p_succ_p50_fix_ep": p50([v[1] for v in E.values() if v[4] > 0]),
            "kept_dd": F["n"], "correct_fix": F["correct"],
            "correct_fix_pct": cw, "wrong_fix": len(F["wrong"]),
            "wrong_fixes": F["wrong"][:200],
            "ceiling_n": ceiling[w]["n"], "ceiling_pct": cl,
            "float_all": pooled[w]["all"].summary(),
            "float_subset": pooled[w]["subset"].summary(),
            "float_all_by_band": {b: a.summary() for b, a in sorted(acc[w]["all"].items())},
            "float_subset_by_band": {b: a.summary() for b, a in sorted(acc[w]["subset"].items())},
            "correct_by_band": {b: {"kept": F["by_band"][b], "correct": F["by_band_ok"][b]}
                                for b in sorted(F["by_band"])},
            "correct_by_kind": {k: {"kept": F["by_kind"][k], "correct": F["by_kind_ok"][k],
                                    "wrong": F["by_kind"][k] - F["by_kind_ok"][k]}
                                for k in sorted(F["by_kind"])},
            "correct_by_sigma": {b: {"kept": F["by_sigma"][b], "correct": F["by_sigma_ok"][b]}
                                 for b in sorted(F["by_sigma"])},
        }
        res["windows"][w] = win

        print("\n---- %s window: %d epochs, %d with a fix (%.1f%%), n_dd p50 %.0f / searched p50 %.0f"
              % (w, n_ep, n_fix_ep, win["fix_epoch_pct"], win["n_dd_p50"], win["n_search_p50"]),
              file=out)
        print("  ratio p50 %.2f (fix epochs %.2f)   p_succ p50 %.4f (fix epochs %.4f)"
              % (win["ratio_p50"], win["ratio_p50_fix_ep"],
                 win["p_succ_p50"], win["p_succ_p50_fix_ep"]), file=out)
        print("  FLOAT DD error vs truth:", file=out)
        print(fmt_acc("all DDs", win["float_all"]), file=out)
        print(fmt_acc("searched subset", win["float_subset"]), file=out)
        for b in sorted(win["float_all_by_band"]):
            print(fmt_acc("  all  " + b, win["float_all_by_band"][b]), file=out)
        for b in sorted(win["float_subset_by_band"]):
            print(fmt_acc("  sub  " + b, win["float_subset_by_band"][b]), file=out)
        print("  FIXABILITY CEILING (searched DDs with |e| < 0.25 -- rounding would be right): "
              "%d/%d = %.1f%%" % (ceiling[w]["ok"], ceiling[w]["n"], cl), file=out)
        for b in sorted(ceiling[w]["by_band"]):
            nb = ceiling[w]["by_band"][b]
            print("    %-12s %6d  ceiling %5.1f%%"
                  % (b, nb, 100.0 * ceiling[w]["by_band_ok"][b] / nb), file=out)
        print("  CORRECT-FIX RATE among kept: %d/%d = %s"
              % (F["correct"], F["n"], "%.2f%%" % cw if F["n"] else "n/a"), file=out)
        for b in sorted(F["by_band"]):
            print("    %-12s kept %6d  correct %6d  %5.1f%%"
                  % (b, F["by_band"][b], F["by_band_ok"][b],
                     100.0 * F["by_band_ok"][b] / F["by_band"][b]), file=out)
        for k in sorted(F["by_kind"]):
            print("    kind %-20s kept %6d  correct %6d  wrong %4d"
                  % (k, F["by_kind"][k], F["by_kind_ok"][k], F["by_kind"][k] - F["by_kind_ok"][k]),
                  file=out)
        for b in sorted(F["by_sigma"]):
            print("    sigma %-10s kept %6d  correct %6d  %5.1f%%"
                  % (b, F["by_sigma"][b], F["by_sigma_ok"][b],
                     100.0 * F["by_sigma_ok"][b] / F["by_sigma"][b]), file=out)
        if F["wrong"]:
            print("  WRONG FIXES (%d) -- each falsifies the covariance as a gate input:"
                  % len(F["wrong"]), file=out)
            for x in sorted(F["wrong"], key=lambda d: -abs(d["err_cyc"]))[:20]:
                print("    tow %9.1f %-10s %s/%s vs %s/%s  fixed %+d true %+d  e %+8.3f cyc "
                      "sigma %.4f (%s)"
                      % (x["tow"], x["band"], x["sta_r"], x["sat_j"], x["ref_sta"], x["ref_sat"],
                         x["fixed"], x["true"], x["err_cyc"], x["sigma_cyc"], x["sigma_bin"]),
                      file=out)
        else:
            print("  WRONG FIXES: none", file=out)

    return res


# ===========================================================================
# Selftest: a fabricated trace + truth tree with a known answer.
# ===========================================================================
def selftest():
    tmp = tempfile.mkdtemp(prefix="netar_grade_")
    fx = os.path.join(tmp, "fixture")
    run = os.path.join(tmp, "run")
    os.makedirs(os.path.join(run, "logs"))
    # Two stations, one signal, three satellites.  Truth integers chosen so the DDs are
    # non-trivial: DD_true(S02,G03) = (N_S02_G03 - N_S02_G01) - (N_S01_G03 - N_S01_G01).
    N = {("S01", "G01"): 10, ("S01", "G02"): -5, ("S01", "G03"): 100,
         ("S02", "G01"): 7, ("S02", "G02"): 3, ("S02", "G03"): 90}
    for sta in ("S01", "S02"):
        os.makedirs(os.path.join(fx, sta))
        with open(os.path.join(fx, sta, "truth_ambiguities.csv"), "w", newline="") as f:
            f.write("satellite,signal,N_int,wavelength_m\n")
            for (s, sat), v in sorted(N.items()):
                if s == sta:
                    f.write("%s,L1C,%d,0.190293673\n" % (sat, v))

    def dd_true(sat):
        return ((N[("S02", sat)] - N[("S02", "G01")]) - (N[("S01", sat)] - N[("S01", "G01")]))

    # One epoch, two DDs (G02 and G03 at station S02 against ref sta S01 / ref sat G01).
    # G02: float sits 0.04 cyc off truth, is searched, is kept, fixed correctly.
    # G03: float sits 0.60 cyc off truth, is searched, is kept, fixed wrong by +1.
    rows = [("G02", dd_true("G02") + 0.04, 0.02, 1, dd_true("G02"), 1),
            ("G03", dd_true("G03") + 0.60, 0.20, 1, dd_true("G03") + 1, 1)]
    with open(os.path.join(run, "logs", "network_producer_ar.csv"), "w", newline="") as f:
        f.write("week,tow,cons,signal,sat_j,ref_sat,sta_r,ref_sta,lambda_m,float_cyc,"
                "sigma_cyc,in_subset,fixed_int,kept,ratio,p_succ,n_dd,n_search,n_fixed\n")
        for sat, fl, sg, insub, fx_int, kept in rows:
            f.write("2313,100.0,G,L1C,%s,G01,S02,S01,0.190293673,%.6f,%.6f,%d,%d,%d,"
                    "5.00,0.999900,2,2,2\n" % (sat, fl, sg, insub, fx_int, kept))

    buf = io.StringIO()
    r = grade(fx, run, settle_h=0.0, out=buf, windup_branch=False)
    w = r["windows"]["settled"]
    ok = True

    def chk(cond, msg):
        nonlocal ok
        if not cond:
            ok = False
            print("FAIL: " + msg)

    chk(r["rows_joined"] == 2 and r["rows_unjoinable"] == 0, "join count")
    chk(w["kept_dd"] == 2, "kept count")
    chk(w["correct_fix"] == 1, "correct-fix count (G02 only)")
    chk(w["wrong_fix"] == 1, "wrong-fix count (G03)")
    chk(abs(w["correct_fix_pct"] - 50.0) < 1e-9, "correct-fix rate 50%%")
    wf = w["wrong_fixes"][0]
    chk(wf["sat_j"] == "G03" and wf["fixed"] == dd_true("G03") + 1, "wrong-fix identity")
    chk(abs(wf["err_cyc"] - 0.60) < 1e-6, "wrong-fix |e| reported")
    # Ceiling: G02 (|wrap(0.04)| = 0.04) counts, G03 (|wrap(0.60)| = 0.40) does not.
    chk(w["ceiling_n"] == 2 and abs(w["ceiling_pct"] - 50.0) < 1e-9, "fixability ceiling 50%%")
    # Wrapping: 0.60 wraps to -0.40, so |frac| p50 over {0.04, 0.40} is 0.40 (upper median).
    chk(abs(w["float_subset"]["wrap_abs_p50"] - 0.40) < 1e-6, "wrapped |frac| p50")
    chk(abs(w["float_subset"]["unwrapped_std"] - 0.28) < 1e-6, "unwrapped std")
    chk(w["fix_epochs"] == 1 and w["epochs"] == 1, "epoch counters")
    chk(r["reference"]["G:L1C"]["sat_switches"] == 0, "reference switch count")

    # Wind-up branch: S02/G03's arc starts where the simulator's wind-up is -0.501 cycle, so the
    # producer's integer is N_sim - 1; a float fixed one below the simulator's DD is then correct.
    # G02's arc starts at +0.499 (k 0).  Rewrite the trace with G03 fixed at dd_true - 1.
    with open(os.path.join(run, "logs", "network_slip_raw.csv"), "w", newline="") as f:
        f.write("# test\nweek,tow,station,sat,signal\n")
        for sta in ("S01", "S02"):
            for sat in ("G01", "G02", "G03"):
                f.write("2313,100.0,%s,%s,L1C\n" % (sta, sat))
    for sta in ("S01", "S02"):
        with open(os.path.join(fx, sta, "truth_windup.csv"), "w", newline="") as f:
            f.write("week,tow_s,satellite,windup_cycles\n")
            for sat in ("G01", "G02", "G03"):
                v = -0.501 if (sta, sat) == ("S02", "G03") else (0.499 if sat == "G02" else 0.1)
                f.write("2313,100.0,%s,%.3f\n" % (sat, v))
    rows2 = [("G02", dd_true("G02") + 0.04, 0.010, 1, dd_true("G02"), 1),
             ("G03", dd_true("G03") - 1 + 0.02, 0.012, 1, dd_true("G03") - 1, 1)]
    with open(os.path.join(run, "logs", "network_producer_ar.csv"), "w", newline="") as f:
        f.write("week,tow,cons,signal,sat_j,ref_sat,sta_r,ref_sta,lambda_m,float_cyc,"
                "sigma_cyc,in_subset,fixed_int,kept,ratio,p_succ,n_dd,n_search,n_fixed\n")
        for sat, fl, sg, insub, fx_int, kept in rows2:
            f.write("2313,100.0,G,L1C,%s,G01,S02,S01,0.190293673,%.6f,%.6f,%d,%d,%d,"
                    "5.00,0.999900,2,2,2\n" % (sat, fl, sg, insub, fx_int, kept))
    r2 = grade(fx, run, settle_h=0.0, out=io.StringIO())
    w2 = r2["windows"]["settled"]
    chk(r2["windup_branch"]["arcs"] == 6 and r2["windup_branch"]["arcs_shifted"] == 1,
        "wind-up branch: one shifted arc of six")
    chk(w2["correct_fix"] == 2 and w2["wrong_fix"] == 0, "wind-up branch: both fixes correct")
    r3 = grade(fx, run, settle_h=0.0, out=io.StringIO(), windup_branch=False)
    chk(r3["windows"]["settled"]["wrong_fix"] == 1, "without the branch: G03 graded wrong")
    # The cycle basis's columns (card AGT): the fixes split by kind.  G02 a partner DD fixed
    # right, G03 a borrowed band-only DD fixed wrong.
    with open(os.path.join(run, "logs", "network_producer_ar.csv"), "w", newline="") as f:
        f.write("week,tow,cons,signal,sat_j,ref_sat,sta_r,ref_sta,lambda_m,float_cyc,"
                "sigma_cyc,in_subset,fixed_int,kept,ratio,p_succ,n_dd,n_search,n_fixed,"
                "cycle_kind,band_only,basis_age_ep,map_state\n")
        for sat, fl, sg, insub, fx_int, kept, kind, bo in (
                ("G02", dd_true("G02") + 0.04, 0.02, 1, dd_true("G02"), 1, "partner", 0),
                ("G03", dd_true("G03") + 0.60, 0.20, 1, dd_true("G03") + 1, 1, "borrowed", 1)):
            f.write("2313,100.0,G,L1C,%s,G01,S02,S01,0.190293673,%.6f,%.6f,%d,%d,%d,"
                    "5.00,0.999900,2,2,2,%s,%d,3,same\n" % (sat, fl, sg, insub, fx_int, kept, kind, bo))
    r4 = grade(fx, run, settle_h=0.0, out=io.StringIO(), windup_branch=False)
    k4 = r4["windows"]["settled"]["correct_by_kind"]
    chk(k4.get("partner") == {"kept": 1, "correct": 1, "wrong": 0}, "kind split: partner")
    chk(k4.get("borrowed/band_only") == {"kept": 1, "correct": 0, "wrong": 1},
        "kind split: borrowed band-only")
    chk(r4["windows"]["settled"]["wrong_fixes"][0]["cycle_kind"] == "borrowed/band_only",
        "kind on the wrong fix")
    print("selftest " + ("PASS" if ok else "FAILED"))
    return 0 if ok else 1


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("fixture_dir", nargs="?", help="simulator truth tree (<fixture>/<sta>/truth_*.csv)")
    ap.add_argument("--run", help="run directory holding logs/network_producer_ar.csv")
    ap.add_argument("--settle-h", type=float, default=12.0,
                    help="hours from the first traced epoch before the SETTLED window starts")
    ap.add_argument("--stride", type=int, default=1, help="grade every Nth traced epoch")
    ap.add_argument("--json", action="store_true", help="dump the full result as JSON to stdout")
    ap.add_argument("--no-windup-branch", action="store_true",
                    help="grade against the simulator's raw integers (no per-arc wind-up branch)")
    ap.add_argument("--selftest", action="store_true")
    a = ap.parse_args()
    if a.selftest:
        return selftest()
    if not a.fixture_dir or not a.run:
        ap.error("fixture_dir and --run are required (or --selftest)")
    out = io.StringIO() if a.json else sys.stdout
    r = grade(a.fixture_dir, a.run, settle_h=a.settle_h, stride=max(1, a.stride), out=out,
              windup_branch=not a.no_windup_branch)
    if a.json:
        print(json.dumps(r, indent=1, default=str))
    return 0


if __name__ == "__main__":
    sys.exit(main())
