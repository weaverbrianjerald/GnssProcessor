#!/usr/bin/env python3
"""Generate the 305-station AGU scaling configs (GPS-only, then GPS+Galileo).

Same experiment as ``make_agu_configs.py`` at 5x the stations.  The roster is
lifted from ``_gg305_pdfix_deg15_smoke.json`` (the only 305 config already on the
PD-safe-fold lineage) but the POSTURE comes from ``_ft63_pdfix_v3.json``, because
the gg305 config differs from the validated FT63 posture in two ways that matter:

  * it does not arm ``adm_screen`` -- the measurement-admission screen is what
    retired both DOY-129 withhold eras at their origin; without it this day is
    not clean;
  * it runs SH ``max_degree`` 15, which the P3b additive ladder measured as an
    expensive NULL at 63 stations (+34 % wall, mature DD steps and total_IF
    unchanged).  Degree 8 is the validated setting and the cheaper one.

Extending the FT63 config inherits both correctly; only the roster, the
constellation set and the signal priorities are overridden here.

Usage:  python scripts/make_agu305_configs.py [--hours 3.0]
"""
from __future__ import annotations

import argparse
import json
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
SUITE = REPO / "config" / "test_suite"
ROSTER_CFG = SUITE / "_gg305_pdfix_deg15_smoke.json"
PARENT = SUITE / "_ft63_pdfix_v3.json"

HELD_OUT = "WTZZ00DEU"
REPLAY_HELD_OUT = "OBE400DEU"
TRUTH = {"WTZZ00DEU": [4075579.1220, 931853.4443, 4801569.2330]}

ARMS = {
    "gps": (["GPS"], {"GPS": "WCSLXPYMIQ"}),
    "gg":  (["GPS", "Galileo"], {"GPS": "WCSLXPYMIQ", "Galileo": "CBXIQ"}),
}


def build(tag: str, arm: str, duration_s: float, roster: list) -> dict:
    systems, priorities = ARMS[arm]
    keep = [s for s in roster if s["name"] not in (HELD_OUT, REPLAY_HELD_OUT)]
    consumer = next(s for s in roster if s["name"] == HELD_OUT)
    run_dir = f"C:/GnssProcessor/runs/{tag}"
    return {
        "_comment": [
            f"AGU26 305-station scaling arm '{arm}' ({tag}).",
            f"Roster {len(roster)} -> {len(keep)} ({HELD_OUT} + {REPLAY_HELD_OUT} held out);",
            f"constellations {'+'.join(systems)}; duration {duration_s/3600.0:.2f} h.",
            "Posture inherited from _ft63_pdfix_v3.json (PD-safe fold, D5 v3,",
            "adm_screen, SH degree 8, W6 self-contained OSB, Jul-28 armed set).",
            "signal_priorities is an ATTRIBUTE STRING here to match the producer",
            "lineage verbatim; note there is no fail-loud guard for a MISSING",
            "GPS/Galileo entry (only GLONASS has one) -- a missing entry emits an",
            "EMPTY product, so both keys are set explicitly above.",
        ],
        "extends": [str(PARENT).replace("\\", "/")],
        "processing": {
            "max_duration_s": duration_s,
            "active_systems": systems,
            "signal_priorities": priorities,
            "frequency_mode": "all",
            "channel_mode": "single",
        },
        # SH: degree 15, SINGLE shell (user directive 2026-08-04).  Costs only
        # ~+5-8 % at 305 stations -- far less than the +34 % the P3b ladder saw at
        # 63, because per-station work dominates at this scale.
        "estimation": {
            "global_vtec_sh": {"max_degree": 15, "layer2_max_degree": 0},
        },
        "network": {
            "max_duration_s": duration_s,
            # The MW/WL lane layer is the 305-station bottleneck: applyLaneMeasurement
            # is 84.7 % of the run (232 ms/row of dense O(dim^2) downdate on a 2K x 2K
            # joint block, max K_c 3866).  OFF takes a settled epoch 310 s -> 26.7 s
            # (12x) and shrinks the largest component 3866 -> 2359, while costing
            # nothing measurable: AR fix rate is 0 % and med|frac| uniform 0.25 with
            # it ON.
            #
            # SCOPE: this is a LARGE-network setting.  At ~60 stations the same change
            # is a small net LOSS (dd-build -73 % but install-post ~3x; 2084 -> 2207
            # ms/epoch), because the 2K x 2K scaling only bites once K is large.
            #
            # UNGRADED SIDE EFFECT: it changes carry RETENTION -- whole-block
            # marginalizations go 0-over-1440-epochs (lane ON) to 735-over-31 (lane
            # OFF), since a per-band component whose chords all refloat is dropped
            # wholesale where the joint block survived if either band did.  Solve-
            # affecting: grade total_IF / a00 / DD steps on a >=2 h run before
            # quoting numbers from this posture.
            "lane_carry": {"enable": False},
            # Diagnostic only, and network_ionosphere.csv is FRAME-BROKEN (emits x_r
            # without its seeded linearization point).  Use network_vtec_sh.csv +
            # scripts/sh_vtec.py for the estimated ionosphere.  Left ON here only
            # because it is cheap (<0.2 % of the epoch) and useful for slip forensics.
            "trace_ionosphere": True,
            "product_output_path": f"{run_dir}/products.txt",
            "state_snapshot_path": f"{run_dir}/state_snapshot.bin",
            "stations": keep,
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
    ap = argparse.ArgumentParser()
    ap.add_argument("--hours", type=float, default=3.0)
    args = ap.parse_args()

    roster = json.loads(ROSTER_CFG.read_text())["network"]["stations"]
    wtzz = next(s for s in roster if s["name"] == HELD_OUT)
    d = max(abs(a - b) for a, b in zip(wtzz["position_ecef"], TRUTH[HELD_OUT]))
    print(f"roster {len(roster)} stations; {HELD_OUT} roster-vs-SSC max |d| = {d*1000:.1f} mm")

    for arm in ("gps", "gg"):
        for suffix, dur in (("_smoke", 300.0), ("", args.hours * 3600.0)):
            tag = f"agu305_{arm}{suffix}"
            cfg = build(tag, arm, dur, roster)
            (SUITE / f"_{tag}.json").write_text(json.dumps(cfg, indent=1) + "\n")
            (REPO / "runs" / tag).mkdir(parents=True, exist_ok=True)
            print(f"  wrote _{tag}.json: {len(cfg['network']['stations'])} sta, "
                  f"{'+'.join(ARMS[arm][0])}, {dur/3600.0:.2f} h")


if __name__ == "__main__":
    main()
