# IRDG: the D0 ruling enacted, and the receiver phase bias carried inside the station block

**STATUS**

- **id:** IRDG
- **topic:** network-vehicle
- **status:** closed
- **summary:** Pre-registered 2026-11-09 before any graded run, graded the same day. Part 1 enacts the author's D0 ruling of 2026-11-09 (card IRDF section 9): the receiver phase-bias floor is off on arm A1 and every arm derived from it in `scripts/ird_make_arms.py`, each with a named floor-on variant, and I-b of every IRD-4 event row is read again on that arm (20 beds, 2 h). No row passes I-b on every bed; two cells pass outright (the slip on `ird_tr`, the split on `ird_msr`), the integer clause holds on 11 of 19 cells, and the misses sit at the level the same pairs reach at quiet epochs, except the merge of `ird_msr`, which steps by integers against its control. The hold of card IRDC section 7.3 is not released: its conditions read 0 wrong on r2 and r3c and 863 wrong would-be certifications on the microscope. Part 2 builds the fix of the time link's double count: `network.phase_datum_eq34_rpb_in_block` (default off, ledger UNVALIDATED) carries the receiver phase bias as a column of the station's block. Off, products and traces are identical on both goldens and the arm. On, the bias's formal sigma follows the block (1.99 and 1.96 m at 2 h on r3c and the golden sim against 0.018 and 0.019 m with the link), the DCB error is the prior's own (0.538 m against 0.823 and 0.618), the search makes 0 wrong would-be fixes (228 and 124), a root handover is pinned at its carried value, and the key arm reads as the no-link arm in every reading. Two clauses FAILED: the held-out float consumer on the golden sim in hour 1 (+24 % against the no-chain wire, bar +10 %), and I-b's counts rise against the link arm on 17 of 24 cells (the split on `ird_msr` passes with the link and fails with the key), both the price of the bias at its honest variance. Adoption of the key on the arm going forward is the author's ruling.
- **source:** [IRDF](IRDF_d0_receiver_floor_redecided_2026-11-07.md) sections 5.1, 7 and 9 (the ruling and the defect); [ARR](ARR_integer_ambiguity_review_2026-11-07.md) section 7 (the order of stage S0); [IRD](../../refound/IRD_integer_recovering_datum_2026-10-13.md) sections 5, 7, 9 and 11 (the event table, the key's reserved name, bar I-b, the increments); [IRDC](../../refound/IRDC_integer_recovering_datum_events_2026-11-02.md) section 7.3 (the hold); [IRDD](../../refound/IRDD_integer_recovering_datum_rows_2026-11-03.md) and [IRDE](../../refound/IRDE_integer_recovering_datum_slip_2026-11-06.md) (the rows' gates and fixtures)
- **knobs / posture:** `network.phase_datum_eq34_rpb_prior_sigma_m` 0 on arm A1 and its descendants; new key `network.phase_datum_eq34_rpb_in_block` (default off; the name card IRD section 7 reserves); readings set `network.phase_datum_eq34_rpb_q_m2s` 0 (the no-link arm). No golden change, nothing armed
- **dates in the text:** 2026-11-09
- **hashes / run dirs:** branch `ird/floor-off-rpb-block` (from `docs/rulings-2026-11-09` at `58d0b3b`; pre-registration `3f3be21`, the build `4500e53` and `88f643f`); base exe `D:/GnssProcessorSupport/gates/exe/x64-Release/gnss_processor_irdg_base.exe` (sha256[:16] `0b50425c9118a037`, the AVX2 default build of `58d0b3b`), candidate exes `gnss_processor_irdg_c1.exe` (`764b8e7cce4d1a36`) and `gnss_processor_irdg_c2.exe` (`8694fdac9b915e23`, the final tree); arms `_rap_irdg_*` (110 runs) and grades `_irdg_grades/` under `D:/GnssProcessorSupport/runs_nom/`; tables `scripts/irdg_run.py`, readings `scripts/irdg_grade.py`
- **gates (frozen at pre-registration):** section 3 (Part 1's readings and the hold's conditions) and section 4 (Part 2's bars)

**Outcome.** Question: with the receiver floor off by the author's ruling, which IRD-4 event rows pass their integer test, and does carrying the receiver phase bias inside the station block remove the time link's double count while a root handover keeps its target? Decisive result: no row passes I-b on every bed and the hold stands (863 wrong would-be certifications on the microscope); the in-block key removes the double count (formal sigma 1.99 m against 0.018, DCB error 0.538 m, 0 wrong fixes) and keeps the root's target. Verdict: the key LANDED default off; the held-out consumer (+24 %, golden sim hour 1) and the I-b no-regression clause FAILED. Standing: adoption on the arm going forward is the author's ruling ([section 7.6](#76-verdict)).

## 1. Why

On 2026-11-09 the author ruled "Yes, D0 floor off." (card IRDF section 9) and "Yes, S0 the chain first, the freeze second, and decline the transform." (card ARR section 7). Card IRDF section 9 lists what the ruling leaves owed: the floor off in the arm definitions, the event rows' I-b read again on that arm, and the in-block build that removes the time link's double count (card IRDF section 5.1). This card does the three. Neither golden runs the chain, so the goldens and the field are untouched; card IRD section 6 holds every new field chain arm until IRD-6.

## 2. The ruling's two clauses, as read here

Card IRD section 10 wrote decision D0 with three clauses (DCB spread, W5 withholds, float consumer). The ruling reads two of them otherwise, and this card uses them in that form:

1. **The DCB clause, against truth.** The statistic is the sd across receivers of the DCB error (the trace's `GPS_DCB` plus the fixture's true C2W less C1C, card IRDF section 4.2) at the end of hour 2. The bar is the rule's own factor on the no-chain arm: at most three times the error of the no-chain reference R1 on the same exe (1.62 and 1.64 m on r3c and the golden sim on card IRDF's exe).
2. **The consumer clause, at the held-out sites.** Nine sites outside the network (S04, S11, S18, S25, S32, S39, S46 and S53 replayed from a wire made without them, and SZZ000SIM from the wire's own trace, card IRDF section 4.1). The statistic is the median over the sites of each site's 3D p50 in hour 1 and in hour 2, with the rule's own +10 %.

The W5 clause stands as written (withholds no higher). The verdict of card IRDF section 3 as written stays in the record.

## 3. Part 1: the floor off on the arm going forward (no solver code)

**The arm definitions.** In `scripts/ird_make_arms.py` arm A1 and every arm derived from it (A1k to A1krhRsm) set `network.phase_datum_eq34_rpb_prior_sigma_m` 0. Each keeps a named floor-on variant (`<arm>_floor`, the floor at 1 m written out), which reproduces the arm as it was defined before 2026-11-09. The A0 family is unchanged (A0 keeps the floor, A0f has it off, neither has the pair family). An arm made before this change records its own knob list in its `rapid.json`.

**The readings, frozen before the runs.** The floor-off arm is A1krhRsm as redefined, 2 h, on the base exe of the STATUS block:

1. **I-b per event row** (card IRD section 9, graded by `scripts/simnet_chain_frame_grade.py --source prehold`): at the row's event epochs, every integer step declared (0 undeclared integer steps) and every undeclared pair's wrapped step at most 0.02 cycle per band on the microscope beds or 0.05 cycle on the wide lane elsewhere. A row passes when both clauses hold on every bed named for it; a missed clause is reported as FAILED. The beds: `kept` and `rehost` on r3c and the microscope; `returning` on `ird_rv16`; `root` on `ird_rg` and `ird_rg_mic`; `slip` on `ird_t1`, `ird_tr`, `ird_t1m` and `ird_trm`; `merge_split` on `ird_ms`, `ird_msr`, `ird_ms_mic` and `ird_msr_mic` (the split and the merge each). The two beds of card IRDF (r3c and the golden sim) are graded for every class they exercise.
2. **Attribution beside the bar, not a bar:** the same pairs against the fixture's control (r3b, the microscope, `ird_ms_ctl`, `ird_msr_ctl` and their microscope twins) and at the quiet epochs of the prior 10 min, as cards IRDD and IRDE report them.
3. **The hold of card IRDC section 7.3.** Its text releases the certified branch "only on an arm whose would-be certification grades 0 wrong (`ird_frame_census.py cert`) with no certifier failure (`ird_frame_census.py nees`), on r2 and r3c (the rule's beds) and on the microscope", and adds that on A1, which kept the floor, "the release waits on the GF-datum defect (decision D0)". Read before any run: the ruling removes the floor from A1, so the wait named in the third bullet ends, but the release itself is a measurement and not a consequence of the ruling. The floor-off arm is graded by both commands on the three beds; the hold is released only if all three read 0 wrong with no certifier failure, and it stands otherwise.

**Seen before this pre-registration.** Card IRDF's readings of the floor-off arm on r3c and the golden sim, and the floor-off rows of cards IRDD and IRDE, which ran the signal family (A0f and its descendants) on the earlier SSE2 exes. The pair family reproduces the signal family where both bands carry every arc (card IRDC section 5), so those rows predict this reading; none was run on this exe or on A1 as redefined.

## 4. Part 2: the receiver phase bias as a carried column of the station block

### 4.1 The defect and the build

Today the harvest (`networkHarvestRxPhaseBias`, `src/core/network/solve/ChainDatum.cpp`) stashes each unpinned receiver phase bias with the diagonal conditional variance of its column, and the next epoch folds it as a diagonal prior (`Eq34RpbTimeLink`, weight 1 / (carried variance + q gap), q = `network.phase_datum_eq34_rpb_q_m2s`). The station's block already carries the ambiguities, the slant ionosphere and the receiver code bias with their cross covariances, which hold the same information, so the bias's information is counted again each epoch (card IRDF section 5.1: formal sigma 0.018 m at 2 h against 2.14 m with no link; DCB error 0.82 and 0.62 m; 228 and 124 wrong would-be fixes). The receiver code bias had the same defect and moved its columns into the block (`network.carry_rx_code_bias_in_block`).

The build, behind `network.phase_datum_eq34_rpb_in_block` (default off, byte-identical when off): the unpinned receiver phase-bias columns join the station's harvest block and are stashed with their border regression and their block row; at the next epoch they fold as columns of the station's whitened block, and the diagonal time-link row is not built (no diagonal fallback, as for the receiver code bias). The carried entry stays the one the root row reads (`applyRootRow`), so a new root is pinned at its carried value. With the link merely switched off (q 0) the harvest stashes nothing and a root handover finds no carry and pins at zero. The steps follow `docs/onboarding/08_adding_a_network_parameter.md` and the station-build and carry tables.

**Event responses of the new carried column (standing invariants 1 and 7), each counted:** the chain's unknown step and cut release erase the entry (the existing `rpb_eq34_erase`); a known shift moves its value and its regression terms with the frame; a gap past the horizon is not folded and the entry is pruned (`rpb_gap_skip`, `rpb_gap_prune`); a step between two harvested values beyond 10 sigma erases the entry (new, the receiver code bias's rule); a column refused by the block, a block that does not factor and an entry without a block row are counted as not folded. The decoupling and re-coupling edges are the block's own: a column that leaves the block is marginalized, and one that returns re-enters with its own row.

**Declared before the build.** The key's name is the one card IRD section 7 reserves for IRD-5b (`network.phase_datum_eq34_rpb_in_block`, "only if D-N fires"). D-N does not fire on the arm going forward (card IRDE section 9.7); the build is owed by the ruling instead. IRD-5b's own gate (the RPB NEES within [0.5, 2] and the W bars against arm A2) is not graded here and stays owed; the NEES is reported as a reading.

### 4.2 The arms

One candidate exe (the branch tip with the key) and the base exe of the STATUS block, 2 h arms, `_rap_irdg_*`:

- **Off identity:** the field golden and the sim golden (arm G), and the floor-off arm on r3c, each on the base exe and on the candidate with the key absent.
- **The key:** A1krhRsm plus the key (the one knob under test) on r3c and the golden sim, on the microscope and r2, and on every row fixture of section 3.
- **References on the candidate exe:** the floor-off arm (the link as built), the no-link arm (`network.phase_datum_eq34_rpb_q_m2s` 0) and the no-chain arm R1 on r3c and the golden sim; the no-link arm on `ird_rg` (the root contrast).
- **Held-out wires:** the no-chain arm, the floor-off arm and the key arm on r3c and the golden sim, each re-run on the 52 stations that exclude the eight held-out sites and replayed at the eight.

### 4.3 The bars (frozen)

| bar | statement | from |
| --- | --- | --- |
| G1 | Off: with the key absent, the products, every trace CSV and the run manifest are byte-identical between the base exe and the candidate on the field golden, the sim golden and the floor-off arm on r3c (`scripts/nom_td_gate.py --whole`); the log lines that differ are listed and are rows of run-start or run-end tables that name the new kind | standing invariant 6 |
| G2 | The formal sigma follows the block: the p50 over unpinned receivers of the bias's formal sigma (`network_rpb_states.csv`) with the key reads between 0.5 and 2 times the no-link arm's at 900, 1 800, 3 600 and 7 170 s after the first epoch, and at least 10 times the link arm's at 7 170 s, on r3c and the golden sim | card IRDF section 5.1 (no link 5.62, 3.95, 2.99, 2.14 m; link 0.021, 0.018, 0.018, 0.018 m) |
| G3 | The DCB error against truth at the end of hour 2 reads at the level of the prior alone: at most 1.10 times the no-chain arm's on the same exe, on both beds, which lies inside the ruling's clause (three times the no-chain arm's) | card IRDF section 4.2 (no chain 0.540, 0.547 m; no link 0.538; link 0.823, 0.618) |
| G4 | 0 wrong would-be fixes of the search on both beds (`scripts/simnet_producer_ar_grade.py`, window h0_12) | card IRDF section 4.3 (link 228, 124; no link 0, 0) |
| G5 | The float consumer at the nine held-out sites: the median of the site p50s in hour 1 and in hour 2 within +10 % of the no-chain wire's on the same exe, on both beds | the ruling's consumer clause (section 2) |
| G6 | A root handover keeps its target: on `ird_rg` and `ird_rg_mic` with the key, both bands' handovers read `outcome=pinned` at the new root's carried value (2 pinned, 0 without a carry at run end), and the undeclared integer steps at the handover are 0; the no-link arm on `ird_rg` reads no carry; a unit test covers the entry the root row reads | card IRDD section 5.3 |
| G7 | I-b on the event rows with the key: on every bed of section 3, per class, the undeclared integer steps and the pairs over the fractional bar are no higher than the floor-off arm's of Part 1; each clause is also reported against the frozen bars of I-b, a miss as FAILED | card IRD section 9 |
| G8 | The new column's responses act: block columns folded on every bed; in-block entries erased by the chain's releases on r3c (count above 0); the gap prune fires on `ird_rv16` (a 990 s gap against the 900 s horizon); the step rule has a unit test, and its count on the quiet beds is reported (a count above 0 there is a false alarm, named) | standing invariants 1, 4 and 7 |
| G9 | Rank and emission: 240 of 240 epochs emitted, no station block dropped, no block left unfactored, on every arm with the key | card IRDF section 4.5 |
| G10 | The in-loop float consumer (SZZ000SIM) by hour within max(+10 %, +5 mm) of the floor-off arm on r3c and the golden sim | card IRD section 11 (IRD-4's regression clause) |
| G11 | The Debug `gnss_tests` suite with 0 failed; `nom_epoch_lint.py`, `nom_n4_verify.py --whole`, `docs_index_check.py`, `make_golden_config.py --verify`, `ledger_render.py --check` and `ird_make_arms.py --selftest` pass | the project's checks |

**Readings without a bar:** the wet delay against truth after hour 1, the reference-free double differences, the stop rule's event-free chords, the search's ceiling, the RPB NEES, the loop time per epoch, and the hold's two commands on the key arm.

**Seen before this pre-registration:** card IRDF's numbers for the link and no-link arms on its exe. No arm of this card had run, and the candidate was not built.

### 4.4 What the verdict will say

Each bar as PASS or FAILED, with the number. "The in-block carry removes the double count" is claimed only if G2, G3 and G4 all pass on both beds. Whatever the bars read, the key lands default off with a ledger row UNVALIDATED, the arm definitions do not adopt it, and adoption on the arm going forward is the author's ruling.

## 5. Out of scope, listed as owed

- Any field chain arm (card IRD section 6: held until IRD-6 has passed).
- The replacement of the emission freeze (it follows the chain, by the author's ruling of 2026-11-09).
- IRD-5b's gate against arm A2, and IRD-5 itself.
- IRD-7's truthful `signal_integer` declaration.

<!-- Results are appended below this line under dated headings. -->

## 6. 2026-11-09: Part 1, the floor off on the arm going forward

### 6.1 What ran

`scripts/ird_make_arms.py` carries the ruling (commit `3f3be21`): A1 and its six descendants set the floor to 0, each with its `<arm>_floor` variant; the selftest checks both. The floor-off arm (A1krhRsm) ran for 2 h on 20 beds on the base exe (`gnss_processor_irdg_base.exe`, sha256[:16] `0b50425c9118a037`): r3c, the golden sim, the microscope, r2, the eleven row fixtures and five controls (`_rap_irdg_f0_*`; table `scripts/irdg_run.py`, readings `scripts/irdg_grade.py`). Every arm exits 0 with every epoch emitted and no station block dropped.

**The exe against card IRDF's.** On r3c and the golden sim the base exe reproduces card IRDF's floor-off readings to the printed digit: the bias's formal sigma (29.3, 22.1, 10.8, 5.97, 2.44, 0.277, 0.072, 0.021, 0.018 m at 0 to 1 800 s on r3c), the DCB error (0.823 and 0.618 m), the search (28 164 kept with 228 wrong; 103 088 with 124) and the float consumer (0.277, 0.031 m; 0.093, 0.033 m).

### 6.2 I-b per event row

The grader is `simnet_chain_frame_grade.py --source prehold`; "integer" is the count of undeclared integer steps over the two bands and the wide lane; the fractional clause is read on the bar's channel (the wide lane at 0.05 cycle on the free beds, E1 at 0.02 cycle on the microscope beds). The last column is the attribution of section 3 item 2: the event epoch's undeclared pairs less the control's (largest difference on the bar's channel, integer pairs), and the largest wrapped step the same pairs reach at the no-event epochs of the prior 10 min (median over those epochs).

| row | bed | class, events | integer clause | fractional clause | I-b | against the control; quiet level |
| --- | --- | --- | --- | --- | --- | --- |
| kept | r3c | kept, 51 | FAILED: 5 836 | FAILED: WL max 0.500, 4 828 pairs | FAILED | no single event; the non-event epochs read WL p50 0.012, 14 167 pairs over the bar, 16 988 integer |
| kept | microscope | kept, 51 | FAILED: 1 091 | FAILED: E1 max 0.498, 6 190 pairs | FAILED | non-event E1 p50 0.0062, 22 238 pairs over the bar, 1 637 integer |
| returning | `ird_rv16` | returning, 3 | FAILED: 204 | FAILED: WL max 0.367, 209 pairs | FAILED | at the station's return (TOW 266520): WL 0.335 on 38 pairs, 0 wide-lane integer; less the control 0.021, 0 integer; quiet 0.107 |
| rehost | r3c | rehost, 47 | FAILED: 4 971 | FAILED: WL max 0.499, 4 104 pairs | FAILED | as the kept row |
| rehost | microscope | rehost, 47 | FAILED: 1 609 | FAILED: E1 max 0.500, 5 435 pairs | FAILED | as the kept row |
| root | `ird_rg` | root_handover, 1 | pass: 0 | FAILED: WL max 0.072, 12 of 91 pairs | FAILED | less the control 0.006, 0 integer; quiet 0.031 (largest 0.100) |
| root | `ird_rg_mic` | root_handover, 1 | pass: 0 | FAILED: E1 max 0.021, 2 of 91 pairs | FAILED | less the control 0.015, 0 integer; quiet 0.022 (largest 0.084) |
| slip | `ird_tr` | slip_deseat, 1 | pass: 0 | pass: WL max 0.026 | **PASS** | less the control 0.004, 0 integer; quiet 0.035 |
| slip | `ird_t1` | slip_deseat, 1 | FAILED: 56 (E1 11, E2 45, wide lane 0) | FAILED: WL max 0.213, 74 pairs | FAILED | less the control 0.010 on the wide lane (0.144 on E1), 0 integer on any channel; quiet 0.142 (largest 0.446) |
| slip | `ird_t1m` | slip_deseat, 1 | pass: 0 | FAILED: E1 max 0.038, 53 pairs | FAILED | less the control 0.066, 0 integer; quiet 0.039 (largest 0.372) |
| slip | `ird_trm` | slip_deseat, 1 | pass: 0 | FAILED: E1 max 0.037, 47 pairs | FAILED | less the control 0.016, 0 integer; quiet 0.024 (largest 0.059) |
| merge_split | `ird_msr` | split, 1 | pass: 0 | pass: WL max 0.048 | **PASS** | less the control 0.006, 0 integer; quiet 0.113 |
| merge_split | `ird_ms` | split, 1 | FAILED: 33 (E1 6, E2 27, wide lane 0) | FAILED: WL max 0.189, 129 pairs | FAILED | less the control 0.012, 0 integer on any channel; quiet 0.279 |
| merge_split | `ird_ms_mic` | split, 1 | pass: 0 | FAILED: E1 max 0.053, 130 pairs | FAILED | less the control 0.010, 0 integer; quiet 0.303 |
| merge_split | `ird_msr_mic` | split, 1 | pass: 0 | FAILED: E1 max 0.120, 261 pairs | FAILED | less the control 0.008, 0 integer; quiet 0.218 |
| merge_split | `ird_ms` | merge, 1 | pass: 0 | FAILED: WL max 0.080, 26 pairs | FAILED | less the control 0.002, 0 integer; quiet 0.096 |
| merge_split | `ird_msr` | merge, 1 | FAILED: 131 | FAILED: WL max 0.485, 202 pairs | FAILED | less the control 0.696 on the wide lane with 8 integer pairs (E1 2.27, 138 integer); quiet 0.072 |
| merge_split | `ird_ms_mic` | merge, 1 | pass: 0 | FAILED: E1 max 0.134, 49 pairs | FAILED | less the control 0.002, 0 integer; quiet 0.042 |
| merge_split | `ird_msr_mic` | merge, 1 | pass: 0 | FAILED: E1 max 0.367, 231 pairs | FAILED | less the control 0.949 with 25 integer pairs; quiet 0.086 |

The two beds of card IRDF: on r3c the classes it exercises (kept, rehost, returning and their releases) FAILED as above; on the golden sim kept (92 events: 8 246 integer, WL max 0.500 on 7 925 pairs), rehost (85: 8 150; 7 526 pairs), returning (2: 148; WL 0.436 on 173 pairs) and slip (1: 159; WL 0.358 on 131 pairs) FAILED, with the non-event epochs at WL p50 0.011 and 8 385 integer steps.

**Which rows pass.** No row passes I-b on every bed named for it. Two cells pass outright: the slip on `ird_tr` and the split on `ird_msr`. The integer clause holds on eleven of the nineteen cells: both root handovers, three of the four slips' beds (the fourth, `ird_t1`, reads 56 per-band steps with none on the wide lane and none against its control), every split but `ird_ms` (33 per-band, none on the wide lane, none against the control) and the merges of `ird_ms` and of both microscope twins. The merge on `ird_msr` and its microscope twin step by integers against their controls (8 wide-lane pairs; 25 on E1), the event card IRDE section 8.5 recorded and did not trace. The kept, rehost and returning rows fail both clauses at the level their beds reach with no event.

**Against the earlier floor-off rows.** Every cell that cards IRDD and IRDE graded with the floor off on the signal family reads the same on the pair family and this exe (`ird_tr` PASS, `ird_t1` 0.213 on 74 pairs with 56 integer, the microscope twins E1 0.038 and 0.037, the split of `ird_msr` PASS, of `ird_ms` 0.189 on 129 with 33, the merges 0.080 and 0.485), as the pre-registration predicted from card IRDC section 5.

### 6.3 The hold of card IRDC section 7.3

The floor-off arm on the hold's three beds (`scripts/irdg_grade.py hold`):

| bed | determined chord integers | wrong against truth | wrong certified re-hosts | `cert` | `nees` |
| --- | --- | --- | --- | --- | --- |
| r3c | 3 370 (1.8 % of 93 178 chord rows per band, 23 arcs) | 0 | 0 | PASS | SOUND: 0 wrong determined rows within the honest bound |
| r2 | 3 406 (1.8 %, 23 arcs) | 0 | 0 | PASS | SOUND |
| microscope | 104 735 (56 % of the rows, 567 arcs) | 863 | 2 | **FAIL** | SOUND |

**The ruling does not release the hold.** The hold's text releases the certified branch only where the would-be certification grades 0 wrong on all three beds. With the floor off it does on r2 and r3c, where one chord row in fifty is determined, and it does not on the microscope, the one bed where most chords are determined: 863 wrong integers and 2 wrong certified re-hosts, none of them a certifier failure. What the ruling changes is the hold's third bullet: A1 no longer keeps the floor, so the release no longer waits on decision D0. The certified branch stays held, and every re-host keeps the uncertified rule.

### 6.4 Reading

1. The floor off does what cards IRDE and IRDF measured, on the pair family too: the rows' own steps are at the control's level on every bed but the merge of `ird_msr`, and the integer clause holds wherever a single event is graded on the wide lane or on the microscope, except there.
2. The fractional bar is missed at the event epochs by what the same pairs do at quiet epochs: the pre-hold wire of a float solution in its first two hours moves by more than 0.02 cycle per band (0.05 on the wide lane) from one epoch to the next on thousands of pairs with no event at all. A row graded over tens of event epochs (kept, rehost) cannot pass before that level falls.
3. The hold stands on the microscope's 863 wrong would-be certifications, which the nees reading attributes to floats off their own covariance and not to the certifier (the float defect of card IRDC section 6).

## 7. 2026-11-09: Part 2, the receiver phase bias in the station block

### 7.1 What was built

Branch `ird/floor-off-rpb-block`, commits `4500e53` and `88f643f`. Key `network.phase_datum_eq34_rpb_in_block`, default off, ledger UNVALIDATED, fingerprinted (`rpbblk`).

- **The route** (`network/carry/RxPhaseBiasCarry`, new). PASS B: step `rx_pb_block` of `kStationCarrySteps`, after the receiver code bias, collects the station's carried unpinned receiver phase-bias columns for the local block; the local-block step folds them as rows of kind `BlockRxPbCarry`. Harvest: `networkHarvestRxPbBlock`, after the receiver code bias and before the border closes, stashes each unpinned column's absolute value, its pure conditional variance, its regression on the border and its row of the station's harvest block (a new segment, `BlockSegment::RxPhaseBias`). The process noise stays `network.phase_datum_eq34_rpb_q_m2s`, on the block's diagonal.
- **The diagonal route steps aside under the key:** the pin step builds no time-link row and the diagonal harvest stashes nothing; the gap prune serves both routes. There is no diagonal fallback: a column that does not fold through the block carries nothing that epoch.
- **The root row is unchanged.** It reads the same carried entry (`StationSignalState::rpb`), whichever route wrote it. A datum receiver's pinned bias is in neither route.
- **Event responses, each counted:** the chain's unknown step or cut release erases the entry (`rpb_eq34_erase`); a known shift moves the value and the regression's terms (the traversal of carried references already lists the site); a gap past the horizon is not folded (`rpb_gap_skip`) and the entry is pruned (`rpb_gap_prune`); a step between two harvested values beyond 10 sigma of both variances, the walk and a (0.05 m)^2 guard erases the entry (`rpb_step_erase`, new); a block that does not factor leaves the columns unfolded (`not folded`).
- **The loader** refuses the key without the chain, with a zero q, under `carry_invariant`, without coupling rows or without a local block (hard errors).
- **Declared tables:** a contribution kind, a response site, the registry's route (`local_block`), the shared-history map, an entity-writer row, a run-end report (`Network (RPB-IN-BLOCK)`, which warns when the key is set and no column entered a block).
- **Tests.** `tests/network/test_rx_pb_carry.cpp`, eight cases: the step rule; the block-column decision (the gap gate, the block rule, the variance); the loader's interlocks; the predicates and tables that follow from the key; the block's algebra with a receiver phase-bias column; and the double count as an information statement (a joint covariance with variances 4 and covariance -3.96: the block fold reproduces the joint information, the diagonal fold beside the block states the weakly determined direction 1.99 times). One case of the chain election's oracle: the root row pins a new root at an in-block entry (value 12.375 m, sigma 2 m) and leaves the entry's regression and block row as they are, and with no entry it leaves the handover as built at the zero target. The layout, step-table and kind tests are extended. Debug `gnss_tests`: 4 186 run, 4 149 passed, 37 skipped (fixtures absent), 0 failed.

### 7.2 Declared adaptations

1. **Two builds.** The first candidate (`gnss_processor_irdg_c1.exe`, sha256[:16] `764b8e7cce4d1a36`, commit `4500e53`) ran the key arm on r3c and the golden sim. The final exe (`gnss_processor_irdg_c2.exe`, `8694fdac9b915e23`, commit `88f643f`) adds a pure function for the block-column decision, its tests and comments, and ran everything else. The key arm is identical between the two in products, every trace, the manifest and the whole logs, on r3c and on the golden sim (`_rap_irdg_b_{r3c,gs}` on the first against `_rap_irdg_bx_{r3c,gs}` on the final).
2. **The references ran on the base exe** (the link, no-link and no-chain arms), which bar G1 shows equal to the final exe with the key absent.
3. **Bar G1's manifest clause cannot hold as written:** the run manifest embeds the run-start tables, so it differs in the same rows the bar allows the logs to differ in. It is reported as FAILED and the values are listed.
4. **A block rule beyond the pre-registered list:** a carried entry folds only when its harvest-block row is shared by another pending column of the station (or no other column is pending). An entry alone in its block would fold with every cross term zeroed, a diagonal prior. Counted (`left out for want of a shared block row`); 0 on every arm.
5. **`/bigobj`** for `tests/test_code_datum_family.cpp`, which the new header tipped over the Debug section limit.

### 7.3 The bars

The key arm is A1krhRsm plus the key; "link" is the floor-off arm as built, "no link" the same with q 0, "no chain" arm R1; 2 h, r3c and the golden sim unless a bed is named.

| bar | result | numbers |
| --- | --- | --- |
| G1 off identity | products and traces PASS; the manifest clause FAILED as written | Base exe against the final exe with the key absent, on the field golden, the sim golden and the floor-off arm on r3c: `products.txt` and every trace CSV identical (37, 37 and 43 files). The manifest differs in 6 values of 4 records, all table rows: the parameter model's source text for the receiver phase bias, the new contribution kind (two records), the phase-colour block kind's parameter set (two records) and the new response site. The logs differ in 5 to 7 lines: those rows, the response table's site count (70 to 71) and, on the chain arm, the gap-skip site's source text |
| G2 formal sigma | PASS | r3c, at 900, 1 800, 3 600, 7 170 s: key 5.582, 3.899, 2.890, 1.987 m; no link 5.615, 3.947, 2.991, 2.142 (ratios 0.99, 0.99, 0.97, 0.93); link 0.021, 0.018, 0.018, 0.018 (the key reads 110 times the link at 7 170 s). Golden sim: key 5.143, 3.648, 2.524, 1.959; no link 5.202, 3.683, 2.653, 2.050 (0.99, 0.99, 0.95, 0.96); link 0.021, 0.019, 0.019, 0.019 (103 times) |
| G3 DCB against truth | PASS | key 0.538 m on both beds (the estimates' own spread 0.000 m: the prior alone); no chain 0.540 and 0.547 (bars 0.594 and 0.602; the ruling's clause 1.62 and 1.64); link 0.823 and 0.618; no link 0.538 and 0.538 |
| G4 wrong would-be fixes | PASS | key 0 of 30 456 kept on r3c and 0 of 101 528 on the golden sim (ceiling 54.3 and 53.3 %); link 228 of 28 164 and 124 of 103 088; no link 0 of 30 386 and 0 of 101 340; no chain 0 of 56 626 and 0 of 140 418 (58.4 and 58.8 %) |
| G5 held-out consumer | r3c PASS; golden sim **FAILED in hour 1** | Median of the nine sites' p50, hours 1 and 2. r3c: key wire 0.227, 0.054 m against no chain 0.227, 0.054 (-0.0 %, -0.6 %; no site worse by 1 mm). Golden sim: key wire 0.146, 0.033 m against no chain 0.118, 0.034 (+24.2 %, -3.1 %); the link wire reads 0.128, 0.033 (+8.6 %, -3.1 %). In hour 1 five of nine sites read worse with the key than with no chain, three of them by 25 to 43 mm (S18 0.106 to 0.146, S39 0.205 to 0.230, S53 0.074 to 0.117 m), the other six within 14 mm; the paired median change is +8 mm |
| G6 root handover | PASS | `ird_rg`: both bands `outcome=pinned`, at -6.867 and -13.104 m (carried sigma 1.29 and 2.12 m); `ird_rg_mic`: -9.438 and -16.620 m (0.048 m); run end 2 pinned, 0 without a carry, 28 satellite edges kept; 0 undeclared integer steps at the handover on both beds. The no-link arm on `ird_rg` reads `outcome=no_carry` on both bands; the link arm pins at -9.643 and -17.368 m (0.016 and 0.019 m). The oracle case of section 7.1 covers the entry |
| G7 I-b with the key | no-regression clause **FAILED** on 17 of 24 cells; against I-b's bars one cell passes | section 7.4 |
| G8 the responses act | PASS | Block columns folded on all 20 arms (27 062 to 54 796 column-epochs, 0 not folded, 0 left out, 0 stashed without a block row, 0 harvest failures); `rpb_eq34_erase` 204 on r3c (428 on the golden sim, 214 on `ird_rv16`); `rpb_gap_prune` 2 on `ird_rv16`, 0 gap skips (the entry is pruned inside the gap); `rpb_step_erase` 0 on every arm (no false alarm), its rule unit-tested |
| G9 rank and emission | PASS | every epoch emitted on all 20 arms, 0 station blocks dropped, 0 block columns refused, 0 factorization fallbacks |
| G10 in-loop consumer | PASS | SZZ000SIM by hour: r3c 0.281, 0.031 m (link 0.277, 0.031); golden sim 0.090, 0.032 (link 0.093, 0.033) |
| G11 checks | PASS | Debug suite 0 failed (section 7.1); the epoch lint, the entity verify, the docs check, both ledger verifies and the arm selftests pass on the final tree |

### 7.4 I-b on the event rows with the key (bar G7)

Per cell, the floor-off arm of section 6.2 against the key arm: undeclared integer steps, and pairs over the fractional bar on the bar's channel (maximum).

| row | bed | integer: link, key | pairs over the bar: link, key | I-b with the key | key arm less its control (bar channel) |
| --- | --- | --- | --- | --- | --- |
| kept | r3c | 5 836, 7 132 | 4 828, 5 650 | FAILED | |
| kept | microscope | 1 091, 1 611 | 6 190, 5 240 | FAILED | |
| returning | `ird_rv16` | 204, 356 | 209, 307 (WL 0.449) | FAILED | at the station's return 0.092, 0 integer |
| rehost | r3c | 4 971, 5 475 | 4 104, 4 711 | FAILED | |
| rehost | microscope | 1 609, 1 520 | 5 435, 4 728 | FAILED (no higher) | |
| root | `ird_rg` | 0, 0 | 12, 12 (WL 0.072, 0.086) | FAILED (no higher) | 0.007, 0 integer |
| root | `ird_rg_mic` | 0, 0 | 2, 3 (E1 0.021, 0.026) | FAILED | 0.016, 0 integer |
| slip | `ird_tr` | 0, 0 | 0, 0 (WL 0.026, 0.035) | **PASS** (no higher) | 0.004, 0 integer |
| slip | `ird_t1` | 56, 48 | 74, 32 (WL 0.213, 0.115) | FAILED (no higher) | 0.007, 0 integer |
| slip | `ird_t1m` | 0, 0 | 53, 95 (E1 0.038, 0.143) | FAILED | 0.090, 0 integer |
| slip | `ird_trm` | 0, 0 | 47, 5 (E1 0.037, 0.023) | FAILED (no higher) | 0.015, 0 integer |
| merge_split | `ird_msr`, split | 0, 27 | 0, 211 (WL 0.048, 0.162) | **FAILED, passed with the link** | 0.013, 0 integer |
| merge_split | `ird_ms`, split | 33, 262 | 129, 103 (WL 0.189, 0.154) | FAILED | 0.035, 0 integer |
| merge_split | `ird_ms_mic`, split | 0, 0 | 130, 202 (E1 0.053, 0.190) | FAILED | 0.022, 0 integer |
| merge_split | `ird_msr_mic`, split | 0, 0 | 261, 257 (E1 0.120, 0.086) | FAILED (no higher) | 0.006, 0 integer |
| merge_split | `ird_ms`, merge | 0, 4 | 26, 48 (WL 0.080, 0.126) | FAILED | 0.000, 0 integer |
| merge_split | `ird_msr`, merge | 131, 398 | 202, 233 (WL 0.485, 0.464) | FAILED | 0.686, 8 integer |
| merge_split | `ird_ms_mic`, merge | 0, 0 | 49, 59 (E1 0.134, 0.129) | FAILED | 0.003, 0 integer |
| merge_split | `ird_msr_mic`, merge | 0, 120 | 231, 238 (E1 0.367, 0.499) | FAILED | 1.989, 167 integer |

The two beds of card IRDF add five cells: r3c's returning (189 to 311 integer, 127 to 182 pairs) and the golden sim's kept (8 246 to 8 936 and 7 925 to 9 417), rehost (8 150 to 8 998 and 7 526 to 9 196), returning (148 to 174 and 173 to 253) and slip (159 to 58 and 131 to 117, no higher). Of the 24 cells 7 are no higher: the microscope's rehost, the root on `ird_rg`, the slips on `ird_tr`, `ird_t1`, `ird_trm` and the golden sim, and the split on `ird_msr_mic`. The other 17 read higher on one count or both.

**Attribution.** The rise is not the block route's own: the key arm reads as the no-link arm, class by class (r3c non-event wide lane p50 0.0159 against 0.0160 cycle, 19 536 against 19 650 integer steps; golden sim 0.0155 against 0.0155, 9 917 against 9 923; kept and rehost within 2 % on both beds). What moved is the link arm's level: with the bias held at a formal 0.018 m the pre-hold wire steps less from epoch to epoch (non-event wide lane p50 0.0121 on r3c and 0.0108 on the golden sim) than a solve with the bias at its honest 2 m. The no-chain wire itself reads 0.0091 on r3c with 12 337 pairs over the bar. Against its own control the key arm steps as the link arm does: 0 integer pairs on every event but the merge of `ird_msr` and its microscope twin, the event section 6.2 names.

### 7.5 Readings without a bar

| reading | link | key | no link | no chain |
| --- | --- | --- | --- | --- |
| wet delay error after hour 1, r3c: p50, p90, p99, max, m | 0.022, 0.179, 0.736, 4.21 | 0.015, 0.068, 0.439, 3.74 | 0.015, 0.068, 0.440, 3.74 | 0.008, 0.022, 0.041, 0.07 |
| the same, golden sim | 0.009, 0.023, 0.056, 0.09 | 0.008, 0.020, 0.037, 0.06 | 0.008, 0.020, 0.037, 0.06 | 0.006, 0.016, 0.028, 0.06 |
| reference-free DD error by hour (L1C, cycle), r3c | 0.314, 0.130 | 0.319, 0.145 | 0.321, 0.144 | 0.208, 0.092 |
| the same, golden sim | 0.353, 0.176 | 0.346, 0.188 | 0.347, 0.192 | 0.244, 0.114 |
| stop rule, event-free chords p50 L1C, L2W: r3c | 0.250, 0.258 | 0.192, 0.220 | 0.192, 0.220 | |
| the same, golden sim | 0.244, 0.259 | 0.217, 0.240 | 0.217, 0.240 | |
| the same, microscope (the rule's bed, bar 0.020) | 0.0213, 0.0167: FAIL | 0.0186, 0.0143: **PASS** | | |
| microscope: search wrong of kept; consumer h1, h2, m | 20 of 177 877; 0.124, 0.045 | 2 of 178 841; 0.123, 0.044 | | |
| would-be certification, microscope: wrong of determined; wrong certified re-hosts | 863 of 104 735; 2 | 91 of 103 560; 0 | | |
| the same, r3c and r2 | 0 of 3 370; 0 of 3 406 | 0 of 3 116; 0 of 2 828 | | |
| RPB time-link NEES (terms) | 0.556 (27 290) r3c; 0.590 (54 796) golden sim | 3.424 (265); 0.013 (462) | no term | |

The certifier reads SOUND on every arm. The NEES statistic is defined where the carried variance exceeds the posterior's, which the block's conditional variance does on about 1 % of the rows, so its key-arm values are not comparable and IRD-5b's gate is not read from them. The loop time was not measured: the machine was shared by four other sessions and the same arm's time per epoch varied by a factor of two between runs. All 110 runs of the card exit 0.

### 7.6 Verdict

1. **The in-block carry removes the double count.** G2, G3 and G4 pass on both beds: the bias's formal sigma follows the block (0.93 to 0.99 of the no-link arm, 100 times the link's), the DCB error is the prior's own (0.538 m) and the search makes no wrong would-be fix. In every reading the key arm is the no-link arm to the third digit, with the one thing the no-link arm lacks: the carried entry, so a root handover is pinned at its carried value (G6) where the no-link arm pins at zero.
2. **Two clauses FAILED.** G5: at the held-out sites the key wire costs the float consumer 24 % in hour 1 on the golden sim (0.146 against 0.118 m with no chain; the link wire 0.128), with r3c and both second hours equal to the no-chain wire. G7: with the key the pre-hold wire steps more between epochs than with the link, so the event rows' counts rise on 17 of 24 cells and the split on `ird_msr`, which passed I-b with the link, fails it (wide lane 0.162 on 211 pairs, none against its control). Both are the price of the bias at its honest variance, shared with the no-link arm; neither is a step of the block route's own.
3. **What it buys beyond the bars.** On the microscope the stop rule's event-free chords pass (0.0186 and 0.0143 cycle), the search's wrong would-be fixes fall from 20 to 2 and the wrong would-be certifications from 863 to 91.
4. **The hold stands with the key too:** 91 wrong would-be certifications on the microscope are not 0.

The key lands default off, ledger UNVALIDATED. The arm definitions do not adopt it: whether the arm going forward takes the key (the double count removed, a root handover that keeps its target, the hour-1 cost at held-out sites on the golden sim) is the author's ruling.

### 7.7 What the chain still owes before I-b passes on every row

1. **The float's own level.** At quiet epochs the pre-hold wire moves by more than the fractional bar on a fifth of the pairs on the chain arms (19 % with the link and 22 % with the key on r3c), and on a tenth on the no-chain wire (12 337 of 113 869). The kept, rehost and returning rows are graded over tens of event epochs and fail at that level with the floor off, with or without the key. Either the wire that is graded changes (the emitted, held wire of IRD-7) or the floats settle (integers fixed in the frame).
2. **The certified branch.** The hold stands (863 wrong would-be certifications on the microscope with the link, 91 with the key), so every re-host is a cut release: a declared integer step and a released subtree, which is what the event epochs of the rehost row, the slip row and the merge grade.
3. **The merge of `ird_msr`.** The one event that steps by integers against its control (8 wide-lane pairs; 167 on E1 on the microscope twin with the key), recorded in card IRDE section 8.5 and not traced.
4. **The slip on `ird_t1` and the splits on the microscope** fail the fractional bar at the quiet epochs' level and step by nothing against their controls: they wait on item 1.

### 7.8 Standing and owed

Open for the author: adoption of the key on the arm going forward (section 7.6). Not covered: two sim beds for the bars and 2 h arms; one fixture per event class; the held-out replay of the no-link wire was not run; the step rule was exercised by its unit test alone (no arm trips it); the shared-history map and the warm resume with the key are covered by no arm (the entry is cold-started); the loop time. Owed, outside this card: any field chain arm (card IRD section 6, after IRD-6); the replacement of the emission freeze; IRD-5 and IRD-5b's gate against arm A2; IRD-7's truthful `signal_integer` declaration; the trace of the `ird_msr` merge.
