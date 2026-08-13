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
                    help="network.clock_datum_station.  Worth setting per arm: §9.8 "
                         "measured that the common clock level FOLLOWS the datum "
                         "station and wanders ~8x more from a polar one.  Must survive "
                         "--stride; the fallback below is stations[0], the most polar.")
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

    cfg.setdefault("network", {})
    cfg["network"]["stations"] = [
        {"name": s["name"], "rinex_path": s["rinex_path"], "position_ecef": s["position_ecef"]}
        for s in solve
    ]
    if cons:
        c = cons[0]
        cfg["network"].setdefault("closed_loop", {})["station"] = {
            "name": c["name"], "rinex_path": c["rinex_path"],
            "position_ecef": c["position_ecef"],
        }
    if args.datum_station:
        cfg["network"]["clock_datum_station"] = args.datum_station
    # The clock datum station must exist in the roster.  The fallback is stations[0],
    # which on a Fibonacci roster is the MOST POLAR station — and §9.8 measured that
    # the common clock level FOLLOWS the datum station, wandering ~8x more from a
    # polar datum than an equatorial one.  A silent fallback would therefore hand a
    # wander-grading arm the worst available datum, so say it out loud.
    if cfg["network"].get("clock_datum_station") not in {s["name"] for s in solve}:
        want = cfg["network"].get("clock_datum_station")
        cfg["network"]["clock_datum_station"] = solve[0]["name"]
        print(f"WARNING: clock_datum_station '{want}' is not in this fixture's roster; "
              f"fell back to stations[0] = {solve[0]['name']}.  On a Fibonacci roster "
              f"that is the most POLAR station and §9.8 measured ~8x more common-level "
              f"wander from a polar datum — pick an equatorial one explicitly.")

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
