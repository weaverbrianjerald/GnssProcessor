# CLAUDE.md history: text retired from the always-loaded file

[`CLAUDE.md`](../../CLAUDE.md) is read in full at the start of every session, so it carries rules and orientation only. This page keeps the descriptive and historical text cut from it on 2026-11-02 (from main `86f9125`; a first pass of 2026-10-24 is kept under the git tag `archive/inspiring-torvalds-2026-10-24`). Each block is verbatim, under a heading naming the section it left. The other moved blocks are in [build_toolchain.md](build_toolchain.md) (migration record, disk placement, run-directory relocations), [data_files.md](data_files.md) (File management) and [where_new_code_goes.md](where_new_code_goes.md) (the two decision trees at full length).

## Retired network rules

Heading of the original: "Retired network rules (full pre-2026-10-13 text: `git show 8326da3:CLAUDE.md`)".

- Card-4 `carry_invariant` lineage (`emit_pb_filter_gate_v2`, `emit_pb_filter_jump_k` 8.0,
  `raim_carry_invariant`, the `trace_station_residuals` zero-row trap): float vehicle, cards RF-1,
  RF-2b, GRB; ledger rows (the `emit_pb_filter*` rows left with their keys, card RKD:
  `git show 4bdd693:docs/CONFIG_promotion_ledger.json`).
- MW/WL lane layer cost and retention (AGU-26, 63 and 305 stations): card AB3; the `lane_carry.*`
  and `mw_wl.*` keys were deleted by card RKD (rows: `git show 4bdd693:docs/CONFIG_promotion_ledger.json`).
- Lane slip responses (`relevel_retap_*`, `relevel_lane_snap`, lane-preserving refloat):
  `docs/record/cards/slip/`; the keys were deleted by card RKD (rows at `4bdd693`, as above).
- SH and GIM ionosphere level (GIM mean series, DEFECT 2, rx-CB carry memory, SH gate release):
  `docs/record/cards/ionosphere/LV-CORRECTION-AND-12H.md`, card AB1.
- Pre-golden armed set (the Jul-28 guard list, `scripts/p2_make_guard_configs.py`,
  `cb_excursion_reinit` chains, `raim_we_code_iono`): the goldens and the ledger.
- Clock-datum candidates (`clock_datum_broadcast_anchored`, `clock_datum_sum_to_zero`, pivot
  election): ledger rows; pivot election is a queued build (`docs/status/OPEN_ITEMS.md`).
- Wind-up accumulation (fractional census, inverted horizon, restart response): card WUP,
  `docs/record/cards/slip/WINDUP-FRACTIONAL-CENSUS.md`.
- Re-admission edge (`readmit_seed_horizon_s` 3900 pinned; its KEEP_OFF siblings):
  `docs/record/cards/network-vehicle/READMIT-EDGE.md`.
- Golden re-bases 2026-09-15 to 2026-10-01 (RF-3-prep, GRB, RF-3, PBF, WUP, SFR, EXL; baselines
  `_rf0hv9` to `_rf0hv14`): the cards in `docs/refound/CARDS.md`.

## Baseline: cards BOF, MRF and GAD, paragraph at full length

**Baseline: card BOF (2026-10-28, `docs/record/refound/BOF_built_off_follow_through_2026-10-28.md`), after
card MRF, on card GAD's Galileo posture (2026-10-13, `docs/record/refound/GAD_galileo_adoption_2026-10-13.md`).**
Card BOF armed ocean tide loading (four keys) and the noon-turn attitude on the field golden, and the
estimable block-Baarda test and the readmit-seed mirrors on both; pair `runs/_rf0hv17_{field,sim}6` (exe
`gnss_processor_bofarm.exe`, sha256[:16] 91143c81de9a9547): field WTZZ00DEU 0.059 0.012 0.023 0.012 0.017
0.012 m by hour (2.0 mm worse than `_rf0hv16` over hours 2 to 6, the loading's 00-06 UT phase: over a
whole tidal day the same keys gain 0.8 mm and the census 1.0 mm); sim products identical to `_rf0hv16_sim6`.
The
Galileo package is armed: `processing.active_systems` `["GPS","Galileo"]` with E1/E5a in both code
families (`["C1C","C1X","L1C","L1X","C5Q","C5X","L5Q","L5X"]`, `frequency_mode` dual),
`estimation.clock_disc_isb_reinflate_std` 10 and
`model_corrections.enable_receiver_cross_system_atx_fallback` (card GAL2),
`network.code_datum_family_difference` with `code_family_birth_prior_sigma_m` 0.10 (card GFC;
`code_datum_pair_continuity` KEEP_OFF), and the MW re-flag skip (`network.cycle_slip.mw_reflag_skip`,
card MRF: an MW flag that re-detects a step the LLI or IF-TD response already answered frees no band
re-levelled since, so one slip frees a carried arc once; field 6 h 18 of 30 re-flag bands skipped, the
sim bed has no receiver flags and skips none). Pair `runs/_rf0hv16_{field,sim}6` (exe
`gnss_processor_mrf.exe`; the field byte-identical to card MRF's arm `_rap_sdr_on_f6`): field WTZZ00DEU
0.057 0.012 0.018 0.010 0.015 0.011 m by hour; sim SZZ000SIM 0.137 0.072 0.031 0.024 0.026 0.018 m from
the run start (card GAD's pair `runs/_rf0hv15_{field,sim}6`, exe `gnss_processor_galp2.exe`: field 0.058
0.011 0.018 0.010 0.015 0.011, the sim the same medians). Census against GPS only,
median site in hours 2 to 6: field -0.020 m (10 better, 2 worse: DAV100ATA, HARB00ZAF), sim
-0.030 m; GPS-only users of the wire unchanged at the median (ARHT00ATA +0.20 m). ⚠ Every golden
arm is GPS + Galileo and 2 to 6 times slower. Constellation-blind graders pool the systems:
`rapid_arm.py score` counts E5a as band 5, `ftc_step_attrib.py` R3 needs `--systems G,E`, and
`census6h.py` / `simcensus6h.py` default to GPS-only bases (use `gcensus.py`, or `SIMCENSUS_BASE` =
the wire's arm). Open: DAV100ATA, the GFC hour-2 clauses, SEYG00SYC's E11 / E12 C5Q rows, nominal
yaw steering only, E14 / E18 broadcast unhealthy and kept (GAD section 4).

## Documentation section: introduction, tree, rule 5 and ledger paragraph at full length

**`docs/INDEX.md` is the single entry point, and `docs/refound/` is the current
program.** The author's directive: *"We need a method to organize our whole project,
rather than simply appending to these massive files."* On 2026-09-14, 18 of the 19
`docs/*.md` over 60 KB were split into a topic tree — verbatim, hash-verified, with the
originals frozen. The nineteenth, `CONFIG_promotion_ledger.md`, is now a generated view
of the ledger JSON (see the ⚠ below).

```
docs/INDEX.md                    entry point: topics, cards, the rules
docs/refound/                    the current program (RF-0..RF-3) — not split, not moved
docs/cards/<topic>/<CARD-ID>.md  one card per file: STATUS block + verbatim text
docs/topics/<topic>.md           a short living page per topic (17 topics)
docs/record/decisions/                  posture rulings whose text is self-contained
docs/status/OPEN_ITEMS.md        everything owed, with its evidence pointer
docs/record/archive/<monolith>.md       the pre-split originals, byte-for-byte, FROZEN
docs/<monolith>.md               a routing stub: section -> card, so an existing
                                 citation ("SPEC_event_harness §24.10") still resolves
```

Rule 5 of the seven rules:

5. **Every card file starts with the STATUS block** (`id`, `topic`, `status`, `summary`,
   `source`, plus the knobs / hashes / run dirs / dates). `status` ∈ {`open`, `closed`,
   `parked`, `superseded`, `reference`}; PROMOTED / KEEP_OFF / FALSIFIED belong in the
   summary and the text. An **Outcome** paragraph follows the block (2026-10-07): question,
   decisive result, verdict (missed bars included), current standing with a link, about 90
   words. The header (title, STATUS, Outcome) is revised as results land; the body never is.

The ledger paragraph:

⚠ **The config ledger is `docs/CONFIG_promotion_ledger.json`: one row, one verdict, per
config key the network loader reads** (rebuilt 2026-10-13; verdicts RECOMMEND_ON /
RECOMMEND_SCOPED / ARMED / UNVALIDATED / KEEP_OFF / REMOVE / PARAM / TRACE / CORE).
`make_golden_config.py --verify` and `config_posture_check.py` read it;
`docs/CONFIG_promotion_ledger.md` is its view (a hand-written head, then tables rendered by
`scripts/ledger_render.py`). A ledger change is a JSON edit, a re-render and the two
verifies; a new knob gets its row in the same commit. The full-prose ledger before
2026-10-13 is in git (`git show 8326da3:docs/CONFIG_promotion_ledger.md`).

## Layout block: entries replaced

The `tests/`, `scripts/` and `data/` entries (the `data/` entry describes the layout that preceded the date-first layout; the current one is in [data_files.md](data_files.md)):

```
tests/                GoogleTest unit suite (1540 cases / 215 suites across 104
                      files) linked into `gnss_tests.exe`.  tests/sim/ is a
                      SEPARATE target, `gnss_simulator_selftest` (80 cases).
                      The network-producer tests are in tests/network/ (same
                      `gnss_tests.exe`); tests/support/ holds the shared harness.
                      Note: docs/test_suite.md documents the real-data
                      truth-graded harness, NOT this unit suite.
scripts/              One-off Python utilities.  Most notably
                      bootstrap_internal_data.py, which populates
                      data/ for internal-mode runs (see "File management"
                      below).
data/                 Local cache for internal-mode test fixtures.
                      Empty in git (.gitignored except for README.md
                      shells); populated by the bootstrap script.
                      Layout matches what ProductResolver creates in
                      production: products/{orbit,clock,dcb,erp,brdc,
                      rinex}/{zip,unzip}/, products/{atx,nc}/,
                      observations/{absolute,baseline/...}/.
```

## Build section: the sentence that introduced the wrapper

Use the wrapper — it encodes every trap below:

## Known state at full length

- **Test suite.** `gnss_tests.exe` runs 2083 GoogleTest cases across 282 suites in 140 files (85 s on an idle box, ~205 s under load; full Debug run under VS 2026, measured 2082 passed + 1 skip on 2026-09-14 after card CC (the consumer's declared-step carry + warm start); the 2073/281/139 figure of 2026-09-13 covered the RS-7..RS-10b warm-resume cards, REF-FAN, the D5-v5 field card and the RAIM-pool re-host gates — the earlier 1540/215/104 and 2013/275/135 figures predate them; one fixture-gated skip — `FixtureSmokeTest.Sp3HeaderTimeConversionMatches`, which needs `bootstrap_internal_data.py` to have run) — math core, site displacement, atmosphere models, antenna/ATX, filter core, transforms/estimation, RTCM/SBF decoders, ILS/MLAMBDA, state-key schema, the network estimator + normal equations + invariant-carry driver + product I/O, cycle-slip/TurboEdit, the RTS smoother, aiding, and real-time seams. `tests/sim/` is a **separate** target, `gnss_simulator_selftest` (80 cases), not linked into `gnss_tests`. The suite guards structure and non-regression (new work is developed default-OFF / byte-identical). It does NOT validate end-to-end estimation accuracy — confidence for numerical changes still comes from a clean build + field runs graded on datum-invariant quantities (see Standing invariants) + trace inspection.
  - Suite hygiene: every suite name should be owned by exactly one file — keep it that way, and prefer a new file over widening an existing one (the zero-cross-file-overlap property was re-verified at 215 suites on 2026-08-22, tests/ + tests/sim/ both scanned). At 51.5k test LOC against 189.2k production LOC the suite is *thin* (0.27 ratio, ~1 case per 139 production lines); the gap is coverage, not bloat. There is no dedicated doc for this suite — `docs/test_suite.md` is the real-data harness.
- **Pre-existing C4828 warnings** (illegal characters at codepage 65001) are from older mixed-encoding files and unrelated to new work. Ignore unless the user asks to clean them up.
- **`vswhere.exe not recognized`** messages from cmd subshells are benign noise when using the vcvars64 pattern above — the build proceeds.
- **Real-time mode** uses a separate path (`Application::runRealTime` → `RealTimeEngine`). Post-processing-only features (e.g. smoother, `gnss_rate_s`) live in `Application::run` and should guard against `AppMode::RealTime`.

## Entry points at full length

- `src/core/app/Application.cpp` — `run()` is the post-processing pipeline, `runRealTime()` the streaming one. The per-epoch loop is the place to wire in new predict/update-phase logic. Mode-specific impls in `ApplicationBaseline.cpp` and `ApplicationRealtime.cpp`.
- `src/core/config/GnssConfigLoad.cpp` — `loadFromJson()` is a thin orchestrator; each top-level JSON section has its own `loadXxxSection_` private helper. New toggles slot into the matching section helper. Shared JSON-reading primitives (`jval`, `jsection`, `resolvePath`) live in `include/config/GnssConfigJsonHelpers.hpp`.
- `include/products/PathResolver.hpp` — single mode-aware locator for every input file. Internal mode globs `data/`; production mode delegates to `ProductResolver`. New file types (e.g. an SSR stream cache) plug in here, not into the call sites in `Application` / `ApplicationBaseline`.
- `include/filter/KalmanFilter.hpp` — predict / update / IMU-aided predict. Variable-dimension state is handled via `StateManager` key maps, not via fixed indices.
- `include/filter/StateKeySchema.hpp` — single source of truth for every dynamic state-vector key format (PPP ambiguity, DD ambiguity, per-sat iono, receiver code bias, sat bias, system time offset, BSSD pivot seed). All key construction and parsing should go through its `make*` / `parse*` helpers.
- `include/filter/Smoother.hpp` — RTS smoother operates on float ambiguities; segment boundaries at ambiguity resets. Smoothed position is degenerate (constant across epochs) in static PPP by construction — that's expected.

## Baseline paragraph of CLAUDE.md, 2026-11-02 to 2026-11-07

Replaced on 2026-11-07 by the `_rf0hv18` paragraph (the AVX2 build). Text as it stood:

**Baseline: card BOF (2026-10-28, `docs/record/refound/BOF_built_off_follow_through_2026-10-28.md`), after card MRF, on card GAD's Galileo posture (2026-10-13).** Pair `runs/_rf0hv17_{field,sim}6` (exe `gnss_processor_bofarm.exe`, sha256[:16] 91143c81de9a9547): field WTZZ00DEU 0.059 0.012 0.023 0.012 0.017 0.012 m by hour; sim products identical to `_rf0hv16_sim6`. The armed keys of cards GAL2, GFC, MRF and BOF, the earlier pairs (`_rf0hv16`, `_rf0hv15`), the census against GPS only and the open items: the cards, and the paragraph at full length in `docs/onboarding/claude_md_history.md`. ⚠ Every golden arm is GPS + Galileo and 2 to 6 times slower. Constellation-blind graders pool the systems: `rapid_arm.py score` counts E5a as band 5, `ftc_step_attrib.py` R3 needs `--systems G,E`, and `census6h.py` / `simcensus6h.py` default to GPS-only bases (use `gcensus.py`, or `SIMCENSUS_BASE` = the wire's arm).

## Posture paragraph of CLAUDE.md, 2026-11-07 to 2026-11-19

Replaced on 2026-11-19 by the paragraph of the integer-fixing posture (card IRM,
`docs/record/refound/IRM_integer_reference_main_2026-11-19.md`). It still describes the frozen lineages
`config/golden/golden_{field,sim}_rf0hv18.json` and the BeiDou lineage. Text as it stood:

Posture, one on two beds: GPS L1/L2 + Galileo E1/E5a, dual frequency; self-contained (satellite
GF code bias pinned at broadcast T_GD, no GIM, no Kaula prior); free per-LOS slant ionosphere (no
SH); float vehicle (`ambiguity_vehicle: float_kf` + `phase_datum_held_basis`) on the
phase-leveling carry with gauge projection, the prioritized phase datum and the exact coupling
update; Q3 clock anchor and clock step test; phase-bias hold; gross-LOS screen with its 12 m cold
bound; accumulated wind-up with restart refloat. Field golden only: ocean tide loading from the SIF
registry and the GPS IIF / IIR noon-turn attitude (the sim fixture carries neither). No lanes, F3,
SH constraint or seams.

## Baseline paragraph of CLAUDE.md, 2026-11-19 to 2026-11-20

Replaced on 2026-11-20 by the `_rap_n20` paragraph (the 6 h baseline of the reference files, card IRM). Text as it
stood:

**Baseline of the reference files: OWED (2026-11-19, card IRM).** The reference files took the integer-fixing posture on 2026-11-19 and have no 6 h baseline yet; it comes from the night 6 h set of card IRM (`D:/GnssProcessorSupport/gates/day_program_2026-11-19/adoption/NIGHT_health_set.md`). Until then compare an arm only against a same-exe control of the same reference file. The day check (2 h, exe `gnss_processor_irm.exe`, sha256[:16] f50e95d27304f1fb, runs `D:/GnssProcessorSupport/runs_irm/_rap_irm_{f129_2,s_2}` with the frozen control `_rap_irmc_f129_2`): every epoch emitted; sim truth audit 0 wrong of 95,810 held link-epochs; held share in hour 2 field 76 % Galileo and 72 % GPS, sim 92 % and 89 %; the built-in float user WTZZ00DEU 2.30 0.32 m by hour (control 0.058 0.013 m, `_rf0hv18` to the millimetre), SZZ000SIM 0.79 0.30 m. ⚠ The built-in user is float and the posture runs the phase-bias hold off, so it is metre-level in hour 1 (as on the five-day days): the reference's user numbers are not comparable with `_rf0hv18`, and the user's type is the author's decision.

## Posture paragraph of CLAUDE.md, 2026-11-19 to 2026-11-20

Amended on 2026-11-20: the built-in user's type (the fixing, differenced user since 2026-11-19 evening, card IRM) and
the two keys armed on 2026-11-20 (cards NRF and CPW). Text as it stood:

Posture, one on two beds, since 2026-11-19 (card IRM: the author's ruling that the integer-fixing
configuration is the main reference): GPS L1/L2 + Galileo E1/E5a, dual frequency; self-contained
(satellite GF code bias pinned at broadcast T_GD, no GIM, no Kaula prior); free per-LOS slant
ionosphere (no SH); float vehicle (`ambiguity_vehicle: float_kf`) on the phase-leveling carry with
gauge projection and the exact coupling update, with the eq34 chain as the phase datum (pair family,
event rows, carry shift, receiver phase bias in the block) and network integer fixing ring by ring
(`network.ring_fix.*`, card NRF: odds gate, certain order, held-first, pairs E:L1C/L5Q and G:L1C/L2W,
settled flags, entry re-validation, near-null monitor); the single Galileo network
(`network.phase_families`); the bad-epoch response (card BER); exact epoch time; GPS band 1 on L1C
first; Q3 clock anchor and clock step test; gross-LOS screen with its 12 m cold bound; accumulated
wind-up with restart refloat. Off with the chain: the held basis, the prioritized datum and the
phase-bias hold (card IRDU stage B: the hold freezes the cold estimate). Field golden only: ocean
tide loading from the SIF registry and the GPS IIF / IIR noon-turn attitude (the sim fixture carries
neither). Sim golden only: the ring fix's truth audit (`network.ring_fix.truth_root`). The held-out
consumer stays float and undifferenced. No lanes, F3, SH constraint or seams. The float-vehicle
posture before that date (held basis, prioritized datum, phase-bias hold; the paragraph:
`docs/onboarding/claude_md_history.md`) is kept generated and verified as the frozen lineages
`golden_{field,sim}_rf0hv18`; the tools whose arms are defined on it read those (`scenario_matrix.py`,
`prm_ab.py`, `clock_level/scb_arms.py`, `clock_level/sfr_calib.py`).

## Baseline paragraph of CLAUDE.md, 2026-11-20 to 2026-11-23

Replaced on 2026-11-23 by the `_arm_h` paragraph (the reference files without the CODE bias file and with the four switches of that day). Text as it stood:

**Baseline of the reference files: `_rap_n20` (2026-11-20, card IRM).** Runs `D:/GnssProcessorSupport/runs_irm/_rap_n20_{f129,f130,f132,s,b}_6` (6 h, the reference files with the fixing built-in user; exe `gnss_processor_int1120.exe`, sha256[:16] de17cb83802a20c2, commit 8109a64) against same-exe frozen controls of `golden_{field,sim}_rf0hv18` (`_rap_n20c_*`); census users `_rap_n20_<doy>_<site>` (16 sites); grades `D:/GnssProcessorSupport/runs_nom/_irdu_grades/reads_2026-11-20/n20_raw.txt`. Every run exit 0, every epoch emitted. Built-in user 3D median by hour 1 to 6: field DOY 129 WTZZ00DEU 2.31 0.09 0.01 0.03 0.04 0.01 m (frozen control 0.06 0.01 0.02 0.01 0.02 0.01), DOY 130 2.06 0.11 0.02 0.04 0.04 0.04 m, DOY 132 0.60 0.05 0.03 0.03 0.02 0.01 m; sim SZZ000SIM 0.68 0.10 0.03 0.03 0.02 0.02 m (control 0.10 0.03 0.04 0.02 0.02 0.01); BeiDou lineage 0.03 0.02 0.02 0.02 0.02 0.02 m. Sim truth audit: 406,237 held link-epochs right, 0 wrong, 0 wrong decisions. Held share from hour 3: field DOY 129 GPS 80.7 to 84.0 %, Galileo 81.3 to 86.7 % (DOY 130 Galileo 69.9 to 79.9 %, the low one); sim 89 to 92 %. Census, hours 2 to 6 paired against the frozen control: +1.4 cm (DOY 129), +1.9 cm (130), +1.4 cm (132); hour 1 +0.50 to +0.61 m (network and user start cold together). Reading: from hour 3 the reference equals the frozen one at the built-in user; hours 1 and 2 are slower, so the reference's user numbers are not comparable with `_rf0hv18` there. The two keys armed on 2026-11-20 move nothing where no event occurs: the 6 h pairs `runs_irm/_rap_la4_{f129,s}_6` (tracking-break response armed) and `runs_irm/_rap_cp1_{f129,s}_6` (both keys armed, engine of 7e4ea01) are identical in products and every trace to `_rap_n20_{f129,s}_6`. The adoption of 2026-11-22 leaves these numbers unchanged: the 6 h runs `runs_irm/_rap_a22h_{f129,f130,f132,s}_6` (the regenerated reference files, code-bias gap response armed, 4 threads, exe `gnss_processor_int1122.exe`, sha256[:16] a0fadbb1564612b9) meet no code-bias gap, equal their same-exe controls with the response off (`_rap_a22hc_*`) in products and every trace, and give the built-in user by hour, the held shares and the sim truth audit (406,237 right, 0 wrong) of `_rap_n20`; products are identical to `_rap_n20` on DOY 129, 132 and the sim, and on DOY 130 they part from 5 h 25 min by one release of the tracking-break response (KIT300UZB-G22; the `_rap_n20` day files lacked the two keys of 2026-11-20; with those keys off, `_rap_a22hk_f130_6` equals `_rap_n20_f130_6`), the user moving 0.1 mm at most (report: `D:/GnssProcessorSupport/gates/adopt_2026-11-22/ADOPTED.md`). The OWED paragraph it replaces and the 2 h day check of 2026-11-19: `docs/onboarding/claude_md_history.md`.

## "Sim start" bullet of CLAUDE.md, to 2026-11-23

Replaced on 2026-11-23, when the simulator reference file took its start (order 4). Text as it stood:

- **Sim start.** The sim golden starts inside the fixture's start artifact: grade sim level,
  ignition and hour 1 from 00:30:15 (`start_offset_hours` 0.504167). Every `start_offset_hours`
  keeps the half-epoch back-off (bare hours sit on a double-residue boundary).

## Test-suite measurement of CLAUDE.md, 2026-11-20 to 2026-11-23

Replaced on 2026-11-23 by the measurement of the round of that day (six shards). Text as it stood:

Debug `gnss_tests.exe`: 5,080 cases in 683 suites, 4,986 passed, 94 skipped, 0 failed (measured 2026-11-20 on 5207fdc, 3046 s; `D:/GnssProcessorSupport/gates/run_queue/int_5207fdc_suite.txt`); `gnss_simulator_selftest` 99 of 99 (measured 2026-11-02, card BDG gate G5); re-measure with the Debug build.
