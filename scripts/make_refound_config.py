#!/usr/bin/env python3
"""Generate the two RF-0 base configs from the two goldens.

ONE posture, two beds (docs/refound/NETWORK_REFOUNDING_2026-09-14.md).  The posture
dict is built ONCE and both configs are posture + a bed overlay, so the POSTURE half
of the classified diff is empty BY CONSTRUCTION.
"""
import json, os, sys, copy

HERE = os.path.dirname(os.path.abspath(__file__))
WT = os.path.dirname(HERE)          # the repo root this script lives in
sys.path.insert(0, HERE)
from gnss_config_chain import load_config

FIELD_G = os.path.join(WT, "config/golden/golden_field_gps_dual.json")
SIM_G = os.path.join(WT, "config/golden/golden_sim_tri_w6.json")
OUT = os.path.join(WT, "config/refound")


# --------------------------------------------------------------------------------
# helpers
# --------------------------------------------------------------------------------
def flat(d, pre=""):
    out = {}
    for k, v in d.items():
        if k.startswith("_") or k.startswith("//") or "//" in k:
            continue
        key = pre + k
        if isinstance(v, dict):
            out.update(flat(v, key + "."))
        else:
            out[key] = v
    return out


def unflat(f):
    out = {}
    for k, v in f.items():
        parts = k.split(".")
        d = out
        for p in parts[:-1]:
            d = d.setdefault(p, {})
        d[parts[-1]] = v
    return out


field, _ = load_config(FIELD_G)
sim, _ = load_config(SIM_G)
ff, fs = flat(field), flat(sim)

# --------------------------------------------------------------------------------
# 1. the shared POSTURE: union of the two goldens, FIELD wins on conflict
#    ("Freeze the posture at your model on the field"), then the RF-0 deltas.
# --------------------------------------------------------------------------------
BED_KEYS = {
    "network.stations",
    "network.clock_datum_station",
    "network.closed_loop.station.name",
    "network.closed_loop.station.position_ecef",
    "network.closed_loop.station.rinex_path",
    "network.product_output_path",
    "files.observation_path",
    "filter.measurement_noise.phase_std_m",
    "estimation.accumulate_phase_windup",
}

posture = {}
for k, v in fs.items():          # sim first
    if k not in BED_KEYS:
        posture[k] = v
for k, v in ff.items():          # field wins
    if k not in BED_KEYS:
        posture[k] = v

# ---- DROP: keys that must not appear at all -------------------------------------
DROP = {
    # --- the decision's scope cut ------------------------------------------------
    "network.code_bias_gf_datum":            "HARD ERROR under frequency_mode dual (GnssConfigLoadNetwork.cpp:3011-3021)",
    "network.phase_leveling_local_block":    "F3 off (decision)",
    "processing.signal_priorities.Galileo":  "GPS only",
    # --- lanes off (the vehicle turns the four enables off explicitly; the rest of
    #     the mw_wl block is then inert noise) --------------------------------------
    "network.mw_wl.bridge_sample_debias":    "lanes off",
    "network.mw_wl.d5_dump":                 "lanes off",
    "network.mw_wl.lane_trace":              "lanes off",
    "network.mw_wl.uwl_dual_top":            "lanes off",
    "network.mw_wl.uwl_family_pairs":        "lanes off",
    "network.mw_wl.uwl_mid_enable":          "lanes off",
    # --- seams / snapshots / warm resume off --------------------------------------
    "network.state_snapshot_path":           "snapshots off",
    "network.state_snapshot_carry":          "snapshots off",
    "network.state_snapshot_carry_lin":      "snapshots off",
    "network.state_snapshot_interval_s":     "snapshots off",
    "network.state_snapshot_keep_history":   "snapshots off",
    "network.state_snapshot_datum_capture":  "snapshots off",
    "network.state_snapshot_emit_capture":   "snapshots off",
    "network.state_snapshot_rxcb_lin_capture": "snapshots off",
    "network.state_snapshot_signal_capture": "snapshots off",
    "network.state_snapshot_station_capture": "snapshots off",
    "network.serialize_disc_counters":       "snapshot serialization",
    "network.serialize_sbasis_contract":     "snapshot serialization",
    # --- inert under this posture (would only produce loader noise) ---------------
    "network.raim_carry_invariant":          "loader DISABLES it with carry_invariant off",
    "network.sh_health_gate":                "SH off",
    "network.cycle_slip.datum_station_detect": "runtime-inert under the prioritized (no-pivot) phase datum",
    "network.closed_loop.ar_wl_only":        "no lanes",
    # --- the SH parameter block: inert with estimate_iono_sh false ----------------
    "estimation.global_vtec_sh.earth_radius_km": "SH off",
    "estimation.global_vtec_sh.kaula_scale_tecu": "SH off",
    "estimation.global_vtec_sh.layer2_height_km": "SH off",
    "estimation.global_vtec_sh.layer2_max_degree": "SH off",
    "estimation.global_vtec_sh.layer_height_km": "SH off",
    "estimation.global_vtec_sh.max_degree": "SH off",
    "estimation.global_vtec_sh.mean_vtec_prior_sigma_tecu": "SH off",
    "estimation.global_vtec_sh.mean_vtec_prior_tecu": "SH off",
    "estimation.global_vtec_sh.q_per_degree": "SH off",
    "estimation.global_vtec_sh.rep_error_d5_keep": "SH off",
    "estimation.global_vtec_sh.rep_error_demean": "SH off",
    "estimation.global_vtec_sh.rep_error_tau_s": "SH off",
    "estimation.global_vtec_sh.representation_sigma_tecu": "SH off",
    "estimation.global_vtec_sh.sun_fixed": "SH off",
    "estimation.global_vtec_sh.trace": "SH off",
    # --- RT / live-tail scoped (this is post-processing) ---------------------------
    "network.product_flush_per_epoch":       "ledger scope: live-tail / RT-consumer only",
    "network.emit_generation_id":            "ledger scope: live / restartable service",
    "network.emit_combined_sigma":           "not on the frozen field model",
    "network.emit_phase_bias_sigma":         "not on the frozen field model",
}
for k in DROP:
    posture.pop(k, None)

# ---- the loader's own expansion of the field golden's GPS attribute-priority
#      string "WCSLXPYMIQ", restricted to bands 1 and 2.  GnssConfigLoad.cpp:795-828:
#      the STRING form expands over system_bands["GPS"] = {'1','2','5'} x {C,L,D,S} x
#      attrs and so ALWAYS registers band 5; only the ARRAY form can exclude a band.
ATTRS = "WCSLXPYMIQ"
gps_prio = [f"{ot}{band}{a}" for band in "12" for ot in "CLDS" for a in ATTRS]

POSTURE_SET = {
    # ---- scope cut -------------------------------------------------------------
    "processing.active_systems": ["GPS"],
    "processing.frequency_mode": "dual",
    "processing.channel_mode": "single",
    "processing.signal_priorities.GPS": gps_prio,
    "processing.elevation_mask_deg": 15.0,
    "processing.eigen_threads": 20,
    "processing.rinex_skip_zero_values": True,
    "estimation.ambiguity_resolution.mode": 0,
    "network.lane_carry.enable": False,
    "network.closed_loop.ar_mode": 0,
    # ---- the leveling vehicle (SPEC_event_harness s24.11; literals from
    #      config/test_suite/_pv_b.json) -------------------------------------------
    "network.carry_invariant": False,
    "network.carry_invariant_honesty": False,
    "network.carry_invariant_innov_calibrate": False,
    "network.carry_invariant_innov_corroborate": False,
    "network.carry_invariant_innov_gate": False,
    "network.carry_invariant_pd_safe": False,
    "network.mw_wl.ewl_enable": False,
    "network.mw_wl.uwl_enable": False,
    "network.mw_wl.uwl_cold_fix": False,
    "network.mw_wl.wl_inject_enable": False,
    "network.cycle_slip.relevel_free_epoch": True,
    "network.carry_coupling_gauge_project": True,
    "network.phase_datum_eq34": False,
    "network.phase_datum_prioritized": True,
    "network.phase_datum_handover_continuous": True,
    "network.phase_anchor_carry_reref": True,
    "network.phase_datum_min_stations": 2,
    # ---- the physical clock time-link (s24.16 Q3; literals from runs/_cm_c8b/arm.json,
    #      clock_anchor_q at the physical floor 2.9e-8) ---------------------------
    "network.clock_temporal_anchor": True,
    "network.clock_anchor_q": 2.9e-08,
    "network.clock_anchor_abs_m": 2.0,
    "network.clock_anchor_gauge_free": False,
    "network.clock_anchor_gauge_free_stat": "proj",
    "network.clock_anchor_k": 6.0,
    "network.clock_anchor_mark": True,
    "network.clock_anchor_max_gap_s": 120.0,
    "network.clock_anchor_recouple_epochs": 4,
    "network.clock_anchor_resolve": True,
    "network.clock_anchor_sigma_floor_m": 0.0018,
    "network.clock_anchor_var_floor_m2": 1e-05,
    # ---- the wire contract ------------------------------------------------------
    "network.clock_disc_jump_test": True,
    # ---- free slant iono: SH dropped from the producer --------------------------
    "network.estimate_iono_sh": False,
    "network.estimate_sat_code_bias": True,
    # LOAD-BEARING UNDER DUAL — see "HOW DUAL IS MADE SOLVABLE" above.  Without it
    # the null-space gate observes 13 against a declared 1 and withholds every epoch.
    "network.rx_code_bias_datum": True,
    "estimation.ionosphere_weighting.enable": True,
    "estimation.ionosphere_weighting.elevation_cutoff_deg": 25.0,
    # ---- run length / logging ---------------------------------------------------
    "processing.max_duration_s": 21600.0,
    "network.max_duration_s": 21600.0,
    "network.epoch_sync_tolerance_s": 1.0,
    "network.health_interval_s": 300.0,
    "network.min_stations": 10,
    "logging.levels.network": "info",
    # ---- diagnostics the RF-0 grader reads (all trace-class) --------------------
    "network.trace_zwd": True,
    "network.trace_postfit_rows": True,
    "network.trace_stations": True,
}
posture.update(POSTURE_SET)

# --------------------------------------------------------------------------------
# 2. the bed overlays
# --------------------------------------------------------------------------------
def bed(src, run_dir):
    o = {k: src[k] for k in BED_KEYS if k in src}
    o["network.product_output_path"] = run_dir + "/products.txt"
    return o


COMMENT_COMMON = [
    "RF-0 BASE CONFIG — the FROZEN POSTURE of the network re-founding",
    "(docs/refound/NETWORK_REFOUNDING_2026-09-14.md).  GENERATED, not hand-written:",
    "the posture dict is built ONCE from the two goldens and both beds get posture +",
    "a bed overlay, so the POSTURE half of the field/sim diff is empty by construction.",
    "",
    "= golden_field_gps_dual U golden_sim_tri_w6 (FIELD wins on conflict) +",
    "  (a) the leveling vehicle, 17 keys, literals from config/test_suite/_pv_b.json",
    "      (SPEC_event_harness s24.11);",
    "  (b) the physical clock time-link, 12 keys from runs/_cm_c8b/arm.json with",
    "      clock_anchor_q 2.9e-8 (s24.16 c8b);",
    "  (c) network.clock_disc_jump_test true (s24.20) — the wire contract;",
    "  (d) network.estimate_iono_sh false — free slant iono, SH dropped from the",
    "      producer.  DEPENDENT FORCING, recorded: with W6 still armed and",
    "      network.code_bias_gf_datum unreachable under dual, the satellite-DCB iono",
    "      constraint falls to the EXTERNAL GIM branch — GnssConfigLoadNetwork.cpp:2893",
    "      forces estimation.ionosphere_weighting.enable = true, and",
    "      ApplicationNetworkSetup.cpp:431-456 hard-errors without a .INX.  The COD",
    "      GIM is loaded.  This is the standing external-data tension",
    "      (feedback_no_external_data_dependence: 'the W6 sat code-bias path has a",
    "      load-bearing GIM constraint ... an outstanding conflict'), now on the",
    "      baseline posture rather than default-OFF.  On the SIM bed the same GIM IS",
    "      the truth ionosphere (scenario_summary.json iono_background = 'gim',",
    "      COD0OPSFIN_20241290000_01D_01H_GIM.INX).",
    "  (e) the scope cut: GPS only, DUAL, lanes off, F3 off, seams/snapshots/warm",
    "      resumes off, consumer cold and float.",
    "",
    "HOW DUAL IS MADE SOLVABLE (RF-0's first finding) — network.rx_code_bias_datum.",
    "The RECEIVER code-bias sum-to-zero gauge is in the SIM golden and absent from the",
    "FIELD golden.  Under TRIPLE the loader's own claim holds — 'the receiver half is",
    "already determined through the satellite gauge here' (GnssConfigLoadNetwork.cpp:",
    "3099-3107) — and s24.14's c2 arm measured it a no-op on every bar.  Under DUAL",
    "that claim FAILS: the receiver-DCB levels leave TEN extra near-zero directions,",
    "the null-space gate sees 13 against a declared 1, SBC's cap (6/epoch) refuses the",
    "completion, drop-and-heal anchors 30 columns and still re-gates at 3, and EVERY",
    "epoch is withheld.  Armed, the same posture gates at 3 and SBC completes the 2",
    "extra along their own eigenvectors: 20/20 emitted, 0 withheld.",
    "",
    "  MEASURED LADDER (20-epoch arms on main's Release 24977604..., this exe):",
    "    _rf0_c5_repro      PV-B + frequency_mode dual  (the literal s24.14 c5)",
    "                       -> 13 ev < cutoff, 0/20 emitted, 20 withheld",
    "    _rf0_c5_fix        + L5 dropped from signal_priorities   -> 13, 0/20",
    "    _rf0_c5_shoff      + estimate_iono_sh false              -> 13, 0/20",
    "    _rf0_rxcbdat_off   THIS config minus rx_code_bias_datum  -> 13, 0/20",
    "    _rf0_field_smoke   THIS config                           ->  3, 20/20",
    "",
    "  s24.14 c5's ATTRIBUTION IS REFUTED.  It read the 13 as 'W6's L5 code-bias",
    "  columns registered but unobserved'.  They are not registered at all under dual:",
    "  frequency_mode restricts the band map BEFORE any state is allocated",
    "  (PppModel.cpp:147-158 getBandMap / activeFrequencyBands -> :957, applied as",
    "  buildFrequencyBandMap's allowed_bands filter), and the W6 column count is",
    "  n_cb = 97 IDENTICALLY on all five arms — L5 in or out of signal_priorities, SH",
    "  on or off.  Putting L5 back into this config's priority list is byte-identical",
    "  on the products (sha256 6218a202... both, _rf0_field_smoke vs",
    "  _rf0_field_smoke_c5).  The reason c6 (require_atx_coverage) read as a",
    "  'byte-identical gate failure to c5' is that neither arm ever touched the cause.",
    "",
    "  processing.signal_priorities.GPS below is nevertheless the loader's own",
    "  expansion of the field golden's attribute string 'WCSLXPYMIQ' MINUS the band-5",
    "  block — belt-and-braces, and measured byte-identical.  It matters only because",
    "  decideConstellationCanonicals (PppModel.cpp:438-470) walks the priority list",
    "  with NO band filter, so without it the constellation canonical map still",
    "  registers C5x/L5x for every station on a run the scope cut says is L1/L2.  The",
    "  ARRAY form is the loader's only way to drop a band: the STRING form is force-",
    "  expanded over system_bands[\"GPS\"] = {'1','2','5'} at GnssConfigLoad.cpp:795-828.",
]


def write(path, base_flat, overlay_flat, comment_extra):
    f = dict(base_flat)
    f.update(overlay_flat)
    doc = unflat(f)
    out = {"_comment": COMMENT_COMMON + [""] + comment_extra}
    for k in ["app_mode", "data_source", "files", "data_download", "processing",
              "estimation", "filter", "model_corrections", "logging", "network"]:
        if k in doc:
            out[k] = doc[k]
    for k in doc:
        if k not in out:
            out[k] = doc[k]
    with open(path, "w", newline="\n", encoding="utf-8") as fh:
        json.dump(out, fh, indent=1)
        fh.write("\n")
    return f


os.makedirs(OUT, exist_ok=True)
ffl = write(os.path.join(OUT, "field_gps_dual_base.json"), posture,
            bed(ff, "C:/GnssProcessor/runs/_rf0_field6"),
            ["BED: the 61-station DOY-129 IGS roster, MBAR00UGA clock datum, the",
             "WTZZ00DEU held-out consumer, fractional (wrapping) phase wind-up and the",
             "field's own phase sigma."])
fsl = write(os.path.join(OUT, "sim_gps_dual_base.json"), posture,
            bed(fs, "C:/GnssProcessor/runs/_rf0_sim6"),
            ["BED: the 61-station simnet60_quiet_wu93_24h truth bed, S30000SIM clock",
             "datum, the SZZ000SIM held-out consumer, ACCUMULATED phase wind-up (the",
             "generator accumulated it — scenario_summary phase_windup.accumulated) and",
             "the generator's own phase sigma 0.003 m."])

# --------------------------------------------------------------------------------
# 3. the classified diff
# --------------------------------------------------------------------------------
keys = sorted(set(ffl) | set(fsl))
print("=== FIELD vs SIM classified diff ===")
nb = np = 0
for k in keys:
    a, b = ffl.get(k, "<absent>"), fsl.get(k, "<absent>")
    if a == b:
        continue
    cls = "BED" if k in BED_KEYS else "POSTURE"
    if cls == "BED":
        nb += 1
    else:
        np += 1
    sa, sb = json.dumps(a)[:76], json.dumps(b)[:76]
    print(f"  [{cls}] {k}\n      field={sa}\n      sim  ={sb}")
print(f"\nBED differences: {nb}   POSTURE differences: {np}   (POSTURE must be 0)")
