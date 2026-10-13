#!/usr/bin/env python3
"""Truth grade of the fixed-cycle fold (card FCF, docs/refound/FCF_fixed_cycle_fold_2026-10-11.md).

Two reads, on sim fixtures only:

  units  <run> [--fixture F] [--settle-h 1]
      Joins logs/network_fixed_cycles.csv (one row per unit-epoch: a DD the search determined in
      both bands, with its integers, floats, unit statistic and fate) to the simulator's truth,
      both bands, through the truth join and the wind-up branch of
      scripts/simnet_producer_ar_grade.py (imported, not copied).  Reports, for all epochs and
      for the settled window: unit-epochs by fate; WRONG unit-epochs by fate (a wrong FOLDED
      unit-epoch is the gate: 0); the per-unit test on correct units (T's mean against its
      chi-square 2-dof mean of 2, the share above the bar against its alpha); the wrong units it
      caught; block acceptance against the kernel's stated mean (chi2 / chi2_expect p50 and
      p95, chi2_expect / K), one figure per (epoch, block) that folded.

  d1  --emit A --observe B [--label L]
      Decision D1's pair read (card FCF section 6): the closed-loop consumer's hourly 3D p50
      (logs/closed_loop_trace.csv) of the Emit arm against the Observe arm, the change in
      percent per hour; card PBH's Read 3 on each arm's products.txt (the ionosphere-free
      level, de-commoned per epoch by the difference median, de-meaned per arc, within-arc RMS
      p50 of the phase-visible correction e_phi, the clock e_clk and the phase bias e_pb, all
      wire satellites and the consumer-tracked ones, by hour), with the loaders and the arc RMS
      of scripts/ftc_step_attrib.py imported; and the PB_HOLD generation starts per hour
      (logs/network_events.csv).

  lattice <run> [--fixture F] [--settle-h 1] [--window-s 60]
      Card FCF step FCF-3's read of a source-lattice Observe run (card FCFC section 2): wrong
      folded unit-epochs; the ledger's releases (logs/network_ar_lattice_events.csv) by cause
      with their dimensions, split into releases on a line of sight whose true integer moved in
      the window before (a scripted slip, or a wind-up branch change) and clean ones (the
      clean-arc bar counts every cause but vanished, first_seen and seam_gap); the latency of
      the first release after each truth slip; each wrong unit's run (how long it was held and
      folded, its statistic against the bar, its float error, its release, the epochs the search
      itself determined the wrong pair and the epochs the ledger held it alone, and whether the
      run was cut off by the end of the run); folded unit-epochs the search did not determine;
      coverage by hour
      (folded units, implied units, lattice dimension, basis rows per band, the search's fixed
      pairs from network_producer_ar.csv); the fold's edges; a planted integer
      (fixed_cycle_plant, the INJECT row of network_events.csv) and the releases within two
      epochs of it.

Usage:  python scripts/simnet_fixed_cycle_grade.py units runs/_rap_fcf2_r3c_obs [--json]
        python scripts/simnet_fixed_cycle_grade.py lattice runs/_rap_fcf3_r3c_obs [--json]
        python scripts/simnet_fixed_cycle_grade.py d1 --emit runs/X --observe runs/Y
        python scripts/simnet_fixed_cycle_grade.py --selftest
"""
import argparse
import collections
import csv
import io
import json
import math
import os
import re
import statistics as st
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from simnet_amb_integer import load_truth, n_at  # noqa: E402
from simnet_producer_ar_grade import load_windup_branches, branch_at  # noqa: E402

TRACE_REL = os.path.join("logs", "network_fixed_cycles.csv")
UNIT_BAR_1E6 = -2.0 * math.log(1e-6)


def fixture_of(run):
    """The sim fixture directory, from the station RINEX paths in the run's arm.json."""
    ap = os.path.join(run, "arm.json")
    txt = open(ap, encoding="utf-8", errors="replace").read()
    for m in re.finditer(r'"rinex_path"\s*:\s*"([^"]+)"', txt):
        sta_dir = os.path.dirname(m.group(1).replace("/", os.sep))
        if os.path.isfile(os.path.join(sta_dir, "truth_ambiguities.csv")):
            return os.path.dirname(sta_dir)
    raise SystemExit("no sim fixture reachable from " + ap)


def q(v, p):
    if not v:
        return float("nan")
    s = sorted(v)
    return s[min(len(s) - 1, int(p * len(s)))]


def grade_units(run, fixture=None, settle_h=1.0, out=sys.stdout):
    fixture = fixture or fixture_of(run)
    stations = sorted(d for d in os.listdir(fixture)
                      if os.path.isfile(os.path.join(fixture, d, "truth_ambiguities.csv")))
    n_int, slips = load_truth(fixture, stations)
    branches, wb = load_windup_branches(fixture, run, stations)
    path = os.path.join(run, TRACE_REL)
    if not os.path.isfile(path):
        raise SystemExit("missing " + path + " (arm network.ambiguity_resolution.fixed_cycles.trace)")
    W = ("all", "settled")
    fate = {w: collections.Counter() for w in W}
    wrong = {w: collections.Counter() for w in W}
    t_ok = {w: [] for w in W}
    t_wrong = {w: [] for w in W}
    blocks = {w: {} for w in W}            # (tow, block) -> (k, chi2, expect), folded blocks
    first = {w: {} for w in W}             # (tow, block) -> (k, chi2, expect), every block at pass 0
    epochs = {w: set() for w in W}
    folded_ep = {w: collections.Counter() for w in W}
    wrong_rows = []
    n_rows = n_nojoin = 0
    tow0 = None
    with io.open(path, newline="", encoding="utf-8", errors="replace") as f:
        rd = csv.DictReader(f)
        for r in rd:
            tow = float(r["tow"])
            if tow0 is None:
                tow0 = tow
            n_rows += 1
            ws = ("all", "settled") if tow - tow0 >= settle_h * 3600.0 else ("all",)
            sta_r, ref_sta, sat_j, ref_sat = r["sta_r"], r["ref_sta"], r["sat_j"], r["ref_sat"]
            ok = True
            truth = []
            for sig in (r["sig1"], r["sig2"]):
                ns = [n_at(n_int, slips, s, j, sig, tow) for s, j in
                      ((sta_r, sat_j), (sta_r, ref_sat), (ref_sta, sat_j), (ref_sta, ref_sat))]
                if None in ns:
                    ok = False
                    break
                ks = [branch_at(branches, s, j, tow) for s, j in
                      ((sta_r, sat_j), (sta_r, ref_sat), (ref_sta, sat_j), (ref_sta, ref_sat))]
                ns = [a + b for a, b in zip(ns, ks)]
                truth.append((ns[0] - ns[1]) - (ns[2] - ns[3]))
            if not ok:
                n_nojoin += 1
                continue
            n1, n2 = int(r["n1"]), int(r["n2"])
            right = (n1 == truth[0] and n2 == truth[1])
            act = r["action"]
            t = float(r["t_unit"])
            for w in ws:
                epochs[w].add(tow)
                fate[w][act] += 1
                if act == "folded":
                    folded_ep[w][tow] += 1
                if right:
                    t_ok[w].append(t)
                else:
                    wrong[w][act] += 1
                    t_wrong[w].append(t)
                fig = (int(r["block_k"]), float(r["block_chi2"]), float(r["block_chi2_expect"]))
                if act == "folded" and int(r["block"]) >= 0:
                    blocks[w][(tow, int(r["block"]))] = fig
                # Every block of the first pass, whatever became of it: the unbiased calibration
                # sample (a failing block's dropped unit carries its pass-0 figures).
                if int(r["pass"]) == 0 and int(r["block"]) >= 0 and fig[0] > 0:
                    first[w][(tow, int(r["block"]))] = fig
            if not right and len(wrong_rows) < 50:
                wrong_rows.append({"tow": tow, "sta_r": sta_r, "ref_sta": ref_sta, "sat_j": sat_j,
                                   "ref_sat": ref_sat, "n": [n1, n2], "true": truth,
                                   "t_unit": t, "action": act})
    res = {"run": run, "fixture": fixture, "settle_h": settle_h, "rows": n_rows,
           "unjoinable": n_nojoin, "windup_arcs_shifted": wb["arcs_shifted"], "windows": {},
           "wrong_rows": wrong_rows}
    print("=" * 100, file=out)
    print("FIXED-CYCLE FOLD TRUTH GRADE  run=%s  fixture=%s" % (run, fixture), file=out)
    print("  rows %d  unjoinable %d  wind-up arcs on a shifted branch %d"
          % (n_rows, n_nojoin, wb["arcs_shifted"]), file=out)
    for w in W:
        bl = list(blocks[w].values())
        ratio = [c / e for (_, c, e) in bl if e > 0]
        per_dof = [e / k for (k, _, e) in bl if k > 0]
        fp = list(first[w].values())
        fp_ratio = [c / e for (_, c, e) in fp if e > 0]
        fp_ratio_big = [c / e for (k, c, e) in fp if e > 0 and k >= 100]
        n_ok = len(t_ok[w])
        above = sum(1 for t in t_ok[w] if not (t <= UNIT_BAR_1E6))
        caught = sum(v for a, v in wrong[w].items() if a == "unit_refused")
        win = {
            "epochs": len(epochs[w]), "unit_epochs": sum(fate[w].values()),
            "by_action": dict(fate[w]), "wrong_by_action": dict(wrong[w]),
            "wrong_folded": wrong[w].get("folded", 0),
            "correct_units": n_ok,
            "correct_t_mean": (sum(t for t in t_ok[w] if math.isfinite(t)) / n_ok) if n_ok else float("nan"),
            "correct_t_p50": q(t_ok[w], 0.5), "correct_t_p95": q(t_ok[w], 0.95),
            "correct_above_bar": above,
            "correct_above_rate": above / n_ok if n_ok else float("nan"),
            "wrong_units": len(t_wrong[w]), "wrong_caught_by_unit_test": caught,
            "folded_per_epoch_p50": q(list(folded_ep[w].values()), 0.5),
            "blocks": len(bl), "chi2_over_expect_p50": q(ratio, 0.5),
            "chi2_over_expect_p95": q(ratio, 0.95), "expect_over_k_p50": q(per_dof, 0.5),
            "block_k_p50": q([k for (k, _, _) in bl], 0.5),
            "first_pass_blocks": len(fp), "first_pass_chi2_over_expect_p50": q(fp_ratio, 0.5),
            "first_pass_chi2_over_expect_p95": q(fp_ratio, 0.95),
            "first_pass_big_blocks": len(fp_ratio_big),
            "first_pass_big_chi2_over_expect_p50": q(fp_ratio_big, 0.5),
        }
        res["windows"][w] = win
        print("\n---- %s window: %d epochs, %d unit-epochs" % (w, win["epochs"], win["unit_epochs"]),
              file=out)
        print("  by fate: %s" % ", ".join("%s %d" % kv for kv in sorted(fate[w].items())), file=out)
        print("  WRONG unit-epochs by fate: %s   (wrong FOLDED = %d)"
              % (", ".join("%s %d" % kv for kv in sorted(wrong[w].items())) or "none",
                 win["wrong_folded"]), file=out)
        print("  unit test on correct units: n %d, T mean %.3f (chi-square 2 dof: 2), p50 %.3f, p95 "
              "%.3f; above the 1e-6 bar %d (rate %.2e, nominal 1e-6)"
              % (n_ok, win["correct_t_mean"], win["correct_t_p50"], win["correct_t_p95"], above,
                 win["correct_above_rate"]), file=out)
        print("  wrong units %d, of them refused by the unit test %d" % (win["wrong_units"], caught),
              file=out)
        print("  folded units per epoch p50 %.0f; block acceptance over %d folded (epoch, block): "
              "chi2/chi2_expect p50 %.3f, p95 %.3f; chi2_expect/K p50 %.3f; K p50 %.0f"
              % (win["folded_per_epoch_p50"], len(bl), win["chi2_over_expect_p50"],
                 win["chi2_over_expect_p95"], win["expect_over_k_p50"], win["block_k_p50"]),
              file=out)
        print("  every block at the first pass (the unbiased sample, %d): chi2/chi2_expect p50 %.3f, "
              "p95 %.3f; blocks of K >= 100 (%d): p50 %.3f"
              % (len(fp), win["first_pass_chi2_over_expect_p50"], win["first_pass_chi2_over_expect_p95"],
                 len(fp_ratio_big), win["first_pass_big_chi2_over_expect_p50"]), file=out)
    for x in wrong_rows[:20]:
        print("  WRONG %s" % json.dumps(x), file=out)
    return res


# ---------------------------------------------------------------------------------------- d1
def consumer_hours(run):
    p = os.path.join(run, "logs", "closed_loop_trace.csv")
    hrs = collections.defaultdict(list)
    t0 = None
    with open(p, newline="") as f:
        for r in csv.DictReader(f):
            try:
                tow, e = float(r["tow"]), float(r["pos_err_3d_m"])
            except (ValueError, TypeError, KeyError):
                continue
            if t0 is None:
                t0 = tow
            hrs[int((tow - t0) // 3600.0) + 1].append(e)
    return {h: st.median(v) for h, v in sorted(hrs.items())}


def generation_starts(run):
    p = os.path.join(run, "logs", "network_events.csv")
    by_h = collections.Counter()
    t0 = None
    with open(p, newline="") as f:
        rd = csv.reader(f)
        next(rd, None)
        for r in rd:
            if len(r) < 4:
                continue
            tow = float(r[1])
            if t0 is None:
                t0 = tow
            if r[2] == "PB_HOLD" and r[3] == "gen_start":
                by_h[int((tow - t0) // 3600.0) + 1] += 1
    return dict(sorted(by_h.items()))


def read3(run, systems=("G",)):
    """Card PBH's Read 3 on a run's products.txt, by hour: p50 within-arc RMS (m) of e_phi,
    e_clk and e_pb at the ionosphere-free level, de-commoned per epoch by the difference median,
    for all wire satellites and the consumer-tracked ones."""
    import ftc_step_attrib as ftc
    import numpy as np
    out = {}
    osb, src, note = ftc.load_truth_bias_sim(run)
    if osb is None:
        return {"note": note}
    wclk, wpb = ftc.load_wire_series(run)
    tracked = set()
    sp = os.path.join(run, "logs", "closed_loop_sat_trace.csv")
    csig = set()
    if os.path.exists(sp):
        with open(sp, newline="") as f:
            for r in csv.DictReader(f):
                tracked.add((r["sat"], ftc.snap(float(r["tow"]))))
                csig.add(r["sig"])
    for sy in systems:
        band2, f1, f2 = ftc.R3_PAIRS.get(sy, ftc.R3_PAIRS["G"])
        a = f1 ** 2 / (f1 ** 2 - f2 ** 2)
        b = f2 ** 2 / (f1 ** 2 - f2 ** 2)
        wc = {k: v for k, v in wclk.items() if k[0][:1] == sy}
        wp = {k: v for k, v in wpb.items() if k[0][:1] == sy}
        osb_sigs = {k[1] for k in osb}
        b1s = sorted({s for (_, s, _) in wp if ftc.band_of(s) == 1})
        b2s = sorted({s for (_, s, _) in wp if ftc.band_of(s) == band2})
        s1 = (next((x for x in b1s if x in osb_sigs and x in csig), None)
              or next((x for x in b1s if x in osb_sigs), None))
        s2 = (next((x for x in b2s if x in osb_sigs and x in csig), None)
              or next((x for x in b2s if x in osb_sigs), None))
        if not (s1 and s2):
            out[sy] = {"note": "no common band pair"}
            continue
        by_tow = collections.defaultdict(list)
        for (sat, t) in wc:
            by_tow[t].append(sat)
        tow0 = min(by_tow)
        raws = {k: collections.defaultdict(dict) for k in ("e_clk", "e_pb", "e_phi")}
        for t, sats in by_tow.items():
            for sat in sats:
                p1, p2 = wp.get((sat, s1, t)), wp.get((sat, s2, t))
                o1, o2 = osb.get((sat, s1)), osb.get((sat, s2))
                if None in (p1, p2, o1, o2):
                    continue
                ec = -wc[(sat, t)]
                ep = a * (p1 - o1) - b * (p2 - o2)
                raws["e_clk"][sat][t] = ec
                raws["e_pb"][sat][t] = ep
                raws["e_phi"][sat][t] = ec + ep
        all_t = sorted(by_tow)
        ser = {k: collections.defaultdict(dict) for k in raws}
        ser_c = {k: collections.defaultdict(dict) for k in raws}
        for k in raws:
            cm, prev = 0.0, None
            for t in all_t:
                cur = {sat: raws[k][sat][t] for sat in raws[k] if t in raws[k][sat]}
                if prev is not None:
                    both = [cur[s_] - prev[s_] for s_ in cur if s_ in prev]
                    if both:
                        cm += float(np.median(both))
                for sat, v in cur.items():
                    ser[k][sat][t] = v - cm
                    if (sat, t) in tracked:
                        ser_c[k][sat][t] = v - cm
                prev = cur
        n_h = int((max(all_t) - tow0) // 3600.0) + 1
        res = {"pair": [s1, s2]}
        for lab, S in (("all", ser), ("tracked", ser_c)):
            for h in range(1, n_h + 1):
                for k in ("e_phi", "e_clk", "e_pb"):
                    hs = {sat: {t: v for t, v in byt.items()
                                if int((t - tow0) // 3600.0) + 1 == h} for sat, byt in S[k].items()}
                    rms, _, na, _ = ftc.arc_rms(hs, 1, tow0)
                    res["%s_h%d_%s" % (lab, h, k)] = (float(np.median(rms)) if rms else float("nan"), na)
        out[sy] = res
    return out


def d1(emit, observe, label="", out=sys.stdout):
    ce, co = consumer_hours(emit), consumer_hours(observe)
    ge, go = generation_starts(emit), generation_starts(observe)
    re_, ro = read3(emit), read3(observe)
    res = {"emit": emit, "observe": observe, "consumer_emit": ce, "consumer_observe": co,
           "gen_emit": ge, "gen_observe": go, "read3_emit": re_, "read3_observe": ro}
    print("=" * 100, file=out)
    print("D1 PAIR %s  emit=%s  observe=%s" % (label, emit, observe), file=out)
    print("  consumer 3D p50 by hour (m): emit %s | observe %s" % (
        " ".join("%.3f" % ce[h] for h in sorted(ce)), " ".join("%.3f" % co[h] for h in sorted(co))),
        file=out)
    chg = {h: 100.0 * (ce[h] - co[h]) / co[h] for h in ce if h in co and co[h] > 0}
    res["consumer_change_pct"] = chg
    print("  consumer change emit vs observe by hour (%%, negative = better): %s"
          % " ".join("h%d %+.1f" % (h, chg[h]) for h in sorted(chg)), file=out)
    print("  PB_HOLD generation starts by hour: emit %s | observe %s" % (ge, go), file=out)
    for sy in sorted(set(re_) | set(ro)):
        a, b = re_.get(sy, {}), ro.get(sy, {})
        if not isinstance(a, dict) or "pair" not in a:
            print("  Read 3 %s: %s" % (sy, a), file=out)
            continue
        print("  Read 3 [%s, pair %s] within-arc RMS p50 (m), emit / observe:" % (sy, a["pair"]),
              file=out)
        for lab in ("all", "tracked"):
            for h in range(1, 10):
                row = []
                for k in ("e_phi", "e_clk", "e_pb"):
                    key = "%s_h%d_%s" % (lab, h, k)
                    if key not in a or key not in b:
                        continue
                    (ra, na), (rb, nb) = a[key], b[key]
                    row.append("%s %.4f / %.4f (x%.2f, arcs %d)" % (k, ra, rb, ra / rb if rb > 0 else float("nan"), na))
                if row:
                    print("    %-7s h%d  %s" % (lab, h, "   ".join(row)), file=out)
    return res


# ----------------------------------------------------------------------------------- lattice
LAT_EPOCH_REL = os.path.join("logs", "network_ar_lattice.csv")
LAT_EVENTS_REL = os.path.join("logs", "network_ar_lattice_events.csv")
PAR_REL = os.path.join("logs", "network_producer_ar.csv")
EDGE_REL = os.path.join("logs", "network_fixed_cycle_events.csv")
EVENTS_REL = os.path.join("logs", "network_events.csv")
#: Release causes that are no verdict on an integer: the arc left the pool, came back after a
#: gap, or a loop epoch went without a step (card FCFC section 2's definition).
STRUCTURAL = ("vanished", "first_seen", "seam_gap")
EPOCH_S = 30.0


def truth_events(fixture, run, stations, slips):
    """Per line of sight (station, satellite): the epochs its true integer moved, from the
    simulator's scripted slips (any signal) and the wind-up branch changes between the
    producer's arcs (scripts/simnet_producer_ar_grade.py's branch)."""
    ev = collections.defaultdict(list)
    for (sta, sat, _sig), lst in slips.items():
        for t, d in lst:
            if d:
                ev[(sta, sat)].append(t)
    branches, wb = load_windup_branches(fixture, run, stations)
    for key, arcs in branches.items():
        prev = None
        for t0, _t1, k in arcs:
            if prev is not None and k != prev:
                ev[key].append(t0)
            prev = k
    return ev, branches, wb


def unit_rows(run, n_int, slips, branches):
    """network_fixed_cycles.csv joined to truth: one dict per unit-epoch (unjoinable rows left
    out), with the unit identity, the integers, the truth, the float errors and the action."""
    out = []
    with io.open(os.path.join(run, TRACE_REL), newline="", encoding="utf-8", errors="replace") as f:
        for r in csv.DictReader(f):
            tow = float(r["tow"])
            terms = ((r["sta_r"], r["sat_j"], +1), (r["sta_r"], r["ref_sat"], -1),
                     (r["ref_sta"], r["sat_j"], -1), (r["ref_sta"], r["ref_sat"], +1))
            truth = []
            for sig in (r["sig1"], r["sig2"]):
                v = 0
                for sta, sat, c in terms:
                    n = n_at(n_int, slips, sta, sat, sig, tow)
                    if n is None:
                        v = None
                        break
                    v += c * (n + branch_at(branches, sta, sat, tow))
                if v is None:
                    break
                truth.append(v)
            if len(truth) != 2:
                continue
            n1, n2 = int(r["n1"]), int(r["n2"])
            out.append({"tow": tow, "key": (r["cons"], r["sig1"], r["sig2"], r["sat_j"], r["ref_sat"],
                                            r["sta_r"], r["ref_sta"]),
                        "los": [(s, j) for s, j, _ in terms], "n": (n1, n2), "true": tuple(truth),
                        "right": (n1, n2) == tuple(truth),
                        "e_float": (float(r["y1_cyc"]) - truth[0], float(r["y2_cyc"]) - truth[1]),
                        "t": float(r["t_unit"]), "action": r["action"]})
    return out


def release_rows(run):
    """network_ar_lattice_events.csv's releases: tow, event, cause, rows dropped, the lines of
    sight involved (one for an arc release, the DD's four for a cycle release: `release_cycle`,
    or `release_support` in runs before card FCFC section 8), the band."""
    out = []
    with io.open(os.path.join(run, LAT_EVENTS_REL), newline="", encoding="utf-8", errors="replace") as f:
        for r in csv.DictReader(f):
            ev = r["event"]
            if ev not in ("release", "release_cycle", "release_support", "release_all"):
                continue
            if ev == "release":
                los = [(r["sta_r"], r["sat_j"])]
            elif ev in ("release_cycle", "release_support"):
                los = [(r["sta_r"], r["sat_j"]), (r["sta_r"], r["ref_sat"]), (r["ref_sta"], r["sat_j"]),
                       (r["ref_sta"], r["ref_sat"])]
            else:
                los = []
            out.append({"tow": float(r["tow"]), "event": ev, "cause": r["cause"],
                        "dropped": int(r["rows_dropped"] or 0), "los": los, "signal": r["signal"],
                        "dd": (r["sat_j"], r["ref_sat"], r["sta_r"], r["ref_sta"]),
                        "change": r["change_bits"], "free": r["free_cause"]})
    return out


def search_pairs(run, with_kept=False):
    """Per epoch, from network_producer_ar.csv: DD rows per band (the basis) and the DD
    identities kept in both bands of a pair (the search's fixed pairs); with with_kept, also the
    kept signals per epoch and DD identity (cons, sat_j, ref_sat, sta_r, ref_sta)."""
    rows = collections.defaultdict(collections.Counter)
    kept = collections.defaultdict(lambda: collections.defaultdict(set))
    with io.open(os.path.join(run, PAR_REL), newline="", encoding="utf-8", errors="replace") as f:
        rd = csv.DictReader(f)
        for r in rd:
            tow = float(r["tow"])
            rows[tow][r["signal"]] += 1
            if r["kept"] == "1":
                kept[tow][(r["cons"], r["sat_j"], r["ref_sat"], r["sta_r"], r["ref_sta"])].add(r["signal"])
    pairs = {t: sum(1 for sigs in m.values() if len(sigs) >= 2) for t, m in kept.items()}
    if with_kept:
        return rows, pairs, kept
    return rows, pairs


def search_kept(kept, u):
    """True when the search determined unit-epoch u's pair (both bands) at its epoch."""
    cons, sig1, sig2, sat_j, ref_sat, sta_r, ref_sta = u["key"]
    sigs = kept.get(u["tow"], {}).get((cons, sat_j, ref_sat, sta_r, ref_sta), ())
    return sig1 in sigs and sig2 in sigs


def grade_lattice(run, fixture=None, settle_h=1.0, window_s=60.0, out=sys.stdout):
    """Card FCF step FCF-3's read of a source-lattice Observe run (card FCFC section 2): wrong
    folded unit-epochs; releases by cause with their dimensions, split into releases on lines of
    sight whose true integer moved within window_s before (slipped) and the rest (clean); the
    release latency after each truth slip; each wrong unit's run (how long it was held and
    folded, its statistic against the bar, its release); coverage by hour; the fold's edges and
    a planted integer's release."""
    P = lambda *a: print(*a, file=out)  # noqa: E731
    fixture = fixture or fixture_of(run)
    stations = sorted(d for d in os.listdir(fixture)
                      if os.path.isfile(os.path.join(fixture, d, "truth_ambiguities.csv")))
    n_int, slips = load_truth(fixture, stations)
    tev, branches, wb = truth_events(fixture, run, stations, slips)
    units = unit_rows(run, n_int, slips, branches)
    rels = release_rows(run)
    lat_rows = []
    with io.open(os.path.join(run, LAT_EPOCH_REL), newline="", encoding="utf-8", errors="replace") as f:
        lat_rows = list(csv.DictReader(f))
    tow0 = float(lat_rows[0]["tow"]) if lat_rows else (units[0]["tow"] if units else 0.0)
    tend = float(lat_rows[-1]["tow"]) if lat_rows else 0.0
    hour = lambda t: int((t - tow0) // 3600.0) + 1  # noqa: E731
    settled = lambda t: t - tow0 >= settle_h * 3600.0  # noqa: E731

    def moved(los_list, tow):
        return any(tow - window_s <= t <= tow for los in los_list for t in tev.get(los, ()))

    res = {"run": run, "fixture": fixture}
    P("=" * 100)
    P("FIXED-CYCLE LEDGER GRADE (source lattice)  run=%s  fixture=%s" % (run, fixture))

    # 1. Wrong folded unit-epochs.
    wrong_folded = sum(1 for u in units if not u["right"] and u["action"] == "folded")
    wrong_any = sum(1 for u in units if not u["right"])
    acts = collections.Counter(u["action"] for u in units)
    res.update({"unit_epochs": len(units), "wrong_unit_epochs": wrong_any, "wrong_folded": wrong_folded,
                "by_action": dict(acts)})
    P("  1. units: %d unit-epochs [%s]; WRONG %d, of them FOLDED %d"
      % (len(units), ", ".join("%s %d" % kv for kv in sorted(acts.items())), wrong_any, wrong_folded))

    # 2. Releases by cause, slipped / clean.
    by = collections.defaultdict(lambda: collections.Counter())
    clean_list = []
    for r in rels:
        c = by[r["cause"]]
        c["n"] += 1
        c["dims"] += r["dropped"]
        if r["dropped"] == 0:
            continue
        c["held"] += 1
        if moved(r["los"], r["tow"]):
            c["held_slipped"] += 1
        else:
            c["held_clean"] += 1
            if r["cause"] not in STRUCTURAL:
                clean_list.append(r)
    clean_total = sum(c["held_clean"] for k, c in by.items() if k not in STRUCTURAL)
    clean_by_h = collections.Counter(hour(r["tow"]) for r in clean_list)
    res["releases"] = {k: dict(v) for k, v in by.items()}
    res["clean_releases"] = clean_total
    res["clean_by_hour"] = dict(clean_by_h)
    P("  2. releases by cause: n / found rows / dims lost / on a moved line of sight / clean")
    for k in sorted(by):
        c = by[k]
        P("     %-14s %6d / %5d / %5d / %5d / %5d%s" % (k, c["n"], c["held"], c["dims"], c["held_slipped"],
                                                     c["held_clean"], "  (structural)" if k in STRUCTURAL else ""))
    P("     clean-arc releases (causes other than %s, rows found, no true integer move within %.0f s): "
      "%d [by hour %s]" % ("/".join(STRUCTURAL), window_s, clean_total,
                           " ".join("h%d %d" % kv for kv in sorted(clean_by_h.items()))))
    bits = collections.Counter((r["cause"], (r["free"] if r["cause"] == "freed" else r["change"]) or "-")
                               for r in clean_list)
    if bits:
        P("     clean, by cause and bits (freed: free_cause; stamp: change_bits): %s"
          % ", ".join("%s:%s %d" % (c, b, n) for (c, b), n in sorted(bits.items())))
    res["clean_list"] = [(r["tow"], r["event"], r["cause"], r["signal"], r["dd"], r["dropped"],
                          r["change"], r["free"]) for r in clean_list]
    for x in res["clean_list"][:40]:
        P("       clean: TOW %.1f %s %s %s %s dropped %d bits %s/%s" % x)

    # 3. Latency after each truth slip in the run (scripted slips only).
    held_los = collections.defaultdict(set)   # tow -> lines of sight in folded units
    for u in units:
        if u["action"] == "folded":
            held_los[u["tow"]].update(u["los"])
    lat = []
    for (sta, sat, sig), lst in sorted(slips.items()):
        for t, d in lst:
            if not d or not (tow0 <= t <= tend):
                continue
            first = None
            for r in rels:
                if r["dropped"] > 0 and r["tow"] >= t - 0.5 and (sta, sat) in r["los"]:
                    if first is None or r["tow"] < first["tow"]:
                        first = r
            held = (sta, sat) in held_los.get(t - EPOCH_S, set())
            wrong_after = sum(1 for u in units if u["tow"] >= t - 0.5 and not u["right"]
                              and u["action"] == "folded" and (sta, sat) in u["los"])
            lat.append({"slip": (sta, sat, sig, t, d), "held_before": held,
                        "first_release_tow": first["tow"] if first else None,
                        "latency_ep": (first["tow"] - t) / EPOCH_S if first else None,
                        "cause": first["cause"] if first else None, "wrong_folded_after": wrong_after})
    res["slip_latency"] = lat
    if lat:
        P("  3. truth slips in the run: %d" % len(lat))
        for x in lat:
            P("     %s %s %s TOW %.1f d %+d: held before %s; first release %s (%s epoch(s), %s); wrong "
              "folded unit-epochs through it after %d"
              % (x["slip"][0], x["slip"][1], x["slip"][2], x["slip"][3], x["slip"][4],
                 "yes" if x["held_before"] else "no",
                 "%.1f" % x["first_release_tow"] if x["first_release_tow"] is not None else "none",
                 "%.0f" % x["latency_ep"] if x["latency_ep"] is not None else "-", x["cause"],
                 x["wrong_folded_after"]))

    # 4. Each wrong unit's run, with the epochs the search itself determined it.
    basis_ep, pairs_ep, kept_ep = search_pairs(run, with_kept=True)
    folded_unkept = sum(1 for u in units if u["action"] == "folded" and not search_kept(kept_ep, u))
    res["folded_unkept"] = folded_unkept
    by_key = collections.defaultdict(list)
    for u in units:
        by_key[u["key"]].append(u)
    runs = []
    for key, us in by_key.items():
        us.sort(key=lambda u: u["tow"])
        cur = None
        for u in us + [None]:
            if u is not None and not u["right"] and cur is not None and u["n"] == cur["n"] \
                    and u["tow"] - cur["last"] <= EPOCH_S + 0.5:
                cur["rows"].append(u)
                cur["last"] = u["tow"]
                continue
            if cur is not None:
                runs.append(cur)
                cur = None
            if u is not None and not u["right"]:
                cur = {"key": key, "n": u["n"], "true": u["true"], "first": u["tow"], "last": u["tow"], "rows": [u]}
    wrong_runs = []
    for wr in runs:
        rows = wr["rows"]
        over = [u["tow"] for u in rows if not (u["t"] <= UNIT_BAR_1E6)]
        after = [r for r in rels if wr["last"] - 0.5 <= r["tow"] <= wr["last"] + EPOCH_S + 0.5 and r["dropped"] > 0
                 and any(l in r["los"] for l in rows[0]["los"])]
        kept_rows = [u for u in rows if search_kept(kept_ep, u)]
        wrong_runs.append({"key": wr["key"], "n": wr["n"], "true": wr["true"], "first": wr["first"],
                           "last": wr["last"], "epochs": len(rows),
                           "folded": sum(1 for u in rows if u["action"] == "folded"),
                           "search_kept": len(kept_rows),
                           "last_search_kept": kept_rows[-1]["tow"] if kept_rows else None,
                           "ledger_only_folded": sum(1 for u in rows if u["action"] == "folded"
                                                     and not search_kept(kept_ep, u)),
                           "cut_by_run_end": wr["last"] >= tend - 2 * EPOCH_S - 0.5,
                           "actions": dict(collections.Counter(u["action"] for u in rows)),
                           "t_first": rows[0]["t"], "t_max": max(u["t"] for u in rows),
                           "first_over_bar": over[0] if over else None,
                           "e_float_first": rows[0]["e_float"], "e_float_last": rows[-1]["e_float"],
                           "release": [(r["tow"], r["cause"], r["event"]) for r in after][:4]})
    res["wrong_runs"] = wrong_runs
    P("  4. wrong units: %d run(s); folded unit-epochs whose pair the search did not determine at "
      "that epoch (the ledger alone): %d" % (len(wrong_runs), folded_unkept))
    for w in wrong_runs:
        P("     %s: the search determined the wrong pair at %d of its %d epochs (last at TOW %s); folded "
          "by the ledger alone at %d; the run ends %s"
          % ("/".join(w["key"]), w["search_kept"], w["epochs"],
             "%.1f" % w["last_search_kept"] if w["last_search_kept"] is not None else "never",
             w["ledger_only_folded"],
             "within two epochs of the run's end (censored)" if w["cut_by_run_end"] else "inside the run"))
        P("     %s held %s (true %s) TOW %.1f..%.1f: %d unit-epochs, %d folded [%s]; T %.2f first, max %.2f "
          "(bar %.2f), first over the bar %s; float error first (%.3f, %.3f), last (%.3f, %.3f) cycle; "
          "released %s" % ("/".join(w["key"]), w["n"], w["true"], w["first"], w["last"], w["epochs"],
                           w["folded"], ", ".join("%s %d" % kv for kv in sorted(w["actions"].items())),
                           w["t_first"], w["t_max"], UNIT_BAR_1E6,
                           "%.1f" % w["first_over_bar"] if w["first_over_bar"] is not None else "never",
                           w["e_float_first"][0], w["e_float_first"][1], w["e_float_last"][0],
                           w["e_float_last"][1], w["release"] or "not at the run's end"))

    # 5. Coverage by hour.
    folded_ep = collections.Counter(u["tow"] for u in units if u["action"] == "folded")
    implied_ep = collections.Counter(u["tow"] for u in units)
    dims_ep = {float(r["tow"]): int(r["rows"]) for r in lat_rows}
    cov = {}
    for h in sorted({hour(t) for t in dims_ep}):
        ts = [t for t in dims_ep if hour(t) == h]
        band_rows = collections.defaultdict(list)
        for t in ts:
            for sig, n in basis_ep.get(t, {}).items():
                band_rows[sig].append(n)
        cov[h] = {"folded_p50": q([folded_ep.get(t, 0) for t in ts], 0.5),
                  "implied_p50": q([implied_ep.get(t, 0) for t in ts], 0.5),
                  "lattice_dims_p50": q([dims_ep[t] for t in ts], 0.5),
                  "basis_rows_p50": {s: q(v, 0.5) for s, v in sorted(band_rows.items())},
                  "search_pairs_p50": q([pairs_ep.get(t, 0) for t in ts], 0.5)}
    ts_set = [t for t in dims_ep if settled(t)]
    fold_p50 = q([folded_ep.get(t, 0) for t in ts_set], 0.5)
    pair_p50 = q([pairs_ep.get(t, 0) for t in ts_set], 0.5)
    res["coverage"] = cov
    res["settled_folded_p50"], res["settled_pairs_p50"] = fold_p50, pair_p50
    P("  5. coverage by hour (per-epoch p50): folded units / implied units / lattice dimension / "
      "basis rows per band / the search's fixed pairs")
    for h, c in cov.items():
        P("     h%d  %.0f / %.0f / %.0f / %s / %.0f" % (h, c["folded_p50"], c["implied_p50"],
                                                       c["lattice_dims_p50"],
                                                       " ".join("%s %.0f" % kv for kv in c["basis_rows_p50"].items()),
                                                       c["search_pairs_p50"]))
    P("     settled: folded units p50 %.0f against the search's fixed pairs p50 %.0f (%.1f %%; bar 90 %%)"
      % (fold_p50, pair_p50, 100.0 * fold_p50 / pair_p50 if pair_p50 else float("nan")))

    # 6. The fold's edges and a planted integer.
    if os.path.isfile(os.path.join(run, EDGE_REL)):
        ed = collections.Counter()
        off = collections.Counter()
        with open(os.path.join(run, EDGE_REL), newline="") as f:
            for r in csv.DictReader(f):
                ed[r["event"]] += 1
                if r["event"] == "offset":
                    off[r["cause"]] += 1
        res["edges"] = dict(ed)
        res["offset_causes"] = dict(off)
        P("  6. edges: %s; offsets by cause [%s]" % (", ".join("%s %d" % kv for kv in sorted(ed.items())),
                                                      ", ".join("%s %d" % kv for kv in sorted(off.items()))))
    plants = []
    if os.path.isfile(os.path.join(run, EVENTS_REL)):
        with open(os.path.join(run, EVENTS_REL), newline="") as f:
            rd = csv.reader(f)
            hdr = next(rd, None)
            for r in rd:
                if len(r) > 6 and r[2] == "INJECT" and r[3] == "fixed_cycle_plant":
                    plants.append({"tow": float(r[1]), "row": r})
    res["plants"] = []
    for p in plants:
        row = p["row"]
        sat, sta = row[4], row[5]
        win = [r for r in rels if p["tow"] - 0.5 <= r["tow"] <= p["tow"] + 2 * EPOCH_S + 0.5 and r["dropped"] > 0
               and r["cause"] not in STRUCTURAL]
        on = [r for r in win if (sta, sat) in r["los"]]
        dims = sum(r["dropped"] for r in win)
        first = min(on, key=lambda r: r["tow"]) if on else None
        own = sum(r["dropped"] for r in on)
        x = {"tow": p["tow"], "sta": sta, "sat": sat, "detail": ",".join(row[6:]),
             "released_tow": first["tow"] if first else None, "cause": first["cause"] if first else None,
             "latency_ep": (first["tow"] - p["tow"]) / EPOCH_S if first else None,
             "dims_in_window": dims, "collateral_dims": dims - 1 if first else None,
             "own_dims": own, "own_collateral_dims": own - 1 if first else None,
             "releases": [(r["tow"], r["cause"], r["event"], r["dropped"]) for r in win]}
        res["plants"].append(x)
        P("     plant %s %s at TOW %.1f: released %s (%s, %s epoch(s)); dimensions lost within 2 "
          "epochs %d, collateral %s (as pre-registered: every non-structural release in the window); "
          "by the releases through the planted line of sight %d, collateral %s; releases %s"
          % (sta, sat, p["tow"], "%.1f" % x["released_tow"] if first else "NOT", x["cause"],
             "%.0f" % x["latency_ep"] if first else "-", dims,
             x["collateral_dims"] if first else "-", own, x["own_collateral_dims"] if first else "-",
             x["releases"]))
    return res


def selftest():
    # The wrong-unit verdict needs both bands right; q() is the nearest-rank quantile.
    assert q([3, 1, 2], 0.5) == 2
    assert abs(UNIT_BAR_1E6 - 27.631021115928547) < 1e-9
    assert "seam_gap" in STRUCTURAL and "revalidation" not in STRUCTURAL
    kept = {30.0: {("G", "G02", "G01", "A", "B"): {"L1C", "L2W"}, ("G", "G03", "G01", "A", "B"): {"L1C"}}}
    unit = {"tow": 30.0, "key": ("G", "L1C", "L2W", "G02", "G01", "A", "B")}
    assert search_kept(kept, unit)
    assert not search_kept(kept, dict(unit, key=("G", "L1C", "L2W", "G03", "G01", "A", "B")))
    assert not search_kept(kept, dict(unit, tow=60.0))
    print("selftest OK")
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--selftest", action="store_true")
    sub = ap.add_subparsers(dest="cmd")
    u = sub.add_parser("units")
    u.add_argument("run")
    u.add_argument("--fixture", default=None)
    u.add_argument("--settle-h", type=float, default=1.0)
    u.add_argument("--json", action="store_true")
    d = sub.add_parser("d1")
    d.add_argument("--emit", required=True)
    d.add_argument("--observe", required=True)
    d.add_argument("--label", default="")
    d.add_argument("--json", action="store_true")
    la = sub.add_parser("lattice")
    la.add_argument("run")
    la.add_argument("--fixture", default=None)
    la.add_argument("--settle-h", type=float, default=1.0)
    la.add_argument("--window-s", type=float, default=60.0)
    la.add_argument("--json", action="store_true")
    a = ap.parse_args()
    if a.selftest:
        return selftest()
    if a.cmd == "units":
        r = grade_units(a.run, a.fixture, a.settle_h)
    elif a.cmd == "d1":
        r = d1(a.emit, a.observe, a.label)
    elif a.cmd == "lattice":
        r = grade_lattice(a.run, a.fixture, a.settle_h, a.window_s)
    else:
        ap.error("units, d1 or lattice")
        return 2
    if a.json:
        print(json.dumps(r, default=str))
    return 0


if __name__ == "__main__":
    sys.exit(main())
