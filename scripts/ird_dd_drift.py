#!/usr/bin/env python3
"""Datum-invariant census of the producer's double-difference floats (card IRD, step IRD-2).

A double difference of four carried ambiguities does not change under any S-basis change, so no
choice of phase datum, re-host or re-reference can move it.  This census reads the producer's DD
floats (logs/network_producer_ar.csv, written with network.ambiguity_resolution.trace) and splits
what they do by what happened at the epoch.  Card IRD's static-algebra stop rule reads chord floats
in the chain's frame; this census tells a frame defect (DDs right, chords off) from a defect of the
float solution itself (DDs off).

  steps <run>             epoch-to-epoch steps of every DD key present at consecutive trace epochs,
                          by class: arc_event (one of its four arcs has a birth, drop or erase row
                          in logs/network_amb_state.csv at either epoch), chain_epoch (otherwise, and
                          the epoch has a CHAIN rehost or PHASE_DATUM eq34_* row in
                          logs/network_events.csv) and quiet.  Needs no truth.
  drift <run> <fixture>   the DD error against truth, e = float_cyc - DD_true, by 10 min bin (wrong
                          share |e| > 0.25 cycle and |e| p50), and every good-to-wrong transition
                          (|e| < 0.10 at one trace epoch, > 0.25 at the next, one DD key) with the
                          first coincident cause at the later epoch: amb:<event> on one of its four
                          arcs, t4 (a T4_attrib row on one of its arcs), rehost (a CHAIN re-host row
                          of one of its stations or satellites), reref (a PHASE_DATUM eq34_* row of
                          one of its satellites), slip (a T0_* row on one of its arcs) or none.
                          The cause is read at the transition epoch on the key's own arcs only, so
                          damage an event starts and the solve spreads to other keys over later
                          epochs reads as none.
  reffree <run> <fixture> the same DD errors less, at each epoch and band, an additive fit
                          a(sta_r) + b(sat_j) by median polish: every DD of the trace shares one
                          reference satellite and station, whose arc errors enter every key, and the
                          fit removes them with any other per-receiver or per-satellite term.  By bin:
                          the residual |e| p50 and its share above 0.25 cycle, with the raw wrong
                          share beside it; with --window a:b (TOW) also per epoch.

DD_true = [N(sta_r, sat_j) - N(sta_r, ref_sat)] - [N(ref_sta, sat_j) - N(ref_sta, ref_sat)], N the
simulator's slip-accumulated integer plus the producer's wind-up branch of the arc
(scripts/simnet_producer_ar_grade.py, through ird_frame_census.Truth).  GPS rows only.  Every mode
prints its sample sizes before any reading.

Usage:
    python scripts/ird_dd_drift.py steps <run> [<run> ...] [--json out.json]
    python scripts/ird_dd_drift.py drift <run> <fixture> [--json out.json]
    python scripts/ird_dd_drift.py reffree <run> <fixture> [--bin-min 10] [--window a:b] [--json out.json]
    python scripts/ird_dd_drift.py --selftest
"""
import argparse
import json
import os
import sys
import tempfile
import warnings
from collections import Counter, defaultdict

import numpy as np
import pandas as pd

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ird_frame_census as IFC   # noqa: E402

KEY = ["signal", "sat_j", "ref_sat", "sta_r", "ref_sta"]
GOOD_CYC, WRONG_CYC = 0.10, 0.25
BIN_S = 600.0
POLISH_ITER = 15          # median-polish sweeps of the reference-free fit


# --------------------------------------------------------------------------- #
# readers
# --------------------------------------------------------------------------- #
def read_ar(run):
    p = os.path.join(run, "logs", "network_producer_ar.csv")
    ar = pd.read_csv(p, usecols=["tow", "cons"] + KEY + ["float_cyc"])
    return ar[ar["cons"] == "G"].drop(columns="cons").reset_index(drop=True)


def read_events(run):
    """(arc events {(tow, sta, sat, signal): event}, run events DataFrame, re-host vertices)."""
    lg = os.path.join(run, "logs")
    amb = pd.read_csv(os.path.join(lg, "network_amb_state.csv"), comment="#",
                      usecols=["tow", "station", "sat", "signal", "event"])
    amb["event"] = amb["event"].fillna("")
    arc_ev = {}
    for r in amb[amb["event"] != ""].itertuples(index=False):
        arc_ev.setdefault((round(r.tow, 3), r.station, r.sat, r.signal), r.event)
    ev = pd.read_csv(os.path.join(lg, "network_events.csv"), usecols=["tow", "tier", "event", "sat", "station"])
    ev["sat"] = ev["sat"].fillna("")
    ev["station"] = ev["station"].fillna("")
    rehost = set()
    p = os.path.join(lg, "network_chain_edges.csv")
    if os.path.isfile(p):
        ce = pd.read_csv(p, usecols=["tow", "system", "band", "child", "row"])
        ce = ce[(ce["system"] == "G") & ce["row"].isin(["RehostUncertified", "RehostCertified"])]
        rehost = {(round(t, 3), b, c) for t, b, c in zip(ce["tow"], ce["band"], ce["child"])}
    return arc_ev, ev, rehost


def chain_epochs(ev):
    m = ((ev["tier"] == "CHAIN") & (ev["event"] == "rehost")) | \
        ((ev["tier"] == "PHASE_DATUM") & ev["event"].astype(str).str.startswith("eq34"))
    return set(ev[m]["tow"].round(3))


def consecutive(ar):
    """The rows of ar with prev_i / prev_* columns of the same DD key at the previous trace epoch."""
    epochs = np.sort(ar["tow"].unique())
    idx = {t: i for i, t in enumerate(epochs)}
    ar = ar.assign(i=ar["tow"].map(idx)).sort_values(KEY + ["i"])
    g = ar.groupby(KEY, sort=False)
    ar["prev_i"] = g["i"].shift(1)
    ar["prev_tow"] = g["tow"].shift(1)
    for c in [c for c in ("float_cyc", "abs") if c in ar.columns]:
        ar["prev_" + c] = g[c].shift(1)
    return ar[ar["prev_i"] == ar["i"] - 1].copy()


def arcs_of(r):
    return ((r.sta_r, r.sat_j), (r.sta_r, r.ref_sat), (r.ref_sta, r.sat_j), (r.ref_sta, r.ref_sat))


# --------------------------------------------------------------------------- #
# mode: steps
# --------------------------------------------------------------------------- #
def steps_census(ar, arc_ev, ev):
    ce = chain_epochs(ev)
    st = consecutive(ar)
    st["step"] = st["float_cyc"] - st["prev_float_cyc"]

    def arc_event(r):
        for t in (round(r.tow, 3), round(r.prev_tow, 3)):
            for sta, sat in arcs_of(r):
                if (t, sta, sat, r.signal) in arc_ev:
                    return True
        return False

    flag = np.array([arc_event(r) for r in st.itertuples(index=False)], bool)
    chain = st["tow"].round(3).isin(ce).to_numpy()
    st["cls"] = np.where(flag, "arc_event", np.where(chain, "chain_epoch", "quiet"))
    out = {}
    for c in ("quiet", "chain_epoch", "arc_event"):
        for sig in sorted(st["signal"].unique()):
            a = np.abs(st[(st["cls"] == c) & (st["signal"] == sig)]["step"].to_numpy())
            out["%s %s" % (c, sig)] = summarize(a, steps=True)
    out["chain_epochs"] = len(ce)
    return out


def summarize(a, steps=False):
    a = np.asarray(a, float)
    if a.size == 0:
        return {"n": 0}
    d = {"n": int(a.size), "p50": float(np.median(a)), "p99": float(np.percentile(a, 99)),
         "gt0.10": float(np.mean(a > 0.10)), "gt0.50": float(np.mean(a > 0.50))}
    if steps:
        d["near_nonzero_int"] = float(np.mean((np.abs(a - np.round(a)) <= 0.05) & (np.round(a) != 0)))
    return d


# --------------------------------------------------------------------------- #
# mode: drift
# --------------------------------------------------------------------------- #
def dd_errors(ar, truth):
    cache = {}

    def N(sta, sat, sig, tow):
        k = (sta, sat, sig, tow)
        if k not in cache:
            cache[k] = truth.N(sta, sat, sig, tow)
        return cache[k]

    e = []
    for r in ar.itertuples(index=False):
        n = [N(s, p, r.signal, r.tow) for s, p in arcs_of(r)]
        e.append(np.nan if None in n else r.float_cyc - ((n[0] - n[1]) - (n[2] - n[3])))
    ar = ar.assign(e=np.array(e, float))
    ar = ar[ar["e"].notna()].copy()
    ar["abs"] = ar["e"].abs()
    return ar


def drift_census(ar, truth, arc_ev, ev, rehost):
    ar = dd_errors(ar, truth)
    t0 = ar["tow"].min()
    ar["bin"] = ((ar["tow"] - t0) // BIN_S).astype(int)
    by_bin = {}
    for b, g in ar.groupby("bin"):
        by_bin[int(b)] = {"n": int(len(g)), "wrong": float(np.mean(g["abs"] > WRONG_CYC)),
                          "p50": float(np.median(g["abs"]))}
    t4 = set(zip(ev[ev["tier"] == "T4_attrib"]["tow"].round(3), ev[ev["tier"] == "T4_attrib"]["station"],
                 ev[ev["tier"] == "T4_attrib"]["sat"]))
    s0 = ev[ev["tier"].astype(str).str.startswith("T0_")]
    slip = set(zip(s0["tow"].round(3), s0["station"], s0["sat"]))
    pdm = ev[(ev["tier"] == "PHASE_DATUM") & ev["event"].astype(str).str.startswith("eq34")]
    reref = set(zip(pdm["tow"].round(3), pdm["sat"]))
    st = consecutive(ar)
    tr = st[(st["prev_abs"] < GOOD_CYC) & (st["abs"] > WRONG_CYC)]
    cause, size = Counter(), defaultdict(list)
    for r in tr.itertuples(index=False):
        t = round(r.tow, 3)
        arcs = arcs_of(r)
        c = next(("amb:" + arc_ev[(t, s, p, r.signal)] for s, p in arcs if (t, s, p, r.signal) in arc_ev), None)
        if c is None and any((t, s, p) in t4 for s, p in arcs):
            c = "t4"
        if c is None and any((t, r.signal, v) in rehost for v in (r.sta_r, r.ref_sta, r.sat_j, r.ref_sat)):
            c = "rehost"
        if c is None and any((t, s) in reref for s in (r.sat_j, r.ref_sat)):
            c = "reref"
        if c is None and any((t, s, p) in slip for s, p in arcs):
            c = "slip"
        c = c or "none"
        cause[c] += 1
        size[c].append(r.abs)
    return {"n_rows": int(len(ar)), "all": summarize(ar["abs"]),
            "wrong": float(np.mean(ar["abs"] > WRONG_CYC)) if len(ar) else float("nan"), "by_bin": by_bin,
            "transitions": int(len(tr)), "cause": dict(cause.most_common()),
            "cause_abs_p50": {k: float(np.median(v)) for k, v in size.items()}}


# --------------------------------------------------------------------------- #
# mode: reffree
# --------------------------------------------------------------------------- #
def reffree_residuals(ar):
    """|e| less its epoch's additive fit a(sta_r) + b(sat_j), per band, by median polish.  `ar` carries the
    column e (dd_errors).  An epoch and band with fewer than three stations or satellites is skipped."""
    out = []
    for (tow, sig), g in ar.groupby(["tow", "signal"]):
        piv = g.pivot_table(index="sta_r", columns="sat_j", values="e")
        M = piv.values
        m = ~np.isnan(M)
        if M.shape[0] < 3 or M.shape[1] < 3:
            continue
        a, b = np.zeros(M.shape[0]), np.zeros(M.shape[1])
        with warnings.catch_warnings():
            warnings.simplefilter("ignore", RuntimeWarning)     # an all-empty row or column: its term is 0
            for _ in range(POLISH_ITER):
                a = np.nan_to_num(np.nanmedian(np.where(m, M - b[None, :], np.nan), axis=1))
                b = np.nan_to_num(np.nanmedian(np.where(m, M - a[:, None], np.nan), axis=0))
        res = np.abs(M - a[:, None] - b[None, :])[m]
        out.append(pd.DataFrame({"tow": tow, "signal": sig, "res": res}))
    return pd.concat(out, ignore_index=True) if out else pd.DataFrame(columns=["tow", "signal", "res"])


def reffree_census(ar, truth, bin_s=BIN_S, window=None):
    ar = dd_errors(ar, truth)
    rf = reffree_residuals(ar)
    t0 = ar["tow"].min()
    by_bin = {}
    for b, g in rf.groupby(((rf["tow"] - t0) // bin_s).astype(int)):
        raw = ar[((ar["tow"] - t0) // bin_s).astype(int) == b]
        by_bin[int(b)] = {"n": int(len(g)), "p50": float(np.median(g["res"])),
                          "wrong": float(np.mean(g["res"] > WRONG_CYC)),
                          "raw_wrong": float(np.mean(raw["abs"] > WRONG_CYC))}
    per_epoch = {}
    if window:
        for tow, g in rf[rf["tow"].between(*window)].groupby("tow"):
            raw = ar[ar["tow"] == tow]
            per_epoch[float(tow)] = {"p50": float(np.median(g["res"])), "wrong": float(np.mean(g["res"] > WRONG_CYC)),
                                     "raw_wrong": float(np.mean(raw["abs"] > WRONG_CYC))}
    return {"n_rows": int(len(rf)), "bin_s": bin_s, "p50": float(np.median(rf["res"])) if len(rf) else float("nan"),
            "wrong": float(np.mean(rf["res"] > WRONG_CYC)) if len(rf) else float("nan"),
            "by_bin": by_bin, "per_epoch": per_epoch}


# --------------------------------------------------------------------------- #
# output
# --------------------------------------------------------------------------- #
def print_steps(name, r):
    print("== steps %s  (%d chain epochs)" % (name, r["chain_epochs"]))
    for k, v in r.items():
        if not isinstance(v, dict):
            continue
        if v["n"] == 0:
            print("   %-22s n 0" % k)
        else:
            print("   %-22s n %7d  |step| p50 %.4f p99 %.4f  >0.10 %.4f  >0.50 %.4f  near a nonzero integer %.4f"
                  % (k, v["n"], v["p50"], v["p99"], v["gt0.10"], v["gt0.50"], v["near_nonzero_int"]))


def print_drift(name, r):
    print("== drift %s  (n %d DD rows against truth)" % (name, r["n_rows"]))
    a = r["all"]
    if a["n"]:
        print("   all rows: |e| p50 %.4f p99 %.3f, wrong (|e| > %.2f) %.4f"
              % (a["p50"], a["p99"], WRONG_CYC, r["wrong"]))
    print("   by %d min bin, wrong share / |e| p50:" % int(BIN_S / 60))
    print("     " + "  ".join("%d:%.3f/%.3f" % (k, v["wrong"], v["p50"]) for k, v in sorted(r["by_bin"].items())))
    print("   good-to-wrong transitions n %d, by coincident cause: %s" % (r["transitions"], r["cause"]))
    print("   |e| p50 after the transition by cause: %s" % {k: round(v, 3) for k, v in r["cause_abs_p50"].items()})


def print_reffree(name, r):
    print("== reffree %s  (n %d DD rows less the additive fit)" % (name, r["n_rows"]))
    print("   all rows: residual |e| p50 %.4f, share > %.2f %.4f" % (r["p50"], WRONG_CYC, r["wrong"]))
    print("   by %d min bin, residual |e| p50 / share > %.2f (raw wrong share):" % (int(r["bin_s"] / 60), WRONG_CYC))
    print("     " + "  ".join("%d:%.3f/%.2f(%.2f)" % (k, v["p50"], v["wrong"], v["raw_wrong"])
                              for k, v in sorted(r["by_bin"].items())))
    for tow, v in sorted(r["per_epoch"].items()):
        print("   tow %.1f  residual |e| p50 %.4f  share > %.2f %.3f  raw wrong %.3f" % (tow, v["p50"], WRONG_CYC,
                                                                                    v["wrong"], v["raw_wrong"]))


# --------------------------------------------------------------------------- #
# selftest
# --------------------------------------------------------------------------- #
def selftest():
    fails = []

    def chk(cond, msg):
        if not cond:
            fails.append(msg)

    A, B, C = "S01000SIM", "S02000SIM", "S03000SIM"
    truth = IFC.DictTruth({(A, "G01", "L1C"): 10, (A, "G02", "L1C"): 20, (B, "G01", "L1C"): 11,
                           (B, "G02", "L1C"): 25, (C, "G01", "L1C"): 3, (C, "G02", "L1C"): 9},
                          branch={(B, "G02"): 1})
    # DD (sat_j G02, ref G01, sta_r B, ref A): true (25 + 1 - 11) - (20 - 10) = 5.
    # DD (G02, G01, C, A): true (9 - 3) - (20 - 10) = -4.
    tows = [261030.0 + 30.0 * k for k in range(30)]
    rows = []
    for k, t in enumerate(tows):
        rows.append({"tow": t, "cons": "G", "signal": "L1C", "sat_j": "G02", "ref_sat": "G01", "sta_r": B,
                     "ref_sta": A, "float_cyc": 5.0 + (0.004 * k if k < 20 else 0.4)})   # wrong at k 20
        rows.append({"tow": t, "cons": "G", "signal": "L1C", "sat_j": "G02", "ref_sat": "G01", "sta_r": C,
                     "ref_sta": A, "float_cyc": -4.0 + (0.6 if k >= 25 else 0.0)})        # wrong at k 25
    with tempfile.TemporaryDirectory() as td:
        lg = os.path.join(td, "logs")
        os.makedirs(lg)
        pd.DataFrame(rows).to_csv(os.path.join(lg, "network_producer_ar.csv"), index=False)
        with open(os.path.join(lg, "network_amb_state.csv"), "w") as fh:
            fh.write("# test\nweek,tow,station,sat,signal,event\n")
            fh.write("2313,%.1f,%s,G02,L1C,drop:t4_attrib\n" % (tows[25], C))
        pd.DataFrame([{"tow": tows[20], "tier": "CHAIN", "event": "rehost", "sat": "G02", "station": B}]) \
            .to_csv(os.path.join(lg, "network_events.csv"), index=False)
        pd.DataFrame([{"tow": tows[20], "system": "G", "band": "L1C", "child": B, "row": "RehostUncertified"}]) \
            .to_csv(os.path.join(lg, "network_chain_edges.csv"), index=False)
        ar = read_ar(td)
        arc_ev, ev, rehost = read_events(td)
        s = steps_census(ar, arc_ev, ev)
        chk(s["chain_epochs"] == 1, "steps: one chain epoch")
        # 29 consecutive pairs per key; the C key's pairs touching k 25 (24->25, 25->26) are arc events.
        chk(s["arc_event L1C"]["n"] == 2, "steps: two arc-event steps (got %s)" % s["arc_event L1C"])
        chk(s["chain_epoch L1C"]["n"] == 2, "steps: the two keys' steps into the chain epoch")
        chk(s["quiet L1C"]["n"] == 29 * 2 - 4, "steps: the rest quiet")
        d = drift_census(ar, truth, arc_ev, ev, rehost)
        chk(d["n_rows"] == 60, "drift: 60 rows against truth")
        chk(d["transitions"] == 2, "drift: two good-to-wrong transitions (got %d)" % d["transitions"])
        chk(d["cause"] == {"rehost": 1, "amb:drop:t4_attrib": 1} or d["cause"] == {"amb:drop:t4_attrib": 1, "rehost": 1},
            "drift: causes rehost and amb:drop:t4_attrib (got %s)" % d["cause"])
        chk(d["by_bin"][0]["wrong"] == 0.0, "drift: the first bin is clean")
    # reffree: per-receiver and per-satellite terms (a reference arc's error is one) leave the reading; an
    # interaction confined to one cell stays.
    stas, sats = ["S%02d" % i for i in range(5)], ["G%02d" % j for j in range(5)]
    rows = [{"tow": 1.0, "signal": "L1C", "sta_r": r, "sat_j": s,
             "e": 0.3 * i - 0.2 * j + (0.6 if (i, j) == (2, 3) else 0.0)}
            for i, r in enumerate(stas) for j, s in enumerate(sats)]
    rows += [{"tow": 2.0, "signal": "L1C", "sta_r": "S00", "sat_j": "G00", "e": 0.4}]   # too small to fit
    rf = reffree_residuals(pd.DataFrame(rows))
    chk(len(rf) == 25 and set(rf["tow"]) == {1.0}, "reffree: the 5 x 5 epoch fitted, the 1 x 1 epoch skipped")
    chk(float(np.max(np.delete(rf["res"].values, 2 * 5 + 3))) < 1e-9, "reffree: additive terms removed exactly")
    chk(abs(float(rf["res"].values[2 * 5 + 3]) - 0.6) < 1e-9, "reffree: the one-cell interaction kept (0.6)")
    print("selftest: %d check(s) failed" % len(fails) if fails else "selftest: PASS")
    for f in fails:
        print("  FAIL " + f)
    return 1 if fails else 0


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--selftest", action="store_true")
    sub = ap.add_subparsers(dest="mode")
    p = sub.add_parser("steps")
    p.add_argument("runs", nargs="+")
    p.add_argument("--json", default=None)
    p = sub.add_parser("drift")
    p.add_argument("run")
    p.add_argument("fixture")
    p.add_argument("--json", default=None)
    p = sub.add_parser("reffree")
    p.add_argument("run")
    p.add_argument("fixture")
    p.add_argument("--bin-min", type=float, default=BIN_S / 60.0)
    p.add_argument("--window", default=None, help="a:b, TOW bounds of the per-epoch listing")
    p.add_argument("--json", default=None)
    a = ap.parse_args()
    if a.selftest:
        return selftest()
    if a.mode == "steps":
        res = {}
        for run in a.runs:
            ar = read_ar(run)
            arc_ev, ev, _ = read_events(run)
            res[os.path.basename(run.rstrip("/\\"))] = r = steps_census(ar, arc_ev, ev)
            print_steps(os.path.basename(run.rstrip("/\\")), r)
    elif a.mode == "drift":
        ar = read_ar(a.run)
        arc_ev, ev, rehost = read_events(a.run)
        res = drift_census(ar, IFC.Truth(a.fixture, a.run), arc_ev, ev, rehost)
        print_drift(os.path.basename(a.run.rstrip("/\\")), res)
    elif a.mode == "reffree":
        win = tuple(float(x) for x in a.window.split(":")) if a.window else None
        res = reffree_census(read_ar(a.run), IFC.Truth(a.fixture, a.run), a.bin_min * 60.0, win)
        print_reffree(os.path.basename(a.run.rstrip("/\\")), res)
    else:
        ap.print_help()
        return 2
    if a.json:
        with open(a.json, "w") as fh:
            json.dump(res, fh, indent=1)
    return 0


if __name__ == "__main__":
    sys.exit(main())
