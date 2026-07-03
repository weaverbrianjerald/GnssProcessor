#!/usr/bin/env python3
"""Generate the PPP-RTK network+user constellation test-suite configs.

Each combo is ONE ``ppp-rtk-network`` producer over a 50-station global subnet
(``config/test_suite/global_network_stations.json``) with the closed-loop,
held-out PPP-RTK USER consumer (CIT100USA) enabled.  The consumer applies the
network's OWN freshly-emitted products (``consume_network_products`` +
``consume_network_clock``), so one run exercises both the network producer and
the user consumer.  Reporting focuses on the converged final hour.

Bias policy ("all receiver and satellite biases enabled", best available):
  * non-GLONASS combos -> ``network.estimate_sat_code_bias=true``: satellite
    clock + phase bias + OSB code bias, receiver DCB (estimate_ifb), and GIM
    iono weighting (all forced on together; requires a GIM, present in the
    bundled 2024-129 products).
  * GLONASS combos -> ``network.estimate_glonass=true``: satellite clock +
    phase bias (GPS/Gal/BDS) plus the GLONASS per-FCN-slot receiver IFB.  No
    satellite code OSB -- ``estimate_sat_code_bias`` and ``estimate_glonass``
    are mutually exclusive in V1 (loader hard-error otherwise).

All-frequency / all-channel.  Iono trace OFF (network.trace_ionosphere=false),
per-station compute parallelism ON.  Network stays float (no network AR).
"""
import argparse
import json
import os
import sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# (slug, active_systems) -- the constellation combos requested for the suite.
COMBOS = [
    ("gps", ["GPS"]),
    ("gps_gal", ["GPS", "Galileo"]),
    ("gps_glo", ["GPS", "GLONASS"]),
    ("gps_bds", ["GPS", "BeiDou"]),
    ("gps_gal_glo", ["GPS", "Galileo", "GLONASS"]),
    ("gps_gal_bds", ["GPS", "Galileo", "BeiDou"]),
    ("gps_gal_glo_bds", ["GPS", "Galileo", "GLONASS", "BeiDou"]),
]

# Per-constellation tracking-attribute priority strings (copied from the
# existing _net305_4sys configs).
SIGNAL_PRIORITIES = {
    "GPS": "WCSLXPYMIQ",
    "Galileo": "CBXIQ",
    "BeiDou": "IDPXQZ",
    "GLONASS": "PCX",
}

DATUM = "JPLM00USA"

# Held-out PPP-RTK user consumer (Caltech CIT1, ~8.7 km from the JPLM datum,
# NOT in the estimator station list).  position_ecef is the RINEX-header survey
# truth used for a-priori seeding + position scoring.
CONSUMER = {
    "name": "CIT100USA",
    "rinex_path": "{data}/2024-129/obs/cit1/cit11290.24o",
    "position_ecef": [-2491490.7172, -4660802.8495, 3559129.1872],
}


def build_stations(stations_json, data_root):
    out = []
    for s in stations_json:
        longn = s["long"]
        out.append({
            "name": longn,
            "rinex_path": f"{data_root}/2024-129/obs/{longn.lower()}/"
                          f"{longn}_R_20241290000_01D_30S_MO.rnx",
            "position_ecef": s["ecef"],
        })
    return out


def build_config(slug, systems, stations, duration_s, data_root):
    has_glo = "GLONASS" in systems
    sig = {s: SIGNAL_PRIORITIES[s] for s in systems}

    cfg = {
        "app_mode": "post",
        "data_source": "internal",
        "processing": {
            "mode": "ppp-rtk-network",
            "active_systems": list(systems),
            "frequency_mode": "all",
            "channel_mode": "all",
            "elevation_mask_deg": 10.0,
            "exclude_eclipsing_satellites": True,
            "leap_seconds_tai": 37.0,
            "relinearize_geometry": False,
            "eigen_threads": 10,
            "max_duration_s": float(duration_s),
            "satellite_interpolation_mode": "lagrange",
            "satellite_clock_interpolation_mode": "linear",
            "signal_priorities": sig,
        },
        "files": {
            "internal_data_root": "./data",
            "observation_path": f"{data_root}/2024-129/obs/jplm00usa/"
                                f"JPLM00USA_R_20241290000_01D_30S_MO.rnx",
        },
        "estimation": {
            "estimate_velocity": False,
            "estimate_phase_clock": False,
            "estimate_ifb": False,
            "estimate_sat_biases": False,
            # Do NOT downweight code on no-.BIA signals; the network needs code
            # to anchor the clock vs phase-bias split.
            "no_bia_code_downweight": 1.0,
            "between_satellite_sd": {"enabled": False},
            "ambiguity_resolution": {
                "mode": 0,
                "wl_only": True,
                "active_systems": ["GPS"],
                "ratio_threshold": 3.0,
                "ratio_upper_bound": 1e4,
                "min_success_rate": 0.999,
                "nl_max_per_pair_frac": 0.15,
                "elevation_mask": 25.0,
                "min_epochs": 1,
                "max_position_sigma_m": 10.0,
                "hold_variance": 0.001,
                "wl_max_frac": 0.40,
            },
            "adaptive_iono_process_noise": {"enable": False},
        },
        "filter": {
            "process_noise": {
                "position_q": 1e2, "clock_bias_q": 1e3, "phase_clock_q": 1e-9,
                "troposphere_q": 1e-9, "ionosphere_q": 1e-3, "code_bias_q": 1e-4,
                "sat_bias_q": 1e-12, "sat_phase_bias_q": 1e-12, "sat_clock_q": 1e-2,
                "system_time_offset_q": 1e-12, "ambiguity_q": 1e-12,
            },
            "initial_std": {
                "position": 100.0, "clock": 1000.0, "troposphere": 0.5,
                "ionosphere": 5.0, "ambiguity": 1000.0, "code_bias": 10.0,
                "sat_bias": 10.0, "sat_phase_bias": 10.0, "sat_clock": 1000.0,
                "system_time_offset": 1000.0,
            },
            "measurement_noise": {"code_std_m": 0.300, "phase_std_m": 0.002},
            "stochastic_model": "elevation",
        },
        "model_corrections": {
            "enable_troposphere": True,
            "enable_relativistic_correction": True,
            "enable_shapiro_delay": True,
            "enable_solid_earth_tide": True,
            "enable_ocean_tide_loading": False,
            "enable_pole_tide": True,
            "enable_atmospheric_loading": False,
            "enable_satellite_pco": True,
            "enable_satellite_pcv": True,
            "enable_receiver_pco": True,
            "enable_receiver_pcv": True,
            "enable_phase_windup": True,
            "enable_higher_order_ionosphere": False,
            # network may override (W6 turns .BIA off when estimating its own OSB)
            "enable_code_phase_bias": True,
        },
        "network": {
            "clock_datum_station": DATUM,
            "epoch_sync_tolerance_s": 1.0,
            "trace_stations": True,
            "trace_ionosphere": False,            # skip iono trace (speed)
            "parallel_stations": True,
            "parallel_min_stations": 10,
            "product_output_path": f"network_products_{slug}.txt",
            "stations": stations,
            "closed_loop": {
                "enable": True,
                "consumer_undifferenced": True,
                "consume_network_products": True,
                "consume_network_clock": True,     # FULL: user applies OUR network clock+bias
                "ar_mode": 1,
                "ar_wl_only": False,
                "ar_active_systems": ["GPS"],
                "station": {
                    "name": CONSUMER["name"],
                    "rinex_path": CONSUMER["rinex_path"].format(data=data_root),
                    "position_ecef": CONSUMER["position_ecef"],
                },
            },
        },
        "data_download": {
            "enabled": False,
            "product_source": "COD",
            "product_type": "FIN",
            "year": 2024,
            "doy": 129,
        },
        "logging": {
            "enable_main_log": True,
            "enable_ambiguity_resolver": True,
            # CSV traces below are written by the held-out CONSUMER (CIT1).
            "enable_state_trace": True,
            "enable_ambiguity_trace": True,
            "enable_residual_trace": True,
            "enable_dop_trace": True,
            "enable_sat_status_trace": True,
            "levels": {
                "measurement_model": "warn",
                "state_manager": "warn",
                "state_initializer": "warn",
                "ppp_model": "warn",
                "kalman_filter": "warn",
                "ambiguity_resolver": "info",
            },
        },
    }

    # --- bias policy (mutually exclusive switches) ---
    has_bds = "BeiDou" in systems
    if has_glo:
        # GLONASS present: code-only GLONASS (per-FCN-slot receiver IFB) plus the
        # satellite clock + phase bias.  estimate_sat_code_bias is mutually
        # exclusive with estimate_glonass in V1 (loader hard-error).
        cfg["network"]["estimate_glonass"] = True
        cfg["network"]["estimate_sat_code_bias"] = False
    elif has_bds:
        # W6 satellite OSB is GPS+Galileo-only: adding BeiDou under W6 leaves the
        # BeiDou satellite-clock gauge unconstrained and the observability gate
        # withholds EVERY epoch (validated in smoke).  Fall back to V1 -- satellite
        # clock + phase bias with the external .BIA for code bias (no network OSB).
        cfg["network"]["estimate_sat_code_bias"] = False
        cfg["network"]["estimate_glonass"] = False
    else:
        # GPS and/or Galileo only: full network satellite OSB + receiver DCB.
        cfg["network"]["estimate_sat_code_bias"] = True
        cfg["network"]["estimate_glonass"] = False

    return cfg


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--outdir", required=True, help="directory to write combo configs into")
    ap.add_argument("--duration", type=float, default=10800.0,
                    help="processing.max_duration_s seconds (default 10800 = 3h)")
    ap.add_argument("--data-root", default="C:/GnssProcessor/data",
                    help="absolute data root for RINEX/products (forward slashes)")
    ap.add_argument("--stations-json",
                    default=os.path.join(REPO, "config", "test_suite",
                                         "global_network_stations.json"))
    ap.add_argument("--prefix", default="_ppprtk50",
                    help="filename prefix (default _ppprtk50)")
    args = ap.parse_args()

    data_root = args.data_root.replace("\\", "/").rstrip("/")
    with open(args.stations_json, encoding="utf-8") as f:
        stations_json = json.load(f)
    stations = build_stations(stations_json, data_root)

    os.makedirs(args.outdir, exist_ok=True)
    written = []
    for i, (slug, systems) in enumerate(COMBOS, start=1):
        cfg = build_config(slug, systems, stations, args.duration, data_root)
        fname = f"{args.prefix}_{i}_{slug}.json"
        path = os.path.join(args.outdir, fname)
        with open(path, "w", encoding="utf-8") as f:
            json.dump(cfg, f, indent=2)
        written.append(path)
        if "GLONASS" in systems:
            bias = "GLO_IFB (no sat OSB)"
        elif "BeiDou" in systems:
            bias = "V1 .BIA code (no sat OSB; W6=GPS+Gal-only)"
        else:
            bias = "sat OSB + rx DCB (W6)"
        print(f"  [{i}] {fname:32s} {'+'.join(systems):28s} biases={bias}")

    print(f"\nWrote {len(written)} configs to {args.outdir} "
          f"({len(stations)} stations, max_duration_s={args.duration:.0f})")


if __name__ == "__main__":
    main()
