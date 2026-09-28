#!/usr/bin/env python3
"""Generate a PPP-RTK network config from a resolved station list.

Loads the committed network config as a template (settings, estimation, filter and
model blocks), strips JSONC comments, and replaces network.stations[] with every
station of the resolved set.  Sets network.clock_datum_station to the chosen datum
station and network.closed_loop.station to the held-out consumer; both must be in
the station set.  Writes plain JSON, which the C++ nlohmann loader accepts.
"""
from __future__ import annotations

import argparse
import json
from pathlib import Path

_REPO = Path(__file__).resolve().parent.parent


def strip_jsonc(text: str) -> str:
    out = []
    i, n = 0, len(text)
    instr = esc = False
    while i < n:
        c = text[i]
        if instr:
            out.append(c)
            if esc:
                esc = False
            elif c == "\\":
                esc = True
            elif c == '"':
                instr = False
            i += 1
            continue
        if c == '"':
            instr = True; out.append(c); i += 1; continue
        if c == "/" and i + 1 < n and text[i + 1] == "/":
            while i < n and text[i] not in "\r\n":
                i += 1
            continue
        if c == "/" and i + 1 < n and text[i + 1] == "*":
            i += 2
            while i + 1 < n and not (text[i] == "*" and text[i + 1] == "/"):
                i += 1
            i += 2
            continue
        out.append(c); i += 1
    return "".join(out)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--stations", default=str(_REPO / "config" / "test_suite" / "global_network_stations.json"))
    ap.add_argument("--template", default=str(_REPO / "config" / "ppp_rtk_network_internal.json"))
    ap.add_argument("--out", default=str(_REPO / "config" / "test_suite" / "ppp_rtk_network_global.json"))
    ap.add_argument("--datum", default="MBAR00UGA", help="clock-datum station (must be in the set, not the consumer)")
    ap.add_argument("--consumer", default="JPLM00USA", help="held-out consumer station (excluded from the estimator)")
    ap.add_argument("--year", type=int, default=2024)
    ap.add_argument("--doy", type=int, default=129)
    ap.add_argument("--rate", default="30S")
    ap.add_argument("--max-duration", type=float, default=3600.0)
    args = ap.parse_args()

    cfg = json.loads(strip_jsonc(Path(args.template).read_text()))
    stations = json.loads(Path(args.stations).read_text())
    datedir = f"{args.year:04d}-{args.doy:03d}"
    yd = f"{args.year:04d}{args.doy:03d}0000"

    def rinex(long: str) -> str:
        return f"C:/GnssProcessor/data/{datedir}/obs/{long.lower()}/{long}_R_{yd}_01D_{args.rate}_MO.rnx"

    by_name = {s["long"]: s for s in stations}
    if args.datum not in by_name:
        print(f"ERROR: datum {args.datum} not in station set"); return 1
    if args.consumer not in by_name:
        print(f"ERROR: consumer {args.consumer} not in station set"); return 1

    cfg["network"]["stations"] = [
        {"name": s["long"], "rinex_path": rinex(s["long"]), "position_ecef": s["ecef"]}
        for s in stations
    ]
    cfg["network"]["clock_datum_station"] = args.datum
    cfg["network"]["product_output_path"] = "network_products_global.txt"
    cfg["network"]["closed_loop"]["station"] = {
        "name": args.consumer,
        "rinex_path": rinex(args.consumer),
        "position_ecef": by_name[args.consumer]["ecef"],
    }
    cfg["network"]["closed_loop"]["enable"] = True
    cfg["network"]["closed_loop"]["consumer_undifferenced"] = True
    cfg["network"]["closed_loop"]["consume_network_products"] = True
    cfg["network"]["closed_loop"]["consume_network_clock"] = False   # bias-only: the consumer does not take the network clock
    cfg["network"]["closed_loop"]["ar_wl_only"] = False
    cfg["files"]["observation_path"] = rinex(args.datum)
    cfg["processing"]["max_duration_s"] = args.max_duration

    Path(args.out).write_text(json.dumps(cfg, indent=2))
    print(f"wrote {args.out}: {len(cfg['network']['stations'])} stations "
          f"(estimator {len(cfg['network']['stations']) - 1}, datum {args.datum}, "
          f"held-out consumer {args.consumer}), max_duration {args.max_duration}s")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
