# Record compression, 2026-10: memory directory, per file

Per-file sizes of pass 3 of [the register](RECORD_COMPRESSION_2026-10.md): the agent memory directory, before (the backup of 2026-10-24) and after, in LF bytes. "handled by" names the stage: the manager (first pass), wave 1 (G1 to G9, by topic) or wave 2 (S1 to S5, second cut). A merged memory is gone as a file; its name is listed in the `Formerly separate memories:` line of the file it was merged into, and the verdicts and traps moved with it. "New digest" is a file created by the pass. No memory was deleted outright.

| memory | handled by | before (B) | after (B) | result |
| --- | --- | ---: | ---: | --- |
| `archive_lessons_carry_fold` | wave 1 G8 | 12,130 | 3,255 | kept |
| `archive_lessons_datum_estimation` | wave 1 G8 | 15,935 | 3,697 | kept |
| `archive_lessons_field_grading` | wave 1 G8 | 9,352 | 3,266 | kept |
| `archive_lessons_iono_lanes` | wave 1 G8 | 12,782 | 3,464 | kept |
| `archive_lessons_perf_infra` | wave 1 G8 | 14,169 | 3,990 | kept |
| `archive_lessons_qc_slip_field` | wave 1 G9 | 18,345 | 3,470 | kept |
| `archive_lessons_raim_sigma0` | wave 1 G9 | 12,188 | 3,447 | kept |
| `archive_lessons_resume_readmit` | wave 1 G9 | 9,113 | 3,465 | kept |
| `archive_lessons_run_harness` | wave 1 G9 | 8,398 | 3,224 | kept |
| `archive_lessons_validation_events` | wave 1 G9 | 12,005 | 3,356 | kept |
| `feedback_agent_worktree_stale_base` | first pass | 1,609 | - | merged into `feedback_run_environment_traps` |
| `feedback_allparallel_release_runs` | wave 2 S4 | 1,742 | 1,359 | kept |
| `feedback_analysis_and_tooling_traps` | first pass | - | 3,926 | new digest |
| `feedback_backslash_via_edit_tool` | first pass | 1,085 | - | merged into `feedback_analysis_and_tooling_traps` |
| `feedback_branch_exe_unknown_knob_silent` | first pass | 1,646 | - | merged into `feedback_run_environment_traps` |
| `feedback_build_wrapper_hardcoded_project` | first pass | 1,519 | - | merged into `feedback_run_environment_traps` |
| `feedback_c_drive_space_policy` | wave 2 S4 | 2,295 | 1,489 | kept |
| `feedback_census_replay_needs_pbf` | first pass | 1,456 | - | merged into `feedback_analysis_and_tooling_traps` |
| `feedback_codebase_readable_by_newcomers` | wave 2 S5 | 1,895 | 1,613 | kept |
| `feedback_comments_no_card_refs` | wave 2 S4 | 952 | 932 | kept |
| `feedback_concurrent_build_locks` | first pass | 3,529 | - | merged into `feedback_run_environment_traps` |
| `feedback_copyitem_mtime_stale_obj` | first pass | 1,725 | - | merged into `feedback_run_environment_traps` |
| `feedback_correctness_over_expedient` | wave 2 S4 | 1,517 | 1,277 | kept |
| `feedback_delegate_to_sonnet` | wave 2 S5 | 1,534 | 1,483 | kept |
| `feedback_depth_over_breadth_ultracode` | wave 2 S4 | 1,635 | 1,217 | kept |
| `feedback_detached_run_launchers` | first pass | 2,353 | - | merged into `feedback_run_environment_traps` |
| `feedback_docs_topic_tree_not_monoliths` | wave 2 S5 | 2,282 | 1,639 | kept |
| `feedback_event_harness_over_daylong_runs` | wave 2 S5 | 2,641 | 1,639 | kept |
| `feedback_golden_configs_first` | wave 2 S5 | 2,351 | 1,632 | kept |
| `feedback_harness_defaults_shared_state` | first pass | 1,638 | - | merged into `feedback_analysis_and_tooling_traps` |
| `feedback_l5_never_abandoned_sequence_not_close` | wave 2 S5 | 2,285 | 1,627 | kept |
| `feedback_lag_attribution_base_rate` | first pass | 1,751 | - | merged into `feedback_analysis_and_tooling_traps` |
| `feedback_lean_review_style` | wave 2 S5 | 2,062 | 1,648 | kept |
| `feedback_manager_worker_pattern` | wave 2 S5 | 2,546 | 1,645 | kept |
| `feedback_matlab_viz_colormaps` | first pass | 1,392 | - | merged into `feedback_viz_defaults` |
| `feedback_multi_site_replay_before_adoption` | wave 2 S5 | 2,544 | 1,646 | kept |
| `feedback_mw_thresholds_empirical` | first pass | 3,608 | - | merged into `feedback_slip_detector_rules` |
| `feedback_network_nonnegotiable_configs` | wave 2 S4 | 2,575 | 1,475 | kept |
| `feedback_never_run_debug` | wave 2 S5 | 1,275 | 1,311 | kept |
| `feedback_no_coauthor_trailer` | wave 2 S4 | 1,618 | 1,248 | kept |
| `feedback_no_external_data_dependence` | wave 2 S4 | 2,545 | 1,764 | kept |
| `feedback_no_knob_promotion_step` | wave 2 S4 | 4,575 | 1,790 | kept |
| `feedback_one_pass_not_two` | wave 2 S4 | 1,830 | 1,300 | kept |
| `feedback_plain_language_summaries` | wave 2 S4 | 1,753 | 1,215 | kept |
| `feedback_plain_vocabulary_reader_docs` | wave 2 S5 | 1,985 | 1,626 | kept |
| `feedback_prefer_fallback_over_drop` | wave 2 S4 | 1,437 | 1,322 | kept |
| `feedback_prose_style_no_emdash_impersonal` | wave 2 S5 | 1,649 | 1,614 | kept |
| `feedback_raim_always_on` | wave 2 S4 | 2,886 | 1,490 | kept |
| `feedback_rapid_mode_2h_checks` | wave 2 S5 | 1,980 | 1,634 | kept |
| `feedback_rapid_two_bed_bisection` | wave 2 S5 | 2,765 | 1,649 | kept |
| `feedback_read_armed_instrument_tables` | wave 2 S5 | 1,717 | 1,637 | kept |
| `feedback_reentry_response_keys_on_carry` | wave 2 S4 | 2,319 | 1,469 | kept |
| `feedback_reset_over_repair_biasless` | first pass | 1,516 | - | merged into `feedback_slip_detector_rules` |
| `feedback_run_environment_traps` | first pass | - | 3,901 | new digest |
| `feedback_satstatus_viz_default` | first pass | 1,238 | - | merged into `feedback_viz_defaults` |
| `feedback_sed_strips_crlf` | first pass | 1,076 | - | merged into `feedback_analysis_and_tooling_traps` |
| `feedback_slip_detector_rules` | first pass | - | 3,408 | new digest |
| `feedback_slip_detectors_by_rate` | first pass | 2,615 | - | merged into `feedback_slip_detector_rules` |
| `feedback_stage_own_hunk_shared_file` | first pass | 2,582 | - | merged into `feedback_run_environment_traps` |
| `feedback_stale_trace_dir_fake_ab` | first pass | 1,676 | - | merged into `feedback_analysis_and_tooling_traps` |
| `feedback_structure_over_byte_identity` | wave 2 S5 | 1,689 | 1,631 | kept |
| `feedback_subcycle_suppression_hurts_position` | first pass | 1,647 | - | merged into `feedback_slip_detector_rules` |
| `feedback_subdm_count_is_threshold_on_standing_offset` | wave 2 S5 | 1,870 | 1,639 | kept |
| `feedback_test_cadence_smoke_first` | wave 2 S4 | 2,764 | 1,765 | kept |
| `feedback_verify_symbol_absence_two_tools` | first pass | 2,246 | - | merged into `feedback_analysis_and_tooling_traps` |
| `feedback_verify_uncommitted_resolved` | first pass | 1,747 | - | merged into `feedback_analysis_and_tooling_traps` |
| `feedback_viz_defaults` | first pass | - | 1,325 | new digest |
| `feedback_wer_zombie_wedge` | first pass | 1,668 | - | merged into `feedback_run_environment_traps` |
| `feedback_wrapped_fraction_null_traps` | first pass | 2,979 | - | merged into `feedback_analysis_and_tooling_traps` |
| `project_adaptive_iono_q_network` | first pass | 2,531 | - | merged into `project_network_falsified_approaches` |
| `project_agu26_abstract_campaign` | wave 2 S2 | 3,656 | - | merged into `project_closed_campaigns_pre_refounding` |
| `project_amb_iono_split_overcount` | wave 2 S1 | 3,277 | - | merged into `project_why_non_integer_rootcause` |
| `project_antenna_eccentricity_arp_frame` | wave 1 G3 | 3,708 | 2,467 | kept |
| `project_ar_validation_criterion` | wave 1 G2 | 3,319 | 1,265 | kept |
| `project_arch_opportunity_backlog` | wave 1 G4 | 4,302 | 2,636 | kept |
| `project_arl_integer_ladder` | wave 2 S2 | 3,227 | 3,770 | kept |
| `project_atx_radome_fallback_card` | wave 1 G6 | 2,628 | 1,106 | kept |
| `project_baseline_requires_sp3` | wave 1 G6 | 6,316 | 2,032 | kept |
| `project_beidou_network_clock_divergence` | wave 1 G3 | 6,382 | 1,387 | kept |
| `project_block_baarda_card` | first pass | 3,271 | - | merged into `project_network_qc_tiers_lessons` |
| `project_border_clone_bcl` | first pass | 2,134 | - | merged into `project_clock_model_hadamard_calib` |
| `project_carry_datum_handover_theory` | wave 2 S1 | 4,657 | 3,792 | kept |
| `project_clock_level_consistent_recursion` | wave 2 S2 | 6,371 | 3,431 | kept |
| `project_clock_model_hadamard_calib` | wave 2 S3 | 4,000 | 3,710 | kept |
| `project_clock_temporal_anchor_q3` | wave 2 S2 | 5,010 | - | merged into `project_clock_level_consistent_recursion` |
| `project_closed_campaigns_pre_refounding` | first pass | - | 3,746 | new digest |
| `project_closedloop_consumer_vertical_bug` | wave 1 G3 | 6,157 | 1,296 | kept |
| `project_codeonly_pivot_iono_scrub_gap` | wave 1 G1 | 4,671 | - | merged into `project_pivot_visibility_gauge_defect` |
| `project_colleague_deck_outline` | wave 2 S2 | 3,043 | - | merged into `project_housekeeping_decks_comments_cleanup` |
| `project_comment_register_rewrite` | wave 2 S2 | 1,430 | - | merged into `project_housekeeping_decks_comments_cleanup` |
| `project_concurrent_run_log_collision` | wave 1 G4 | 1,906 | - | merged into `project_object_code_identity_proof` |
| `project_config_inheritance_consolidation` | wave 1 G6 | 4,598 | - | merged into `project_config_toggle_gotchas` |
| `project_config_toggle_gotchas` | wave 1 G6 | 3,710 | 2,526 | kept |
| `project_consumer_gap_integrity` | wave 1 G3 | 3,280 | 2,289 | kept |
| `project_consumer_instrument_card` | wave 2 S2 | 5,044 | - | merged into `project_closed_campaigns_pre_refounding` |
| `project_consumer_replay_driver` | wave 1 G6 | 5,062 | 1,769 | kept |
| `project_cv_slant_iono` | wave 1 G5 | 2,966 | - | merged into `project_rt_service_work_queue` |
| `project_day129_pivot_cb_ratchet` | wave 2 S1 | 5,193 | 2,545 | kept |
| `project_default_sample_configs` | wave 1 G6 | 1,807 | - | merged into `project_config_toggle_gotchas` |
| `project_dev300s_tier` | wave 1 G4 | 3,804 | 1,300 | kept |
| `project_event_harness` | wave 1 G6 | 5,795 | 1,210 | kept |
| `project_ewl_wl_card_impl` | wave 1 G2 | 3,602 | - | merged into `project_mwwl_conditioning_day129` |
| `project_extproduct_ar_probe` | wave 1 G2 | 2,697 | 1,373 | kept |
| `project_ft63_fast_tier` | wave 1 G4 | 4,473 | 1,266 | kept |
| `project_galileo_c5c7_code_bias` | wave 1 G5 | 4,267 | - | merged into `project_realtime_igs_rtcm_stream` |
| `project_global_sh_iono` | wave 1 G3 | 4,468 | 1,244 | kept |
| `project_glonass_ifb_network` | wave 1 G5 | 5,072 | 1,273 | kept |
| `project_golden0729_3h_validation` | first pass | 2,530 | - | merged into `project_network_qc_tiers_lessons` |
| `project_housekeeping_decks_comments_cleanup` | first pass | - | 2,838 | new digest |
| `project_ignition_level_redraw_noise` | wave 2 S2 | 2,392 | - | merged into `project_sim_fixture_start_artifact` |
| `project_iono2_card` | wave 2 S1 | 2,760 | - | merged into `project_day129_pivot_cb_ratchet` |
| `project_iono_ar_review_jun2026` | wave 1 G2 | 5,064 | - | merged into `project_ppp_ar_standard` |
| `project_l5g_weekend_galileo_l5` | first pass | 10,501 | 3,112 | kept |
| `project_lane_slip_response` | wave 2 S1 | 3,815 | - | merged into `project_network_if_td_slip` |
| `project_laser_theory_mapping` | wave 1 G7 | 8,029 | 2,788 | kept |
| `project_latex_math_reference` | wave 1 G7 | 6,037 | - | merged into `project_network_logviz_implementation` |
| `project_level_information_budget` | wave 1 G1 | 4,723 | 1,276 | kept |
| `project_lgm_phase_colour_state` | wave 2 S1 | 4,842 | - | merged into `project_carry_datum_handover_theory` |
| `project_logmanager_initialize_nothrow` | wave 1 G7 | 2,416 | - | merged into `project_spdlog_free_function_hazard` |
| `project_m2_lsvce_carryq` | first pass | 3,976 | - | merged into `project_network_qc_tiers_lessons` |
| `project_mpages_enhancement_backlog` | wave 1 G7 | 10,976 | 3,031 | kept |
| `project_multiday_replay_mdr` | wave 2 S2 | 2,771 | 1,680 | kept |
| `project_multignss_signal_priorities_drop` | wave 1 G5 | 2,150 | 1,212 | kept |
| `project_mw_reset_pivot_handover` | wave 1 G1 | 3,437 | - | merged into `project_pivot_visibility_gauge_defect` |
| `project_mwwl_conditioning_day129` | wave 1 G2 | 3,810 | 2,918 | kept |
| `project_nav_readiness_enhancements` | wave 1 G6 | 3,302 | 906 | kept |
| `project_net305_attribution_rerun` | wave 1 G4 | 4,467 | 1,367 | kept |
| `project_network_ar_producer_scope` | wave 2 S2 | 4,924 | - | merged into `project_arl_integer_ladder` |
| `project_network_atmosphere_readiness` | wave 1 G1 | 2,801 | - | merged into `project_ppp_rtk_network` |
| `project_network_clock_rate_c1` | first pass | 3,092 | - | merged into `project_clock_model_hadamard_calib` |
| `project_network_code_bias_scope` | wave 1 G1 | 5,094 | 3,784 | kept |
| `project_network_ddfrac_rootcause_shvtec` | wave 1 G2 | 4,594 | 1,291 | kept |
| `project_network_decomposition_plan` | wave 1 G4 | 4,336 | 1,198 | kept |
| `project_network_enhancement_queue` | wave 1 G3 | 1,318 | 701 | kept |
| `project_network_falsified_approaches` | wave 2 S3 | 7,498 | 3,148 | kept |
| `project_network_fullcov_carry` | wave 1 G4 | 4,786 | 1,284 | kept |
| `project_network_if_td_slip` | wave 2 S1 | 6,833 | 2,897 | kept |
| `project_network_kf_vs_information_form` | wave 2 S1 | 3,891 | - | merged into `project_carry_datum_handover_theory` |
| `project_network_logviz_implementation` | wave 1 G7 | 3,116 | 2,862 | kept |
| `project_network_object_model_nom` | first pass | 28,533 | 3,559 | kept |
| `project_network_phase_leveling` | wave 1 G2 | 4,743 | 1,280 | kept |
| `project_network_qc_tiers_lessons` | first pass | - | 3,443 | new digest |
| `project_network_refounding` | first pass | 11,447 | 5,002 | kept |
| `project_network_ssystem_dia_assessment` | wave 2 S1 | 7,848 | 3,689 | kept |
| `project_network_xlin_invariance` | wave 2 S1 | 3,307 | 1,549 | kept |
| `project_ngs_laser_lcf` | wave 1 G7 | 1,987 | - | merged into `project_sif_integration` |
| `project_nl_wl_basis` | wave 1 G2 | 3,229 | - | merged into `project_ppp_ar_standard` |
| `project_noncausal_smoother_jumpfit` | wave 1 G3 | 2,531 | 1,252 | kept |
| `project_object_code_identity_proof` | wave 1 G4 | 2,440 | 1,692 | kept |
| `project_optimization_state` | wave 1 G4 | 6,126 | - | merged into `project_arch_opportunity_backlog` |
| `project_pb_gate_v2` | wave 1 G3 | 3,137 | 1,152 | kept |
| `project_perf_deterministic_parallel_seam` | wave 1 G4 | 5,052 | 1,367 | kept |
| `project_phase_windup_sign_defect` | wave 1 G3 | 4,624 | 1,266 | kept |
| `project_pivot_frame_process_noise` | wave 2 S3 | 2,113 | - | merged into `project_clock_model_hadamard_calib` |
| `project_pivot_handover_flipflop_fix` | wave 1 G1 | 4,295 | - | merged into `project_pivot_visibility_gauge_defect` |
| `project_pivot_visibility_gauge_defect` | wave 1 G1 | 6,045 | 3,871 | kept |
| `project_ppp_ar_standard` | wave 1 G2 | 3,044 | 3,573 | kept |
| `project_ppp_rtk_network` | wave 1 G1 | 5,074 | 3,760 | kept |
| `project_pppar_user_control` | wave 2 S2 | 5,292 | - | merged into `project_arl_integer_ladder` |
| `project_ppprtk_constellation_suite` | wave 1 G1 | 2,143 | - | merged into `project_ppp_rtk_network` |
| `project_product_legend_schema_fix` | wave 1 G1 | 3,008 | 1,282 | kept |
| `project_pyviz_python_port` | wave 1 G7 | 3,839 | - | merged into `project_network_logviz_implementation` |
| `project_queued_mpages_v12_builds` | wave 2 S2 | 4,861 | 2,946 | kept |
| `project_raim_pool_rehost` | first pass | 3,571 | - | merged into `project_network_qc_tiers_lessons` |
| `project_realdata_test_suite` | wave 1 G6 | 3,946 | 1,675 | kept |
| `project_realtime_asterx_ppprtk` | wave 1 G5 | 8,699 | 2,436 | kept |
| `project_realtime_bssd_epoch_blowup` | wave 1 G5 | 4,883 | - | merged into `project_realtime_igs_rtcm_stream` |
| `project_realtime_glonass` | wave 1 G5 | 2,371 | 1,222 | kept |
| `project_realtime_igs_rtcm_stream` | wave 1 G5 | 5,270 | 3,851 | kept |
| `project_realtime_model_degradation` | wave 1 G5 | 3,065 | - | merged into `project_realtime_igs_rtcm_stream` |
| `project_remove_keys_deletion` | wave 2 S2 | 2,339 | - | merged into `project_queued_mpages_v12_builds` |
| `project_rinex_phase_shift_convention` | wave 1 G3 | 2,106 | - | merged into `project_antenna_eccentricity_arp_frame` |
| `project_root_cleanup_onboarding_2026-09-19` | wave 2 S2 | 2,702 | - | merged into `project_housekeeping_decks_comments_cleanup` |
| `project_rt_report_surveyed_reference` | wave 1 G5 | 4,236 | 1,315 | kept |
| `project_rt_service_work_queue` | wave 1 G5 | 4,944 | 2,025 | kept |
| `project_rx_cb_carry_card` | wave 1 G1 | 4,170 | - | merged into `project_network_code_bias_scope` |
| `project_rxcb_datum_gate_review` | wave 1 G1 | 2,484 | - | merged into `project_network_code_bias_scope` |
| `project_scenario_matrix_and_independence_2026-10-24` | first pass | 4,746 | 5,041 | kept |
| `project_scl_self_contained_clock_level` | wave 2 S2 | 4,948 | 1,656 | kept |
| `project_sif_integration` | wave 1 G7 | 7,778 | 2,974 | kept |
| `project_sim_fixture_start_artifact` | wave 2 S2 | 2,809 | 3,677 | kept |
| `project_sim_injection_bench` | wave 2 S2 | 4,997 | - | merged into `project_sim_fixture_start_artifact` |
| `project_simnet_report_lane` | wave 1 G6 | 2,355 | 876 | kept |
| `project_slant_triple_handoff` | wave 2 S1 | 7,348 | 1,672 | kept |
| `project_slip_audit_jun2026` | wave 1 G2 | 5,484 | - | merged into `project_turboedit_mwgf` |
| `project_slip_meas_registry` | wave 1 G2 | 2,195 | - | merged into `project_turboedit_mwgf` |
| `project_spdlog_free_function_hazard` | wave 1 G7 | 4,122 | 2,259 | kept |
| `project_ssystem_theory_reference` | wave 2 S1 | 5,278 | - | merged into `project_network_ssystem_dia_assessment` |
| `project_status_update_2026-10-29` | first pass | 3,114 | 2,906 | kept |
| `project_synthetic_minimum_network` | wave 1 G6 | 4,809 | 1,971 | kept |
| `project_test_suite_initiative` | wave 1 G6 | 3,237 | - | merged into `project_realdata_test_suite` |
| `project_timing305_allon_baseline` | wave 1 G4 | 4,219 | 1,162 | kept |
| `project_turboedit_mwgf` | wave 1 G2 | 3,833 | 3,377 | kept |
| `project_user_object_model_uom` | first pass | 11,890 | 3,124 | kept |
| `project_uwl_card` | wave 1 G2 | 3,460 | - | merged into `project_mwwl_conditioning_day129` |
| `project_vertical_instability_solidtide` | wave 1 G3 | 3,624 | 1,355 | kept |
| `project_vmf3_gpt3_pathbug` | wave 1 G7 | 2,553 | 1,217 | kept |
| `project_warm_resume_card` | wave 1 G3 | 5,234 | - | merged into `project_consumer_gap_integrity` |
| `project_why_non_integer_rootcause` | wave 2 S1 | 6,418 | 2,870 | kept |
| `project_within_epoch_replay_rpd` | first pass | 2,725 | - | merged into `project_network_qc_tiers_lessons` |
| `project_worktree_fresh_cmake_configure` | wave 1 G7 | 1,968 | - | merged into `project_spdlog_free_function_hazard` |
| `project_zwd_sink_census_2026-09-09` | wave 2 S2 | 6,976 | - | merged into `project_closed_campaigns_pre_refounding` |
| `reference_datum_transformations` | wave 1 G7 | 1,450 | - | merged into `project_sif_integration` |
| `reference_hybrid_wire_attribution` | wave 2 S3 | 1,984 | - | merged into `project_clock_model_hadamard_calib` |
| `reference_mpages_codebase` | wave 2 S3 | 8,040 | 3,335 | kept |
| `reference_network_fault_qc_literature` | wave 2 S3 | 4,089 | 2,963 | kept |
| `reference_point_codebase` | wave 2 S3 | 4,680 | 2,311 | kept |
| `reference_ppprtk_literature_pack` | wave 2 S3 | 4,564 | 2,973 | kept |
| `user_role` | wave 2 S4 | 1,597 | 1,308 | kept |
| `MEMORY.md` | first pass | 16,947 | 12,529 | rebuilt, one line per memory |
