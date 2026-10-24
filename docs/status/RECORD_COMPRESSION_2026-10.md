# Record compression, 2026-10

Register of the compression of the project record on 2026-10-24: `CLAUDE.md`, the open-items register and the agent memory directory. The rule of every pass is that nothing is lost. Text leaves a file only by moving verbatim to a named destination, by being reduced to a pointer whose destination holds the full text, or (memories only) by being deleted because it is wrong against the tree or fully present elsewhere. Each such step is listed with byte counts before and after. Sizes are LF bytes (the git blob); the Windows working copy adds one byte per line.

| pass | target | before | after |
| --- | --- | --- | --- |
| 1 | `CLAUDE.md` | 69,800 B (70,538 B as checked out) | 34,667 B (35,049 B as checked out) |
| 2 | `docs/status/OPEN_ITEMS.md` | 56,276 B, 79 items, 38 lines over 300 characters | 25,303 B, 79 items, 22 lines over 300 characters (none over 376 except one note) |
| 3 | agent memory directory (outside git) | 863,321 B in 203 files | 296,108 B in 129 files |

## Pass 1: CLAUDE.md

Line numbers below are those of `CLAUDE.md` at commit 7bdcebc. The new pages are under `docs/onboarding/`: [build_toolchain.md](../onboarding/build_toolchain.md), [data_files.md](../onboarding/data_files.md), [where_new_code_goes.md](../onboarding/where_new_code_goes.md) and [claude_md_history.md](../onboarding/claude_md_history.md).

### Moved verbatim

| original lines | text | destination | bytes |
| --- | --- | --- | --- |
| 20 to 64 | VS 2026 migration facts: vcvars64 path, toolchain versions, the two CMakes, the `%PATH%` trap, the cached-static logger defect, the vswhere noise | build_toolchain.md, Migration facts | 3,222 |
| 72 to 76 | hardware background of the disk placement (C: and D: drives, USB link) | build_toolchain.md, Disk placement | 353 |
| 92 to 95 | scratchpad bullet of the disk placement, original wording | build_toolchain.md, Disk placement | 255 |
| 197 to 224 | `runs/` Layout entry: the four dated relocations, the MANIFEST story, the 2026-08-12 archive | build_toolchain.md, Run-directory relocations | 2,110 |
| 516 to 637 | File management in full: unified layout, both modes, migration from the flat layout, CDDIS authentication, crx2rnx, `ResolvedPaths`, bootstrap, baseline fixture presets, `.gz` cleanup | data_files.md | 6,209 |
| 656 to 738 | the two anti-regrowth decision trees at full length (network producer, real-time engine); the two headings are raised from level 3 to level 2 | where_new_code_goes.md | 23,732 |
| 493 to 512 | Retired network rules (the heading text is quoted at the top of the block) | claude_md_history.md | 1,653 |
| 455 to 470 | Baseline: card GAD paragraph with its run numbers | claude_md_history.md | 1,414 |
| 245 to 262, 273 to 278, 293 to 300 | Documentation section: introduction and tree block, rule 5, ledger paragraph | claude_md_history.md | 2,402 |
| 187 to 193, 226 to 236 | Layout entries `tests/`, `scripts/`, `data/` | claude_md_history.md | 1,225 |
| 11 | Build sentence that introduced the wrapper | claude_md_history.md | 49 |
| 641 to 645 | Known state at full length (2083-case count and its history, suite hygiene, C4828 warnings, vswhere, real-time mode) | claude_md_history.md | 2,397 |
| 649 to 654 | Entry points at full length | claude_md_history.md | 1,581 |

A line-by-line check (every non-blank line of the old file against the new file and the four pages, whitespace normalised) finds each line verbatim in one of them, except the heading of the Retired block, whose text is quoted in claude_md_history.md. The Layout block is checked as word sequences per entry (30 entries unchanged in words, four replaced).

### Condensed in CLAUDE.md, full text in the destination

| section | what the compact text keeps | what it drops |
| --- | --- | --- |
| Build | wrapper calls, targets, build tree, the three disk-placement rules, the Debug rule | migration narrative (a two-line pointer remains), hardware detail, the scratchpad rationale |
| Layout | every entry as written (the block is re-wrapped: description column 22 to 16, width 120; same words) | the `runs/` story (one sentence, the MANIFEST pointer and the trust rule remain), stale counts in `tests/` |
| Documentation | rules 1 to 4, 6, 7, the never-rewrite paragraph, the ledger procedure | the introduction and tree block (the INDEX maps the tree), the field list of rule 5 (INDEX rule 5 holds it) |
| Network runs | posture, arms, grading, sim start, shared conventions, interim hold, armed QC, the grading traps | the GAD run numbers (card GAD holds them), the Retired list (names kept, records in the history page) |
| File management | `data_source` modes, date selection, path precedence | layout tree, CDDIS, crx2rnx, bootstrap, presets, `.gz` cleanup |
| Known state | latest recorded test count with date, what the suite does not validate, suite hygiene rule | counts of earlier dates, LOC ratio, the C4828 and vswhere bullets |
| Entry points | one paragraph naming `Application.cpp`, `GnssConfigLoad.cpp` and `StateKeySchema.hpp` with their rules | per-file prose, the `KalmanFilter.hpp` and `PathResolver.hpp` lines (File management holds the latter) |
| Decision trees | one rule per row with its destination and its "never" clause; the real-time rows are merged where short | entity access functions, check scripts per row, line counts, card lineage, the real-time cut statistics |

### Corrected while moving

- Known state: the 2083-case figure (2026-09-14) is replaced by the latest recorded count, 3182 cases (3158 passed, 24 fixture-gated skips, 0 failed), Debug, 2026-10-17, from `docs/status/OPEN_ITEMS.md`. Not re-measured in this pass: the wrapper rebuilds the main checkout.
- Layout `data/` entry described the pre-date-first layout (`products/{orbit,clock,...}/{zip,unzip}/`). It now states the current layout, checked against `C:\GnssProcessor\data` (`2024-129\{obs,products}`, `static`, `cache`).
- Layout `tests/` entry: the 1540/215/104 counts (superseded) and the "80 cases" of `gnss_simulator_selftest` (not re-verified) are dropped.
- Layout `scripts/` entry: the "see File management below" clause moved into the `data/` entry.

### Other files edited

- `docs/INDEX.md`: links to the four pages and to this register.
- `docs/OVERVIEW.md`: the migration-notes pointer now names build_toolchain.md.
- `docs/onboarding/README.md`: a reference-pages table. `docs/onboarding/07_network_tour.md`: a pointer to where_new_code_goes.md.

### Left as written

Header, the Visual Studio directive, the wrapper block, targets, build tree, disk-placement rules 1 to 3, the Debug rule, Conventions, Standing invariants, the golden rules and the grading traps.

## Pass 2: docs/status/OPEN_ITEMS.md

The head rule of the register is one line per item with its evidence. Line numbers below are those of the file at commit bfb1f8e. Every item over about 300 characters (27 items, 38 lines, 39,846 bytes; the eight lines over 1,500 characters among them) moved verbatim to the card that owns it, under the dated heading "2026-10-24: register text moved from OPEN_ITEMS.md" at the end of the card, one sub-heading per item; the register row keeps one line and the link. The 79 top-level items are all still present.

### Moved verbatim

| old lines | items | destination | destination size, before to after |
| --- | --- | --- | --- |
| 22, 62, 63, 95 | open-cards row NSD, stash-status mirrors, hard clock refloat, mixed-system clock step | refound/NSD_network_small_defects_2026-10-17.md | 17,580 to 19,939 B |
| 23, 102 | open-cards row NIS, consumer innovation screen | refound/NIS_consumer_innovation_screen_2026-10-17.md | 33,109 to 34,622 B |
| 59 | block_baarda on the float vehicle | refound/BBE_block_baarda_estimable_2026-10-18.md | 25,775 to 26,676 B |
| 96 | GPS + Galileo cost | refound/NETCOSTB_gps_galileo_cost_continued_2026-10-18.md | 20,766 to 22,639 B |
| 137, 141, 143 | N3c signal decisions, object model remainder, onboarding parameter page | refound/NOM4_network_object_model_finish_2026-10-15.md | 39,241 to 41,074 B |
| 158 | Build 5, SIF outside its window | refound/SEA_seasonal_term_outside_window_2026-10-16.md | 25,937 to 26,968 B |
| 159, 160 | Build 2, ocean tide loading; SIF loading horizontal vector | refound/OTL_ocean_tide_loading_source_2026-10-16.md | 48,240 to 52,086 B |
| 161 | Build 1, clock pivot election | refound/PIV_pivot_election_coverage_2026-10-17.md | 26,330 to 27,618 B |
| 162 | Build 3, noon-turn attitude | refound/NTA_noon_turn_attitude_2026-10-16.md | 32,326 to 33,673 B |
| 163 | Build 4, wind-up through gaps | refound/WGB_windup_gap_bridge_2026-10-17.md | 52,846 to 54,435 B |
| 164 | retired-key deletion notes and follow-ons | refound/RKD_retired_key_deletion_2026-10-14.md | 14,639 to 16,591 B |
| 45, 106 to 115, 120, 121, 127, 129 to 131, 133 to 135 | gate records of the fix branches: product contract gaps (fix/contract); real-time gap tolerance with its seven sub-records, real-time suspicions, GLONASS 1020 date (fix/realtime); script-hygiene items and both check stages (hyg/scripts) | status/OPEN_ITEMS_CODE_DEFECTS_GATES.md (new) | 0 to 25,158 B |

The gate records are code-defect records, whose home is OPEN_ITEMS_CODE_DEFECTS.md. That file is 58,281 B against the 60 KB bar (61,440 B) and the records total about 25 KB, so they form a sibling page, listed in the INDEX and linked from the register's Code defects section. No destination reaches the bar (largest: WGB 54,435 B, OTL 52,086 B). NETCOST itself (61,366 B) takes no appended text for the same reason; its continuation card NETCOSTB holds the cost record.

A check of every non-blank line of the old register against the new register and the thirteen destinations finds each of them verbatim.

### Changed in the register rows

Each replacement row is written from the old row and shortened; two changes are corrections of state, made because the old rows would otherwise repeat a claim that no longer holds:

- GPS + Galileo cost: the old row listed `network.sparse_station_normals` as default off and the arming decision as owed. Card NETCOSTB (section "Armed (2026-10-19)") and the ledger (verdict ARMED, loader default true, pinned by both goldens) record that it was armed on 2026-10-19; the new row says so.
- Builds 1 to 5 and the fix branches: the old rows of Build 5 and Build 4 owed the merge. `git branch --merged` on 2026-10-24 lists fix/netsmall, fix/contract, fix/realtime, hyg/scripts, mpg/seasonal, mpg/otl, mpg/pivot, mpg/attitude and mpg/windupgap as merged into main; the new rows say "merged into main" and drop "owed: the merge".

Rows of 303 and 316 characters (the census site list, the archive candidates) were already one line and stay as written. After the pass 22 lines exceed 300 characters, all between 303 and 376 including their links, plus the 396-character note of the Code defects section.

### Other files edited

`docs/INDEX.md` (a line for the new page); the Code defects section of the register (one sentence naming the new page).

## Pass 3: the agent memory directory

The memory directory (`C:\Users\weaverb\.claude\projects\C--GnssProcessor\memory\`) is outside git and was edited in place, so this section and [the per-file page](RECORD_COMPRESSION_2026-10_memories.md) are the only record of the pass. It went from 203 files and 863,321 B (202 memories and the index) to 129 files and 296,108 B (128 memories and the index). Every memory is at most 4,000 B except two hubs of about 5.0 KB (`project_network_refounding` and `project_scenario_matrix_and_independence_2026-10-24`). The index (`MEMORY.md`) has one line per memory, in the old section order and with the old star ratings. Sizes are on-disk bytes of LF files; the memory tool adds three metadata lines (about 113 B) to every file it writes, and the "after" sizes include them.

**Totals by kind**

| kind | files before | files after | bytes before | bytes after |
| --- | ---: | ---: | ---: | ---: |
| feedback | 55 | 36 | 111,814 | 60,985 |
| project | 130 | 77 | 583,739 | 175,070 |
| reference | 6 | 4 | 24,807 | 11,582 |
| archive lessons | 10 | 10 | 124,417 | 34,634 |
| user | 1 | 1 | 1,597 | 1,308 |
| memory files | 202 | 128 | 846,374 | 283,579 |
| MEMORY.md (index) | 1 | 1 | 16,947 | 12,529 |
| **directory** | **203** | **129** | **863,321** | **296,108** |

### How it was done

Three stages. (1) The manager rewrote 106 memories into lesson form (verdict and traps first, run-log detail dropped): all 55 feedback memories, the hubs and the project and reference memories outside wave 1. (2) Wave 1: nine agents (G1 to G9) took the other 93 project, reference and archived-lesson memories by topic under per-file caps (ordinary 1,300 B, digest 3,800 B, archived lesson 3,500 B). (3) Wave 2: five agents (S1 to S5) cut the manager's files a second time (ordinary 1,700 B, feedback rules 1,500 to 1,650 B). G1 to G9 covered datum and pivots, integers and slip detectors, ionosphere and consumer, performance, real time, test harnesses, tools and reference codebases, and the two archived-lesson halves; S1 to S5 covered theory and carry, programs and campaigns, references and digests, feedback rules and the star directives. All worked under the same rules:

- Every claim that names a file, knob, script or function was checked against the tree (paths, dotted keys, identifiers, links, frontmatter); a wrong claim was corrected, not copied (list below).
- A fact guard listed every number or identifier that a rewrite dropped and that occurs nowhere in the repository; each was restored or accepted as run-log detail (run tags, executable names, workflow ids, line numbers, hashes).
- Citation rule: 92 memory names were cited by cards, specs, configs, scripts or code comments before the pass (133 after it, because the landed text names its source notes). Each is still a file or is listed in the `Formerly separate memories:` line of the file it was merged into; a script confirms that none is unfindable.
- A memory that marked itself as the only record of a result, or whose agent reported cut text that no repository file holds, was landed in a card first (next section), then shortened with a pointer.

### Landed in cards before shortening

The original text of 49 memories was appended to the owning card (or standing specification) under the heading "2026-10-24: ...", at the end of the file, after the verbatim marker where the card has one. Em dashes were replaced by commas; nothing else was reworded. A `Status note, 2026-10-24` paragraph says where the tree had moved on (renamed code, deleted keys, changed status). Nine reference cards were created where no owning card existed (marked "new" below; all `reference` except BSSD-PIVOT, which is `open`); each has its row in the INDEX and its topic page. Every surviving memory ends with `Full note copied to <path>`. Five small facts of the archived lessons (the SULP00UKR/OWMG pivot class, two runs on one machine, the 12 h run time, the shared-block drop, the oracle-test method) were restored in `archive_lessons_perf_infra` and `archive_lessons_datum_estimation` instead.


| card | memory notes landed (bytes of text) | card size before to after (LF bytes) |
| --- | --- | --- |
| `docs/SPEC_carry_factored_pz_C2.md` | `project_network_kf_vs_information_form` (3,605) | 24,214 to 28,458 |
| `docs/SPEC_d5_churn_limit_cycle.md` | `project_net305_attribution_rerun` (4,052) | 29,218 to 33,848 |
| `docs/SPEC_dev_tier_perf_cards.md` | `project_dev300s_tier` (3,541); `project_perf_deterministic_parallel_seam` (4,642) | 22,835 to 32,318 |
| `docs/SPEC_fast_clock_split_cadence.md` | `project_consumer_gap_integrity` (2,924) | 40,462 to 44,140 |
| `docs/SPEC_iono2_mechB_v3.md` | `project_iono2_card` (2,421) | 22,737 to 25,500 |
| `docs/SPEC_rx_cb_carry.md` | `project_rxcb_datum_gate_review` (2,170) | 8,783 to 11,612 |
| `docs/SPEC_warm_resume_event_card.md` | `project_warm_resume_card` (4,994) | 23,395 to 29,120 |
| `docs/cards/consumer/CI-consumer-instrument.md` | `project_closedloop_consumer_vertical_bug` (5,830) | 7,877 to 14,288 |
| `docs/cards/consumer/CONSUMER-AR-BLUNDER.md` | `project_pppar_user_control` (4,900); `project_iono_ar_review_jun2026` (4,828); `project_antenna_eccentricity_arp_frame` (3,245) | 20,492 to 35,048 |
| `docs/cards/harness/OBJECT-CODE-IDENTITY.md` (new) | `project_object_code_identity_proof` (2,033); `project_concurrent_run_log_collision` (1,615) | 0 to 5,475 |
| `docs/cards/ionosphere/AMB-IONO-SPLIT.md` | `project_why_non_integer_rootcause` (6,065); `project_extproduct_ar_probe` (2,451) | 47,473 to 57,101 |
| `docs/cards/network-vehicle/CHAIN-SLANT-ODIJK.md` | `project_slant_triple_handoff` (7,032) | 18,378 to 26,071 |
| `docs/cards/network-vehicle/HANDOVER-STRANSFORM-SPEC.md` | `project_agu26_abstract_campaign` (3,275) | 15,209 to 18,839 |
| `docs/cards/network-vehicle/SBASIS-CONFORMANCE-F.md` | `project_network_ssystem_dia_assessment` (7,479) | 23,487 to 32,014 |
| `docs/cards/network-vehicle/SBASIS-THEORY-E.md` | `project_ssystem_theory_reference` (4,952) | 26,581 to 31,899 |
| `docs/cards/network-vehicle/WINDUP-FIELD-LADDER.md` | `project_vertical_instability_solidtide` (3,325) | 18,845 to 22,866 |
| `docs/cards/queues/ARCH-BACKLOG-2026-06.md` (new) | `project_arch_opportunity_backlog` (4,062); `project_optimization_state` (5,894) | 0 to 12,572 |
| `docs/cards/queues/BASELINE-MODE-TRAPS.md` (new) | `project_baseline_requires_sp3` (5,978) | 0 to 7,761 |
| `docs/cards/queues/CAF-COMMENT-AUDIT-FINDINGS.md` | `project_rinex_phase_shift_convention` (1,859) | 27,688 to 29,901 |
| `docs/cards/queues/MPAGES-BACKLOG-2026-06.md` (new) | `project_mpages_enhancement_backlog` (10,618) | 0 to 12,472 |
| `docs/cards/raim-qc/DAY129-FRAME-ANATOMY.md` | `project_day129_pivot_cb_ratchet` (4,870) | 18,567 to 24,096 |
| `docs/cards/raim-qc/LASER-THEORY-STUDY.md` (new) | `project_laser_theory_mapping` (7,678) | 0 to 9,782 |
| `docs/cards/realtime/RT-SERVICE-RECORDS.md` (new) | `project_rt_service_work_queue` (4,618); `project_cv_slant_iono` (2,655); `project_rt_report_surveyed_reference` (3,924) | 0 to 13,914 |
| `docs/cards/realtime/RT-STREAM-RECORDS.md` (new) | `project_realtime_asterx_ppprtk` (8,373); `project_realtime_igs_rtcm_stream` (4,905); `project_realtime_bssd_epoch_blowup` (4,558); `project_realtime_model_degradation` (2,740); `project_galileo_c5c7_code_bias` (3,935) | 0 to 30,018 |
| `docs/cards/sim-bed/SIM-INJECTION-BENCH.md` | `project_sim_injection_bench` (4,660) | 28,998 to 33,998 |
| `docs/cards/sim-bed/SIMNET-REF.md` | `project_synthetic_minimum_network` (4,479) | 43,814 to 49,095 |
| `docs/cards/slip/BSSD-PIVOT-USER-SIDE.md` (new) | `project_mw_reset_pivot_handover` (3,181); `project_codeonly_pivot_iono_scrub_gap` (4,286) | 0 to 9,821 |
| `docs/cards/slip/SLIP-AUDIT-2026-06.md` (new) | `project_slip_audit_jun2026` (5,087) | 0 to 6,898 |
| `docs/cards/slip/SLIPQC-REF.md` | `project_network_if_td_slip` (6,516) | 32,967 to 40,272 |
| `docs/glonass_enablement_plan.md` | `project_glonass_ifb_network` (4,733); `project_realtime_glonass` (2,045); `project_multignss_signal_priorities_drop` (1,844) | 29,557 to 39,660 |
| `docs/refound/ARL_integer_ambiguity_ladder_2026-10-09.md` | `project_network_ar_producer_scope` (4,685) | 42,842 to 47,920 |
| `docs/refound/BDSB_beidou_continued_2026-10-19.md` | `project_beidou_network_clock_divergence` (5,965) | 11,788 to 18,584 |
| `docs/refound/RF-0b_dcb_datum_2026-09-15.md` | `project_network_code_bias_scope` (4,770) | 8,407 to 13,707 |
| `docs/refound/SNXB_sinex_roster_arms_2026-10-19.md` | `project_sif_integration` (7,295) | 22,592 to 30,708 |

Text landed: 221,592 bytes from 49 memory notes into 34 cards.

### Merges (no memory was deleted outright)

81 memories were merged into 35 surviving files, 7 of them new digests (`feedback_run_environment_traps`, `feedback_analysis_and_tooling_traps`, `feedback_slip_detector_rules`, `feedback_viz_defaults`, `project_network_qc_tiers_lessons`, `project_closed_campaigns_pre_refounding`, `project_housekeeping_decks_comments_cleanup`). A memory was merged when its topic is closed or historic and its verdicts and traps fit the survivor; an armed or active item and any trap with its own handle kept a file. Each survivor ends with a `Formerly separate memories:` line naming the merged files, so a citation by the old name still finds the text. The table lists all 81.


| surviving memory | bytes after | memories merged into it (bytes before) |
| --- | ---: | --- |
| `feedback_analysis_and_tooling_traps` | 3,926 | `feedback_backslash_via_edit_tool` (1,085); `feedback_census_replay_needs_pbf` (1,456); `feedback_harness_defaults_shared_state` (1,638); `feedback_lag_attribution_base_rate` (1,751); `feedback_sed_strips_crlf` (1,076); `feedback_stale_trace_dir_fake_ab` (1,676); `feedback_verify_symbol_absence_two_tools` (2,246); `feedback_verify_uncommitted_resolved` (1,747); `feedback_wrapped_fraction_null_traps` (2,979) |
| `feedback_run_environment_traps` | 3,901 | `feedback_agent_worktree_stale_base` (1,609); `feedback_branch_exe_unknown_knob_silent` (1,646); `feedback_build_wrapper_hardcoded_project` (1,519); `feedback_concurrent_build_locks` (3,529); `feedback_copyitem_mtime_stale_obj` (1,725); `feedback_detached_run_launchers` (2,353); `feedback_stage_own_hunk_shared_file` (2,582); `feedback_wer_zombie_wedge` (1,668) |
| `feedback_slip_detector_rules` | 3,408 | `feedback_mw_thresholds_empirical` (3,608); `feedback_reset_over_repair_biasless` (1,516); `feedback_slip_detectors_by_rate` (2,615); `feedback_subcycle_suppression_hurts_position` (1,647) |
| `feedback_viz_defaults` | 1,325 | `feedback_matlab_viz_colormaps` (1,392); `feedback_satstatus_viz_default` (1,238) |
| `project_antenna_eccentricity_arp_frame` | 2,467 | `project_rinex_phase_shift_convention` (2,106) |
| `project_arch_opportunity_backlog` | 2,636 | `project_optimization_state` (6,126) |
| `project_arl_integer_ladder` | 3,770 | `project_network_ar_producer_scope` (4,924); `project_pppar_user_control` (5,292) |
| `project_carry_datum_handover_theory` | 3,792 | `project_lgm_phase_colour_state` (4,842); `project_network_kf_vs_information_form` (3,891) |
| `project_clock_level_consistent_recursion` | 3,431 | `project_clock_temporal_anchor_q3` (5,010) |
| `project_clock_model_hadamard_calib` | 3,710 | `project_border_clone_bcl` (2,134); `project_network_clock_rate_c1` (3,092); `project_pivot_frame_process_noise` (2,113); `reference_hybrid_wire_attribution` (1,984) |
| `project_closed_campaigns_pre_refounding` | 3,746 | `project_agu26_abstract_campaign` (3,656); `project_consumer_instrument_card` (5,044); `project_zwd_sink_census_2026-09-09` (6,976) |
| `project_config_toggle_gotchas` | 2,526 | `project_config_inheritance_consolidation` (4,598); `project_default_sample_configs` (1,807) |
| `project_consumer_gap_integrity` | 2,289 | `project_warm_resume_card` (5,234) |
| `project_day129_pivot_cb_ratchet` | 2,545 | `project_iono2_card` (2,760) |
| `project_housekeeping_decks_comments_cleanup` | 2,838 | `project_colleague_deck_outline` (3,043); `project_comment_register_rewrite` (1,430); `project_root_cleanup_onboarding_2026-09-19` (2,702) |
| `project_mwwl_conditioning_day129` | 2,918 | `project_ewl_wl_card_impl` (3,602); `project_uwl_card` (3,460) |
| `project_network_code_bias_scope` | 3,784 | `project_rx_cb_carry_card` (4,170); `project_rxcb_datum_gate_review` (2,484) |
| `project_network_falsified_approaches` | 3,148 | `project_adaptive_iono_q_network` (2,531) |
| `project_network_if_td_slip` | 2,897 | `project_lane_slip_response` (3,815) |
| `project_network_logviz_implementation` | 2,862 | `project_latex_math_reference` (6,037); `project_pyviz_python_port` (3,839) |
| `project_network_qc_tiers_lessons` | 3,443 | `project_block_baarda_card` (3,271); `project_golden0729_3h_validation` (2,530); `project_m2_lsvce_carryq` (3,976); `project_raim_pool_rehost` (3,571); `project_within_epoch_replay_rpd` (2,725) |
| `project_network_ssystem_dia_assessment` | 3,689 | `project_ssystem_theory_reference` (5,278) |
| `project_object_code_identity_proof` | 1,692 | `project_concurrent_run_log_collision` (1,906) |
| `project_pivot_visibility_gauge_defect` | 3,871 | `project_codeonly_pivot_iono_scrub_gap` (4,671); `project_mw_reset_pivot_handover` (3,437); `project_pivot_handover_flipflop_fix` (4,295) |
| `project_ppp_ar_standard` | 3,573 | `project_iono_ar_review_jun2026` (5,064); `project_nl_wl_basis` (3,229) |
| `project_ppp_rtk_network` | 3,760 | `project_network_atmosphere_readiness` (2,801); `project_ppprtk_constellation_suite` (2,143) |
| `project_queued_mpages_v12_builds` | 2,946 | `project_remove_keys_deletion` (2,339) |
| `project_realdata_test_suite` | 1,675 | `project_test_suite_initiative` (3,237) |
| `project_realtime_igs_rtcm_stream` | 3,851 | `project_galileo_c5c7_code_bias` (4,267); `project_realtime_bssd_epoch_blowup` (4,883); `project_realtime_model_degradation` (3,065) |
| `project_rt_service_work_queue` | 2,025 | `project_cv_slant_iono` (2,966) |
| `project_sif_integration` | 2,974 | `project_ngs_laser_lcf` (1,987); `reference_datum_transformations` (1,450) |
| `project_sim_fixture_start_artifact` | 3,677 | `project_ignition_level_redraw_noise` (2,392); `project_sim_injection_bench` (4,997) |
| `project_spdlog_free_function_hazard` | 2,259 | `project_logmanager_initialize_nothrow` (2,416); `project_worktree_fresh_cmake_configure` (1,968) |
| `project_turboedit_mwgf` | 3,377 | `project_slip_audit_jun2026` (5,484); `project_slip_meas_registry` (2,195) |
| `project_why_non_integer_rootcause` | 2,870 | `project_amb_iono_split_overcount` (3,277) |

### Corrected against the tree

- Keys and code deleted by card RKD (2026-10-14) that memories presented as live: `network.parallel_reduce` and its exclusion with `parallel_carry`; `emit_pb_filter*`, `iono_constraint_deweight*` and the SH health gate; the `fast_clock.*` family and `emit_clock_rate` (one note said the code stays); the lane layer (`mw_wl.*`, `lane_carry.*`) and its responses (`relevel_retap_*`, `relevel_lane_snap`); `serialize_sbasis_contract`; the S2H harvest keys; `shared_carry_gauge_free`, `carry_coupling_sh_marginalize`, `carry_coupling_joint_whiten`; `network.adaptive_iono_q`, `phase_datum_ensemble`, `clock_signal_basis_hold`; `phase_bias_exclude_unmatched_antenna` (the live path is `network.exclude_unmodeled_antenna_stations`); `state_snapshot_resume_shock_m` and the other resume-shock keys.
- Statuses that changed: the SBF Galileo I/NAV decode was wrong (odd-page offset, fixed 2026-10-23 in 0378e6d, with `processing.ephemeris_jump_reseed` added); the single-band BeiDou admission rule is built (`network.second_band_admission`, UNVALIDATED, not armed); cards LGM, AIL, PRM, PRM3 and CI are closed; the `prior_rxcb` datum and carry mass are tagged apart in `ContributionKind`; uncombined `mode: PPP` was fixed (f3b8f2c) and AR mode-3 fix-and-hold is no longer a no-op (b114033); the VMF3 defect was `Gpt3::load` returning unscaled coefficients (e728fdc), not the mapping math; the SIF post-seismic term double counted 1.48 m at MIZU00JPN (4800a92); `network.sparse_station_normals` was armed on 2026-10-19; the leap-second defect, the comment-card sweep and the branches `card/mday`, `mpg/*`, `fix/*` and `hyg/*` are done or merged.
- Renamed code: the `NetworkXxx.cpp` sources are `src/core/network/<concern>/Xxx.cpp`; `kSsrPhaseBiasSign` and `kSsrCodeBiasSign` are one `kSatBiasSign`; tests moved to `tests/network/`; the network driver is 354 lines, not 3,139; `rt_seam_verify.py` was retired on 2026-10-17.
- Facts: the SIF OTL columns are BLQ rows (the +180 survives only on the legacy `enable_station_info` path); GLONASS 1020 dating and MSM weeks use the parser's stream-time tracker (host clock only at cold start, ff1c464); M-PAGES `Partial.cpp:375-377` does not exist (the mapping is `Lib/AmbRes/Partial.cpp:121`) and the oneTBB `task_scheduler_init` break holds for v1.1 only; the cssrlib SP3 rotation delta is resolved in `getSatellitePV`; `network.carry_zwd` is the falsified double count while `carry_zwd_in_block` is RECOMMEND_SCOPED; `network.raim_we_code_iono` is KEEP_OFF, not UNVALIDATED; the ledger never cited the falsified-approaches note.

### Stale text found in the repository (not edited)

- `docs/topics/perf.md` (line 10), `docs/SPEC_carry_factored_pz_C2.md` (line 192), `docs/SPEC_cb_admission_card.md` (line 318) and `config/golden/golden_manifest.json` (lines 133 and 226) still describe or list `network.parallel_reduce` and its exclusion with `parallel_carry`.
- `docs/SPEC_dev_tier_perf_cards.md` section 8 names the deleted `network_emit_pb_filter_window_epochs`; `docs/refound/CARDS.md` (SEA entry) says the merge is owed, although `mpg/seasonal` is merged.
- `src/core/products/LiveEphemerisStore.cpp` says the SBF I/NAV field decode matches RTCM 1046, which the odd-page defect disproved; card SBASIS-CONFORMANCE-F still cites the renamed `kSsrPhaseBiasSign`.
- `config/README.md` names `ppp.json` and `ppp_rtk.json` (absent) and says `baseline_single.json` uses IF DDs while the file has `use_iono_free: false`; `scripts/event_leg.py` line 105 (`DEFAULT_GOLDEN`) is the retired `golden_sim_tri_w6.json`.
- `config/realtime_igs_ntrip.json` streams BRUX00BEL0 but activates the P389 antenna override, and its `ssr_max_age_s` is 60 where the memory said 30.
- `docs/SPEC_warm_resume_event_card.md` cites the memory `project_carry_blob_diagonal` (merged on 2026-10-07; its name is in the `Formerly separate memories:` line of `project_consumer_gap_integrity`).

### Open items seen in memories with no register row (not added)

The register row for the MW reset at an SD pivot change was added (card BSSD-PIVOT). The following stay in memories and cards only; the author decides which become rows:

- Real time (cards RT-STREAM, RT-SERVICE): the stall-fallback decision, the BDS SSR age policy, the unsteered receiver-clock gauge fix, the outlier amplifier fixes, the PZ-90 to ITRF gap of about 0.5 m, the SSR phase-bias sign measurement, the RTA-10 H-record gaps.
- Estimation: the kinematic vertical sinusoid (no card, no row), the smoother T4 wiring (SPEC section 13 only), the BeiDou arming decision (cards BDS, BDSB, MDAY say open), the rx-CB inject gate frame-blindness (card RS-7 to RS-9), the global sigma0 rider field gates, the decoupling of `sigma0_adapt` from `carry_warm`, the S-system plan items and critic blind spots (landed in SBASIS-CONFORMANCE-F), the sim A4-withhold episode of 2026-08-15 (SIMNET-REF).
- Tooling: `scripts/event_leg.py` `DEFAULT_GOLDEN` and the seeds written before the GAD baseline; `config/README.md` and `baseline_single.json` as above.

### Observation: credentials in tracked configs

`config/realtime_igs_ntrip.json` and `config/realtime_ppp_rtk.json` are tracked and hold non-empty plaintext NTRIP passwords. The values were not copied into any memory, card or this page, and the files were not edited.

### Checks at the end

- `final_check` (script over the directory): 0 problems: frontmatter (name equals file stem, description at most 230 characters, valid type), no dead `[[link]]`, no memory over 4,000 B except the named hubs, no em dash in a body, `MEMORY.md` has exactly one link per line and one line per file, and every one of the 133 memory names found in the repository is a file or a `Formerly` entry.
- Claim check (`mem_verify`) over the 128 memories: the remaining flags are run output file names, merged-away memory names inside the `Formerly` lines, deliberately removed identifiers listed as forbidden, two never-built names, paths of external codebases (M-PAGES, POINT, cssrlib) and the scenario files of another working tree.
- Fact guard against the 202 originals, with the repository (landed cards included) as the corpus: 59 numbers and 170 identifiers that occur in no file remain dropped; all are run tags, executable and workflow names, line numbers, hashes or names of things never built.
- `python scripts/docs_index_check.py`: passes (467 documents).

### Other files edited by this pass

`docs/INDEX.md` (rows for the nine new cards, a line for the per-file page), the topic pages `slip`, `queues`, `raim-qc`, `harness` and `realtime`, one row in `docs/status/OPEN_ITEMS.md` (QC, slip and carry responses), and the 25 existing cards and specifications that received a landing.
