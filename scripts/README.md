# scripts/

Every file in `scripts/`, grouped by the documentation topic it serves. The purpose
column is the script's **own** docstring or header comment, trimmed — if it is wrong,
fix the script, not this table.

**Nothing was moved in the first pass.** Script paths are cited from `CLAUDE.md`, `docs/`,
arm configs, other scripts and the author's memory files, so a rename is a separate,
reference-scanned change. What the first pass added is the grouping and the **archive
candidates**: the 36 scripts that nothing outside themselves referenced.

**Archive pass, 2026-10-17.** The candidates were scanned again (the tracked tree, the
author's memory files and the heads of the open branches). Five had gained a reference since
and stay where they are, with the references counted in the "refs" column: `wt_build.ps1`
(`CLAUDE.md`), `fetch_coastline.py` (`pyviz/gnssviz/coastline.py`), `cm_make_arms.py` (the
golden manifest), `pv_consumer_read.py` (`refound_grade.py`) and `view_network_dd_fractional.py`
(the pyviz viewer and its test). The other 31 moved with `git mv` to `scripts/archive/`,
unedited, and are listed in the last section. The counts below are those of the first pass.

| | |
| --- | --- |
| Total | 249 files + `docs_map/` (the split's mapping tables) |
| Referenced somewhere | 213 |
| Unreferenced (archive candidates) | 36 (31 archived, 5 referenced since) |

Topic pages live in `docs/topics/`; the index is `docs/INDEX.md`.


## Build, test and repo tooling

The wrapper, the unit-suite runners, the docs gate.

| script | purpose | refs |
| --- | --- | --- |
| `gen_ppprtk_suite.py` | Generate the PPP-RTK network+user constellation test-suite configs. Each combo is ONE ``ppp-rtk-network`` producer over a 50-station global subnet (`… | 1 |
| `run_batch.py` | run_batch.py Batch-run @c gnss_processor.exe across a grid of (station, date) cells. Each cell gets: - A generated per-run JSON config (template + st… | 2 |
| `run_ppprtk_suite.py` | Run the PPP-RTK network+user constellation test suite and report. For every combo config in --config-dir this: 1. runs gnss_processor.exe <config> wi… | 1 |
| `run_test_slice.py` | run_test_slice.py -- thin slice of the "standard test run" harness. Runs ``gnss_processor.exe`` over a small matrix of (station x config-cell) on the… | 3 |
| `run_test_suite.py` | run_test_suite.py -- manifest-driven "standard test run" harness. Runs ``gnss_processor.exe`` over a matrix of (station x config-cell) defined in a J… | 8 |
| `vs2026_build.ps1` | compile check (Debug is for compile checks and gnss_tests ONLY -- never data runs) | 29 |
| `cpp_lex.py` | The C++ comment and string-literal lexer shared by the source-reading scripts (`strip_comments`, `string_literals`). Moved here from `rt_seam_verify.py` when that gate was retired. | 3 |
| `wt_build.ps1` | THE ONE DIFFERENCE from scripts\vs2026_build.ps1: root from $PSScriptRoot. | 1 |

## Documentation tree

The split, its hash proof, and the index gate. See `docs/INDEX.md`.

| script | purpose | refs |
| --- | --- | --- |
| `docs_index_check.py` | Gate for the docs/ topic tree. Run it before any docs commit. | 6 |
| `docs_split.py` | Split a docs/ monolith into one-card-per-file, VERBATIM and verifiably. | 6 |
| `gen_docs_index.py` | Generate docs/README.md — a navigable index of the docs/ corpus. python scripts/gen_docs_index.py # write docs/README.md python scripts/gen_docs_inde… | 1 |

## Config generation and posture

Goldens, arm generators, posture checks.

| script | purpose | refs |
| --- | --- | --- |
| `config_posture_check.py` | Config POSTURE check - which validated-good knobs is this config leaving OFF? The project ships new work default-OFF + byte-identity-gated (standing … | 44 |
| `extprod_ar_make_configs.py` | Build the external-product integer-AR probe arms (DOY-129, 50-sta GPS-only tri-freq). Three arms, identical in EVERY respect except how the satellite… | 1 |
| `gen_net305_sh.py` | Generate 305-station global SH-VTEC PPP-RTK network configs. Merges the 305-station roster (from config/test_suite/_net305_3sys_smoke.json) into the … | 1 |
| `gen_network_config.py` | Generate a PPP-RTK network config from a resolved station list. Loads the committed network config as a template (settings, estimation, filter, model… | 1 |
| `gnss_config_chain.py` | Resolve `"extends"` config inheritance the way the C++ loader does. Commit 18b4502 gave configs a base+delta inheritance format. Every offline tool t… | 24 |
| `loader_keys.py` | Which config keys the C++ loader reads, judged from its source (`src/core/config`), including the paths it builds from a prefix constant, a class table or a logger name. `rapid_arm.py`, `prm_ab.py` and `ird_make_arms.py` refuse an arm that sets a key the loader ignores, which would run as its base; `rapid_arm.py make --allow-unread KEY` lets a named key through. `config CONFIG.json` lists a config's unread keys; `selftest`. | 3 |
| `make_agu_configs.py` | Generate the AGU-abstract network configs from the FT63 pdfix_v3 lineage. The abstract needs ONE producer run whose emitted products a held-out consu… | 1 |
| `ledger_render.py` | Render docs/CONFIG_promotion_ledger.md from the ledger JSON (one verdict per config key); `--check` exits 1 when the view is stale. | 3 |
| `make_golden_config.py` | make_golden_config.py -- generate and VERIFY the golden network configs. WHY. The project ships new work default-OFF and byte-identity-gated, and the… | 101 |
| `overnight_make_arms.py` | Generate the overnight follow-on arm configs for the FT63 convergence lineage. Every arm is derived from the validated `_ft63_24h_conv.json` posture … | 1 |
| `p2_make_guard_configs.py` | Generate the SPEC_day129_frame_guards Phase-2 ARMED configs. Derives config/test_suite/_day129_guards_conv.json from _night305_conv.json by ADDING th… | 5 |
| `raim_cold_event_config.py` | Cold-start guards-armed window over the 20:47:00 excursion (rapid-iteration probe). Derives runs/raim_cold_event/leg.json from the Phase-2 guards con… | 1 |
| `s2z_make_config.py` | Generate the SPEC_s2z_realization_ab arm config (sibling of p2_make_guard_configs.py). Derives config/test_suite/_day129_s2z_conv.json from _night305… | 1 |
| `simnet_make_producer_config.py` | Generate a network producer config from a synth-network fixture manifest. Exists so the station block can never drift from the fixture. The producer … | 78 |
| `simnet_make_roster.py` | Generate a globally-distributed station roster for a synth-network sim config. Why a generator: 50 hand-written coordinate triples is precisely where… | 28 |

## Data, products and fixtures

Fetching, converting and laying out inputs.

| script | purpose | refs |
| --- | --- | --- |
| `bootstrap_internal_data.py` | bootstrap_internal_data.py Populate ``data/`` for internal-mode GnssProcessor runs. Internal mode (``data_source = "internal"`` in the JSON config) e… | 5 |
| `test_bootstrap_auth.py` | Tests of the Earthdata login in `bootstrap_internal_data.py` (netrc resolution, Basic credentials, the redirect and the session cookie) against loopback servers; no network. | - |
| `rinex_ids.py` | The canonical satellite id of a RINEX 3 record (`G 7` is `G07`, as `RinexParser.cpp` reads it). The raw-observation readers (`mw_census.py`, `ewl_census.py`, `extcb_gim_verdict.py` and ten more) key on `norm_sat(line[0:3])`. `lint` lists the readers that still slice the id raw; `selftest`. | 14 |
| `decimate_rinex3_obs.py` | Decimate a RINEX 3 OBSERVATION file to a coarser epoch interval. Why this exists: `processing.gnss_rate_s` is applied ONLY in `Application::run` (src… | 3 |
| `extract_vmf3_coeffs.py` | Extract VMF3 spherical harmonic coefficients from vmf3.m and emit a C++ header. The TU Wien VMF3 reference implementation (https://vmf.geo.tuwien.ac.… | 2 |
| `fetch_coastline.py` | Regenerate the vendored world-coastline asset used by the pyviz geo viewers. The network-level visualizations (``viewers/network_map`` and the ionosp… | 1 |
| `fetch_global_obs.py` | Fetch RINEX-3 daily obs for a station list from CDDIS into the data tree. Reads ``global_network_stations.json`` (from select_global_stations.py), pu… | 1 |
| `fetch_sinex_subset.py` | fetch_sinex_subset.py -- freeze a small station subset out of a SINEX/SSC. IGS / ITRF SINEX coordinate products are tens of MB (every global station)… | 1 |
| `migrate_data_layout.py` | migrate_data_layout.py One-shot migrator from the legacy *flat* GNSS data layout to the unified *date-first* layout used by ``PathResolver`` and ``Pr… | 1 |
| `refresh_atx.py` | Keep the IGS ANTEX (igs20.atx) antenna model up to date. The IGS ``igs20.atx`` at files.igs.org is a single, continuously-updated file: new satellite… | 5 |
| `resolve_itrf_ecef.py` | Resolve ITRF2020 (IGS SSC) station ECEF coordinates at an observation epoch. Wraps the test-suite's authoritative ``SinexTruth`` parser (``pyviz/gnss… | 9 |
| `select_global_stations.py` | Select a globally-distributed subset of IGS stations from a CDDIS day listing. Input: a CDDIS daily-directory listing (``curl '<dir>*?list'``) for a … | 2 |

## Event harness

Leg launch and grading for `docs/topics/harness.md`.

| script | purpose | refs |
| --- | --- | --- |
| `ev_coldfb_census.py` | COLD ANCHOR-FALLBACK card, STEP 1 — the family-selection census (no runs). Answers, from a seed run's OWN traces, the question the card's harness leg… | 2 |
| `ev_consumer_invariant_grade.py` | The DATUM-INVARIANT consumer census (SPEC_event_harness §15.10). Why it exists. The storm arms of the cold anchor-fallback card were graded on the cl… | 6 |
| `ev_dose_legs.py` | EVENT HARNESS — slip-DOSE legs (Study 4, docs/SPEC_event_harness.md §12). The single-event legs price ONE event. A dose leg prices a SCHEDULE: a conv… | 6 |
| `ev_launch.ps1` | Detached run launcher for the event harness: runs the producer with cwd=rundir (so ./logs/ cannot collide with a concurrent run) and always writes ex… | 9 |
| `ev_mw_detect_census.py` | EVENT HARNESS — census of 'MW slip detected' lines in mw_slip_detector*.log: lane wavelength, arc age, evidence ratio \|dev\|/thresh, integer known?,… | 2 |
| `ev_oracle_pin_grade.py` | ORACLE PIN grader — gates G1..G4 of docs/SPEC_oracle_pin.md. Every cell comes from a TRUTH grader, never a raw wire step (standing invariant #5 / the… | 2 |
| `ev_oracle_pin_legs.py` | ORACLE PIN driver — the leg matrix for docs/SPEC_oracle_pin.md. The card (SPEC_event_harness.md §17): hold some or all of the producer's float ambigu… | 2 |
| `ev_seam_grade.py` | WARM-RESUME SEAM grader (card RS-2) — a NULL leg against its own SEED's traces. python scripts/ev_seam_grade.py runs/evleg_rs_null_nocarry \ --seed r… | 7 |
| `ev_slip_census.py` | EVENT HARNESS — detector-coverage census over injected slips (SPEC_event_harness §13.1): for each schedule row, what caught it (within +0..+2 epochs,… | 2 |
| `ev_slot.py` | Box-wide PRODUCER SLOT locks for the event harness (standing cap: 2 concurrent gnss_processor runs). python scripts/ev_slot.py claim <worker> [--max-… | 3 |
| `ev_study1_legs.py` | EVENT HARNESS Study 1 driver — make + run the erase-vs-refloat leg matrix. One leg per (arc, mode). Every leg resumes from the SAME snapshot as the v… | 4 |
| `ev_study1_table.py` | EVENT HARNESS Study 1 — the erase-vs-refloat summary table. Runs ``scripts/event_grade.py --json`` for every leg of the matrix against the single NUL… | 4 |
| `ev_study3_legs.py` | EVENT HARNESS Study 3 driver — the BAND-SCOPED response leg matrix (§Z). Study 1 measured that the persistent ~0.45-cycle level floor an erase or a w… | 3 |
| `ev_study3_table.py` | EVENT HARNESS Study 3 — the BAND-SCOPED response summary table (§Z). Same reduction as ``ev_study1_table.py`` (which it reuses wholesale), over the S… | 2 |
| `ev_study7_gates.py` | Study 7 storm-arm gates (SPEC_event_harness s16.4-s16.5; promoted from the manager scratchpad 2026-09-11) W1/W2/G4/W3(handovers) for arm B against A … | 3 |
| `event_dose_grade.py` | EVENT HARNESS — the slip-DOSE grader (Study 4, docs/SPEC_event_harness.md §12). ``scripts/event_grade.py`` prices ONE event on ONE arc; ``event_frame… | 5 |
| `event_field_grade.py` | FIELD EVENT-HARNESS grader — truth-free, everything is EVENT minus NULL. python scripts/event_field_grade.py runs/evleg_fld_e1_off \ --null runs/evle… | 6 |
| `event_frame_grade.py` | EVENT HARNESS — the STATION-FRAME channels (Study 2's instrument). ``scripts/event_grade.py`` grades the injected ARC and the WIRE. This grades the s… | 13 |
| `event_grade.py` | EVENT HARNESS grader — one injected event, read against its NULL leg. python scripts/event_grade.py runs/evleg_<name> --null runs/evleg_<null> \ --fi… | 19 |
| `event_leg.py` | EVENT HARNESS — snapshot seed writer + single-event resume-leg generator. The point of the harness is stated by the directive that motivated it: *run… | 44 |

## Simulated truth bed (simnet)

Fixture build, truth grading, reporting — `docs/topics/sim-bed.md`.

| script | purpose | refs |
| --- | --- | --- |
| `s2z_p2_grade.py` | SPEC_s2z_realization_ab Phase-2 grading — P1(a,c) + P2 (wire damage). All statistics datum-invariant (standing invariant #5 is doubly binding: the tw… | 1 |
| `s2z_smoke_check.py` | SPEC_s2z_realization_ab Phase-0/1 smoke assertions (staged-cadence gate). Run against the 10-epoch s2z smoke (runs/s2z_ab/smoke) BEFORE the 6 h seed … | 1 |
| `simnet_amb_integer.py` | SYNTHETIC MINIMUM NETWORK — the ambiguity-INTEGER grader (docs/SPEC_synthetic_minimum_network.md §13.6 (method rules §13.8); SPEC_synthetic_minimum_n… | 307 |
| `simnet_bridge_truth.py` | Truth audit of relevel delta-bridge REPLACE events against the sim fixture. Joins a run's ``bridge_replace`` event rows (logs/network_events.csv, tie… | 2 |
| `simnet_common_mode.py` | SYNTHETIC NETWORK — fleet-common C-record wander extractor / A-B comparator. docs/SPEC_synthetic_minimum_network.md §9.8 (the datum-wander arm). On t… | 5 |
| `simnet_dd_reref.py` | REFERENCE-SATELLITE re-referencing of the DD-integer grade (SPEC_amb_iono_split_overcount §6.12, 2026-09-06). The producer's DD trace (network_dd_fra… | 27 |
| `simnet_drift_age.py` | simnet_drift_age.py -- STABILITY target 1: settled late drift vs carry age. The 12 h quiet flagship (_camp60wu_quiet12h_r2q3) rises h7->h11 in settle… | 3 |
| `simnet_exchange.py` | SYNTHETIC W6+SH ARM — the clock <-> receiver-DCB <-> ionosphere EXCHANGE signature. docs/SPEC_synthetic_minimum_network.md §11 item 4. WHY THIS TOOL … | 14 |
| `simnet_grade.py` | SYNTHETIC MINIMUM NETWORK — the truth grader (docs/SPEC_synthetic_minimum_network.md §4). The first ABSOLUTE grade the network producer has ever had.… | 56 |
| `simnet_inject_arms.py` | Build the FAULT arm of a slip-injection bench from an already-synthesized NULL arm. null fixture --(this script)--> fault sim config + injection mani… | 13 |
| `simnet_inject_grade.py` | Grade a NULL/FAULT injection arm pair: detection, latency, false alarms, MDB. python scripts/simnet_inject_grade.py \\ --null-run runs/inj_null/run -… | 7 |
| `simnet_injection_preflight.py` | Injection PRE-FLIGHT: is the mechanism you are about to inject at even ALIVE? A synthetic event-injection bench answers "does the response mechanism … | 6 |
| `simnet_iono_interaction.py` | Per-LOS ionosphere interaction grade — the SPEC_t3_null_derivation §7.8 decomposition, promoted from the scratch join that produced the 0.764 m numbe… | 13 |
| `simnet_lane_slip_age.py` | simnet_lane_slip_age.py -- WHERE does the lane-integer slip cost live? Follow-up to the wu93 campaign finding (SPEC_slip_relevel_walk.md section 9): … | 5 |
| `simnet_level_authority_grade.py` | LV-6 G1 grader — the census operator applied to TRUTH. `docs/SPEC_level_authority_internal.md` §4 G1 / §5 item 5. The gate is corr(L_cs(t), censusOpe… | 2 |
| `simnet_make_seam_leg.py` | Generate a warm-resume BRANCH LEG of the 24 h quiet bed from its .ep snapshot library. STABILITY campaign (SPEC_synthetic_minimum_network §18.7 / §18… | 13 |
| `simnet_oracle_grade.py` | ORACLE SLIP-RESPONSE REPAIR-EFFECT CENSUS — did the truth-fed repair actuate? `network.cycle_slip.oracle_relevel` (include/slip/SlipOracle.hpp… | 2 |
| `simnet_plateau_decompose.py` | Plateau decomposition for the post-relevel-fix stationary ZWD/iono plateau (QUEUE 8b). Promoted from the 2026-08-20 session scratch join (docs/SPEC_i… | 8 |
| `simnet_producer_ar_grade.py` | TRUTH GRADE of the producer's Phase-2 AR search (docs/network_ambiguity_resolution_design.md, the 2026-09-10 note §4.1: "a fix count is not a grade")… | 11 |
| `simnet_report.py` | One-command HTML dashboard for a SIMULATED NETWORK run. Chains the two halves that already exist, in the order the artifacts flow: 1. ``scripts/simne… | 7 |
| `simnet_sat_clock_census.py` | simnet_sat_clock_census.py -- STABILITY target 3: per-sat settled clock tails. The 12 h quiet bed shows metre-class SETTLED clock tails on a few sate… | 13 |
| `simnet_seam_tools.py` | simnet_seam_tools.py -- seam / wire-realization instruments for gnss_network_product v2. Seven read-only instruments over the producer wire (products… | 7 |
| `simnet_slip_census.py` | Organic-slip detection census — truth_slips (ALL rows, not just scripted) joined against the run's slip/RAIM response events. The injection bench (si… | 15 |
| `simnet_storm_grade.py` | SLIPPED-BED STORM-ARM GRADER — the pre-registered gate table of SPEC_event_harness §Z.6 / §Z.7 in one command. Usage (from C:/GnssProcessor, Release … | 14 |
| `simnet_t4_census.py` | T4 ATTRIBUTION-ARC CENSUS — what does the T4 "attrib_arc" tier actually erase? On the synthetic SLIPPED bed the network producer's T4 attribution tie… | 14 |
| `simnet_wire_integer_consistency.py` | WIRE INTEGER CONSISTENCY -- is the PRODUCER's emitted (satellite clock + phase bias) integer-consistent between satellites, measured DIRECTLY against… | 11 |

## Network vehicle, datum and the card drivers

`docs/topics/network-vehicle.md`.

| script | purpose | refs |
| --- | --- | --- |
| `ce_make_arms.py` | Card CE arm generator (SPEC_event_harness s24.14): the CHAIN-ERA bisect on the LEVELING vehicle. python scripts/ce_make_arms.py [--base config/test_s… | 1 |
| `characterize_storm.py` | Characterize a network null-space "storm": decompose the below-cutoff near-null directions of the shared border by CLASS, constellation and culprit s… | 6 |
| `check_solve_identity.py` | Check that two runs share a byte-identical SOLVE (standing invariant #6). An OUTPUTS-only knob (an emission filter, a trace, a legend) must leave the… | 1 |
| `cm2_make_arms.py` | Card CM2 arm generator (SPEC_event_harness s24.12): the s24.8 knob arms on the LEVELING vehicle. python scripts/cm2_make_arms.py [--base config/test_… | 2 |
| `cm_grade.py` | Card CM grader (SPEC_event_harness s24.8, CODE-MODEL CONSISTENCY): the five reads per arm on the LAST HOUR of a 2 h cold arm (k = 121..240, k = 1 at … | 8 |
| `cm_make_arms.py` | Card CM (SPEC_event_harness s24.8, CODE-MODEL CONSISTENCY): write the six single-knob COLD arm configs from the field golden and the two trace-merge … | 2 |
| `d5v5_grade.py` | D5 v5 (``network.carry_invariant_innov_carried_only``) A/B grader. The knob restricts the Card-4 D5 innovation gate to chords genuinely CARRIED into … | 2 |
| `d5v5_prodiff.py` | Which RECORD KINDS of two ``products.txt`` wires differ, and by how much. A sha mismatch says "not byte-identical" and nothing else. For a default-OF… | 2 |
| `d5v5_seam_ab.py` | A/B two ``ev_seam_grade.py`` JSONs on the V3 no-harm channels of the D5 v5 card. ``ev_seam_grade.py`` grades ONE leg against the continuous seed it r… | 3 |
| `datum_gauge_audit.py` | Audit the single-pivot phase-bias gauge: which clock re-realizations were AVOIDABLE. Two modes, and the important one needs NO truth: --run <dir> POS… | 12 |
| `field_eclipse_census.py` | field_eclipse_census.py -- wire-only per-sat exclusion/return census. The field analog of simnet_sat_clock_census (STABILITY targets 1/3, SPEC_ synth… | 2 |
| `field_wire_datum_probe.py` | FIELD WIRE — datum-station attribution probe (SPEC_synthetic_minimum_network §9.9). The §9.8 sim mechanism: under the legacy pivot emit the wire's fl… | 5 |
| `field_wire_phase_clock_census.py` | FIELD WIRE — per-satellite PHASE-CLOCK wander census + consumer geometry closure. WHY --- On the DOY-129 63-station field bed the closed-loop consume… | 5 |
| `fp1_gate_table.py` | FP-1 gate table — the §20 G-A..G-E bars applied to a PIN/OFF field leg matrix. python scripts/fp1_gate_table.py [--runs C:/GnssProcessor/runs] [--jso… | 4 |
| `fp1_refslip_census.py` | REFERENCE-SLIP FAN-OUT census for the FP-1 field matrix. python scripts/fp1_refslip_census.py [--runs C:/GnssProcessor/runs] [--prefix fp1] [--json o… | 4 |
| `fp2_leg_verify.py` | FP-2 per-leg PROVENANCE verification — the checks a gate table cannot do. python scripts/fp2_leg_verify.py [--runs C:/GnssProcessor/runs] [--prefix f… | 1 |
| `grade_agu_arms.py` | Tabulate held-out consumer performance across runs / replay arms. Reads each run's ``logs/closed_loop_trace.csv`` and reports the FINAL-HOUR median e… | 1 |
| `grade_pdfix_arms.py` | Side-by-side arm comparator for the PD-safe re-ask queue (docs/RUNQUEUE_pdfix_reask.md). python scripts/grade_pdfix_arms.py <run_dir> [<run_dir> ...]… | 8 |
| `neq_svd.py` | Offline spectral analysis of a network NEQ export (logs/neq_export/). Usage: python neq_svd.py <export_dir> [n_weak] 1. Loads the full arrowhead (bor… | 3 |
| `pv_consumer_read.py` | Card PV consumer read (SPEC_event_harness s24.11, the s24.3 instrument): the held-out closed-loop consumer's error against the roster position it alr… | 1 |
| `tropo_arc_wander.py` | The FOURTH member: does the per-epoch ZWD state close the two-band budget of the DD-ambiguity/iono co-motion? (SPEC_amb_iono §6.15.3a item 5, peer de… | 3 |
| `zp_station_block_read.py` | Read `logs/network_station_block.csv` — the card-ZP per-station LOCAL-BLOCK audit. python scripts/zp_station_block_read.py <run_dir> [<run_dir> ...] … | 2 |

## Warm resume and the seam

`docs/topics/resume-seam.md`.

| script | purpose | refs |
| --- | --- | --- |
| `diff_carry_blobs.py` | RTB-7(d) RS card BR-2 — branch-faithfulness instrument: diff two carry blobs. Between-sat SD of CLK is an omnibus readout at the END of the station-l… | 11 |
| `rs9_census.py` | RS-9 census (read-only): the rx-CB withheld share and the withholding SET. python scripts/rs9_census.py <run> [<run> ...] [--lo 277320] [--hi 279090]… | 2 |
| `run_warm_resume_arm.sh` | RTB-7(d) RS-card harness: run ONE warm-resume arm under the HB-2 protocol. | 3 |
| `warm_resume_sd_metric.py` | RTB-7(d) RS card — warm-resume divergence metric (between-satellite SD-of-SD). Grades how far a resumed producer's EMITTED products drift from an inl… | 3 |

## Clock

`docs/topics/clock.md`.

| script | purpose | refs |
| --- | --- | --- |
| `clock_level/` | 31 graders, census drivers and offline wire tools of the clock-level cards (CLV, CLV2, HLT, CTU, CTU2, LAH, PVN, SMH, SMD, QPV, PRM): the 2 h and 6 h consumer censuses, the level and anchor graders, the hold rewrite tools, run identity, the SMH G3 walk. See `clock_level/README.md`. Added 2026-09-30. | cards |
| `clk_step_episodes.py` | CLK_STEP episode census over network_events.csv. The per-mark count is a bad discriminator (SPEC_pivot_visibility_gauge_defect §5.0.1: 9 of 16 marks … | 2 |
| `clock_decompose.py` | Decompose a run's emitted satellite-clock series into drift / residual / step noise. python scripts/clock_decompose.py <run_dir> [tail_seconds=3600] … | 1 |
| `clock_hadamard_calib.py` | Two-state clock-model q1/q2 recalibration instrument (Phase A of the ``network.clock_model`` revival card; write-up ``docs/SPEC_c1_clock_rate_validat… | 7 |
| `fc1_grade.py` | **Archive-only** (card RKD deleted the mechanism it grades; it reads archived runs). Grade the FC-1 split-cadence fast clock against the full solve. Two modes, both grading DATUM-INVARIANT quantities only (standing invariant #5 — raw … | 7 |
| `q3_clock_structure_function.py` | **Archive-only** (card RKD deleted the mechanism it grades; it reads archived runs). Card Q3 — floor-vs-walk split of the phase-measured (TDCP) satellite clock correction, from the FC-1 shadow trace (network_fast_clock.csv). The per-t… | 6 |
| `q3_fc1_hours.py` | **Archive-only** (card RKD deleted the mechanism it grades; it reads archived runs). The FC-1 SHADOW HOUR TABLE — the score for Card Q3 work. ``project_fast_clock_card``: *"KEEP network.fast_clock.shadow as a standing instrument ... i… | 2 |
| `q3_ladder_grade.py` | Card Q3 ladder leg — one table per arm, scraped from the run console (SPEC §8). Everything here is an ON-PATH COUNTER or a run-end statistic the prod… | 4 |
| `q3_ladder_queue.ps1` | Card Q3 ladder leg — the serialized 3 h arm queue (SPEC §8). | 2 |
| `q3_ladder_queue2.ps1` | Card Q3 ladder leg — phase 2: the §3 k-CALIBRATION arms, then the consumer REPLAY of every graded arm (SPEC §8.4 gates 3 and 6). | 2 |
| `q3_ladder_run.ps1` | Card Q3 ladder-leg arm runner (docs/SPEC_clock_temporal_anchor.md §8). | 15 |
| `q3_make_replay.py` | Generate consumer-REPLAY configs for Card Q3 ladder arms (SPEC §8.4 gate 3). The live closed-loop consumer runs inside the producer arm and is one in… | 3 |
| `q3_mark_tieout.py` | Card Q3 §3 — the wire-mark TIE-OUT for one arm (SPEC §8.4 gate 5). A trip is graded as a POPULATION, and clause (b) of that grading is "it lands a wi… | 4 |
| `q5_make_replay_arm.py` | Generate a Q5 consumer-replay arm config (SPEC_day129_frame_guards Phase 0). The replay driver (`Application::runConsumerReplay`, armed by `network.c… | 3 |
| `q5_reanchor_hourly.py` | Re-anchor an already-run Q5 consumer-replay arm onto a different truth ECEF. Phase 0 of ``docs/SPEC_day129_frame_guards.md`` graded consumer NRMG00NC… | 4 |

## Ionosphere and the level

`docs/topics/ionosphere.md`.

| script | purpose | refs |
| --- | --- | --- |
| `analyze_net_sh.py` | Analyze a 305-station global SH-VTEC network run for field divergence. Reads logs/network_vtec_sh.csv (week,tow,n,m,trig,value_tecu,std_tecu) and log… | 2 |
| `census_gim_operator.py` | LV-6 gate G2 — censusOperator{GIM}, the sampling-matched GIM reference. `docs/SPEC_level_authority_internal.md` §4 G2 / §5.2. The gate is settled L_c… | 2 |
| `extcb_gim_verdict.py` | GIM-referenced closing analysis for DEFECT 2. Per LOS (29 stations x ~10 sats x 5 epochs, el>20): y_tecu = [(P2W-osb2)-(P1C-osb1)]/KGF/M_TECU (slant … | 14 |
| `extcb_raw_if_check.py` | Estimator-free per-satellite IF code residuals from raw RINEX + COD products. resid(sta,sat) = IF(C1C-osb, C2W-osb) - [ \|r_rx - r_apc\| - c*dt_CLK -… | 6 |
| `field_apriori_clk_probe.py` | FIELD WIRE — a-priori CLK common-drift probe (SPEC_synthetic_minimum_network §9.9.1). §9.9 eliminated the receiver-code driver of the field wire's co… | 3 |
| `field_iono_pair_agreement.py` | FIELD — cross-station iono-field agreement from raw GF (SPEC §9.10). Two stations tracking the SAME satellite sample nearby columns of the same ionos… | 3 |
| `field_vs_gim_footprint.py` | Compare the network's SH-VTEC field with the GIM GLOBALLY and over its FOOTPRINT. Why this exists. `scripts/gim_global_mean_series.py` compares a00 w… | 13 |
| `fleet_bias_diurnal.py` | Diurnal sweep of the receiver fleet GF-bias term (SPEC §17 item 0's diagnostic rider). Same estimator-free instrument as scripts/extcb_gim_verdict.py… | 5 |
| `gim_global_mean_series.py` | Grade a network run's SH a00 against the GIM's OWN global mean VTEC, epoch by epoch. Why this exists. Every level verdict on the EXT-CB ladder so far… | 20 |
| `higher_order_iono_reference.py` | Independent reference oracle for HigherOrderIonosphere (I2 + I3). Locks the May-2026 second/third-order ionosphere fixes: 1. I2 (second-order, group/… | 2 |
| `iono_arc_wander.py` | Independent check of the DD-ambiguity wander against the ESTIMATED SLANT IONOSPHERE (SPEC_amb_iono §6.15.3a). If the per-band DD wander were iono-bor… | 3 |
| `lvseed_read.py` | SPEC_event_harness §24.13 — card LV-SEED: where the remaining per-LOS level is born. READ-ONLY. Four pre-registered reads on EXISTING runs (no produc… | 2 |
| `sh_vtec.py` | Synthesize the network's spherical-harmonic VTEC field from its coefficients. A faithful Python port of ``SphericalHarmonicIonosphere`` (basis + norm… | 13 |

## Code bias and admission

`docs/topics/code-bias.md`.

| script | purpose | refs |
| --- | --- | --- |
| `adm_gate_verdict.py` | Grade the docs/SPEC_measurement_admission.md §7 gates from the arm run dirs. Companion to ``adm_make_arms.py``. Reads only committed artifacts (``net… | 2 |
| `adm_make_arms.py` | Build the validation arm configs for docs/SPEC_measurement_admission.md. Every arm derives from ONE base so the estimation configuration is identical… | 4 |
| `cb_structure_function.py` | Card CB-Q -- floor-vs-walk split of the EMITTED satellite code-bias wire. The CB analogue of ``q3_clock_structure_function.py``. For each (constellat… | 7 |

## Cycle slips, lanes and DD fractions

`docs/topics/slip.md`.

| script | purpose | refs |
| --- | --- | --- |
| `check_card6_relevel.py` | Judge a brdc_rts armed run against RTF-1 Card 8 criteria 2 & 3 (Card 6 re-level). Consumes two CSVs the run writes (network.trace_events + service_mo… | 3 |
| `compare_wl_vs_census.py` | Score the engine's MW widelane integers against the estimator-free MW census. WHY --- ``AmbiguityResolver::resolveWlConstrained`` anchors the narrow-… | 1 |
| `dd_arc_stability.py` | UNWRAPPED, truth-free DD scale readout: per-chord ARC STABILITY of the traced DD estimate — v3. A chord (sat_i, ref_sat, sta_a, sta_b) has ONE true i… | 4 |
| `dd_frac_reref.py` | TRUTH-FREE reference re-referencing of the DD fractional histogram (SPEC_amb_iono_split_overcount §6.13). The companion `simnet_dd_reref.py` grades t… | 16 |
| `dd_frac_wl.py` | Synthesize a WIDE-LANE (1,2) DD-fractional trace from a run's per-band network_dd_fractional.csv, in the SAME schema, so scripts/dd_frac_reref.py can… | 3 |
| `ewl_bridge_oracle.py` | Estimator-free oracle of the WL-2 bridge arc (SPEC_ewl_wl_cascade_card §3.4). Chain, per station-pair/sat-pair DD, straight from raw RINEX (GPS, W/Q … | 1 |
| `ewl_census.py` | Estimator-free EWL/WL integer census (WL-2 spec eyeball). Extends scripts/mw_census.py to the tri-frequency lanes: for each pair the MW combination i… | 1 |
| `field_lane_frac.py` | field_lane_frac.py -- TRUTH-FREE lane-fraction grade for field (and sim) runs. The per-band DD fraction is iono-limited and uniform on every field li… | 7 |
| `field_lane_obs_grade.py` | **Archive-only** (card RKD deleted the mechanism it grades; it reads archived runs). Grade a lane-observatory run's per-event LANE_OBS rows against the SPEC_lane_slip_response §7.6 pre-registered field rule. Input: a run directory who… | 4 |
| `grade_dd_spillway.py` | Spillway forensics on network_dd_fractional.csv — the 2026-08-02 root-cause kit. Four analyses on the traced between-station DDs (all datum-invariant… | 13 |
| `grade_ddfrac.py` | Grade a network producer run's DATUM-INVARIANT double-differenced ambiguities. This is the go/no-go instrument for producer-side (and downstream user… | 41 |
| `grade_ddfrac_perhour.py` | Per-hour + per-bucket DD-step breakdown for the Card-4 D5 acceptance. Reuses grade_ddfrac's EXACT methodology (epoch-adjacent \|ΔDD\| within a fixed … | 7 |
| `grade_ddfrac_sigma_conditioned.py` | Condition the DD fractional part on its FORMAL sigma — the payoff test of ``docs/SPEC_carry_aware_covariance.md``. WHY THIS SCRIPT EXISTS -----------… | 6 |
| `grade_lane_trace.py` | **Archive-only** (card RKD deleted the mechanism it grades; it reads archived runs). Grade the WL-2 lane trace (F-B instrument): injected/held-cycle circR vs control. Input: logs/network_wl_lane_trace.csv from a run with network.mw_wl… | 7 |
| `grade_slip_agreement.py` | Grade the network slip layer by CROSS-CHECKING its two detectors against each other and against the raw observables. The network producer runs two in… | 12 |
| `lane_spectrometer.py` | Lane-ladder spectrometer — project the network's own float DDs onto the MW/EWL lane ladder and grade them against the MW integers (docs/SPEC_lane_spe… | 24 |
| `mw_census.py` | Estimator-free WL integer census: MW combination from raw RINEX, first N epochs. Median MW per (station,sat) arc -> star-basis DD within same band-2 … | 15 |
| `mw_census_allpairs.py` | Estimator-free ALL-PAIRS MW census (UWL-1 ladder survey, Jul-16 2026). Every channel combination across DISTINCT frequency bands, per constellation (… | 7 |
| `mwwl_event_census.py` | Estimator-free lane-information census across an EVENT WINDOW (DOY-129 19:26 detonation). Settles one question that needs NO estimator run: when the … | 3 |
| `spectro_grade_vfold.py` | Grade the S2 VIRTUAL FOLD — the between-leg state delta per state class. docs/SPEC_lane_spectrometer.md §4 / §12.4. Reads the four legs emitted by ``… | 1 |
| `spectro_make_vfold.py` | Generate the S2 VIRTUAL-FOLD legs of the lane-spectrometer campaign. docs/SPEC_lane_spectrometer.md §4: from the S1 run's snapshot, resume paired leg… | 6 |
| `windup_ab_linearity.py` | windup_ab_linearity.py -- decide the carrier-phase wind-up SIGN defect on real data. docs/SPEC_phase_windup_sign.md section 8 item 1. The simulator i… | 4 |
| `windup_ab_slice.py` | windup_ab_slice.py -- the SPEC_phase_windup_sign.md §8 item 1 decisive arm. Real-data A/B the sim structurally cannot provide (the sim injects wind-u… | 3 |
| `windup_dd_regress.py` | THE WIND-UP DISCRIMINATOR for the band-common DD error (SPEC_synthetic §13.8.2 finding 4, owed since 2026-08). Finding 4: the per-band DDs of the pos… | 3 |

## RAIM, QC and the censuses

`docs/topics/raim-qc.md`.

| script | purpose | refs |
| --- | --- | --- |
| `churn_run_arms.ps1` | Wipe BOTH the traces and every snapshot: a stale logs/ dir makes an A/B | 2 |
| `edge_reaction_correlate.py` | Consumer-side datum-break REACTION census, and the v1-contract threshold audit. Companion to product_step_census.py. That script asks what the produc… | 2 |
| `grade_churn_arms.py` | Scorecard for the D5 churn-card arms (docs/SPEC_d5_churn_limit_cycle.md). Two of the card's four grading axes, side by side across arms: CHURN -- n_c… | 3 |
| `grade_d5_algebra.py` | Take the D5 innovation gate's standardized number APART. `network_d5_flags.csv` has always carried `std_innov` = \|v\| / sqrt(Cov(v)_ii). A populatio… | 1 |
| `p2_gate_verdict.py` | SPEC_day129_frame_guards Phase-2 validation-gate verdict. Grades the ARMED leg (runs/day129_guards/leg01) against the spec's Phase-2 gates: (ii) ON-p… | 4 |
| `pc_gate_verdict.py` | Grade the §12 known-answer gates of docs/SPEC_prefit_census_observability.md. Every check is a KNOWN ANSWER from the standing FT63 leg-19 replay (``r… | 2 |
| `pc_make_arms.py` | Build the validation arm configs for docs/SPEC_prefit_census_observability.md. Every arm derives from the standing FT63 leg-19 replay config (``confi… | 2 |
| `pc_run_arms.ps1` | Arm -> snapshot index (matches scripts/pc_make_arms.py). | 2 |
| `pf_rows_attrib.py` | SPEC_event_harness §24.6 — the per-LOS CODE-RESIDUAL attribution, read off the producer's own SIGNED post-fit residual rows (`network.trace_postfit_r… | 6 |
| `qc_dashboard.py` | Self-contained per-epoch QC dashboard for any network producer run. Reduces the run's standard trace CSVs to time-series and writes ONE static HTML f… | 1 |
| `raim_gate_grade.py` | Field-gate grader for the RAIM pool re-host card (docs/SPEC_raim_pool_rehost.md). The card's whole point is that a QC tier read ARMED and screened no… | 2 |
| `raimp_batch.py` | RAIM POWER leg batch driver (docs/SPEC_raim_pool_rehost.md §7.2 + the P0' extension). python scripts/raimp_batch.py [--extension] [--only name,name] … | 3 |
| `raimp_grade.py` | RAIM POWER leg grader — docs/SPEC_raim_pool_rehost.md §7.2, the pre-registered P0–P3 bars, plus the manager's P0' extension (n = 1 re-drawn at three … | 2 |
| `t3_null_fit.py` | T3-class null calibration: diagnose the statistic, not the threshold. QUEUE item 4 (`docs/QUEUE_principled_replacements.md`) — the bandaid being repl… | 9 |

## The product wire

`docs/topics/wire-contract.md`.

| script | purpose | refs |
| --- | --- | --- |
| `count_disc_bumps.py` | Per-epoch discontinuity-bump counter from a run's logs/network_solution.csv. Usage: python scripts/count_disc_bumps.py <run_dir> [<run_dir2> ...] For… | 7 |
| `grade_codeloose.py` | Grade a 305-sta closed-loop consumer run from its trace files. Consumer logs are dark at 305 stations, so grading uses the network trace CSVs written… | 1 |
| `grade_total_if.py` | Grade the total_IF level channel and test whether it has PLATEAUED. ``total_IF`` is the ionosphere-free clock + code-bias LEVEL a consumer actually f… | 2 |
| `grade_wire.py` | Grade the emitted phase-bias WIRE — but on a datum-invariant quantity. The producer writes the satellite phase-bias corrections it puts on the wire t… | 6 |
| `product_step_census.py` | Wire-side step census for an emitted network product (products.txt). The disc record the product may not carry, computed after the fact: scans P (pha… | 20 |
| `splice_wire.py` | Splice a CONTINUOUS seed wire with a warm-resume LEG's wire at the seam. python scripts/splice_wire.py <seed_products> <leg_products> <out_products> … | 13 |
| `wh_level_flag_replay.py` | Phase A of SPEC_withhold_era_release §12: replay the level-health classifier over a recorded run's own artifacts — zero C++ — and grade it against th… | 3 |
| `wire_step_audit.py` | R10 - the WIRE'S UNSIGNALLED STEPS: an M4 contract audit (SPEC_event_harness s24.19). python scripts/wire_step_audit.py [--runs C:/GnssProcessor/runs… | 3 |
| `wire_vs_code_census.py` | wire_vs_code_census.py - the wire-vs-CODE consistency census (SPEC_event_harness section 24.5). READ-ONLY. Compares the network wire (products.txt C … | 9 |

## The closed-loop consumer

`docs/topics/consumer.md`.

| script | purpose | refs |
| --- | --- | --- |
| `consumer_float_truth_join.py` | Join the closed-loop CONSUMER's float ambiguities to the simulator's TRUTH integers and measure the fixability of the integer-natured combinations (p… | 8 |
| `field_consumer_residual_census.py` | field_consumer_residual_census.py — what drives the REMAINING closed-loop consumer residual on a NETWORK field run. Analysis-only. Reads a completed … | 3 |
| `field_consumer_zwd_grade.py` | Field-arm grader: closed-loop consumer ENU/3D + fleet ZWD span, settled window. Built for the FIELD ZWD-in-block arm (SPEC_amb_iono_split_overcount.m… | 5 |

## Performance, chains and campaign drivers

`docs/topics/perf.md`.

| script | purpose | refs |
| --- | --- | --- |
| `chain_make_leg.py` | Generate one leg config for the DOY-129 continuous/chained network run. Derives ``network.start_offset_hours`` from a snapshot's ``M`` line using the… | 10 |
| `chain_run_day129.ps1` | _(no header comment — read the source)_ | 1 |
| `chain_stability.py` | Stability / convergence assessment for a DOY-129-style continuous network run. WHAT THIS GRADES, AND WHY IT CHANGED ---------------------------------… | 16 |
| `parse_carry_psd.py` | Carry-PSD audit of a network state snapshot — THE gate of the PD-safe fold card. python scripts/parse_carry_psd.py <snapshot.bin> [more_snapshots ...… | 12 |
| `run_decomp_arm.py` | Run one decomposition golden-matrix arm into a fresh run directory. Companion to compare_network_run.py (see docs/network_decomposition_plan.md §9). … | 5 |
| `run_inc4_gate.ps1` | INC-4 full fast-matrix gate: 8 arms + snap_resume + obs vs goldens. Usage: powershell -File scripts/run_inc4_gate.ps1 | 5 |
| `stage_brdc_only_root.ps1` | _(no header comment — read the source)_ | 3 |

## Real-time engine

`docs/topics/realtime.md`.

| script | purpose | refs |
| --- | --- | --- |
| `rt_seam_compare.py` | Masked byte-identity comparator for the RealTimeEngine seam gate. python scripts/rt_seam_compare.py pre inc2 Compares runs/rtseam_goldens/<golden>/ a… | 5 |
| `rt_seam_gate.ps1` | RealTimeEngine seam-decomposition byte-identity gate. | 5 |
| `rt_seam_verify.py` | **Retired 2026-10-17** (prints its note and exits 2). The verbatim-relocation verifier of the RealTimeEngine seam cut; eight bodies have been extended since, so it could only fail. Re-exports the lexer of `cpp_lex.py` for the verifiers pinned to earlier commits. | 16 |
| `rt_seamcut.py` | Generate the RealTimeEngine seam TUs by byte-line surgery on the frozen source. python scripts/rt_seamcut.py # write the .cpp files + the manifest py… | 5 |
| `sbf_probe.py` | Connect to a Septentrio SBF TCP output, capture a few seconds, and report which SBF block numbers are present (CRC-validated). Also saves the raw byt… | 2 |
| `sbf_rawnav_dump.py` | Dump the raw structure of the first SBF RawNav block of each requested block number, to ground the container-layout spec against real bytes. Usage: p… | 1 |

## Figures and reports

Rendering for papers, dashboards and reviews.

| script | purpose | refs |
| --- | --- | --- |
| `agu_figure.py` | Compose the 4-tile AGU26 abstract figure from one network run. (a) network geometry + the network's OWN estimated ionosphere, as ionospheric pierce p… | 1 |
| `compare_network_run.py` | compare_network_run.py Field-aware byte-identity harness for the ApplicationNetwork.cpp decomposition series (see docs/network_decomposition_plan.md,… | 11 |
| `gen_report_tables.py` | Generate LaTeX reference tables for docs/report from authoritative source. This is the "Tier 3" half of the GnssProcessor documentation tooling: smal… | 7 |
| `view_network_dd_fractional.py` | Fractional-cycle histogram of the network cross-station DD ambiguities (B5). The network producer, with ``network.trace_dd_fractional`` on, writes ``… | 2 |

## Independent reference implementations

Cross-checks for the model code; not part of any run.

| script | purpose | refs |
| --- | --- | --- |
| `diff_nutation_table.py` | Mechanically diff the IAU 1980 106-term nutation table between GnssProcessor (src/core/model/Planetary.cpp) and cssrlib (reference_software/cssrlib/s… | 1 |
| `gmst_crosscheck.py` | Cross-check GnssProcessor's two independent GMST implementations against each other and against the cssrlib/RTKLIB form, for the SAME UT1 input. Gnss… | 1 |
| `nmf_reference.py` | Canonical Niell Mapping Function (NMF) reference, mirroring cssrlib's tropmapfNiell / interpc (gnss.py) verbatim — itself a port of RTKLIB tropmapf. … | 3 |
| `ocean_pole_tide_reference.py` | Independent reference for the IERS 2010 (Sec. 7.1.5 / Desai 2002) ocean pole tide site displacement. This is an *independent* Python reimplementation… | 3 |
| `solid_tide_reference.py` | Standalone reference for the solid Earth tide (IERS 2010 dehanttideinel). This is an INDEPENDENT Python port of the same IERS Conventions dehanttidei… | 3 |
| `vmf3_reference.py` | Standalone Python port of vmf3.m used to generate reference values for the C++ Vmf3 unit test. Reads coefficients directly from the generated header … | 1 |

## Campaign one-offs

Each was written for one card. Ledger changes are edits to `docs/CONFIG_promotion_ledger.json` followed by `scripts/ledger_render.py`; the `_*_ledger_oneoff.py` patch scripts were retired on 2026-10-13 with the prose ledger they patched (git history keeps them).

| script | purpose | refs |
| --- | --- | --- |
| `_arm_run.sh` | Run one arm and snapshot the shared trace CSVs into the run dir. | 5 |
| `_ifsc_ab.sh` | SPEC_amb_iono_split_overcount §6.14.5 defect 2 A/B: does excluding the reference satellite's structural zero from the IF-TD pool scale change the FLA… | 2 |
| `_ifsc_ab_compare.py` | Compare the two IF-TD pool-scale arms (SPEC_amb_iono_split_overcount §6.14.5 defect 2). old = reference satellite's structural zero INCLUDED in the r… | 2 |

---

Regenerate the reference counts with a stem grep over `CLAUDE.md`, `docs/`, `tests/`, `src/`, `include/`, `config/`, `scripts/` and the author's memory directory, excluding the script itself.

---

## Carried over from the previous `scripts/README.md`

Kept verbatim — the detail below is not in any script docstring.

## Warm-resume / branch-faithfulness (RS card)

| script | purpose |
|---|---|
| `run_warm_resume_arm.sh` | Run ONE warm-resume arm under the HB-2 protocol: re-seed the working snapshot from the immutable converge artifact **before every resume launch**, then assert the expected `WARM RESUME` / `BRANCH-FAITHFUL` verdict in the log. Guards the §0.4 hazard — a resume overwrites the snapshot it read, so an un-re-seeded relaunch silently full-cold-starts (warn only) and would be graded as warm. |
| `warm_resume_sd_metric.py` | Between-satellite SD-of-SD divergence of emitted products (CLK/CB) vs an inline reference, per epoch, with post-transient slope fitting and an optional plot. Datum-invariant by construction; PB excluded (integer-ambiguous). Read levels **and** slopes — a higher level inside the adaptive-window transient with a shallower slope is a transient, not divergence. |
| `diff_carry_blobs.py` | Branch-faithfulness instrument: compares the Card-4 carry blobs of two snapshots per (group, chord) **after translating ids to names** via each file's own `carry_sat_vertex` / `carry_group_id` maps (raw-id comparison is a silent mis-binding trap). Reports branch-agree / branch-flip fractions and set churn. |

Typical use (see the spec's §5 run matrix):

```bash
bash scripts/run_warm_resume_arm.sh config/test_suite/_warm_resume.json run.log warm+carry
python scripts/warm_resume_sd_metric.py --ref solA.csv \
    --arm cold=solB2off.csv --arm warm=solB2on.csv --plot rs_curves.png
```


## Archived (`scripts/archive/`)

Moved unedited on 2026-10-17: one-offs of closed cards that nothing else referenced. A path computed
from the file's own location (`ROOT` as the parent of `scripts/`) is one level short from here, and an
import of a sibling script no longer resolves; edit before running one, or read its history with `git log --follow`.

| script | purpose | refs |
| --- | --- | --- |
| `archive/eval_speedcheck.py` | eval_speedcheck.py -- consolidate + grade the _net305_gpsgal_sh_n15 duration sweep. Walks the run dirs produced by run_chain.ps1 (1ep / 3ep / 10ep / … | 0 |
| `archive/gen_net_matrix_configs.py` | Generate the network PPP-RTK test matrix configs (50/305 stations x 10ep/10min/1h) with the full "all new features" toggle set layered onto each rost… | 0 |
| `archive/gen_net_validation_configs.py` | Silent/fast net305 25deg producer + closed-loop HELD-OUT consumer (ZIM200CHE) for a quick end-to-end validation: does a user PPP-RTK station kept OUT… | 0 |
| `archive/guards_cont_anchor_config.py` | Continuous GUARDS-ARMED run 13:00 -> 21:29:30 with full snapshot history. Follow-on to runs/raim_cold_event (Jul-28), which proved the 20:47:00 epoch… | 0 |
| `archive/make_agu305_configs.py` | Generate the 305-station AGU scaling configs (GPS-only, then GPS+Galileo). Same experiment as ``make_agu_configs.py`` at 5x the stations. The roster … | 0 |
| `archive/make_agu_replay_arms.py` | Generate consumer-replay arms against an already-written network product file. ``Application::runConsumerReplay`` (armed by ``network.closed_loop.rep… | 0 |
| `archive/verify_rinex2_parser.py` | Reference parser for RINEX 2.xx mixed observation files. Mirrors the algorithm now implemented in src/core/rinex/RinexParser.cpp so we can sanity-che… | 0 |
| `archive/ev_coldfb_grade.py` | COLD ANCHOR-FALLBACK (SPEC_event_harness §15): the ELECTION channels of a leg. `scripts/event_grade.py` grades the wire and consumer channels of an … | 0 |
| `archive/ce_g4_read.py` | Card CE / s24.17 leg 3 (G4) reader: the guard's behaviour in the +/-N epochs around an event. python ce_g4_read.py <arm> [<arm> ...] [--tow 334020] [… | 0 |
| `archive/ce_hourly_grade.py` | Card CE / s24.17 leg 2: the HOUR-BY-HOUR grade of a long cold arm. python scripts/ce_hourly_grade.py --arms c10,pvb6 [--hours 6] [--tow0 259200] [--r… | 0 |
| `archive/cm_driver.ps1` | Card CM (SPEC_event_harness s24.8) detached run driver. | 0 |
| `archive/d5v5_same_realization_ab.py` | SAME-REALIZATION A/B for the D5 v5 card: two legs off ONE snapshot, arm-to-arm. Why this is not ``d5v5_seam_ab.py``. That script grades each arm agai… | 0 |
| `archive/fp2_fanout_window.py` | FP-2 FAN-OUT CENSUS on the pre-registered §20.2 window: k in [-1, +2]. python scripts/fp2_fanout_window.py [--runs C:/GnssProcessor/runs] [--prefix … | 0 |
| `archive/fp2_wire_step_census.py` | UNDECLARED WIRE-STEP census across an FP-2 leg matrix. python scripts/fp2_wire_step_census.py [--runs C:/GnssProcessor/runs] [--prefix fp2] [--tol 0.… | 0 |
| `archive/clkr_grade_g3.py` | Card CLK-R G3 grader (docs/SPEC_clock_emit_rebase.md §4). Grades the 12 h A/B (runs/clkr_12h_ctl vs runs/clkr_12h_on) against the stored denominator … | 0 |
| `archive/clkr_run_arm.ps1` | Card CLK-R arm runner: serialized producer run + trace harvest. Usage: .\clkr_run_arm.ps1 -Config <config.json> -RunDir <runs/name> Redirects console… | 0 |
| `archive/q3_consumer_hours.py` | Hourly LEVEL bands of the in-process closed-loop consumer, for Card Q3 A/B arms. Grades ``logs/closed_loop_trace.csv`` (the live WTZZ holdout) as hou… | 0 |
| `archive/cbq305_readfirst.py` | Card CB-Q leg C: the invariant-#4 read-first sheet for a 2-signal arm. Reads the ON-PATH counters BEFORE any quality number, because the failure mod… | 0 |
| `archive/arc_life.py` | Arc-life estimator: per traced DD pair, split series into hold-segments at | 0 |
| `archive/p1_trigger_verdict.py` | SPEC_day129_frame_guards Phase-1 trigger verdict. Reads one instrumented leg's traces and answers the three falsifiable questions the spec poses abou… | 0 |
| `archive/weci_detonation_arm.py` | Warm-start arms over the 19:26:00 SH detonation from the guards_cont history. Derives an arm config from the anchor's OWN leg.json (fingerprint match… | 0 |
| `archive/chain_status.py` | One-screen status for the DOY-129 continuous network run. python scripts/chain_status.py [--runroot C:/GnssProcessor/runs/day129_cont] Reports, per s… | 0 |
| `archive/iofloat_run_arms.ps1` | _(no header comment, read the source)_ | 0 |
| `archive/overnight_run_queue.ps1` | ---- 1. wait for the parent 24 h run --------------------------------------- | 0 |
| `archive/p2_run_guards_chain.ps1` | Epoch times carry ~1e-5 s of double residue BELOW nominal (TimeUtils builds them through a Julian-date divide). Every tow comparison against a nomin… | 0 |
| `archive/analyze_network_clock_rms.py` | Per-constellation network satellite-clock RMS, epoch by epoch. Reads a network producer run's logs/ and reports, for each epoch, the per-constellatio… | 0 |
| `archive/_arm_compare.py` | Extract the Card-1f/1g A/B metrics from a set of run logs. | 0 |
| `archive/_ifg_ab.sh` | §6.14.6 three-arm validation. Captures CWD-relative ./logs/ with a SOURCE-mtime freshness assertion (the stale-trace-dir trap, SPEC_amb_iono §6.13.4). | 0 |
| `archive/_ifref_ab.sh` | Card-R ref-gate A/B: capture CWD-relative ./logs/ with SOURCE-mtime freshness assertion. | 0 |
| `archive/cmd_batch_commands.txt` | _(no header comment, read the source)_ | 0 |
| `archive/run_d5_ab.ps1` | D5 A/B for SPEC_synthetic_minimum_network §13.7: same-build control + treatment. Both arms run from their own cwd because Application::log_dir_ defa… | 0 |
