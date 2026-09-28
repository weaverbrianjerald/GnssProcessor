#!/usr/bin/env python3
"""R10: an M4 contract audit for the wire's unsignalled steps (SPEC_event_harness s24.19).

    python scripts/wire_step_audit.py [--runs C:/GnssProcessor/runs]
        [--arms _cm_pvb,_cm_c8b,_cm_c10,_gl_on6,_gl2_on6,_cm_pvb6,_cm_g4a,_cm_g4b]
        [--tow0 259200] [--attrib-m 2.0] [--json out.json]

The contract: a network product's per-satellite clock and phase bias are only
integrable by a consumer across an epoch boundary if every re-realization of them
is signalled, which is what the per-key discontinuity counter in
`network_solution.csv` is for, and what `NetworkProductEmitter::markSatDiscontinuity`
bumps.  An emitted step with no counter increment is a silent frame change: the
consumer integrates straight across it and its position moves by the step.  This
script measures that population directly off the emitted products.

Measured, per arm:
  * per (satellite, epoch): |dclock| = |c0(t) - c0(t-1)| and |dPB| per signal,
    paired with whether that key's `disc_counter` incremented at the same epoch;
  * the count of steps over 0.5 / 2 / 10 m, split signalled vs unsignalled, per
    arm and per hour;
  * every unsignalled step over --attrib-m attributed to the nearest producer
    event within +-2 epochs (network_events.csv: CLK_ANCHOR cta_recouple /
    cta_decouple, PHASE_ANCHOR, GROSSLOS, RETRO, CLK_FRAME, CLK_STEP, datum and
    refloat rows), with the held-out consumer's own 3D step at that epoch from
    `closed_loop_trace.csv`.

The common mode is not removed here: the consumer integrates the raw emitted
value, so the raw step is what costs it, which is the contract this script
audits.  The per-epoch network median step is reported beside every flagged row
so a frame move (median large, deviation small) is not confused with a
per-satellite re-realization (median near zero, deviation large), the
distinction the emitter's own stage-2 test is written on.

The anchor read (card CS part B, s24.20) takes each unsignalled clock step
attributed to a `T4_attrib/attrib_arc` refloat of the same satellite in the same
epoch, which names a (station, satellite) arc, and asks whether the refloated
station was that satellite's elected phase anchor at the time: whether the
clock re-realization is the gauge moving under the satellite rather than a
generic arc refloat.

  * the incumbent anchor per (satellite, signal) is reconstructed by replaying the
    handover chain (`PBGAUGE/handover_cold|handover_continuous`, whose `sat` field
    is "<SAT>_<SIGNAL>" and whose `station` is the new anchor, and
    `PHASE_ANCHOR/anchor_handover`), so `anchors(sat, t)` is the set of stations
    holding any of that satellite's signals at epoch t: a set, not a scalar,
    because the signal families split-reference independently (s24.15 R6);
  * each flagged step is then labelled on-anchor / off-anchor by whether the
    refloated station is in that set;
  * and the satellite's phase bias keys are checked at the same epoch, so the
    report can state whether the PB held while the clock took the step.

Frozen (s24.20): more than 50 % on the anchor makes the anchor-election card the
structural fix; less than 20 % makes a "T4 refloat forces a clock mark" rule the fix.

Read-only: opens products and traces, writes nothing but its report.
"""
import argparse
import bisect
import csv
import json
import os
import statistics
import sys
from collections import defaultdict

try:
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
except Exception:
    pass

DT = 30.0
EP_PER_HOUR = 120
BANDS = (0.5, 2.0, 10.0)
# Producer events worth attributing a silent step to, by tier.
ATTRIB_TIERS = ("CLK_ANCHOR", "CLK_FRAME", "CLK_STEP", "RETRO", "GROSSLOS",
                "PHASE_ANCHOR", "PBGAUGE", "DATUMGRAPH", "CB_DATUM", "T4_attrib",
                "T3_stafactor", "ADMISSION", "RTA-10_perconst", "IONO_DW")


def read_solution(path):
    """(kind, sat, signal) -> [(tow, value, disc)], epoch-ordered."""
    series = defaultdict(list)
    if not os.path.exists(path):
        return series
    with open(path, encoding="utf-8", errors="replace", newline="") as f:
        for r in csv.DictReader(f):
            try:
                tow = float(r["tow"])
                val = float(r["value_m"])
                disc = int(r["disc_counter"])
            except (ValueError, KeyError, TypeError):
                continue
            series[(r["kind"], r["sat"], r.get("signal", ""))].append((tow, val, disc))
    for k in series:
        series[k].sort()
    return series


def read_events(path):
    """tow -> [(tier, event, sat, station, action, detail)] for the attributable tiers."""
    ev = defaultdict(list)
    if not os.path.exists(path):
        return ev
    with open(path, encoding="utf-8", errors="replace", newline="") as f:
        for r in csv.DictReader(f):
            if r.get("tier") not in ATTRIB_TIERS:
                continue
            try:
                tow = float(r["tow"])
            except (ValueError, KeyError, TypeError):
                continue
            ev[tow].append((r["tier"], r.get("event", ""), r.get("sat", ""),
                            r.get("station", ""), r.get("action", ""),
                            (r.get("detail", "") or "")[:60]))
    return ev


def read_anchor_chain(path):
    """[(tow, sat, signal, station)] handovers, epoch-ordered.

    Two writers, both replayed: `PBGAUGE/handover_cold|handover_continuous` names
    the key as "<SAT>_<SIGNAL>" in its sat column with the new anchor station in
    its station column, and `PHASE_ANCHOR/anchor_handover` names the satellite
    with the new anchor and "OLD->NEW" in detail.  The PHASE_ANCHOR row has no
    signal, so it applies to every signal of that satellite already seen.
    """
    out = []
    if not os.path.exists(path):
        return out
    with open(path, encoding="utf-8", errors="replace", newline="") as f:
        for r in csv.DictReader(f):
            tier, ev = r.get("tier"), r.get("event", "")
            if tier == "PBGAUGE" and ev.startswith("handover"):
                key, sta = r.get("sat", ""), r.get("station", "")
                if "_" in key and sta:
                    sat, _, sig = key.partition("_")
                    out.append((float(r["tow"]), sat, sig, sta))
            elif tier == "PHASE_ANCHOR" and ev == "anchor_handover":
                sat, sta = r.get("sat", ""), r.get("station", "")
                if sat and sta:
                    out.append((float(r["tow"]), sat, "*", sta))
    out.sort()
    return out


def anchors_at(chain, sat, tow):
    """Set of stations holding any signal of `sat` at `tow` (replay to that epoch)."""
    cur = {}
    for t, s2, sig, sta in chain:
        if t > tow:
            break
        if s2 != sat:
            continue
        if sig == "*":
            if cur:
                for k in list(cur):
                    cur[k] = sta
            else:
                cur["*"] = sta
        else:
            cur[sig] = sta
    return set(cur.values())


def read_consumer(path):
    """tow -> (3D error, |d3D| against the previous epoch)."""
    out = {}
    if not os.path.exists(path):
        return out
    rows = []
    with open(path, encoding="utf-8", errors="replace", newline="") as f:
        for r in csv.DictReader(f):
            try:
                rows.append((float(r["tow"]), float(r["pos_err_3d_m"])))
            except (ValueError, KeyError, TypeError):
                continue
    rows.sort()
    for i, (t, e) in enumerate(rows):
        out[t] = (e, abs(e - rows[i - 1][1]) if i else 0.0)
    return out


def audit_arm(rundir, tow0, attrib_m):
    sol = read_solution(os.path.join(rundir, "logs", "network_solution.csv"))
    if not sol:
        return None
    ev = read_events(os.path.join(rundir, "logs", "network_events.csv"))
    chain = read_anchor_chain(os.path.join(rundir, "logs", "network_events.csv"))
    # Raw T4 rows, for the anchor read: (tow, sat) -> the refloated station(s).
    t4 = defaultdict(set)
    evp = os.path.join(rundir, "logs", "network_events.csv")
    if os.path.exists(evp):
        with open(evp, encoding="utf-8", errors="replace", newline="") as f:
            for r in csv.DictReader(f):
                if r.get("tier") == "T4_attrib" and r.get("event") == "attrib_arc":
                    try:
                        t4[(float(r["tow"]), r.get("sat", ""))].add(r.get("station", ""))
                    except (ValueError, KeyError, TypeError):
                        pass
    cons = read_consumer(os.path.join(rundir, "logs", "closed_loop_trace.csv"))
    ev_tows = sorted(ev)

    # Per-epoch RAW clock steps, for the network median (frame move vs per-sat step).
    med_by_tow = {}
    clk_steps_by_tow = defaultdict(list)
    for (kind, sat, sig), pts in sol.items():
        if kind != "CLK":
            continue
        for i in range(1, len(pts)):
            if pts[i][0] - pts[i - 1][0] > 1.5 * DT:
                continue                      # a gap is not a one-epoch step
            clk_steps_by_tow[pts[i][0]].append(pts[i][1] - pts[i - 1][1])
    for t, v in clk_steps_by_tow.items():
        med_by_tow[t] = statistics.median(v) if v else 0.0

    # counts[kind][band][signalled?] and the same per hour
    counts = {k: {b: {True: 0, False: 0} for b in BANDS} for k in ("CLK", "PB")}
    hourly = defaultdict(lambda: {k: {b: {True: 0, False: 0} for b in BANDS}
                                  for k in ("CLK", "PB")})
    rows_seen = {"CLK": 0, "PB": 0}
    flagged = []

    for (kind, sat, sig), pts in sol.items():
        if kind not in ("CLK", "PB"):
            continue
        for i in range(1, len(pts)):
            t0, v0, d0 = pts[i - 1]
            t1, v1, d1 = pts[i]
            if t1 - t0 > 1.5 * DT:
                continue                      # gap: not an epoch-to-epoch step
            rows_seen[kind] += 1
            step = abs(v1 - v0)
            signalled = d1 != d0
            h = int((t1 - tow0) // (EP_PER_HOUR * DT)) + 1
            for b in BANDS:
                if step > b:
                    counts[kind][b][signalled] += 1
                    hourly[h][kind][b][signalled] += 1
            if step > attrib_m and not signalled:
                med = med_by_tow.get(t1, 0.0)
                # nearest attributable event within +- 2 epochs
                lo, hi = t1 - 2 * DT, t1 + 2 * DT
                near = []
                for tt in ev_tows[bisect.bisect_left(ev_tows, lo):
                                  bisect.bisect_right(ev_tows, hi)]:
                    for e in ev[tt]:
                        near.append((abs(tt - t1), tt, e))
                near.sort(key=lambda x: (x[0], x[2][0]))
                # prefer an event naming this satellite, else the nearest of any
                same = [n for n in near if n[2][2] == sat or n[2][2].startswith(sat)]
                pick = (same or near or [None])[0]
                c3, cs = cons.get(t1, (float("nan"), float("nan")))
                flagged.append({
                    "tow": t1, "hour": h, "kind": kind, "sat": sat, "signal": sig,
                    "step_m": round(step, 4), "signed_m": round(v1 - v0, 4),
                    "net_median_step_m": round(med, 4),
                    "dev_from_median_m": round((v1 - v0) - med, 4) if kind == "CLK" else None,
                    "disc": d1,
                    "near_dt_s": (pick[1] - t1) if pick else None,
                    "near_event": ("%s/%s %s %s" % (pick[2][0], pick[2][1],
                                                    pick[2][2] or "-", pick[2][4]))
                                  if pick else "NONE within +-2 ep",
                    "near_names_sat": bool(same),
                    "consumer_3d_m": None if c3 != c3 else round(c3, 3),
                    "consumer_step_m": None if cs != cs else round(cs, 3),
                })
                if kind == "CLK":
                    # --- the anchor read (card CS part B) -------------------
                    stas = set()
                    for dt in (0.0, -DT, DT, -2 * DT, 2 * DT):
                        stas |= t4.get((t1 + dt, sat), set())
                    anch = anchors_at(chain, sat, t1)
                    on = sorted(stas & anch)
                    flagged[-1].update({
                        "t4_stations": sorted(stas),
                        "anchor_stations": sorted(anch),
                        "on_anchor": bool(on),
                        "on_anchor_stations": on,
                    })
                    # --- did the PB hold while the clock stepped? -----------
                    pbmax, pbsig = 0.0, ""
                    for (k2, s2, g2), p2 in sol.items():
                        if k2 != "PB" or s2 != sat:
                            continue
                        j = bisect.bisect_left([x[0] for x in p2], t1)
                        if 0 < j < len(p2) and p2[j][0] == t1 \
                                and p2[j][0] - p2[j - 1][0] <= 1.5 * DT:
                            d2 = abs(p2[j][1] - p2[j - 1][1])
                            if d2 > pbmax:
                                pbmax, pbsig = d2, g2
                    flagged[-1]["pb_max_step_m"] = round(pbmax, 4)
                    flagged[-1]["pb_worst_signal"] = pbsig
    flagged.sort(key=lambda r: -r["step_m"])
    return {"counts": counts, "hourly": dict(hourly), "rows": rows_seen,
            "flagged": flagged}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--runs", default="C:/GnssProcessor/runs")
    ap.add_argument("--arms", default="_cm_pvb,_cm_c8b,_cm_c10,_gl_on6,_gl2_on6,"
                                      "_cm_pvb6,_cm_g4a,_cm_g4b")
    ap.add_argument("--tow0", type=float, default=259200.0)
    ap.add_argument("--attrib-m", type=float, default=2.0)
    ap.add_argument("--max-list", type=int, default=40)
    ap.add_argument("--json", default="")
    a = ap.parse_args()

    out = {}
    for arm in a.arms.split(","):
        arm = arm.strip()
        if not arm:
            continue
        res = audit_arm(os.path.join(a.runs, arm), a.tow0, a.attrib_m)
        if res is None:
            print("%-10s  no network_solution.csv" % arm)
            continue
        out[arm] = res

    print("=" * 108)
    print("R10 - EPOCH-TO-EPOCH WIRE STEPS vs the per-key DISCONTINUITY COUNTER")
    print("      sig = the counter incremented at that epoch;  UNSIG = it did not")
    print("=" * 108)
    hdr = ("%-10s %-4s %8s | %7s %7s | %7s %7s | %7s %7s"
           % ("arm", "kind", "rows", ">0.5 sig", "UNSIG", ">2 sig", "UNSIG",
              ">10 sig", "UNSIG"))
    print(hdr)
    for arm, res in out.items():
        for kind in ("CLK", "PB"):
            c = res["counts"][kind]
            print("%-10s %-4s %8d | %7d %7d | %7d %7d | %7d %7d"
                  % (arm, kind, res["rows"][kind],
                     c[0.5][True], c[0.5][False], c[2.0][True], c[2.0][False],
                     c[10.0][True], c[10.0][False]))

    print("\n" + "=" * 108)
    print("PER HOUR - UNSIGNALLED steps over 2 m  (CLK / PB)")
    print("=" * 108)
    print("%-10s %s" % ("arm", " ".join("   h%d" % h for h in range(1, 7))))
    for arm, res in out.items():
        cells = []
        for h in range(1, 7):
            hh = res["hourly"].get(h)
            if hh is None:
                cells.append("   - ")
            else:
                cells.append("%2d/%-2d" % (hh["CLK"][2.0][False], hh["PB"][2.0][False]))
        print("%-10s %s" % (arm, " ".join(cells)))

    print("\n" + "=" * 108)
    print("UNSIGNALLED steps > %.1f m, attributed  (net_med = the epoch's network median"
          " clock step: large ⇒ frame move, ~0 ⇒ per-satellite re-realization)" % a.attrib_m)
    print("=" * 108)
    for arm, res in out.items():
        fl = res["flagged"]
        print("\n--- %s : %d unsignalled step(s) > %.1f m" % (arm, len(fl), a.attrib_m))
        if not fl:
            continue
        print("      tow      h kind sat  sig    step_m   net_med   dev   cons3D consStep  nearest event (dt s)")
        for r in fl[:a.max_list]:
            print("  %9.0f %2d %-4s %-4s %-4s %8.3f %8.4f %7s %7s %8s  %s (%+.0f)%s"
                  % (r["tow"], r["hour"], r["kind"], r["sat"], r["signal"] or "-",
                     r["step_m"], r["net_median_step_m"],
                     "%.3f" % r["dev_from_median_m"] if r["dev_from_median_m"] is not None else "-",
                     r["consumer_3d_m"] if r["consumer_3d_m"] is not None else "-",
                     r["consumer_step_m"] if r["consumer_step_m"] is not None else "-",
                     r["near_event"], r["near_dt_s"] if r["near_dt_s"] is not None else 0,
                     "" if r["near_names_sat"] else "  [not this sat]"))
        if len(fl) > a.max_list:
            print("      ... %d more" % (len(fl) - a.max_list))
        # the frozen reading's own statistic
        rec = [r for r in fl if "cta_recouple" in r["near_event"]]
        print("      within +-2 ep of a cta_recouple: %d of %d (%.0f %%)"
              % (len(rec), len(fl), 100.0 * len(rec) / max(1, len(fl))))

    print("\n" + "=" * 108)
    print("(B) THE ANCHOR READ - was the T4-refloated station the satellite's elected PHASE ANCHOR?")
    print("     and did the PHASE BIAS hold while the CLOCK took the re-realization?")
    print("=" * 108)
    tot = on_t = pb_held = 0
    for arm, res in out.items():
        fl = [r for r in res["flagged"] if r["kind"] == "CLK" and "on_anchor" in r]
        if not fl:
            continue
        o = sum(1 for r in fl if r["on_anchor"])
        h = sum(1 for r in fl if r["pb_max_step_m"] <= 0.5)
        tot += len(fl); on_t += o; pb_held += h
        print("\n--- %s : %d clock step(s); ON-ANCHOR %d (%.0f %%); PB held (<=0.5 m) %d (%.0f %%)"
              % (arm, len(fl), o, 100.0 * o / len(fl), h, 100.0 * h / len(fl)))
        print("      tow      sat   step_m  anchor?  T4 station(s)            anchor station(s)        PBmax/sig")
        for r in fl[:a.max_list]:
            print("  %9.0f %-4s %8.3f  %-7s %-24s %-24s %6.3f/%s"
                  % (r["tow"], r["sat"], r["step_m"],
                     "ON" if r["on_anchor"] else "off",
                     ",".join(r["t4_stations"])[:24] or "-",
                     ",".join(r["anchor_stations"])[:24] or "-",
                     r["pb_max_step_m"], r["pb_worst_signal"] or "-"))
    if tot:
        print("\n  POOLED over every arm: %d unsignalled clock step(s) > %.1f m; "
              "ON-ANCHOR %d (%.1f %%); PB HELD (<= 0.5 m) %d (%.1f %%)"
              % (tot, a.attrib_m, on_t, 100.0 * on_t / tot, pb_held, 100.0 * pb_held / tot))
        print("  s24.20 frozen: > 50 % on the anchor => the anchor-election card is the "
              "structural fix; < 20 % => a 'T4 refloat forces a clock mark' rule is.")

    if a.json:
        with open(a.json, "w", encoding="utf-8") as f:
            json.dump(out, f, indent=1)
        print("\njson -> " + a.json)


if __name__ == "__main__":
    main()
