#!/usr/bin/env python3
"""Generate a network producer config from a synth-network fixture manifest.

Exists so the station block can never drift from the fixture.  The producer HOLDS
station coordinates fixed (CORS geometry), so a stale or mistyped position_ecef is
invisible to every downstream gate — a held coordinate is by definition not
estimated, and nothing in the run can tell a 1 km error from a real one.  Reading
them straight out of network_fixture.json removes that whole failure mode.

By default the product lands in the FIXTURE dir (one arm per fixture).  A fixture
that carries several arms — a posture A/B, a short smoke — needs a per-arm product
path, or the second arm silently overwrites the first arm's products.txt and the
grader then grades the wrong run against the right truth.  --products (and
--epochs, for a smoke that walks a prefix of the arc) exist for exactly that.

Usage:
  python scripts/simnet_make_producer_config.py <fixture_dir> <template.json> <out.json>
                                                [--products PATH] [--epochs N]
"""
import argparse
import json
import os
import re
import sys


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("fixture_dir")
    ap.add_argument("template")
    ap.add_argument("out")
    ap.add_argument("--products", default=None,
                    help="product_output_path (default <fixture_dir>/products.txt)")
    ap.add_argument("--epochs", type=int, default=None,
                    help="walk only the first N epochs (default: the whole fixture arc)")
    ap.add_argument("--datum-station", default=None,
                    help="network.clock_datum_station, explicit override.  When OMITTED "
                         "the generator ELECTS it (QUEUE item 2): fewest mid-arc "
                         "satellite losses over the post-stride roster, most equatorial "
                         "among ties (SPEC_pivot_visibility_gauge_defect section 4: 5.9x "
                         "datum-free contrast + a 40 percent T3 cut for clean election; "
                         "section 9.8: 8x wander from a polar pivot).  An explicit "
                         "station absent from the roster is now a hard error, never a "
                         "silent fallback.")
    ap.add_argument("--constellations", nargs="*", default=None, metavar="LETTER",
                    help="count election losses only for these systems (match the "
                         "producer's active_systems; a satellite the producer does not "
                         "estimate cannot cost a gauge event)")
    ap.add_argument("--stride", type=int, default=1,
                    help="use every Nth solve station (default 1 = all).  This is the "
                         "ONLY correct way to subset a Fibonacci roster: its index is "
                         "monotone in latitude, so a PREFIX is a latitude band, not a "
                         "network, while a stride stays near-uniform on the sphere.  The "
                         "consumer is never strided out.")
    args = ap.parse_args()
    fixture_dir, template, out = args.fixture_dir, args.template, args.out

    with open(os.path.join(fixture_dir, "network_fixture.json"), encoding="utf-8") as fh:
        man = json.load(fh)

    src = open(template, encoding="utf-8").read()
    # Strip // comments only for the parse; we rewrite the whole file from the parsed
    # object, so the template's commentary does not survive (the generated file says
    # where it came from instead).
    cfg = json.loads(re.sub(r"^\s*//.*$", "", src, flags=re.M))

    solve = [s for s in man["stations"] if not s.get("consumer")]
    cons = [s for s in man["stations"] if s.get("consumer")]
    if args.stride > 1:
        n_all = len(solve)
        solve = solve[::args.stride]
        print(f"stride {args.stride}: {n_all} -> {len(solve)} solve stations "
              f"(near-uniform subset; a prefix would be a latitude band)")

    # ABSOLUTIZE rinex_path too (same rationale as product_output_path below):
    # synth-network stores whatever path string it was launched with, so a
    # fixture generated from a relative output dir carries relative paths, and
    # the producer — which runs from its own per-arm CWD — then resolves them
    # against the run dir and fails every station load in 2 ms.
    def _abs(p):
        return os.path.abspath(p).replace("\\", "/")

    cfg.setdefault("network", {})
    cfg["network"]["stations"] = [
        {"name": s["name"], "rinex_path": _abs(s["rinex_path"]),
         "position_ecef": s["position_ecef"]}
        for s in solve
    ]
    if cons:
        c = cons[0]
        cfg["network"].setdefault("closed_loop", {})["station"] = {
            "name": c["name"], "rinex_path": _abs(c["rinex_path"]),
            "position_ecef": c["position_ecef"],
        }
    # QUEUE item 2 — datum-station election by criterion, not luck.  An explicit
    # --datum-station wins but must exist in the (post-stride) roster: the old
    # silent fallback to stations[0] handed a Fibonacci roster its MOST POLAR
    # station (§9.8: ~8x more common-level wander), which is precisely the
    # "arrived at by luck" failure the item names.  With no explicit choice, the
    # election runs: fewest mid-arc satellite losses (each pivot loss is a
    # network-wide avoidable gauge event — the measured 5.9x contrast / 40% T3
    # lever), most equatorial among ties, name for determinism (the elected value
    # is warm-resume-fingerprinted; a fixed roster must always elect the same
    # station).
    roster_names = {s["name"] for s in solve}
    if args.datum_station:
        if args.datum_station not in roster_names:
            print(f"ERROR: --datum-station '{args.datum_station}' is not in the "
                  f"{len(solve)}-station (post-stride) roster.  Election ranking:")
            sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
            from datum_gauge_audit import compute_rank
            rank = compute_rank(fixture_dir,
                                set(args.constellations) if args.constellations else None,
                                roster=roster_names) or []
            for r in rank[:8]:
                print(f"    {r['station']:<12} losses {r['losses']:>3}  "
                      f"lat {r['lat_deg']:+.1f}")
            return 2
        cfg["network"]["clock_datum_station"] = args.datum_station
    else:
        sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
        from datum_gauge_audit import compute_rank
        rank = compute_rank(fixture_dir,
                            set(args.constellations) if args.constellations else None,
                            roster=roster_names)
        if rank:
            winner = rank[0]
            cfg["network"]["clock_datum_station"] = winner["station"]
            lat = (f"{winner['lat_deg']:+.1f} deg" if winner["lat_deg"] is not None
                   else "lat ?")
            print(f"ELECTED clock_datum_station = {winner['station']} "
                  f"({winner['losses']} mid-arc losses, {lat}; "
                  f"{len(rank)} candidates ranked by losses then |lat|)")
            if winner["losses"] > 0:
                print(f"  NOTE: the roster offers no zero-loss candidate — the elected "
                      f"datum still loses {winner['losses']} satellite(s) mid-arc; "
                      f"gate the run with datum_gauge_audit.py --run.")
        else:
            # No truth_visibility (non-sim fixture): geometry-only election.
            def lat_of(s):
                x, y, z = s["position_ecef"]
                import math
                return abs(math.degrees(math.atan2(z, math.hypot(x, y))))
            winner = min(solve, key=lambda s: (round(lat_of(s), 1), s["name"]))
            cfg["network"]["clock_datum_station"] = winner["name"]
            print(f"ELECTED clock_datum_station = {winner['name']} (geometry-only: no "
                  f"truth_visibility.csv in this fixture; most equatorial station).")

    # Product path and any run-dir-derived path follow the fixture directory.
    # ABSOLUTIZE: the producer resolves a relative product_output_path against its
    # own hardcoded "logs" dir, so a relative fixture path silently becomes
    # logs/<path> and the product lands somewhere nobody looks — the run still
    # completes and only the round-trip self-check notices, in a warning.
    fx = os.path.abspath(fixture_dir).replace("\\", "/")
    cfg["network"]["product_output_path"] = (
        os.path.abspath(args.products).replace("\\", "/") if args.products
        else f"{fx}/products.txt")

    # Cover the whole arc with margin; a short max_duration_s truncates silently.
    # --epochs walks a PREFIX instead: (N-1) intervals is the span of N epochs, and
    # the margin is deliberately sub-interval so N+1 epochs cannot sneak in.
    n_ep = int(man["n_epochs"]) if args.epochs is None else min(args.epochs, int(man["n_epochs"]))
    dur = (float(man["interval_s"]) * n_ep + 300.0 if args.epochs is None
           else float(man["interval_s"]) * (n_ep - 1) + 1.0)
    cfg.setdefault("processing", {})["max_duration_s"] = dur

    cfg["_comment"] = [
        f"GENERATED by scripts/simnet_make_producer_config.py from {fx}/network_fixture.json",
        f"template: {template}",
        (f"STRIDE {args.stride} subset of the roster; " if args.stride > 1 else "")
        + f"{len(solve)} solve stations + {len(cons)} consumer; "
        f"{man['n_epochs']} epochs @ {man['interval_s']} s"
        + (f"; WALKING THE FIRST {n_ep} ONLY (--epochs)" if args.epochs is not None else ""),
        "Do not hand-edit the station block — regenerate it, or the held coordinates",
        "can silently drift from the fixture and nothing downstream will notice.",
    ]

    with open(out, "w", encoding="utf-8") as fh:
        json.dump(cfg, fh, indent=2)
        fh.write("\n")
    print(f"wrote {out}: {len(solve)} solve + {len(cons)} consumer, max_duration_s={dur:.0f}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
