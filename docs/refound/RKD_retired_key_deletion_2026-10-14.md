# RKD: the retired network configuration keys deleted with their code

**STATUS**

- **id:** RKD
- **topic:** config-ledger
- **status:** open
- **summary:** Built 2026-10-14 on branch `rmk/delete`: the 144 network keys the ledger rebuild marked REMOVE, and five lane keys they orphan, deleted with their parse sites, members, getters and every code path they armed. 149 ledger rows leave the ledger (784 to 635); the goldens unset 20 keys in 17 entries and drop one pin. The regenerated goldens equal the gate's reference goldens in every value. Open until the same-build golden comparison (plan section 5.1) is appended.
- **source:** the REMOVE verdicts of the ledger rebuild (main `4bdd693`, [CONFIG_promotion_ledger](../CONFIG_promotion_ledger.md)); the register item "Delete the REMOVE keys" ([OPEN_ITEMS](../status/OPEN_ITEMS.md)); the deletion plan of 2026-10-13 (session scratchpad, `rmk/PLAN.md`: sections 3 to 6, 11 and appendices A and B)
- **keys:** 144 under `network.` (appendix A of the plan, by family in section 1 below) plus `mw_wl.d5_dump`, `.lane_trace`, `.sigma_floor_cyc`, `.max_rows_per_epoch`, `.fix_weight_sigma_cyc` and `iono_constraint_deweight_stations` (no ledger row); `cold_clock_seed` stays (regime `"False"`)
- **dates in the text:** 2026-10-13, 2026-10-14
- **hashes / run dirs:** base main `4bdd693`; deletion `72d84db`..`82f0015` (35 commits, rebased from `d5b33a0`); ledger and goldens `9a5c8d3`; the pre-deletion ledger is `git show 4bdd693:docs/CONFIG_promotion_ledger.json`; candidate exe `build/x64-Release/gnss_processor_rmkcand.exe` (hash recorded with the gate result); gate not run

**Outcome.** Question: can the ledger's retired network keys be deleted with their code while the golden posture moves only on declared lines? The deletion is built: 149 keys leave the loader and the ledger, 30 C++ files (11,281 lines) are deleted, and `src/`, `include/` and `tests/` lose about 31,900 lines net. The regenerated goldens equal the reference goldens of the gate in every value; `make_golden_config.py --verify`, `config_posture_check.py --verify-loader`, `ledger_render.py --check` and the Debug suites pass. Verdict pending: the same-build comparison of section 5.1 has not run. Status: open.

## 1. What was deleted

| family | keys | plan commit |
| --- | ---: | --- |
| parse-only and loader-forced (`serialize_sbasis_contract`, `carry_invariant_honesty`, `carry_invariant_innov_refold`, `prefit_fuse_require_uncheckable`, `stations[].fixed`, `amb_float_q_m2s`, `ambiguity_resolution.wl_cascade`) | 7 | C2 |
| WL-1 MW-seeded wide-lane rows and the F6 dump (`mw_wl.enable` and its knobs) | 10 | C3 |
| WL-3 lane-coordinate carry (`lane_carry.*`) and the UWL family pairs | 11 | C4 |
| UWL-1 per-constellation lane tiers (`mw_wl.uwl_*`) | 8 | C5 |
| WL-2 EWL lane, WL inject, GF bridge, the lane layer files | 11 | C6 |
| lane slip responses (bridge v3 EWL slot, statistical retap, corroboration, lane snap) | 7 | C7 |
| FC-1 split-cadence fast clock (`fast_clock.*`) | 10 | C8 |
| finite-difference C1 clock rate (`emit_clock_rate`) | 1 | C9 |
| CLV out-of-solve clock-level re-base (`clock_level_carry` and eight parameters) | 9 | C10 |
| ensemble phase datum and its hold | 2 | C11 |
| PB emission-continuity filter (`emit_pb_filter` and seven parameters) | 8 | C12 |
| smoothed hold's block member (`emit_pb_hold_smoothed_block`) | 1 | C13 |
| SH health gate | 3 | C14 |
| station iono deweight gate and its release hygiene (`iono_constraint_deweight_*`, `iono_dw_*`) | 13 | C15 |
| network adaptive iono q | 1 | C16 |
| T1d SH-convergence latch | 3 | C17 |
| diagonal slant-iono carry (`carry_iono`, `iono_carry_*`) | 6 | C18 |
| level tools LV-1, LV-2, LV-4, LV-5 and S2H | 9 | C19 |
| retired code-bias datum options and the clock signal-basis hold | 5 | C20 |
| age-aware young-arc inflate | 3 | C21 |
| F1 and the F3 age gate | 3 | C22 |
| coupling SH marginalize, joint whitening, gauge-free closure | 3 | C23 |
| retired warm-resume knobs (RS-2 re-base, RS-6 shdev switch, seam re-inflate, resume shock) | 4 | C24 |
| parallel Schur reduce fan | 1 | C25 |
| study-3 band-scoped responses | 2 | C26 |
| de-commoned phase jump test (card JT) | 1 | C27 |
| unmatched-antenna phase-bias exclude | 1 | C28 |
| RAIM carry-invariant refloat actuator | 1 | C29 |
| orphaned by the lane layer and the deweight gate (plan Q1) | 6 | C6, C15 |

Files deleted whole: 30 C++ files, 11,281 lines (production 5,272 in 15 files: the MW/WL glue
and layer, `MwLaneTiers.hpp`, the fast clock, the PB emit filter, the deweight gate and release
hygiene, the station-iono run end, the young-arc inflate; tests 6,009 in 15 files), 21
`CMakeLists.txt` entries, `scripts/mwwl_cond_arm.py` and the five `goldens/decomp` manifests of
the retired INC-4 gate arms (lane, uwl, wlinj, jw, cpljw). Over the branch `src/`, `include/` and
`tests/` lose 34,776 lines and gain 2,910. The run-end table goes from 116 to 105 reports.
Behaviour that becomes unconditional at the old defaults is listed in section 4 of the plan
(for example the T5 half of the RS-6 restore, the serial Schur reduce, the hard unmatched-antenna
exclude).

## 2. Plan questions: the defaults adopted

- **Q1:** the six orphaned keys deleted with their families (two more manifest fields, M20).
- **Q2:** no retired-key tombstone in this branch; a warn-once loader table is follow-on F4.
- **Q3:** the fingerprint term of `emit_clock_rate` stays as the constant `add("false")`.
- **Q4:** the snapshot records `O`, `PF` and `PS` lose their parsers (such a snapshot cold-starts).
- **Q5:** vocabulary that printed only zero counts deleted (M3, M11 to M13, M22 to M25).
- **Q6:** golden-printed text naming a deleted mechanism edited (M4, M9, M14, M16, M17), M18
  deleted; the epoch-status deweight columns stay as empty columns.
- **Q7:** wire vocabulary: F/K records and `discdecom` deleted; `lvl=`, `gfpin`, `pbhold=4` and
  `withheld_sh` kept.
- **Q8:** library code kept (`NetworkNormalEquations::reduce(int)`, the sum-to-zero gauge kernel,
  `promoteBridgingArcs`, the CLV jump counters, the SMD block-clone instrument);
  `JointWhitenCouplingMarginalizationEqualsBatch` deleted.
- **Q9:** the relocation verifier (gate G-TD-R, `nom_td_verify.py`) maintained;
  `nom_hv0_verify.py` is historical.
- **Q10:** `cold_clock_seed` keeps its verdict with regime `"False"` and a `posture_why`; its
  posture row is deleted; no output move.
- **Q11:** the retired goldens stay frozen; the NOM3 gate arms `cil` and `fcs2` leave the gate set;
  AGT and IRD identity references are re-taken after the deletion.
- **Q12:** no config edited; the dead graders are kept for the archived runs and marked
  archive-only in `scripts/README.md`.
- **Q13, Q14:** follow-ons F1 to F4 (section 5); this card, topic config-ledger.

## 3. Declared output moves on the goldens

Relative to a same-build reference on main `4bdd693`, plan section 5.2 (identical on both beds
unless stated):

- **gnss_config (stdout):** M1 the posture header loses `{} lane layer, `; M2 the RF-1 startup
  line loses `amb_float_q_m2s`; M3 the process-noise line prints 11 classes, not 12; M4 the
  RXCB-DATUM line loses its `external_sat_code_bias` clause.
- **main_log and stdout:** M5 every per-epoch D0b line loses `fast-clock=`; M6 the RF-1 run end
  loses `q=`; M7 and M8 the arc-ledger lines lose the `iono_carry_max_gap_s` term (horizon 3900 s
  unchanged); M9 the param-model `slant_iono` source; M10 the S-contract declaration loses
  `gfpin=[...]`; M11 seven contribution kinds and three contribution texts; M12 the
  `clock_level` mechanism leaves the carry-site lines; M13 the response summary reads
  `32 of 65 ..., 33 not armed`; M14 the leveling census [C] loses the SH-convergence flush clause;
  M15 the F3 line loses the age-gate clause; M16 the wind-up restart line loses the lane
  observatory sentence; M17 the prefit census INC-4 line loses the deweight sentence; M18 the
  producer OWD re-inflation line disappears.
- **logs/network_run_manifest.json:** M20 the `startup` record loses 36 fields and one
  process-noise member; M21 the `param_model` source; M22 the `unmatched_antenna` reason; M23 the
  contribution objects of M11; M24 the `clock_level` entries of `param_sites`; M25 four response
  sites and the `clock_level` mechanism.
- **products.txt:** M26 the two FC-1 legend lines and the `[,discdecom=1]` token of the `S` legend.
  Records, the `S,` data line and every CSV are unchanged. M19 is not used.

Moves added by the review rounds (both goldens unless stated):

- `Network (rhs-attrib INC-5): flight recorder ARMED ...` loses `SH-gate engage, ` (C14).
- `Network (SAT-GF TIME-LINK, RF-0e e2): ...` ends at "which is the cost of this arm." (C20).
- The provenance line of `logs/network_amb_state.csv` no longer lists the F1 B_eff scale (C21,
  C22).
- `Network (slip-QC card G / gap instrumentation): ...` loses its D5 attribution clause (C4, C6).
- The `pb_hold` reason text of the `eq34_shift` and `declared_handover` mechanisms in the
  `param_sites` record no longer names CLV (part of M24).
- M14 as rewritten: `[C] ONE-SOLVE REFLOATS: {} T4 attribution and {} T3 consensus
  (quality-selected); ...`.
- Off the goldens: the card DG over-specified withhold warning and the `Card 4 D5 v4` re-tap-only
  info line.

The posture tallies do not move against `4bdd693`: the deleted ON and SCOPED rows
(`serialize_sbasis_contract`, `phase_leveling_los_scale` and the others) had already left that
posture table, and the `cold_clock_seed` row never applied (regime `lane`, false on both goldens).
Against `d5b33a0` the closing tally moved from ON 24/25 to 23/24; that move belongs to `4bdd693`.

## 4. Snapshot fingerprint

The fingerprint is computed only with a snapshot path, so the goldens compute none. For stored
snapshots: the constant `add("false")` keeps every snapshot written with `emit_clock_rate` off
resumable. The token `pbexclunmatched=true` is no longer formed, so a golden-lineage snapshot
written before the deletion cold-starts (or refuses under `state_snapshot_require`); the
re-founding posture writes none, and the gate's `snapw` / `snapr` arms are re-taken from the
post-deletion reference. Armed-only tokens that disappear (`phdatum:ens`, `pl:losscale`,
`extcb:1`, `ionocarry:...`, the rx-CB level-tool tokens, `clv:`, `clvrp:`, `clv2:fcb`, `lv1seed:`,
`discdecom=1`, `clkbasishold=true`, `cplshmarg=true`, `carrycpljoint=true`; `cta:gf+cl` becomes
`cta:gf`) cold-start a snapshot written with the knob armed. A snapshot holding `O`, `PF` or `PS`
records fails to parse and cold-starts; `ZL` rows still parse.

## 5. Follow-ons and open edges

- **F1:** the empty diagonal `iono_carry` map and its two carry sites (declared moves then: two
  site lines, the `param_sites` record, the "carried instances" text).
- **F2:** remove the `erase_keep_iono` inject mode (it now acts like `erase`).
- **F3:** renames (`lane_lk` / `lane_lit`, `test_resume_carry_rebase.cpp`).
- **F4 (plan Q2):** a retired-key tombstone, a warn-once loader table of the deleted keys. There is
  no unknown-key check, so the 712 live configs that set a deleted key load the default silently
  (plan risk R1); a refusal instead would make them and the retired goldens fail to load.
- **Gate owed:** plan section 5.1 (2 h field and sim arms through `nom_td_gate.py --whole`), the
  user-PPP run (R5), a 2 h field arm with `network.clock_level_monitor` (R7), re-taken
  `snapw` / `snapr` snapshots.
- **Stale references:** the INC-4 gate goldens (allon, snap, snap_resume, obs, estim, consumer)
  predate the deletion; the NOM3 EP1 tooling (`scripts/nom_ep1/`, `scripts/nom3_designs/EP1_spec.md`)
  still models the lane objects and is re-surveyed before EP1a-1 is re-run.
- **Documentation notes:** listed in [OPEN_ITEMS](../status/OPEN_ITEMS.md) ("Retired-key deletion:
  documentation notes").
