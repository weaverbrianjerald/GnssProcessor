#!/usr/bin/env python3
"""Cold anchor-fallback card, step 1: the family-selection census (no runs).

Answers, from a seed run's own traces, the question the card's harness legs need
answered before any leg exists: which (sat, signal) families, at the snapshot
epoch, would hand over to a candidate whose arc is not carry-usable while a
lower-ranked candidate's is?  Those are the families whose handover goes cold
today and would go continuous under `network.phase_datum_candidacy_usable`.

Two traces, both written by the seed itself:

``logs/network_phase_anchor.csv``   week,tow,cons,signal,sat,anchor_sta
    who anchors what at the epoch: the incumbent of every family.

``logs/network_slip_raw.csv``       week,tow,station,sat,sig1,sig2,elev_deg,...
    every station x satellite x epoch with elevation, so an arc's age at the
    epoch is the number of consecutive 30 s epochs the satellite has been seen
    at that station, and the signals it tracks are the union of the sig1/sig2
    columns of its rows.  (The residual trace cannot be used: a pinned arc never
    appears in it, SPEC_event_harness.md §15.)

The candidate order is reproduced the way `networkElectPhaseAnchors` builds it
(`src/core/filter/NetworkPhaseDatum.cpp`): per constellation, stations ranked by
descending coverage, the number of satellites of that constellation the station
tracks at elevation >= `network.phase_datum_pref_elev_deg` (default 15), then by
ascending station name.  `usable_keys` (the station allocated a phase-bias state
for the (sat, signal)) is approximated, as the brief specifies, by "the station
tracks that satellite on that signal at that epoch".

Usage
-----
    python scripts/ev_coldfb_census.py --seed-run <dir> --tow 279150 [--pref-elev 15]
        [--min-stations 2] [--mature 60] [--high 60] [--low 35] [--top N]
"""
import argparse
import csv
import os
import sys
from collections import defaultdict

EPOCH_S = 30.0


def load_anchors(path, tow):
    """(cons, signal, sat) -> anchoring station, at `tow`."""
    out = {}
    with open(path, newline="", encoding="utf-8", errors="replace") as fh:
        for r in csv.DictReader(fh):
            if abs(float(r["tow"]) - tow) > 1e-6:
                continue
            out[(r["cons"], r["signal"], r["sat"])] = r["anchor_sta"]
    return out


def load_tracking(path, tow):
    """Walk network_slip_raw.csv once, up to and including `tow`.

    Returns (sigs, elev, age) where
      sigs[(sta, sat)] = set of signals tracked at `tow`
      elev[(sta, sat)] = elevation (deg) at `tow`
      age[(sta, sat)]  = consecutive-epoch arc length ending at `tow` (0 = absent)
    """
    last = {}      # (sta,sat) -> last tow seen
    run = {}       # (sta,sat) -> consecutive epochs ending at `last`
    sigs = defaultdict(set)
    elev = {}
    with open(path, newline="", encoding="utf-8", errors="replace") as fh:
        rd = csv.DictReader(fh)
        for r in rd:
            t = float(r["tow"])
            if t > tow + 1e-6:
                break                       # the trace is time-ordered
            k = (r["station"], r["sat"])
            prev = last.get(k)
            if prev is None:
                run[k] = 1
            elif abs(t - prev) < 1e-6:
                pass                        # another signal pair, same epoch
            elif abs(t - prev - EPOCH_S) < 1e-6:
                run[k] += 1
            else:
                run[k] = 1                  # gap: the arc restarts
            last[k] = t
            if abs(t - tow) < 1e-6:
                sigs[k].add(r["sig1"])
                sigs[k].add(r["sig2"])
                elev[k] = float(r["elev_deg"])
    age = {k: run[k] for k in run if abs(last[k] - tow) < 1e-6}
    return sigs, elev, age


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--seed-run", required=True)
    ap.add_argument("--tow", type=float, required=True)
    ap.add_argument("--pref-elev", type=float, default=15.0)
    ap.add_argument("--min-stations", type=int, default=2)
    ap.add_argument("--mature", type=int, default=60, help="mature arc age (epochs)")
    ap.add_argument("--high", type=float, default=60.0, help="high elevation (deg)")
    ap.add_argument("--low", type=float, default=35.0, help="low elevation (deg)")
    ap.add_argument("--top", type=int, default=25, help="rows of the shortlist to print")
    a = ap.parse_args()

    logs = os.path.join(a.seed_run, "logs")
    anchors = load_anchors(os.path.join(logs, "network_phase_anchor.csv"), a.tow)
    if not anchors:
        sys.exit(f"FATAL: no network_phase_anchor rows at tow {a.tow}")
    sigs, elev, age = load_tracking(os.path.join(logs, "network_slip_raw.csv"), a.tow)

    stations = sorted({k[0] for k in sigs})
    # Coverage rank, per constellation, exactly as the election builds it.
    cov = defaultdict(lambda: defaultdict(int))
    for (sta, sat), e in elev.items():
        if e >= a.pref_elev:
            cov[sat[0]][sta] += 1
    order = {c: sorted(stations, key=lambda s: (-cov[c][s], s)) for c in cov}

    print(f"# seed {a.seed_run}  tow {a.tow:.0f}  stations {len(stations)}  "
          f"families anchored {len(anchors)}  pref_elev {a.pref_elev}  "
          f"min_stations {a.min_stations}")
    print(f"# mature >= {a.mature} ep, high > {a.high} deg, low < {a.low} deg\n")

    rows = []
    for (cons, sig, sat), inc in sorted(anchors.items(), key=lambda kv: kv[0][2]):
        trk = [s for s in order.get(cons, [])
               if sig in sigs.get((s, sat), ()) and (s, sat) in age]
        if len(trk) < a.min_stations:
            continue
        others = [s for s in trk if s != inc]
        if not others:
            continue
        top = others[0]
        top_age, top_el = age.get((top, sat), 0), elev.get((top, sat), 0.0)
        # The best mature, high-elevation alternative anywhere below the top-ranked one.
        better = [s for s in others[1:]
                  if age.get((s, sat), 0) >= a.mature and elev.get((s, sat), 0.0) > a.high]
        qualifies = (top_age < a.mature or top_el < a.low) and bool(better)
        rows.append({
            "amb": f"{sat}_{sig}", "sat": sat, "sig": sig, "cons": cons,
            "inc": inc, "inc_age": age.get((inc, sat), 0),
            "inc_el": elev.get((inc, sat), 0.0),
            "n_trk": len(trk), "top": top, "top_age": top_age, "top_el": top_el,
            "top_rank": trk.index(top),
            "alt": better[0] if better else "-",
            "alt_age": age.get((better[0], sat), 0) if better else 0,
            "alt_el": elev.get((better[0], sat), 0.0) if better else 0.0,
            "alt_rank": trk.index(better[0]) if better else -1,
            "q": qualifies,
        })

    hdr = (f"{'family':<10} {'incumbent':<10} {'age':>5} {'el':>5} | {'trk':>3} "
           f"{'top cand':<10} {'rk':>2} {'age':>5} {'el':>5} | "
           f"{'mature alt':<10} {'rk':>2} {'age':>5} {'el':>5} | qualifies")
    print(hdr)
    print("-" * len(hdr))
    shortlist = [r for r in rows if r["q"]]
    # Strongest first: the youngest top-ranked candidate (the most likely to have
    # no carry-usable ledger value at all), then the highest mature alternative.
    shortlist.sort(key=lambda r: (r["top_age"], -r["alt_el"]))
    for r in (shortlist or rows)[:a.top]:
        print(f"{r['amb']:<10} {r['inc']:<10} {r['inc_age']:>5} {r['inc_el']:>5.1f} | "
              f"{r['n_trk']:>3} {r['top']:<10} {r['top_rank']:>2} {r['top_age']:>5} "
              f"{r['top_el']:>5.1f} | {r['alt']:<10} {r['alt_rank']:>2} {r['alt_age']:>5} "
              f"{r['alt_el']:>5.1f} | {'YES' if r['q'] else 'no'}")
    print(f"\n{len(shortlist)} of {len(rows)} anchored families qualify "
          f"(top-ranked non-incumbent candidate young or low, a mature high one below it)")

    # Per-satellite roll-up: a handover moves every family of the satellite that
    # names the same incumbent, so a satellite with three qualifying families is
    # one leg, not three.
    bysat = defaultdict(list)
    for r in shortlist:
        bysat[(r["sat"], r["inc"])].append(r["sig"])
    print("\nby satellite (a `signal: all` directive moves every family at once):")
    for (sat, inc), sg in sorted(bysat.items(), key=lambda kv: (-len(kv[1]), kv[0])):
        print(f"  {sat:<4} inc {inc:<10} qualifying signals {','.join(sorted(sg))}")


if __name__ == "__main__":
    main()
