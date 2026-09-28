#!/usr/bin/env python3
"""Cold anchor-fallback grading (SPEC_event_harness §15): the election channels of a leg.

`scripts/event_grade.py` grades the wire and consumer channels of an event leg
against its null. It does not see the channel this script covers: which station
the anchor election picked, whether the handover it made was continuous or cold,
what the candidacy pass walked past to get there, and how old the successor's arc
was. Those live in `logs/network_events.csv` (PBGAUGE / PHASE_ANCHOR / INJECT rows),
in the run-end clauses of `stdout.txt`, and, for the successor's age and elevation,
in the leg's own `logs/network_slip_raw.csv`, the only trace that carries a pinned
arc at all (a pinned arc never appears in the residual trace).

    python scripts/ev_coldfb_grade.py runs/evleg_<leg> [runs/evleg_<other> ...]
        [--window 40] [--json out.json]

Prints, per leg: the handover rows in `--window` epochs after the event (family,
from → to, branch, target, var, cand_skipped / cand_first_unusable), the
successor's arc age and elevation at the event epoch, the T4 attribution rows on
the event satellite's arcs in the same window, and the run-end candidacy and
PB-GAUGE counters.  Nothing here is a verdict; it is an instrument.
"""
import argparse
import csv
import json
import os
import re
import sys
from collections import defaultdict

EPOCH_S = 30.0


def leg_meta(d):
    with open(os.path.join(d, "leg_meta.json"), encoding="utf-8") as fh:
        return json.load(fh)


def events(d):
    p = os.path.join(d, "logs", "network_events.csv")
    if not os.path.exists(p):
        return []
    with open(p, newline="", encoding="utf-8", errors="replace") as fh:
        return list(csv.DictReader(fh))


def detail_field(detail, key):
    m = re.search(re.escape(key) + r"=([^;]*)", detail or "")
    return m.group(1) if m else None


def arc_state(d, tow, sat):
    """(age_epochs, elev_deg) per station for `sat` at `tow`, from the leg's trace.

    Age is the number of consecutive 30 s epochs the satellite has been seen at
    that station inside this leg: a 60-epoch leg cannot see an older arc than it
    is long, so the seed's census is the authority on absolute age and this is
    the leg-local cross-check (a successor that first appears mid-leg reads
    young here and is young in truth).
    """
    p = os.path.join(d, "logs", "network_slip_raw.csv")
    if not os.path.exists(p):
        return {}
    last, run, elev = {}, {}, {}
    with open(p, newline="", encoding="utf-8", errors="replace") as fh:
        for r in csv.DictReader(fh):
            if r["sat"] != sat:
                continue
            t = float(r["tow"])
            if t > tow + 1e-6:
                break
            k = r["station"]
            prev = last.get(k)
            if prev is None or abs(t - prev - EPOCH_S) > 1e-6 and abs(t - prev) > 1e-6:
                run[k] = 1
            elif abs(t - prev - EPOCH_S) < 1e-6:
                run[k] = run.get(k, 0) + 1
            last[k] = t
            if abs(t - tow) < 1e-6:
                elev[k] = float(r["elev_deg"])
    return {k: (run.get(k, 0), elev.get(k)) for k in elev}


_CAND_RE = re.compile(
    r"COLD ANCHOR-FALLBACK\): the usable-first candidacy pass ran on (\d+) handover "
    r"election\(s\) — (\d+) elected .*?of which (\d+) were NOT.*?and (\d+) found NO "
    r"usable candidate.*?(\d+) \(candidate, family\) pair\(s\) were skipped: (\d+) no "
    r"ledger entry, (\d+) refused by the variance/ratio ceiling, (\d+) freed this "
    r"epoch, (\d+) younger", re.S)
_HANDOVER_RE = re.compile(
    r"PB-GAUGE HANDOVER, two-edge\): (\d+) anchor handover\(s\).*?— (\d+) made "
    r"CONTINUOUS.*?and (\d+) COLD", re.S)


def run_end(d):
    """Run-end counters, from the producer's own clauses."""
    out = {}
    p = os.path.join(d, "stdout.txt")
    if not os.path.exists(p):
        return out
    with open(p, encoding="utf-8", errors="replace") as fh:
        txt = fh.read()
    m = _CAND_RE.search(txt)
    if m:
        (out["cand_entered"], out["cand_usable_elected"], out["cand_rank_changed"],
         out["cand_no_usable_fallback"], out["cand_skipped"], out["skip_no_entry"],
         out["skip_var_refused"], out["skip_freed"], out["skip_young"]) = \
            (int(x) for x in m.groups())
    m = _HANDOVER_RE.search(txt)
    if m:
        (out["reattaches"], out["handover_continuous"], out["handover_cold"]) = \
            (int(x) for x in m.groups())
    m = re.search(r"marked \(sat,signal\) family\(ies\) marginalized", txt)
    out["pa_reref_lines"] = len(re.findall(r"family\(ies\) marginalized", txt))
    return out


def grade(d, window):
    m = leg_meta(d)
    t0 = m.get("event_tow")
    ev = m.get("event") or {}
    sat = (ev or {}).get("sat", "")
    ev_rows = events(d)
    out = {"leg": os.path.basename(d), "event": ev, "event_tow": t0,
           "extra": m.get("extra", {}), "run_end": run_end(d)}
    rc = os.path.join(d, "exitcode.txt")
    if os.path.exists(rc):
        with open(rc, encoding="utf-8") as fh:
            ln = fh.read().split()
        out["exit"] = int(ln[0]) if ln else None
        out["wall_s"] = float(ln[1]) if len(ln) > 1 else None

    # The leg's recorded event_tow carries the resume recurrence's double
    # residue (...480.0000178814), so a half-epoch back-off is required, or the
    # event's own rows, written at the exact tow, fall outside the window.
    hi = (t0 + window * EPOCH_S) if t0 else None
    def inwin(r):
        if t0 is None:
            return True
        t = float(r["tow"])
        return t0 - 0.5 * EPOCH_S <= t <= hi + 0.5 * EPOCH_S

    def off_ep(r):
        return (int(round((float(r["tow"]) - t0) / EPOCH_S)) if t0 else
                int(round((float(r["tow"]) - m["first_tow"]) / EPOCH_S)))

    # The gauge handovers themselves: both edges in one row (invariant #7).
    hand = []
    for r in ev_rows:
        if r["tier"] != "PBGAUGE" or not r["event"].startswith("handover") or not inwin(r):
            continue
        hand.append({
            "off_ep": off_ep(r), "tow": float(r["tow"]),
            "amb": r["sat"], "to": r["station"],
            "from": detail_field(r["detail"], "from"),
            "branch": "continuous" if r["event"].endswith("continuous") else "cold",
            "target_m": float(r["statistic"]) if r["statistic"] else 0.0,
            "var": detail_field(r["detail"], "var"),
            "cand_skipped": detail_field(r["detail"], "cand_skipped"),
            "cand_first_unusable": detail_field(r["detail"], "cand_first_unusable"),
        })
    out["handovers"] = hand
    out["n_handover"] = len(hand)
    out["n_cold"] = sum(1 for h in hand if h["branch"] == "cold")

    # The injected directive's own readback (what it retired and elected).
    out["inject_rows"] = [
        {"off_ep": off_ep(r), "sat": r["sat"], "detail": r["detail"]}
        for r in ev_rows if r["tier"] == "INJECT" and inwin(r)]
    # The production anchor-slip staging (the §12.7 path), when armed.
    out["slip_handover_rows"] = [
        {"off_ep": off_ep(r), "amb": r["sat"], "from": r["station"],
         "detail": r["detail"]}
        for r in ev_rows
        if r["tier"] == "PHASE_ANCHOR" and r["action"] == "handover" and inwin(r)]

    # Counts T4 attribution erases on the event satellite's arcs in the window
    # and network-wide; erases on healthy arcs elsewhere are the §12.7 side effect.
    t4 = [r for r in ev_rows if r["tier"].startswith("T4") and inwin(r)]
    out["t4_total"] = len(t4)
    out["t4_on_sat"] = sum(1 for r in t4 if sat and r["sat"].startswith(sat))
    out["t4_stations"] = len({r["station"] for r in t4 if r["station"]})

    # The successor's arc at the event epoch.
    cache = {}
    for h in hand:
        hsat = h["amb"].split("_")[0]
        if hsat not in cache:
            cache[hsat] = arc_state(d, h["tow"], hsat)
        a = cache[hsat].get(h["to"])
        h["to_age_leg_ep"] = a[0] if a else None
        h["to_elev_deg"] = a[1] if a else None
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("legs", nargs="+")
    ap.add_argument("--window", type=int, default=40,
                    help="epochs after the event to scan (default 40)")
    ap.add_argument("--json", default=None)
    a = ap.parse_args()

    allout = []
    for d in a.legs:
        g = grade(d, a.window)
        allout.append(g)
        print(f"\n=== {g['leg']}   exit {g.get('exit')}  {g.get('wall_s')} s"
              f"   event {json.dumps(g['event']) if g['event'] else 'NULL'}")
        if g["extra"]:
            print(f"    extra {json.dumps(g['extra'])}")
        re_ = g["run_end"]
        if re_:
            print("    run-end: " + "  ".join(f"{k}={v}" for k, v in re_.items()))
        if g["inject_rows"]:
            for r in g["inject_rows"]:
                print(f"    INJECT @{r['off_ep']:+d}  {r['sat']}  {r['detail']}")
        for r in g["slip_handover_rows"]:
            print(f"    PHASE_ANCHOR/handover @{r['off_ep']:+d}  {r['amb']}  "
                  f"from {r['from']}  {r['detail']}")
        print(f"    PBGAUGE handovers in window: {g['n_handover']} "
              f"({g['n_cold']} cold)")
        for h in g["handovers"][:24]:
            print(f"      @{h['off_ep']:+d} {h['amb']:<10} {h['from']} -> {h['to']:<10} "
                  f"{h['branch']:<10} target {h['target_m']:+.4f}"
                  + (f"  age {h.get('to_age_leg_ep')} ep  el {h.get('to_elev_deg')}"
                     if h.get("to_elev_deg") is not None else "")
                  + (f"  cand_skipped {h['cand_skipped']}"
                     f" first_unusable {h['cand_first_unusable']}"
                     if h["cand_skipped"] is not None else ""))
        print(f"    T4 rows in window: {g['t4_total']} total, {g['t4_on_sat']} on the "
              f"event satellite, over {g['t4_stations']} station(s)")
    if a.json:
        with open(a.json, "w", encoding="utf-8") as fh:
            json.dump(allout, fh, indent=2)
        print(f"\nwrote {a.json}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
