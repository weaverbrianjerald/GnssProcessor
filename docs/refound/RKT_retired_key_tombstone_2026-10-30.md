# RKT: a warn-once tombstone of the retired configuration keys

**STATUS**

- **id:** RKT
- **topic:** config-ledger
- **status:** closed
- **summary:** CLOSED 2026-10-30 on branch `claude/pensive-ptolemy-4f6046` (not merged; pre-registered in `9cd2d8d`). BUILT and GATED: follow-on F4 of card RKD. `include/config/RetiredConfigKeys.hpp` declares the 150 network keys card RKD deleted with their code (each with its flat legacy spelling and its deletion commit, 28 families), and the network loader logs one warning per retired key a configuration sets, under either spelling the old loader read. PASS on every gate: the 2 h field and sim goldens and the 16 user-side arms identical on a same-build pair; a 2 h field golden with 30 retired keys added identical to the golden in every product, CSV and manifest record, its logs longer by exactly the 30 warnings the census predicted; the unit suite green. Census: 1,153 of the 1,234 network-mode configurations under `config/` now warn (710 outside the archive). Card RKD's documentation notes written, two still owed.
- **source:** card RKD section 5 (follow-on F4, plan question Q2, plan risk R1); the register item "Retired-key deletion: documentation notes and follow-ons" ([OPEN_ITEMS](../status/OPEN_ITEMS.md)); the loader and the ledger at main `4bdd693` (`git show 4bdd693:docs/CONFIG_promotion_ledger.json`)
- **knobs / posture:** none: no key is added, read or armed; both goldens unchanged
- **dates in the text:** 2026-10-30
- **hashes / run dirs:** base main `5127429`; reference exe `D:/GnssProcessorSupport/gates/exe/x64-Release/gnss_processor_rktref.exe` (the Release build of `5127429` in the worktree `C:/GnssProcessor/.claude/worktrees/pensive-ptolemy-4f6046`, build tree `D:/GnssProcessorSupport/worktrees/pensive-ptolemy-4f6046_build`, sha256[:16] `45ddd4c9329e8ced`); candidates beside it, the Release builds of this branch in the same build tree: `gnss_processor_rkt.exe` (first, `ddb363ca2eb4946b`) and `gnss_processor_rkt2.exe` (final, `fb38d0f93899e07b`, the same code: section 4.1); run root `D:/GnssProcessorSupport/runs_nom`, arms `_rap_rkt_ref_f2`, `_rap_rkt_cand_f2`, `_rap_rkt_ref_s2`, `_rap_rkt_cand_s2` and `_rap_rkt_ret_f2`; user-side tags `rktref` and `rkt2` (runs under `D:/GnssProcessorSupport/worktrees/pensive-ptolemy-4f6046_runs`); gate reports and the census list `D:/GnssProcessorSupport/gates/rkt/`
- **gates (frozen at pre-registration):** section 3, written before any run of the changed executable

**Outcome.** Question: can the loader name each retired key a configuration sets, once and with the deletion that removed it, while every run stays as it was? Result: yes. A declared table of the 150 keys card RKD deleted drives one warning per key set, naming the key and its deletion commit. Both 2 h goldens and the 16 user-side arms are identical on a same-build pair; a golden with 30 retired keys added differs only by the 30 warnings, which the census predicted exactly. Verdict: PASS on every gate. 1,153 of the 1,234 network-mode configurations now warn. Standing: built, not merged (section 4.5).

## 1. Question

Card RKD deleted 150 network keys with their code on 2026-10-14. The loader has no unknown-key check, so a
configuration that still sets a deleted key loads the old default without a word (plan risk R1; about 712 live
configurations did at the time). A refusal would make those configurations and the retired goldens fail to load
(card RKD section 5), so the follow-on is a warning. `scripts/rapid_arm.py make` already refuses an unread key on
the tool side (`scripts/loader_keys.py`); the loader itself says nothing. The question: can the loader warn once per
retired key, naming the key and the deletion, with no other change to any run?

## 2. Design

| piece | where | what |
| --- | --- | --- |
| the table | `include/config/RetiredConfigKeys.hpp`, `kRetiredConfigKeys` | one row per retired key: the dotted path, the flat top-level spelling the old loader also read, and the family; 150 rows |
| the families | the same header, `kRkd*` | one per deletion commit of card RKD (28): the card, the date, the commit on main and what the keys armed |
| the scan | `findRetiredConfigKeys`, `src/core/config/RetiredConfigKeys.cpp` | one hit per row the merged document sets: the dotted path through nested objects, or the flat key at the top level (the two lookups of `detail::jval`, in its order); `network.stations[].fixed` once for the whole roster |
| the warning | `warnRetiredConfigKeys`, called at the head of `loadNetworkSection_` | one `gnss_config` warning per hit, naming the key, the spellings found, the value, and the card, date, family and commit of the deletion |
| the test | `tests/test_retired_config_keys.cpp` | the pins listed below |

The facts the table rests on, checked against `4bdd693` and the 35 deletion commits before the build:

- The ledger at `4bdd693` holds 141 REMOVE rows. Card RKD's count of 144 is the plan's appendix A, which adds the
  three `lane_carry` rows (KEEP_OFF and PARAM) deleted with the WL-3 carry. With the five `mw_wl` keys the lane layer
  orphaned (PARAM and TRACE rows) and `iono_constraint_deweight_stations` (no row) the deletion is 150 keys; 149
  ledger rows left the ledger (784 to 635).
- 149 keys were read by `jval` with one flat spelling each, and no two share one. `network.stations[].fixed` was read
  from each roster object and had none. All 150 reads sat in network-section helpers (`loadNetwork*_`), so the scan
  runs in network mode only, where the reads stood, and a user-side run cannot change.
- The family of a key is the commit whose loader diff removed its read: 28 commits, with the counts of card RKD
  section 1 and the orphaned keys under `365edfe` (five) and `158b9b2` (one).
- No retired path is read by the current loader (`scripts/loader_keys.py`), and no flat spelling is a string literal
  of `src/core/config/*.cpp`.
- The table is in a header on purpose: `scripts/loader_keys.py` and `scripts/comment_gate.py` read every string
  literal of `src/core/config/*.cpp` as a key the loader reads, so a retired key spelled there would read as live and
  `rapid_arm.py make` would stop refusing it. The `.cpp` holds no key name.

The unit test pins: (1) the rows equal the 141 REMOVE rows of the ledger at `4bdd693` (embedded, blob `c8d31c0e`)
plus the nine keys deleted beside them; (2) the families in commit order with card RKD's counts, the rows of a family
sorted; (3) every `jval` key has its own flat spelling; (4) no row is a key of the current ledger; (5) the current
goldens set none and the retired `golden_field_gps_dual.json` sets 28; (6) the scan: either spelling found once, the
dotted value when both are set, the roster member once, and silence on live keys, near misses, a flat spelling below
the top level and a dotted name used as one literal key; (7) the message text; (8) a load with six retired keys logs
six warnings in table order and otherwise exactly the log of the same configuration without them.

## 3. Gates (frozen 2026-10-30, before any run of the changed executable)

The reference executable is the Release build of main `5127429` in this worktree (`gnss_processor_rktref.exe`,
sha256[:16] `45ddd4c9329e8ced`). The candidate is the Release build of this branch from the same build tree, copied
beside it as `gnss_processor_rkt.exe`. Arms are golden copies made with `scripts/rapid_arm.py make` under
`RAPID_RUNS_ROOT=D:/GnssProcessorSupport/runs_nom`; the sim arms start at 00:30:15 (`network.start_offset_hours`
0.504167). At most two arms run at once while card NOM5's gate runs are live.

| gate | arms | bar |
| --- | --- | --- |
| G0 unit and tools | Debug `gnss_tests`; scripts | the suite `RetiredConfigKeys` passes; the full suite passes apart from the fixture-gated skips; `loader_keys.py selftest`, `make_golden_config.py --verify`, `config_posture_check.py --verify-loader` and `docs_index_check.py` pass |
| G1 field | `_rap_rkt_ref_f2` and `_rap_rkt_cand_f2`, the 2 h field golden | `nom_td_gate.py --whole` IDENTICAL |
| G2 sim | `_rap_rkt_ref_s2` and `_rap_rkt_cand_s2`, the 2 h sim golden | `nom_td_gate.py --whole` IDENTICAL |
| G3 user side | `uom_gate.py` quick set, tags `rktref` and `rkt` | 16 of 16 IDENTICAL |
| G4 retired keys present | `_rap_rkt_ret_f2`: the 2 h field golden plus the 28 retired keys `golden_field_gps_dual.json` sets, at its values, plus `carry_iono` and `network_fast_clock_enable` flat and `parallel_reduce` flat beside its dotted spelling; candidate exe, against `_rap_rkt_cand_f2` | `nom_td_gate.py --whole --no-arm-check`: every product, CSV and run manifest record identical; the logs differ only by inserted warning lines, 30 (one per retired key the arm sets), each naming its key, card RKD and the deletion commit |
| G5 census | every `config/**/*.json` resolved through `extends` (`scripts/gnss_config_chain.py`) | reported with the results: the network-mode configurations that now warn and the keys they set; the census reads the header's table, and on the G4 arm it predicts exactly the warnings the candidate printed |

A gate that fails is reported with the difference; no tolerance is applied to a byte comparison. Results are
appended below under dated headings.

## 4. 2026-10-30: results

### 4.1 Executables

- Reference: `gnss_processor_rktref.exe` (sha256[:16] `45ddd4c9329e8ced`), the Release build of main `5127429`. The
  loader edit landed while that build ran; its loader object was compiled 4 to 47 s into the build and the edit
  after the link, and the executable holds no warning text.
- First candidate: `gnss_processor_rkt.exe` (`ddb363ca2eb4946b`), the code of section 2.
- Final candidate: `gnss_processor_rkt2.exe` (`fb38d0f93899e07b`). After pre-registration the family identifiers were
  renamed from `kRkd*` to `kRetired*` (the factory `rkdFamily` to `retiredOn20261014`) and the comments of the header
  and the test lost their card citations (the code base's comment rule); the warning text did not change. The two
  candidate images are byte-identical once the per-link stamps are masked (the COFF and debug-directory timestamps,
  the checksum, the PDB GUID and age), so the G1 candidate arm, run on the first candidate, stands for the final one.
  G2 to G4 ran on the final candidate (the user-side candidate tag is `rkt2`, where section 3 names `rkt`).

The arms ran beside eight or nine producer runs of other sessions: the G1 field arms took 73 and 75 min (the G4 arm,
run later, 29 min) and the sim arms 21 and 20 min (wall times only; nothing is graded on them).

### 4.2 Gates

| gate | result |
| --- | --- |
| G0 | `RetiredConfigKeys` 10 of 10 (the list names the renamed case, so the binary is current); the full Debug `gnss_tests` 3,662 cases in 498 suites: 3,629 passed, 33 skipped, none failed; every skip is a data-fixture gate, named by rerunning the suites that hold a skip site (25 the SIF registry files, 5 the GPT3 grid, one each the SP3, IONEX and broadcast files: this worktree's `data/` tree is not populated); `loader_keys.py selftest` 85 of 85 (the retired keys are still refused: the table adds no read); `make_golden_config.py --verify` PASS; `config_posture_check.py --verify-loader` 0 gaps; `ledger_render.py --check` matches; `docs_index_check.py` OK; `retired_keys_census.py selftest` 16 of 16. PASS |
| G1 | IDENTICAL: the arm configs, all 113,902 lines of `products.txt`, every CSV in row order, the 12 run manifest records, the whole `stdout.txt` (3,031 lines) and main log (2,488 lines), `rt_engine.log` (386 lines) and the exit code. PASS |
| G2 | IDENTICAL: the arm configs, all 80,704 lines of `products.txt`, every CSV, the 12 manifest records, `stdout.txt` (2,703 lines), the main log (2,168 lines), `rt_engine.log` (382 lines) and the exit code. PASS |
| G3 | IDENTICAL: 16 of 16 arms (`uom_gate.py cmp rktref rkt2`), every CSV and log after the gate's masks, every exit 0. PASS |
| G4 | Against `_rap_rkt_cand_f2`, every product line, every CSV in row order, the 12 manifest records, the main log, `rt_engine.log` and the exit code identical (41 parts). `stdout.txt` (3,031 to 3,061 lines) and `logs/gnss_config.log` (153 to 183 lines) differ by 30 inserted lines alone: the golden arm's log, after the gate's masks, is a subsequence of the other in order, and every inserted line is a warning, one per retired key the arm sets, in table order, naming the key, card RKD, the date, the family and the commit. The flat spellings read as the old loader read them: `(spelled carry_iono, value true)`, `(spelled network_fast_clock_enable, value true)` and `(spelled network.parallel_reduce and parallel_reduce, value false)`, the dotted value where both are set. PASS |
| G5 | reported in section 4.3; the census predicted 30 warnings for the G4 arm's `arm.json` before it ran: the 30 printed warnings equal the prediction key for key, in order, with the same values, spellings and commits. PASS |

### 4.3 Census of the configurations that now warn

Read with `python scripts/retired_keys_census.py --md` (the header's table, `extends` resolved, network mode only):
1,334 JSON files under `config/`; 5 do not resolve (an `extends` base that has moved; the loader refuses them too,
before any warning); 1,234 network-mode configurations; **1,153 warn**, 20,965 warnings in all. Outside
`config/archive/`, 710 warn, card RKD's "about 712 live configurations". 1,105 of the 1,153 set at least one retired
key, outside the parse-only and loader-forced family (inert before the deletion too), to a value other than the old
loader's default: those runs no longer do what their configuration asks. The other 48 restate old defaults. No
configuration uses a flat spelling, and 55 of the 150 keys are set by none.

| directory | network-mode configurations | warn |
| --- | ---: | ---: |
| `config/test_suite` | 686 | 673 |
| `config/archive/test_suite` | 270 | 270 |
| `config/archive` | 234 | 173 |
| `config/sim/arl` | 22 | 22 |
| `config/refound` | 11 | 11 |
| `config` | 7 | 2 |
| `config/golden` | 4 | 2 |

By deletion family (a configuration counts once per family it touches):

| deletion commit | family | keys | keys set | configurations | outside `config/archive` |
| --- | --- | ---: | ---: | ---: | ---: |
| `4c0e731` | the parse-only and loader-forced keys | 7 | 4 | 1068 | 688 |
| `f1ea3de` | the WL-1 MW-seeded wide-lane rows and the F6 dump | 10 | 7 | 49 | 30 |
| `c5be295` | the WL-3 lane-coordinate carry and the UWL family pairs | 11 | 9 | 917 | 625 |
| `819f4a6` | the UWL-1 per-constellation lane tiers | 8 | 6 | 587 | 459 |
| `365edfe` | the WL-2 EWL lane, the WL inject, the GF bridge and the lane layer | 16 | 6 | 621 | 470 |
| `f48868c` | the lane slip responses | 7 | 4 | 12 | 9 |
| `1de8b9b` | the FC-1 split-cadence fast clock | 10 | 6 | 28 | 28 |
| `2ba604a` | the finite-difference C1 clock rate | 1 | 1 | 2 | 1 |
| `b326c8c` | the CLV out-of-solve clock-level re-base | 9 | 0 | 0 | 0 |
| `7914263` | the ensemble phase datum and its hold | 2 | 1 | 2 | 1 |
| `6d719e4` | the PB emission-continuity filter | 8 | 7 | 735 | 522 |
| `08cd639` | the smoothed hold's block member | 1 | 0 | 0 | 0 |
| `b216f55` | the SH health gate | 3 | 2 | 492 | 435 |
| `158b9b2` | the station iono deweight gate and its release hygiene | 14 | 13 | 737 | 523 |
| `1d35585` | the network adaptive iono q | 1 | 1 | 1 | 1 |
| `09e3151` | the T1d SH-convergence latch | 3 | 3 | 75 | 14 |
| `f4be1a8` | the diagonal slant-iono carry | 6 | 3 | 8 | 2 |
| `15b927c` | the level tools LV-1, LV-2, LV-4, LV-5 and S2H | 9 | 5 | 24 | 24 |
| `d1cd9ea` | the retired code-bias datum options and the clock signal-basis hold | 5 | 2 | 422 | 276 |
| `1068750` | the age-aware young-arc inflate | 3 | 3 | 3 | 0 |
| `34793c9` | F1 and the F3 age gate | 3 | 3 | 5 | 5 |
| `e7e470b` | the coupling carry's SH marginalize, joint whitening and gauge-free closure | 3 | 3 | 82 | 17 |
| `98df220` | the retired warm-resume knobs | 4 | 2 | 18 | 18 |
| `6e86437` | the parallel Schur reduce fan | 1 | 1 | 1041 | 686 |
| `dab7d80` | the study-3 band-scoped responses | 2 | 2 | 2 | 2 |
| `475b587` | the de-commoned phase jump test | 1 | 0 | 0 | 0 |
| `159f846` | the unmatched-antenna phase-bias exclude | 1 | 1 | 735 | 522 |
| `8409b43` | the RAIM carry-invariant refloat actuator | 1 | 0 | 0 | 0 |

The most-set keys: `network.parallel_reduce` (1,041 configurations), `network.lane_carry.enable` (917), the seven
`iono_constraint_deweight_*` keys (736 each), `network.emit_pb_filter` and
`network.phase_bias_exclude_unmatched_antenna` (735 each). Configurations outside the archive and the test suite
that warn: the 22 producers of `config/sim/arl/` (21 retired keys each: the lane, PB filter, deweight, unmatched
antenna and parallel-reduce families), the 11 September bases of `config/refound/` (20 each), the two retired
goldens (`golden_field_gps_dual.json` 28, `golden_sim_tri_w6.json` 4), and `config/_rtseam_cl.json` and
`config/ppp_rtk_network_brdc_rts.json` (`serialize_sbasis_contract` alone, a key that was inert before the deletion).
The per-configuration list is `D:/GnssProcessorSupport/gates/rkt/census_list.tsv`.

### 4.4 Card RKD's documentation notes

- One appended, dated note (2026-10-30), naming the retired keys and their deletion commits, on each owing card and
  standing SPEC: AB3, AB1, CLV, CLV2, PBH, SMD, JT, LAH, OWD, RF-IGN, RF-0c, RF-0e, the RF-1 design (a shorter note:
  that file is at the 60 KB bar), GFC, NETWORK_REFOUNDING; S2H-HARVEST, LV-1, LV-2, LV-4, LV-5, LV-EXTCB-CAMPAIGN,
  CARD-1B-JOINT-WHITENING, CARD-1F-1G-TRUNCATED-ROW-GAUGE, READMIT-EDGE, RESUME-FRAME-RESTORE, RS-2, RS-6a,
  LANE-SLIP-RESPONSE, LANE-SNAP-RUNG3, STUDY-3, CAF-COMMENT-AUDIT-FINDINGS; SPEC_mw_wl_card, SPEC_ewl_wl_cascade_card,
  SPEC_lane_carry_card, SPEC_uwl_card, SPEC_lane_spectrometer, SPEC_slip_relevel_walk, SPEC_fast_clock_split_cadence,
  SPEC_c1_clock_rate_validation, SPEC_iono_carry_8b, SPEC_deweight_release_hygiene, SPEC_prefit_census_observability,
  SPEC_raim_pool_rehost, SPEC_warm_resume_event_card (44 notes).
- `SPEC_lane_slip_response.md` is the routing stub of a split monolith and takes no note; its cards
  LANE-SLIP-RESPONSE and LANE-SNAP-RUNG3 carry the notes.
- A dated sentence after the current-posture paragraph of the topic pages slip, clock, perf, ionosphere, code-bias,
  network-vehicle, resume-seam and config-ledger (the form of code-bias's 2026-10-13 line).
- `rt_service_operations.md` section 3.2: a dated note. The producer writes no `SH_GATE` event, no
  `sh_gate_withheld` disposition and no `withheld_sh` heartbeat any more; the consumer still reads `withheld_sh` in
  archived product files.
- NOM3: a dated line (the deletion landed; the gate arms `cil` and `fcs2` left the gate set with their postures).
- Still owed: `docs/onboarding/07_network_tour.md` (the ensemble election, the run-end report count 105; edited on the
  unmerged branch `nom/n6-tail`) and `docs/topics/realtime.md` (uncommitted edits in the main checkout).

### 4.5 Verdict

**PASS on every gate.** The loader names each retired key a configuration sets, once, with the record, date, family and commit of its deletion, and nothing else moves: both 2 h goldens and the user-side quick set are identical on a same-build pair, and a golden with 30 retired keys added differs from the golden only by the 30 warnings, which the census predicted exactly. 1,153 of the 1,234 network-mode configurations under `config/` now warn, 710 of them outside the archive. Standing: built on branch `claude/pensive-ptolemy-4f6046`, not merged; no key added, nothing armed. Left open: card RKD's follow-ons F1 to F3, the two notes still owed (section 4.4), and the configurations themselves (the 22 ARL producers and the 11 refound bases still set 20 or 21 retired keys each; card RKD left every configuration unedited, plan question Q12, so editing them is a separate decision).
