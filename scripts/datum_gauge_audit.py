#!/usr/bin/env python3
"""Audit the single-pivot phase-bias gauge: which clock re-realizations were avoidable.

Two modes, and the important one needs no truth:

  --run <dir>       Post-hoc audit, works on field runs. Classifies every
                    phase-bias column removal in network_events.csv as
                      PIVOT-ONLY  (PB removed, CLK retained)  -> avoidable defect
                      SAT-LEFT    (PB + CLK [+ CB] removed)   -> legitimate
                    and ties each to any CLK_STEP mark in the same epoch.

  --fixture <dir>   Planner, sim only. Ranks candidate clock_datum_station by how
                    many satellites they lose mid-arc, because that count is very
                    nearly the number of avoidable network-wide gauge events the
                    run will suffer.

## The defect

With `network.phase_datum_prioritized` off (the default), the network's shared
phase-bias set is taken from `stations.front()`:

    if (!phase_datum_prioritized) {
        shared_phase_keys.clear();
        for (const auto& kv : stations.front().sm->getSatPhaseBiasStates())
            shared_phase_keys.insert(kv.first);
    }                                    (ApplicationNetwork.cpp:1627-1630)

and `stations.front()` is the clock datum station (`clock_datum_station` becomes
`roster.reference`, ApplicationNetworkSetup.cpp:181-186). So when the pivot
alone loses a satellite, that satellite's phase-bias columns are removed
network-wide even though every other station is still tracking it. The
released direction has nowhere to go and the satellite clock absorbs it.

That is standing invariant #7 with an unbounded decoupling edge: nothing bounds
what the clock can absorb when the column goes, and the re-coupling edge (each
station's ambiguity refloating) is unmanaged and staggered over several epochs.

## Measured (docs/SPEC_pivot_visibility_gauge_defect.md)

24-station sim fixture, truth-clean, `phase_datum_prioritized` off:

| datum station | loses | PB-only removals | CLK_STEP | peak cell resid |
|---|---|---|---|---|
| S12000SIM | G15 @303510 | G15 x3 @303510 | -12.06 m | 2214 |
| S13000SIM | G07 @304290 | G07 x3 @304290 | -13.19 m | 1882 |
| S06000SIM | nothing | none | none | 36 |

and at 60 stations, 6 of 6 PB-only removals matched a datum-station satellite
loss to the epoch, while all 3 PB+CLK+CB removals were satellites genuinely
leaving the network. The discriminator this script implements is that clean.

## What not to conclude

`network.phase_datum_prioritized: true` removes the specific excursion (peak
2214 -> 112) and is net-harmful on the same fixture: the datum-free satellite
clock contrast degrades 3.07 -> 18.66 m rms and CLK_STEP marks go 1 -> 15. The
cheap remedy measured here is datum election, not that knob.

## QUEUE item 2 plumbing (2026-08-21)

  --config <json>   Field-config preflight. Flattens the config through its
                    "extends" chain (scripts/gnss_config_chain.py, the same
                    merge the loader performs) and checks the datum-station
                    declaration: explicitly set (the empty-string default is
                    stations.front(), the polar trap), present in the roster,
                    not polar while an equatorial alternative exists (--allow-
                    polar overrides; §9.8 measured 8x wander from a polar
                    pivot), and the marking channel armed (emit_clock_disc +
                    clock_disc_jump_test; an unmarked re-realization is
                    silent to the consumer). Visibility continuity cannot be
                    checked statically on a field config; that is what --run
                    grades post-hoc, truth-free, so run it on every arm.
                    Exit 1 on any failed check (harness-gate semantics, same
                    as --run's avoidable-removal gate).

  The election itself (used by scripts/simnet_make_producer_config.py):
  rank by mid-arc losses, then |geocentric latitude| among ties (the §9.8
  equatorial preference, now enforced rather than advised), then name for
  determinism. The elected value lands in the config and is therefore
  fingerprint-stable (SnapshotFingerprint.cpp: the datum station is in the
  warm-resume fingerprint, so election must never vary for a fixed roster).
"""
import argparse
import collections
import csv
import json
import math
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


def geocentric_lat_deg(ecef):
    """Geocentric latitude in degrees; precision is irrelevant for a tiebreak."""
    x, y, z = ecef
    return math.degrees(math.atan2(z, math.hypot(x, y)))


def compute_rank(fixture_dir, constellations=None, roster=None):
    """The election ranking, no printing: mid-arc losses first, |latitude| among
    ties (§9.8: 8x wander from a polar pivot), then name for determinism (the
    elected value is warm-resume-fingerprinted, so a fixed roster must always
    elect the same station). `roster` restricts candidates (e.g. the
    post-stride surviving set in the config generator)."""
    man_p = os.path.join(fixture_dir, "network_fixture.json")
    if not os.path.exists(man_p):
        return None
    man = json.load(open(man_p, encoding="utf-8"))
    out = []
    for s in man["stations"]:
        if s.get("consumer"):
            continue
        if roster is not None and s["name"] not in roster:
            continue
        vp = os.path.join(fixture_dir, s["name"], "truth_visibility.csv")
        if not os.path.exists(vp):
            continue
        seq = [set(x for x in (r.get("sat_list") or "").split()
                   if not constellations or x[:1] in constellations)
               for r in csv.DictReader(open(vp, encoding="utf-8", errors="replace"))]
        losses = sum(len(seq[i - 1] - seq[i]) for i in range(1, len(seq)))
        gains = sum(len(seq[i] - seq[i - 1]) for i in range(1, len(seq)))
        lat = geocentric_lat_deg(s["position_ecef"]) if s.get("position_ecef") else None
        out.append({"station": s["name"], "losses": losses, "gains": gains,
                    "min_sats": min((len(x) for x in seq), default=0),
                    "lat_deg": lat})
    out.sort(key=lambda r: (r["losses"],
                            round(abs(r["lat_deg"]), 1) if r["lat_deg"] is not None else 90.0,
                            r["station"]))
    return out


def rank_datums(fixture_dir, top, constellations=None):
    out = compute_rank(fixture_dir, constellations)
    if out is None:
        print(f"  FAIL: no network_fixture.json under {fixture_dir}")
        return None
    print(f"\n-- candidate clock_datum_station, ranked by LOSSES then |latitude| " + "-" * 8)
    print( "  A loss at the pivot removes that satellite's phase-bias columns")
    print( "  NETWORK-WIDE and the clock absorbs the released direction; among the")
    print( "  zero-loss candidates the most equatorial wins (§9.8: 8x wander from a")
    print( "  polar pivot).  The sort IS the election.")
    print(f"\n    {'station':<12}{'losses':>8}{'gains':>8}{'min sats':>10}{'lat':>9}")
    for r in out[:top]:
        lat = f"{r['lat_deg']:+.1f}" if r["lat_deg"] is not None else "?"
        print(f"    {r['station']:<12}{r['losses']:>8}{r['gains']:>8}{r['min_sats']:>10}{lat:>9}")
    if len(out) > top:
        print(f"    ... {len(out)-top} more; worst is {out[-1]['station']} "
              f"with {out[-1]['losses']} losses")
    return out


# Preflight classification bounds.  §9.8's measured pair was -68.6 deg (0.193 m
# wander) vs +1.1 deg (0.024 m); the bounds put a wide neutral band between the
# classes so the gate fires on the measured regime, not on mid-latitudes.
POLAR_LAT_DEG = 55.0
EQUATORIAL_LAT_DEG = 30.0


def preflight_config(cfg_path, allow_polar=False):
    """Static field-config preflight (QUEUE item 2). Returns (n_fail, report dict)."""
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    from gnss_config_chain import load_config, ConfigChainError
    print(f"\n-- preflight {cfg_path} " + "-" * max(4, 55 - len(cfg_path)))
    try:
        cfg, chain = load_config(cfg_path)
    except ConfigChainError as e:
        print(f"  FAIL: config chain error: {e}")
        return 1, {"config": cfg_path, "error": str(e)}
    if chain:
        print(f"  extends: {' -> '.join(os.path.basename(c) for c in chain)}")
    net = cfg.get("network", {}) if isinstance(cfg, dict) else {}
    roster = net.get("stations", []) or []
    names = [s.get("name") for s in roster]
    datum = net.get("clock_datum_station", "")
    fails = []
    warns = []
    if not datum:
        fails.append("clock_datum_station is UNSET — the loader defaults to "
                     "stations.front(), which is a roster-order accident, not a "
                     "product-quality decision (the polar trap on Fibonacci rosters).")
    elif datum not in names:
        fails.append(f"clock_datum_station '{datum}' is not in the {len(names)}-station "
                     f"roster — the loader will hard-fail this config.")
    else:
        st = next(s for s in roster if s.get("name") == datum)
        lats = {s["name"]: geocentric_lat_deg(s["position_ecef"])
                for s in roster if s.get("position_ecef")}
        lat = lats.get(datum)
        if lat is not None:
            eq_alts = [n for n, l in lats.items()
                       if n != datum and abs(l) <= EQUATORIAL_LAT_DEG]
            if abs(lat) >= POLAR_LAT_DEG and eq_alts:
                msg = (f"clock_datum_station '{datum}' is POLAR ({lat:+.1f} deg) while "
                       f"{len(eq_alts)} equatorial candidate(s) exist (§9.8: 8x "
                       f"common-level wander from a polar pivot).")
                (warns if allow_polar else fails).append(
                    msg + ("  [--allow-polar]" if allow_polar else
                           "  Re-elect, or pass --allow-polar to accept."))
            else:
                print(f"  datum '{datum}' lat {lat:+.1f} deg — geometry OK")
    # Marking channel: without these, an avoidable re-realization is silent to
    # the consumer (the --run audit flags exactly this population).
    for knob in ("emit_clock_disc", "clock_disc_jump_test"):
        if not net.get(knob, False):
            warns.append(f"network.{knob} is unarmed — avoidable clock re-realizations "
                         f"will carry no CLK_STEP mark (silent to the consumer and "
                         f"invisible to --run's worst-case readout).")
    if not (net.get("cycle_slip", {}) or {}).get("datum_station_detect", False):
        warns.append("network.cycle_slip.datum_station_detect is unarmed — the elected "
                     "datum station has no slip response (standing invariant #1).")
    for f in fails:
        print(f"  FAIL: {f}")
    for w in warns:
        print(f"  warn: {w}")
    if not fails:
        print("  preflight PASS" + (f" ({len(warns)} warning(s))" if warns else ""))
    print("  NOTE: visibility CONTINUITY is not statically checkable — gate every run")
    print("  post-hoc with --run <run_dir> (exit 1 on any avoidable removal).")
    return len(fails), {"config": cfg_path, "datum": datum,
                        "fails": fails, "warns": warns}


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
    ap.add_argument("--config", nargs="*", default=[],
                    help="config JSON(s) to preflight (QUEUE item 2): flattens the "
                         "extends chain and gates the datum-station declaration")
    ap.add_argument("--allow-polar", action="store_true",
                    help="downgrade the polar-datum preflight FAIL to a warning "
                         "(a deliberate polar datum is a measured 8x wander cost)")
    ap.add_argument("--json", default=None)
    args = ap.parse_args()
    if not args.run and not args.fixture and not args.config:
        ap.error("give --run, --fixture and/or --config")

    print("=" * 78)
    print("DATUM GAUGE AUDIT — avoidable phase-bias column removals")
    print("=" * 78)
    payload = {"runs": [], "ranking": None, "preflight": []}
    n_preflight_fails = 0
    for c in args.config:
        n, rep = preflight_config(c, allow_polar=args.allow_polar)
        n_preflight_fails += n
        payload["preflight"].append(rep)
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
    # Non-zero when any run carries an avoidable removal OR any preflight check
    # failed, so a harness can gate on either.
    return 1 if (n_preflight_fails
                 or any(p["pivot_only"] for p in payload["runs"])) else 0


if __name__ == "__main__":
    sys.exit(main())
