# REORG PLAN — the docs/ topic tree (2026-09-14)

**The complaint this answers, verbatim (author, 2026-09-14):** *"It is incredibly
difficult to follow because our project seems extremely bloated. For example,
`docs/SPEC_event_harness.md` is enormous. We need a method to organize our whole
project, rather than simply appending to these massive files. Perhaps a file tree
organized by topic is needed."*

**What this plan commits to.** `docs/` holds 77 markdown files totalling **4.72 MB**,
of which **19 are over 60 KB** and the top four alone are **1.84 MB** — 39 % of the
corpus in four files that are appended to, in chronological order, by every session.
This plan builds a topic tree, moves the existing content into it **verbatim and
verifiably**, writes the index and the rules, and leaves every existing citation
resolvable in one hop.

Nothing in this plan rewrites, shortens, reflows or "cleans up" any existing section
text. The docs are the project's scientific record. The split is a **partition**:
every byte of a monolith lands in exactly one card file, in document order, and
`scripts/docs_split.py --verify` proves it by hash.

Companion file: **[REORG_MAPPING.md](REORG_MAPPING.md)** — the per-monolith
section → target-file mapping tables.

---

## 1. What is being organised — the inventory

Sizes are LF bytes (the working tree is CRLF because `core.autocrlf=true`; every
number here and every hash in the split is computed on LF-normalised text, so it is
stable regardless of what the checkout did).

`##` / `###` are the heading counts — the unit the split cuts on. "Inbound refs" is
the number of lines, outside the doc itself, that name the file or its stem
(`SPEC_event_harness`), scanned over `CLAUDE.md`, `docs/`, `scripts/`, `config/`,
`tests/`, `src/`, `include/` and the user's memory directory
(`~/.claude/projects/C--GnssProcessor/memory/`, **read-only — never edited by this
work**). Two known over-counts: `test_suite.md`'s `config:594` is mostly the literal
path `config/test_suite/…`, and `SPEC_pivot_visibility_gauge_defect.md`'s
`config:242` is the doc stem appearing in arm-config comment fields.

| doc | KB | `##` | `###` | first → last commit | commits | inbound refs (file / scope) | topic | this pass |
| --- | ---: | ---: | ---: | --- | ---: | --- | --- | --- |
| `SPEC_event_harness.md` | 663 | 29 | 162 | 2026-09-10 → 2026-09-14 | 111 | 572 — src:137 docs:129 include:128 scripts:87 config:40 tests:26 memory:24 CLAUDE.md:1 | harness | **SPLIT** |
| `CONFIG_promotion_ledger.md` | 585 | 31 | 210 | 2026-07-29 → 2026-09-13 | 96 | 175 — config:89 docs:35 scripts:24 memory:17 src:5 CLAUDE.md:3 include:2 | config-ledger | **SPLIT** |
| `SPEC_amb_iono_split_overcount.md` | 401 | 20 | 67 | 2026-09-04 → 2026-09-10 | 51 | 344 — config:201 docs:57 include:35 src:33 tests:8 scripts:7 memory:3 | ionosphere | **SPLIT** |
| `SPEC_synthetic_minimum_network.md` | 309 | 32 | 105 | 2026-08-11 → 2026-09-09 | 48 | 340 — config:201 docs:50 src:29 include:26 scripts:15 memory:11 tests:7 CLAUDE.md:1 | sim-bed | **SPLIT** |
| `SPEC_level_information_budget.md` | 166 | 36 | 94 | 2026-08-05 → 2026-08-21 | 7 | 109 — src:33 docs:23 include:20 config:15 memory:9 scripts:5 tests:3 CLAUDE.md:1 | ionosphere | **SPLIT** |
| `SPEC_slip_qc_hardening.md` | 115 | 14 | 28 | 2026-07-22 → 2026-09-10 | 6 | 69 — docs:31 config:12 src:9 include:6 memory:6 tests:3 scripts:2 | slip | **SPLIT** |
| `SPEC_clock_temporal_anchor.md` | 89 | 10 | 38 | 2026-08-10 → 2026-08-31 | 12 | 63 — config:18 docs:15 src:8 memory:8 include:7 scripts:4 tests:3 | clock | **SPLIT** |
| `SPEC_redundancy_census.md` | 81 | 15 | 41 | 2026-08-03 → 2026-08-15 | 2 | 86 — src:41 include:20 docs:14 tests:4 memory:4 config:3 | raim-qc | **SPLIT** |
| `SPEC_measurement_admission.md` | 75 | 10 | 34 | 2026-08-03 → 2026-08-15 | 2 | 22 — src:8 docs:4 include:4 memory:3 scripts:2 tests:1 | raim-qc | **SPLIT** |
| `SPEC_prior_residual_census.md` | 73 | 18 | 25 | 2026-08-05 → 2026-09-10 | 3 | 65 — src:24 include:13 docs:11 config:9 tests:4 memory:4 | raim-qc | **SPLIT** |
| `SPEC_sbasis_realization_contract.md` | 73 | 21 | 0 | 2026-08-10 → 2026-08-15 | 3 | 22 — docs:10 memory:7 tests:2 src:2 include:1 | network-vehicle | **SPLIT** |
| `SPEC_pivot_visibility_gauge_defect.md` | 71 | 17 | 27 | 2026-08-13 → 2026-08-31 | 12 | 318 — config:242 docs:32 src:19 include:13 memory:6 scripts:3 tests:3 | network-vehicle | **SPLIT** |
| `QUEUE_principled_replacements.md` | 71 | 12 | 2 | 2026-08-14 → 2026-08-21 | 25 | 14 — docs:8 memory:3 include:2 scripts:1 | queues | **SPLIT** |
| `SPEC_day129_frame_guards.md` | 69 | 26 | 12 | 2026-07-28 → 2026-08-15 | 2 | 68 — src:21 include:17 docs:16 scripts:10 memory:4 | raim-qc | **SPLIT** |
| `SPEC_withhold_era_release.md` | 69 | 12 | 21 | 2026-08-21 → 2026-08-23 | 3 | 33 — src:12 config:9 include:7 docs:2 scripts:1 tests:1 memory:1 | raim-qc | **SPLIT** |
| `SPEC_lane_slip_response.md` | 68 | 8 | 15 | 2026-08-22 → 2026-08-23 | 9 | 70 — docs:22 src:15 config:11 include:11 memory:4 scripts:3 CLAUDE.md:2 tests:2 | slip | **SPLIT** |
| `SPEC_phase_windup_sign.md` | 67 | 11 | 28 | 2026-08-21 → 2026-08-23 | 7 | 41 — src:13 config:8 docs:6 include:6 tests:3 CLAUDE.md:2 scripts:2 memory:1 | network-vehicle | **SPLIT** |
| `SPEC_t3_null_derivation.md` | 67 | 15 | 22 | 2026-08-14 → 2026-08-21 | 11 | 45 — docs:12 src:12 include:9 config:6 scripts:3 memory:2 tests:1 | raim-qc | **SPLIT** |
| `SPEC_card1_coupling_carry.md` | 61 | 14 | 13 | 2026-08-15 → 2026-08-19 | 9 | 29 — docs:11 memory:6 src:5 include:4 tests:2 config:1 | network-vehicle | **SPLIT** |
| `network_decomposition_plan.md` | 58 | 18 | 6 | 2026-07-05 → 2026-08-15 | 13 | 28 — docs:11 include:9 src:4 scripts:2 memory:2 | perf | indexed in place |
| `rt_service_conventions.md` | 56 | 12 | 24 | 2026-07-02 → 2026-08-15 | 2 | 16 — docs:6 include:5 src:2 memory:2 config:1 | realtime | indexed in place |
| `SPEC_ewl_wl_cascade_card.md` | 50 | 20 | 7 | 2026-07-16 → 2026-08-15 | 2 | 31 — docs:15 include:5 scripts:3 src:3 memory:3 tests:2 | slip | indexed in place |
| `SPEC_family_gf_datum.md` | 50 | 11 | 10 | 2026-08-13 → 2026-09-10 | 4 | 31 — docs:13 src:5 memory:4 include:3 scripts:2 config:2 tests:2 | network-vehicle | indexed in place |
| `SPEC_oracle_pin.md` | 48 | 13 | 12 | 2026-09-11 → 2026-09-11 | 3 | 13 — scripts:5 src:3 docs:2 memory:2 include:1 | harness | indexed in place |
| `SPEC_sd_ewl_prereduce.md` | 47 | 21 | 18 | 2026-07-31 → 2026-08-15 | 2 | 43 — docs:17 include:9 src:7 memory:4 config:3 tests:3 | slip | indexed in place |
| `SPEC_inc8b_election_split.md` | 45 | 13 | 0 | 2026-07-22 → 2026-08-21 | 3 | 9 — docs:6 include:2 memory:1 | perf | indexed in place |
| `SPEC_noncausal_slip_bridge.md` | 44 | 15 | 2 | 2026-08-15 → 2026-08-15 | 1 | 5 — memory:3 docs:2 | slip | indexed in place |
| `SPEC_c1_clock_rate_validation.md` | 41 | 13 | 18 | 2026-07-28 → 2026-08-21 | 3 | 12 — docs:6 memory:4 scripts:2 | clock | indexed in place |
| `SPEC_prefit_census_observability.md` | 40 | 18 | 3 | 2026-08-03 → 2026-09-01 | 4 | 42 — src:16 docs:10 include:7 scripts:5 memory:2 CLAUDE.md:1 tests:1 | raim-qc | indexed in place |
| `SPEC_fast_clock_split_cadence.md` | 40 | 17 | 11 | 2026-07-28 → 2026-08-15 | 2 | 15 — docs:5 include:4 src:2 memory:2 config:1 tests:1 | clock | indexed in place |
| `SPEC_cb_admission_card.md` | 39 | 14 | 26 | 2026-07-22 → 2026-08-15 | 2 | 12 — docs:7 include:2 memory:2 scripts:1 | code-bias | indexed in place |
| `SPEC_lane_spectrometer.md` | 39 | 14 | 19 | 2026-07-31 → 2026-08-15 | 2 | 20 — docs:7 scripts:5 config:3 src:3 memory:2 | slip | indexed in place |
| `HANDOFF_edge_response_queue.md` | 36 | 7 | 11 | 2026-08-05 → 2026-08-21 | 7 | 18 — docs:5 memory:5 config:4 include:2 scripts:1 tests:1 | queues | indexed in place |
| `CLOCK_HADAMARD_CALIB_2026-07-14.md` | 36 | 4 | 6 | 2026-07-16 → 2026-08-15 | 2 | 7 — docs:5 CLAUDE.md:1 memory:1 | clock | indexed in place |
| `SPEC_raim_pool_rehost.md` | 36 | 9 | 11 | 2026-08-13 → 2026-09-13 | 6 | 42 — docs:18 src:7 memory:6 scripts:5 include:4 CLAUDE.md:1 tests:1 | raim-qc | indexed in place |
| `aiding_source_abstraction_design.md` | 35 | 16 | 0 | 2026-06-15 → 2026-08-15 | 2 | 15 — include:7 src:5 docs:1 tests:1 memory:1 | build-infra | indexed in place |
| `SPEC_lane_carry_card.md` | 35 | 18 | 9 | 2026-07-16 → 2026-08-15 | 2 | 38 — docs:12 include:11 src:7 memory:5 tests:3 | slip | indexed in place |
| `SPEC_pd_safe_dd_fold.md` | 34 | 12 | 16 | 2026-08-03 → 2026-08-15 | 2 | 44 — src:11 docs:9 include:7 tests:6 config:5 scripts:3 memory:3 | network-vehicle | indexed in place |
| `SPEC_mw_wl_card.md` | 33 | 15 | 10 | 2026-07-16 → 2026-08-15 | 2 | 18 — docs:8 src:5 include:5 | slip | indexed in place |
| `network_ambiguity_resolution_design.md` | 32 | 10 | 9 | 2026-06-29 → 2026-09-10 | 6 | 17 — docs:4 include:4 src:3 memory:3 tests:2 scripts:1 | network-vehicle | indexed in place |
| `SPEC_flicker_gap_integrity.md` | 30 | 9 | 9 | 2026-08-03 → 2026-08-15 | 2 | 13 — docs:4 include:4 src:3 tests:1 memory:1 | raim-qc | indexed in place |
| `SPEC_slip_relevel_walk.md` | 29 | 10 | 6 | 2026-08-20 → 2026-08-21 | 8 | 73 — docs:22 src:17 include:16 config:8 memory:4 tests:3 scripts:2 CLAUDE.md:1 | slip | indexed in place |
| `glonass_enablement_plan.md` | 29 | 6 | 5 | 2026-07-02 → 2026-08-15 | 2 | 11 — docs:5 memory:5 include:1 | code-bias | indexed in place |
| `SPEC_carry_aware_covariance.md` | 29 | 11 | 4 | 2026-08-03 → 2026-08-15 | 2 | 29 — src:8 include:8 docs:6 config:2 tests:2 memory:2 scripts:1 | network-vehicle | indexed in place |
| `SPEC_d5_churn_limit_cycle.md` | 29 | 9 | 10 | 2026-08-03 → 2026-08-15 | 2 | 9 — docs:3 memory:3 scripts:2 src:1 | network-vehicle | indexed in place |
| `SPEC_iono_carry_8b.md` | 27 | 6 | 6 | 2026-08-20 → 2026-08-21 | 2 | 22 — config:9 docs:8 include:2 scripts:1 src:1 memory:1 | ionosphere | indexed in place |
| `SPEC_o2_carry_invariant_accumulator.md` | 26 | 10 | 0 | 2026-08-21 → 2026-08-21 | 1 | 11 — docs:7 memory:3 src:1 | network-vehicle | indexed in place |
| `SPEC_level_authority_internal.md` | 25 | 11 | 7 | 2026-08-21 → 2026-09-01 | 4 | 30 — src:7 config:6 scripts:5 docs:4 include:4 tests:2 memory:2 | ionosphere | indexed in place |
| `SPEC_latex_doc_refresh_2026-07-17.md` | 25 | 5 | 9 | 2026-07-22 → 2026-08-15 | 2 | 5 — docs:3 memory:2 | build-infra | indexed in place |
| `SPEC_eq34_seed_round.md` | 24 | 11 | 6 | 2026-08-12 → 2026-08-15 | 4 | 20 — src:5 include:5 docs:4 config:3 tests:2 memory:1 | network-vehicle | indexed in place |
| `SPEC_carry_factored_pz_C2.md` | 24 | 9 | 14 | 2026-07-16 → 2026-08-15 | 2 | 13 — docs:9 memory:2 src:1 include:1 | perf | indexed in place |
| `slip_comparison_report.md` | 23 | 5 | 5 | 2026-08-15 → 2026-08-15 | 1 | 9 — memory:6 docs:3 | slip | indexed in place |
| `SPEC_cb_carry_physical_q.md` | 23 | 9 | 4 | 2026-08-12 → 2026-08-15 | 2 | 15 — config:11 memory:2 docs:1 scripts:1 | code-bias | indexed in place |
| `SPEC_warm_resume_event_card.md` | 23 | 8 | 5 | 2026-07-24 → 2026-08-15 | 2 | 12 — docs:9 memory:2 scripts:1 | resume-seam | indexed in place |
| `SPEC_dev_tier_perf_cards.md` | 22 | 8 | 9 | 2026-08-15 → 2026-08-15 | 1 | 18 — docs:13 memory:3 src:1 include:1 | perf | indexed in place |
| `SPEC_iono2_mechB_v3.md` | 22 | 11 | 3 | 2026-07-31 → 2026-08-15 | 2 | 23 — docs:8 src:8 include:4 memory:2 tests:1 | ionosphere | indexed in place |
| `SPEC_deweight_release_hygiene.md` | 22 | 7 | 8 | 2026-08-03 → 2026-08-15 | 2 | 28 — docs:10 src:6 memory:6 include:4 tests:2 | ionosphere | indexed in place |
| `SPEC_beidou_w6_datum.md` | 21 | 8 | 3 | 2026-07-22 → 2026-08-15 | 2 | 16 — docs:8 include:4 src:2 tests:1 memory:1 | code-bias | indexed in place |
| `SPEC_eq34_reattach_hygiene.md` | 21 | 8 | 6 | 2026-08-03 → 2026-08-15 | 2 | 12 — src:5 docs:3 include:2 tests:1 memory:1 | network-vehicle | indexed in place |
| `SPEC_carry_vce_apply.md` | 21 | 11 | 1 | 2026-08-05 → 2026-08-21 | 3 | 24 — src:8 include:7 docs:3 memory:3 config:2 tests:1 | network-vehicle | indexed in place |
| `network_global_sh_iono_design_2026-06-29.md` | 20 | 11 | 4 | 2026-06-30 → 2026-08-15 | 2 | 4 — docs:2 include:1 memory:1 | ionosphere | indexed in place |
| `test_suite.md` | 19 | 11 | 7 | 2026-06-28 → 2026-08-15 | 3 | 909 — config:594 docs:168 scripts:96 memory:44 src:3 CLAUDE.md:2 tests:1 include:1 | build-infra | indexed in place |
| `RUNQUEUE_pdfix_reask.md` | 19 | 9 | 4 | 2026-08-03 → 2026-08-15 | 2 | 25 — config:12 docs:9 scripts:2 memory:2 | history | indexed in place |
| `rt_service_operations.md` | 19 | 5 | 4 | 2026-07-02 → 2026-08-15 | 4 | 15 — include:7 docs:6 memory:2 | realtime | indexed in place |
| `SPEC_uwl_card.md` | 18 | 9 | 4 | 2026-07-16 → 2026-08-15 | 2 | 22 — src:7 docs:6 tests:4 include:3 memory:2 | slip | indexed in place |
| `SPEC_card4_invariant_carry.md` | 18 | 13 | 3 | 2026-08-15 → 2026-08-15 | 1 | 14 — docs:5 memory:4 include:3 tests:1 src:1 | network-vehicle | indexed in place |
| `SPEC_rinex_zero_sentinel.md` | 16 | 7 | 9 | 2026-08-03 → 2026-08-15 | 2 | 9 — docs:4 src:2 tests:1 include:1 memory:1 | build-infra | indexed in place |
| `SPEC_clock_emit_rebase.md` | 15 | 7 | 1 | 2026-08-09 → 2026-08-15 | 3 | 34 — config:14 docs:11 src:3 include:2 memory:2 scripts:1 tests:1 | clock | indexed in place |
| `SPEC_datum_graph_census.md` | 15 | 11 | 6 | 2026-08-05 → 2026-08-15 | 2 | 16 — src:7 include:4 docs:2 memory:2 tests:1 | network-vehicle | indexed in place |
| `HANDOFF_seam_frame_restore_2026-09-03.md` | 13 | 8 | 0 | 2026-09-03 → 2026-09-06 | 5 | 3 — config:2 memory:1 | resume-seam | indexed in place |
| `SPEC_product_cbpin_contract.md` | 12 | 6 | 6 | 2026-07-28 → 2026-08-15 | 3 | 17 — docs:14 memory:3 | wire-contract | indexed in place |
| `README.md` | 12 | 12 | 0 | 2026-08-12 → 2026-08-15 | 3 | 32 — scripts:14 memory:8 docs:4 src:4 CLAUDE.md:2 | build-infra | indexed in place |
| `datum_transformation_reference_2026-08-07.md` | 11 | 9 | 5 | 2026-08-07 → 2026-08-15 | 2 | 3 — memory:2 docs:1 | build-infra | indexed in place |
| `CONFIG.md` | 9 | 10 | 4 | 2026-04-30 → 2026-08-15 | 4 | 127 — config:87 docs:15 memory:9 scripts:6 include:4 tests:3 src:2 CLAUDE.md:1 | build-infra | indexed in place |
| `rt_seam_decomposition.md` | 9 | 8 | 0 | 2026-08-03 → 2026-08-15 | 2 | 5 — docs:2 memory:2 CLAUDE.md:1 | realtime | indexed in place |
| `SPEC_rx_cb_carry.md` | 9 | 6 | 1 | 2026-07-31 → 2026-08-15 | 2 | 21 — src:9 include:6 docs:3 memory:2 tests:1 | code-bias | indexed in place |
| `turbo_edit_lli_bridge_design.md` | 4 | 3 | 0 | 2026-06-15 → 2026-08-15 | 2 | 5 — include:2 docs:1 src:1 memory:1 | slip | indexed in place |

### Why the stubs are not optional

`src/` and `include/` cite these docs **by path, from code comments** — 137 lines
name `SPEC_event_harness`, 128 more in headers, and the pattern
`// RS-2 (SPEC_event_harness §21.4): …` is the house style. A split that moved a
monolith without leaving a stub would strand several hundred code comments. Hence:
the old path always survives as a stub whose section table resolves any `§N.N`
citation in one hop.

---

## 2. The topic list

The brief proposed thirteen topics; the content dictated seventeen. Four were added
because the corpus carries enough of each to warrant its own page, and none of the
proposed thirteen was dropped:

| topic | what it covers | added? |
| --- | --- | --- |
| `harness` | the event harness itself — injectable events, seeds, legs, the leg recipe, the grader, the oracle pin | |
| `network-vehicle` | the producer's estimation vehicle: `carry_invariant` vs phase leveling, the S-basis realization contract, the gauge / pivot / datum, eq34, the coupling carry, the station block | |
| `clock` | the clock datum, CLK-R emission rebase, the Q3 temporal anchor, fast clock, the C1 rate card, clock-step signalling | |
| `ionosphere` | the SH field, the per-LOS iono representation, the level information budget, iono ties, the ambiguity/iono split | |
| `code-bias` | W6 / OSB satellite code bias, the rx-CB carry and its harvest, CB admission, BeiDou, GLONASS IFB | |
| `slip` | detectors (MW, GF, IF-TD, LLI), the re-level responses, the lane cascade (EWL/WL/UWL), the lane carry and spectrometer | |
| `raim-qc` | RAIM tiers, MDB / Baarda, the redundancy, prior-residual and prefit censuses, measurement admission, the T3 null, withhold policy | |
| `wire-contract` | what the product wire promises: M4, discontinuity declaration, the cbpin contract, the level contract and level-health channel | |
| `consumer` | the closed-loop consumer as an instrument, consumer replay, the consumer-side AR blunder | |
| `resume-seam` | warm resume and state snapshots: the RS-* seam cards, frame / coupling / border restore | **added** |
| `sim-bed` | the synthetic minimum network, truth grading, the injection bench, the dashboard, the stability campaign | |
| `perf` | timing, parallelism, the producer decomposition, the dev tiers | |
| `realtime` | the RT engine, its seam cut, the service work queue, conventions and operations | **added** |
| `config-ledger` | `CONFIG_promotion_ledger` — the per-knob verdict ledger and its appendices | **added** |
| `queues` | live cross-family work registers (principled replacements, the edge-response queue) | **added** |
| `build-infra` | toolchain, the test harness, config reference, data layout, ingest guards, docs tooling, architecture designs | |
| `history` | closed campaign records kept for provenance | |

Doc counts per topic (existing `docs/*.md` only — the split adds cards on top):

| topic | docs assigned |
| --- | ---: |
| `build-infra` | 7 |
| `clock` | 5 |
| `code-bias` | 5 |
| `config-ledger` | 1 |
| `harness` | 2 |
| `history` | 1 |
| `ionosphere` | 7 |
| `network-vehicle` | 15 |
| `perf` | 4 |
| `queues` | 2 |
| `raim-qc` | 9 |
| `realtime` | 3 |
| `resume-seam` | 2 |
| `sim-bed` | 1 |
| `slip` | 12 |
| `wire-contract` | 1 |

`consumer`, `wire-contract`, `harness` and `sim-bed` look thin in that table because
most of their content is currently *inside* the monoliths; the split is what gives
them their cards.

---

## 3. The target tree

```
docs/
  INDEX.md                      the single entry point: topics, cards, rules
  REORG_PLAN.md                 this file
  REORG_MAPPING.md              the section → file mapping tables
  cards/<topic>/<CARD-ID>.md    one card per file: STATUS block + verbatim text
  topics/<topic>.md             a short living reference page per topic
  decisions/<DEC-ID>.md         posture rulings whose text is self-contained
  status/OPEN_ITEMS.md          every owed / open item, with its evidence pointer
  archive/<monolith>.md         the originals, byte-for-byte, FROZEN
  <monolith>.md                 a stub with the full section → file table
  attic/                        (unchanged) the 2026-08-15 attic, 43 files
  report/                       (unchanged) the LaTeX deliverable
```

### The anatomy of a card file

```
# <ID> — <one-line summary>

**STATUS**

- **id:** RS-4
- **topic:** resume-seam
- **status:** open | closed | parked | superseded | reference
- **summary:** one line
- **dates in the text:** 2026-09-12 → 2026-09-13
- **knobs / posture:** `network.…`
- **commits named in the text:** …
- **run dirs named in the text:** `runs/…`
- **source:** `docs/archive/SPEC_event_harness.md` — §21.10, §21.11

> Results are appended TO THIS FILE, below the verbatim text.

---
<!-- BEGIN VERBATIM: SPEC_event_harness.md -->
…the section text, byte for byte…
```

The STATUS block is the only new prose. Its `knobs`, `commits`, `run dirs` and
`dates` fields are **extracted from the section text itself** by the splitter (no
value is invented); `id`, `status` and `summary` come from the mapping file, where
the summary is the section's own title.

`status` uses exactly five values: `open`, `closed`, `parked`, `superseded`, and
`reference` (for text that is standing reference material rather than a card — the
harness manual, the S-basis contract, the ledger tiers). The finer verdict a card
reached — PROMOTED, KEEP_OFF, FALSIFIED, RECOMMEND_SCOPED — stays in the summary and
in the text, because that vocabulary belongs to the project, not to the file format.

### Why a `REF-` / `reference` class exists

Not every section of a monolith is a card. `SPEC_event_harness` §1–§8 is the
harness's user manual; `SPEC_sbasis_realization_contract` is a theory contract; the
ledger's tiers are a knob table. Those become `reference`-status files under
`cards/<topic>/`, so that the partition stays exact (every byte has exactly one
home) while the *topic pages* stay short living pages that nobody has to scroll.

---

## 4. The nineteen monoliths, and how each is cut

Every `docs/*.md` over 60 KB is split. The cut points are headings — `##` or `###` —
so a slice is always a whole section, never a paragraph. Cards that the document
records out of order (the harness interleaves §14.4–§14.9 after §15.11, and §16.7 after
§17) are reassembled: a target file may own several non-adjacent slices, concatenated
in document order, which is exactly how the verification walks them.

Two mechanical constraints shaped the cuts:

1. **No non-archive file over 60 KB.** Where a card is bigger than that, it is cut at
   its own internal boundary and both halves keep the card id — `COLD-FB` becomes
   `COLD-FB-anchor-fallbacks.md` (pre-registration, G1–G3, iteration 1) and
   `COLD-FB-iterations-2-4.md`. The ledger's Tier 2 and Tier 3 are cut into lettered
   runs of knob rows the same way.
2. **One section with no internal heading cannot be cut at all.**
   `SPEC_amb_iono_split_overcount` §7 (the open-question register) is 56.5 KB of
   continuous text with no `###` inside it. It becomes the single largest card at
   **59.0 KB** — under the bar, but the reason it is not smaller is that cutting it
   would require editing the text, which this pass will not do. Its items are the
   raw material for `status/OPEN_ITEMS.md`.

Result: **19 monoliths → 166 card files + 2 decision files + 19 stubs + 19
archived originals** (168 split targets in all). Per-monolith tables are in
[REORG_MAPPING.md](REORG_MAPPING.md).

### The ledger is NOT generated — it is split, with one hazard

`docs/CONFIG_promotion_ledger.json` is the machine-read artifact
(`scripts/make_golden_config.py`, `scripts/config_posture_check.py`). The `.md`
ledger is **not** generated from it: both are edited in parallel, by hand and by the
`scripts/_*_ledger_oneoff.py` pattern (`_coldfb_`, `_fieldharness_`, `_rs2_refarc_`,
`_scopedpromo_`, `_stspromo_`, `_study7_`), each of which opens
`docs/CONFIG_promotion_ledger.md` by path and patches a row.

So the `.md` is split like any other monolith — and the hazard is recorded here and
in the stub: **after this pass, `docs/CONFIG_promotion_ledger.md` is a stub.** A
future ledger one-off that appends to that path would write into the stub instead of
the ledger. The stub says so in its first lines, `scripts/README.md` flags the
one-off family, and this is filed in `status/OPEN_ITEMS.md` as the one follow-up the
split creates rather than closes. The `.json` and its generator are untouched.

### What this pass deliberately does NOT move

The 58 docs under 60 KB **stay at `docs/<name>.md`** and are indexed by topic instead
of relocated. Reason: every one of them is referenced by path — from `src/` and
`include/` comments, from `scripts/`, from arm configs, and from the author's memory
directory, which this work is not allowed to edit. Relocating 58 files would cost 58
more stubs and would strand memory links that only the author can repair. `INDEX.md`
and the topic pages already give the navigation; the physical move is a later pass,
once the tree has settled and the memory files have been re-pointed.

---

## 5. How "verbatim" is proven

`scripts/docs_split.py` (new, added in the split commit) does four things:

| mode | what it does |
| --- | --- |
| `--sizes` | dry run: target sizes, and a non-zero exit if anything would exceed 60 KB |
| `--build` | writes the cards, the archived original (+ a one-line READ-ONLY banner) and the stub |
| `--verify` | **the proof** — reconstructs the monolith from the card bodies and hash-compares |
| `--tables` | regenerates the mapping tables in `REORG_MAPPING.md` |

`--verify` reads the archived monolith, strips its banner, recomputes the slice
boundaries from the mapping, then walks the groups in document order pulling bytes
out of each card's verbatim body (one cursor per card file, so a card that owns
several slices is consumed in order). It concatenates them, takes `sha256` of both
sides, and prints them. A single altered, inserted or dropped byte anywhere in any
card body changes the reconstruction hash. It also refuses if a card is missing, if
a card has no verbatim marker or more than one, or if any card has bytes left over
after the walk.

The hashes from the split commit are printed in that commit's body.

---

## 6. The rules that stop it happening again

These go into `docs/INDEX.md` and, in short form, into `CLAUDE.md`:

1. **One card = one file** under `docs/cards/<topic>/<CARD-ID>.md`.
2. **Results are appended to the CARD's file only** — never to a topic page, never to
   `archive/`.
3. **The INDEX line is updated twice**: at pre-registration, and at close.
4. **No non-archive file over ~60 KB.** Split by topic when one approaches it.
5. **Every card file starts with the STATUS block.**
6. **Monoliths are frozen.** `docs/archive/` is read-only; the stub at the old path
   is a routing table, not a document.
7. `scripts/docs_index_check.py` must pass before a docs commit.

`scripts/docs_index_check.py` (new, added in the index commit) enforces 1, 4, 5 and
link integrity: every `.md` under `docs/` is linked from `INDEX.md`; no relative link
dangles; every file under `cards/` and `decisions/` has a STATUS block with the
required keys; no file outside `docs/archive/` exceeds 60 KB. Non-zero exit
otherwise.

---

## 7. Memory files that will want a path update

The author's memory directory is **read-only for this work**. 123 lines across 67
memory files cite one of the 19 split docs, usually as
`docs/SPEC_<name>.md §N.N`. None of them *break* — the stub at the old path resolves
every one of them in one hop — but each would read better pointing straight at the
card file. The author's own directive says so: *"Memory files stay short and point at
the card files, not at monolith sections, once the tree exists."*

| split doc | memory lines | memory files that cite it |
| --- | ---: | --- |
| `CONFIG_promotion_ledger.md` | 17 | `feedback_no_knob_promotion_step.md`, `project_edge1_consumer_link_hrider.md`, `project_event_harness.md`, `project_internal_12h_stability.md`, `project_network_falsified_approaches.md`, `project_network_fullcov_carry.md`, `project_network_phase_leveling.md`, `project_network_ssystem_dia_assessment.md`, `project_pb_gate_v2.md`, `project_s2h_harvest_card.md`, `project_simnet_deep_onoff_campaign_seed.md` |
| `QUEUE_principled_replacements.md` | 3 | `project_iono_carry_8b_campaign.md`, `project_principled_replacement_queue.md` |
| `SPEC_amb_iono_split_overcount.md` | 3 | `project_amb_iono_split_overcount.md`, `project_iftd_accuracy_verdict.md`, `project_oracle_pin_card.md` |
| `SPEC_card1_coupling_carry.md` | 6 | `project_card123_lineage_archive.md`, `project_card1e_gauge_amputation.md`, `project_card1fg_isolation_s14fix.md`, `project_network_fullcov_carry.md` |
| `SPEC_clock_temporal_anchor.md` | 8 | `project_anchor_churn_r2_sticky.md`, `project_clock_temporal_anchor_q3.md`, `project_consumer_instrument_card.md`, `project_fast_clock_card.md`, `project_q3_floor_governed_ladder.md`, `project_simnet_deep_onoff_campaign_seed.md` |
| `SPEC_day129_frame_guards.md` | 4 | `project_day129_gate_policy.md`, `project_day129_pivot_cb_ratchet.md`, `project_iono_deweight_card.md` |
| `SPEC_event_harness.md` | 24 | `feedback_docs_topic_tree_not_monoliths.md`, `feedback_event_harness_over_daylong_runs.md`, `project_closedloop_consumer_vertical_bug.md`, `project_cold_fallback_card.md`, `project_consumer_instrument_card.md`, `project_event_harness.md`, `project_field_harness_card.md`, `project_oracle_pin_card.md`, `project_pivot_departure_card.md`, `project_ref_arc_clause_card.md`, `project_rxcb_gate_frame_blind.md` |
| `SPEC_lane_slip_response.md` | 4 | `project_event_harness.md`, `project_lane_slip_response.md` |
| `SPEC_level_information_budget.md` | 9 | `feedback_verify_symbol_absence_two_tools.md`, `project_extcb_defect2_rootcause.md`, `project_global_sh_iono.md`, `project_level_authority_lv6.md`, `project_level_information_budget.md`, `project_rxcb_gate_frame_blind.md`, `project_s2h_harvest_card.md` |
| `SPEC_measurement_admission.md` | 3 | `project_ft63_fast_tier.md`, `project_network_enhancement_queue.md` |
| `SPEC_phase_windup_sign.md` | 1 | `project_phase_windup_sign_defect.md` |
| `SPEC_pivot_visibility_gauge_defect.md` | 6 | `project_anchor_churn_r2_sticky.md`, `project_carry_datum_handover_theory.md`, `project_pivot_departure_card.md`, `project_pivot_visibility_gauge_defect.md` |
| `SPEC_prior_residual_census.md` | 4 | `project_laser_theory_mapping.md`, `project_sigma0_constraint_aware.md`, `project_zwd_sink_census_2026-09-09.md` |
| `SPEC_redundancy_census.md` | 4 | `project_block_baarda_card.md`, `project_laser_theory_mapping.md`, `project_network_enhancement_queue.md` |
| `SPEC_sbasis_realization_contract.md` | 7 | `project_clock_emit_rebase_card.md`, `project_clock_temporal_anchor_q3.md`, `project_network_falsified_approaches.md`, `project_product_legend_schema_fix.md`, `project_ssystem_theory_reference.md` |
| `SPEC_slip_qc_hardening.md` | 6 | `project_fast_clock_card.md`, `project_ifphase_mw_slip_agreement.md`, `project_network_phase_leveling.md`, `project_zwd_sink_census_2026-09-09.md` |
| `SPEC_synthetic_minimum_network.md` | 11 | `project_datum_wander_mechanism.md`, `project_o2_carry_invariant_derivation.md`, `project_rt_report_surveyed_reference.md`, `project_seam_continuity_card.md`, `project_sim_field_n_regime.md`, `project_sim_injection_bench.md`, `project_simnet_report_lane.md`, `project_slant_triple_handoff.md`, `project_slip_relevel_walk_campaign.md`, `project_synthetic_minimum_network.md`, `project_wsh_synthetic_arm.md` |
| `SPEC_t3_null_derivation.md` | 2 | `project_t3_null_derivation.md` |
| `SPEC_withhold_era_release.md` | 1 | `project_withhold_era_release.md` |

Total: 123 lines across 67 memory files.

Reproduce the line-level list with:

```bash
grep -rn 'SPEC_event_harness\|CONFIG_promotion_ledger\|SPEC_amb_iono_split_overcount' \
  ~/.claude/projects/C--GnssProcessor/memory/
```

---

## 8. Commit order

| # | contents |
| --- | --- |
| 1 | `docs/REORG_PLAN.md` + `docs/REORG_MAPPING.md` — this plan, committed **before** anything moves |
| 2 | `scripts/docs_split.py` + `scripts/docs_map/*.json` + the split output (cards, decisions, archive, stubs); `--verify` hashes in the commit body |
| 3 | `docs/INDEX.md`, `docs/topics/*.md`, `docs/status/OPEN_ITEMS.md`, `scripts/docs_index_check.py`, the `CLAUDE.md` Documentation section |
| 4 | `config/archive/test_suite/` (unreferenced arm configs, extends chains intact) + `config/test_suite/README.md` + `scripts/README.md` |
