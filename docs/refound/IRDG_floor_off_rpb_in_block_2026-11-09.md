# IRDG: the D0 ruling enacted, and the receiver phase bias carried inside the station block

**STATUS**

- **id:** IRDG
- **topic:** network-vehicle
- **status:** open
- **summary:** Pre-registered 2026-11-09, before any graded run. Part 1 enacts the author's D0 ruling of 2026-11-09 (card IRDF section 9): the receiver phase-bias floor goes off on the arm going forward (`scripts/ird_make_arms.py`, A1 and every arm derived from it, with a named floor-on variant), and the integer test I-b of every IRD-4 event row is read again on that arm, with the release conditions of the hold of card IRDC section 7.3. Part 2 builds the fix of the defect the ruling leaves recorded, the receiver phase bias's time link counting the station block's information twice: the bias as a carried column of the station's block behind a default-off key, with bars frozen from card IRDF's own numbers (sections 3 and 4 here). Results are appended below the marker.
- **source:** [IRDF](IRDF_d0_receiver_floor_redecided_2026-11-07.md) sections 5.1, 7 and 9 (the ruling and the defect); [ARR](ARR_integer_ambiguity_review_2026-11-07.md) section 7 (the order of stage S0); [IRD](IRD_integer_recovering_datum_2026-10-13.md) sections 5, 7, 9 and 11 (the event table, the key's reserved name, bar I-b, the increments); [IRDC](IRDC_integer_recovering_datum_events_2026-11-02.md) section 7.3 (the hold); [IRDD](IRDD_integer_recovering_datum_rows_2026-11-03.md) and [IRDE](IRDE_integer_recovering_datum_slip_2026-11-06.md) (the rows' gates and fixtures)
- **knobs / posture:** `network.phase_datum_eq34_rpb_prior_sigma_m` 0 on arm A1 and its descendants; new key `network.phase_datum_eq34_rpb_in_block` (default off; the name card IRD section 7 reserves); readings set `network.phase_datum_eq34_rpb_q_m2s` 0 (the no-link arm). No golden change, nothing armed
- **dates in the text:** 2026-11-09
- **hashes / run dirs:** branch `ird/floor-off-rpb-block` (from `docs/rulings-2026-11-09` at `58d0b3b`); base exe `D:/GnssProcessorSupport/gates/exe/x64-Release/gnss_processor_irdg_base.exe` (sha256[:16] `0b50425c9118a037`, the AVX2 default build of `58d0b3b`); arms `_rap_irdg_*` under `D:/GnssProcessorSupport/runs_nom/`
- **gates (frozen at pre-registration):** section 3 (Part 1's readings and the hold's conditions) and section 4 (Part 2's bars)

**Outcome.** Question: with the receiver floor off by the author's ruling, which IRD-4 event rows pass their integer test, and does carrying the receiver phase bias inside the station block remove the time link's double count while a root handover keeps its target? Decisive result: not yet measured; this card is pre-registered and its results are appended below. Verdict: none yet. Standing: open. The floor is off in the arm definitions, the bars of section 4 are frozen, the key lands default off, and no golden, ledger verdict or arm adopts it without the author's ruling.

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
