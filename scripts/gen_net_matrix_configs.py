#!/usr/bin/env python3
"""Generate the network PPP-RTK test matrix configs (50/305 stations x 10ep/10min/1h)
with the full "all new features" toggle set layered onto each roster's validated tuning.

Bases (per-roster validated roster + filter/mask/degree/datum) are the current
working-tree configs:
  net50  <- config/test_suite/_net50_em25_pl_zeroq_slip_1h.json  (50 sta, deg 4,  datum JPLM00USA)
  net305 <- config/test_suite/_net305_gpsgal_sh_n15_1h.json      (305 sta, deg 15, datum MBAR00UGA)

We KEEP each base's roster, filter q's, elevation mask (25 deg — the validated
healthy set; 10 deg destroys the zero-q carry at 305, see runs/net_matrix net305_10min
vs net305_30min_em25), SH degree, datum
and only (a) set processing.max_duration_s per duration cell and (b) overwrite the
network{} feature toggles + estimation.global_vtec_sh.trace with the canonical
all-features set below.  Output -> runs/net_matrix_2024-129/configs/.
"""
import json, copy, os

REPO = r"C:/GnssProcessor"
TS   = os.path.join(REPO, "config", "test_suite")
OUT  = os.path.join(REPO, "runs", "net_matrix_2024-129", "configs")
os.makedirs(OUT, exist_ok=True)

BASES = {
    50:  os.path.join(TS, "_net50_em25_pl_zeroq_slip_1h.json"),
    305: os.path.join(TS, "_net305_gpsgal_sh_n15_1h.json"),
}
DURATIONS = {"10ep": 285.0, "10min": 600.0, "1h": 3600.0}

# --- Canonical "all new features" network block (overwrites base network toggles) -----
# Estimation/QC/datum features (touch the shared normal equations) + all viz trace
# toggles + product-file emission.  GLONASS is intentionally absent: estimate_iono_sh
# forces sat-code-bias on, which is mutually exclusive with network.estimate_glonass.
FEATURES = {
    # --- iono + satellite code bias (non-negotiable; forces W6 OSB code-bias slice) ---
    "estimate_iono_sh": True,

    # --- per-station compute parallelism ---
    "parallel_stations": True,
    "parallel_min_stations": 8,

    # --- phase-leveling zero-q carry + slip DETECTION (robust: mw_detect + if_phase_td) ---
    "phase_leveling": True,
    "phase_leveling_q_amb": 0.0,
    "phase_leveling_warmup_epochs": 6,
    "phase_leveling_max_gap_s": 120.0,
    "cycle_slip": {
        "mw_detect": True,
        "if_phase_td": True,
        "if_phase_td_k_mad": 8.0,
        "if_phase_td_floor_frac": 1.0,
        "if_phase_td_min_pool": 30,
    },
    # --- tropospheric ZWD random-walk carry ---
    "carry_zwd": True,
    "zwd_carry_q": 1e-9,

    # --- QC: post-fit RAIM + within-epoch surgical RAIM + attribution refloat ---
    "raim": True,
    "raim_attribution": True,
    "raim_within_epoch": True,
    "raim_surgical_station": True,
    "raim_k_sigma": 6.0,

    # --- QC: carried-clock sanity monitor + two-state (Allan) oscillator model + runaway ---
    "clock_monitor": True,
    "clock_monitor_couple_raim": True,
    "clock_model": True,
    "clock_runaway_check": True,

    # --- Datum (newest): anchor-free sum-to-zero clock gauge + prioritized phase-bias anchor ---
    "clock_datum_sum_to_zero": True,
    "phase_datum_prioritized": True,
    "phase_datum_sticky": True,

    # --- URA quality channel + discontinuity signalling + S-basis contract ---
    "emit_ura": True,
    "ura_retro_bump": True,
    "emit_clock_disc": True,
    "serialize_sbasis_contract": True,   # inert since card OWD (the S line is on every wire); kept, so the output is unchanged
    "emit_generation_id": True,
    "serialize_disc_counters": True,
    "emit_epoch_status_record": True,

    # --- product file (feeds network_product_file + network_discontinuities Half-A viewers) ---
    "product_output_path": "network_products.txt",

    # --- operator heartbeat + run provenance manifest ---
    "health_interval_s": 30.0,
    "write_run_manifest": True,

    # --- ALL visualization trace toggles (feed the full pyviz network viewer set) ---
    "trace_stations": True,
    "trace_ionosphere": True,
    "trace_estimability": True,
    "trace_slip_events": True,
    "trace_slip_raw": True,
    "trace_clock_full": True,
    "trace_anchor_info": True,
    "trace_dd_fractional": True,
    "trace_flag_rates": True,
    "trace_epoch_status": True,
    "trace_events": True,
    "trace_zwd": True,
    "trace_station_residuals": True,
}

# Keys that must NOT be present alongside sum-to-zero clock datum (they'd warn as inert).
DROP_KEYS = ["clock_datum_satellite", "sticky_pivot", "pivot_vanish_return_signal"]

def build(nsta, tag, dur):
    with open(BASES[nsta]) as f:
        cfg = json.load(f)
    cfg["processing"]["max_duration_s"] = dur
    # Ensure SH-VTEC coefficient trace is on (network_vtec_sh.csv).
    cfg.setdefault("estimation", {}).setdefault("global_vtec_sh", {})["trace"] = True
    nw = cfg["network"]
    for k in DROP_KEYS:
        nw.pop(k, None)
    # Overwrite / add all feature toggles.  cycle_slip is replaced wholesale so a
    # base's turbo_edit block doesn't shadow mw_detect.
    for k, v in FEATURES.items():
        nw[k] = copy.deepcopy(v)
    out = os.path.join(OUT, f"net{nsta}_allfeat_{tag}.json")
    with open(out, "w") as f:
        json.dump(cfg, f, indent=2)
    return out, len(nw["stations"]), cfg["processing"].get("elevation_mask_deg"), \
           cfg["estimation"]["global_vtec_sh"].get("max_degree"), nw.get("clock_datum_station")

print(f"{'config':<44} {'nsta':>4} {'elev':>4} {'deg':>3} {'datum':>10} {'dur_s':>7}")
for nsta in (50, 305):
    for tag, dur in DURATIONS.items():
        out, n, elev, deg, datum = build(nsta, tag, dur)
        print(f"{os.path.basename(out):<44} {n:>4} {elev:>4} {deg:>3} {datum:>10} {dur:>7.0f}")
print("\nWrote to", OUT)
