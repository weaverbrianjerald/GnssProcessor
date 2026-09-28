#!/usr/bin/env python3
"""Generate 305-station global SH-VTEC PPP-RTK network configs.

Merges the 305-station roster (from config/test_suite/_net305_3sys_smoke.json)
into the validated co-estimated SH-VTEC template (GPS+Galileo, common-mode-free
R, Kaula-regularized SH field).  Parameterized by SH truncation degree and run
duration so a controlled degree sweep can be run on the same 305-station network.

Per project_global_sh_iono.md: degree-4 diverged at 50 stations (a00 -> thousands
of TECU, NaN crash ~epoch 13).  The hypothesis under test: a degree sufficient to
represent the global field (CODE uses 15; validated to <0.5 TECU in the unit
suite) removes the aliasing of unrepresentable small-scale TEC into the low-degree
coefficients, at the CODE-like 305-station density.
"""
import json
import re
import sys
from pathlib import Path

ROOT = Path("C:/GnssProcessor")
SRC_305 = ROOT / "config/test_suite/_net305_3sys_smoke.json"
OUT_DIR = ROOT / "config/test_suite"


def load_jsonc(path: Path):
    """Load JSON tolerating // line comments (not "//"-keys) and trailing commas."""
    lines = []
    for ln in path.read_text(encoding="utf-8", errors="replace").splitlines():
        if ln.lstrip().startswith("//"):
            continue
        lines.append(ln)
    txt = "\n".join(lines)
    txt = re.sub(r",(\s*[}\]])", r"\1", txt)  # trailing commas
    return json.loads(txt)


def build(degree: int, duration_s: float, tag: str, datum: str,
          clk_sigma: float = 1000.0, a00_sigma: float = 30.0,
          kaula_scale: float = 10.0, elev_mask: float = 10.0):
    src = load_jsonc(SRC_305)
    stations = src["network"]["stations"]
    assert len(stations) >= 50, f"expected full roster, got {len(stations)}"

    cfg = {
        "app_mode": "post",
        "data_source": "internal",
        "processing": {
            "mode": "ppp-rtk-network",
            "active_systems": ["GPS", "Galileo"],
            "frequency_mode": "all",
            "channel_mode": "all",
            "elevation_mask_deg": elev_mask,
            "exclude_eclipsing_satellites": True,
            "leap_seconds_tai": 37.0,
            "relinearize_geometry": False,
            "eigen_threads": 20,
            "max_duration_s": duration_s,
            "satellite_interpolation_mode": "lagrange",
            "satellite_clock_interpolation_mode": "linear",
            "signal_priorities": {"GPS": "WCSLXPYMIQ", "Galileo": "CBXIQ"},
        },
        "files": {
            "internal_data_root": "C:/GnssProcessor/data",
            "observation_path": stations[0]["rinex_path"],
        },
        "estimation": {
            "estimate_velocity": False,
            "estimate_phase_clock": False,
            "estimate_ifb": False,
            "estimate_sat_biases": False,
            "no_bia_code_downweight": 1.0,
            "between_satellite_sd": {"enabled": False},
            "ionosphere_weighting": {
                "enable": True,
                "common_mode_sigma_m": 50.0,
                "std_scale": 1.0,
                "floor_m": 0.05,
                "elevation_cutoff_deg": elev_mask,
            },
            "adaptive_iono_process_noise": {"enable": False},
            "global_vtec_sh": {
                "max_degree": degree,
                "layer_height_km": 450.0,
                "earth_radius_km": 6371.0,
                "kaula_scale_tecu": kaula_scale,
                "q_per_degree": 0.01,
                "mean_vtec_prior_tecu": 15.0,
                "mean_vtec_prior_sigma_tecu": a00_sigma,
                "representation_sigma_tecu": 5.0,
                "sun_fixed": True,
                "trace": True,
            },
        },
        "filter": {
            "process_noise": {
                "position_q": 100.0, "clock_bias_q": 1000.0, "phase_clock_q": 1e-09,
                "troposphere_q": 1e-09, "ionosphere_q": 0.001, "code_bias_q": 0.0001,
                "sat_bias_q": 1e-12, "sat_code_bias_q": 1e-10, "sat_phase_bias_q": 1e-12,
                "sat_clock_q": 0.01, "system_time_offset_q": 1e-12, "ambiguity_q": 1e-12,
            },
            "initial_std": {
                "position": 100.0, "clock": 1000.0, "troposphere": 0.5, "ionosphere": 5.0,
                "ambiguity": 1000.0, "code_bias": 10.0, "sat_bias": 10.0,
                "sat_phase_bias": 10.0, "sat_clock": clk_sigma, "system_time_offset": 1000.0,
            },
            "measurement_noise": {"code_std_m": 0.3, "phase_std_m": 0.002},
            "stochastic_model": "elevation",
        },
        "model_corrections": {
            "enable_troposphere": True, "enable_relativistic_correction": True,
            "enable_shapiro_delay": True, "enable_solid_earth_tide": True,
            "enable_ocean_tide_loading": False, "enable_pole_tide": True,
            "enable_atmospheric_loading": False, "enable_satellite_pco": True,
            "enable_satellite_pcv": True, "enable_receiver_pco": True,
            "enable_receiver_pcv": True, "enable_phase_windup": True,
            "enable_higher_order_ionosphere": False, "enable_code_phase_bias": True,
        },
        "network": {
            "clock_datum_station": datum,
            "epoch_sync_tolerance_s": 1.0,
            "parallel_stations": True,
            "parallel_min_stations": 8,
            "estimate_iono_sh": True,
            "trace_ionosphere": True,
            "product_output_path": "",
            "stations": stations,
        },
        "data_download": {
            "enabled": False, "product_source": "COD", "product_type": "FIN",
            "year": 2024, "doy": 129,
        },
        "logging": {
            "enable_main_log": True,
            "levels": {
                "measurement_model": "warn", "state_manager": "warn",
                "state_initializer": "warn", "ppp_model": "warn",
                "kalman_filter": "warn", "ambiguity_resolver": "warn",
            },
        },
    }

    # Ensure the datum station is actually in the roster.
    names = {s["name"] for s in stations}
    assert datum in names, f"datum {datum} not in roster ({len(names)} stations)"

    out = OUT_DIR / f"_net305_gpsgal_sh_n{degree}_{tag}.json"
    out.write_text(json.dumps(cfg, indent=2), encoding="utf-8")
    print(f"wrote {out}  ({len(stations)} stations, degree {degree}, "
          f"{duration_s:.0f}s, datum {datum})")
    return out


if __name__ == "__main__":
    datum = "MBAR00UGA"
    # Baseline (no conditioning fix): loose clock std 1000 m, a00 prior 30 TECU.
    for deg in (4, 12, 15):
        build(deg, 900.0, "smoke", datum)

    # Conditioning fix: bind the sat-clock Δ a-priori anchor (σ = 0.3 m, sub-m
    # precise-product quality) + physical a00 climatology bound (15 ± 15 TECU).
    for deg in (4, 15):
        build(deg, 900.0, "fix_smoke", datum, clk_sigma=0.3, a00_sigma=15.0)
    build(15, 3600.0, "fix_1h", datum, clk_sigma=0.3, a00_sigma=15.0)

    # Tighter sat-clock anchor (sigma = 0.05 m): tests whether the residual clock
    # spread is irreducible datum offset (uniform) or still-tameable per-sat scatter.
    build(15, 900.0, "fix2_smoke", datum, clk_sigma=0.05, a00_sigma=15.0)
    # Tight clock anchor plus tighter physical a00 bound (15 ± 5 TECU): defense in depth.
    build(15, 900.0, "fix3_smoke", datum, clk_sigma=0.05, a00_sigma=5.0)
    # Strongly prior-dominated field (a00 15±3, Kaula scale 2 → σ_n=2/n) + tight clock.
    # Diagnostic: is the field controllable by its prior at all, or does corrupted
    # cold-start data overwhelm it regardless?
    build(15, 900.0, "fix4_smoke", datum, clk_sigma=0.05, a00_sigma=3.0, kaula_scale=2.0)

    # Elevation-mask 25° (vs 10°): removes the worst-mapping / most slip- and
    # multipath-prone low-elevation obs that dominate the a00 mapping-spread handle.
    # em25_smoke isolates the elevation effect at cold start (no clock anchor);
    # em25_long is the convergence test, 3 h so the slant-iono / GF-ambiguity can
    # begin to separate, with the kept clock anchor (σ=0.3) and physical a00 bound.
    build(15, 900.0,   "em25_smoke", datum, elev_mask=25.0)
    # Pure elevation-25 convergence test (no clock anchor / default priors): does the
    # already-physical cold-start field stay clean and refine the high degrees over 3 h?
    build(15, 10800.0, "em25_long",  datum, elev_mask=25.0)
