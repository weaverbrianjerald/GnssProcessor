#!/usr/bin/env python3
"""Card EV7 section 6 gate E1: grade a run's COVERAGE-GATE DROP EDGES.

Trace-only and stdout-only: nothing under the run directory is written or moved.

The defect (docs/refound/EV7_all_sat_clock_decouple_2026-09-21.md section 5.3).  The
coverage gate network.phase_datum_min_stations removes a (satellite, signal)
phase-bias column when it thins below the threshold.  On the float vehicle the
surviving station's ambiguity is a CARRIED state, so it absorbs the whole removed
bias in one epoch against a centimetre prior, and the satellite clocks absorb what
the prior does not yield.  The fix (network.phase_datum_gate_drop_erase) adds the
family to the epoch's re-reference set at the drop edge, so every station's carried
ambiguity of it is erased and re-born instead.

What is read, and from where:

  logs/network_events.csv   PHASE_ANCHOR / gate_drop rows (the edges; emitted in
                            BOTH knob states), TOPOLOGY / row_kinds rows that ADD
                            obs_other (the independent cross-check the card's own
                            section 5.3 table was built from), CLK_ANCHOR /
                            cta_decouple rows with reason=nis (the clock trips),
                            and the T4_attrib / PHASE_ANCHOR / PBGAUGE / STRANSFORM
                            row counts.
  logs/network_amb_state.csv  the carried ambiguities: one row per (station, sat,
                            signal) per epoch, plus an 'erase:<cause>' row captured
                            immediately before a deletion and a 'birth' row for a
                            fresh arc.
  logs/closed_loop_trace.csv  the consumer's 3D error and Up, for the record.
  logs/main_log.log         the run-end counters this card added.

A CARRIED JUMP is measured only where the arc actually continued: an ordinary row
at the edge epoch (event empty) against that same arc's previous row.  An arc that
was erased and re-born at the edge has no carried continuation and contributes no
jump, which is the whole point of the response.  Dispositions are listed either way
so a silent edge cannot be confused with an absent one.

    python scripts/ev7_gate_drop_grade.py runs/_rap_g1_field_on
    python scripts/ev7_gate_drop_grade.py runs/_rap_g1_field_on --ref runs/_rap_r6_dual
"""
import argparse
import csv
import os
import re
import statistics as st
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__))).replace("\\", "/")
JUMP_BAR_M = 0.5        # E1's bar on a carried move at an edge
TRIP_BAR = 8            # E1's bar on reason=nis clock trips in one epoch


def resolve(tag):
    if os.path.isdir(tag):
        return tag.replace("\\", "/")
    for cand in (f"{ROOT}/runs/{tag}", f"{ROOT}/runs/_rap_{tag}"):
        if os.path.isdir(cand):
            return cand
    sys.exit("no such run dir: " + tag)


# --------------------------------------------------------------------------- reads
def read_events(rd):
    p = rd + "/logs/network_events.csv"
    if not os.path.exists(p):
        return []
    with open(p, newline="") as f:
        return [r for r in csv.DictReader(f)]


def gate_drop_edges(events):
    """The PHASE_ANCHOR / gate_drop rows, as (tow, sat, signal, detail-dict)."""
    out = []
    for r in events:
        if r.get("tier") != "PHASE_ANCHOR" or r.get("event") != "gate_drop":
            continue
        d = dict(kv.split("=", 1) for kv in (r.get("detail") or "").split(";") if "=" in kv)
        amb = r.get("sat") or ""
        sat, _, sig = amb.partition("_")
        out.append({"tow": float(r["tow"]), "sat": sat, "signal": sig,
                    "survivors": d.get("survivors", "-"),
                    "trackers_now": d.get("trackers_now", "?"),
                    "prev_anchor": d.get("prev_anchor", "-"),
                    "handled": d.get("handled", "?"),
                    "station_col": r.get("station", "")})
    out.sort(key=lambda e: (e["tow"], e["sat"], e["signal"]))
    return out


def topology_add_other(events):
    """TOPOLOGY / row_kinds edges whose `now=` ADDS obs_other — the card's own
    independent census of the same class (a station's rows losing their shared
    column are re-classified obs_other)."""
    out = []
    for r in events:
        if r.get("tier") != "TOPOLOGY" or r.get("event") != "row_kinds":
            continue
        det = r.get("detail") or ""
        m = re.search(r"prev=(\S+)\s+now=(\S+)", det)
        if not m:
            continue
        prev, now = m.group(1), m.group(2)
        if "obs_other" in now and "obs_other" not in prev:
            out.append((float(r["tow"]), r.get("sat", ""), r.get("station", "")))
    out.sort()
    return out


def clock_trips(events):
    """reason=nis CLK_ANCHOR decouples: per-epoch counts and the run total."""
    per = {}
    for r in events:
        if r.get("tier") != "CLK_ANCHOR" or r.get("event") != "cta_decouple":
            continue
        if "reason=nis" not in (r.get("detail") or ""):
            continue
        per.setdefault(float(r["tow"]), []).append(r.get("sat", ""))
    return per


def read_amb_state(rd):
    """(station, sat, signal) -> ordered list of rows; erase rows kept separate."""
    p = rd + "/logs/network_amb_state.csv"
    if not os.path.exists(p):
        return {}, {}
    val, era = {}, {}
    with open(p, newline="") as f:
        rows = [ln for ln in f if not ln.startswith("#")]
    for r in csv.DictReader(rows):
        try:
            key = (r["station"], r["sat"], r["signal"])
            tow = float(r["tow"])
            v = float(r["value_abs_m"])
        except (KeyError, ValueError, TypeError):
            continue
        ev = (r.get("event") or "").strip()
        rec = {"tow": tow, "v": v, "event": ev, "age": r.get("age_ep", ""),
               "var": r.get("var_m2", "")}
        if ev.startswith("erase:"):
            era.setdefault(key, []).append(rec)
        else:
            val.setdefault(key, []).append(rec)
    for d in (val, era):
        for k in d:
            d[k].sort(key=lambda x: x["tow"])
    return val, era


# --------------------------------------------------------------------------- E1
def grade_edges(edges, val, era):
    """Per edge: every station's disposition for the family, and the carried jump."""
    out = []
    for e in edges:
        tow, sat, sig = e["tow"], e["sat"], e["signal"]
        stations = {k[0] for k in val if k[1] == sat and k[2] == sig}
        stations |= {k[0] for k in era if k[1] == sat and k[2] == sig}
        rows = []
        for stn in sorted(stations):
            k = (stn, sat, sig)
            # VALUE-carrying rows only.  An arc can also have a 'drop:<cause>'
            # annotation row at the same epoch (the RF-1 step-4 bounded drop
            # re-states the entry at its birth variance); it is a second row for
            # the SAME value, and taking it as the epoch's row makes a real
            # carried jump read as 'no previous row'.
            seq = [r for r in val.get(k, [])
                   if r["event"] in ("", "birth", "stransform")]
            at = [r for r in seq if r["tow"] == tow]
            prev = [r for r in seq if r["tow"] < tow]
            erased = [r for r in era.get(k, []) if r["tow"] == tow]
            if not at and not erased:
                continue                       # this station has no arc at the edge
            disp, jump = "absent", None
            if erased and any(r["event"] == "birth" for r in at):
                disp = "erased+reborn"
            elif erased:
                disp = "erased"
            elif at and at[-1]["event"] == "birth":
                disp = "born"
            elif at and not at[-1]["event"] and prev:
                disp, jump = "carried", at[-1]["v"] - prev[-1]["v"]
            elif at:
                disp = "carried(no prev)"
            rows.append({"station": stn, "disp": disp, "jump": jump,
                         "age": (prev[-1]["age"] if prev else
                                 (erased[-1]["age"] if erased else "")),
                         "var": (prev[-1]["var"] if prev else
                                 (erased[-1]["var"] if erased else ""))})
        jumps = [abs(r["jump"]) for r in rows if r["jump"] is not None]
        out.append({**e, "rows": rows, "max_jump": max(jumps) if jumps else None})
    return out


# --------------------------------------------------------------------------- record
def hourly(rd, field):
    p = rd + "/logs/closed_loop_trace.csv"
    if not os.path.exists(p):
        return {}
    hrs = {}
    with open(p, newline="") as f:
        rows = list(csv.DictReader(f))
    tows = [float(r["tow"]) for r in rows if r.get("tow")]
    if not tows:
        return {}
    t0 = min(tows)
    for r in rows:
        try:
            hrs.setdefault(int((float(r["tow"]) - t0) // 3600), []).append(float(r[field]))
        except (ValueError, TypeError, KeyError):
            pass
    return hrs


def per_hour_events(events, tier):
    tows = [float(r["tow"]) for r in events if r.get("tow")]
    if not tows:
        return {}
    t0 = min(tows)
    hrs = {}
    for r in events:
        if r.get("tier") != tier:
            continue
        hrs[int((float(r["tow"]) - t0) // 3600)] = hrs.get(int((float(r["tow"]) - t0) // 3600), 0) + 1
    return hrs


def run_end_counters(rd):
    p = rd + "/logs/main_log.log"
    if not os.path.exists(p):
        return []
    out = []
    with open(p, encoding="utf-8", errors="replace") as f:
        for ln in f:
            if "EV7 gate-drop erase):" in ln and "coverage-gate DROP EDGE" in ln:
                out.append(ln.split("] ", 3)[-1].strip())
    return out


# --------------------------------------------------------------------------- report
def report(rd):
    tag = os.path.basename(rd)
    ev = read_events(rd)
    edges = gate_drop_edges(ev)
    val, era = read_amb_state(rd)
    graded = grade_edges(edges, val, era)
    topo = topology_add_other(ev)
    trips = clock_trips(ev)

    print(f"===== {tag}   ({rd})")
    print(f"-- E1a  gate-drop edges: {len(edges)}   "
          f"(TOPOLOGY row_kinds edges that ADD obs_other: {len(topo)})")
    print(f"{'TOW':>10} {'sat':4} {'sig':4} {'survivors':<22} {'prev_anchor':<12} "
          f"{'hd':>2}  {'max|jump| m':>11}  dispositions")
    n_over = 0
    for g in graded:
        mj = g["max_jump"]
        if mj is not None and mj > JUMP_BAR_M:
            n_over += 1
        disp = ", ".join(
            f"{r['station']}:{r['disp']}"
            + (f"{r['jump']:+.3f}" if r["jump"] is not None else "")
            for r in g["rows"]) or "-"
        print(f"{g['tow']:10.0f} {g['sat']:4} {g['signal']:4} {g['survivors']:<22} "
              f"{g['prev_anchor']:<12} {g['handled']:>2}  "
              f"{('%11.3f' % mj) if mj is not None else ' '*8+'-':>11}  {disp}")
    print(f"-- E1a  edges with a CARRIED jump above {JUMP_BAR_M} m: "
          f"{n_over} of {len(edges)}")

    big = sorted(t for t, s in trips.items() if len(s) >= TRIP_BAR)
    total = sum(len(s) for s in trips.values())
    print(f"-- E1b  CLK_ANCHOR cta_decouple reason=nis: {total} trip(s) in the run, "
          f"{len(trips)} epoch(s) with at least one")
    print(f"-- E1b  epochs with {TRIP_BAR} or more trips: {len(big)}"
          + (("  " + ", ".join(f"{t:.0f}({len(trips[t])})" for t in big)) if big else ""))
    in_edge = sum(len(s) for t, s in trips.items() if t in {g["tow"] for g in graded})
    print(f"-- E1b  …of which in a gate-drop edge epoch: {in_edge} of {total}")

    for lbl, fld in (("3D", "pos_err_3d_m"), ("Up", "up_m")):
        h = hourly(rd, fld)
        mean = " ".join(f"h{k}:{st.mean(v):.3f}" for k, v in sorted(h.items()) if k >= 2)
        p50 = " ".join(f"{st.median(v):.3f}" for _, v in sorted(h.items()))
        print(f"-- rec  consumer {lbl} mean/hour (hours 3-6, 0-based h2+): {mean}")
        print(f"-- rec  consumer {lbl} p50/hour (all hours):               {p50}")
    sub = hourly(rd, "pos_err_3d_m")
    print(f"-- rec  consumer epochs below 0.10 m: "
          f"{sum(1 for v in sub.values() for e in v if e < 0.10)}")
    t4 = per_hour_events(ev, "T4_attrib")
    print("-- rec  T4_attrib rows per hour: "
          + (" ".join(f"h{k}:{v}" for k, v in sorted(t4.items())) or "0"))
    cnt = {}
    for r in ev:
        cnt[r.get("tier")] = cnt.get(r.get("tier"), 0) + 1
    print("-- rec  rows by tier: "
          + " ".join(f"{t}={cnt.get(t, 0)}" for t in ("PHASE_ANCHOR", "PBGAUGE", "STRANSFORM")))
    for ln in run_end_counters(rd) or ["(no EV7 run-end counter line: 0 edges)"]:
        print("-- rec  run-end: " + ln)
    return graded, trips


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("run_dir")
    ap.add_argument("--ref", help="a second run dir reported alongside for contrast")
    a = ap.parse_args()
    rd = resolve(a.run_dir)
    g_arm, t_arm = report(rd)
    if a.ref:
        print()
        g_ref, t_ref = report(resolve(a.ref))
        print()
        over = lambda g: sum(1 for x in g if x["max_jump"] is not None
                             and x["max_jump"] > JUMP_BAR_M)
        big = lambda t: sum(1 for _, s in t.items() if len(s) >= TRIP_BAR)
        tot = lambda t: sum(len(s) for s in t.values())
        print(f"===== CONTRAST  {os.path.basename(rd)}  vs  {os.path.basename(resolve(a.ref))}")
        print(f"  edges                    {len(g_arm):5d}   {len(g_ref):5d}")
        print(f"  edges with jump > {JUMP_BAR_M} m  {over(g_arm):5d}   {over(g_ref):5d}")
        print(f"  reason=nis trips         {tot(t_arm):5d}   {tot(t_ref):5d}")
        print(f"  epochs with {TRIP_BAR}+ trips    {big(t_arm):5d}   {big(t_ref):5d}")


if __name__ == "__main__":
    main()
