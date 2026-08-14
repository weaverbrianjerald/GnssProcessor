#!/usr/bin/env python3
"""Audit the single-pivot phase-bias gauge: which clock re-realizations were AVOIDABLE.

Two modes, and the important one needs NO truth:

  --run <dir>       POST-HOC AUDIT, works on FIELD runs.  Classifies every
                    phase-bias column removal in network_events.csv as
                      PIVOT-ONLY  (PB removed, CLK retained)  -> AVOIDABLE defect
                      SAT-LEFT    (PB + CLK [+ CB] removed)   -> legitimate
                    and ties each to any CLK_STEP mark in the same epoch.

  --fixture <dir>   PLANNER, sim only.  Ranks candidate clock_datum_station by how
                    many satellites they lose mid-arc, because that count is very
                    nearly the number of avoidable network-wide gauge events the
                    run will suffer.

## The defect

With `network.phase_datum_prioritized` OFF (the default), the network's shared
phase-bias set is taken from `stations.front()`:

    if (!phase_datum_prioritized) {
        shared_phase_keys.clear();
        for (const auto& kv : stations.front().sm->getSatPhaseBiasStates())
            shared_phase_keys.insert(kv.first);
    }                                    -- ApplicationNetwork.cpp:1627-1630

and `stations.front()` IS the clock datum station (`clock_datum_station` becomes
`roster.reference`, ApplicationNetworkSetup.cpp:181-186).  So when the PIVOT
alone loses a satellite, that satellite's phase-bias columns are removed
**network-wide** even though every other station is still tracking it.  The
released direction has nowhere to go and the satellite CLOCK absorbs it.

That is standing invariant #7 with an unbounded decoupling edge: nothing bounds
what the clock can absorb when the column goes, and the re-coupling edge (each
station's ambiguity refloating) is unmanaged and staggered over several epochs.

## Measured (docs/SPEC_pivot_visibility_gauge_defect.md)

24-station sim fixture, truth-clean, `phase_datum_prioritized` OFF:

| datum station | loses | PB-only removals | CLK_STEP | peak cell resid |
|---|---|---|---|---|
| S12000SIM | G15 @303510 | G15 x3 @303510 | -12.06 m | 2214 |
| S13000SIM | G07 @304290 | G07 x3 @304290 | -13.19 m | 1882 |
| S06000SIM | nothing | **none** | **none** | **36** |

and at 60 stations, 6 of 6 PB-only removals matched a datum-station satellite
loss to the epoch, while all 3 PB+CLK+CB removals were satellites genuinely
leaving the network.  The discriminator this script implements is that clean.

## What NOT to conclude

`network.phase_datum_prioritized: true` removes the specific excursion (peak
2214 -> 112) and is NET-HARMFUL on the same fixture: the datum-free satellite
clock contrast degrades 3.07 -> 18.66 m rms and CLK_STEP marks go 1 -> 15.  The
cheap remedy measured here is DATUM ELECTION, not that knob.
"""
import argparse
import collections
import csv
import json
import os
import sys

if hasattr(sys.stdout, "reconfigure"):
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")


def find_events(run_dir):
    for sub in ("logs", "traces", ""):
        p = os.path.join(run_dir, sub, "network_events.csv") if sub else \
            os.path.join(run_dir, "network_events.csv")
        if os.path.exists(p):
            return p
    return None


def audit_run(run_dir):
    p = find_events(run_dir)
    if not p:
        print(f"  FAIL: no network_events.csv under {run_dir} — arm network.trace_events")
        return None
    rows = list(csv.DictReader(open(p, encoding="utf-8", errors="replace")))
    removed = collections.defaultdict(set)      # (tow, sat) -> {PB, CLK, CB}
    added = collections.defaultdict(set)
    clk_step = {}                               # (tow, sat) -> step_m
    for r in rows:
        if r["tier"] == "TOPOLOGY" and r["event"] in ("col_removed", "col_added"):
            lab = (r["detail"] or "").replace("label=", "")
            parts = lab.split(":")
            if len(parts) < 2:
                continue
            tgt = added if r["event"] == "col_added" else removed
            tgt[(float(r["tow"]), parts[1])].add(parts[0])
        elif r["tier"] == "CLK_STEP":
            try:
                clk_step[(float(r["tow"]), r["sat"])] = float(r["statistic"])
            except (ValueError, KeyError):
                clk_step[(float(r["tow"]), r["sat"])] = float("nan")

    pivot_only, sat_left = [], []
    for (tow, sat), kinds in sorted(removed.items()):
        if "PB" not in kinds:
            continue
        rec = {"tow": tow, "sat": sat, "kinds": sorted(kinds),
               "clk_step_m": clk_step.get((tow, sat))}
        (sat_left if "CLK" in kinds else pivot_only).append(rec)

    print(f"\n-- {run_dir} " + "-" * max(4, 60 - len(run_dir)))
    print(f"  phase-bias column removals: {len(pivot_only) + len(sat_left)}"
          f"   ({len(pivot_only)} pivot-only, {len(sat_left)} satellite-left)")
    if pivot_only:
        print("\n  AVOIDABLE — PB removed while the CLK column was RETAINED, i.e. the")
        print("  satellite was still in the network and only the PIVOT stopped seeing it:")
        print(f"    {'tow':>12} {'sat':<5} {'columns':<12} {'CLK_STEP':>12}")
        for r in pivot_only:
            s = f"{r['clk_step_m']:+.3f} m" if r["clk_step_m"] is not None else "(not marked)"
            print(f"    {r['tow']:>12.0f} {r['sat']:<5} {','.join(r['kinds']):<12} {s:>12}")
        marked = [r for r in pivot_only if r["clk_step_m"] is not None]
        if marked:
            worst = max(marked, key=lambda r: abs(r["clk_step_m"]))
            print(f"\n    worst avoidable clock re-realization: {worst['sat']} "
                  f"{worst['clk_step_m']:+.3f} m at tow {worst['tow']:.0f}")
        unmarked = len(pivot_only) - len(marked)
        if unmarked:
            print(f"    ⚠ {unmarked} of these carry NO CLK_STEP mark — either the step was")
            print( "      below clock_disc_jump_test's threshold, or the marking channel is")
            print( "      unarmed (network.emit_clock_disc + network.clock_disc_jump_test).")
            print( "      An unmarked re-realization is SILENT to the consumer.")
    else:
        print("  no avoidable (pivot-only) removals — the datum station never lost a")
        print("  satellite the rest of the network was still tracking.")
    if sat_left:
        print(f"\n  legitimate — satellite left the network entirely (CLK removed too):")
        for r in sat_left:
            print(f"    tow {r['tow']:>10.0f} {r['sat']:<5} {','.join(r['kinds'])}")
    return {"run": run_dir, "pivot_only": pivot_only, "sat_left": sat_left}


def rank_datums(fixture_dir, top, constellations=None):
    man_p = os.path.join(fixture_dir, "network_fixture.json")
    if not os.path.exists(man_p):
        print(f"  FAIL: no network_fixture.json under {fixture_dir}")
        return None
    man = json.load(open(man_p, encoding="utf-8"))
    stations = [s["name"] for s in man["stations"] if not s.get("consumer")]
    out = []
    for s in stations:
        vp = os.path.join(fixture_dir, s, "truth_visibility.csv")
        if not os.path.exists(vp):
            continue
        seq = [set(x for x in (r.get("sat_list") or "").split()
                   if not constellations or x[:1] in constellations)
               for r in csv.DictReader(open(vp, encoding="utf-8", errors="replace"))]
        losses = sum(len(seq[i - 1] - seq[i]) for i in range(1, len(seq)))
        gains = sum(len(seq[i] - seq[i - 1]) for i in range(1, len(seq)))
        out.append({"station": s, "losses": losses, "gains": gains,
                    "min_sats": min((len(x) for x in seq), default=0)})
    out.sort(key=lambda r: (r["losses"], -r["min_sats"]))
    print(f"\n-- candidate clock_datum_station, ranked by mid-arc satellite LOSSES " + "-" * 8)
    print( "  A loss at the pivot removes that satellite's phase-bias columns")
    print( "  NETWORK-WIDE and the clock absorbs the released direction.  Elect a")
    print( "  station with zero losses when the roster offers one.")
    print(f"\n    {'station':<12}{'losses':>8}{'gains':>8}{'min sats':>10}")
    for r in out[:top]:
        print(f"    {r['station']:<12}{r['losses']:>8}{r['gains']:>8}{r['min_sats']:>10}")
    if len(out) > top:
        print(f"    ... {len(out)-top} more; worst is {out[-1]['station']} "
              f"with {out[-1]['losses']} losses")
    print( "\n  NOTE this is a NECESSARY criterion, not the only one: SPEC_synthetic_")
    print( "  minimum_network §9.8 measured 8x more common-level wander from a POLAR")
    print( "  pivot, and the datum-free contrast moved 2.2x on pivot geometry alone.")
    print( "  Rank by losses first, then prefer an equatorial station among the ties.")
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--run", nargs="*", default=[],
                    help="producer run dir(s) to audit (works on FIELD runs — needs only "
                         "network_events.csv)")
    ap.add_argument("--fixture", default=None,
                    help="synth-network fixture dir; ranks candidate datum stations")
    ap.add_argument("--top", type=int, default=8)
    ap.add_argument("--constellations", nargs="*", default=None, metavar="LETTER",
                    help="count losses only for these systems, e.g. --constellations G. "
                         "A satellite the PRODUCER does not estimate cannot cost a gauge "
                         "event, so counting it mis-ranks the roster (measured: S06000SIM "
                         "ranks 0 losses on GPS and 2 with Galileo counted, on a GPS-only "
                         "producer config).")
    ap.add_argument("--json", default=None)
    args = ap.parse_args()
    if not args.run and not args.fixture:
        ap.error("give --run and/or --fixture")

    print("=" * 78)
    print("DATUM GAUGE AUDIT — avoidable phase-bias column removals")
    print("=" * 78)
    payload = {"runs": [], "ranking": None}
    for r in args.run:
        got = audit_run(r)
        if got:
            payload["runs"].append(got)
    if args.fixture:
        payload["ranking"] = rank_datums(
            args.fixture, args.top,
            set(args.constellations) if args.constellations else None)
    print("\n" + "=" * 78)
    if args.json:
        with open(args.json, "w", encoding="utf-8") as fh:
            json.dump(payload, fh, indent=2, default=str)
        print(f"wrote {args.json}")
    # Non-zero when any run carries an avoidable removal, so a harness can gate on it.
    return 1 if any(p["pivot_only"] for p in payload["runs"]) else 0


if __name__ == "__main__":
    sys.exit(main())
