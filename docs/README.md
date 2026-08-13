# docs/ index

**Generated — do not edit by hand.** Regenerate after adding or renaming a doc:

```
python scripts/gen_docs_index.py
```

98 documents, 39,047 lines.

**Columns.** `status` is read from each doc's own header markers (these docs declare their own state). `in` counts inbound citations from other docs, the agent memory store, code/scripts, and CLAUDE.md — it is the *load-bearing* number: a doc cited by many source files must not be renamed, merged or moved without updating its referrers, because code cites these by name and section (e.g. `// gotcha in SPEC_day129_frame_guards §6.`). A finished doc with `in` = 0 is an archive candidate for `docs/attic/`.

| Group | Docs | Lines |
|---|---:|---:|
| [Real-time / RTF](#real-time--rtf) | 7 | 1,521 |
| [Synthetic network / simulator](#synthetic-network--simulator) | 3 | 3,213 |
| [Ingest / RINEX](#ingest--rinex) | 2 | 336 |
| [Ionosphere / level](#ionosphere--level) | 9 | 4,677 |
| [Lanes / ambiguity / integer](#lanes--ambiguity--integer) | 13 | 4,041 |
| [Datum / S-system / clock](#datum--s-system--clock) | 22 | 10,064 |
| [Carry / covariance / VCE](#carry--covariance--vce) | 13 | 4,223 |
| [QC / RAIM / admission / slip](#qc--raim--admission--slip) | 14 | 5,761 |
| [Product / consumer / emit](#product--consumer--emit) | 2 | 256 |
| [Architecture / decomposition / perf](#architecture--decomposition--perf) | 5 | 1,525 |
| [Field-run reports](#field-run-reports) | 1 | 276 |
| [Config / process / reference](#config--process--reference) | 7 | 3,154 |

## Real-time / RTF

| doc | status | in | lines | last commit |
|---|---|---:|---:|---|
| [rt_service_operations](rt_service_operations.md) | OPEN | 10 | 305 | 2026-07-28 |
| [rt_service_conventions](rt_service_conventions.md) | — | 9 | 98 | 2026-07-02 |
| [rtf12_brdc_rts_plan](rtf12_brdc_rts_plan.md) | CLOSED | 5 | 395 | 2026-07-02 |
| [rt_seam_decomposition](rt_seam_decomposition.md) | OPEN | 3 | 171 | 2026-08-03 |
| [rtf_card8_runbook](rtf_card8_runbook.md) | MIXED | 3 | 93 | 2026-07-05 |
| [SPEC_rtf3_streaming_frontend](SPEC_rtf3_streaming_frontend.md) | MIXED | 2 | 96 | 2026-08-03 |
| [rtf_opus_first_handoff](rtf_opus_first_handoff.md) | MIXED | 2 | 363 | 2026-07-02 |

## Synthetic network / simulator

| doc | status | in | lines | last commit |
|---|---|---:|---:|---|
| [SPEC_synthetic_minimum_network](SPEC_synthetic_minimum_network.md) | MIXED | 52 | 2497 | 2026-08-12 |
| [HANDOFF_wsh_chain_slant](HANDOFF_wsh_chain_slant.md) | MIXED | 13 | 274 | 2026-08-12 |
| [SPEC_sim_injection_bench](SPEC_sim_injection_bench.md) | MIXED | 0 | 442 | uncommitted |

## Ingest / RINEX

| doc | status | in | lines | last commit |
|---|---|---:|---:|---|
| [SPEC_rinex_zero_sentinel](SPEC_rinex_zero_sentinel.md) | CLOSED | 7 | 93 | 2026-08-03 |
| [SPEC_ingest_structural_guard](SPEC_ingest_structural_guard.md) | MIXED | 4 | 243 | 2026-08-03 |

## Ionosphere / level

| doc | status | in | lines | last commit |
|---|---|---:|---:|---|
| [SPEC_level_information_budget](SPEC_level_information_budget.md) | CLOSED | 43 | 2311 | 2026-08-07 |
| [SPEC_deweight_release_hygiene](SPEC_deweight_release_hygiene.md) | — | 25 | 347 | 2026-08-03 |
| [SPEC_iono2_mechB_v3](SPEC_iono2_mechB_v3.md) | OPEN | 10 | 146 | 2026-07-31 |
| [HANDOFF_slant_triple_next](HANDOFF_slant_triple_next.md) | MIXED | 9 | 246 | 2026-08-12 |
| [network_ambiguity_resolution_design](network_ambiguity_resolution_design.md) | MIXED | 8 | 368 | 2026-06-29 |
| [HANDOFF_ext_cb_iono_level](HANDOFF_ext_cb_iono_level.md) | MIXED | 7 | 512 | 2026-08-05 |
| [HANDOFF_iono2_card](HANDOFF_iono2_card.md) | — | 6 | 207 | 2026-07-22 |
| [CONTRACT_level_authority](CONTRACT_level_authority.md) | — | 3 | 260 | 2026-08-07 |
| [network_global_sh_iono_design_2026-06-29](network_global_sh_iono_design_2026-06-29.md) | — | 3 | 280 | 2026-06-30 |

## Lanes / ambiguity / integer

| doc | status | in | lines | last commit |
|---|---|---:|---:|---|
| [SPEC_sd_ewl_prereduce](SPEC_sd_ewl_prereduce.md) | — | 17 | 374 | 2026-07-31 |
| [SPEC_uwl_card](SPEC_uwl_card.md) | CLOSED | 17 | 309 | 2026-07-16 |
| [SPEC_eq34_seed_round](SPEC_eq34_seed_round.md) | MIXED | 16 | 337 | 2026-08-12 |
| [SPEC_ewl_wl_cascade_card](SPEC_ewl_wl_cascade_card.md) | OPEN | 16 | 447 | 2026-07-16 |
| [SPEC_lane_spectrometer](SPEC_lane_spectrometer.md) | MIXED | 16 | 697 | 2026-07-31 |
| [SPEC_eq34_reattach_hygiene](SPEC_eq34_reattach_hygiene.md) | OPEN | 15 | 324 | 2026-08-03 |
| [SPEC_lane_carry_card](SPEC_lane_carry_card.md) | MIXED | 14 | 187 | 2026-07-16 |
| [SPEC_mw_wl_card](SPEC_mw_wl_card.md) | CLOSED | 10 | 265 | 2026-07-16 |
| [SPEC_sd_ewl_integer_hold](SPEC_sd_ewl_integer_hold.md) | OPEN | 9 | 246 | 2026-07-31 |
| [SPEC_lane_fold_perf_card](SPEC_lane_fold_perf_card.md) | CLOSED | 6 | 162 | 2026-07-22 |
| [HANDOFF_mw_wl_card](HANDOFF_mw_wl_card.md) | MIXED | 5 | 246 | 2026-07-16 |
| [HANDOFF_lane_carry_card](HANDOFF_lane_carry_card.md) | MIXED | 4 | 126 | 2026-07-16 |
| [REPORT_mwwl_conditioning_day129](REPORT_mwwl_conditioning_day129.md) | CLOSED | 2 | 321 | 2026-07-29 |

## Datum / S-system / clock

| doc | status | in | lines | last commit |
|---|---|---:|---:|---|
| [SPEC_clock_temporal_anchor](SPEC_clock_temporal_anchor.md) | OPEN | 38 | 1623 | 2026-08-11 |
| [SPEC_day129_frame_guards](SPEC_day129_frame_guards.md) | CLOSED | 33 | 169 | 2026-07-28 |
| [SPEC_clock_emit_rebase](SPEC_clock_emit_rebase.md) | — | 25 | 251 | 2026-08-10 |
| [SPEC_family_gf_datum](SPEC_family_gf_datum.md) | MIXED | 17 | 801 | 2026-08-13 |
| [HANDOFF_edge_response_queue](HANDOFF_edge_response_queue.md) | MIXED | 15 | 644 | 2026-08-09 |
| [SPEC_datum_graph_census](SPEC_datum_graph_census.md) | MIXED | 14 | 256 | 2026-08-05 |
| [SPEC_fast_clock_split_cadence](SPEC_fast_clock_split_cadence.md) | — | 11 | 401 | 2026-07-28 |
| [SPEC_beidou_w6_datum](SPEC_beidou_w6_datum.md) | MIXED | 10 | 318 | 2026-07-22 |
| [SPEC_sbasis_realization_contract](SPEC_sbasis_realization_contract.md) | MIXED | 10 | 261 | 2026-08-13 |
| [SPEC_cb_admission_card](SPEC_cb_admission_card.md) | CLOSED | 8 | 384 | 2026-07-22 |
| [REPORT_night305_day129_investigation](REPORT_night305_day129_investigation.md) | OPEN | 6 | 245 | 2026-07-28 |
| [REPORT_fc1_split_cadence](REPORT_fc1_split_cadence.md) | — | 5 | 239 | 2026-07-31 |
| [HANDOFF_cb_admission_card](HANDOFF_cb_admission_card.md) | CLOSED | 4 | 476 | 2026-07-22 |
| [HANDOFF_clock_model_revival](HANDOFF_clock_model_revival.md) | CLOSED | 4 | 159 | 2026-07-16 |
| [SPEC_c1_clock_rate_validation](SPEC_c1_clock_rate_validation.md) | MIXED | 4 | 484 | 2026-07-28 |
| [SPEC_inc8b_election_split](SPEC_inc8b_election_split.md) | MIXED | 4 | 1054 | 2026-07-22 |
| [ssystem_conformance_2026-06-15](ssystem_conformance_2026-06-15.md) | OPEN | 4 | 339 | 2026-06-15 |
| [ssystem_theory_reference_2026-07-09](ssystem_theory_reference_2026-07-09.md) | OPEN | 4 | 367 | 2026-07-11 |
| [CLOCK_HADAMARD_CALIB_2026-07-14](CLOCK_HADAMARD_CALIB_2026-07-14.md) | — | 3 | 358 | 2026-07-16 |
| [SPEC_handover_stransform](SPEC_handover_stransform.md) | CLOSED | 3 | 187 | 2026-08-05 |
| [SPEC_day129_gate_policy](SPEC_day129_gate_policy.md) | CLOSED | 2 | 743 | 2026-07-28 |
| [datum_transformation_reference_2026-08-07](datum_transformation_reference_2026-08-07.md) | — | 2 | 305 | 2026-08-07 |

## Carry / covariance / VCE

| doc | status | in | lines | last commit |
|---|---|---:|---:|---|
| [SPEC_pd_safe_dd_fold](SPEC_pd_safe_dd_fold.md) | CLOSED | 35 | 607 | 2026-08-03 |
| [SPEC_prior_residual_census](SPEC_prior_residual_census.md) | CLOSED | 22 | 647 | 2026-08-05 |
| [SPEC_carry_vce_apply](SPEC_carry_vce_apply.md) | CLOSED | 18 | 313 | 2026-08-05 |
| [SPEC_carry_aware_covariance](SPEC_carry_aware_covariance.md) | CLOSED | 17 | 470 | 2026-08-03 |
| [SPEC_cb_carry_physical_q](SPEC_cb_carry_physical_q.md) | CLOSED | 14 | 411 | 2026-08-12 |
| [SPEC_raim_pool_rehost](SPEC_raim_pool_rehost.md) | MIXED | 14 | 383 | 2026-08-13 |
| [SPEC_rx_cb_carry](SPEC_rx_cb_carry.md) | CLOSED | 11 | 94 | 2026-07-31 |
| [HANDOFF_card4_invariant_carry](HANDOFF_card4_invariant_carry.md) | MIXED | 6 | 234 | 2026-07-11 |
| [HANDOFF_carry_driver_perf](HANDOFF_carry_driver_perf.md) | OPEN | 6 | 125 | 2026-07-16 |
| [SPEC_carry_factored_pz_C2](SPEC_carry_factored_pz_C2.md) | — | 4 | 241 | 2026-07-16 |
| [SPEC_warm_resume_event_card](SPEC_warm_resume_event_card.md) | MIXED | 4 | 363 | 2026-07-24 |
| [HANDOFF_carry_rigor_card1b](HANDOFF_carry_rigor_card1b.md) | — | 3 | 146 | 2026-07-11 |
| [HANDOFF_carry_timeupdate_card1d](HANDOFF_carry_timeupdate_card1d.md) | — | 3 | 189 | 2026-07-11 |

## QC / RAIM / admission / slip

| doc | status | in | lines | last commit |
|---|---|---:|---:|---|
| [SPEC_prefit_census_observability](SPEC_prefit_census_observability.md) | OPEN | 32 | 635 | 2026-08-03 |
| [SPEC_measurement_admission](SPEC_measurement_admission.md) | OPEN | 21 | 1379 | 2026-08-03 |
| [SPEC_sigma0_constraint_aware](SPEC_sigma0_constraint_aware.md) | — | 20 | 266 | 2026-08-05 |
| [SPEC_network_mdb_reliability](SPEC_network_mdb_reliability.md) | — | 19 | 347 | 2026-08-05 |
| [SPEC_redundancy_census](SPEC_redundancy_census.md) | — | 19 | 268 | 2026-08-03 |
| [SPEC_station_congruence](SPEC_station_congruence.md) | OPEN | 19 | 287 | 2026-08-05 |
| [SPEC_block_baarda](SPEC_block_baarda.md) | — | 18 | 262 | 2026-08-05 |
| [SPEC_slip_qc_hardening](SPEC_slip_qc_hardening.md) | — | 16 | 333 | 2026-07-22 |
| [SPEC_flicker_gap_integrity](SPEC_flicker_gap_integrity.md) | OPEN | 13 | 433 | 2026-08-03 |
| [SPEC_slip_qc_followups](SPEC_slip_qc_followups.md) | MIXED | 8 | 238 | 2026-07-22 |
| [SPEC_d5_churn_limit_cycle](SPEC_d5_churn_limit_cycle.md) | MIXED | 6 | 491 | 2026-08-03 |
| [slip_qc_d5a_report_2026-07-22](slip_qc_d5a_report_2026-07-22.md) | CLOSED | 5 | 434 | 2026-07-22 |
| [slip_qc_hardening_report_2026-07-21](slip_qc_hardening_report_2026-07-21.md) | CLOSED | 4 | 330 | 2026-07-22 |
| [turbo_edit_lli_bridge_design](turbo_edit_lli_bridge_design.md) | — | 4 | 58 | 2026-06-15 |

## Product / consumer / emit

| doc | status | in | lines | last commit |
|---|---|---:|---:|---|
| [SPEC_product_cbpin_contract](SPEC_product_cbpin_contract.md) | CLOSED | 7 | 200 | 2026-08-13 |
| [HANDOFF_card4_phaseD](HANDOFF_card4_phaseD.md) | — | 4 | 56 | 2026-07-12 |

## Architecture / decomposition / perf

| doc | status | in | lines | last commit |
|---|---|---:|---:|---|
| [network_decomposition_addendum_2026-07-16](network_decomposition_addendum_2026-07-16.md) | CLOSED | 17 | 204 | 2026-07-22 |
| [aiding_source_abstraction_design](aiding_source_abstraction_design.md) | OPEN | 16 | 645 | 2026-06-15 |
| [network_decomposition_plan](network_decomposition_plan.md) | MIXED | 10 | 362 | 2026-07-17 |
| [HANDOFF_perlos_parallel_sweeps](HANDOFF_perlos_parallel_sweeps.md) | OPEN | 7 | 227 | 2026-07-16 |
| [HANDOFF_rate_transferable_windows](HANDOFF_rate_transferable_windows.md) | CLOSED | 2 | 87 | 2026-07-16 |

## Field-run reports

| doc | status | in | lines | last commit |
|---|---|---:|---:|---|
| [NIGHTRUN_REPORT_2026-07-15](NIGHTRUN_REPORT_2026-07-15.md) | MIXED | 6 | 276 | 2026-07-16 |

## Config / process / reference

| doc | status | in | lines | last commit |
|---|---|---:|---:|---|
| [RUNQUEUE_pdfix_reask](RUNQUEUE_pdfix_reask.md) | MIXED | 19 | 306 | 2026-08-03 |
| [test_suite](test_suite.md) | — | 14 | 369 | 2026-06-28 |
| [SESSION_overnight_2026-08-07](SESSION_overnight_2026-08-07.md) | MIXED | 9 | 470 | 2026-08-09 |
| [CONFIG_promotion_ledger](CONFIG_promotion_ledger.md) | CLOSED | 8 | 1118 | 2026-08-11 |
| [CONFIG](CONFIG.md) | — | 7 | 195 | 2026-07-02 |
| [SPEC_latex_doc_refresh_2026-07-17](SPEC_latex_doc_refresh_2026-07-17.md) | MIXED | 4 | 270 | 2026-07-22 |
| [glonass_enablement_plan](glonass_enablement_plan.md) | CLOSED | 3 | 426 | 2026-07-02 |

## attic/

Retired: work finished **and** zero inbound citations when archived. Moved, not deleted — full git history is intact, and `git log --follow` works across the move.

- [HANDOFF_selfar_producer_m5.md](attic/HANDOFF_selfar_producer_m5.md)
- [network_code_bias_m1_cc_scope_2026-06-29.md](attic/network_code_bias_m1_cc_scope_2026-06-29.md)
- [slip_repair_audit_2026-06-10.md](attic/slip_repair_audit_2026-06-10.md)
- [slip_repair_audit_2026-06-10_pass2_results.json](attic/slip_repair_audit_2026-06-10_pass2_results.json) — companion data for the audit above

