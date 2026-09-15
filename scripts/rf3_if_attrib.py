#!/usr/bin/env python3
"""RF-3-read: what moves the per-satellite IF correction a consumer applies (a DIAGNOSTIC).

Pre-registered in docs/refound/RF-3-prep_golden_rebase_2026-09-15.md s5.  For every satellite the
consumer's code correction is ``x_s(t) = -clk + a1*b(C1C) + a2*b(C2W)`` (refound_grade.if_dispersion's
quantity).  Its network-COMMON change cancels in the consumer clock, so each epoch-to-epoch change is
de-commoned by the median change over the satellites present at both epochs:

    dy_s(t) = dx_s(t) - median_{s' in both} dx_s'(t)

and split into STEPS (|dy| > --step-m) and SMOOTH movement; a satellite absent in between is a GAP
(its arc boundary), reported separately.  Each step is matched to the trace events of that satellite
logged in (t_prev, t] and compared with the same match rate over the non-step pairs (the base rate:
at storm density "an event nearby" is near its base rate, so a lift, not a count, attributes).
The change is also split into its clock part (-dclk) and its code-bias part (a1*db1 + a2*db2).

Frozen reading (s5): steps at the vehicle's own events => a shared float-ambiguity / GF-bias direction
the T_GD pin does not close; smooth growth between events => a rank / weighting read of the float block.

Usage: python scripts/rf3_if_attrib.py runs/_rf3p_field6 runs/_rf0g_field6 [--step-m 0.25] [--top 6]
"""
import argparse
import csv
import os
from collections import Counter, defaultdict

import numpy as np

F1, F2 = 1575.42e6, 1227.60e6
A1, A2 = F1 * F1 / (F1 * F1 - F2 * F2), -F2 * F2 / (F1 * F1 - F2 * F2)


def read_products(run_dir):
    """tow -> sat -> [clk, b1, b2]; plus the sorted emitted tows."""
    per = defaultdict(dict)
    tow = None
    with open(os.path.join(run_dir, "products.txt"), encoding="utf-8", errors="replace") as fh:
        for ln in fh:
            if ln[0] == "#":
                continue
            f = ln.rstrip("\n").split(",")
            if f[0] == "E":
                tow = round(float(f[2]), 1)
            elif tow is None:
                continue
            elif f[0] == "C":
                per[tow].setdefault(f[1], [None, None, None])[0] = float(f[2])
            elif f[0] == "B" and f[2] in ("C1C", "C2W"):
                per[tow].setdefault(f[1], [None, None, None])[1 if f[2] == "C1C" else 2] = float(f[3])
    return per, sorted(per)


def read_events(run_dir):
    """(sat) -> list of (tow, key); key = tier:event:action.  Also per-sat anchor drops from amb_state."""
    ev = defaultdict(list)
    p = os.path.join(run_dir, "logs", "network_events.csv")
    with open(p, encoding="utf-8", errors="replace") as fh:
        for r in csv.DictReader(fh):
            sat = r.get("sat") or ""
            if not sat:
                continue
            for s in sat.split(";"):
                ev[s].append((round(float(r["tow"]), 1), f"{r['tier']}:{r['event']}:{r['action']}"))
    # anchor per (sat, signal) by epoch, to label amb_state drops/erases on the anchor station
    anchor = {}
    pa = os.path.join(run_dir, "logs", "network_phase_anchor.csv")
    if os.path.exists(pa):
        with open(pa, encoding="utf-8", errors="replace") as fh:
            for r in csv.DictReader(fh):
                anchor[(round(float(r["tow"]), 1), r["sat"], r["signal"])] = r["anchor_sta"]
    pa = os.path.join(run_dir, "logs", "network_amb_state.csv")
    if os.path.exists(pa):
        with open(pa, encoding="utf-8", errors="replace") as fh:
            next(fh)  # the '#' banner
            for r in csv.DictReader(fh):
                e = r["event"]
                if not e or e == "birth":
                    continue
                t = round(float(r["tow"]), 1)
                on_anchor = anchor.get((t, r["sat"], r["signal"])) == r["station"]
                ev[r["sat"]].append((t, f"AMB:{e.split(':')[0]}:{e.split(':')[-1]}" + (":ANCHOR" if on_anchor else "")))
    for s in ev:
        ev[s].sort()
    return ev


def analyse(run_dir, step_m, top, t_from=None, t_to=None):
    per, tows = read_products(run_dir)
    ev = read_events(run_dir)
    t0 = tows[0]
    x = {t: {s: -v[0] + A1 * v[1] + A2 * v[2] for s, v in per[t].items() if None not in v} for t in tows}
    parts = {t: {s: (-v[0], A1 * v[1] + A2 * v[2]) for s, v in per[t].items() if None not in v} for t in tows}
    last = {}                       # sat -> (tow index, value)
    steps, smooth, gaps = [], [], []   # (sat, t_prev, t, dy, dclk_part, dcb_part)
    for i in range(1, len(tows)):
        tp, t = tows[i - 1], tows[i]
        if (t_from is not None and t <= t_from) or (t_to is not None and t > t_to):
            for s in x[t]:
                last[s] = (t, x[t][s])
            continue
        both = [s for s in x[t] if s in x[tp]]
        if len(both) < 4:
            continue
        dx = {s: x[t][s] - x[tp][s] for s in both}
        med = float(np.median(list(dx.values())))
        medc = float(np.median([parts[t][s][0] - parts[tp][s][0] for s in both]))
        medb = float(np.median([parts[t][s][1] - parts[tp][s][1] for s in both]))
        for s in both:
            rec = (s, tp, t, dx[s] - med, parts[t][s][0] - parts[tp][s][0] - medc,
                   parts[t][s][1] - parts[tp][s][1] - medb)
            (steps if abs(rec[3]) > step_m else smooth).append(rec)
        for s in x[t]:
            if s not in x[tp] and s in last:
                gaps.append((s, last[s][0], t))
        for s in x[t]:
            last[s] = (t, x[t][s])

    def keys_in(s, tp, t):
        return {k for (te, k) in ev.get(s, ()) if tp < te <= t}

    step_keys = Counter()
    for s, tp, t, *_ in steps:
        for k in keys_in(s, tp, t):
            step_keys[k] += 1
    base_keys = Counter()
    for s, tp, t, *_ in smooth:
        for k in keys_in(s, tp, t):
            base_keys[k] += 1
    n_step, n_base = max(len(steps), 1), max(len(smooth), 1)
    n_step_any = sum(1 for s, tp, t, *_ in steps if keys_in(s, tp, t))
    n_base_any = sum(1 for s, tp, t, *_ in smooth if keys_in(s, tp, t))

    # per-satellite movement budget
    bud = defaultdict(lambda: [0.0, 0.0, 0.0, 0.0, 0, 0.0, 0.0])  # net_step, net_smooth, abs_step, rms_smooth^2, n_step, clk_step, cb_step
    for s, tp, t, dy, dc, db in steps:
        b = bud[s]; b[0] += dy; b[2] += abs(dy); b[4] += 1; b[5] += dc; b[6] += db
    for s, tp, t, dy, dc, db in smooth:
        b = bud[s]; b[1] += dy; b[3] += dy * dy

    print(f"\n===== {run_dir}: {len(tows)} emitted epochs, t0 {t0:.0f}; step |dy| > {step_m} m =====")
    allsm = np.array([r[3] for r in smooth]) if smooth else np.zeros(1)
    print(f"pairs: steps {len(steps)}  smooth {len(smooth)} (rms {np.sqrt(np.mean(allsm**2)):.3f} m, "
          f"MAD {1.4826*np.median(np.abs(allsm)):.3f} m)  gaps {len(gaps)}")
    tot_step = sum(abs(r[3]) for r in steps)
    print(f"step movement: sum|dy| {tot_step:.1f} m = clock part {sum(abs(r[4]) for r in steps):.1f} m, "
          f"code-bias part {sum(abs(r[5]) for r in steps):.1f} m (de-commoned separately)")
    print(f"pairs with ANY sat event in (t_prev,t]: steps {n_step_any}/{len(steps)} = {n_step_any/n_step:.1%}; "
          f"smooth {n_base_any}/{len(smooth)} = {n_base_any/n_base:.1%}")
    print("event key                                            steps  rate   base   lift")
    rows = []
    for k in set(step_keys) | set(base_keys):
        rs, rb = step_keys[k] / n_step, base_keys[k] / n_base
        rows.append((step_keys[k], k, rs, rb, rs / rb if rb > 0 else float("inf")))
    for c, k, rs, rb, lift in sorted(rows, reverse=True)[:12]:
        print(f"  {k:50s} {c:5d} {rs:6.1%} {rb:6.2%} {lift:6.1f}")
    print("worst satellites by net de-commoned drift (net = step part + smooth part):")
    print("  sat   net_m  step_net  smooth_net  n_step  sum|step|  smooth_rms  step_clk  step_cb")
    order = sorted(bud.items(), key=lambda kv: -abs(kv[1][0] + kv[1][1]))[:top]
    for s, b in order:
        n_sm = sum(1 for r in smooth if r[0] == s)
        print(f"  {s}  {b[0]+b[1]:7.2f}  {b[0]:8.2f}  {b[1]:10.2f}  {b[4]:6d}  {b[2]:9.2f}  "
              f"{np.sqrt(b[3]/max(n_sm,1)):10.3f}  {b[5]:8.2f}  {b[6]:7.2f}")
    for s, b in order[:3]:
        big = sorted([r for r in steps if r[0] == s], key=lambda r: -abs(r[3]))[:4]
        for _, tp, t, dy, dc, db in big:
            ks = sorted(keys_in(s, tp, t))
            print(f"    {s} tow {t:.0f} (+{(t-t0)/60:.0f} min) dy {dy:+.2f} (clk {dc:+.2f} cb {db:+.2f}) : {', '.join(ks)[:150]}")
    return dict(steps=len(steps), smooth=len(smooth))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("runs", nargs="+")
    ap.add_argument("--step-m", type=float, default=0.25)
    ap.add_argument("--top", type=int, default=6)
    ap.add_argument("--handovers", action="store_true", help="the held-basis handover-mode table instead")
    ap.add_argument("--from-tow", type=float, default=None, help="only pairs with t > this (e.g. 264600 = mid hour-2 window)")
    ap.add_argument("--to-tow", type=float, default=None, help="only pairs with t <= this (e.g. 279000 = mid hour-6 window)")
    a = ap.parse_args()
    for r in a.runs:
        if a.handovers:
            handover_table(r)
        else:
            analyse(r, a.step_m, a.top, a.from_tow, a.to_tow)


def handover_table(run_dir, big_m=1.0):
    """Held-basis handovers by per-satellite-epoch MODE vs the de-commoned IF-correction step they coincide with.

    Mode per (sat, tow) from the PBGAUGE rows: all-continuous / all-cold / MIXED (one signal of the satellite kept
    its frame, the other re-realized).  'cold_unmarked' = an sts_cold_declared row with marked=0 (the RF-2b predicate
    |value - prev_value| read 0 although the family re-realized).  |dy| = the larger de-commoned step at the handover
    epoch and the next emitted epoch; 'none' = satellite-epochs with no handover (the null)."""
    per, tows = read_products(run_dir)
    x = {t: {s: -v[0] + A1 * v[1] + A2 * v[2] for s, v in per[t].items() if None not in v} for t in tows}
    dy = {}
    for i in range(1, len(tows)):
        tp, t = tows[i - 1], tows[i]
        both = [s for s in x[t] if s in x[tp]]
        if len(both) < 4:
            continue
        med = float(np.median([x[t][s] - x[tp][s] for s in both]))
        for s in both:
            dy[(s, t)] = abs(x[t][s] - x[tp][s] - med)
    nxt = {tows[i]: tows[i + 1] for i in range(len(tows) - 1)}
    hv = defaultdict(lambda: dict(cont=0, cold=0, cold_unmarked=0, cont_marked=0))
    with open(os.path.join(run_dir, "logs", "network_events.csv"), encoding="utf-8", errors="replace") as fh:
        for r in csv.DictReader(fh):
            if r["tier"] not in ("PBGAUGE", "STRANSFORM"):
                continue
            sat = (r["sat"] or "").split("_")[0][:3]
            k = (sat, round(float(r["tow"]), 1))
            if r["event"] == "handover_continuous":
                hv[k]["cont"] += 1
            elif r["event"] == "handover_cold":
                hv[k]["cold"] += 1
            elif r["event"] == "sts_cold_declared" and "marked=0" in r["detail"]:
                hv[k]["cold_unmarked"] += 1
            elif r["event"] == "sts_applied" and "marked=1" in r["detail"]:
                hv[k]["cont_marked"] += 1

    def step_at(s, t):
        t1 = min((u for u in tows if u >= t), default=None)
        if t1 is None:
            return None
        c = [dy.get((s, t1))]
        if t1 in nxt:
            c.append(dy.get((s, nxt[t1])))
        c = [v for v in c if v is not None]
        return max(c) if c else None

    cls = defaultdict(list)
    for (s, t), h in hv.items():
        mode = "mixed" if h["cont"] and h["cold"] else ("all-cold" if h["cold"] else "all-cont")
        if h["cold_unmarked"]:
            mode += "+cold_unmarked"
        v = step_at(s, t)
        if v is not None:
            cls[mode].append(v)
    hk = {(s, min((u for u in tows if u >= t), default=-1)) for (s, t) in hv}
    cls["none (null)"] = [v for (s, t), v in dy.items() if (s, t) not in hk]
    print(f"\n== {run_dir}: held-basis handovers (satellite-epochs) vs de-commoned IF step |dy| ==")
    print("  mode                          n   p50_m   p90_m   max_m  n>|%.1f m|" % big_m)
    for mode in sorted(cls):
        a = np.array(cls[mode])
        print(f"  {mode:28s} {len(a):5d} {np.percentile(a,50):7.3f} {np.percentile(a,90):7.3f} {a.max():7.2f} {int((a>big_m).sum()):6d}")


if __name__ == "__main__":
    main()
