#!/usr/bin/env python3
"""Generate a Q5 consumer-replay arm config (SPEC_day129_frame_guards Phase 0).

The replay driver (`Application::runConsumerReplay`, armed by
`network.closed_loop.replay_products_path`) drives a held-out PPP-RTK consumer
from an already-written network product file.  The two arms of Phase 0 differ in
exactly ONE thing — which GPS code signals the consumer uses:

    A:  processing.signal_priorities.GPS = ["C1W","L1C","C2W","L2W"]
    B:  processing.signal_priorities.GPS = ["C1C","L1C","C2L","L2L"]

(`channel_mode: single` takes ONE canonical code + phase per band from that list;
see ARM_SIGNALS below for why the list must be explicit rather than an attribute
priority string.)

Everything else is inherited from the reference producer leg so the consumer's
modeling matches the run that produced the stream.  The producer NEVER runs: the
`network.stations` roster below exists only to satisfy load-time validation.

Usage:
  python scripts/q5_make_replay_arm.py --arm A --outdir runs/q5/armA [--duration 300]
"""

import argparse
import json
import os
import pathlib

REPO = pathlib.Path(__file__).resolve().parents[1]
LEG = REPO / "runs" / "day129_cont" / "seg01" / "leg.json"
PRODUCTS = REPO / "runs" / "day129_cont" / "seg01" / "products.txt"

# Held-out consumer: NOT in the 305-station roster, DOY-129 obs present, 30 s
# sampling (matches the product cadence exactly), and tracks BOTH arm signal
# pairs.
#
# Truth ECEF is the ITRF2020 / IGS20 monument coordinate, velocity-propagated to
# the observation epoch:
#
#   python scripts/resolve_itrf_ecef.py NRMG00NCL --doy 2024:129
#   -> -5743538.2169  1380503.7952  -2397895.8332   sigma(mm) 3.2/1.4/1.7
#
# from the frozen SSC at data/static/sinex/ITRF2020-IGS-TRF.SSC (SOLN 4,
# 2015.0 + velocity, extrapolated past the 2020.999 window end — the standard
# ITRF usage for a current epoch).  The frame matches the products: the leg runs
# COD MGEX finals, which are IGS20-aligned.  Independent by construction — the
# SSC is consumed NOWHERE in the C++ engine.
#
# This REPLACES the RINEX header `APPROX POSITION XYZ`
# (-5743537.9710, 1380504.0131, -2397896.3726) that Phase 0 graded against.
# That header value is not wrong, it is STALE: it sits on this station's own
# ITRF2020 secular track at epoch 2011.91 with a 2.6 cm perpendicular residual,
# i.e. it is a ~2012 realization of the same monument carried forward unchanged
# while New Caledonia moved 5.07 cm/yr.  The resulting offset is 0.632 m
# (E -0.269 / N -0.571 / U +0.030 — almost pure horizontal).
#
# `ANTENNA: DELTA H/E/N` is 0/0/0 in this file, so header-APPROX, ARP and
# monument are the same point and the swap is a like-for-like replacement.
#
# A truth error is a CONSTANT common to every arm and cannot manufacture the
# time-localized signature Phase 0 tests for — which is why the A/B verdict did
# not depend on this.  It does set the ABSOLUTE level, so the quiet-floor and
# fault-era numbers are only trustworthy against a real datum.  Re-anchor an
# already-run arm onto this truth with `scripts/q5_reanchor_hourly.py`.
CONSUMER = {
    "name": "NRMG00NCL",
    "rinex_path": str(REPO / "data/2024-129/obs/nrmg00ncl/NRMG00NCL_R_20241290000_01D_30S_MO.rnx"),
    "position_ecef": [-5743538.2169, 1380503.7952, -2397895.8332],
}

# The arm MUST be expressed as an explicit signal ARRAY, not an attribute-priority
# STRING.  PppModel::decideConstellationCanonicals picks, per (band, obs type), the
# priority-list signal with the HIGHEST OBSERVATION COUNT this epoch and only breaks
# TIES by priority order (PppModel.cpp:463-501).  NRMG tracks C2W on all 32 GPS
# satellites and C2L on ~25, so C2W wins on count under ANY attribute string — a
# "C-first" string yields (C1C, C2W), an L1-only contrast, not the intended pair
# contrast.  Listing only the wanted signals removes the competitor from the
# candidate set entirely, which is the only way to bind L2.
#
# L1 phase is L1C in BOTH arms: NRMG reports C1W but no L1W (the standard RINEX-3
# P(Y)-code / C-A-phase convention), and the phase side is not what the +49 m
# code-bias fault lives in (phase biases moved <= 4 mm at the event).
ARM_SIGNALS = {
    "A": ["C1W", "L1C", "C2W", "L2W"],   # the graded + RTKLIB-convention pair
    "B": ["C1C", "L1C", "C2L", "L2L"],   # the DATUM-PINNED pair (selectIfPinPair)
    # The §1.5 MIXED pair, now deliberate: C1C sits in the datum-pinned frame,
    # C2W in the convention frame.  This is the explicit-array replication of
    # the accidental string-defined arm (runs/q5/arm*_L1only) whose position was
    # WORSE than either consistent pair.  Used by the mechanism test [H]: vary
    # the consumer's receiver code_bias_q (--code-bias-q) on this arm to test
    # whether the receiver CB state absorbing a between-channel differential is
    # what turns the intermediate correction-domain fault into the worst
    # position-domain outcome.
    "M": ["C1C", "L1C", "C2W", "L2W"],
}


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--arm", choices=sorted(ARM_SIGNALS), required=True)
    ap.add_argument("--outdir", required=True)
    ap.add_argument("--duration", type=float, default=86400.0,
                    help="processing.max_duration_s (use ~300 for the 10-epoch smoke)")
    ap.add_argument("--products", default=str(PRODUCTS))
    ap.add_argument("--code-bias-q", type=float, default=None,
                    help="override filter.process_noise.code_bias_q (m^2/s) — the "
                         "consumer's RECEIVER code-bias process noise (leg default "
                         "1e-4).  The §1.5 mechanism-[H] knob for arm M.")
    args = ap.parse_args()

    leg = json.loads(LEG.read_text())
    outdir = pathlib.Path(args.outdir).resolve()
    outdir.mkdir(parents=True, exist_ok=True)

    # Two roster entries, purely to satisfy loadNetworkSection_ validation
    # ("need >=2 stations"): the replay branch in runNetwork fires BEFORE
    # networkValidateInputs_/networkSetup_, so no station is ever loaded or solved.
    roster_stub = leg["network"]["stations"][:2]

    cfg = {
        "app_mode": "post",
        "data_source": "internal",
        "processing": {
            # ppp-rtk-network so runNetwork is entered; the replay branch then
            # short-circuits to the consumer driver, which derives a PPP_RTK
            # consumer config via deriveClosedLoopConsumerConfig().
            "mode": "ppp-rtk-network",
            "active_systems": leg["processing"]["active_systems"],
            "frequency_mode": "dual",
            "channel_mode": "single",
            "elevation_mask_deg": leg["processing"]["elevation_mask_deg"],
            "exclude_eclipsing_satellites": leg["processing"]["exclude_eclipsing_satellites"],
            "leap_seconds_tai": leg["processing"]["leap_seconds_tai"],
            "eigen_threads": 4,
            "max_duration_s": args.duration,
            "satellite_interpolation_mode": leg["processing"]["satellite_interpolation_mode"],
            "satellite_clock_interpolation_mode": leg["processing"]["satellite_clock_interpolation_mode"],
            "signal_priorities": {"GPS": ARM_SIGNALS[args.arm]},
        },
        "files": {
            "internal_data_root": leg["files"]["internal_data_root"],
            # Date hint for PathResolver + RinexParser header seeding.
            "observation_path": CONSUMER["rinex_path"],
        },
        # Modeling inherited verbatim from the producer leg so the consumer's
        # geometry/atmosphere/antenna corrections match the stream's producer.
        "model_corrections": dict(leg["model_corrections"]),
        "filter": json.loads(json.dumps(leg["filter"])),
        "estimation": {
            "estimate_velocity": False,
            "estimate_phase_clock": False,
            "estimate_ifb": True,
            "ionosphere_weighting": dict(leg["estimation"]["ionosphere_weighting"]),
        },
        "network": {
            # Roster + datum: load-time validation only (never used).
            "stations": roster_stub,
            "clock_datum_station": leg["network"]["clock_datum_station"],
            "epoch_sync_tolerance_s": leg["network"]["epoch_sync_tolerance_s"],
            # Reproduces the producer's code-bias regime on the consumer config:
            # forces enable_code_phase_bias (.BIA) OFF + estimate_ifb ON, so the
            # network OSB is the SOLE code-bias source and nothing double-corrects.
            "estimate_sat_code_bias": True,
            "estimate_iono_sh": False,
            "closed_loop": {
                "enable": True,
                "station": CONSUMER,
                # FLOAT consumer: a +49 m code-bias fault expresses itself in the
                # float position directly.  AR on a faulted stream would either
                # refuse to fix or fix wrong, and the position would then measure
                # the resolver, not the product.
                "ar_mode": 0,
                "ar_wl_only": True,
                "consumer_undifferenced": False,
                "consume_network_products": True,
                "consume_network_clock": True,
                "consume_network_iono": False,
                "replay_products_path": args.products,
            },
        },
        "data_download": {"enabled": False, "year": 2024, "doy": 129},
        "logging": {
            "enable_main_log": True,
            # Per-(satellite,signal) innovation/residual: the channel that turns
            # "the position moved" into "G01's pseudorange moved".  Without it
            # the arms can only show the position-domain symptom.
            "enable_residual_trace": True,
            "enable_sat_status_trace": True,
            "levels": dict(leg["logging"]["levels"]),
        },
    }

    if args.code_bias_q is not None:
        cfg["filter"]["process_noise"]["code_bias_q"] = args.code_bias_q

    path = outdir / "config.json"
    path.write_text(json.dumps(cfg, indent=2))
    print(f"arm {args.arm}: wrote {path}")
    print(f"  consumer      : {CONSUMER['name']}")
    print(f"  truth ECEF    : {CONSUMER['position_ecef']}  (ITRF2020 @ 2024:129)")
    print(f"  GPS signals   : {ARM_SIGNALS[args.arm]}")
    print(f"  products      : {args.products}")
    print(f"  max_duration_s: {args.duration}")
    print(f"  code_bias_q   : {cfg['filter']['process_noise']['code_bias_q']}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
