#!/usr/bin/env python3
"""FP-2 fan-out census on the pre-registered §20.2 window: k in [-1, +2].

    python scripts/fp2_fanout_window.py [--runs C:/GnssProcessor/runs]
                                        [--prefix fp2] [--lo -1] [--hi 2]
                                        [--pb-tol 0.5] [--json out.json]

`scripts/fp1_refslip_census.py` is the full mechanism census and reads the
fan-out over k in [0, +5].  §22.7 pre-registers the FP-2 bar on a different
window: ``collateral (non-reference) IF-TD refloats at GOPE k in [-1, +2] must
read 0 on every G24 PIN leg``, which reaches one epoch before the event (an
epoch the [0,+5] window cannot see) and stops at +2.  This script reads exactly
that window and the two companion counts the bar names:

  collateral       T1_if_slip refloat rows at the injected station on satellites
                   other than the injected one, event minus null (a satellite
                   that also refloats in the null is a natural slip, not fan-out);
  reference's own  the same rows on the injected satellite: under
                   `if_phase_td_ref_arm_all` the response lands here instead,
                   which is the fix working rather than a cost;
  PB re-realizations  products ``P`` rows for a collateral satellite whose value
                   moves > --pb-tol m at the event epoch (FP-1's innocent
                   13.7 m ``P/G32/L1C``), with the phase_disc counter and sigma
                   on both sides: invariant #7's declaration edge.

Bars (§22.7, verbatim): collateral 0 on every G24 PIN leg; the reference's own
arcs reset instead; PB datum re-realizations caused by collateral 0.
"""
import argparse
import json
import os
import sys

_HERE = os.path.dirname(os.path.abspath(__file__))
for _p in (_HERE, r"C:\GnssProcessor\scripts"):
    if os.path.isdir(_p) and _p not in sys.path:
        sys.path.insert(0, _p)

from event_field_grade import leg_meta, read_events          # noqa: E402
from fp1_refslip_census import products_at                   # noqa: E402

EPOCH_S = 30.0
# key -> (station, injected sat, is the injected sat a chain reference at k=0?)
LEGS = (("e1",    "GOPE00CZE", "G24", True),
        ("e2",    "GOPE00CZE", "G24", True),
        ("e3g24", "GOPE00CZE", "G24", True),
        ("e5",    "GOPE00CZE", "G24", True),
        ("e3g25", "GOPE00CZE", "G25", False))
ARMS = ("off", "pin")


def refloats(legdir, sta, t0, lo, hi):
    """{sat: [(k, detail)]}: T1_if_slip refloat rows at one station in a window."""
    out = {}
    for r in read_events(legdir) or []:
        if r.get("tier") != "T1_if_slip" or r.get("action") != "refloat":
            continue
        if r.get("station") != sta:
            continue
        try:
            k = int(round((float(r["tow"]) - t0) / EPOCH_S))
        except (KeyError, ValueError):
            continue
        if lo <= k <= hi:
            out.setdefault(r.get("sat"), []).append((k, r.get("detail", "")[:70]))
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--runs", default=r"C:\GnssProcessor\runs")
    ap.add_argument("--prefix", default="fp2")
    ap.add_argument("--lo", type=int, default=-1)
    ap.add_argument("--hi", type=int, default=2)
    ap.add_argument("--pb-tol", type=float, default=0.5)
    ap.add_argument("--json", default=None)
    a = ap.parse_args()

    print("=" * 94)
    print(f"FP-2 FAN-OUT CENSUS   window k in [{a.lo:+d}, {a.hi:+d}]   "
          f"(§22.7's pre-registered bar)")
    print("  collateral = T1_if_slip refloats at the station on OTHER satellites, "
          "event MINUS null")
    print("=" * 94)
    res, fails = {}, []
    for key, sta, sat, is_ref in LEGS:
        print(f"\n--- {key}  station {sta}  injected {sat}"
              + ("" if is_ref else "   (CONTROL: not a chain reference at k=0)"))
        d = {"station": sta, "sat": sat, "arms": {}}
        for arm in ARMS:
            leg = os.path.join(a.runs, f"evleg_{a.prefix}_{key}_{arm}")
            null = os.path.join(a.runs, f"evleg_{a.prefix}_null_{arm}")
            if not os.path.isfile(os.path.join(leg, "exitcode.txt")):
                print(f"  {arm.upper():<4} leg missing")
                fails.append(f"{key}/{arm}: leg missing — the bar below is VACUOUS")
                continue
            t0 = float(leg_meta(leg)["event_tow"])
            L = refloats(leg, sta, t0, a.lo, a.hi)
            N = refloats(null, sta, t0, a.lo, a.hi)
            coll = {s: v for s, v in L.items() if s != sat and s not in N}
            n_coll = sum(len(v) for v in coll.values())
            own = L.get(sat, [])

            tw = int(round(t0))
            pa, _ = products_at(os.path.join(leg, "products.txt"), tw)
            pb, _ = products_at(os.path.join(null, "products.txt"), tw)
            moved_coll = []
            for kk, (va, da, sa_) in pa.items():
                vb = pb.get(kk)
                if vb is None or kk[0] != "P":
                    continue
                if len(kk) > 1 and kk[1] == sat:
                    continue                      # the injected satellite's own PB
                if abs(va - vb[0]) > a.pb_tol:
                    moved_coll.append((round(abs(va - vb[0]), 4), "/".join(kk),
                                       f"disc {vb[1]}->{da}", f"sigma {vb[2]}->{sa_}"))
            moved_coll.sort(reverse=True)

            d["arms"][arm] = {"collateral_sats": sorted(coll), "collateral_rows": n_coll,
                              "own_resets": len(own), "pb_collateral": moved_coll}
            print(f"  {arm.upper():<4} collateral refloats {n_coll:>2} "
                  f"({', '.join(sorted(coll)) or 'none'})   "
                  f"reference's OWN arcs reset {len(own):>2}   "
                  f"collateral PB re-realizations {len(moved_coll)}")
            for s in sorted(coll):
                for k, det in coll[s]:
                    print(f"         collateral  k={k:+d} {s}  {det}")
            for k, det in own:
                print(f"         own         k={k:+d} {sat}  {det}")
            for v, kk, disc, sg in moved_coll:
                print(f"         PB          {kk:<14} |d|={v:9.4f}  {disc}  {sg}")

            if arm == "pin" and is_ref:
                if n_coll:
                    fails.append(f"{key}: {n_coll} collateral refloat(s) on the PIN arm "
                                 f"(bar 0)")
                if moved_coll:
                    fails.append(f"{key}: {len(moved_coll)} collateral PB "
                                 f"re-realization(s) on the PIN arm (bar 0)")
        res[key] = d

    print("\n=== FAN-OUT BARS (§22.7) ===")
    if fails:
        for f in fails:
            print("  FAIL  " + f)
    else:
        print("  PASS  collateral 0 and collateral PB re-realizations 0 on every "
              "G24 PIN leg; the reference's own arcs reset instead")
    if a.json:
        with open(a.json, "w", encoding="utf-8") as fh:
            json.dump({"window": [a.lo, a.hi], "legs": res, "fails": fails}, fh,
                      indent=1, default=str)
        print(f"\njson -> {a.json}")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
