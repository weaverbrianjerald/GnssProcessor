#!/usr/bin/env python3
"""Generate the AGU-abstract network configs from the FT63 pdfix_v3 lineage.

The abstract needs one producer run whose emitted products a held-out consumer
then positions itself from. Two stations are lifted out of the roster:

  * ``WTZZ00DEU``: the co-located test. Its siblings stay in the network
    (WTZR at 1.6 m and WTZS at 69 m), so the held-out station's ionosphere and
    troposphere are sampled by the network essentially exactly. Residual error
    is therefore attributable to the clock and bias products rather than to
    atmospheric representation.
  * ``OBE400DEU``: the operational case at about 167 km, graded afterwards
    through the consumer-replay driver against the same written product file.

Everything else is inherited verbatim from ``_ft63_pdfix_v3.json`` (PD-safe DD
fold, D5 v3 corroboration, the Jul-28 armed set, W6 self-contained OSB).

Usage: python scripts/make_agu_configs.py
"""
from __future__ import annotations

import json
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
SUITE = REPO / "config" / "test_suite"
ROOT_CFG = SUITE / "_ft63_24h_conv.json"        # the roster lives here
PARENT = SUITE / "_ft63_pdfix_v3.json"          # the posture this generator extends

HELD_OUT = "WTZZ00DEU"                          # co-located (WTZR 1.6 m, WTZS 69 m)
REPLAY_HELD_OUT = "OBE400DEU"                   # operational, ~167 km

# ITRF2020 @ 2024:129 from scripts/resolve_itrf_ecef.py (SSC, engine never reads it).
TRUTH = {
    "WTZZ00DEU": [4075579.1220, 931853.4443, 4801569.2330],
    "OBE400DEU": [4186704.2141, 834903.7857, 4723664.9317],
}


def _strip_comments(obj):
    """Drop the ``_comment`` keys so generated configs stay readable."""
    if isinstance(obj, dict):
        return {k: _strip_comments(v) for k, v in obj.items() if k != "_comment"}
    if isinstance(obj, list):
        return [_strip_comments(v) for v in obj]
    return obj


def load_roster() -> list:
    cfg = json.loads(ROOT_CFG.read_text())
    return cfg["network"]["stations"]


def station_by_name(roster: list, name: str) -> dict:
    for s in roster:
        if s["name"] == name:
            return s
    raise KeyError(f"{name} not in roster")


def build(duration_s: float, tag: str, roster: list) -> dict:
    """One producer config: 61-station roster + armed closed-loop consumer."""
    keep = [s for s in roster if s["name"] not in (HELD_OUT, REPLAY_HELD_OUT)]
    consumer = station_by_name(roster, HELD_OUT)
    run_dir = f"C:/GnssProcessor/runs/{tag}"
    return {
        "_comment": [
            f"AGU26 abstract producer run ({tag}).  Extends _ft63_pdfix_v3.json",
            "(PD-safe DD fold + D5 v3 corroboration + Jul-28 armed set + W6",
            "self-contained satellite OSB).  Deltas, and no others:",
            f"  1. roster {len(roster)} -> {len(keep)}: {HELD_OUT} and {REPLAY_HELD_OUT} lifted out;",
            "  2. network.trace_ionosphere ON (panel (a) of the 4-tile figure has no",
            "     data without it -- the loader default is FALSE);",
            f"  3. closed_loop armed on {HELD_OUT}, whose siblings WTZR (1.6 m) and",
            "     WTZS (69 m) REMAIN in the network -- atmospheric representation error",
            "     is removed by construction, isolating clock/bias product quality;",
            "  4. consume_network_iono ON -- the network's own SH ionosphere is part of",
            "     the correction set (this is PPP-RTK, not PPP), and it is the one lever",
            "     measured to zero the vertical median.  Network solve is unaffected;",
            f"  5. duration {duration_s/3600.0:.2f} h.",
            "Truth ECEF is ITRF2020 from the SSC via scripts/resolve_itrf_ecef.py and is",
            "consumed nowhere in the engine, so grading stays solver-independent.",
        ],
        "extends": [str(PARENT).replace("\\", "/")],
        "processing": {"max_duration_s": duration_s},
        "network": {
            "max_duration_s": duration_s,
            "trace_ionosphere": True,
            "product_output_path": f"{run_dir}/products.txt",
            "state_snapshot_path": f"{run_dir}/state_snapshot.bin",
            "stations": _strip_comments(keep),
            "closed_loop": {
                "enable": True,
                "station": {
                    "name": consumer["name"],
                    "rinex_path": consumer["rinex_path"],
                    "position_ecef": TRUTH[HELD_OUT],
                },
                "consume_network_products": True,
                "consume_network_clock": True,
                "consume_network_iono": True,
                "ar_mode": 1,
                "ar_wl_only": True,
            },
        },
    }


def main() -> None:
    roster = load_roster()
    for name in (HELD_OUT, REPLAY_HELD_OUT):
        station_by_name(roster, name)          # fail loudly if the roster changed

    for tag, dur in (("agu63_smoke", 300.0), ("agu63", 43200.0)):
        cfg = build(dur, tag, roster)
        out = SUITE / f"_{tag}.json"
        out.write_text(json.dumps(cfg, indent=1) + "\n")
        n = len(cfg["network"]["stations"])
        print(f"wrote {out.name}: {n} stations, {dur/3600.0:.2f} h, "
              f"held out {HELD_OUT} (+ {REPLAY_HELD_OUT} for replay)")


if __name__ == "__main__":
    main()
