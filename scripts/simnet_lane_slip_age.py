#!/usr/bin/env python3
"""simnet_lane_slip_age.py: where does the lane-integer slip cost live?

Follow-up to the wu93 campaign finding (SPEC_slip_relevel_walk.md section 9): live
slips cost about 25 points of at-own-integer rate at both lane rungs (EWL 95.1 to
70.2 %, WL 46.2 to 21.1 %).  This script decomposes the slipped arm's lane errors
by the truth slip history of each DD quadruple:

  * virgin rows: none of the quadruple's 8 (station,sat)x(band) legs has slipped
                 yet at this epoch.  If these match the quiet twin's rate, slips
                 carry the whole cost.
  * slip-aged rows: bucketed by epochs since the most recent truth slip on any
                 leg.  A cost that decreases with age reflects re-convergence
                 (bounded, arguably acceptable); a cost that persists across
                 hundreds of epochs is a wrong lane level fixed in place by the
                 re-level response, the signature that makes lane-level
                 continuity (not iono, not detection) the fix.

Reuses the truth join of simnet_amb_integer.py verbatim (load_truth / n_at);
lane errors are formed exactly as there: e_lane = e_band1 - e_band2 on matched
(tow, sat, ref, sta_a, sta_b) quadruples, truth in python ints.

Usage:
    python scripts/simnet_lane_slip_age.py runs/simnet60_campaign_wu93 --run runs/camp60wu_f1
"""

from __future__ import annotations

import argparse
import bisect
import csv
import os
import sys
from collections import defaultdict

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from simnet_amb_integer import load_truth, n_at  # noqa: E402

EPOCH_S = 30.0

# (label, band_minuend, band_subtrahend) in cycle-difference form, GPS + GAL.
LANES = [
    ("EWL(2,5) G", "L2W", "L5Q"),
    ("WL(1,2)  G", "L1C", "L2W"),
    ("EWL(5,7) E", "L5Q", "L7Q"),
    ("WL(1,5)  E", "L1C", "L5Q"),
]

AGE_BUCKETS = [(1, 5), (6, 20), (21, 60), (61, 200), (201, 10**9)]


def last_slip_tow(slips, sta, sat, sig, tow):
    """tow of the most recent truth slip on (sta,sat,sig) at or before tow, or None."""
    lst = slips.get((sta, sat, sig))
    if not lst:
        return None
    tows = [t for t, _ in lst]
    i = bisect.bisect_right(tows, tow)
    return tows[i - 1] if i else None


def build_vertex_slips(slips):
    """(sta,sat) -> sorted tows of truth slips on any signal.  The estimator's
    refloat response is vertex-scoped (a slip on one band refloats the whole
    (station,sat) vertex), so 'virgin' means any-band virgin."""
    vx = defaultdict(list)
    for (sta, sat, _sig), lst in slips.items():
        vx[(sta, sat)].extend(t for t, _ in lst)
    return {k: sorted(v) for k, v in vx.items()}


def last_vertex_slip(vx, sta, sat, tow):
    lst = vx.get((sta, sat))
    if not lst:
        return None
    i = bisect.bisect_right(lst, tow)
    return lst[i - 1] if i else None


RESPONSE_EVENTS = {"if_slip", "mw_wl_slip", "lli_slip", "attrib_arc"}


def build_vertex_responses(run_dir):
    """(sta,sat) -> sorted tows of estimator response events (refloat-class),
    from network_events.csv.  Truth-blind: false-alarm responses count too,
    which is the point of the second pass."""
    path = os.path.join(run_dir, "logs", "network_events.csv")
    vx = defaultdict(list)
    if not os.path.isfile(path):
        return {}
    for r in csv.DictReader(open(path, newline="")):
        if r.get("event") in RESPONSE_EVENTS and r.get("station") and r.get("sat"):
            vx[(r["station"], r["sat"])].append((float(r["tow"]), r["event"]))
    return {k: sorted(v) for k, v in vx.items()}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("fixture_dir")
    ap.add_argument("--run", required=True)
    args = ap.parse_args()

    try:
        sys.stdout.reconfigure(encoding="utf-8")
    except (AttributeError, ValueError):
        pass

    dd_path = os.path.join(args.run, "logs", "network_dd_fractional.csv")
    rows = list(csv.DictReader(open(dd_path, newline="")))
    stations = {r["sta_a"] for r in rows} | {r["sta_b"] for r in rows}
    n_int, slips = load_truth(args.fixture_dir, stations)

    # (tow, sat, ref, sta_a, sta_b) -> {signal: e}
    quad = defaultdict(dict)
    for r in rows:
        tow = float(r["tow"])
        sat, ref, sa, sb = r["sat_i"], r["ref_sat"], r["sta_a"], r["sta_b"]
        sig = r["signal"]
        legs = [n_at(n_int, slips, sa, sat, sig, tow),
                n_at(n_int, slips, sa, ref, sig, tow),
                n_at(n_int, slips, sb, sat, sig, tow),
                n_at(n_int, slips, sb, ref, sig, tow)]
        if any(v is None for v in legs):
            continue
        dd_true = (legs[0] - legs[1]) - (legs[2] - legs[3])
        quad[(tow, sat, ref, sa, sb)][sig] = float(r["dd_cyc"]) - dd_true

    print(f"quadruple-epochs matched: {len(quad)}")

    vx = build_vertex_slips(slips)

    for label, b1, b2 in LANES:
        # key None = any-band virgin; ('other',) = lane-band virgin but another
        # band slipped a leg (vertex refloat still fired); (lo,hi) = age buckets
        # since the last in-lane-band leg slip.
        buckets = defaultdict(list)
        for (tow, sat, ref, sa, sb), sigs in quad.items():
            if b1 not in sigs or b2 not in sigs:
                continue
            e = sigs[b1] - sigs[b2]
            lane_ages, vx_ages = [], []
            for sta in (sa, sb):
                for sv in (sat, ref):
                    t = last_vertex_slip(vx, sta, sv, tow)
                    if t is not None:
                        vx_ages.append(tow - t)
                    for sig in (b1, b2):
                        t = last_slip_tow(slips, sta, sv, sig, tow)
                        if t is not None:
                            lane_ages.append(tow - t)
            if not vx_ages:
                buckets[None].append(e)
            elif not lane_ages:
                buckets[("other",)].append(e)
            else:
                age_ep = min(lane_ages) / EPOCH_S
                for lo, hi in AGE_BUCKETS:
                    if lo <= age_ep <= hi:
                        buckets[(lo, hi)].append(e)
                        break
                else:
                    buckets[(1, 5)].append(e)   # same-epoch slip -> youngest bucket
        print(f"\n-- {label}  (e = e_{b1} - e_{b2}; round(e)=0 = at own true integer) --")
        order = [None, ("other",)] + AGE_BUCKETS
        for key in order:
            v = buckets.get(key)
            if not v:
                continue
            n = len(v)
            med = sorted(abs(x) for x in v)[n // 2]
            hit = sum(1 for x in v if round(x) == 0) / n * 100.0
            if key is None:
                tag = "VIRGIN — no leg slipped in ANY band yet"
            elif key == ("other",):
                tag = "other-band leg slip only (vertex refloated)"
            else:
                tag = f"age {key[0]:>3}-{key[1] if key[1] < 10**9 else 'inf'} ep since last in-band leg slip"
            print(f"  {tag:<46} n={n:7d}  med|e|={med:7.3f}  round(e)=0 {hit:5.1f}%")

    # ---- second pass: bucket by estimator response events (truth-blind) ----
    rvx = build_vertex_responses(args.run)
    n_ev = sum(len(v) for v in rvx.values())
    print(f"\n==== BY ESTIMATOR RESPONSE EVENT (refloat-class: {sorted(RESPONSE_EVENTS)}; "
          f"{n_ev} events on {len(rvx)} vertices — false alarms included) ====")

    def last_response(sta, sv, tow):
        lst = rvx.get((sta, sv))
        if not lst:
            return None
        i = bisect.bisect_right(lst, (tow, "\xff"))
        return lst[i - 1] if i else None

    for label, b1, b2 in LANES:
        buckets = defaultdict(list)
        byclass = defaultdict(list)     # nearest event class, rows within 20 ep
        for (tow, sat, ref, sa, sb), sigs in quad.items():
            if b1 not in sigs or b2 not in sigs:
                continue
            e = sigs[b1] - sigs[b2]
            best = None
            for sta in (sa, sb):
                for sv in (sat, ref):
                    ev = last_response(sta, sv, tow)
                    if ev is not None and (best is None or ev[0] > best[0]):
                        best = ev
            if best is None:
                buckets[None].append(e)
            else:
                age_ep = (tow - best[0]) / EPOCH_S
                if age_ep <= 20:
                    byclass[best[1]].append(e)
                for lo, hi in AGE_BUCKETS:
                    if lo <= age_ep <= hi:
                        buckets[(lo, hi)].append(e)
                        break
                else:
                    buckets[(1, 5)].append(e)
        print(f"\n-- {label} --")
        for key in [None] + AGE_BUCKETS:
            v = buckets.get(key)
            if not v:
                continue
            n = len(v)
            med = sorted(abs(x) for x in v)[n // 2]
            hit = sum(1 for x in v if round(x) == 0) / n * 100.0
            tag = ("RESPONSE-VIRGIN — no refloat-class event on any leg yet"
                   if key is None else
                   f"age {key[0]:>3}-{key[1] if key[1] < 10**9 else 'inf'} ep since last leg response")
            print(f"  {tag:<52} n={n:7d}  med|e|={med:7.3f}  round(e)=0 {hit:5.1f}%")
        for cls in sorted(byclass):
            v = byclass[cls]
            n = len(v)
            med = sorted(abs(x) for x in v)[n // 2]
            hit = sum(1 for x in v if round(x) == 0) / n * 100.0
            print(f"    within 20 ep of {cls:<12}                       n={n:7d}  med|e|={med:7.3f}  round(e)=0 {hit:5.1f}%")


if __name__ == "__main__":
    main()
