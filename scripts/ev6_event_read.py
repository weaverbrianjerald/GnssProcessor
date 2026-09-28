#!/usr/bin/env python3
"""EV6-read: the hour-6 Q3 mass-decouple event on the RF-0hv float vehicle (a diagnostic).

Pre-registered in ``docs/refound/EV6_hour6_decouple_event_2026-09-15.md``. Trace-only: it reads a
finished run directory and writes nothing but stdout (no plotting, no run, no solve).

Five sections, in the order the card's read (a)-(d) asks for them:

(a) ``--section consumer``   per-epoch closed-loop rows in the window: 3D / E / N / U / n_sats and
                             the per-epoch delta, so a step and a ramp are distinguishable by eye.
(b) ``--section events``     every ``network_events.csv`` row in the window with its identity and
                             detail. The two census families (PRIORRES / MDB) are summarised as a
                             count per epoch unless ``--all-families``; everything else is printed.
(c) ``--section wire``       the emitted wire. Per satellite per epoch: the de-commoned change of
                             the satellite clock, of the consumer's IF code correction
                             ``-clk + a1*b(C1C) + a2*b(C2W)`` and of its IF phase correction
                             ``-clk + a1*B(L1x) + a2*B(L2x)``, each against the median change over
                             the satellites present at both epochs (standing invariant #5: the
                             network-common part cancels in the consumer clock and must not be
                             graded). Every cell carries whether the epoch declared that satellite
                             (a ``disc_counter`` bump or an ``R`` record), so an undeclared step is
                             visible as such.
(d) ``--section census``     the whole-run CLK_ANCHOR census: one row per decouple epoch with its
                             satellite count, the trip reasons, the in-epoch pass depth, the max
                             de-commoned wire excursion it created, how much of that excursion
                             reversed at the next epoch (the return), and the consumer before /
                             peak / after. This is the size-vs-step instrument RF-0hv s7.3 asked
                             for, with the reversal column that names what the size alone misses.
    ``--section undeclared`` the wire-contract ledger: every (satellite, epoch) whose de-commoned IF
                             code or phase correction moved more than ``--step-m`` with no
                             declaration, tagged with whether the satellite was loose (decoupled and
                             not yet re-coupled) at that epoch.

Usage:
    python scripts/ev6_event_read.py runs/_rfpdfix_field6_both --tow-min 276600 --tow-max 277500
    python scripts/ev6_event_read.py runs/_rfpdfix_field6_both --section census
    python scripts/ev6_event_read.py runs/_rfpdfix_field6_both --section undeclared --step-m 0.10
"""
import argparse
import csv
import os
import statistics
from collections import Counter, defaultdict

F1, F2 = 1575.42e6, 1227.60e6
A1, A2 = F1 * F1 / (F1 * F1 - F2 * F2), -F2 * F2 / (F1 * F1 - F2 * F2)

# Signal pairs the consumer's IF combination is formed from, most preferred first.
CODE_PAIRS = (("C1C", "C2W"), ("C1W", "C2W"), ("C1C", "C2L"))
PHASE_PAIRS = (("L1W", "L2W"), ("L1C", "L2W"), ("L1C", "L2L"))

# Census families that are one row per epoch per family and would drown the event table.
CENSUS_FAMILIES = ("PRIORRES", "MDB", "CONGRUENCE", "REDUNDANCY")


# --------------------------------------------------------------------------- readers
def read_products(run_dir):
    """tow -> sat -> dict(clk=, code={sig: m}, phase={sig: m}, declared=bool)."""
    per = defaultdict(dict)
    tow = None
    path = os.path.join(run_dir, "products.txt")
    with open(path, encoding="utf-8", errors="replace") as fh:
        for ln in fh:
            if not ln or ln[0] == "#":
                continue
            f = ln.rstrip("\n").split(",")
            tag = f[0]
            if tag == "E":
                tow = int(round(float(f[2])))
            elif tow is None:
                continue
            elif tag == "C":
                per[tow].setdefault(f[1], _blank())["clk"] = float(f[2])
            elif tag == "B" and len(f) >= 4:
                per[tow].setdefault(f[1], _blank())["code"][f[2]] = float(f[3])
            elif tag == "P" and len(f) >= 4:
                per[tow].setdefault(f[1], _blank())["phase"][f[2]] = float(f[3])
            elif tag == "R":
                # B1 per-satellite discontinuity bump: every satellite named here is declared.
                for s in f[1:]:
                    if s:
                        per[tow].setdefault(s, _blank())["declared"] = True
    return per, sorted(per)


def _blank():
    return {"clk": None, "code": {}, "phase": {}, "declared": False}


def read_disc_counters(run_dir):
    """tow -> sat -> max disc_counter over that satellite's records (the solution-CSV view)."""
    out = defaultdict(dict)
    path = os.path.join(run_dir, "logs", "network_solution.csv")
    if not os.path.exists(path):
        return out
    with open(path, encoding="utf-8", errors="replace") as fh:
        for r in csv.DictReader(fh):
            try:
                t = int(round(float(r["tow"])))
                d = int(r["disc_counter"])
            except (TypeError, ValueError, KeyError):
                continue
            s = r["sat"]
            if d > out[t].get(s, -1):
                out[t][s] = d
    return out


def read_events(run_dir):
    """tow -> list of event dicts (raw rows, in file order)."""
    out = defaultdict(list)
    path = os.path.join(run_dir, "logs", "network_events.csv")
    with open(path, encoding="utf-8", errors="replace") as fh:
        for r in csv.DictReader(fh):
            try:
                out[int(round(float(r["tow"])))].append(r)
            except (TypeError, ValueError):
                continue
    return out


def read_consumer(run_dir):
    out = {}
    path = os.path.join(run_dir, "logs", "closed_loop_trace.csv")
    with open(path, encoding="utf-8", errors="replace") as fh:
        for r in csv.DictReader(fh):
            out[int(round(float(r["tow"])))] = r
    return out


def read_epoch_status(run_dir):
    out = {}
    path = os.path.join(run_dir, "logs", "network_epoch_status.csv")
    if not os.path.exists(path):
        return out
    with open(path, encoding="utf-8", errors="replace") as fh:
        for r in csv.DictReader(fh):
            out[int(round(float(r["tow"])))] = r
    return out


# --------------------------------------------------------------------------- wire algebra
def _combo(rec, pairs, kind):
    """(-clk + a1*b(s1) + a2*b(s2), pair) for the first available signal pair, else (None, None).

    The pair travels with the value because a satellite whose available pair changes between epochs
    has no comparable difference: the two combinations are different linear functionals of a
    different state, and differencing them manufactures a step of tens of metres out of nothing.
    """
    if rec["clk"] is None:
        return None, None
    d = rec[kind]
    for s1, s2 in pairs:
        if s1 in d and s2 in d:
            return -rec["clk"] + A1 * d[s1] + A2 * d[s2], (s1, s2)
    return None, None


def wire_deltas(per, tp, t):
    """sat -> (dclk, dcode, dphase), each de-commoned by the median over satellites in both epochs.

    Standing invariant #5: only the between-satellite contrast is datum-invariant; the common part is
    the consumer's own clock and must be removed before anything is graded.
    """
    a, b = per.get(tp, {}), per.get(t, {})
    both = [s for s in b if s in a]
    raw = {}
    for s in both:
        ra, rb = a[s], b[s]
        dclk = (rb["clk"] - ra["clk"]) if (ra["clk"] is not None and rb["clk"] is not None) else None
        ca, ka = _combo(ra, CODE_PAIRS, "code")
        cb, kb = _combo(rb, CODE_PAIRS, "code")
        pa, qa = _combo(ra, PHASE_PAIRS, "phase")
        pb, qb = _combo(rb, PHASE_PAIRS, "phase")
        raw[s] = (dclk,
                  (cb - ca) if (ca is not None and cb is not None and ka == kb) else None,
                  (pb - pa) if (pa is not None and pb is not None and qa == qb) else None)
    meds = []
    for i in range(3):
        v = [raw[s][i] for s in both if raw[s][i] is not None]
        meds.append(statistics.median(v) if v else 0.0)
    return {s: tuple(None if raw[s][i] is None else raw[s][i] - meds[i] for i in range(3))
            for s in both}, meds


def declared_at(per, disc, tp, t, sat):
    """Did the emitted wire declare this satellite at epoch t?

    The ``R`` record is the only discontinuity channel products.txt actually carries: measured on
    this lineage the ``C`` records are 3 fields (no ``clock_disc`` even with
    ``network.emit_clock_disc``), ``B`` 4 (no ``code_disc``) and ``P`` 6 (no ``phase_disc``). The
    per-record ``disc_counter`` of ``network_solution.csv`` is an internal trace: it also counts
    code-bias re-realizations that never reach a consumer, so it is reported separately
    (@ref internal_bump) and never treated as a declaration.
    """
    _ = (tp, disc)   # kept in the signature: the internal view is one call away, see internal_bump
    return bool(per.get(t, {}).get(sat, {}).get("declared"))


def internal_bump(disc, tp, t, sat):
    """Did any per-record disc_counter of this satellite move? (network_solution.csv, internal.)"""
    a, b = disc.get(tp, {}).get(sat), disc.get(t, {}).get(sat)
    return a is not None and b is not None and b != a


def loose_spans(events):
    """sat -> list of (t_decouple, t_recouple_or_inf) from the CLK_ANCHOR state machine."""
    open_at, spans = {}, defaultdict(list)
    for t in sorted(events):
        for r in events[t]:
            if r["event"] == "cta_decouple":
                open_at.setdefault(r["sat"], t)
            elif r["event"] == "cta_recouple" and r["sat"] in open_at:
                spans[r["sat"]].append((open_at.pop(r["sat"]), t))
    for s, t in open_at.items():
        spans[s].append((t, float("inf")))
    return spans


def is_loose(spans, sat, t):
    return any(t0 < t < t1 for t0, t1 in spans.get(sat, ()))


# --------------------------------------------------------------------------- sections
def sec_consumer(con, lo, hi):
    print("\n(a) CONSUMER — closed_loop_trace.csv")
    print(f"{'tow':>8} {'3D_m':>7} {'d3D':>7} {'east_m':>8} {'north_m':>8} {'up_m':>8} {'n_sats':>6} {'ar_gate':>9}")
    prev = None
    for t in sorted(con):
        if not (lo <= t <= hi):
            continue
        r = con[t]
        e = float(r["pos_err_3d_m"])
        d = "" if prev is None else f"{e - prev:+.3f}"
        print(f"{t:8d} {e:7.3f} {d:>7} {float(r['east_m']):8.3f} {float(r['north_m']):8.3f} "
              f"{float(r['up_m']):8.3f} {int(r['n_sats']):6d} {r.get('ar_gate', ''):>9}")
        prev = e


def sec_events(events, status, lo, hi, all_families):
    print("\n(b) PRODUCER EVENTS — network_events.csv (census families summarised unless --all-families)")
    for t in sorted(events):
        if not (lo <= t <= hi):
            continue
        rows, census = [], Counter()
        for r in events[t]:
            if r["tier"] in CENSUS_FAMILIES and not all_families:
                census[r["tier"]] += 1
            else:
                rows.append(r)
        st = status.get(t, {})
        head = (f"  n_present={st.get('n_present','?')} n_clk={st.get('n_clock_sats','?')} "
                f"s0_pf_phase={st.get('sigma0_pf_phase','?')} s0_pf_code={st.get('sigma0_pf_code','?')} "
                f"n_arc_refloat={st.get('n_arc_refloat','?')} datum_changed={st.get('datum_changed','?')}")
        if not rows and not census:
            continue
        print(f"\nTOW {t}{head}")
        if census:
            print("   [census] " + "  ".join(f"{k}x{v}" for k, v in sorted(census.items())))
        for r in rows:
            who = "/".join(x for x in (r["sat"], r["station"], r["signal"] if "signal" in r else "") if x)
            print(f"   {r['tier']:<14} {r['event']:<20} {who:<22} stat={r['statistic']:<14} "
                  f"thr={r['threshold']:<10} {r['action']:<16} {r['detail']}")


def sec_wire(per, disc, events, lo, hi, step_m, top):
    print(f"\n(c) WIRE — de-commoned per-satellite change, |d| > {step_m} m")
    print("    DECL = an R record named this satellite (the wire's only discontinuity channel);"
          " INT = an internal disc_counter moved")
    print(f"{'tow':>8} {'sat':>4} {'dCLK_m':>8} {'dIFcode_m':>10} {'dIFphase_m':>11} {'DECL':>5} {'INT':>4} {'LOOSE':>6}")
    spans = loose_spans(events)
    tows = sorted(per)
    for i in range(1, len(tows)):
        tp, t = tows[i - 1], tows[i]
        if not (lo <= t <= hi):
            continue
        dd, _ = wire_deltas(per, tp, t)
        def mag(kv):
            vals = [abs(v) for v in kv[1] if v is not None]
            return -max(vals) if vals else 0.0

        pick = sorted(dd.items(), key=mag)
        n = 0
        for s, (dc, dco, dph) in pick:
            vals = [abs(v) for v in (dc, dco, dph) if v is not None]
            if not vals or max(vals) <= step_m or n >= top:
                continue
            n += 1
            cell = lambda v, w: (" " * (w - 2) + "--") if v is None else f"{v:{w}.3f}"
            print(f"{t:8d} {s:>4} {cell(dc, 8)} {cell(dco, 10)} {cell(dph, 11)} "
                  f"{('yes' if declared_at(per, disc, tp, t, s) else 'NO'):>5} "
                  f"{('y' if internal_bump(disc, tp, t, s) else '-'):>4} "
                  f"{('yes' if is_loose(spans, s, t) else '-'):>6}")


def sec_census(per, disc, events, con, status, window_ep):
    print("\n(d) CLK_ANCHOR DECOUPLE CENSUS — one row per decouple EPOCH (whole run)")
    print("    maxEx = largest de-commoned wire CLOCK excursion the epoch created on a tripped satellite")
    print("    rev%  = how much of that excursion REVERSED at the very next epoch (undeclared if DECL=NO)")
    tows = sorted(per)
    idx = {t: i for i, t in enumerate(tows)}
    ctows = sorted(con)
    cidx = {t: i for i, t in enumerate(ctows)}
    dec = defaultdict(list)
    for t in events:
        for r in events[t]:
            if r["event"] == "cta_decouple":
                dec[t].append((r["sat"], r["detail"]))
    print(f"{'tow':>8} {'n':>3} {'reasons':<12} {'pass':>4} {'maxEx_m':>8} {'rev%':>5} {'revDECL':>7} "
          f"{'arcRF':>5} {'con_pre':>7} {'con_pk':>7} {'con_post':>8} {'verdict':>8}")
    for t in sorted(dec):
        i = idx.get(t)
        if i is None or i == 0:
            continue
        tp = tows[i - 1]
        tn = tows[i + 1] if i + 1 < len(tows) else None
        sats = [s for s, _ in dec[t]]
        reasons = Counter(d.split(";")[0].replace("reason=", "") for _, d in dec[t])
        passes = 1 + max((int(x.split("pass=")[1].split(";")[0]) for _, x in dec[t] if "pass=" in x),
                         default=0)
        dd, _ = wire_deltas(per, tp, t)
        ex = {s: dd[s][0] for s in sats if s in dd and dd[s][0] is not None}
        max_s = max(ex, key=lambda s: abs(ex[s])) if ex else None
        maxex = abs(ex[max_s]) if max_s else 0.0
        rev, rev_decl = float("nan"), "-"
        if tn is not None and max_s:
            dn, _ = wire_deltas(per, t, tn)
            if max_s in dn and dn[max_s][0] is not None and maxex > 0:
                rev = -100.0 * dn[max_s][0] / ex[max_s]
                rev_decl = "yes" if declared_at(per, disc, t, tn, max_s) else "NO"
        ci = cidx.get(t)
        pre = pk = post = float("nan")
        if ci is not None and ci > 0:
            pre = float(con[ctows[ci - 1]]["pos_err_3d_m"])
            fut = [float(con[x]["pos_err_3d_m"]) for x in ctows[ci:ci + window_ep + 1]]
            if fut:
                pk, post = max(fut), fut[-1]
        verdict = "-"
        if pre == pre and post == post:
            verdict = "DAMAGE" if post - pre > 1.0 else ("ok" if post - pre < 0.5 else "mixed")
        arc = status.get(t, {}).get("n_arc_refloat", "?")
        print(f"{t:8d} {len(sats):3d} {','.join(f'{k}:{v}' for k, v in reasons.items()):<12} {passes:4d} "
              f"{maxex:8.3f} {rev:5.0f} {rev_decl:>7} {str(arc):>5} {pre:7.2f} {pk:7.2f} {post:8.2f} {verdict:>8}")


def sec_undeclared(per, disc, events, lo, hi, step_m):
    print(f"\n(e) UNDECLARED-STEP LEDGER — |de-commoned dIFcode| or |dIFphase| > {step_m} m with NO declaration")
    spans = loose_spans(events)
    tows = sorted(per)
    n_tot = n_undecl = n_loose = 0
    worst = []
    for i in range(1, len(tows)):
        tp, t = tows[i - 1], tows[i]
        if not (lo <= t <= hi):
            continue
        dd, _ = wire_deltas(per, tp, t)
        for s, (dc, dco, dph) in dd.items():
            big = max(abs(v) for v in (dco, dph) if v is not None) if any(
                v is not None for v in (dco, dph)) else 0.0
            if big <= step_m:
                continue
            n_tot += 1
            decl = declared_at(per, disc, tp, t, s)
            lo_ose = is_loose(spans, s, t)
            if not decl:
                n_undecl += 1
                if lo_ose:
                    n_loose += 1
                worst.append((big, t, s, dc, dco, dph, lo_ose))
    print(f"  steps > {step_m} m: {n_tot};  UNDECLARED {n_undecl} ({n_undecl / max(n_tot, 1):.0%});  "
          f"of those, {n_loose} on a satellite that was LOOSE (decoupled, not yet re-coupled)")
    worst.sort(reverse=True)
    print(f"{'tow':>8} {'sat':>4} {'dCLK_m':>8} {'dIFcode_m':>10} {'dIFphase_m':>11} {'LOOSE':>6}")
    for big, t, s, dc, dco, dph, lo_ose in worst[:25]:
        f = lambda v: "      --" if v is None else f"{v:8.3f}"
        print(f"{t:8d} {s:>4} {f(dc)} {('' if dco is None else f'{dco:10.3f}'):>10} "
              f"{('' if dph is None else f'{dph:11.3f}'):>11} {('yes' if lo_ose else '-'):>6}")


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("run_dir")
    ap.add_argument("--tow-min", type=float, default=0.0)
    ap.add_argument("--tow-max", type=float, default=1e12)
    ap.add_argument("--section", default="all",
                    choices=("all", "consumer", "events", "wire", "census", "undeclared"))
    ap.add_argument("--step-m", type=float, default=0.05, help="wire-step threshold (m)")
    ap.add_argument("--top", type=int, default=12, help="max satellites per epoch in the wire table")
    ap.add_argument("--window-ep", type=int, default=20, help="census consumer window (epochs)")
    ap.add_argument("--all-families", action="store_true",
                    help="print the PRIORRES/MDB/CONGRUENCE/REDUNDANCY census rows too")
    a = ap.parse_args()

    lo, hi = int(a.tow_min), int(a.tow_max)
    per, _ = read_products(a.run_dir)
    disc = read_disc_counters(a.run_dir)
    events = read_events(a.run_dir)
    con = read_consumer(a.run_dir)
    status = read_epoch_status(a.run_dir)
    print(f"##### EV6 read: {a.run_dir}   emitted epochs {len(per)}   window [{lo}, {hi}]")

    if a.section in ("all", "consumer"):
        sec_consumer(con, lo, hi)
    if a.section in ("all", "events"):
        sec_events(events, status, lo, hi, a.all_families)
    if a.section in ("all", "wire"):
        sec_wire(per, disc, events, lo, hi, a.step_m, a.top)
    if a.section in ("all", "census"):
        sec_census(per, disc, events, con, status, a.window_ep)
    if a.section in ("all", "undeclared"):
        sec_undeclared(per, disc, events, 0, 10 ** 12, max(a.step_m, 0.05))


if __name__ == "__main__":
    main()
