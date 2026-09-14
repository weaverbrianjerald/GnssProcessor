# Config promotion ledger — which default-OFF knobs are validated, and in what regime — SPLIT

`docs/CONFIG_promotion_ledger.md` was split on 2026-09-14 — the original is [archive/CONFIG_promotion_ledger.md](archive/CONFIG_promotion_ledger.md); sections map as follows.

The archived copy is **frozen**: it is kept byte-for-byte as proof and is never appended to. New results go in the card file named below. See [INDEX.md](INDEX.md) for the whole tree and the rules, and [topics/config-ledger.md](topics/config-ledger.md) for this subject.

| section | card id | file |
| --- | --- | --- |
| (title + preamble) | LEDGER | [cards/config-ledger/LEDGER-00-HOW-TO-READ.md](cards/config-ledger/LEDGER-00-HOW-TO-READ.md) |
| Tier 1 — RECOMMEND_ON  (measured benefit, no known counter-scope) | Tier 1 | [cards/config-ledger/LEDGER-TIER-1-RECOMMEND-ON.md](cards/config-ledger/LEDGER-TIER-1-RECOMMEND-ON.md) |
| `network.carry_invariant` | Tier 1 | [cards/config-ledger/LEDGER-TIER-1-RECOMMEND-ON.md](cards/config-ledger/LEDGER-TIER-1-RECOMMEND-ON.md) |
| `network.carry_invariant_innov_calibrate` | Tier 1 | [cards/config-ledger/LEDGER-TIER-1-RECOMMEND-ON.md](cards/config-ledger/LEDGER-TIER-1-RECOMMEND-ON.md) |
| `network.carry_invariant_innov_gate` | Tier 1 | [cards/config-ledger/LEDGER-TIER-1-RECOMMEND-ON.md](cards/config-ledger/LEDGER-TIER-1-RECOMMEND-ON.md) |
| `network.carry_invariant_innov_k` | Tier 1 | [cards/config-ledger/LEDGER-TIER-1-RECOMMEND-ON.md](cards/config-ledger/LEDGER-TIER-1-RECOMMEND-ON.md) |
| `network.carry_invariant_innov_carried_only` | Tier 1 | [cards/config-ledger/LEDGER-TIER-1-RECOMMEND-ON.md](cards/config-ledger/LEDGER-TIER-1-RECOMMEND-ON.md) |
| `network.carry_invariant_qz` | Tier 1 | [cards/config-ledger/LEDGER-TIER-1-RECOMMEND-ON.md](cards/config-ledger/LEDGER-TIER-1-RECOMMEND-ON.md) |
| `network.cycle_slip.if_phase_td` | Tier 1 | [cards/config-ledger/LEDGER-TIER-1-RECOMMEND-ON.md](cards/config-ledger/LEDGER-TIER-1-RECOMMEND-ON.md) |
| `network.cycle_slip.mw_detect` | Tier 1 | [cards/config-ledger/LEDGER-TIER-1-RECOMMEND-ON.md](cards/config-ledger/LEDGER-TIER-1-RECOMMEND-ON.md) |
| `network.emit_clock_frame_hold` | Tier 1 | [cards/config-ledger/LEDGER-TIER-1-RECOMMEND-ON.md](cards/config-ledger/LEDGER-TIER-1-RECOMMEND-ON.md) |
| `network.disc_threshold_frac_cycles` | Tier 1 | [cards/config-ledger/LEDGER-TIER-1-RECOMMEND-ON.md](cards/config-ledger/LEDGER-TIER-1-RECOMMEND-ON.md) |
| `network.emit_pb_filter_gate_v2` | Tier 1 | [cards/config-ledger/LEDGER-TIER-1-RECOMMEND-ON.md](cards/config-ledger/LEDGER-TIER-1-RECOMMEND-ON.md) |
| `network.estimate_iono_sh` | Tier 1 | [cards/config-ledger/LEDGER-TIER-1-RECOMMEND-ON.md](cards/config-ledger/LEDGER-TIER-1-RECOMMEND-ON.md) |
| `network.parallel_seam` | Tier 1 | [cards/config-ledger/LEDGER-TIER-1-RECOMMEND-ON.md](cards/config-ledger/LEDGER-TIER-1-RECOMMEND-ON.md) |
| `network.phase_leveling` | Tier 1 | [cards/config-ledger/LEDGER-TIER-1-RECOMMEND-ON.md](cards/config-ledger/LEDGER-TIER-1-RECOMMEND-ON.md) |
| `network.state_snapshot_path` (RTB-7 warm resume, the family head) | Tier 1 | [cards/config-ledger/LEDGER-TIER-1-RECOMMEND-ON.md](cards/config-ledger/LEDGER-TIER-1-RECOMMEND-ON.md) |
| `network.state_snapshot_require` | Tier 1 | [cards/config-ledger/LEDGER-TIER-1-RECOMMEND-ON.md](cards/config-ledger/LEDGER-TIER-1-RECOMMEND-ON.md) |
| `network.station_pd_pin_aware` | Tier 1 | [cards/config-ledger/LEDGER-TIER-1-RECOMMEND-ON.md](cards/config-ledger/LEDGER-TIER-1-RECOMMEND-ON.md) |
| `network.trace_epoch_status` | Tier 1 | [cards/config-ledger/LEDGER-TIER-1-RECOMMEND-ON.md](cards/config-ledger/LEDGER-TIER-1-RECOMMEND-ON.md) |
| `network.trace_flag_rates` | Tier 1 | [cards/config-ledger/LEDGER-TIER-1-RECOMMEND-ON.md](cards/config-ledger/LEDGER-TIER-1-RECOMMEND-ON.md) |
| `network.trace_section_timers` | Tier 1 | [cards/config-ledger/LEDGER-TIER-1-RECOMMEND-ON.md](cards/config-ledger/LEDGER-TIER-1-RECOMMEND-ON.md) |
| `network.write_run_manifest` | Tier 1 | [cards/config-ledger/LEDGER-TIER-1-RECOMMEND-ON.md](cards/config-ledger/LEDGER-TIER-1-RECOMMEND-ON.md) |
| Tier 2 — RECOMMEND_SCOPED  (helps in a stated regime, hurts or is inert outside it) | Tier 2 (a) | [cards/config-ledger/LEDGER-TIER-2A-IONO-ZWD-CARRY.md](cards/config-ledger/LEDGER-TIER-2A-IONO-ZWD-CARRY.md) |
| `estimation.global_vtec_sh.max_degree` | Tier 2 (a) | [cards/config-ledger/LEDGER-TIER-2A-IONO-ZWD-CARRY.md](cards/config-ledger/LEDGER-TIER-2A-IONO-ZWD-CARRY.md) |
| `estimation.ionosphere_weighting.elevation_cutoff_deg` | Tier 2 (a) | [cards/config-ledger/LEDGER-TIER-2A-IONO-ZWD-CARRY.md](cards/config-ledger/LEDGER-TIER-2A-IONO-ZWD-CARRY.md) |
| `estimation.ionosphere_weighting.enable` | Tier 2 (a) | [cards/config-ledger/LEDGER-TIER-2A-IONO-ZWD-CARRY.md](cards/config-ledger/LEDGER-TIER-2A-IONO-ZWD-CARRY.md) |
| `network.apriori_source = "brdc_rts"` | Tier 2 (a) | [cards/config-ledger/LEDGER-TIER-2A-IONO-ZWD-CARRY.md](cards/config-ledger/LEDGER-TIER-2A-IONO-ZWD-CARRY.md) |
| `network.carry_aware_covariance` | Tier 2 (a) | [cards/config-ledger/LEDGER-TIER-2A-IONO-ZWD-CARRY.md](cards/config-ledger/LEDGER-TIER-2A-IONO-ZWD-CARRY.md) |
| `network.carry_conditional_coupling` | Tier 2 (a) | [cards/config-ledger/LEDGER-TIER-2A-IONO-ZWD-CARRY.md](cards/config-ledger/LEDGER-TIER-2A-IONO-ZWD-CARRY.md) |
| `network.carry_coupling_time_update` | Tier 2 (a) | [cards/config-ledger/LEDGER-TIER-2A-IONO-ZWD-CARRY.md](cards/config-ledger/LEDGER-TIER-2A-IONO-ZWD-CARRY.md) |
| `network.carry_zwd` | Tier 2 (a) | [cards/config-ledger/LEDGER-TIER-2A-IONO-ZWD-CARRY.md](cards/config-ledger/LEDGER-TIER-2A-IONO-ZWD-CARRY.md) |
| `network.carry_zwd_in_block` | Tier 2 (a) | [cards/config-ledger/LEDGER-TIER-2A-IONO-ZWD-CARRY.md](cards/config-ledger/LEDGER-TIER-2A-IONO-ZWD-CARRY.md) |
| `realtime.state_snapshot_path` (+ `_interval_s`, `_horizon_s`, `_seam_refloat`, `_restore_sco… | Tier 2 (b) | [cards/config-ledger/LEDGER-TIER-2B-SEAM-CLOCK-DATUM-SLIP.md](cards/config-ledger/LEDGER-TIER-2B-SEAM-CLOCK-DATUM-SLIP.md) |
| `network.zwd_seam_reseed` (+ `network.zwd_seam_reseed_epochs`) | Tier 2 (b) | [cards/config-ledger/LEDGER-TIER-2B-SEAM-CLOCK-DATUM-SLIP.md](cards/config-ledger/LEDGER-TIER-2B-SEAM-CLOCK-DATUM-SLIP.md) |
| `network.zwd_seam_reseed_q` | Tier 2 (b) | [cards/config-ledger/LEDGER-TIER-2B-SEAM-CLOCK-DATUM-SLIP.md](cards/config-ledger/LEDGER-TIER-2B-SEAM-CLOCK-DATUM-SLIP.md) |
| `network.cb_kaula_prior` | Tier 2 (b) | [cards/config-ledger/LEDGER-TIER-2B-SEAM-CLOCK-DATUM-SLIP.md](cards/config-ledger/LEDGER-TIER-2B-SEAM-CLOCK-DATUM-SLIP.md) |
| `network.cb_kaula_sigma_m` | Tier 2 (b) | [cards/config-ledger/LEDGER-TIER-2B-SEAM-CLOCK-DATUM-SLIP.md](cards/config-ledger/LEDGER-TIER-2B-SEAM-CLOCK-DATUM-SLIP.md) |
| `network.clock_datum_broadcast_anchored` | Tier 2 (b) | [cards/config-ledger/LEDGER-TIER-2B-SEAM-CLOCK-DATUM-SLIP.md](cards/config-ledger/LEDGER-TIER-2B-SEAM-CLOCK-DATUM-SLIP.md) |
| `network.clock_datum_sum_to_zero` | Tier 2 (b) | [cards/config-ledger/LEDGER-TIER-2B-SEAM-CLOCK-DATUM-SLIP.md](cards/config-ledger/LEDGER-TIER-2B-SEAM-CLOCK-DATUM-SLIP.md) |
| `network.code_bias_disc_jump_test` | Tier 2 (b) | [cards/config-ledger/LEDGER-TIER-2B-SEAM-CLOCK-DATUM-SLIP.md](cards/config-ledger/LEDGER-TIER-2B-SEAM-CLOCK-DATUM-SLIP.md) |
| `network.code_datum_time_linked` | Tier 2 (b) | [cards/config-ledger/LEDGER-TIER-2B-SEAM-CLOCK-DATUM-SLIP.md](cards/config-ledger/LEDGER-TIER-2B-SEAM-CLOCK-DATUM-SLIP.md) |
| `network.cold_clock_seed` | Tier 2 (b) | [cards/config-ledger/LEDGER-TIER-2B-SEAM-CLOCK-DATUM-SLIP.md](cards/config-ledger/LEDGER-TIER-2B-SEAM-CLOCK-DATUM-SLIP.md) |
| `network.cycle_slip.datum_station_detect` | Tier 2 (b) | [cards/config-ledger/LEDGER-TIER-2B-SEAM-CLOCK-DATUM-SLIP.md](cards/config-ledger/LEDGER-TIER-2B-SEAM-CLOCK-DATUM-SLIP.md) |
| `network.cycle_slip.if_phase_td_ref_gate` | Tier 2 (b) | [cards/config-ledger/LEDGER-TIER-2B-SEAM-CLOCK-DATUM-SLIP.md](cards/config-ledger/LEDGER-TIER-2B-SEAM-CLOCK-DATUM-SLIP.md) |
| `network.cycle_slip.if_phase_td_ref_warmup_abstain` | Tier 2 (b) | [cards/config-ledger/LEDGER-TIER-2B-SEAM-CLOCK-DATUM-SLIP.md](cards/config-ledger/LEDGER-TIER-2B-SEAM-CLOCK-DATUM-SLIP.md) |
| `network.cycle_slip.if_phase_td_ref_elev_norm` | Tier 2 (b) | [cards/config-ledger/LEDGER-TIER-2B-SEAM-CLOCK-DATUM-SLIP.md](cards/config-ledger/LEDGER-TIER-2B-SEAM-CLOCK-DATUM-SLIP.md) |
| `network.cycle_slip.if_phase_td_ref_elev_clamp_deg` | Tier 2 (b) | [cards/config-ledger/LEDGER-TIER-2B-SEAM-CLOCK-DATUM-SLIP.md](cards/config-ledger/LEDGER-TIER-2B-SEAM-CLOCK-DATUM-SLIP.md) |
| `processing.exclude_eclipsing_satellites` | Tier 2 (b) | [cards/config-ledger/LEDGER-TIER-2B-SEAM-CLOCK-DATUM-SLIP.md](cards/config-ledger/LEDGER-TIER-2B-SEAM-CLOCK-DATUM-SLIP.md) |
| `network.amb_seam_reinflate_var_m2` | Tier 2 (b) | [cards/config-ledger/LEDGER-TIER-2B-SEAM-CLOCK-DATUM-SLIP.md](cards/config-ledger/LEDGER-TIER-2B-SEAM-CLOCK-DATUM-SLIP.md) |
| `network.sticky_pivot`  (RTA-1 sticky clock-pivot election) | Tier 2 (c) | [cards/config-ledger/LEDGER-TIER-2C-RESUME-FAMILY.md](cards/config-ledger/LEDGER-TIER-2C-RESUME-FAMILY.md) |
| `network.resume_coupling_restore`  (§18.12 Card-1 coupling-regressor restore — read side) | Tier 2 (c) | [cards/config-ledger/LEDGER-TIER-2C-RESUME-FAMILY.md](cards/config-ledger/LEDGER-TIER-2C-RESUME-FAMILY.md) |
| `network.resume_carry_rebase` | Tier 2 (c) | [cards/config-ledger/LEDGER-TIER-2C-RESUME-FAMILY.md](cards/config-ledger/LEDGER-TIER-2C-RESUME-FAMILY.md) |
| `network.resume_d5_warmup_ep` | Tier 2 (c) | [cards/config-ledger/LEDGER-TIER-2C-RESUME-FAMILY.md](cards/config-ledger/LEDGER-TIER-2C-RESUME-FAMILY.md) |
| `network.resume_datum_restore` | Tier 2 (c) | [cards/config-ledger/LEDGER-TIER-2C-RESUME-FAMILY.md](cards/config-ledger/LEDGER-TIER-2C-RESUME-FAMILY.md) |
| `network.resume_pb_seam_mark` | Tier 2 (c) | [cards/config-ledger/LEDGER-TIER-2C-RESUME-FAMILY.md](cards/config-ledger/LEDGER-TIER-2C-RESUME-FAMILY.md) |
| `network.resume_emit_restore` | Tier 2 (c) | [cards/config-ledger/LEDGER-TIER-2C-RESUME-FAMILY.md](cards/config-ledger/LEDGER-TIER-2C-RESUME-FAMILY.md) |
| `network.resume_station_restore` | Tier 2 (c) | [cards/config-ledger/LEDGER-TIER-2C-RESUME-FAMILY.md](cards/config-ledger/LEDGER-TIER-2C-RESUME-FAMILY.md) |
| `network.resume_rxcb_lin_restore` | Tier 2 (c) | [cards/config-ledger/LEDGER-TIER-2C-RESUME-FAMILY.md](cards/config-ledger/LEDGER-TIER-2C-RESUME-FAMILY.md) |
| `network.resume_signal_canon_restore` | Tier 2 (c) | [cards/config-ledger/LEDGER-TIER-2C-RESUME-FAMILY.md](cards/config-ledger/LEDGER-TIER-2C-RESUME-FAMILY.md) |
| `network.resume_border_restore` | Tier 2 (c) | [cards/config-ledger/LEDGER-TIER-2C-RESUME-FAMILY.md](cards/config-ledger/LEDGER-TIER-2C-RESUME-FAMILY.md) |
| `network.serialize_disc_counters` | Tier 2 (c) | [cards/config-ledger/LEDGER-TIER-2C-RESUME-FAMILY.md](cards/config-ledger/LEDGER-TIER-2C-RESUME-FAMILY.md) |
| `network.readmit_seed_horizon_s`  (READMIT continuity — E2 seed-first re-admission, BUILT 202… | Tier 2 (c) | [cards/config-ledger/LEDGER-TIER-2C-RESUME-FAMILY.md](cards/config-ledger/LEDGER-TIER-2C-RESUME-FAMILY.md) |
| `network.readmit_coupling_marg_ep`  (READMIT re-coupling edge on the coupling rows / F3 block… | Tier 2 (c) | [cards/config-ledger/LEDGER-TIER-2C-RESUME-FAMILY.md](cards/config-ledger/LEDGER-TIER-2C-RESUME-FAMILY.md) |
| `network.phase_datum_handover_max_var_ratio`  (the honest-σ-scale PB-gauge handover ceiling, … | Tier 2 (c) | [cards/config-ledger/LEDGER-TIER-2C-RESUME-FAMILY.md](cards/config-ledger/LEDGER-TIER-2C-RESUME-FAMILY.md) |
| `network.phase_datum_candidacy_usable`  (COLD ANCHOR-FALLBACK: a usable carried value is a CA… | Tier 2 (c2) | [cards/config-ledger/LEDGER-TIER-2C2-CANDIDACY-AND-CAPTURE.md](cards/config-ledger/LEDGER-TIER-2C2-CANDIDACY-AND-CAPTURE.md) |
| `network.phase_leveling_local_block_min_age_ep`  (F3 block ADMISSION age gate, BUILT 2026-09-05) | Tier 2 (c2) | [cards/config-ledger/LEDGER-TIER-2C2-CANDIDACY-AND-CAPTURE.md](cards/config-ledger/LEDGER-TIER-2C2-CANDIDACY-AND-CAPTURE.md) |
| `network.readmit_seed_code_bias`  (readmit seed scope sub-knob, BUILT 2026-09-05) | Tier 2 (c2) | [cards/config-ledger/LEDGER-TIER-2C2-CANDIDACY-AND-CAPTURE.md](cards/config-ledger/LEDGER-TIER-2C2-CANDIDACY-AND-CAPTURE.md) |
| `network.state_snapshot_coupling_capture`  (§18.12 coupling-regressor capture — write side) | Tier 2 (c2) | [cards/config-ledger/LEDGER-TIER-2C2-CANDIDACY-AND-CAPTURE.md](cards/config-ledger/LEDGER-TIER-2C2-CANDIDACY-AND-CAPTURE.md) |
| `network.resume_frame_restore`  (§18.10 resume FRAME restore — read side) | Tier 2 (c2) | [cards/config-ledger/LEDGER-TIER-2C2-CANDIDACY-AND-CAPTURE.md](cards/config-ledger/LEDGER-TIER-2C2-CANDIDACY-AND-CAPTURE.md) |
| `network.resume_slip_warmup_ep`  (§18.10 post-resume MW/LLI drain warm-up) | Tier 2 (c2) | [cards/config-ledger/LEDGER-TIER-2C2-CANDIDACY-AND-CAPTURE.md](cards/config-ledger/LEDGER-TIER-2C2-CANDIDACY-AND-CAPTURE.md) |
| `network.state_snapshot_frame_capture`  (§18.10 resume FRAME capture — write side) | Tier 2 (c2) | [cards/config-ledger/LEDGER-TIER-2C2-CANDIDACY-AND-CAPTURE.md](cards/config-ledger/LEDGER-TIER-2C2-CANDIDACY-AND-CAPTURE.md) |
| `network.emit_clock_disc` | Tier 2 (d) | [cards/config-ledger/LEDGER-TIER-2D-EMIT-PARALLEL-RAIM.md](cards/config-ledger/LEDGER-TIER-2D-EMIT-PARALLEL-RAIM.md) |
| `network.emit_combined_sigma  (M4 sigc= / sigcc= / rho= wire tokens)` | Tier 2 (d) | [cards/config-ledger/LEDGER-TIER-2D-EMIT-PARALLEL-RAIM.md](cards/config-ledger/LEDGER-TIER-2D-EMIT-PARALLEL-RAIM.md) |
| `network.emit_generation_id` | Tier 2 (d) | [cards/config-ledger/LEDGER-TIER-2D-EMIT-PARALLEL-RAIM.md](cards/config-ledger/LEDGER-TIER-2D-EMIT-PARALLEL-RAIM.md) |
| `network.emit_pb_filter` | Tier 2 (d) | [cards/config-ledger/LEDGER-TIER-2D-EMIT-PARALLEL-RAIM.md](cards/config-ledger/LEDGER-TIER-2D-EMIT-PARALLEL-RAIM.md) |
| `network.emit_pb_filter_jump_k / _meas_sigma_m` | Tier 2 (d) | [cards/config-ledger/LEDGER-TIER-2D-EMIT-PARALLEL-RAIM.md](cards/config-ledger/LEDGER-TIER-2D-EMIT-PARALLEL-RAIM.md) |
| `network.emit_pb_filter_window_epochs` | Tier 2 (d) | [cards/config-ledger/LEDGER-TIER-2D-EMIT-PARALLEL-RAIM.md](cards/config-ledger/LEDGER-TIER-2D-EMIT-PARALLEL-RAIM.md) |
| `network.emit_ura` | Tier 2 (d) | [cards/config-ledger/LEDGER-TIER-2D-EMIT-PARALLEL-RAIM.md](cards/config-ledger/LEDGER-TIER-2D-EMIT-PARALLEL-RAIM.md) |
| `network.epoch_decimation_n` | Tier 2 (d) | [cards/config-ledger/LEDGER-TIER-2D-EMIT-PARALLEL-RAIM.md](cards/config-ledger/LEDGER-TIER-2D-EMIT-PARALLEL-RAIM.md) |
| `network.estimate_sat_code_bias (W6 OSB slice)` | Tier 2 (d) | [cards/config-ledger/LEDGER-TIER-2D-EMIT-PARALLEL-RAIM.md](cards/config-ledger/LEDGER-TIER-2D-EMIT-PARALLEL-RAIM.md) |
| `network.exclude_unmodeled_antenna_stations` | Tier 2 (d) | [cards/config-ledger/LEDGER-TIER-2D-EMIT-PARALLEL-RAIM.md](cards/config-ledger/LEDGER-TIER-2D-EMIT-PARALLEL-RAIM.md) |
| `network.health_interval_s` | Tier 2 (d) | [cards/config-ledger/LEDGER-TIER-2D-EMIT-PARALLEL-RAIM.md](cards/config-ledger/LEDGER-TIER-2D-EMIT-PARALLEL-RAIM.md) |
| `network.iono_constraint_deweight_adaptive` | Tier 2 (d) | [cards/config-ledger/LEDGER-TIER-2D-EMIT-PARALLEL-RAIM.md](cards/config-ledger/LEDGER-TIER-2D-EMIT-PARALLEL-RAIM.md) |
| `network.iono_constraint_deweight_min_epochs` | Tier 2 (d) | [cards/config-ledger/LEDGER-TIER-2D-EMIT-PARALLEL-RAIM.md](cards/config-ledger/LEDGER-TIER-2D-EMIT-PARALLEL-RAIM.md) |
| `network.iono_constraint_deweight_window_epochs` | Tier 2 (d) | [cards/config-ledger/LEDGER-TIER-2D-EMIT-PARALLEL-RAIM.md](cards/config-ledger/LEDGER-TIER-2D-EMIT-PARALLEL-RAIM.md) |
| `network.lane_carry.parallel_components` | Tier 2 (d) | [cards/config-ledger/LEDGER-TIER-2D-EMIT-PARALLEL-RAIM.md](cards/config-ledger/LEDGER-TIER-2D-EMIT-PARALLEL-RAIM.md) |
| `network.parallel_carry` | Tier 2 (d) | [cards/config-ledger/LEDGER-TIER-2D-EMIT-PARALLEL-RAIM.md](cards/config-ledger/LEDGER-TIER-2D-EMIT-PARALLEL-RAIM.md) |
| `network.parallel_max_threads` | Tier 2 (d) | [cards/config-ledger/LEDGER-TIER-2D-EMIT-PARALLEL-RAIM.md](cards/config-ledger/LEDGER-TIER-2D-EMIT-PARALLEL-RAIM.md) |
| `network.parallel_min_stations` | Tier 2 (d) | [cards/config-ledger/LEDGER-TIER-2D-EMIT-PARALLEL-RAIM.md](cards/config-ledger/LEDGER-TIER-2D-EMIT-PARALLEL-RAIM.md) |
| `network.phase_anchor_carry_reref` | Tier 2 (d) | [cards/config-ledger/LEDGER-TIER-2D-EMIT-PARALLEL-RAIM.md](cards/config-ledger/LEDGER-TIER-2D-EMIT-PARALLEL-RAIM.md) |
| `network.phase_datum_eq34` | Tier 2 (d) | [cards/config-ledger/LEDGER-TIER-2D-EMIT-PARALLEL-RAIM.md](cards/config-ledger/LEDGER-TIER-2D-EMIT-PARALLEL-RAIM.md) |
| `network.phase_datum_min_stations` | Tier 2 (d) | [cards/config-ledger/LEDGER-TIER-2D-EMIT-PARALLEL-RAIM.md](cards/config-ledger/LEDGER-TIER-2D-EMIT-PARALLEL-RAIM.md) |
| `network.phase_datum_prioritized` | Tier 2 (d) | [cards/config-ledger/LEDGER-TIER-2D-EMIT-PARALLEL-RAIM.md](cards/config-ledger/LEDGER-TIER-2D-EMIT-PARALLEL-RAIM.md) |
| `network.phase_leveling_max_gap_s` | Tier 2 (d) | [cards/config-ledger/LEDGER-TIER-2D-EMIT-PARALLEL-RAIM.md](cards/config-ledger/LEDGER-TIER-2D-EMIT-PARALLEL-RAIM.md) |
| `network.product_flush_per_epoch` | Tier 2 (d) | [cards/config-ledger/LEDGER-TIER-2D-EMIT-PARALLEL-RAIM.md](cards/config-ledger/LEDGER-TIER-2D-EMIT-PARALLEL-RAIM.md) |
| `network.raim` | Tier 2 (d) | [cards/config-ledger/LEDGER-TIER-2D-EMIT-PARALLEL-RAIM.md](cards/config-ledger/LEDGER-TIER-2D-EMIT-PARALLEL-RAIM.md) |
| `network.raim_attribution` | Tier 2 (d) | [cards/config-ledger/LEDGER-TIER-2D-EMIT-PARALLEL-RAIM.md](cards/config-ledger/LEDGER-TIER-2D-EMIT-PARALLEL-RAIM.md) |
| `network.raim_withhold_sigma0` | Tier 2 (d) | [cards/config-ledger/LEDGER-TIER-2D-EMIT-PARALLEL-RAIM.md](cards/config-ledger/LEDGER-TIER-2D-EMIT-PARALLEL-RAIM.md) |
| `network.raim_within_epoch` | Tier 2 (d) | [cards/config-ledger/LEDGER-TIER-2D-EMIT-PARALLEL-RAIM.md](cards/config-ledger/LEDGER-TIER-2D-EMIT-PARALLEL-RAIM.md) |
| `network.raim_carry_invariant` | Tier 2 (d) | [cards/config-ledger/LEDGER-TIER-2D-EMIT-PARALLEL-RAIM.md](cards/config-ledger/LEDGER-TIER-2D-EMIT-PARALLEL-RAIM.md) |
| `network.raim_carry_invariant_refloat` | Tier 2 (d) | [cards/config-ledger/LEDGER-TIER-2D-EMIT-PARALLEL-RAIM.md](cards/config-ledger/LEDGER-TIER-2D-EMIT-PARALLEL-RAIM.md) |
| `network.sbasis_complete` | Tier 2 (e) | [cards/config-ledger/LEDGER-TIER-2E-SBASIS-SNAPSHOT-TRACE.md](cards/config-ledger/LEDGER-TIER-2E-SBASIS-SNAPSHOT-TRACE.md) |
| `network.shared_carry_full_cov` | Tier 2 (e) | [cards/config-ledger/LEDGER-TIER-2E-SBASIS-SNAPSHOT-TRACE.md](cards/config-ledger/LEDGER-TIER-2E-SBASIS-SNAPSHOT-TRACE.md) |
| `network.state_snapshot_carry` | Tier 2 (e) | [cards/config-ledger/LEDGER-TIER-2E-SBASIS-SNAPSHOT-TRACE.md](cards/config-ledger/LEDGER-TIER-2E-SBASIS-SNAPSHOT-TRACE.md) |
| `network.state_snapshot_carry_diagonal` | Tier 2 (e) | [cards/config-ledger/LEDGER-TIER-2E-SBASIS-SNAPSHOT-TRACE.md](cards/config-ledger/LEDGER-TIER-2E-SBASIS-SNAPSHOT-TRACE.md) |
| `network.state_snapshot_carry_lin` | Tier 2 (e) | [cards/config-ledger/LEDGER-TIER-2E-SBASIS-SNAPSHOT-TRACE.md](cards/config-ledger/LEDGER-TIER-2E-SBASIS-SNAPSHOT-TRACE.md) |
| `network.state_snapshot_datum_capture` | Tier 2 (e) | [cards/config-ledger/LEDGER-TIER-2E-SBASIS-SNAPSHOT-TRACE.md](cards/config-ledger/LEDGER-TIER-2E-SBASIS-SNAPSHOT-TRACE.md) |
| `network.state_snapshot_emit_capture` | Tier 2 (e) | [cards/config-ledger/LEDGER-TIER-2E-SBASIS-SNAPSHOT-TRACE.md](cards/config-ledger/LEDGER-TIER-2E-SBASIS-SNAPSHOT-TRACE.md) |
| `network.state_snapshot_station_capture` | Tier 2 (e) | [cards/config-ledger/LEDGER-TIER-2E-SBASIS-SNAPSHOT-TRACE.md](cards/config-ledger/LEDGER-TIER-2E-SBASIS-SNAPSHOT-TRACE.md) |
| `network.state_snapshot_rxcb_lin_capture` | Tier 2 (e) | [cards/config-ledger/LEDGER-TIER-2E-SBASIS-SNAPSHOT-TRACE.md](cards/config-ledger/LEDGER-TIER-2E-SBASIS-SNAPSHOT-TRACE.md) |
| `network.state_snapshot_signal_capture` | Tier 2 (e) | [cards/config-ledger/LEDGER-TIER-2E-SBASIS-SNAPSHOT-TRACE.md](cards/config-ledger/LEDGER-TIER-2E-SBASIS-SNAPSHOT-TRACE.md) |
| `network.state_snapshot_path` | Tier 2 (e) | [cards/config-ledger/LEDGER-TIER-2E-SBASIS-SNAPSHOT-TRACE.md](cards/config-ledger/LEDGER-TIER-2E-SBASIS-SNAPSHOT-TRACE.md) |
| `network.trace_dd_fractional` | Tier 2 (e) | [cards/config-ledger/LEDGER-TIER-2E-SBASIS-SNAPSHOT-TRACE.md](cards/config-ledger/LEDGER-TIER-2E-SBASIS-SNAPSHOT-TRACE.md) |
| `network.trace_dd_fractional_max_pairs` | Tier 2 (e) | [cards/config-ledger/LEDGER-TIER-2E-SBASIS-SNAPSHOT-TRACE.md](cards/config-ledger/LEDGER-TIER-2E-SBASIS-SNAPSHOT-TRACE.md) |
| `network.trace_events` | Tier 2 (e) | [cards/config-ledger/LEDGER-TIER-2E-SBASIS-SNAPSHOT-TRACE.md](cards/config-ledger/LEDGER-TIER-2E-SBASIS-SNAPSHOT-TRACE.md) |
| `network.trace_ionosphere` | Tier 2 (e) | [cards/config-ledger/LEDGER-TIER-2E-SBASIS-SNAPSHOT-TRACE.md](cards/config-ledger/LEDGER-TIER-2E-SBASIS-SNAPSHOT-TRACE.md) |
| `network.trace_postfit_sigma0` | Tier 2 (e) | [cards/config-ledger/LEDGER-TIER-2E-SBASIS-SNAPSHOT-TRACE.md](cards/config-ledger/LEDGER-TIER-2E-SBASIS-SNAPSHOT-TRACE.md) |
| `network.trace_raim_redundancy` | Tier 2 (e) | [cards/config-ledger/LEDGER-TIER-2E-SBASIS-SNAPSHOT-TRACE.md](cards/config-ledger/LEDGER-TIER-2E-SBASIS-SNAPSHOT-TRACE.md) |
| Tier 3 — UNVALIDATED  (plausible, never measured on field data) | Tier 3 (a) | [cards/config-ledger/LEDGER-TIER-3A-UNVALIDATED.md](cards/config-ledger/LEDGER-TIER-3A-UNVALIDATED.md) |
| `cbpin  (SbasisContract::cb_pin_pair, S-line cbpin=<const>:<s1>-<s2>)` | Tier 3 (a) | [cards/config-ledger/LEDGER-TIER-3A-UNVALIDATED.md](cards/config-ledger/LEDGER-TIER-3A-UNVALIDATED.md) |
| `estimation.global_vtec_sh.layer2_max_degree` | Tier 3 (a) | [cards/config-ledger/LEDGER-TIER-3A-UNVALIDATED.md](cards/config-ledger/LEDGER-TIER-3A-UNVALIDATED.md) |
| `estimation.global_vtec_sh.rep_error_tau_s` | Tier 3 (a) | [cards/config-ledger/LEDGER-TIER-3A-UNVALIDATED.md](cards/config-ledger/LEDGER-TIER-3A-UNVALIDATED.md) |
| `estimation.global_vtec_sh.row_decimation_epochs` | Tier 3 (a) | [cards/config-ledger/LEDGER-TIER-3A-UNVALIDATED.md](cards/config-ledger/LEDGER-TIER-3A-UNVALIDATED.md) |
| `network.carry_coupling_sh_marginalize` | Tier 3 (a) | [cards/config-ledger/LEDGER-TIER-3A-UNVALIDATED.md](cards/config-ledger/LEDGER-TIER-3A-UNVALIDATED.md) |
| `network.carry_invariant_pd_safe` | Tier 3 (a) | [cards/config-ledger/LEDGER-TIER-3A-UNVALIDATED.md](cards/config-ledger/LEDGER-TIER-3A-UNVALIDATED.md) |
| `network.carry_zwd` | Tier 3 (a) | [cards/config-ledger/LEDGER-TIER-3A-UNVALIDATED.md](cards/config-ledger/LEDGER-TIER-3A-UNVALIDATED.md) |
| `network.cb_excursion_reinit_all_sats` | Tier 3 (a) | [cards/config-ledger/LEDGER-TIER-3A-UNVALIDATED.md](cards/config-ledger/LEDGER-TIER-3A-UNVALIDATED.md) |
| `network.clock_datum_satellite` | Tier 3 (a) | [cards/config-ledger/LEDGER-TIER-3A-UNVALIDATED.md](cards/config-ledger/LEDGER-TIER-3A-UNVALIDATED.md) |
| `network.clock_datum_station` | Tier 3 (a) | [cards/config-ledger/LEDGER-TIER-3A-UNVALIDATED.md](cards/config-ledger/LEDGER-TIER-3A-UNVALIDATED.md) |
| `network.clock_monitor` | Tier 3 (a) | [cards/config-ledger/LEDGER-TIER-3A-UNVALIDATED.md](cards/config-ledger/LEDGER-TIER-3A-UNVALIDATED.md) |
| `network.clock_runaway_check` | Tier 3 (a) | [cards/config-ledger/LEDGER-TIER-3A-UNVALIDATED.md](cards/config-ledger/LEDGER-TIER-3A-UNVALIDATED.md) |
| `network.combined_sigma_agek  (M4 agek= S-line token)` | Tier 3 (a) | [cards/config-ledger/LEDGER-TIER-3A-UNVALIDATED.md](cards/config-ledger/LEDGER-TIER-3A-UNVALIDATED.md) |
| `network.cycle_slip.mw_lli_corroborate` | Tier 3 (a) | [cards/config-ledger/LEDGER-TIER-3A-UNVALIDATED.md](cards/config-ledger/LEDGER-TIER-3A-UNVALIDATED.md) |
| `network.emit_pb_filter_emit_sigma` | Tier 3 (a) | [cards/config-ledger/LEDGER-TIER-3A-UNVALIDATED.md](cards/config-ledger/LEDGER-TIER-3A-UNVALIDATED.md) |
| `network.emitter_staleness_horizon_s` | Tier 3 (a) | [cards/config-ledger/LEDGER-TIER-3A-UNVALIDATED.md](cards/config-ledger/LEDGER-TIER-3A-UNVALIDATED.md) |
| `network.iono_constraint_deweight_factor (+ network.iono_constraint_deweight_stations)` | Tier 3 (a) | [cards/config-ledger/LEDGER-TIER-3A-UNVALIDATED.md](cards/config-ledger/LEDGER-TIER-3A-UNVALIDATED.md) |
| `network.iono_constraint_deweight_floor` | Tier 3 (a) | [cards/config-ledger/LEDGER-TIER-3A-UNVALIDATED.md](cards/config-ledger/LEDGER-TIER-3A-UNVALIDATED.md) |
| `network.lane_carry.skip_pinned_increments` | Tier 3 (a) | [cards/config-ledger/LEDGER-TIER-3A-UNVALIDATED.md](cards/config-ledger/LEDGER-TIER-3A-UNVALIDATED.md) |
| `network.lane_carry.skip_pinned_within_n` | Tier 3 (a) | [cards/config-ledger/LEDGER-TIER-3A-UNVALIDATED.md](cards/config-ledger/LEDGER-TIER-3A-UNVALIDATED.md) |
| `network.mdb` | Tier 3 (a) | [cards/config-ledger/LEDGER-TIER-3A-UNVALIDATED.md](cards/config-ledger/LEDGER-TIER-3A-UNVALIDATED.md) |
| `network.mw_wl.max_rows_per_epoch` | Tier 3 (a) | [cards/config-ledger/LEDGER-TIER-3A-UNVALIDATED.md](cards/config-ledger/LEDGER-TIER-3A-UNVALIDATED.md) |
| `network.per_const_withhold` | Tier 3 (a) | [cards/config-ledger/LEDGER-TIER-3A-UNVALIDATED.md](cards/config-ledger/LEDGER-TIER-3A-UNVALIDATED.md) |
| `network.phase_datum_eq34_extend_only` | Tier 3 (a) | [cards/config-ledger/LEDGER-TIER-3A-UNVALIDATED.md](cards/config-ledger/LEDGER-TIER-3A-UNVALIDATED.md) |
| `network.phase_datum_eq34_receiver` | Tier 3 (a) | [cards/config-ledger/LEDGER-TIER-3A-UNVALIDATED.md](cards/config-ledger/LEDGER-TIER-3A-UNVALIDATED.md) |
| `network.pivot_vanish_return_signal` | Tier 3 (a) | [cards/config-ledger/LEDGER-TIER-3A-UNVALIDATED.md](cards/config-ledger/LEDGER-TIER-3A-UNVALIDATED.md) |
| `network.raim_redundancy_normalize` | Tier 3 (b) | [cards/config-ledger/LEDGER-TIER-3B-RAIM-AND-HANDOVER.md](cards/config-ledger/LEDGER-TIER-3B-RAIM-AND-HANDOVER.md) |
| `network.raim_refloat_maturity_gate` | Tier 3 (b) | [cards/config-ledger/LEDGER-TIER-3B-RAIM-AND-HANDOVER.md](cards/config-ledger/LEDGER-TIER-3B-RAIM-AND-HANDOVER.md) |
| `network.raim_we_code_iono` | Tier 3 (b) | [cards/config-ledger/LEDGER-TIER-3B-RAIM-AND-HANDOVER.md](cards/config-ledger/LEDGER-TIER-3B-RAIM-AND-HANDOVER.md) |
| `network.scoped_handover_response`  (SCOPE the pivot-handover response to the constellation w… | Tier 3 (b) | [cards/config-ledger/LEDGER-TIER-3B-RAIM-AND-HANDOVER.md](cards/config-ledger/LEDGER-TIER-3B-RAIM-AND-HANDOVER.md) |
| `network.handover_stransform`  (queue 1(b) — RE-EXPRESS the changed constellation's carry at … | Tier 3 (b) | [cards/config-ledger/LEDGER-TIER-3B-RAIM-AND-HANDOVER.md](cards/config-ledger/LEDGER-TIER-3B-RAIM-AND-HANDOVER.md) |
| `network.sticky_pivot` | Tier 3 (b) | [cards/config-ledger/LEDGER-TIER-3B-RAIM-AND-HANDOVER.md](cards/config-ledger/LEDGER-TIER-3B-RAIM-AND-HANDOVER.md) |
| `network.trace_dd_fractional_epoch_stride` | Tier 3 (b) | [cards/config-ledger/LEDGER-TIER-3B-RAIM-AND-HANDOVER.md](cards/config-ledger/LEDGER-TIER-3B-RAIM-AND-HANDOVER.md) |
| `network.trace_dd_fractional_ref` | Tier 3 (b) | [cards/config-ledger/LEDGER-TIER-3B-RAIM-AND-HANDOVER.md](cards/config-ledger/LEDGER-TIER-3B-RAIM-AND-HANDOVER.md) |
| `network.warmup_epochs` | Tier 3 (b) | [cards/config-ledger/LEDGER-TIER-3B-RAIM-AND-HANDOVER.md](cards/config-ledger/LEDGER-TIER-3B-RAIM-AND-HANDOVER.md) |
| `network.warmup_ura_floor_m` | Tier 3 (b) | [cards/config-ledger/LEDGER-TIER-3B-RAIM-AND-HANDOVER.md](cards/config-ledger/LEDGER-TIER-3B-RAIM-AND-HANDOVER.md) |
| `network.phase_leveling_los_scale`  (F1 — the per-LOS B_eff variance scale on the leveling ca… | Tier 3 (c) | [cards/config-ledger/LEDGER-TIER-3C-LEVELING-CARRY.md](cards/config-ledger/LEDGER-TIER-3C-LEVELING-CARRY.md) |
| `network.phase_leveling_q_amb`  (the leveling-carry process noise — the levels' MEMORY) | Tier 3 (c) | [cards/config-ledger/LEDGER-TIER-3C-LEVELING-CARRY.md](cards/config-ledger/LEDGER-TIER-3C-LEVELING-CARRY.md) |
| `network.phase_leveling_var_floor_m2`  (the leveling-carry Λ cap / lock-in guard) | Tier 3 (c) | [cards/config-ledger/LEDGER-TIER-3C-LEVELING-CARRY.md](cards/config-ledger/LEDGER-TIER-3C-LEVELING-CARRY.md) |
| `network.carry_iono_in_block`  (+ `_q_m2`, `_max_gap_s`) | Tier 3 (c) | [cards/config-ledger/LEDGER-TIER-3C-LEVELING-CARRY.md](cards/config-ledger/LEDGER-TIER-3C-LEVELING-CARRY.md) |
| `network.phase_leveling_young_inflate_var_m2`  (+ `_tau_ep`, `_max_age_ep`) | Tier 3 (c) | [cards/config-ledger/LEDGER-TIER-3C-LEVELING-CARRY.md](cards/config-ledger/LEDGER-TIER-3C-LEVELING-CARRY.md) |
| `network.carry_rx_clock_in_block`  (+ `_q_m2`, `_max_gap_s`, `_rate_var0_m2ps2`, `_rate_q_m2p… | Tier 3 (c) | [cards/config-ledger/LEDGER-TIER-3C-LEVELING-CARRY.md](cards/config-ledger/LEDGER-TIER-3C-LEVELING-CARRY.md) |
| `network.raim_erase_band_scope`  (all \| min_keep_one) | Tier 3 (d) | [cards/config-ledger/LEDGER-TIER-3D-SLIP-RESPONSE.md](cards/config-ledger/LEDGER-TIER-3D-SLIP-RESPONSE.md) |
| `network.cycle_slip.anchor_slip_handover` | Tier 3 (d) | [cards/config-ledger/LEDGER-TIER-3D-SLIP-RESPONSE.md](cards/config-ledger/LEDGER-TIER-3D-SLIP-RESPONSE.md) |
| `network.cycle_slip.mw_evidence_refloat` | Tier 3 (d) | [cards/config-ledger/LEDGER-TIER-3D-SLIP-RESPONSE.md](cards/config-ledger/LEDGER-TIER-3D-SLIP-RESPONSE.md) |
| `network.cycle_slip.common_band_scope` | Tier 3 (e) | [cards/config-ledger/LEDGER-TIER-3E-BAND-SCOPE-AND-IFTD.md](cards/config-ledger/LEDGER-TIER-3E-BAND-SCOPE-AND-IFTD.md) |
| `network.cycle_slip.if_phase_td_pairs` | Tier 3 (e) | [cards/config-ledger/LEDGER-TIER-3E-BAND-SCOPE-AND-IFTD.md](cards/config-ledger/LEDGER-TIER-3E-BAND-SCOPE-AND-IFTD.md) |
| `network.cycle_slip.common_band_ref_arc` | Tier 3 (e) | [cards/config-ledger/LEDGER-TIER-3E-BAND-SCOPE-AND-IFTD.md](cards/config-ledger/LEDGER-TIER-3E-BAND-SCOPE-AND-IFTD.md) |
| `network.cycle_slip.if_phase_td_ref_arm_all` | Tier 3 (e) | [cards/config-ledger/LEDGER-TIER-3E-BAND-SCOPE-AND-IFTD.md](cards/config-ledger/LEDGER-TIER-3E-BAND-SCOPE-AND-IFTD.md) |
| `network.cycle_slip.refloat_band_scope`  (all \| implicated) | Tier 3 (f) | [cards/config-ledger/LEDGER-TIER-3F-ORACLE-AR-F3.md](cards/config-ledger/LEDGER-TIER-3F-ORACLE-AR-F3.md) |
| `network.cycle_slip.oracle_true_slip`  (`repair` \| `free`) | Tier 3 (f) | [cards/config-ledger/LEDGER-TIER-3F-ORACLE-AR-F3.md](cards/config-ledger/LEDGER-TIER-3F-ORACLE-AR-F3.md) |
| `network.raim_k_sigma`  (value knob, code default 6.0) | Tier 3 (f) | [cards/config-ledger/LEDGER-TIER-3F-ORACLE-AR-F3.md](cards/config-ledger/LEDGER-TIER-3F-ORACLE-AR-F3.md) |
| `network.ambiguity_resolution.trace`  (the Phase-2 producer-AR diagnostic's per-DD trace) | Tier 3 (f) | [cards/config-ledger/LEDGER-TIER-3F-ORACLE-AR-F3.md](cards/config-ledger/LEDGER-TIER-3F-ORACLE-AR-F3.md) |
| `network.phase_leveling_local_block`  (F3 — the station-wide jointly-whitened local ambiguity… | Tier 3 (f) | [cards/config-ledger/LEDGER-TIER-3F-ORACLE-AR-F3.md](cards/config-ledger/LEDGER-TIER-3F-ORACLE-AR-F3.md) |
| `network.phase_datum_handover_max_var_m2`  (the PB-gauge continuous-handover ledger ceiling —… | Tier 3 (f) | [cards/config-ledger/LEDGER-TIER-3F-ORACLE-AR-F3.md](cards/config-ledger/LEDGER-TIER-3F-ORACLE-AR-F3.md) |
| Tier 4 — KEEP_OFF  (diagnostic, falsified, or superseded) | Tier 4 | [cards/config-ledger/LEDGER-TIER-4-KEEP-OFF.md](cards/config-ledger/LEDGER-TIER-4-KEEP-OFF.md) |
| `estimation.adaptive_iono_process_noise.enable` | Tier 4 | [cards/config-ledger/LEDGER-TIER-4-KEEP-OFF.md](cards/config-ledger/LEDGER-TIER-4-KEEP-OFF.md) |
| `estimation.adaptive_iono_process_noise.trace` | Tier 4 | [cards/config-ledger/LEDGER-TIER-4-KEEP-OFF.md](cards/config-ledger/LEDGER-TIER-4-KEEP-OFF.md) |
| `estimation.estimate_ifb (standalone, network mode)` | Tier 4 | [cards/config-ledger/LEDGER-TIER-4-KEEP-OFF.md](cards/config-ledger/LEDGER-TIER-4-KEEP-OFF.md) |
| `estimation.global_vtec_sh.trace` | Tier 4 | [cards/config-ledger/LEDGER-TIER-4-KEEP-OFF.md](cards/config-ledger/LEDGER-TIER-4-KEEP-OFF.md) |
| `network.apriori_clock_anchor_sigma_m` | Tier 4 | [cards/config-ledger/LEDGER-TIER-4-KEEP-OFF.md](cards/config-ledger/LEDGER-TIER-4-KEEP-OFF.md) |
| `network.carry_coupling_joint_whiten` | Tier 4 | [cards/config-ledger/LEDGER-TIER-4-KEEP-OFF.md](cards/config-ledger/LEDGER-TIER-4-KEEP-OFF.md) |
| `network.carry_invariant_honesty` | Tier 4 | [cards/config-ledger/LEDGER-TIER-4-KEEP-OFF.md](cards/config-ledger/LEDGER-TIER-4-KEEP-OFF.md) |
| `network.carry_invariant_innov_refold` | Tier 4 | [cards/config-ledger/LEDGER-TIER-4-KEEP-OFF.md](cards/config-ledger/LEDGER-TIER-4-KEEP-OFF.md) |
| `network.carry_vce` | Tier 4 | [cards/config-ledger/LEDGER-TIER-4-KEEP-OFF.md](cards/config-ledger/LEDGER-TIER-4-KEEP-OFF.md) |
| `network.carry_vce_apply` | Tier 4 | [cards/config-ledger/LEDGER-TIER-4-KEEP-OFF.md](cards/config-ledger/LEDGER-TIER-4-KEEP-OFF.md) |
| `network.cb_excursion_reinit` | Tier 4 | [cards/config-ledger/LEDGER-TIER-4-KEEP-OFF.md](cards/config-ledger/LEDGER-TIER-4-KEEP-OFF.md) |
| `network.clock_disc_jump_test` | Tier 4 | [cards/config-ledger/LEDGER-TIER-4-KEEP-OFF.md](cards/config-ledger/LEDGER-TIER-4-KEEP-OFF.md) |
| `network.clock_disc_on_set_change` | Tier 4 | [cards/config-ledger/LEDGER-TIER-4-KEEP-OFF.md](cards/config-ledger/LEDGER-TIER-4-KEEP-OFF.md) |
| `network.clock_model (two-state oscillator carry)` | Tier 4 | [cards/config-ledger/LEDGER-TIER-4-KEEP-OFF.md](cards/config-ledger/LEDGER-TIER-4-KEEP-OFF.md) |
| `network.clock_model_q1` | Tier 4 | [cards/config-ledger/LEDGER-TIER-4-KEEP-OFF.md](cards/config-ledger/LEDGER-TIER-4-KEEP-OFF.md) |
| `network.clock_monitor_couple_raim` | Tier 4 | [cards/config-ledger/LEDGER-TIER-4-KEEP-OFF.md](cards/config-ledger/LEDGER-TIER-4-KEEP-OFF.md) |
| `network.clock_signal_basis_hold` | Tier 4 | [cards/config-ledger/LEDGER-TIER-4-KEEP-OFF.md](cards/config-ledger/LEDGER-TIER-4-KEEP-OFF.md) |
| `network.clock_temporal_anchor` (+ the clock_anchor_* family) | Tier 4 | [cards/config-ledger/LEDGER-TIER-4-KEEP-OFF.md](cards/config-ledger/LEDGER-TIER-4-KEEP-OFF.md) |
| `network.cycle_slip.gap_instrument` | Tier 4 | [cards/config-ledger/LEDGER-TIER-4-KEEP-OFF.md](cards/config-ledger/LEDGER-TIER-4-KEEP-OFF.md) |
| `network.cycle_slip.oracle_relevel`  (+ `_truth_root`, `_noise_cyc`, `_false_alarm`, `_window… | Tier 4 | [cards/config-ledger/LEDGER-TIER-4-KEEP-OFF.md](cards/config-ledger/LEDGER-TIER-4-KEEP-OFF.md) |
| `network.cycle_slip.turbo_edit` | Tier 4 | [cards/config-ledger/LEDGER-TIER-4-KEEP-OFF.md](cards/config-ledger/LEDGER-TIER-4-KEEP-OFF.md) |
| `network.emit_clock_rate` | Tier 4 | [cards/config-ledger/LEDGER-TIER-4-KEEP-OFF.md](cards/config-ledger/LEDGER-TIER-4-KEEP-OFF.md) |
| `network.emit_pb_gauge_level`  (+ `_alpha`, `_min_stations`, `_min_concentration`, `_trace`, … | Tier 4 (b) | [cards/config-ledger/LEDGER-TIER-4B-KEEP-OFF.md](cards/config-ledger/LEDGER-TIER-4B-KEEP-OFF.md) |
| `network.estimate_glonass` | Tier 4 (b) | [cards/config-ledger/LEDGER-TIER-4B-KEEP-OFF.md](cards/config-ledger/LEDGER-TIER-4B-KEEP-OFF.md) |
| `network.lane_carry.fold_blocked` | Tier 4 (b) | [cards/config-ledger/LEDGER-TIER-4B-KEEP-OFF.md](cards/config-ledger/LEDGER-TIER-4B-KEEP-OFF.md) |
| `network.parallel_reduce` | Tier 4 (b) | [cards/config-ledger/LEDGER-TIER-4B-KEEP-OFF.md](cards/config-ledger/LEDGER-TIER-4B-KEEP-OFF.md) |
| `network.phase_bias_exclude_unmatched_antenna` | Tier 4 (b) | [cards/config-ledger/LEDGER-TIER-4B-KEEP-OFF.md](cards/config-ledger/LEDGER-TIER-4B-KEEP-OFF.md) |
| `network.phase_datum_ensemble` | Tier 4 (b) | [cards/config-ledger/LEDGER-TIER-4B-KEEP-OFF.md](cards/config-ledger/LEDGER-TIER-4B-KEEP-OFF.md) |
| `network.phase_datum_eq34_carry_shift` | Tier 4 (b) | [cards/config-ledger/LEDGER-TIER-4B-KEEP-OFF.md](cards/config-ledger/LEDGER-TIER-4B-KEEP-OFF.md) |
| `network.phase_datum_eq34_longevity_election` | Tier 4 (b) | [cards/config-ledger/LEDGER-TIER-4B-KEEP-OFF.md](cards/config-ledger/LEDGER-TIER-4B-KEEP-OFF.md) |
| `network.phase_datum_hold_sigma_m` | Tier 4 (b) | [cards/config-ledger/LEDGER-TIER-4B-KEEP-OFF.md](cards/config-ledger/LEDGER-TIER-4B-KEEP-OFF.md) |
| `network.service_mode (the bundle that arms this family)` | Tier 4 (b) | [cards/config-ledger/LEDGER-TIER-4B-KEEP-OFF.md](cards/config-ledger/LEDGER-TIER-4B-KEEP-OFF.md) |
| `network.sh_gate_rms_max_tecu` | Tier 4 (b) | [cards/config-ledger/LEDGER-TIER-4B-KEEP-OFF.md](cards/config-ledger/LEDGER-TIER-4B-KEEP-OFF.md) |
| `network.sh_health_gate` | Tier 4 (b) | [cards/config-ledger/LEDGER-TIER-4B-KEEP-OFF.md](cards/config-ledger/LEDGER-TIER-4B-KEEP-OFF.md) |
| `network.state_snapshot_keep_history` | Tier 4 (b) | [cards/config-ledger/LEDGER-TIER-4B-KEEP-OFF.md](cards/config-ledger/LEDGER-TIER-4B-KEEP-OFF.md) |
| `network.state_snapshot_resume_shock_m` | Tier 4 (b) | [cards/config-ledger/LEDGER-TIER-4B-KEEP-OFF.md](cards/config-ledger/LEDGER-TIER-4B-KEEP-OFF.md) |
| `network.trace_postfit_keyed` | Tier 4 (b) | [cards/config-ledger/LEDGER-TIER-4B-KEEP-OFF.md](cards/config-ledger/LEDGER-TIER-4B-KEEP-OFF.md) |
| `processing.eigen_threads` | Tier 4 (b) | [cards/config-ledger/LEDGER-TIER-4B-KEEP-OFF.md](cards/config-ledger/LEDGER-TIER-4B-KEEP-OFF.md) |
| Cross-cutting notes from the audit | cross-cutting | [cards/config-ledger/LEDGER-CROSS-CUTTING-NOTES.md](cards/config-ledger/LEDGER-CROSS-CUTTING-NOTES.md) |
| A1 Step 0c — the load-time `parallel_carry` / `parallel_reduce` guard (LANDED) | A1 | [cards/config-ledger/LEDGER-A1-PCARRY-GUARD-AND-R-RESULTS.md](cards/config-ledger/LEDGER-A1-PCARRY-GUARD-AND-R-RESULTS.md) |
| A1 R1 — pb-gate-v2 gate G3 @300 s (two 11-ep smokes, pcarry ON both) — PASS | A1 | [cards/config-ledger/LEDGER-A1-PCARRY-GUARD-AND-R-RESULTS.md](cards/config-ledger/LEDGER-A1-PCARRY-GUARD-AND-R-RESULTS.md) |
| A1 R2 — `parallel_carry` equivalence control (OFF) + the P0b fold split — PASS | A1 | [cards/config-ledger/LEDGER-A1-PCARRY-GUARD-AND-R-RESULTS.md](cards/config-ledger/LEDGER-A1-PCARRY-GUARD-AND-R-RESULTS.md) |
| A1 R3 — pb-gate-v2 gates G4 / G5 @30 s (two 60-ep runs, pcarry ON both) — PASS | A1 | [cards/config-ledger/LEDGER-A1-PCARRY-GUARD-AND-R-RESULTS.md](cards/config-ledger/LEDGER-A1-PCARRY-GUARD-AND-R-RESULTS.md) |
| A1 R4 — the 300 s dev tier's D5 load, cold vs steady (`run_night_dev3h`) | A1 | [cards/config-ledger/LEDGER-A1-PCARRY-GUARD-AND-R-RESULTS.md](cards/config-ledger/LEDGER-A1-PCARRY-GUARD-AND-R-RESULTS.md) |
| A1 R5 — golden deciders, em25d15 vs em10d8 (3 h, 30 s, pcarry ON, gate_v2) | A1 | [cards/config-ledger/LEDGER-A1-PCARRY-GUARD-AND-R-RESULTS.md](cards/config-ledger/LEDGER-A1-PCARRY-GUARD-AND-R-RESULTS.md) |
| A1 R5b — em25d15 at 6 h: the hr-4 cliff readout | A1 | [cards/config-ledger/LEDGER-A1-PCARRY-GUARD-AND-R-RESULTS.md](cards/config-ledger/LEDGER-A1-PCARRY-GUARD-AND-R-RESULTS.md) |
| A1 R6 — the M5 no-refloat-arc discriminator (VERDICT) | A1 | [cards/config-ledger/LEDGER-A1-PCARRY-GUARD-AND-R-RESULTS.md](cards/config-ledger/LEDGER-A1-PCARRY-GUARD-AND-R-RESULTS.md) |
| A2 F0 / F4 — suite baselines | A2 | [cards/config-ledger/LEDGER-A2-SESSION-GATES.md](cards/config-ledger/LEDGER-A2-SESSION-GATES.md) |
| A2 F1 — orientation findings from existing artifacts (free) | A2 | [cards/config-ledger/LEDGER-A2-SESSION-GATES.md](cards/config-ledger/LEDGER-A2-SESSION-GATES.md) |
| A2 F2 — Card S2H: `network.rx_cb_harvest_demean` (knob contract) | A2 | [cards/config-ledger/LEDGER-A2-SESSION-GATES.md](cards/config-ledger/LEDGER-A2-SESSION-GATES.md) |
| A2 F3 — the H-heartbeat rider: `network.h_sigma0_prefit_census` (knob contract) | A2 | [cards/config-ledger/LEDGER-A2-SESSION-GATES.md](cards/config-ledger/LEDGER-A2-SESSION-GATES.md) |
| A2 F6 — Q4 CLOSED: the R-record consumer link, proven end-to-end | A2 | [cards/config-ledger/LEDGER-A2-SESSION-GATES.md](cards/config-ledger/LEDGER-A2-SESSION-GATES.md) |
| A2 F7 — 20-ep smoke verdicts (corrected posture) — ALL PASS | A2 | [cards/config-ledger/LEDGER-A2-SESSION-GATES.md](cards/config-ledger/LEDGER-A2-SESSION-GATES.md) |
| A2 F8 — Card S2H, 1 h stability gates — ALL FIVE PASS | A2 | [cards/config-ledger/LEDGER-A2-SESSION-GATES.md](cards/config-ledger/LEDGER-A2-SESSION-GATES.md) |
| A2 F9 — Card S2H, 3 h gates — ALL PASS | A2 | [cards/config-ledger/LEDGER-A2-SESSION-GATES.md](cards/config-ledger/LEDGER-A2-SESSION-GATES.md) |
| A2 F12 — Card S2H stage 3 (12 h): fold-side PASSES stronger; the level target is NOT met by E… | A2 | [cards/config-ledger/LEDGER-A2-SESSION-GATES.md](cards/config-ledger/LEDGER-A2-SESSION-GATES.md) |
| A2 F10 + F13 — Edge 2, `clock_datum_sum_to_zero`: wiring smoke then the 12 h jump-count A/B | A2 | [cards/config-ledger/LEDGER-A2-SESSION-GATES.md](cards/config-ledger/LEDGER-A2-SESSION-GATES.md) |
| A2 F14 — the ProductParser receiver-bias fix (unblocks item 0b, external receiver DCBs) | A2 | [cards/config-ledger/LEDGER-A2-SESSION-GATES.md](cards/config-ledger/LEDGER-A2-SESSION-GATES.md) |
| A2 F15 — THE INTERNAL (no-external) 12 h stability verdict | A2 | [cards/config-ledger/LEDGER-A2-SESSION-GATES.md](cards/config-ledger/LEDGER-A2-SESSION-GATES.md) |
| A2 F16 — the golden-config promotion shortlist, and what was APPLIED | A2 | [cards/config-ledger/LEDGER-A2-SESSION-GATES.md](cards/config-ledger/LEDGER-A2-SESSION-GATES.md) |
| A2 F11 — concurrent-workstream verification, and one process lesson | A2 | [cards/config-ledger/LEDGER-A2-SESSION-GATES.md](cards/config-ledger/LEDGER-A2-SESSION-GATES.md) |
| A2 — session gate results that certify its own binaries | A2 | [cards/config-ledger/LEDGER-A2-SESSION-GATES.md](cards/config-ledger/LEDGER-A2-SESSION-GATES.md) |
| A3 — the wu93 night (2026-08-21/22): wind-up fix landed; slip-response lane verdicts; golden … | A3 | [cards/config-ledger/LEDGER-A3-WU93-NIGHT.md](cards/config-ledger/LEDGER-A3-WU93-NIGHT.md) |
| A4 — THE GOLDEN CONFIGS (2026-09-05): the ledger made executable | A4 | [cards/config-ledger/LEDGER-A4-GOLDEN-CONFIGS.md](cards/config-ledger/LEDGER-A4-GOLDEN-CONFIGS.md) |
| A4.1 Smoke results (2026-09-05, Release exe of the F3 build, 20 epochs each) | A4 | [cards/config-ledger/LEDGER-A4-GOLDEN-CONFIGS.md](cards/config-ledger/LEDGER-A4-GOLDEN-CONFIGS.md) |
| A5 — the 2026-09-09 field session: the gap re-entry response, bridge v3, and the E1 probe | A5 | [cards/config-ledger/LEDGER-A5-FIELD-SESSION.md](cards/config-ledger/LEDGER-A5-FIELD-SESSION.md) |
