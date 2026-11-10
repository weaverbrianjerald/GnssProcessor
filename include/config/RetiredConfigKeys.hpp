#pragma once

/**
 * @file RetiredConfigKeys.hpp
 * @brief The tombstone of the retired configuration keys: every key the loader once read and
 *        reads no more, each with the deletion that removed it.
 *
 * The loader ignores a key it does not know without a word.  A configuration that still sets
 * a key deleted from the loader therefore runs on the code as it now stands, and nothing in
 * the log says the key had no effect.  This header declares every deleted key.  The network
 * section loader passes the merged document (after @c extends) to @ref warnRetiredConfigKeys,
 * which logs one warning per retired key the document sets, naming the key and the deletion.
 * The scan reads the document and changes nothing else: a configuration loads to the same
 * values with or without its retired keys.
 *
 * A key counts as set under either spelling the old loader read: the dotted path through
 * nested objects (@c detail::resolvePath), or the flat top-level key that @c detail::jval
 * tried second.  A path with @c "[]" names a member of the objects of an array
 * (@c network.stations[].fixed), reported once for the whole array.
 *
 * The rows are the 150 network keys deleted from the loader with their code on 2026-10-14
 * (main 4c0e731 to 8409b43; the record is docs/record/refound/RKD_retired_key_deletion_2026-10-14.md,
 * id "RKD"): the 141 REMOVE rows of the configuration ledger at main 4bdd693, the three
 * @c lane_carry keys deleted with the WL-3 carry, the five @c mw_wl keys the lane layer
 * orphaned, and @c iono_constraint_deweight_stations, which had no ledger row.  Each flat
 * spelling is the legacy key of the key's @c jval call in the loader at 4bdd693.  A family is
 * one deletion commit on main.  tests/test_retired_config_keys.cpp pins the rows against that
 * ledger and refuses a row that names a key the current ledger lists as read.
 *
 * A key deleted from the loader later gets its row here in the same commit, under a family
 * for its deletion.  A row leaves only if its name returns to the loader as a live key.
 *
 * The table is in this header, not in a src/core/config/*.cpp, on purpose:
 * scripts/loader_keys.py and scripts/comment_gate.py read every string literal of those files
 * as a key the loader reads, so a retired key spelled there would read as live and
 * scripts/rapid_arm.py make would stop refusing it.
 */

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

namespace spdlog { class logger; }

namespace GnssProcessor {

/// @brief One deletion: a family of keys removed from the loader with their code.
struct RetiredKeyFamily {
    std::string_view card;    ///< the id of the deletion's record under docs/refound/, printed in the warning
    std::string_view date;    ///< the date the deletion landed on main
    std::string_view commit;  ///< the commit on main that deleted the keys and their code
    std::string_view what;    ///< what the keys armed, in the record's words
};

/// @brief One retired key: the spellings a configuration could set it by, and its deletion.
struct RetiredConfigKey {
    std::string_view path;           ///< dotted path; "[]" marks the objects of an array
    std::string_view flat;           ///< flat top-level spelling the old loader also read; empty when none
    const RetiredKeyFamily* family;  ///< the deletion that removed it
};

/// @brief A family of the deletion of 2026-10-14 (record "RKD").
constexpr RetiredKeyFamily retiredOn20261014(std::string_view commit, std::string_view what) {
    return RetiredKeyFamily{"RKD", "2026-10-14", commit, what};
}

// The deletion of 2026-10-14, one family per commit on main, in commit order.
inline constexpr RetiredKeyFamily kRetiredParseOnly =
    retiredOn20261014("4c0e731", "the parse-only and loader-forced keys");
inline constexpr RetiredKeyFamily kRetiredWl1 =
    retiredOn20261014("f1ea3de", "the WL-1 MW-seeded wide-lane rows and the F6 dump");
inline constexpr RetiredKeyFamily kRetiredWl3 =
    retiredOn20261014("c5be295", "the WL-3 lane-coordinate carry and the UWL family pairs");
inline constexpr RetiredKeyFamily kRetiredUwl1 =
    retiredOn20261014("819f4a6", "the UWL-1 per-constellation lane tiers");
inline constexpr RetiredKeyFamily kRetiredWl2 =
    retiredOn20261014("365edfe", "the WL-2 EWL lane, the WL inject, the GF bridge and the lane layer");
inline constexpr RetiredKeyFamily kRetiredLaneSlip =
    retiredOn20261014("f48868c", "the lane slip responses (bridge v3 EWL slot, statistical retap, its corroboration, lane snap)");
inline constexpr RetiredKeyFamily kRetiredFastClock =
    retiredOn20261014("1de8b9b", "the FC-1 split-cadence fast clock");
inline constexpr RetiredKeyFamily kRetiredClockRate =
    retiredOn20261014("2ba604a", "the finite-difference C1 clock rate");
inline constexpr RetiredKeyFamily kRetiredClv =
    retiredOn20261014("b326c8c", "the CLV out-of-solve clock-level re-base");
inline constexpr RetiredKeyFamily kRetiredEnsembleDatum =
    retiredOn20261014("7914263", "the ensemble phase datum and its hold");
inline constexpr RetiredKeyFamily kRetiredPbFilter =
    retiredOn20261014("6d719e4", "the PB emission-continuity filter");
inline constexpr RetiredKeyFamily kRetiredSmoothedBlock =
    retiredOn20261014("08cd639", "the smoothed hold's block member");
inline constexpr RetiredKeyFamily kRetiredShGate =
    retiredOn20261014("b216f55", "the SH health gate");
inline constexpr RetiredKeyFamily kRetiredDeweight =
    retiredOn20261014("158b9b2", "the station iono deweight gate and its release hygiene");
inline constexpr RetiredKeyFamily kRetiredAdaptiveIonoQ =
    retiredOn20261014("1d35585", "the network adaptive iono q");
inline constexpr RetiredKeyFamily kRetiredT1dLatch =
    retiredOn20261014("09e3151", "the T1d SH-convergence latch");
inline constexpr RetiredKeyFamily kRetiredIonoCarry =
    retiredOn20261014("f4be1a8", "the diagonal slant-iono carry");
inline constexpr RetiredKeyFamily kRetiredLevelTools =
    retiredOn20261014("15b927c", "the level tools LV-1, LV-2, LV-4, LV-5 and S2H");
inline constexpr RetiredKeyFamily kRetiredCodeBiasDatum =
    retiredOn20261014("d1cd9ea", "the retired code-bias datum options and the clock signal-basis hold");
inline constexpr RetiredKeyFamily kRetiredYoungInflate =
    retiredOn20261014("1068750", "the age-aware young-arc inflate");
inline constexpr RetiredKeyFamily kRetiredF1F3AgeGate =
    retiredOn20261014("34793c9", "F1 and the F3 age gate");
inline constexpr RetiredKeyFamily kRetiredCouplingVariants =
    retiredOn20261014("e7e470b", "the coupling carry's SH marginalize, joint whitening and gauge-free closure");
inline constexpr RetiredKeyFamily kRetiredResumeKnobs =
    retiredOn20261014("98df220", "the retired warm-resume knobs (RS-2 re-base, RS-6 shdev switch, seam re-inflate, resume shock)");
inline constexpr RetiredKeyFamily kRetiredParallelReduce =
    retiredOn20261014("6e86437", "the parallel Schur reduce fan");
inline constexpr RetiredKeyFamily kRetiredStudy3 =
    retiredOn20261014("dab7d80", "the study-3 band-scoped responses");
inline constexpr RetiredKeyFamily kRetiredJumpDecommon =
    retiredOn20261014("475b587", "the de-commoned phase jump test (card JT)");
inline constexpr RetiredKeyFamily kRetiredUnmatchedAntenna =
    retiredOn20261014("159f846", "the unmatched-antenna phase-bias exclude");
inline constexpr RetiredKeyFamily kRetiredRaimRefloat =
    retiredOn20261014("8409b43", "the RAIM carry-invariant refloat actuator");

/**
 * @brief Every retired configuration key, by family in commit order, then by path.
 *
 * Each row: the dotted path, the flat spelling the loader at 4bdd693 also read, the family.
 */
inline constexpr RetiredConfigKey kRetiredConfigKeys[] = {
    // kRetiredParseOnly, commit 4c0e731
    {"network.amb_float_q_m2s", "amb_float_q_m2s", &kRetiredParseOnly},
    {"network.ambiguity_resolution.wl_cascade", "network_ar_wl_cascade", &kRetiredParseOnly},
    {"network.carry_invariant_honesty", "network_carry_invariant_honesty", &kRetiredParseOnly},
    {"network.carry_invariant_innov_refold", "network_carry_invariant_innov_refold", &kRetiredParseOnly},
    {"network.prefit_fuse_require_uncheckable", "prefit_fuse_require_uncheckable", &kRetiredParseOnly},
    {"network.serialize_sbasis_contract", "serialize_sbasis_contract", &kRetiredParseOnly},
    {"network.stations[].fixed", "", &kRetiredParseOnly},
    // kRetiredWl1, commit f1ea3de
    {"network.mw_wl.debug_r_scale", "network_mw_wl_debug_r_scale", &kRetiredWl1},
    {"network.mw_wl.enable", "network_mw_wl_enable", &kRetiredWl1},
    {"network.mw_wl.ewl_fold_rows", "network_mw_wl_ewl_fold_rows", &kRetiredWl1},
    {"network.mw_wl.f6_all_blocks", "network_mw_wl_f6_all_blocks", &kRetiredWl1},
    {"network.mw_wl.f6_dump", "network_mw_wl_f6_dump", &kRetiredWl1},
    {"network.mw_wl.f6_max_rows", "network_mw_wl_f6_max_rows", &kRetiredWl1},
    {"network.mw_wl.fix_enable", "network_mw_wl_fix_enable", &kRetiredWl1},
    {"network.mw_wl.fix_frac_cyc", "network_mw_wl_fix_frac_cyc", &kRetiredWl1},
    {"network.mw_wl.fix_sigma_cyc", "network_mw_wl_fix_sigma_cyc", &kRetiredWl1},
    {"network.mw_wl.min_arc_epochs", "network_mw_wl_min_arc_epochs", &kRetiredWl1},
    // kRetiredWl3, commit c5be295
    {"network.lane_carry.bridge_meas", "network_lane_carry_bridge_meas", &kRetiredWl3},
    {"network.lane_carry.enable", "network_lane_carry_enable", &kRetiredWl3},
    {"network.lane_carry.fold_blocked", "network_lane_carry_fold_blocked", &kRetiredWl3},
    {"network.lane_carry.lane_meas", "network_lane_carry_lane_meas", &kRetiredWl3},
    {"network.lane_carry.parallel_components", "network_lane_carry_parallel_components", &kRetiredWl3},
    {"network.lane_carry.q_wl_scale", "network_lane_carry_q_wl_scale", &kRetiredWl3},
    {"network.lane_carry.sigma_wl_fresh_cyc", "network_lane_carry_sigma_wl_fresh_cyc", &kRetiredWl3},
    {"network.lane_carry.skip_pinned_increments", "network_lane_carry_skip_pinned_increments", &kRetiredWl3},
    {"network.lane_carry.skip_pinned_within_n", "network_lane_carry_skip_pinned_within_n", &kRetiredWl3},
    {"network.mw_wl.uwl_family_min_arcs", "network_mw_wl_uwl_family_min_arcs", &kRetiredWl3},
    {"network.mw_wl.uwl_family_pairs", "network_mw_wl_uwl_family_pairs", &kRetiredWl3},
    // kRetiredUwl1, commit 819f4a6
    {"network.mw_wl.uwl_cold_fix", "network_mw_wl_uwl_cold_fix", &kRetiredUwl1},
    {"network.mw_wl.uwl_cold_min_lambda_m", "network_mw_wl_uwl_cold_min_lambda_m", &kRetiredUwl1},
    {"network.mw_wl.uwl_cold_sigma_m", "network_mw_wl_uwl_cold_sigma_m", &kRetiredUwl1},
    {"network.mw_wl.uwl_dual_top", "network_mw_wl_uwl_dual_top", &kRetiredUwl1},
    {"network.mw_wl.uwl_enable", "network_mw_wl_uwl_enable", &kRetiredUwl1},
    {"network.mw_wl.uwl_gal_top_band", "network_mw_wl_uwl_gal_top_band", &kRetiredUwl1},
    {"network.mw_wl.uwl_mid_enable", "network_mw_wl_uwl_mid_enable", &kRetiredUwl1},
    {"network.mw_wl.uwl_sib_gate", "network_mw_wl_uwl_sib_gate", &kRetiredUwl1},
    // kRetiredWl2, commit 365edfe
    {"network.mw_wl.bridge_max_sigma_cyc", "network_mw_wl_bridge_max_sigma_cyc", &kRetiredWl2},
    {"network.mw_wl.bridge_sample_debias", "network_mw_wl_bridge_sample_debias", &kRetiredWl2},
    {"network.mw_wl.d5_dump", "network_mw_wl_d5_dump", &kRetiredWl2},
    {"network.mw_wl.ewl_enable", "network_mw_wl_ewl_enable", &kRetiredWl2},
    {"network.mw_wl.ewl_fix_frac_cyc", "network_mw_wl_ewl_fix_frac_cyc", &kRetiredWl2},
    {"network.mw_wl.ewl_fix_sigma_cyc", "network_mw_wl_ewl_fix_sigma_cyc", &kRetiredWl2},
    {"network.mw_wl.ewl_min_arc_epochs", "network_mw_wl_ewl_min_arc_epochs", &kRetiredWl2},
    {"network.mw_wl.fix_weight_sigma_cyc", "network_mw_wl_fix_weight_sigma_cyc", &kRetiredWl2},
    {"network.mw_wl.inject_disagree_min_cyc", "network_mw_wl_inject_disagree_min_cyc", &kRetiredWl2},
    {"network.mw_wl.lane_max_abs_xlin_m", "network_mw_wl_lane_max_abs_xlin_m", &kRetiredWl2},
    {"network.mw_wl.lane_trace", "network_mw_wl_lane_trace", &kRetiredWl2},
    {"network.mw_wl.max_rows_per_epoch", "network_mw_wl_max_rows_per_epoch", &kRetiredWl2},
    {"network.mw_wl.sigma_floor_cyc", "network_mw_wl_sigma_floor_cyc", &kRetiredWl2},
    {"network.mw_wl.wl_fix_frac_cyc", "network_mw_wl_wl_fix_frac_cyc", &kRetiredWl2},
    {"network.mw_wl.wl_fix_sigma_cyc", "network_mw_wl_wl_fix_sigma_cyc", &kRetiredWl2},
    {"network.mw_wl.wl_inject_enable", "network_mw_wl_wl_inject_enable", &kRetiredWl2},
    // kRetiredLaneSlip, commit f48868c
    {"network.cycle_slip.relevel_bridge_ewl", "network_cycle_slip_relevel_bridge_ewl", &kRetiredLaneSlip},
    {"network.cycle_slip.relevel_bridge_ewl_sigma_floor_cyc", "network_cycle_slip_relevel_bridge_ewl_sigma_floor_cyc", &kRetiredLaneSlip},
    {"network.cycle_slip.relevel_lane_snap", "network_cycle_slip_relevel_lane_snap", &kRetiredLaneSlip},
    {"network.cycle_slip.relevel_retap_corroborate", "network_cycle_slip_relevel_retap_corroborate", &kRetiredLaneSlip},
    {"network.cycle_slip.relevel_retap_statistical", "network_cycle_slip_relevel_retap_statistical", &kRetiredLaneSlip},
    {"network.cycle_slip.relevel_retap_streak_max", "network_cycle_slip_relevel_retap_streak_max", &kRetiredLaneSlip},
    {"network.cycle_slip.relevel_retap_var_m2", "network_cycle_slip_relevel_retap_var_m2", &kRetiredLaneSlip},
    // kRetiredFastClock, commit 1de8b9b
    {"network.fast_clock.allow_single_band", "network_fast_clock_allow_single_band", &kRetiredFastClock},
    {"network.fast_clock.emit", "network_fast_clock_emit", &kRetiredFastClock},
    {"network.fast_clock.emit_max_gap_s", "network_fast_clock_emit_max_gap_s", &kRetiredFastClock},
    {"network.fast_clock.enable", "network_fast_clock_enable", &kRetiredFastClock},
    {"network.fast_clock.min_sats_per_station", "network_fast_clock_min_sats_per_station", &kRetiredFastClock},
    {"network.fast_clock.min_stations_per_sat", "network_fast_clock_min_stations_per_sat", &kRetiredFastClock},
    {"network.fast_clock.screen_k_sigma", "network_fast_clock_screen_k_sigma", &kRetiredFastClock},
    {"network.fast_clock.shadow", "network_fast_clock_shadow", &kRetiredFastClock},
    {"network.fast_clock.trace", "network_fast_clock_trace", &kRetiredFastClock},
    {"network.fast_clock.trace_sat_stride", "network_fast_clock_trace_sat_stride", &kRetiredFastClock},
    // kRetiredClockRate, commit 2ba604a
    {"network.emit_clock_rate", "network_emit_clock_rate", &kRetiredClockRate},
    // kRetiredClv, commit b326c8c
    {"network.clock_level_carry", "network_clock_level_carry", &kRetiredClv},
    {"network.clock_level_confirm_epochs", "network_clock_level_confirm_epochs", &kRetiredClv},
    {"network.clock_level_datum_pause", "network_clock_level_datum_pause", &kRetiredClv},
    {"network.clock_level_free_cb_follow", "network_clock_level_free_cb_follow", &kRetiredClv},
    {"network.clock_level_free_cb_rebase", "network_clock_level_free_cb_rebase", &kRetiredClv},
    {"network.clock_level_horizon_s", "network_clock_level_horizon_s", &kRetiredClv},
    {"network.clock_level_pin_pair_read", "network_clock_level_pin_pair_read", &kRetiredClv},
    {"network.clock_level_pivot_ref", "network_clock_level_pivot_ref", &kRetiredClv},
    {"network.clock_level_sigma_floor_m", "network_clock_level_sigma_floor_m", &kRetiredClv},
    // kRetiredEnsembleDatum, commit 7914263
    {"network.phase_datum_ensemble", "network_phase_datum_ensemble", &kRetiredEnsembleDatum},
    {"network.phase_datum_hold_sigma_m", "network_phase_datum_hold_sigma_m", &kRetiredEnsembleDatum},
    // kRetiredPbFilter, commit 6d719e4
    {"network.emit_pb_filter", "network_emit_pb_filter", &kRetiredPbFilter},
    {"network.emit_pb_filter_emit_sigma", "network_emit_pb_filter_emit_sigma", &kRetiredPbFilter},
    {"network.emit_pb_filter_gate_v2", "network_emit_pb_filter_gate_v2", &kRetiredPbFilter},
    {"network.emit_pb_filter_jump_k", "network_emit_pb_filter_jump_k", &kRetiredPbFilter},
    {"network.emit_pb_filter_max_gap_s", "network_emit_pb_filter_max_gap_s", &kRetiredPbFilter},
    {"network.emit_pb_filter_meas_sigma_m", "network_emit_pb_filter_meas_sigma_m", &kRetiredPbFilter},
    {"network.emit_pb_filter_q", "network_emit_pb_filter_q", &kRetiredPbFilter},
    {"network.emit_pb_filter_window_epochs", "network_emit_pb_filter_window_epochs", &kRetiredPbFilter},
    // kRetiredSmoothedBlock, commit 08cd639
    {"network.emit_pb_hold_smoothed_block", "emit_pb_hold_smoothed_block", &kRetiredSmoothedBlock},
    // kRetiredShGate, commit b216f55
    {"network.sh_gate_min_epochs", "sh_gate_min_epochs", &kRetiredShGate},
    {"network.sh_gate_rms_max_tecu", "sh_gate_rms_max_tecu", &kRetiredShGate},
    {"network.sh_health_gate", "sh_health_gate", &kRetiredShGate},
    // kRetiredDeweight, commit 158b9b2
    {"network.iono_constraint_deweight_adaptive", "iono_constraint_deweight_adaptive", &kRetiredDeweight},
    {"network.iono_constraint_deweight_factor", "iono_constraint_deweight_factor", &kRetiredDeweight},
    {"network.iono_constraint_deweight_floor", "iono_constraint_deweight_floor", &kRetiredDeweight},
    {"network.iono_constraint_deweight_min_epochs", "iono_constraint_deweight_min_epochs", &kRetiredDeweight},
    {"network.iono_constraint_deweight_rate_limit", "iono_constraint_deweight_rate_limit", &kRetiredDeweight},
    {"network.iono_constraint_deweight_sigma_hi", "iono_constraint_deweight_sigma_hi", &kRetiredDeweight},
    {"network.iono_constraint_deweight_sigma_lo", "iono_constraint_deweight_sigma_lo", &kRetiredDeweight},
    {"network.iono_constraint_deweight_stations", "iono_constraint_deweight_stations", &kRetiredDeweight},
    {"network.iono_constraint_deweight_window_epochs", "iono_constraint_deweight_window_epochs", &kRetiredDeweight},
    {"network.iono_dw_admit_validate_k", "network_iono_dw_admit_validate_k", &kRetiredDeweight},
    {"network.iono_dw_park_bound_tecu", "network_iono_dw_park_bound_tecu", &kRetiredDeweight},
    {"network.iono_dw_release_floor_eps", "network_iono_dw_release_floor_eps", &kRetiredDeweight},
    {"network.iono_dw_release_reseed", "network_iono_dw_release_reseed", &kRetiredDeweight},
    {"network.iono_dw_release_validate_k", "network_iono_dw_release_validate_k", &kRetiredDeweight},
    // kRetiredAdaptiveIonoQ, commit 1d35585
    {"network.adaptive_iono_q", "network_adaptive_iono_q", &kRetiredAdaptiveIonoQ},
    // kRetiredT1dLatch, commit 09e3151
    {"network.phase_leveling_conv_rms_thresh_tecu", "phase_leveling_conv_rms_thresh_tecu", &kRetiredT1dLatch},
    {"network.phase_leveling_refloat_at_convergence", "phase_leveling_refloat_at_convergence", &kRetiredT1dLatch},
    {"network.phase_leveling_warmup_on_convergence", "phase_leveling_warmup_on_convergence", &kRetiredT1dLatch},
    // kRetiredIonoCarry, commit f4be1a8
    {"network.carry_iono", "carry_iono", &kRetiredIonoCarry},
    {"network.iono_carry_max_gap_s", "iono_carry_max_gap_s", &kRetiredIonoCarry},
    {"network.iono_carry_q", "iono_carry_q", &kRetiredIonoCarry},
    {"network.iono_carry_scope", "iono_carry_scope", &kRetiredIonoCarry},
    {"network.iono_carry_source", "iono_carry_source", &kRetiredIonoCarry},
    {"network.iono_carry_var_floor", "iono_carry_var_floor", &kRetiredIonoCarry},
    // kRetiredLevelTools, commit 15b927c
    {"network.iono_seed_field_frame", "iono_seed_field_frame", &kRetiredLevelTools},
    {"network.iono_seed_field_frame_min_sats", "iono_seed_field_frame_min_sats", &kRetiredLevelTools},
    {"network.rx_cb_carry_demean", "rx_cb_carry_demean", &kRetiredLevelTools},
    {"network.rx_cb_carry_demean_min_stations", "rx_cb_carry_demean_min_stations", &kRetiredLevelTools},
    {"network.rx_cb_carry_demean_within", "rx_cb_carry_demean_within", &kRetiredLevelTools},
    {"network.rx_cb_carry_step_gate", "rx_cb_carry_step_gate", &kRetiredLevelTools},
    {"network.rx_cb_carry_step_k_sigma", "rx_cb_carry_step_k_sigma", &kRetiredLevelTools},
    {"network.rx_cb_harvest_demean", "rx_cb_harvest_demean", &kRetiredLevelTools},
    {"network.rx_cb_harvest_screen_z", "rx_cb_harvest_screen_z", &kRetiredLevelTools},
    // kRetiredCodeBiasDatum, commit d1cd9ea
    {"network.clock_signal_basis_hold", "network_clock_signal_basis_hold", &kRetiredCodeBiasDatum},
    {"network.code_bias_gf_datum", "code_bias_gf_datum", &kRetiredCodeBiasDatum},
    {"network.code_bias_gf_datum_per_family", "code_bias_gf_datum_per_family", &kRetiredCodeBiasDatum},
    {"network.external_sat_code_bias", "external_sat_code_bias", &kRetiredCodeBiasDatum},
    {"network.sat_if_level_datum", "sat_if_level_datum", &kRetiredCodeBiasDatum},
    // kRetiredYoungInflate, commit 1068750
    {"network.phase_leveling_young_inflate_max_age_ep", "phase_leveling_young_inflate_max_age_ep", &kRetiredYoungInflate},
    {"network.phase_leveling_young_inflate_tau_ep", "phase_leveling_young_inflate_tau_ep", &kRetiredYoungInflate},
    {"network.phase_leveling_young_inflate_var_m2", "phase_leveling_young_inflate_var_m2", &kRetiredYoungInflate},
    // kRetiredF1F3AgeGate, commit 34793c9
    {"network.phase_leveling_local_block_min_age_ep", "phase_leveling_local_block_min_age_ep", &kRetiredF1F3AgeGate},
    {"network.phase_leveling_local_block_young_mode", "phase_leveling_local_block_young_mode", &kRetiredF1F3AgeGate},
    {"network.phase_leveling_los_scale", "phase_leveling_los_scale", &kRetiredF1F3AgeGate},
    // kRetiredCouplingVariants, commit e7e470b
    {"network.carry_coupling_joint_whiten", "carry_coupling_joint_whiten", &kRetiredCouplingVariants},
    {"network.carry_coupling_sh_marginalize", "carry_coupling_sh_marginalize", &kRetiredCouplingVariants},
    {"network.shared_carry_gauge_free", "shared_carry_gauge_free", &kRetiredCouplingVariants},
    // kRetiredResumeKnobs, commit 98df220
    {"network.amb_seam_reinflate_var_m2", "amb_seam_reinflate_var_m2", &kRetiredResumeKnobs},
    {"network.resume_carry_rebase", "resume_carry_rebase", &kRetiredResumeKnobs},
    {"network.resume_station_restore_shdev", "resume_station_restore_shdev", &kRetiredResumeKnobs},
    {"network.state_snapshot_resume_shock_m", "state_snapshot_resume_shock_m", &kRetiredResumeKnobs},
    // kRetiredParallelReduce, commit 6e86437
    {"network.parallel_reduce", "parallel_reduce", &kRetiredParallelReduce},
    // kRetiredStudy3, commit dab7d80
    {"network.cycle_slip.refloat_band_scope", "network_cycle_slip_refloat_band_scope", &kRetiredStudy3},
    {"network.raim_erase_band_scope", "raim_erase_band_scope", &kRetiredStudy3},
    // kRetiredJumpDecommon, commit 475b587
    {"network.disc_jump_test_decommon", "disc_jump_test_decommon", &kRetiredJumpDecommon},
    // kRetiredUnmatchedAntenna, commit 159f846
    {"network.phase_bias_exclude_unmatched_antenna", "network_phase_bias_exclude_unmatched_antenna", &kRetiredUnmatchedAntenna},
    // kRetiredRaimRefloat, commit 8409b43
    {"network.raim_carry_invariant_refloat", "network_raim_carry_invariant_refloat", &kRetiredRaimRefloat},
};

/// @brief A retired key that a configuration sets, as @ref findRetiredConfigKeys reports it.
struct RetiredKeyHit {
    const RetiredConfigKey* key{nullptr};  ///< the table row
    std::vector<std::string> spellings;    ///< the spellings found, the dotted path first
    std::string value;                     ///< the value the old loader read, as JSON text (cut at 60 characters)
    std::size_t entries{0};                ///< a "[]" row only: the number of array objects that set it
};

/**
 * @brief Every retired key that a configuration sets.
 * @param root The merged configuration document.
 * @return One hit per row of @ref kRetiredConfigKeys that @p root sets under either spelling,
 *         in table order.  When both spellings are present the value is the dotted path's,
 *         the one the old loader read.
 */
std::vector<RetiredKeyHit> findRetiredConfigKeys(const nlohmann::json& root);

/**
 * @brief The warning text for one hit.
 * @param hit A hit of @ref findRetiredConfigKeys.
 * @return One line naming the key, the spellings found, the value, and the record, date, family
 *         and commit of the deletion.
 */
std::string retiredConfigKeyMessage(const RetiredKeyHit& hit);

/**
 * @brief Log one warning per retired key that a configuration sets.
 * @param root The merged configuration document.
 * @param log  The logger to warn on (the loader passes "gnss_config").
 * @return The number of warnings logged.
 */
std::size_t warnRetiredConfigKeys(const nlohmann::json& root, spdlog::logger& log);

}  // namespace GnssProcessor
