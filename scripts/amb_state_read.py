#!/usr/bin/env python3
"""RF-1 step 2: the four pre-registered reads of ``network_amb_state.csv``.

Card: ``docs/refound/RF-1_float_ambiguities_design.md`` §7.4, under
``docs/refound/NETWORK_REFOUNDING_2026-09-14.md``. The trace is written by
``network.trace_amb_state`` and this script implements §7.4's reads verbatim, so the
RF-0 baseline and every later arm are read the same way::

    python scripts/amb_state_read.py <run_dir_or_csv> [--json out.json] [--max-folds 30]

``<run_dir_or_csv>`` is a run directory (``<dir>/logs/network_amb_state.csv`` is found
automatically) or the CSV itself. ``network_prior_residual.csv`` is picked up beside it
when present and reported as D4's unsplit cross-check.

The four reads
--------------
**D1, the variance trajectory.** Per arc, fit ``log(fold_var) = a - b*log(n_folds)``
over the first ``--max-folds`` (default 30) folds, and report ``b`` at p50 across arcs.
RF-0 prediction: ``b > 1``, faster than 1/n, the over-count of design §1.3, because the
B rows of one line of sight assert the same station-common direction B times per epoch.
RF-1 bar: ``b = 1.00 +/- 0.15`` at p50. This is the most direct test of §3.2.

**D2, the level trajectory** (R1 re-asked on the carry instead of on the code residual).
Two fractions: arcs whose ``value_abs`` at folds 1-3 is already within 0.3 m of its
settled value, and arcs whose ``|value_abs|`` grows monotonically over the arc.
RF-0 proxies (code-residual): 0.39-0.46 and 74-77 %. RF-1 bar: growing < 55 %.

**D3, the drop / erase inventory.** Per hour by cause, with the fold variance
immediately before and after the event and the number of folds taken to recover the
pre-event variance. Invariant #4 in the other direction: a ``float_kf`` run with 0
drops is a miswiring, not a quiet run.

**D4, sigma0(pl) by arc age** (first 5 folds / 5-30 / > 30). A prior without bias reads
about 1.0 in every bucket. This is the read that can refute the card before any
mechanism is built, and the one that calibrates the birth floor if it does not.

  .. warning::
     D4's split cannot come from ``network_prior_residual.csv``: that file is aggregated
     per (family, label) per epoch and has no arc-age column. It is formed here from
     consecutive rows of one arc: ``e = value_abs(t-1) - value_abs(t)`` is exactly the
     harvest's own prior-residual numerator (``innov = oc->second.value_abs -
     value_abs``) and ``fold_var_m2(t)`` is exactly its denominator, so
     ``sigma0 = sqrt(mean(e^2 / fold_var))`` over a bucket is the census's statistic
     restricted to that bucket. The census's standing caveat travels with it: under
     Card-1 conditional coupling the fold's RHS is not ``value_abs`` alone (the coupling
     rows contribute ``sum M_rj * s_prev_j``), so this e-hat and the one the fold saw
     differ by the regressor term. That is an open card on the census numerator, not a
     defect of this trace, and it is why the census's own unsplit sigma0 is printed
     beside the split as a cross-check.

  .. note::
     Rows whose fold and posterior variances are inconsistent (``post_var > fold_var``,
     the RNEG class the census re-partitions) are dropped from D1 and D4 and counted
     separately. Their normalization is not a standardized residual: r slams into the
     redundancy floor and |t| inflates, which is how a handful of rows can come to head
     every ladder in such a census.
"""
from __future__ import annotations

import argparse
import csv
import json
import math
import os
import sys
from collections import defaultdict

# Bars and reference values, from design §7.4 / §7.6.  Printed beside every number so a
# reading is never quoted without the bar it was pre-registered against.
D1_RF1_BAR = (0.85, 1.15)          # b at p50
D2_SETTLED_BAR_M = 0.3             # "within 0.3 m of settled"
D2_EARLY_FOLDS = 3                 # folds 1..3
D2_GROWING_BAR = 0.55              # growing fraction stays below 55 % (RF-1 bar)
D4_BUCKETS = (("first5", 1, 5), ("5to30", 6, 30), ("gt30", 31, 1 << 62))
MIN_FOLDS_FOR_FIT = 5              # an arc needs a few points before a slope means anything


# ----------------------------------------------------------------------------- loading
def _find_csv(path: str, name: str) -> str | None:
    if os.path.isfile(path):
        if os.path.basename(path) == name:
            return path
        sib = os.path.join(os.path.dirname(path), name)
        return sib if os.path.isfile(sib) else None
    for cand in (os.path.join(path, "logs", name), os.path.join(path, name)):
        if os.path.isfile(cand):
            return cand
    return None


def load_rows(csv_path: str) -> list[dict]:
    """Read the trace, skipping the `#` provenance line, coercing the numeric columns."""
    out: list[dict] = []
    with open(csv_path, "r", newline="", encoding="utf-8", errors="replace") as fh:
        lines = [ln for ln in fh if not ln.startswith("#")]
    for r in csv.DictReader(lines):
        try:
            out.append({
                "week": int(r["week"]),
                "tow": float(r["tow"]),
                "station": r["station"],
                "sat": r["sat"],
                "signal": r["signal"],
                "value_abs_m": float(r["value_abs_m"]),
                "var_m2": float(r["var_m2"]),
                "var_first_m2": float(r["var_first_m2"]),
                "var_birth_m2": float(r["var_birth_m2"]),
                "fold_var_m2": float(r["fold_var_m2"]),
                "n_folds": int(r["n_folds"]),
                "age_ep": int(r["age_ep"]),
                "age_since_relevel_ep": int(r["age_since_relevel_ep"]),
                "folded": r["folded"] == "1",
                "route": r["route"],
                "event": r["event"],
            })
        except (KeyError, ValueError):
            continue
    return out


def arcs_of(rows: list[dict]) -> "dict[tuple, list[dict]]":
    """Group by (station, sat, signal) and split at every erase.

    An erase-and-rebirth is a different arc: birth stamps and ``n_folds`` restart, so
    pooling across one would fit a slope through two unrelated trajectories. The erase
    row itself stays at the end of the arc it closes, where D3 reads it.
    """
    by_key: "dict[tuple, list[dict]]" = defaultdict(list)
    for r in sorted(rows, key=lambda x: (x["station"], x["sat"], x["signal"],
                                         x["week"], x["tow"])):
        by_key[(r["station"], r["sat"], r["signal"])].append(r)
    arcs: "dict[tuple, list[dict]]" = {}
    for key, rs in by_key.items():
        seg, gen = [], 0
        for r in rs:
            seg.append(r)
            if r["event"].startswith("erase:"):
                arcs[key + (gen,)] = seg
                seg, gen = [], gen + 1
        if seg:
            arcs[key + (gen,)] = seg
    return arcs


def p50(xs: list[float]) -> float:
    if not xs:
        return float("nan")
    s = sorted(xs)
    n = len(s)
    return s[n // 2] if n % 2 else 0.5 * (s[n // 2 - 1] + s[n // 2])


def pct(xs: list[float], q: float) -> float:
    if not xs:
        return float("nan")
    s = sorted(xs)
    i = min(len(s) - 1, max(0, int(round(q * (len(s) - 1)))))
    return s[i]


# ---------------------------------------------------------------------------- D1
def read_d1(arcs, max_folds: int) -> dict:
    """Per arc, OLS of log(fold_var) on log(n_folds) over the first `max_folds` folds."""
    slopes, rneg, used_arcs, pts = [], 0, 0, 0
    at_n: "dict[int, list[float]]" = defaultdict(list)   # fold_var at n = 1, 2, 5, 10, 30
    for rs in arcs.values():
        for r in rs:
            if r["folded"] and r["fold_var_m2"] > 0.0 and r["n_folds"] in (1, 2, 5, 10, 30):
                at_n[r["n_folds"]].append(r["fold_var_m2"])
    for rs in arcs.values():
        xs, ys = [], []
        for r in rs:
            if not r["folded"] or r["n_folds"] < 1 or r["n_folds"] > max_folds:
                continue
            if not (r["fold_var_m2"] > 0.0):
                continue
            if r["var_m2"] > r["fold_var_m2"]:   # RNEG: posterior above prior
                rneg += 1
                continue
            xs.append(math.log(r["n_folds"]))
            ys.append(math.log(r["fold_var_m2"]))
        if len(xs) < MIN_FOLDS_FOR_FIT:
            continue
        n = len(xs)
        mx, my = sum(xs) / n, sum(ys) / n
        sxx = sum((x - mx) ** 2 for x in xs)
        if sxx <= 0.0:
            continue
        sxy = sum((x - mx) * (y - my) for x, y in zip(xs, ys))
        slopes.append(-sxy / sxx)            # log var = a - b log n  =>  b = -slope
        used_arcs += 1
        pts += n
    return {"b_p50": p50(slopes), "b_p25": pct(slopes, 0.25), "b_p75": pct(slopes, 0.75),
            "n_arcs": used_arcs, "n_points": pts, "n_rneg_dropped": rneg,
            "max_folds": max_folds,
            # The collapse in physical units, so `b` is never quoted alone: a power-law
            # fit to a geometric collapse (the K_eff = (B+1)/(B-1) freeze the F1 comment
            # predicts) reads as a large b, and these medians say how far the variance
            # actually fell over the window.
            "fold_var_p50_at_n": {str(k): p50(v) for k, v in sorted(at_n.items())},
            "rf1_bar": list(D1_RF1_BAR),
            "verdict": ("in RF-1 bar" if used_arcs and D1_RF1_BAR[0] <= p50(slopes) <= D1_RF1_BAR[1]
                        else "faster than 1/n (over-count)" if used_arcs and p50(slopes) > D1_RF1_BAR[1]
                        else "slower than 1/n" if used_arcs else "no arcs")}


# ---------------------------------------------------------------------------- D2
def read_d2(arcs) -> dict:
    """Early-level accuracy and the growing fraction, on the carry itself."""
    near, growing_end, growing_trend, total, dev_early = 0, 0, 0, 0, []
    for rs in arcs.values():
        vals = [r["value_abs_m"] for r in rs
                if r["folded"] or r["event"] == "birth"] or [r["value_abs_m"] for r in rs]
        if len(vals) < 2 * D2_EARLY_FOLDS:
            continue
        total += 1
        settled = p50(vals[-max(3, len(vals) // 10):])   # last 10 % (>= 3 samples)
        early = vals[:D2_EARLY_FOLDS]
        d = min(abs(v - settled) for v in early)
        dev_early.append(d)
        if d <= D2_SETTLED_BAR_M:
            near += 1
        # "Grows" is reported as two tests: strict monotonicity is unmeasurable against
        # solve noise, so neither is R1's literal statistic and the pair brackets it.
        #   * Endpoint: |value_abs| ends above where it started (what a single before /
        #     after comparison sees). Chance is 0.5 for a symmetric random walk, so a
        #     reading near 0.5 says only that there is no net outward drift on this test.
        #   * Trend: the OLS slope of |value_abs| over the arc is positive. Sensitive to
        #     a sustained ramp that the endpoints can miss, and the closer analogue of
        #     the level growing over the arc.
        if abs(vals[-1]) > abs(vals[0]) + 1e-9:
            growing_end += 1
        n = len(vals)
        mx = (n - 1) / 2.0
        ay = [abs(v) for v in vals]
        my = sum(ay) / n
        sxx = sum((i - mx) ** 2 for i in range(n))
        if sxx > 0.0 and sum((i - mx) * (y - my) for i, y in enumerate(ay)) > 0.0:
            growing_trend += 1
    return {"n_arcs": total,
            "frac_within_0p3m_at_folds_1_3": (near / total) if total else float("nan"),
            "frac_growing": (growing_end / total) if total else float("nan"),
            "frac_growing_trend": (growing_trend / total) if total else float("nan"),
            "p50_early_dev_m": p50(dev_early), "p90_early_dev_m": pct(dev_early, 0.9),
            "rf0_proxy_within": [0.39, 0.46], "rf0_proxy_growing": [0.74, 0.77],
            "rf1_bar_growing_lt": D2_GROWING_BAR}


# ---------------------------------------------------------------------------- D3
def read_d3(rows, arcs) -> dict:
    """Events per hour by cause, with the variance before / after and folds to recover."""
    if not rows:
        return {"causes": {}, "hours": 0}
    t0 = min(r["week"] * 604800.0 + r["tow"] for r in rows)
    t1 = max(r["week"] * 604800.0 + r["tow"] for r in rows)
    hours = max((t1 - t0) / 3600.0, 1e-9)
    by_cause: "dict[str, dict]" = defaultdict(
        lambda: {"n": 0, "var_before": [], "var_after": [], "folds_to_recover": []})
    # An erase ends its arc, so its "after" is the first fold of the successor arc at the
    # same (station, sat, signal): an erase throws a converged level away and the network
    # pays for the re-earn, which is what this read measures. Index the successors by
    # generation so the scan can cross the boundary.
    succ: "dict[tuple, list[dict]]" = {}
    for (sta, sat, sig, gen), rs in arcs.items():
        succ[(sta, sat, sig, gen)] = arcs.get((sta, sat, sig, gen + 1), [])
    # Per-arc scan: an event's "before" is the last folded variance ahead of it, its
    # "after" the first folded variance behind it (crossing into the successor arc for an
    # erase), and the recovery count the number of folds until the fold variance is back
    # at or below the pre-event value.
    for key, rs in arcs.items():
        tail = succ.get(key, [])
        for i, r in enumerate(rs):
            ev = r["event"]
            if not ev or ev == "birth":
                continue
            c = by_cause[ev]
            c["n"] += 1
            before = next((rs[j]["fold_var_m2"] for j in range(i - 1, -1, -1)
                           if rs[j]["folded"] and rs[j]["fold_var_m2"] > 0.0), None)
            after_rows = rs[i + 1:] + (tail if ev.startswith("erase:") else [])
            after_idx = next((j for j, x in enumerate(after_rows)
                              if x["folded"] and x["fold_var_m2"] > 0.0), None)
            if before is not None:
                c["var_before"].append(before)
            if after_idx is not None:
                c["var_after"].append(after_rows[after_idx]["fold_var_m2"])
                if before is not None:
                    k = 0
                    for x in after_rows[after_idx:]:
                        if not x["folded"] or not (x["fold_var_m2"] > 0.0):
                            continue
                        k += 1
                        if x["fold_var_m2"] <= before:
                            c["folds_to_recover"].append(k)
                            break
    out = {}
    for cause, c in sorted(by_cause.items()):
        out[cause] = {"n": c["n"], "per_hour": c["n"] / hours,
                      "var_before_p50_m2": p50(c["var_before"]),
                      "var_after_p50_m2": p50(c["var_after"]),
                      "folds_to_recover_p50": p50([float(x) for x in c["folds_to_recover"]]),
                      "n_recovered": len(c["folds_to_recover"])}
    births = sum(1 for r in rows if r["event"] == "birth")
    return {"causes": out, "hours": hours, "n_births": births,
            "births_per_hour": births / hours, "n_arcs": len(arcs)}


# ---------------------------------------------------------------------------- D4
def read_d4(arcs) -> dict:
    """sigma0(pl) from consecutive rows of one arc, split by arc age in folds."""
    buckets = {name: [] for name, _, _ in D4_BUCKETS}
    fvar = {name: [] for name, _, _ in D4_BUCKETS}
    rneg, paired = 0, 0
    for rs in arcs.values():
        prev = None
        for r in rs:
            if r["event"].startswith("erase:"):
                break
            # The pair is valid only when row r's fold actually consumed row prev's
            # value. n_folds is the guard that makes that stride-safe: a folded row's
            # count is exactly one above the previous folded row's, so a trace stride > 1
            # (or any epoch the carry did not act) breaks the pair instead of silently
            # differencing across a gap.
            if (r["folded"] and r["fold_var_m2"] > 0.0
                    and prev is not None and r["n_folds"] == prev["n_folds"] + 1):
                if r["var_m2"] > r["fold_var_m2"]:
                    rneg += 1       # RNEG: posterior above prior, |t| is not standardized
                else:
                    e = prev["value_abs_m"] - r["value_abs_m"]
                    t2 = (e * e) / r["fold_var_m2"]
                    for name, lo, hi in D4_BUCKETS:
                        if lo <= r["n_folds"] <= hi:
                            buckets[name].append(t2)
                            fvar[name].append(r["fold_var_m2"])
                            paired += 1
                            break
            prev = r
    out = {}
    for name, _, _ in D4_BUCKETS:
        v, fv = buckets[name], fvar[name]
        # The robust twin divides the median of t^2 by the chi2_1 median (0.4549), the
        # scale-consistent estimator; the plain mean is the census's own sigma0 and is
        # what the bar is written against. A large gap between the two is meaningful,
        # not a nuisance: it says the prior-residual population is heavy-tailed, so the
        # floor a mean implies and the floor a median implies are different decisions.
        s_mean = math.sqrt(sum(v) / len(v)) if v else float("nan")
        s_rob = math.sqrt(p50(v) / 0.4549) if v else float("nan")
        fv50 = p50(fv)
        out[name] = {"n": len(v), "sigma0": s_mean, "sigma0_robust": s_rob,
                     "abs_t_p90": math.sqrt(pct(v, 0.90)) if v else float("nan"),
                     "fold_var_p50_m2": fv50,
                     # The variance this bucket's residuals actually deserve, in the units
                     # the knob takes: the calibration input for §3.3(c).
                     "implied_floor_m2": (s_mean * s_mean * fv50) if v else float("nan"),
                     "implied_floor_robust_m2": (s_rob * s_rob * fv50) if v else float("nan")}
    return {"buckets": out, "n_rneg_dropped": rneg, "n_paired": paired,
            "honest_prior_reads": 1.0,
            "young_gate": "0.8-1.2 => the prior is already honest (card premise refuted)"}


def prires_pl_sigma0(path: str | None) -> dict:
    """The census's own unsplit pl sigma0: D4's cross-check, not its source."""
    if not path or not os.path.isfile(path):
        return {"available": False}
    vals, warm = [], []
    with open(path, "r", newline="", encoding="utf-8", errors="replace") as fh:
        for r in csv.DictReader(ln for ln in fh if not ln.startswith("#")):
            if r.get("family") != "pl":
                continue
            try:
                s = float(r["sigma0"])
            except (KeyError, ValueError):
                continue
            if s > 0.0:
                vals.append(s)
                if r.get("sigma0_warm", "") in ("1", "true", "True"):
                    warm.append(s)
    return {"available": bool(vals), "n_epoch_rows": len(vals),
            "sigma0_p50": p50(vals), "sigma0_p90": pct(vals, 0.9),
            "note": "aggregated per (family,label) per EPOCH — no arc-age split exists here"}


# ---------------------------------------------------------------------------- main
def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("run", help="run directory or network_amb_state.csv")
    ap.add_argument("--json", help="also write the full result as JSON")
    ap.add_argument("--max-folds", type=int, default=30,
                    help="D1 fit window in FOLDS (design §7.4: 30)")
    a = ap.parse_args()

    csv_path = _find_csv(a.run, "network_amb_state.csv")
    if not csv_path:
        print(f"network_amb_state.csv not found under {a.run}", file=sys.stderr)
        return 2
    rows = load_rows(csv_path)
    if not rows:
        print(f"{csv_path}: no rows (an ARMED trace with 0 rows means the leveling "
              f"harvest never ran — read the run-end RF-1 §7.4 line)", file=sys.stderr)
        return 3
    arcs = arcs_of(rows)
    route_n = defaultdict(int)
    for r in rows:
        route_n[r["route"] or "(not folded)"] += 1
    res = {"csv": csv_path, "n_rows": len(rows), "n_arcs": len(arcs),
           "routes": dict(sorted(route_n.items())),
           "D1": read_d1(arcs, a.max_folds),
           "D2": read_d2(arcs),
           "D3": read_d3(rows, arcs),
           "D4": read_d4(arcs),
           "prires_pl_unsplit": prires_pl_sigma0(
               _find_csv(a.run, "network_prior_residual.csv"))}
    print(f"== {csv_path}")
    print(f"   {len(rows)} rows, {len(arcs)} arcs (split at every erase); "
          f"routes: {res['routes']}")
    d1 = res["D1"]
    print(f"\n-- D1 variance trajectory  log var = a - b log n, first {d1['max_folds']} folds")
    print(f"   b p50 = {d1['b_p50']:.3f}   (p25 {d1['b_p25']:.3f} / p75 {d1['b_p75']:.3f})"
          f"   over {d1['n_arcs']} arcs / {d1['n_points']} folded rows"
          f"   [RF-0 predicts b > 1; RF-1 bar {D1_RF1_BAR[0]}-{D1_RF1_BAR[1]}] -> {d1['verdict']}")
    print("   fold_var p50 at n = "
          + "  ".join(f"{k}:{v:.3e}" for k, v in d1["fold_var_p50_at_n"].items())
          + "  m2   (a 1/n recursion would fall exactly 30x from n=1 to n=30)")
    if d1["n_rneg_dropped"]:
        print(f"   ({d1['n_rneg_dropped']} RNEG rows dropped: post_var > fold_var)")
    d2 = res["D2"]
    print(f"\n-- D2 level trajectory  ({d2['n_arcs']} arcs)")
    print(f"   within {D2_SETTLED_BAR_M} m of settled at folds 1-{D2_EARLY_FOLDS}: "
          f"{d2['frac_within_0p3m_at_folds_1_3']:.3f}   [RF-0 code-residual proxy 0.39-0.46]")
    print(f"   |value_abs| grows, ENDPOINT test:          {d2['frac_growing']:.3f}"
          f"   [RF-0 proxy 0.74-0.77; RF-1 bar < {D2_GROWING_BAR}; chance 0.5]")
    print(f"   |value_abs| grows, TREND test:             {d2['frac_growing_trend']:.3f}"
          f"   [same bars; chance 0.5]")
    print(f"   early |dev| p50 {d2['p50_early_dev_m']:.3f} m / p90 {d2['p90_early_dev_m']:.3f} m")
    d3 = res["D3"]
    print(f"\n-- D3 drop / erase inventory  ({d3['hours']:.2f} h, {d3['n_arcs']} arcs, "
          f"{d3['n_births']} births = {d3['births_per_hour']:.1f}/h)")
    if not d3["causes"]:
        print("   NO drop / erase events  <- invariant #4: on float_kf this is a MISWIRE, "
              "not a quiet run")
    for cause, c in d3["causes"].items():
        print(f"   {cause:<24} n={c['n']:<7} {c['per_hour']:8.1f}/h   "
              f"var before {c['var_before_p50_m2']:.3e} -> after {c['var_after_p50_m2']:.3e} m2"
              f"   folds to recover p50 {c['folds_to_recover_p50']}"
              f" (n={c['n_recovered']})")
    d4 = res["D4"]
    print(f"\n-- D4 sigma0(pl) by arc age in folds   [honest prior reads ~ 1.0]")
    for name, _, _ in D4_BUCKETS:
        b = d4["buckets"][name]
        print(f"   {name:<8} n={b['n']:<9} sigma0 = {b['sigma0']:9.3f}"
              f"   robust {b['sigma0_robust']:7.3f}   |t| p90 {b['abs_t_p90']:9.3f}")
        print(f"   {'':<8} fold_var p50 {b['fold_var_p50_m2']:.3e} m2"
              f"  ->  implied floor {b['implied_floor_m2']:.3e} m2"
              f"  (robust {b['implied_floor_robust_m2']:.3e} m2)")
    print(f"   FROZEN GATE (the manager's decision, 2026-09-14): young bucket sigma0 in "
          f"0.8-1.2 => the prior is already honest, the card's premise is REFUTED and "
          f"steps 3-5 do not start.")
    pr = res["prires_pl_unsplit"]
    if pr.get("available"):
        print(f"   cross-check, network_prior_residual.csv `pl` (UNSPLIT, per-epoch): "
              f"sigma0 p50 {pr['sigma0_p50']:.3f} / p90 {pr['sigma0_p90']:.3f} "
              f"over {pr['n_epoch_rows']} epoch rows")
    if a.json:
        with open(a.json, "w", encoding="utf-8") as fh:
            json.dump(res, fh, indent=2)
        print(f"\n[json] {a.json}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
