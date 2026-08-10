#!/usr/bin/env python3
"""Generate consumer-replay arms against an already-written network product file.

``Application::runConsumerReplay`` (armed by
``network.closed_loop.replay_products_path``) drives a held-out PPP-RTK consumer
from a product file WITHOUT re-running the producer -- roughly 30 s per arm
instead of an hour -- so the consumer side can be explored properly against one
fixed product stream.  ``runNetwork`` short-circuits to it before input
validation, so the roster below is a two-station stub that exists only to satisfy
load-time validation; no station is ever loaded or solved.

Arms
----
  base      reproduces the in-run closed loop (the replay/live cross-check)
  noiono    consume_network_iono OFF -- isolates the network ionosphere's value
  biasonly  consume_network_clock OFF -- the fallback leg on external precise
            clock.  This is the leg that dissolves the "we make our own clock"
            motivation, so it is the control, not the headline.
  undiff    consumer_undifferenced ON -- the locked architectural end-state
            (Odijk/Teunissen: the S-basis moves into the product datum)
  pairW     consumer pinned to C1W/C2W    -- explicit ARRAY, see below
  pairX     consumer pinned to C1C/C2X
  obe4      the ~167 km operational held-out station, full consumption

A signal arm MUST be an explicit ARRAY.  ``decideConstellationCanonicals`` picks,
per (band, obs type), the priority-list signal with the highest OBSERVATION COUNT
this epoch and breaks only TIES by priority order, so an attribute string does
not bind the signal.  Listing only the wanted signals removes the competitor from
the candidate set, which is the only way to pin the pair.

WTZZ00DEU tracks C1C C1W C1X C2W C2X C5X (no C2L), so the L2 contrast here is
W vs X.

Usage:  python scripts/make_agu_replay_arms.py --producer-run runs/agu63
"""
from __future__ import annotations

import argparse
import json
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
SUITE = REPO / "config" / "test_suite"

CONSUMERS = {
    "WTZZ00DEU": {
        "rinex_path": "C:/GnssProcessor/data/2024-129/obs/wtzz00deu/"
                      "WTZZ00DEU_R_20241290000_01D_30S_MO.rnx",
        "position_ecef": [4075579.1220, 931853.4443, 4801569.2330],
    },
    "OBE400DEU": {
        "rinex_path": "C:/GnssProcessor/data/2024-129/obs/obe400deu/"
                      "OBE400DEU_R_20241290000_01D_30S_MO.rnx",
        "position_ecef": [4186704.2141, 834903.7857, 4723664.9317],
    },
}

# (closed_loop overrides, processing overrides, consumer station)
ARMS = {
    "base":     ({}, {}, "WTZZ00DEU"),
    "noiono":   ({"consume_network_iono": False}, {}, "WTZZ00DEU"),
    "biasonly": ({"consume_network_clock": False}, {}, "WTZZ00DEU"),
    "undiff":   ({"consumer_undifferenced": True}, {}, "WTZZ00DEU"),
    "pairW":    ({}, {"signal_priorities": {"GPS": ["C1W", "L1C", "C2W", "L2W"]}},
                 "WTZZ00DEU"),
    "pairX":    ({}, {"signal_priorities": {"GPS": ["C1C", "L1C", "C2X", "L2X"]}},
                 "WTZZ00DEU"),
    "obe4":     ({}, {}, "OBE400DEU"),
}


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--producer-run", default="runs/agu63")
    ap.add_argument("--producer-config", default="config/test_suite/_agu63.json")
    ap.add_argument("--tag", default="agu63")
    args = ap.parse_args()

    prod_run = (REPO / args.producer_run).resolve()
    prod_cfg = (REPO / args.producer_config).resolve()
    products = prod_run / "products.txt"
    if not products.is_file():
        raise SystemExit(f"no product file yet: {products}")

    # The stub exists only to satisfy load-time validation, but it MUST contain
    # network.clock_datum_station -- the loader resolves that name against the
    # roster and hard-fails before the replay branch is ever reached.  The name
    # lives up the `extends` chain, so walk it.
    def resolve(path: Path, key: str, depth: int = 0):
        """First definition of network.<key> walking up the `extends` chain.

        A child config that only overrides a knob (e.g. _agu63_pbv2.json) declares
        neither the roster nor the datum station, so both must be inherited.
        """
        if depth > 8:
            return None
        cfg = json.loads(path.read_text())
        got = cfg.get("network", {}).get(key)
        if got:
            return got
        for parent in cfg.get("extends", []):
            got = resolve(Path(parent), key, depth + 1)
            if got:
                return got
        return None

    def resolve_datum(path: Path) -> str | None:
        return resolve(path, "clock_datum_station")

    roster = resolve(prod_cfg, "stations")
    if not roster:
        raise SystemExit(f"no network.stations resolvable from {prod_cfg}")
    datum = resolve_datum(prod_cfg)
    stub = [s for s in roster if s["name"] == datum]
    if not stub:
        raise SystemExit(f"clock_datum_station {datum!r} not in the producer roster")
    stub += [s for s in roster if s["name"] != datum][:1]
    roster_stub = stub
    print(f"roster stub: {[s['name'] for s in roster_stub]} (datum {datum})")

    for arm, (cl_over, proc_over, station) in ARMS.items():
        tag = f"{args.tag}_rp_{arm}"
        run_dir = REPO / "runs" / tag
        run_dir.mkdir(parents=True, exist_ok=True)
        cl = {
            "enable": True,
            "replay_products_path": str(products).replace("\\", "/"),
            "station": {"name": station, **CONSUMERS[station]},
            "consume_network_products": True,
            "consume_network_clock": True,
            "consume_network_iono": True,
            "ar_mode": 1,
            "ar_wl_only": True,
        }
        cl.update(cl_over)
        cfg = {
            "_comment": [
                f"AGU consumer-replay arm '{arm}' against {products.name}.",
                "Producer does NOT run; runNetwork short-circuits to the replay",
                "driver, so the 2-station roster is load-validation ballast only.",
                f"Consumer: {station}; truth is ITRF2020 from the SSC, which the",
                "engine consumes nowhere, so grading stays solver-independent.",
            ],
            "extends": [str(prod_cfg).replace("\\", "/")],
            "processing": proc_over,
            "network": {
                "stations": roster_stub,
                "product_output_path": f"C:/GnssProcessor/runs/{tag}/products.txt",
                "state_snapshot_path": f"C:/GnssProcessor/runs/{tag}/state_snapshot.bin",
                "closed_loop": cl,
            },
        }
        out = SUITE / f"_{tag}.json"
        out.write_text(json.dumps(cfg, indent=1) + "\n")
        print(f"wrote {out.name:28s} consumer={station} "
              f"overrides={ {**cl_over, **proc_over} or '(none)'}")


if __name__ == "__main__":
    main()
