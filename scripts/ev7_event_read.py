#!/usr/bin/env python3
"""EV7-read: the all-satellite clock-anchor decouple of TOW 278520 (a diagnostic).

Pre-registered in ``docs/refound/EV7_all_sat_clock_decouple_2026-09-21.md``.  Trace-only: it
reads a finished run directory and writes nothing but stdout (no plotting, no run, no solve).
The frozen windows of the card's section 3 are all expressed relative to the trip epoch
``--tow`` (default 278520), so an arm that moved the event is read with the same instrument by
passing the new epoch.

Sections, in the order the card asks for them:

(a) the event rows of ``network_events.csv`` over ``T-60 .. T+120`` counted by
    (tow, tier, event) with the PRIORRES census excluded, plus every identity row (tiers
    TOPOLOGY, READMIT, T1_if_slip, T0_mw_detect, T0_gap, PHASE_ANCHOR, PBGAUGE, STRANSFORM)
    printed in full.
(b) the trip itself: the ``CLK_ANCHOR cta_decouple`` count, the in-epoch pass split, the sum
    of the absolute first-solve innovations ``e=``, the five largest, and, with ``--ref``,
    the slope through the origin and the correlation of this run's pattern against the
    reference run's over the satellites they share.  The run is then scanned for any
    other epoch carrying ``--mass-n`` or more decouples (an arm may move the event) and each
    is printed the same way.
(c) the post-fit refloat count (``T4_attrib``) over ``T .. T+120`` and per hour of the run.
(d) the ``PRIORRES`` row count at the trip epoch and at the epoch before it.
(e) the satellite code-bias steps of ``network_sat_bias_states.csv`` across
    ``T-30 -> T -> T+30``: per signal family the median step over the satellites present at
    both epochs, and the three largest de-medianed steps (standing invariant #5: only the
    between-satellite contrast is datum-invariant).
(f) the frozen consumer statistic: PRE = mean 3D over ``T-300 .. T-30`` (10 epochs),
    PEAK = max over ``T .. T+120``, PLATEAU = mean over ``T+300 .. T+2250``,
    cost = PLATEAU - PRE, with the E/N/U means of the PRE and PLATEAU windows.
(g) for every moved event at ``T*``: PRE = the 10 epochs before ``T*``, PEAK = ``T* .. T*+120``,
    POST = mean over ``T*+300 .. T*+1200``.

Usage:
    python scripts/ev7_event_read.py runs/_rap_r6_dual
    python scripts/ev7_event_read.py runs/_rap_v1_g30_early --ref runs/_rap_r6_dual
    python scripts/ev7_event_read.py runs/_rf0hv5_field6 --tow 278520
"""
import argparse
import csv
import math
import os
import statistics
from collections import Counter, defaultdict

IDENTITY_TIERS = ("TOPOLOGY", "READMIT", "T1_if_slip", "T0_mw_detect", "T0_gap",
                  "PHASE_ANCHOR", "PBGAUGE", "STRANSFORM")
CODE_FAMILIES = ("C1C", "C1W", "C2W")


# --------------------------------------------------------------------------- readers
def read_events(run_dir):
    """tow -> list of rows, in file order."""
    out = defaultdict(list)
    path = os.path.join(run_dir, "logs", "network_events.csv")
    if not os.path.exists(path):
        return out
    with open(path, encoding="utf-8", errors="replace") as fh:
        for r in csv.DictReader(fh):
            try:
                out[int(round(float(r["tow"])))].append(r)
            except (TypeError, ValueError, KeyError):
                continue
    return out


def read_consumer(run_dir):
    out = {}
    path = os.path.join(run_dir, "logs", "closed_loop_trace.csv")
    if not os.path.exists(path):
        return out
    with open(path, encoding="utf-8", errors="replace") as fh:
        for r in csv.DictReader(fh):
            try:
                out[int(round(float(r["tow"])))] = r
            except (TypeError, ValueError):
                continue
    return out


def read_code_bias(run_dir, tows):
    """(tow, sat, signal) -> code_bias_m, restricted to `tows` so the whole file is not held."""
    out = {}
    path = os.path.join(run_dir, "logs", "network_sat_bias_states.csv")
    if not os.path.exists(path):
        return out
    want = set(tows)
    with open(path, encoding="utf-8", errors="replace") as fh:
        for r in csv.DictReader(fh):
            try:
                t = int(round(float(r["tow"])))
            except (TypeError, ValueError, KeyError):
                continue
            if t in want:
                try:
                    out[(t, r["sat"], r["signal"])] = float(r["code_bias_m"])
                except (TypeError, ValueError):
                    pass
    return out


def kv(detail, key, cast=float):
    """Parse `key=value` out of an event's `detail` field; None when absent."""
    for part in detail.split(";"):
        if part.startswith(key + "="):
            try:
                return cast(part[len(key) + 1:])
            except ValueError:
                return None
    return None


# --------------------------------------------------------------------------- pieces
def decouples(events, t):
    return [r for r in events.get(t, []) if r["event"] == "cta_decouple"]


def trip_block(events, t, ref_events, label, ref_tow):
    """(b)/(g) core: the trip's identity numbers at epoch `t`, and the fit against `ref`.

    A moved event is fitted against the reference run's own trip epoch (`ref_tow`), because
    the question the fit answers is whether the arm reproduced the same satellite pattern
    somewhere else, not whether the reference also tripped at the moved epoch.
    """
    rows = decouples(events, t)
    if not rows:
        print(f"\n  TOW {t} ({label}): no cta_decouple row")
        return
    e = {r["sat"]: kv(r["detail"], "e") for r in rows}
    e = {s: v for s, v in e.items() if v is not None}
    passes = Counter(kv(r["detail"], "pass", int) for r in rows)
    reasons = Counter(kv(r["detail"], "reason", str) for r in rows)
    tot = sum(abs(v) for v in e.values())
    print(f"\n  TOW {t} ({label}): {len(rows)} cta_decouple  "
          f"reasons {dict(reasons)}  sum|e| = {tot:.4f} m")
    print("      pass split: " + "  ".join(f"pass {k}: {v}" for k, v in sorted(passes.items(),
                                                                               key=lambda x: (x[0] is None, x[0]))))
    top = sorted(e.items(), key=lambda kvp: -abs(kvp[1]))[:5]
    print("      largest |e|: " + "  ".join(f"{s} {v:+.4f}" for s, v in top))
    if ref_events is not None:
        rrows = decouples(ref_events, ref_tow) or decouples(ref_events, t)
        rref = {r["sat"]: kv(r["detail"], "e") for r in rrows}
        rref = {s: v for s, v in rref.items() if v is not None}
        common = sorted(set(e) & set(rref))
        if len(common) >= 3:
            x = [rref[s] for s in common]
            y = [e[s] for s in common]
            slope = sum(a * b for a, b in zip(x, y)) / sum(a * a for a in x)
            mx, my = statistics.fmean(x), statistics.fmean(y)
            sxy = sum((a - mx) * (b - my) for a, b in zip(x, y))
            sxx = sum((a - mx) ** 2 for a in x)
            syy = sum((b - my) ** 2 for b in y)
            corr = sxy / math.sqrt(sxx * syy) if sxx > 0 and syy > 0 else float("nan")
            print(f"      vs reference over {len(common)} common satellites: "
                  f"slope(run on ref, through origin) = {slope:.3f}, corr = {corr:.3f}")
        else:
            print(f"      vs reference: only {len(common)} common satellites, not fitted")


def sec_a(events, t):
    lo, hi = t - 60, t + 120
    print(f"\n(a) EVENT ROWS  TOW {lo}..{hi}   (PRIORRES excluded from the count)")
    cnt = Counter()
    for tt in sorted(events):
        if lo <= tt <= hi:
            for r in events[tt]:
                if r["tier"] != "PRIORRES":
                    cnt[(tt, r["tier"], r["event"])] += 1
    for (tt, tier, ev), n in sorted(cnt.items()):
        print(f"   {tt:8d}  {tier:<14} {ev:<22} {n:4d}")
    print(f"\n    IDENTITY ROWS ({', '.join(IDENTITY_TIERS)})")
    any_row = False
    for tt in sorted(events):
        if not (lo <= tt <= hi):
            continue
        for r in events[tt]:
            if r["tier"] not in IDENTITY_TIERS:
                continue
            any_row = True
            who = "/".join(x for x in (r["sat"], r["station"]) if x)
            print(f"   {tt:8d}  {r['tier']:<14} {r['event']:<22} {who:<26} "
                  f"stat={r['statistic']:<11} {r['action']:<14} {r['detail']}")
    if not any_row:
        print("   (none)")


def sec_b(events, t, ref_events, mass_n):
    print(f"\n(b) THE TRIP  (CLK_ANCHOR cta_decouple), reference = "
          f"{'none' if ref_events is None else 'given'}")
    trip_block(events, t, ref_events, "the card's trip epoch", t)
    moved = [tt for tt in sorted(events)
             if tt != t and len(decouples(events, tt)) >= mass_n]
    if moved:
        print(f"\n    OTHER EPOCHS WITH >= {mass_n} DECOUPLES (the event may have MOVED):")
        for tt in moved:
            trip_block(events, tt, ref_events, "moved event", t)
    else:
        print(f"\n    no other epoch in the run carries >= {mass_n} decouples")
    return moved


def sec_c(events, t):
    lo, hi = t, t + 120
    n = sum(1 for tt in events if lo <= tt <= hi for r in events[tt] if r["tier"] == "T4_attrib")
    print(f"\n(c) POST-FIT REFLOATS  T4_attrib rows in {lo}..{hi}: {n}")
    tows = [tt for tt in events]
    if not tows:
        return n
    t0 = min(tows)
    per = Counter()
    for tt in events:
        h = int((tt - t0) // 3600)
        per[h] += sum(1 for r in events[tt] if r["tier"] == "T4_attrib")
    print("    per hour of the run: " + "  ".join(f"h{h}:{per.get(h, 0)}" for h in sorted(per)))
    return n


def sec_d(events, t):
    a = sum(1 for r in events.get(t, []) if r["tier"] == "PRIORRES")
    b = sum(1 for r in events.get(t - 30, []) if r["tier"] == "PRIORRES")
    sta = len({r["station"] for r in events.get(t, []) if r["tier"] == "PRIORRES" and r["station"]})
    print(f"\n(d) PRIOR RESIDUALS  PRIORRES rows: {t} -> {a} (at {sta} stations);  {t - 30} -> {b}")
    return a, b


def sec_e(run_dir, t):
    tows = (t - 30, t, t + 30)
    cb = read_code_bias(run_dir, tows)
    print(f"\n(e) SATELLITE CODE-BIAS STEPS  network_sat_bias_states.csv, "
          f"{tows[0]} -> {tows[1]} -> {tows[2]}  (metres)")
    if not cb:
        print("    (no network_sat_bias_states.csv)")
        return {}
    out = {}
    print(f"    {'step':<18} {'sig':<5} {'n':>4} {'median':>9}   three largest de-medianed")
    for a, b in ((tows[0], tows[1]), (tows[1], tows[2])):
        for sig in CODE_FAMILIES:
            sats = sorted({s for (tt, s, g) in cb if g == sig and tt == a}
                          & {s for (tt, s, g) in cb if g == sig and tt == b})
            d = {s: cb[(b, s, sig)] - cb[(a, s, sig)] for s in sats}
            if not d:
                continue
            med = statistics.median(d.values())
            top = sorted(((v - med, s) for s, v in d.items()), key=lambda x: -abs(x[0]))[:3]
            out[(a, b, sig)] = (med, top)
            print(f"    {f'{a}->{b}':<18} {sig:<5} {len(d):>4} {med:>9.4f}   "
                  + "  ".join(f"{s} {v:+.4f}" for v, s in top))
    return out


def _win(con, lo, hi, key="pos_err_3d_m"):
    return [float(con[tt][key]) for tt in sorted(con) if lo <= tt <= hi and con[tt].get(key)]


def sec_f(con, t):
    pre_lo, pre_hi = t - 300, t - 30
    pk_lo, pk_hi = t, t + 120
    pl_lo, pl_hi = t + 300, t + 2250
    pre = _win(con, pre_lo, pre_hi)
    pk = _win(con, pk_lo, pk_hi)
    pl = _win(con, pl_lo, pl_hi)
    m = lambda v: statistics.fmean(v) if v else float("nan")
    pre_m, pl_m = m(pre), m(pl)
    print(f"\n(f) CONSUMER (frozen statistic)  closed_loop_trace.csv")
    print(f"    PRE     {pre_lo}..{pre_hi}   n={len(pre):3d}  mean 3D = {pre_m:.4f}")
    print(f"    PEAK    {pk_lo}..{pk_hi}   n={len(pk):3d}  max  3D = {(max(pk) if pk else float('nan')):.4f}")
    print(f"    PLATEAU {pl_lo}..{pl_hi}  n={len(pl):3d}  mean 3D = {pl_m:.4f}")
    print(f"    cost = PLATEAU - PRE = {pl_m - pre_m:+.4f}")
    for name, lo, hi in (("PRE", pre_lo, pre_hi), ("PLATEAU", pl_lo, pl_hi)):
        e, n, u = (_win(con, lo, hi, k) for k in ("east_m", "north_m", "up_m"))
        print(f"    {name:<8} E {m(e):+.4f}  N {m(n):+.4f}  U {m(u):+.4f}")
    return pre_m, (max(pk) if pk else float("nan")), pl_m


def sec_g(con, moved):
    if not moved:
        return
    print("\n(g) CONSUMER AROUND EACH MOVED EVENT")
    m = lambda v: statistics.fmean(v) if v else float("nan")
    for ts in moved:
        pre = _win(con, ts - 300, ts - 30)
        pk = _win(con, ts, ts + 120)
        post = _win(con, ts + 300, ts + 1200)
        print(f"    T* {ts}:  PRE(10 ep) {m(pre):.4f}   PEAK({ts}..{ts + 120}) "
              f"{(max(pk) if pk else float('nan')):.4f}   POST({ts + 300}..{ts + 1200}) {m(post):.4f}"
              f"   POST-PRE {m(post) - m(pre):+.4f}")


# --------------------------------------------------------------------------- main
def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("run_dir")
    ap.add_argument("--ref", help="a reference run directory for the innovation-pattern fit")
    ap.add_argument("--tow", type=int, default=278520, help="the trip epoch (default 278520)")
    ap.add_argument("--mass-n", type=int, default=15,
                    help="decouples in one epoch that count as an all-satellite event (default 15)")
    a = ap.parse_args()

    events = read_events(a.run_dir)
    con = read_consumer(a.run_dir)
    ref_events = read_events(a.ref) if a.ref else None
    print(f"##### EV7 read: {a.run_dir}   trip epoch {a.tow}   "
          f"event epochs {len(events)}   consumer epochs {len(con)}"
          + (f"   ref {a.ref}" if a.ref else ""))
    sec_a(events, a.tow)
    moved = sec_b(events, a.tow, ref_events, a.mass_n)
    sec_c(events, a.tow)
    sec_d(events, a.tow)
    sec_e(a.run_dir, a.tow)
    sec_f(con, a.tow)
    sec_g(con, moved)


if __name__ == "__main__":
    main()
