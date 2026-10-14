#!/usr/bin/env python3
"""Build the external-product integer-AR probe arms (DOY-129, 50-sta GPS-only tri-freq).

Three arms, identical in every respect except how the satellite code bias and the
ionosphere are treated.  The point is to isolate what an external product can and
cannot buy for the network's DD ambiguity integer character.

    S  self      network.estimate_iono_sh = true
                 -> own SH-VTEC deg-8 iono + own satellite OSB; external .BIA is
                    force-disabled by the loader; no GIM is loaded.
                    (= the validated Jul-17 _wlver_50sta lineage.)

    G  ext-GIM   estimate_iono_sh = false, estimate_sat_code_bias = true
                 -> the COD .INX GIM becomes the load-bearing iono constraint that
                    makes the satellite DCB observable; the network still estimates
                    its own OSB; external .BIA still force-disabled.

    B  ext-OSB   estimate_iono_sh = false, estimate_sat_code_bias = false,
                 model_corrections.enable_code_phase_bias = true
                 -> COD OSB (.BIA) is applied to the code observations, no satellite
                    code-bias states, no iono pseudo-observation at all: free
                    per-(rx,sat) slant iono constrained only by the (now unbiased)
                    geometry-free code.  The per-LOS iono model, bought with an
                    external bias product.

Why all three also get network.phase_leveling
----------------------------------------------
Without it the network producer is a per-epoch absolute-snapshot solve
(GnssConfig.hpp:1568-1576: "Default false keeps the per-epoch absolute snapshot
solve byte-identical").  A single-epoch float ambiguity carries code-level
precision, i.e. of order a cycle or worse, so its DD fraction is uniform by
construction, and no product change could ever show up.  The Jul-17 base config
had no carry, so its DD-frac readout could only ever have read uniform.
phase_leveling auto-arms network.cycle_slip.mw_detect (GnssConfigLoadNetwork.cpp
:1795-1802), satisfying standing invariant #1 (zero-q needs a slip response).

Usage:  python scripts/extprod_ar_make_configs.py [--epochs 30]
"""
import argparse
import copy
import json
import os

BASE = os.path.join(os.path.dirname(__file__), "..", "config", "test_suite",
                    "_wlver_50sta_10ep_d10_gps_tri_sh8.json")
OUTDIR = os.path.join(os.path.dirname(__file__), "..", "config", "test_suite")

# ApplicationNetwork breaks on `elapsed >= max_duration` anchored at the first
# epoch, first exempt -> n_ep = (max_duration_s / 30) + 1.  300 s measured 11 ep.
def duration_for(epochs):
    return float((epochs - 1) * 30)


# --- the Jul-28 standard armed set -------------------------------------------
# The base config predates the directive (Jul-17), so arm it explicitly here.
# Everything below is fingerprint-safe or fresh-convergence-only; every arm is a
# cold start, so the fingerprinted members are legal.
ARMED_QC = {
    # QC, fingerprint-safe
    "raim": True,
    "raim_attribution": True,
    "raim_within_epoch": True,
    "raim_withhold_sigma0": 6.0,
    "clock_runaway_check": True,
    # fingerprinted - fresh convergence only (all arms are cold starts)
    "phase_leveling": True,
    "phase_leveling_max_gap_s": 900.0,
    "emit_ura": True,
    # cold-start hygiene (cold_clock_seed already true in the base)
    "cold_clock_seed": True,
    # observability
    "write_run_manifest": True,
    "trace_epoch_status": True,
    "trace_events": True,
    "trace_flag_rates": True,
    "health_interval_s": 300,
    "emit_epoch_status_record": True,
    "trace_postfit_sigma0": True,
    "trace_postfit_keyed": True,
    # parallel
    "parallel_carry": True,
}

ARMS = {
    # tag: (network overrides, model_corrections overrides, note)
    "s_self": (
        {"estimate_iono_sh": True, "estimate_sat_code_bias": True,
         "sh_health_gate": True},
        {"enable_code_phase_bias": False},
        "self-contained: own SH deg-8 iono + own satellite OSB (control)",
    ),
    "g_extgim": (
        {"estimate_iono_sh": False, "estimate_sat_code_bias": True,
         "sh_health_gate": False},
        {"enable_code_phase_bias": False},
        "external COD GIM as the load-bearing iono constraint; own satellite OSB",
    ),
    "b_extosb": (
        {"estimate_iono_sh": False, "estimate_sat_code_bias": False,
         "sh_health_gate": False},
        {"enable_code_phase_bias": True},
        "external COD OSB applied; free per-LOS slant iono, no iono pseudo-obs",
    ),
}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--epochs", type=int, default=30)
    ap.add_argument("--tag", default="")
    args = ap.parse_args()

    with open(BASE, "r", encoding="utf-8") as fh:
        base = json.load(fh)

    dur = duration_for(args.epochs)
    suffix = args.tag or f"{args.epochs}ep"
    written = []

    for tag, (net_over, mc_over, note) in ARMS.items():
        cfg = copy.deepcopy(base)
        cfg["processing"]["max_duration_s"] = dur
        cfg["network"]["max_duration_s"] = dur
        cfg["network"].update(ARMED_QC)
        cfg["network"].update(net_over)
        cfg["model_corrections"].update(mc_over)
        cfg["_note"] = note

        # The SH block only means anything in arm S; leave it in place (harmless,
        # and keeps the three files diffable line-for-line).
        name = f"_extar_50sta_{suffix}_gps_tri_{tag}.json"
        path = os.path.normpath(os.path.join(OUTDIR, name))
        with open(path, "w", encoding="utf-8") as fh:
            json.dump(cfg, fh, indent=2)
        written.append((name, note))

    n_sta = len(base["network"]["stations"])
    print(f"{len(written)} arms, {args.epochs} ep (max_duration_s={dur:.0f}), "
          f"{n_sta} stations")
    for name, note in written:
        print(f"  {name}\n      {note}")


if __name__ == "__main__":
    main()
