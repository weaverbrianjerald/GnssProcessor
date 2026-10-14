# docs/ index

**Generated — do not edit by hand.** Regenerate after adding or renaming a doc:

```
python scripts/gen_docs_index.py
```

80 documents, 21,986 lines.

**Columns.** `status` is read from each doc's own header markers (these docs declare their own state). `in` counts inbound citations from other docs, the agent memory store, code/scripts, and CLAUDE.md — it is the *load-bearing* number: a doc cited by many source files must not be renamed, merged or moved without updating its referrers, because code cites these by name and section (e.g. `// gotcha in SPEC_day129_frame_guards §6.`). A finished doc with `in` = 0 is an archive candidate for `docs/attic/`.

| Group | Docs | Lines |
|---|---:|---:|
| [Real-time / RTF](#real-time--rtf) | 3 | 1,086 |
| [Synthetic network / simulator](#synthetic-network--simulator) | 1 | 146 |
| [Ingest / RINEX](#ingest--rinex) | 1 | 235 |
| [Ionosphere / level](#ionosphere--level) | 10 | 2,967 |
| [Lanes / ambiguity / integer](#lanes--ambiguity--integer) | 9 | 1,808 |
| [Datum / S-system / clock](#datum--s-system--clock) | 15 | 5,431 |
| [Carry / covariance / VCE](#carry--covariance--vce) | 12 | 2,367 |
| [QC / RAIM / admission / slip](#qc--raim--admission--slip) | 10 | 1,826 |
| [Product / consumer / emit](#product--consumer--emit) | 1 | 176 |
| [Architecture / decomposition / perf](#architecture--decomposition--perf) | 4 | 1,156 |
| [Config / process / reference](#config--process--reference) | 6 | 2,190 |
| [Other](#other) | 8 | 2,598 |

## Real-time / RTF

| doc | status | in | lines | last commit |
|---|---|---:|---:|---|
| [rt_service_conventions](rt_service_conventions.md) | MIXED | 18 | 702 | 2026-08-15 |
| [rt_service_operations](rt_service_operations.md) | OPEN | 14 | 250 | 2026-08-15 |
| [rt_seam_decomposition](rt_seam_decomposition.md) | OPEN | 7 | 134 | 2026-08-15 |

## Synthetic network / simulator

| doc | status | in | lines | last commit |
|---|---|---:|---:|---|
| [SPEC_synthetic_minimum_network](SPEC_synthetic_minimum_network.md) | CLOSED | 179 | 146 | 2026-09-14 |

## Ingest / RINEX

| doc | status | in | lines | last commit |
|---|---|---:|---:|---|
| [SPEC_rinex_zero_sentinel](SPEC_rinex_zero_sentinel.md) | — | 9 | 235 | 2026-08-15 |

## Ionosphere / level

| doc | status | in | lines | last commit |
|---|---|---:|---:|---|
| [SPEC_amb_iono_split_overcount](SPEC_amb_iono_split_overcount.md) | — | 138 | 96 | 2026-09-14 |
| [SPEC_level_information_budget](SPEC_level_information_budget.md) | OPEN | 47 | 139 | 2026-09-14 |
| [SPEC_slip_relevel_walk](SPEC_slip_relevel_walk.md) | MIXED | 34 | 476 | 2026-08-21 |
| [SPEC_level_authority_internal](SPEC_level_authority_internal.md) | CLOSED | 29 | 421 | 2026-09-01 |
| [SPEC_iono_carry_8b](SPEC_iono_carry_8b.md) | CLOSED | 22 | 434 | 2026-08-21 |
| [SPEC_deweight_release_hygiene](SPEC_deweight_release_hygiene.md) | — | 21 | 267 | 2026-08-15 |
| [network_ambiguity_resolution_design](network_ambiguity_resolution_design.md) | MIXED | 18 | 500 | 2026-09-10 |
| [SPEC_iono2_mechB_v3](SPEC_iono2_mechB_v3.md) | — | 15 | 318 | 2026-08-15 |
| [QUEUE_principled_replacements](QUEUE_principled_replacements.md) | MIXED | 13 | 23 | 2026-09-14 |
| [network_global_sh_iono_design_2026-06-29](network_global_sh_iono_design_2026-06-29.md) | OPEN | 6 | 293 | 2026-08-15 |

## Lanes / ambiguity / integer

| doc | status | in | lines | last commit |
|---|---|---:|---:|---|
| [SPEC_lane_slip_response](SPEC_lane_slip_response.md) | MIXED | 38 | 32 | 2026-09-14 |
| [SPEC_sd_ewl_prereduce](SPEC_sd_ewl_prereduce.md) | OPEN | 23 | 337 | 2026-08-15 |
| [SPEC_ewl_wl_cascade_card](SPEC_ewl_wl_cascade_card.md) | MIXED | 21 | 300 | 2026-08-15 |
| [SPEC_lane_spectrometer](SPEC_lane_spectrometer.md) | MIXED | 20 | 290 | 2026-08-15 |
| [SPEC_mw_wl_card](SPEC_mw_wl_card.md) | MIXED | 19 | 212 | 2026-08-15 |
| [SPEC_lane_carry_card](SPEC_lane_carry_card.md) | MIXED | 17 | 217 | 2026-08-15 |
| [SPEC_uwl_card](SPEC_uwl_card.md) | MIXED | 17 | 116 | 2026-08-15 |
| [SPEC_eq34_reattach_hygiene](SPEC_eq34_reattach_hygiene.md) | MIXED | 16 | 118 | 2026-08-15 |
| [SPEC_eq34_seed_round](SPEC_eq34_seed_round.md) | MIXED | 16 | 186 | 2026-08-15 |

## Datum / S-system / clock

| doc | status | in | lines | last commit |
|---|---|---:|---:|---|
| [SPEC_pivot_visibility_gauge_defect](SPEC_pivot_visibility_gauge_defect.md) | MIXED | 273 | 53 | 2026-09-14 |
| [SPEC_clock_temporal_anchor](SPEC_clock_temporal_anchor.md) | MIXED | 47 | 57 | 2026-09-14 |
| [SPEC_day129_frame_guards](SPEC_day129_frame_guards.md) | CLOSED | 39 | 47 | 2026-09-14 |
| [SPEC_clock_emit_rebase](SPEC_clock_emit_rebase.md) | — | 30 | 237 | 2026-08-15 |
| [SPEC_family_gf_datum](SPEC_family_gf_datum.md) | MIXED | 18 | 690 | 2026-09-10 |
| [SPEC_sbasis_realization_contract](SPEC_sbasis_realization_contract.md) | — | 17 | 30 | 2026-09-14 |
| [SPEC_fast_clock_split_cadence](SPEC_fast_clock_split_cadence.md) | — | 16 | 651 | 2026-08-15 |
| [SPEC_datum_graph_census](SPEC_datum_graph_census.md) | MIXED | 15 | 241 | 2026-08-15 |
| [HANDOFF_edge_response_queue](HANDOFF_edge_response_queue.md) | MIXED | 14 | 550 | 2026-08-21 |
| [SPEC_beidou_w6_datum](SPEC_beidou_w6_datum.md) | MIXED | 11 | 312 | 2026-08-15 |
| [SPEC_cb_admission_card](SPEC_cb_admission_card.md) | CLOSED | 11 | 682 | 2026-08-15 |
| [SPEC_c1_clock_rate_validation](SPEC_c1_clock_rate_validation.md) | CLOSED | 10 | 690 | 2026-08-21 |
| [CLOCK_HADAMARD_CALIB_2026-07-14](CLOCK_HADAMARD_CALIB_2026-07-14.md) | OPEN | 8 | 360 | 2026-08-15 |
| [SPEC_inc8b_election_split](SPEC_inc8b_election_split.md) | MIXED | 8 | 574 | 2026-08-21 |
| [datum_transformation_reference_2026-08-07](datum_transformation_reference_2026-08-07.md) | — | 4 | 257 | 2026-08-15 |

## Carry / covariance / VCE

| doc | status | in | lines | last commit |
|---|---|---:|---:|---|
| [SPEC_pd_safe_dd_fold](SPEC_pd_safe_dd_fold.md) | CLOSED | 38 | 280 | 2026-08-15 |
| [SPEC_carry_aware_covariance](SPEC_carry_aware_covariance.md) | MIXED | 29 | 216 | 2026-08-15 |
| [SPEC_prior_residual_census](SPEC_prior_residual_census.md) | MIXED | 28 | 52 | 2026-09-14 |
| [SPEC_card1_coupling_carry](SPEC_card1_coupling_carry.md) | MIXED | 20 | 36 | 2026-09-14 |
| [SPEC_cb_carry_physical_q](SPEC_cb_carry_physical_q.md) | MIXED | 19 | 202 | 2026-08-15 |
| [SPEC_raim_pool_rehost](SPEC_raim_pool_rehost.md) | MIXED | 19 | 421 | 2026-09-13 |
| [SPEC_carry_vce_apply](SPEC_carry_vce_apply.md) | MIXED | 18 | 177 | 2026-10-08 |
| [SPEC_card4_invariant_carry](SPEC_card4_invariant_carry.md) | MIXED | 14 | 134 | 2026-08-15 |
| [SPEC_rx_cb_carry](SPEC_rx_cb_carry.md) | CLOSED | 13 | 61 | 2026-08-15 |
| [SPEC_carry_factored_pz_C2](SPEC_carry_factored_pz_C2.md) | — | 10 | 197 | 2026-08-15 |
| [SPEC_o2_carry_invariant_accumulator](SPEC_o2_carry_invariant_accumulator.md) | — | 9 | 432 | 2026-08-21 |
| [SPEC_warm_resume_event_card](SPEC_warm_resume_event_card.md) | MIXED | 6 | 159 | 2026-08-15 |

## QC / RAIM / admission / slip

| doc | status | in | lines | last commit |
|---|---|---:|---:|---|
| [SPEC_slip_qc_hardening](SPEC_slip_qc_hardening.md) | MIXED | 38 | 51 | 2026-09-14 |
| [SPEC_redundancy_census](SPEC_redundancy_census.md) | OPEN | 33 | 65 | 2026-09-14 |
| [SPEC_prefit_census_observability](SPEC_prefit_census_observability.md) | MIXED | 32 | 545 | 2026-09-01 |
| [SPEC_t3_null_derivation](SPEC_t3_null_derivation.md) | OPEN | 26 | 46 | 2026-09-14 |
| [SPEC_measurement_admission](SPEC_measurement_admission.md) | CLOSED | 22 | 53 | 2026-09-14 |
| [SPEC_d5_churn_limit_cycle](SPEC_d5_churn_limit_cycle.md) | MIXED | 15 | 391 | 2026-08-15 |
| [SPEC_flicker_gap_integrity](SPEC_flicker_gap_integrity.md) | OPEN | 12 | 284 | 2026-08-15 |
| [slip_comparison_report](slip_comparison_report.md) | MIXED | 8 | 93 | 2026-08-15 |
| [turbo_edit_lli_bridge_design](turbo_edit_lli_bridge_design.md) | OPEN | 7 | 43 | 2026-08-15 |
| [SPEC_noncausal_slip_bridge](SPEC_noncausal_slip_bridge.md) | MIXED | 5 | 255 | 2026-08-15 |

## Product / consumer / emit

| doc | status | in | lines | last commit |
|---|---|---:|---:|---|
| [SPEC_product_cbpin_contract](SPEC_product_cbpin_contract.md) | CLOSED | 8 | 176 | 2026-08-15 |

## Architecture / decomposition / perf

| doc | status | in | lines | last commit |
|---|---|---:|---:|---|
| [network_decomposition_plan](network_decomposition_plan.md) | MIXED | 25 | 378 | 2026-08-15 |
| [aiding_source_abstraction_design](aiding_source_abstraction_design.md) | CLOSED | 17 | 326 | 2026-08-15 |
| [SPEC_dev_tier_perf_cards](SPEC_dev_tier_perf_cards.md) | MIXED | 10 | 271 | 2026-08-15 |
| [HANDOFF_seam_frame_restore_2026-09-03](HANDOFF_seam_frame_restore_2026-09-03.md) | MIXED | 4 | 181 | 2026-09-06 |

## Config / process / reference

| doc | status | in | lines | last commit |
|---|---|---:|---:|---|
| [CONFIG_promotion_ledger](CONFIG_promotion_ledger.md) | CLOSED | 145 | 749 | 2026-10-13 |
| [RUNQUEUE_pdfix_reask](RUNQUEUE_pdfix_reask.md) | MIXED | 26 | 289 | 2026-08-15 |
| [test_suite](test_suite.md) | MIXED | 16 | 328 | 2026-08-15 |
| [CONFIG](CONFIG.md) | CLOSED | 11 | 183 | 2026-08-15 |
| [glonass_enablement_plan](glonass_enablement_plan.md) | MIXED | 9 | 384 | 2026-08-15 |
| [SPEC_latex_doc_refresh_2026-07-17](SPEC_latex_doc_refresh_2026-07-17.md) | MIXED | 4 | 257 | 2026-08-15 |

## Other

| doc | status | in | lines | last commit |
|---|---|---:|---:|---|
| [SPEC_event_harness](SPEC_event_harness.md) | — | 139 | 200 | 2026-09-14 |
| [SPEC_phase_windup_sign](SPEC_phase_windup_sign.md) | CLOSED | 35 | 48 | 2026-09-14 |
| [INDEX](INDEX.md) | — | 27 | 498 | 2026-10-13 |
| [SPEC_withhold_era_release](SPEC_withhold_era_release.md) | MIXED | 26 | 42 | 2026-09-14 |
| [SPEC_oracle_pin](SPEC_oracle_pin.md) | MIXED | 13 | 770 | 2026-09-11 |
| [OVERVIEW](OVERVIEW.md) | — | 4 | 350 | 2026-09-19 |
| [REORG_PLAN](REORG_PLAN.md) | MIXED | 4 | 413 | 2026-09-14 |
| [REORG_MAPPING](REORG_MAPPING.md) | MIXED | 3 | 277 | 2026-09-14 |

## attic/

Retired: work finished **and** zero inbound citations when archived. Moved, not deleted — full git history is intact, and `git log --follow` works across the move.

- [CONTRACT_level_authority.md](attic/CONTRACT_level_authority.md)
- [HANDOFF_card4_phaseD.md](attic/HANDOFF_card4_phaseD.md)
- [HANDOFF_carry_driver_perf.md](attic/HANDOFF_carry_driver_perf.md)
- [HANDOFF_carry_rigor_card1b.md](attic/HANDOFF_carry_rigor_card1b.md)
- [HANDOFF_cb_admission_card.md](attic/HANDOFF_cb_admission_card.md)
- [HANDOFF_clock_model_revival.md](attic/HANDOFF_clock_model_revival.md)
- [HANDOFF_ext_cb_iono_level.md](attic/HANDOFF_ext_cb_iono_level.md)
- [HANDOFF_iono2_card.md](attic/HANDOFF_iono2_card.md)
- [HANDOFF_lane_carry_card.md](attic/HANDOFF_lane_carry_card.md)
- [HANDOFF_mw_wl_card.md](attic/HANDOFF_mw_wl_card.md)
- [HANDOFF_rate_transferable_windows.md](attic/HANDOFF_rate_transferable_windows.md)
- [HANDOFF_selfar_producer_m5.md](attic/HANDOFF_selfar_producer_m5.md)
- [HANDOFF_slant_triple_next.md](attic/HANDOFF_slant_triple_next.md)
- [HANDOFF_wsh_chain_slant.md](attic/HANDOFF_wsh_chain_slant.md)
- [network_code_bias_m1_cc_scope_2026-06-29.md](attic/network_code_bias_m1_cc_scope_2026-06-29.md)
- [network_decomposition_addendum_2026-07-16.md](attic/network_decomposition_addendum_2026-07-16.md)
- [NIGHTRUN_REPORT_2026-07-15.md](attic/NIGHTRUN_REPORT_2026-07-15.md)
- [noncausal_smoother_ar_feedback_addendum.md](attic/noncausal_smoother_ar_feedback_addendum.md)
- [noncausal_smoother_jumpfit_scope.md](attic/noncausal_smoother_jumpfit_scope.md)
- [REPORT_fc1_split_cadence.md](attic/REPORT_fc1_split_cadence.md)
- [REPORT_mwwl_conditioning_day129.md](attic/REPORT_mwwl_conditioning_day129.md)
- [REPORT_night305_day129_investigation.md](attic/REPORT_night305_day129_investigation.md)
- [rtf12_brdc_rts_plan.md](attic/rtf12_brdc_rts_plan.md)
- [rtf_card8_runbook.md](attic/rtf_card8_runbook.md)
- [rtf_opus_first_handoff.md](attic/rtf_opus_first_handoff.md)
- [SESSION_overnight_2026-08-07.md](attic/SESSION_overnight_2026-08-07.md)
- [slip_qc_d5a_report_2026-07-22.md](attic/slip_qc_d5a_report_2026-07-22.md)
- [slip_qc_hardening_report_2026-07-21.md](attic/slip_qc_hardening_report_2026-07-21.md)
- [slip_repair_audit_2026-06-10.md](attic/slip_repair_audit_2026-06-10.md)
- [slip_repair_audit_2026-06-10_pass2_results.json](attic/slip_repair_audit_2026-06-10_pass2_results.json) — companion data for the audit above
- [SPEC_block_baarda.md](attic/SPEC_block_baarda.md)
- [SPEC_day129_gate_policy.md](attic/SPEC_day129_gate_policy.md)
- [SPEC_handover_stransform.md](attic/SPEC_handover_stransform.md)
- [SPEC_ingest_structural_guard.md](attic/SPEC_ingest_structural_guard.md)
- [SPEC_lane_fold_perf_card.md](attic/SPEC_lane_fold_perf_card.md)
- [SPEC_network_mdb_reliability.md](attic/SPEC_network_mdb_reliability.md)
- [SPEC_rtf3_streaming_frontend.md](attic/SPEC_rtf3_streaming_frontend.md)
- [SPEC_sd_ewl_integer_hold.md](attic/SPEC_sd_ewl_integer_hold.md)
- [SPEC_sigma0_constraint_aware.md](attic/SPEC_sigma0_constraint_aware.md)
- [SPEC_sim_injection_bench.md](attic/SPEC_sim_injection_bench.md)
- [SPEC_slip_qc_followups.md](attic/SPEC_slip_qc_followups.md)
- [SPEC_station_congruence.md](attic/SPEC_station_congruence.md)
- [ssystem_conformance_2026-06-15.md](attic/ssystem_conformance_2026-06-15.md)
- [ssystem_theory_reference_2026-07-09.md](attic/ssystem_theory_reference_2026-07-09.md)

