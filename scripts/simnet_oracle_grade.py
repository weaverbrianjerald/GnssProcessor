#!/usr/bin/env python3
"""Oracle slip-response repair-effect census: grades whether the truth-fed repair actuated.

`network.cycle_slip.oracle_relevel` (include/slip/SlipOracle.hpp) answers
every detector actuation on a carried arc by shifting `LocalCarry::value_abs` by
the true integer jump sum d*lambda instead of freeing the arc.  The run-end
counters report how many arcs it touched; they cannot show whether the touch had
the right sign, landed on the right key, or moved the estimate at all.  This
script measures that on the one instrument that carries truth: the chord-median
DD error of the repaired arc, reused unchanged from `scripts/simnet_t4_census.py`
(`scan_dd` + `ArcStat`) so the two censuses share one definition of `e`.

The sign test (the primary result)
----------------------------------
Convention, fixed by `scan_dd`: for a chord (sta_a, sta_b) it forms
`e = dd_cyc - dd_true` and attributes `+e` to sta_a's leg and `-e` to sta_b's, so
`e(S, s, sig, t)` is the signed error of that arc's estimated ambiguity minus its
true integer, in cycles.  A truth slip of delta cycles at time t raises the true
integer by delta while a stiff zero-q carried level does not move, so the
pre-repair error reads

    e(t0 - 30) ~ ambient - delta

and a correct repair (which adds +delta cycles to the carried level) returns it to

    e(t0) ~ ambient,          i.e.  e(t0) - e(t0 - 30) ~ +delta   (slope +1)

A repair applied with the wrong sign lands at e(t0) ~ -2*delta (slope -1,
|e(t0)| ~ 2|delta|); a repair that never reaches the solved state leaves
e(t0) ~ -delta (slope 0, |e(t0)| ~ |delta|).  The three cases are separated both
by the regression of (e(t0) - e(t0-30)) on delta and by the |e(t0)|
distribution against |delta|, so both are reported.

These expectations assume the detector fired at least one epoch after the
slip.  With detection latency 0 the slip lands in the repair epoch, e(t0-30) is
a pre-slip sample, and the slope map shifts by -1 (0 = exact, -1 = inert,
-2 = wrong sign); effect_report() therefore also splits by latency.

  * The slope is reported for the raw pair and for the ref-free (leave-one-out
    per-station-signal median removed, SPEC_amb_iono 6.12) pair.
  * `slope_pre` (the regression of e(t0-30) on delta) is the control: for late
    detections it should be ~ -1, showing that the pre-repair trace carries the
    un-absorbed slip.  If it is ~0 there, the arcs being repaired were not the
    arcs that jumped, and the sign test carries no information.

Identification of the repaired signal
-------------------------------------
`network_events.csv` carries one row per satellite, but the oracle decides per
signal, so the row's `delta=` names the band that won and the row's prefix names
the candidate bands (`L1C;L2W`, or the bare token `LLI`, which names none).  The
signal is recovered by matching the row's delta against the truth table: the
repaired signal is the one whose truth slips inside the oracle's own window
`(t0 - window_s, t0]` sum to exactly that delta.  When no candidate band
matches, the match is retried over every signal.  Rows where no signal or
several signals match are counted and excluded, not guessed.

Usage
-----
    python scripts/simnet_oracle_grade.py <oracle_run> <fixture> \
        [--control <control_run>] [--window-s 90] [--json out.json] [--txt out.txt]
"""

import argparse
import bisect
import csv
import json
import math
import os
import sys
from collections import Counter, defaultdict

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from simnet_t4_census import (           # noqa: E402  (the same machinery, by import)
    ArcStat,
    EPOCH_S,
    build_sig_index,
    load_stations,
    median,
    pct,
    scan_dd,
)

DET_TIERS = ("T0_mw_detect", "T1_if_slip")
T4_TIER = "T4_attrib"
ORACLE_ACTIONS = ("oracle_repair", "oracle_keep")
OFFSETS = (-1, 0, 1, 5)          # in epochs: t0-30, t0, t0+30, t0+150


# --------------------------------------------------------------------------
# small stats
# --------------------------------------------------------------------------
def summ(vals):
    """n / median / p90 / mean of |.|-style magnitude lists (None-safe)."""
    v = [x for x in vals if x is not None]
    if not v:
        return {"n": 0}
    return {"n": len(v), "med": median(v), "p90": pct(v, 0.9), "p10": pct(v, 0.1),
            "mean": sum(v) / len(v), "max": max(v)}


def fmt_summ(s):
    if not s or s.get("n", 0) == 0:
        return "n=0"
    return (f"n={s['n']:<6d} med={s['med']:+8.3f} p10={s['p10']:+8.3f} "
            f"p90={s['p90']:+8.3f} mean={s['mean']:+8.3f} max={s['max']:+9.3f}")


def ols(xs, ys):
    """Least-squares slope/intercept/R^2 of y on x, plus a robust (Theil-Sen-ish)
    median-of-ratios slope that a handful of outliers cannot drag."""
    n = len(xs)
    if n < 3:
        return None
    mx = sum(xs) / n
    my = sum(ys) / n
    sxx = sum((x - mx) ** 2 for x in xs)
    if sxx <= 0:
        return None
    sxy = sum((x - mx) * (y - my) for x, y in zip(xs, ys))
    b = sxy / sxx
    a = my - b * mx
    syy = sum((y - my) ** 2 for y in ys)
    r2 = (sxy * sxy) / (sxx * syy) if syy > 0 else None
    resid = [y - (a + b * x) for x, y in zip(xs, ys)]
    rms = math.sqrt(sum(r * r for r in resid) / n)
    ratios = [y / x for x, y in zip(xs, ys) if abs(x) > 1e-9]
    return {"n": n, "slope": b, "intercept": a, "r2": r2, "resid_rms": rms,
            "slope_median_ratio": median(ratios) if ratios else None}


def fmt_ols(o):
    if o is None:
        return "n<3"
    r2 = "  -  " if o["r2"] is None else f"{o['r2']:.4f}"
    return (f"n={o['n']:<6d} slope={o['slope']:+8.4f}  intercept={o['intercept']:+8.3f}  "
            f"R2={r2}  resid_rms={o['resid_rms']:7.3f}  median(y/x)={o['slope_median_ratio']:+8.4f}"
            if o["slope_median_ratio"] is not None else
            f"n={o['n']:<6d} slope={o['slope']:+8.4f}  intercept={o['intercept']:+8.3f}  R2={r2}")


# --------------------------------------------------------------------------
# inputs
# --------------------------------------------------------------------------
def load_truth_full(fixture_dir, stations):
    """(sta, sat, sig) -> sorted [(s_cont, delta)] and (sta, sat) -> sorted [s_cont].

    `s_cont` is continuous-GPS seconds (week*604800 + tow), the same clock the
    oracle's window arithmetic runs on.
    """
    by_sig = defaultdict(list)
    by_sat = defaultdict(list)
    total = 0
    missing = []
    for sta in sorted(stations):
        path = os.path.join(fixture_dir, sta, "truth_slips.csv")
        if not os.path.isfile(path):
            missing.append(sta)
            continue
        with open(path, newline="") as f:
            for row in csv.DictReader(f):
                s = float(row["week"]) * 604800.0 + float(row["tow_s"])
                d = int(round(float(row["delta_cycles"])))
                by_sig[(sta, row["satellite"], row["signal"])].append((s, d))
                by_sat[(sta, row["satellite"])].append(s)
                total += 1
    for d in (by_sig, by_sat):
        for k in d:
            d[k].sort()
    return dict(by_sig), dict(by_sat), total, missing


def parse_detail(detail):
    """('L1C;L2W;delta=-94;n=1;noise=0.0000') -> (['L1C','L2W'], -94, 1)."""
    parts = detail.split(";")
    sigs, delta, n = [], None, None
    for p in parts:
        if p.startswith("delta="):
            delta = int(round(float(p[6:])))
        elif p.startswith("n="):
            n = int(p[2:])
        elif p.startswith("noise="):
            pass
        elif p and p != "LLI":
            sigs.append(p)
    return sigs, delta, n


def load_events(run_dir, held):
    """Everything this census needs out of network_events.csv."""
    path = os.path.join(run_dir, "logs", "network_events.csv")
    if not os.path.isfile(path):
        raise SystemExit(f"missing {path}")
    repairs, keeps, refloats, t4 = [], [], [], []
    any_qc = defaultdict(list)          # (sta,sat) -> [tow_int] every T0/T1/T4 row
    counts = Counter()
    with open(path, newline="") as f:
        for row in csv.DictReader(f):
            tier = row.get("tier", "")
            if tier != T4_TIER and tier not in DET_TIERS:
                continue
            sat = (row.get("sat") or "").strip()
            sta = (row.get("station") or "").strip()
            if not sat or not sta or sta in held:
                continue
            week = float(row["week"])
            tow = float(row["tow"])
            ti = int(round(tow))
            s_cont = week * 604800.0 + tow
            action = (row.get("action") or "").strip()
            ev = (row.get("event") or "").strip()
            counts[f"{tier}|{ev}|{action}"] += 1
            any_qc[(sta, sat)].append(ti)
            rec = {"ti": ti, "s": s_cont, "sat": sat, "sta": sta, "tier": tier,
                   "event": ev, "detail": row.get("detail", "")}
            if tier == T4_TIER:
                t4.append(rec)
            elif action == "oracle_repair":
                repairs.append(rec)
            elif action == "oracle_keep":
                keeps.append(rec)
            elif action == "refloat":
                refloats.append(rec)
    for k in any_qc:
        any_qc[k].sort()
    for lst in (repairs, keeps, refloats, t4):
        lst.sort(key=lambda r: (r["ti"], r["sta"], r["sat"]))
    return {"repairs": repairs, "keeps": keeps, "refloats": refloats, "t4": t4,
            "any_qc": dict(any_qc), "counts": counts}


# --------------------------------------------------------------------------
# signal identification
# --------------------------------------------------------------------------
ALL_SIGS = ("L1C", "L2W", "L5Q", "L7Q", "L1W", "L5X", "L7X", "L2C", "L6C")


def window_sum(by_sig, sta, sat, sig, t_hi, window_s):
    """Sum of truth deltas on (sta,sat,sig) in (t_hi - window_s, t_hi]."""
    lst = by_sig.get((sta, sat, sig))
    if not lst:
        return 0, 0
    lo = t_hi - window_s
    tot, n = 0, 0
    for s, d in lst:
        if lo < s <= t_hi:
            tot += d
            n += 1
    return tot, n


def identify_signal(rec, by_sig, window_s):
    """Recover the repaired signal by matching the row's delta against truth.

    Returns (sig, status, delta, n) with status in {ok, ok_wide, ambiguous,
    nomatch}: `ok` is a unique match among the row's named bands, `ok_wide` a
    unique match after widening to every signal; sig is None unless status is
    ok or ok_wide.
    """
    sigs, delta, n = parse_detail(rec["detail"])
    sta, sat, t0 = rec["sta"], rec["sat"], rec["s"]
    cands = sigs if sigs else list(ALL_SIGS)
    hits = []
    for sig in cands:
        tot, k = window_sum(by_sig, sta, sat, sig, t0, window_s)
        if k > 0 and tot == delta:
            hits.append(sig)
    if len(hits) == 1:
        return hits[0], "ok", delta, n
    if len(hits) > 1:
        return None, "ambiguous", delta, n
    # widen to every signal (an LLI row names none; a pair row may name the wrong two)
    hits2 = []
    for sig in ALL_SIGS:
        tot, k = window_sum(by_sig, sta, sat, sig, t0, window_s)
        if k > 0 and tot == delta:
            hits2.append(sig)
    if len(hits2) == 1:
        return hits2[0], "ok_wide", delta, n
    if len(hits2) > 1:
        return None, "ambiguous", delta, n
    return None, "nomatch", delta, n


# --------------------------------------------------------------------------
# the repair-effect measurement
# --------------------------------------------------------------------------
def measure(stat, ti, sta, sat, sig, ref_free):
    return stat.value(ti, sta, sat, sig, ref_free)


def next_event_after(any_qc, sta, sat, ti):
    lst = any_qc.get((sta, sat), ())
    if not lst:
        return None
    j = bisect.bisect_right(lst, ti)
    return lst[j] if j < len(lst) else None


def prev_event_before(any_qc, sta, sat, ti):
    lst = any_qc.get((sta, sat), ())
    if not lst:
        return None
    i = bisect.bisect_left(lst, ti)
    return lst[i - 1] if i > 0 else None


def repair_effect(evs, stat, any_qc, by_sig, window_s, ref_free):
    """One record per identified oracle_repair, with e at the four offsets."""
    recs = []
    ident = Counter()
    for rec in evs:
        sig, status, delta, nslip = identify_signal(rec, by_sig, window_s)
        ident[status] += 1
        if sig is None:
            continue
        ti = rec["ti"]
        nxt = next_event_after(any_qc, rec["sta"], rec["sat"], ti)
        prv = prev_event_before(any_qc, rec["sta"], rec["sat"], ti)
        # Detection latency: how far back in the window the repaired jump sits.
        # It determines how e(t0-30) reads: a latency-0 repair has no slip yet
        # at t0-30, so an ambient e(t0-30) is expected and the pre-slope is
        # uninformative.  Only latency >= 1 epoch puts the un-absorbed jump into
        # the pre-repair sample.
        lst = by_sig.get((rec["sta"], rec["sat"], sig), ())
        w = [s for s, d in lst if rec["s"] - window_s < s <= rec["s"]]
        lat = (rec["s"] - max(w)) if w else None
        out = {"ti": ti, "sta": rec["sta"], "sat": rec["sat"], "sig": sig,
               "delta": delta, "n": nslip, "tier": rec["tier"], "event": rec["event"],
               "status": status, "next_dt": (nxt - ti) if nxt else None,
               "lat_s": lat, "s_cont": rec["s"],
               "prev_dt": (ti - prv) if prv else None, "e": {}}
        for k in OFFSETS:
            tk = ti + k * EPOCH_S
            if k > 0 and nxt is not None and tk >= nxt:
                continue                      # censored by the next event on (S,s)
            if k < 0 and prv is not None and tk <= prv:
                continue                      # censored by the previous event
            v = measure(stat, tk, rec["sta"], rec["sat"], sig, ref_free)
            if v is not None:
                out["e"][k] = v
        recs.append(out)
    return recs, ident


def effect_report(recs, label):
    """The sign/slope test + the magnitude distributions."""
    res = {"label": label, "n": len(recs)}
    have = [r for r in recs if -1 in r["e"] and 0 in r["e"]]
    res["n_pair"] = len(have)
    res["abs_delta"] = summ([abs(r["delta"]) for r in recs])
    for k, name in ((-1, "e_pre"), (0, "e_at"), (1, "e_p1"), (5, "e_p5")):
        res[name + "_abs"] = summ([abs(r["e"][k]) for r in recs if k in r["e"]])
        res[name + "_sgn"] = summ([r["e"][k] for r in recs if k in r["e"]])
    # ratio |e| / |delta|: the three hypotheses sit at 0 / 1 / 2
    for k, name in ((-1, "e_pre"), (0, "e_at"), (1, "e_p1"), (5, "e_p5")):
        res[name + "_over_delta"] = summ(
            [abs(r["e"][k]) / abs(r["delta"]) for r in recs
             if k in r["e"] and abs(r["delta"]) > 0])
    # the primary regression: (e(t0) - e(t0-30)) on delta
    xs = [float(r["delta"]) for r in have]
    ys = [r["e"][0] - r["e"][-1] for r in have]
    res["slope_delta_e"] = ols(xs, ys)
    res["slope_pre"] = ols([float(r["delta"]) for r in recs if -1 in r["e"]],
                           [r["e"][-1] for r in recs if -1 in r["e"]])
    res["slope_at"] = ols([float(r["delta"]) for r in recs if 0 in r["e"]],
                          [r["e"][0] for r in recs if 0 in r["e"]])
    res["slope_p1"] = ols([float(r["delta"]) for r in recs if 1 in r["e"]],
                          [r["e"][1] for r in recs if 1 in r["e"]])
    res["slope_p5"] = ols([float(r["delta"]) for r in recs if 5 in r["e"]],
                          [r["e"][5] for r in recs if 5 in r["e"]])
    # Latency split: the pre-sample carries the jump only when the detector was
    # late.  lat = 0 s means the slip landed in this epoch, so e(t0-30) is a
    # pre-slip sample and reads ambient whatever the repair did.
    # Ceiling figure: with latency 0 an exact repair leaves e continuous across
    # its own slip epoch, so |e(t0) - e(t0-30)| / |delta| is the fraction of the
    # true jump the repair failed to remove (1.0 = the level never moved).
    res["unrepaired_frac"] = summ(
        [abs(r["e"][0] - r["e"][-1]) / abs(r["delta"]) for r in have
         if r["lat_s"] == 0 and r["delta"]])
    res["unrepaired_cyc"] = summ(
        [abs(r["e"][0] - r["e"][-1]) for r in have if r["lat_s"] == 0])
    res["latency_hist"] = dict(sorted(Counter(
        ("none" if r["lat_s"] is None else f"{int(r['lat_s'])}s") for r in recs).items()))
    for tag, sel in (("lat0", lambda r: r["lat_s"] == 0),
                     ("lat_ge30", lambda r: r["lat_s"] is not None and r["lat_s"] >= 30)):
        sub = [r for r in recs if sel(r)]
        subp = [r for r in sub if -1 in r["e"] and 0 in r["e"]]
        res[tag] = {
            "n": len(sub),
            "slope_delta_e": ols([float(r["delta"]) for r in subp],
                                 [r["e"][0] - r["e"][-1] for r in subp]),
            "slope_pre": ols([float(r["delta"]) for r in sub if -1 in r["e"]],
                             [r["e"][-1] for r in sub if -1 in r["e"]]),
            "slope_at": ols([float(r["delta"]) for r in sub if 0 in r["e"]],
                            [r["e"][0] for r in sub if 0 in r["e"]]),
            "e_pre_abs": summ([abs(r["e"][-1]) for r in sub if -1 in r["e"]]),
            "e_at_abs": summ([abs(r["e"][0]) for r in sub if 0 in r["e"]]),
            "e_pre_over_d": summ([abs(r["e"][-1]) / abs(r["delta"])
                                  for r in sub if -1 in r["e"] and r["delta"]]),
            "e_at_over_d": summ([abs(r["e"][0]) / abs(r["delta"])
                                 for r in sub if 0 in r["e"] and r["delta"]]),
        }
    # per-tier
    res["by_tier"] = {}
    for tier in sorted({r["tier"] for r in recs}):
        sub = [r for r in recs if r["tier"] == tier]
        subp = [r for r in sub if -1 in r["e"] and 0 in r["e"]]
        res["by_tier"][tier] = {
            "n": len(sub),
            "slope": ols([float(r["delta"]) for r in subp],
                         [r["e"][0] - r["e"][-1] for r in subp]),
            "e_pre_abs": summ([abs(r["e"][-1]) for r in sub if -1 in r["e"]]),
            "e_at_abs": summ([abs(r["e"][0]) for r in sub if 0 in r["e"]]),
        }
    # coverage
    res["coverage"] = {f"k={k}": sum(1 for r in recs if k in r["e"]) for k in OFFSETS}
    return res


# --------------------------------------------------------------------------
# keep classification
# --------------------------------------------------------------------------
def classify_keeps(keeps, repairs, t4, by_sat, by_sig, stat, any_qc, window_s, ref_free):
    """Sort every `oracle_keep` row into the reason it was kept.

    A truth slip on (S,s) inside the window does not by itself mean the oracle
    missed it.  Two large confounders are removed first:

      * the same slip keeps the detector firing for 2-3 epochs, and the oracle's
        `last_applied_s` lower bound correctly refuses the second and third
        actuation (run-end counter: "lower bound set by an earlier repair");
      * a fixture slip lands on a subset of the satellite's signals, while a
        detector fires on the satellite (LLI) or on a band pair, so the arcs that
        did not jump are correctly kept.

    Only what survives both is a real miss, and those are then split by whether a
    T4 erase re-born the carry between the slip and the actuation (the design's
    `carry.birth_s` floor) or not (an unexplained miss).  Keeps with no truth slip
    in the window are classed by whether a slip follows t0, falls within
    +/-600 s, or is absent (a detector false alarm).
    """
    rep_key = {(r["ti"], r["sta"], r["sat"]) for r in repairs}
    rep_idx = defaultdict(list)
    for r in repairs:
        rep_idx[(r["sta"], r["sat"])].append(r["s"])
    for k in rep_idx:
        rep_idx[k].sort()
    t4_idx = defaultdict(list)
    for r in t4:
        t4_idx[(r["sta"], r["sat"])].append(r["s"])
    for k in t4_idx:
        t4_idx[k].sort()

    out = {"n": len(keeps), "partner": 0, "already_repaired": 0, "other_band": 0,
           "miss_t4_reborn": 0, "miss_unexplained": 0, "slip_after_t0": 0,
           "true_slip_600": 0, "false_alarm": 0,
           "by_tier": defaultdict(Counter), "cls": Counter()}
    b_recs = []
    for rec in keeps:
        sta, sat, ti, s0 = rec["sta"], rec["sat"], rec["ti"], rec["s"]
        named, _, _ = parse_detail(rec["detail"])
        lst = by_sat.get((sta, sat), ())
        back = [s for s in lst if s0 - window_s < s <= s0]     # the oracle's own window
        fwd = [s for s in lst if s0 < s <= s0 + window_s]
        near600 = [s for s in lst if abs(s - s0) <= 600.0]
        if (ti, sta, sat) in rep_key:
            cls = "partner"
        elif back:
            # (i) did the oracle already repair this satellite at/after the slip?
            rl = rep_idx.get((sta, sat), ())
            if any(max(back) <= tr <= s0 for tr in rl):
                cls = "already_repaired"
            else:
                # (ii) did the named signals jump inside the window?
                cand = named if named else list(ALL_SIGS)
                jumped = [sg for sg in cand
                          if window_sum(by_sig, sta, sat, sg, s0, window_s)[1] > 0]
                if named and not jumped:
                    cls = "other_band"
                else:
                    tl = t4_idx.get((sta, sat), ())
                    cls = ("miss_t4_reborn"
                           if any(max(back) < te <= s0 for te in tl)
                           else "miss_unexplained")
        elif fwd:
            cls = "slip_after_t0"
        elif near600:
            cls = "true_slip_600"
        else:
            cls = "false_alarm"
        out["cls"][cls] += 1
        out[cls] = out.get(cls, 0) + 1
        out["by_tier"][rec["tier"]][cls] += 1
        if cls in ("miss_t4_reborn", "miss_unexplained", "other_band"):
            b = {"ti": ti, "sta": sta, "sat": sat, "tier": rec["tier"], "cls": cls,
                 "e": {}, "sig_e": {}}
            # The decisive measurement is on the signal that jumped in truth, not
            # on the arc's worst band.  |e| / |delta| ~ 1 there means the jump was
            # never repaired on that band (freed, or missed); ~0 means it was.
            for sg in ALL_SIGS:
                tot, k = window_sum(by_sig, sta, sat, sg, s0, window_s)
                if k > 0 and tot != 0:
                    for koff in (0, 5):
                        tk = ti + koff * EPOCH_S
                        v = measure(stat, tk, sta, sat, sg, ref_free)
                        if v is not None:
                            b["sig_e"].setdefault(koff, []).append(abs(v) / abs(tot))
            sigs = [s for s in ALL_SIGS if stat.raw(ti, sta, sat, s) is not None]
            nxt = next_event_after(any_qc, sta, sat, ti)
            for k in (0, 5):
                tk = ti + k * EPOCH_S
                if k > 0 and nxt is not None and tk >= nxt:
                    continue
                vals = [abs(v) for v in (measure(stat, tk, sta, sat, sg, ref_free)
                                         for sg in sigs) if v is not None]
                if vals:
                    b["e"][k] = max(vals)
            dtot = 0
            for sg in ALL_SIGS:
                for s, d in by_sig.get((sta, sat, sg), ()):
                    if abs(s - s0) <= window_s:
                        dtot = max(dtot, abs(d))
            b["abs_delta"] = dtot
            b_recs.append(b)
    out["by_tier"] = {k: dict(v) for k, v in out["by_tier"].items()}
    out["cls"] = dict(out["cls"])
    for cls in ("miss_t4_reborn", "miss_unexplained", "other_band"):
        sub = [r for r in b_recs if r["cls"] == cls]
        out[cls + "_e_at"] = summ([r["e"][0] for r in sub if 0 in r["e"]])
        out[cls + "_e_p5"] = summ([r["e"][5] for r in sub if 5 in r["e"]])
        out[cls + "_abs_delta"] = summ([r["abs_delta"] for r in sub])
        out[cls + "_sig_ratio_t0"] = summ(
            [x for r in sub for x in r.get("sig_e", {}).get(0, [])])
        out[cls + "_sig_ratio_p5"] = summ(
            [x for r in sub for x in r.get("sig_e", {}).get(5, [])])
    return out


# --------------------------------------------------------------------------
# slip-side census: which injected jumps the ceiling did not repair
# --------------------------------------------------------------------------
def slip_side_census(by_sig, recs, ev, stat, window_s, ref_free):
    """Walk the truth slips, not the events.

    The event-side classification cannot be complete: one row is emitted per
    satellite while the oracle decides per signal, and the row's action is an OR
    across bands, so a band that fell through with no carry (and was therefore
    freed by the standard response) is invisible whenever a sibling band was
    handled.  Walking the truth table gives an exhaustive account of every
    injected jump, independent of the row shape.
    """
    covered = set()
    for r in recs:
        for s, d in by_sig.get((r["sta"], r["sat"], r["sig"]), ()):
            if r["s_cont"] - window_s < s <= r["s_cont"]:
                covered.add((r["sta"], r["sat"], r["sig"], s))
    # every detector row (any action) and every repair row, by (sta,sat)
    det_any, rep_any, refl_any = defaultdict(list), defaultdict(list), defaultdict(list)
    for lst, dst in ((ev["repairs"], rep_any), (ev["keeps"], det_any),
                     (ev["refloats"], refl_any)):
        for r in lst:
            dst[(r["sta"], r["sat"])].append(r["s"])
    for r in ev["repairs"]:
        det_any[(r["sta"], r["sat"])].append(r["s"])
    for r in ev["refloats"]:
        det_any[(r["sta"], r["sat"])].append(r["s"])
    for d in (det_any, rep_any, refl_any):
        for k in d:
            d[k].sort()

    def hit(idx, sta, sat, s):
        return [t for t in idx.get((sta, sat), ()) if s <= t <= s + window_s]

    out = {"n": 0, "cls": Counter(), "e_ratio": defaultdict(list)}
    for (sta, sat, sig), lst in by_sig.items():
        for s, d in lst:
            out["n"] += 1
            if (sta, sat, sig, s) in covered:
                cls = "REPAIRED"
            elif not hit(det_any, sta, sat, s):
                cls = "SILENT (no detector row on (S,s) in the window)"
            elif hit(refl_any, sta, sat, s):
                cls = "FREED (a no-carry `refloat` row in the window)"
            elif hit(rep_any, sta, sat, s):
                cls = "sibling band repaired, THIS band not"
            else:
                cls = "detected, KEPT on every row"
            out["cls"][cls] += 1
            # `s` is continuous-GPS seconds; ArcStat is keyed on the epoch's tow.
            ti = int(round(s)) % 604800
            for koff, tag in ((0, "t0"), (5, "t0+150")):
                v = stat.value(ti + koff * EPOCH_S, sta, sat, sig, ref_free)
                if v is not None and d:
                    out["e_ratio"][cls + "|" + tag].append(abs(v) / abs(d))
    out["cls"] = dict(out["cls"])
    out["e_ratio"] = {k: summ(v) for k, v in sorted(out["e_ratio"].items())}
    return out


# --------------------------------------------------------------------------
# no-carry fall-throughs and the T4 chain
# --------------------------------------------------------------------------
def classify_refloats(refloats, t4, tows):
    """The `refloat` rows on an oracle arm = the no-carry fall-throughs."""
    t4_idx = defaultdict(list)
    for r in t4:
        t4_idx[(r["sta"], r["sat"])].append(r["ti"])
    for k in t4_idx:
        t4_idx[k].sort()
    t_min = tows[0] if tows else 0
    out = {"n": len(refloats), "after_t4_3ep": 0, "by_tier": Counter(),
           "hour": Counter(), "ignition": 0, "settled": 0}
    for r in refloats:
        out["by_tier"][r["tier"] + "|" + r["event"]] += 1
        dt = r["ti"] - t_min
        out["hour"][int(dt // 3600)] += 1
        if dt < 3600:
            out["ignition"] += 1
        else:
            out["settled"] += 1
        lst = t4_idx.get((r["sta"], r["sat"]), ())
        i = bisect.bisect_left(lst, r["ti"])
        if any(0 < r["ti"] - lst[j] <= 3 * EPOCH_S for j in range(max(0, i - 6), i)):
            out["after_t4_3ep"] += 1
    out["by_tier"] = dict(out["by_tier"])
    out["hour"] = dict(sorted(out["hour"].items()))
    return out


def t4_split(t4, repairs, keeps, stat, any_qc, ref_free, label):
    """Split T4 events by what the oracle did to the same (S,s) in the past 10 min,
    and measure |e_pre| (the arc's worst signal at t0-30)."""
    rep_idx, keep_idx = defaultdict(list), defaultdict(list)
    for r in repairs:
        rep_idx[(r["sta"], r["sat"])].append(r["ti"])
    for r in keeps:
        keep_idx[(r["sta"], r["sat"])].append(r["ti"])
    for d in (rep_idx, keep_idx):
        for k in d:
            d[k].sort()

    def within(idx, sta, sat, ti, win):
        lst = idx.get((sta, sat), ())
        i = bisect.bisect_left(lst, ti)
        return any(0 < ti - lst[j] <= win for j in range(max(0, i - 40), i))

    out = {"label": label, "n": len(t4), "cls": Counter(), "e_pre": defaultdict(list)}
    for r in t4:
        sta, sat, ti = r["sta"], r["sat"], r["ti"]
        if within(rep_idx, sta, sat, ti, 600):
            cls = "after_repair<=10min"
        elif within(keep_idx, sta, sat, ti, 600):
            cls = "after_keep<=10min"
        else:
            cls = "neither"
        out["cls"][cls] += 1
        prv = prev_event_before(any_qc, sta, sat, ti)
        tk = ti - EPOCH_S
        if prv is not None and tk <= prv:
            continue
        sigs = [s for s in ALL_SIGS if stat.raw(tk, sta, sat, s) is not None]
        vals = [abs(v) for v in (measure(stat, tk, sta, sat, sg, ref_free)
                                 for sg in sigs) if v is not None]
        if vals:
            out["e_pre"][cls].append(max(vals))
            out["e_pre"]["ALL"].append(max(vals))
    out["cls"] = dict(out["cls"])
    out["e_pre"] = {k: summ(v) for k, v in out["e_pre"].items()}
    return out


# --------------------------------------------------------------------------
# driver
# --------------------------------------------------------------------------
def render(R, out):
    p = lambda *a: print(*a, file=out)
    p("=" * 96)
    p("ORACLE SLIP-RESPONSE REPAIR-EFFECT CENSUS")
    p("=" * 96)
    p(f"  run      : {R['run']}")
    p(f"  control  : {R['control']}")
    p(f"  fixture  : {R['fixture']}   window {R['window_s']:.0f} s   ref_free={R['ref_free']}")
    p(f"  events   : {R['n_repair']} oracle_repair row(s), {R['n_keep']} oracle_keep, "
      f"{R['n_refloat']} refloat (no-carry fall-through), {R['n_t4']} T4_attrib")
    p(f"  truth    : {R['n_truth']} slip row(s)")
    p("")
    p("-- signal identification (delta matched against truth in the oracle's own window) --")
    for k, v in sorted(R["ident"].items(), key=lambda kv: -kv[1]):
        p(f"    {k:12s} {v}")
    p("")
    for tag in ("astraced", "reffree"):
        e = R["effect"][tag]
        p(f"== THE SIGN TEST [{tag}] ==  (e = estimate - truth, cycles; "
          f"a slip of +delta pushes e by -delta)")
        p(f"    identified repairs {e['n']}, with BOTH t0-30 and t0 measurable: {e['n_pair']}")
        p(f"    |delta| applied      {fmt_summ(e['abs_delta'])}")
        p("")
        p(f"    *** slope of [e(t0) - e(t0-30)] on delta : {fmt_ols(e['slope_delta_e'])}")
        p( "        READ IT AGAINST THE LATENCY (below).  With latency 0 the t0-30 sample is")
        p( "        PRE-slip, so the map is:  0 = repair EXACT,  -1 = inert (level never moved),")
        p( "        -2 = wrong sign.  With latency >= 1 epoch the pre-sample already carries the")
        p( "        jump and the map shifts to:  +1 = exact,  0 = inert,  -1 = wrong sign.")
        p(f"    slope of e(t0-30) on delta               : {fmt_ols(e['slope_pre'])}")
        p( "        (~ -1 only when the detector was LATE; ~0 when it fired in the slip epoch)")
        p(f"    slope of e(t0)    on delta               : {fmt_ols(e['slope_at'])}   (want ~  0)")
        p(f"    slope of e(t0+30) on delta               : {fmt_ols(e['slope_p1'])}")
        p(f"    slope of e(t0+150) on delta              : {fmt_ols(e['slope_p5'])}")
        p("")
        p("    magnitudes (cycles)")
        for nm, lbl in (("e_pre", "|e(t0-30)|"), ("e_at", "|e(t0)|  "),
                        ("e_p1", "|e(t0+30)|"), ("e_p5", "|e(t0+150)|")):
            p(f"      {lbl:12s} {fmt_summ(e[nm + '_abs'])}")
        p("    ratio |e| / |delta|   (0 = repaired, 1 = inert, 2 = wrong sign)")
        for nm, lbl in (("e_pre", "t0-30"), ("e_at", "t0   "),
                        ("e_p1", "t0+30"), ("e_p5", "t0+150")):
            p(f"      {lbl:12s} {fmt_summ(e[nm + '_over_delta'])}")
        p(f"    detection latency of the repaired jump (t0 - slip): {e['latency_hist']}")
        p(f"    *** UNREPAIRED FRACTION |e(t0)-e(t0-30)| / |delta|, latency-0 only "
          f"(0 = exact, 1 = inert):")
        p(f"          {fmt_summ(e['unrepaired_frac'])}")
        p(f"          in cycles: {fmt_summ(e['unrepaired_cyc'])}")
        for tag, lbl in (("lat0", "latency 0 s (slip landed THIS epoch)"),
                         ("lat_ge30", "latency >= 30 s (jump un-absorbed at t0-30)")):
            d = e[tag]
            p(f"    [{lbl}]  n={d['n']}")
            p(f"        slope [e(t0)-e(t0-30)] on delta : {fmt_ols(d['slope_delta_e'])}")
            p(f"        slope e(t0-30) on delta         : {fmt_ols(d['slope_pre'])}")
            p(f"        slope e(t0)    on delta         : {fmt_ols(d['slope_at'])}")
            p(f"        |e(t0-30)|      {fmt_summ(d['e_pre_abs'])}")
            p(f"        |e(t0)|         {fmt_summ(d['e_at_abs'])}")
            p(f"        |e(t0-30)|/|d|  {fmt_summ(d['e_pre_over_d'])}")
            p(f"        |e(t0)|/|d|     {fmt_summ(d['e_at_over_d'])}")
        p("    by detector tier")
        for tier, d in sorted(e["by_tier"].items()):
            p(f"      {tier:14s} n={d['n']:<5d} slope={fmt_ols(d['slope'])}")
            p(f"      {'':14s}   |e(t0-30)| {fmt_summ(d['e_pre_abs'])}")
            p(f"      {'':14s}   |e(t0)|    {fmt_summ(d['e_at_abs'])}")
        p(f"    offset coverage: {e['coverage']}")
        p("")
    k = R["keeps"]
    c = R["coverage"]
    p("-- TRUTH-SLIP COVERAGE of the ceiling --")
    p(f"    {c['truth_slips_on_roster']} truth slip(s) on the solved roster; "
      f"{c['repaired']} were repaired by the oracle ({100.0 * c['share']:.1f} %)")
    p(f"    the remaining {c['truth_slips_on_roster'] - c['repaired']} were answered by "
      f"today's response (free + re-level) or not answered at all")
    p("")
    k = R["keeps"]
    ss = R["slip_side"]
    p("-- SLIP-SIDE census: every injected jump, by what answered it --")
    p(f"    {ss['n']} truth slip(s)")
    for cls, n in sorted(ss["cls"].items(), key=lambda kv: -kv[1]):
        p(f"      {n:6d}  {100.0 * n / max(1, ss['n']):5.1f} %  {cls}")
    p("    |e| / |delta| on the slipped signal (~0 = the jump is gone, ~1 = it is sitting there):")
    for _rk, _rs in ss["e_ratio"].items():
        p(f"      {_rk:56s} {fmt_summ(_rs)}")
    p("")
    p("-- oracle_keep classification (rows; the run-end counter is per SIGNAL) --")
    p(f"    total {k['n']}")
    p(f"    (a)  partner-band of a repair at the same (t0, S, s)  : {k.get('partner', 0)}")
    p(f"    (a2) same slip, ALREADY repaired at an earlier epoch   : {k.get('already_repaired', 0)}"
      f"   <- last_applied_s bound, correct")
    p(f"    (a3) the named band(s) did NOT jump; another band did  : {k.get('other_band', 0)}"
      f"   <- per-signal truth, correct")
    p(f"    (b1) MISS, carry re-born by a T4 erase after the slip  : {k.get('miss_t4_reborn', 0)}"
      f"   <- birth_s floor, by design")
    p(f"    (b2) MISS, unexplained                                 : {k.get('miss_unexplained', 0)}")
    p(f"    (b3) truth slip lands AFTER t0 (detector fired early)  : {k.get('slip_after_t0', 0)}")
    p(f"    (c1) standalone, truth slip within +/-600 s only       : {k.get('true_slip_600', 0)}")
    p(f"    (c2) standalone, NO truth slip within +/-600 s         : {k.get('false_alarm', 0)}"
      f"   <- data-domain detector false alarm")
    for cls in ("other_band", "miss_t4_reborn", "miss_unexplained"):
        if not k.get(cls, 0):
            continue
        p(f"    [{cls}] arc |e| at t0     {fmt_summ(k.get(cls + '_e_at'))}")
        p(f"    [{cls}] arc |e| at t0+150 {fmt_summ(k.get(cls + '_e_p5'))}")
        p(f"    [{cls}] truth |delta|     {fmt_summ(k.get(cls + '_abs_delta'))}")
        p(f"    [{cls}] |e|/|delta| ON THE SLIPPED SIGNAL, t0     "
          f"{fmt_summ(k.get(cls + '_sig_ratio_t0'))}   (~1 = never repaired)")
        p(f"    [{cls}] |e|/|delta| ON THE SLIPPED SIGNAL, t0+150 "
          f"{fmt_summ(k.get(cls + '_sig_ratio_p5'))}")
    p("    per tier:")
    for t, d in sorted(k["by_tier"].items()):
        p(f"      {t:14s} {d}")
    p("")
    r = R["refloats"]
    p("-- `refloat` rows on the ORACLE arm = no-carry fall-throughs --")
    p(f"    total {r['n']};  ignition (first hour) {r['ignition']}, settled {r['settled']}")
    p(f"    followed a T4 erase on the same (S,s) within 3 epochs: {r['after_t4_3ep']}"
      f"  ({100.0 * r['after_t4_3ep'] / max(1, r['n']):.1f} %)")
    p(f"    by hour : {r['hour']}")
    p(f"    by tier : {r['by_tier']}")
    p("")
    p("-- T4 attribution events: oracle arm vs control --")
    for t in R["t4"]:
        p(f"    [{t['label']}] n={t['n']}  {t['cls']}")
        for cls, s in sorted(t["e_pre"].items()):
            p(f"        |e_pre| {cls:22s} {fmt_summ(s)}")
    p("=" * 96)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("run_dir")
    ap.add_argument("fixture_dir")
    ap.add_argument("--control", default=None, help="control run dir (no oracle)")
    ap.add_argument("--window-s", type=float, default=90.0)
    ap.add_argument("--min-chords", type=int, default=3)
    ap.add_argument("--min-sats", type=int, default=3)
    ap.add_argument("--json", dest="json_out", default=None)
    ap.add_argument("--txt", dest="txt_out", default=None)
    args = ap.parse_args()

    allsta, held = load_stations(args.run_dir)
    print(f"    stations {len(allsta)} ({len(held)} held out)", file=sys.stderr)
    by_sig, by_sat, n_truth, missing = load_truth_full(
        args.fixture_dir, sorted(allsta - held))
    if missing:
        print(f"    WARN: {len(missing)} station(s) with no truth_slips.csv", file=sys.stderr)

    ev = load_events(args.run_dir, held)
    med, pool, tows, diag = scan_dd(args.run_dir, args.fixture_dir, held, args.min_chords)
    stat = ArcStat(med, pool, args.min_chords, args.min_sats)

    R = {"run": args.run_dir, "control": args.control, "fixture": args.fixture_dir,
         "window_s": args.window_s, "ref_free": "both",
         "n_repair": len(ev["repairs"]), "n_keep": len(ev["keeps"]),
         "n_refloat": len(ev["refloats"]), "n_t4": len(ev["t4"]),
         "n_truth": n_truth, "dd_diag": dict(diag), "event_counts": dict(ev["counts"]),
         "effect": {}, "t4": []}

    for tag, rf in (("astraced", False), ("reffree", True)):
        recs, ident = repair_effect(ev["repairs"], stat, ev["any_qc"], by_sig,
                                    args.window_s, rf)
        R["ident"] = dict(ident)
        R["effect"][tag] = effect_report(recs, tag)
        if tag == "astraced":
            R["_recs_n"] = len(recs)

    # ---- truth-slip coverage: the fraction of the bed's injected jumps the
    # ceiling repaired.  A ceiling applied to half the population bounds only
    # that half, so the plateau verdict is read against this number.
    recs0, _ = repair_effect(ev["repairs"], stat, ev["any_qc"], by_sig,
                             args.window_s, False)
    covered = set()
    for r in recs0:
        for s, d in by_sig.get((r["sta"], r["sat"], r["sig"]), ()):
            if r["s_cont"] - args.window_s < s <= r["s_cont"]:
                covered.add((r["sta"], r["sat"], r["sig"], s))
    n_slip_roster = sum(len(v) for v in by_sig.values())
    R["coverage"] = {
        "truth_slips_on_roster": n_slip_roster,
        "repaired": len(covered),
        "share": len(covered) / max(1, n_slip_roster),
        "repair_rows": len(ev["repairs"]),
    }
    R["slip_side"] = slip_side_census(by_sig, recs0, ev, stat, args.window_s, False)
    R["keeps"] = classify_keeps(ev["keeps"], ev["repairs"], ev["t4"], by_sat, by_sig,
                                stat, ev["any_qc"], args.window_s, False)
    R["refloats"] = classify_refloats(ev["refloats"], ev["t4"], tows)
    R["t4"].append(t4_split(ev["t4"], ev["repairs"], ev["keeps"], stat,
                            ev["any_qc"], False, os.path.basename(args.run_dir)))

    if args.control:
        callsta, cheld = load_stations(args.control)
        cev = load_events(args.control, cheld)
        cmed, cpool, ctows, cdiag = scan_dd(args.control, args.fixture_dir, cheld,
                                            args.min_chords)
        cstat = ArcStat(cmed, cpool, args.min_chords, args.min_sats)
        R["t4"].append(t4_split(cev["t4"], [], [], cstat, cev["any_qc"], False,
                                os.path.basename(args.control) + " (CONTROL)"))
        R["control_event_counts"] = dict(cev["counts"])
        R["control_n_t4"] = len(cev["t4"])

    render(R, sys.stdout)
    if args.txt_out:
        with open(args.txt_out, "w", encoding="utf-8") as f:
            render(R, f)
    if args.json_out:
        with open(args.json_out, "w", encoding="utf-8") as f:
            json.dump(R, f, indent=1, default=str)
    return 0


if __name__ == "__main__":
    sys.exit(main())
