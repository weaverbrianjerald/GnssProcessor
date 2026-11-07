# SCR: a satellite clock step routed to the clock, and a tripped clock declared to the consumer

**STATUS**

- **id:** SCR
- **topic:** clock
- **status:** open
- **summary:** BUILT and MEASURED 2026-10-26, both knobs default off and UNVALIDATED. Builds SCK decisions 1 and 3 (the author's choice of 2026-10-26, decision 3 as a generation edge). (1) A satellite the IF-TD pool flags at most of its stations with one common step is a clock event: the consistent flags keep their arcs, and the satellite's clock folds LOOSE in that epoch with a declared decoupling (`slip_clk`). (2) A clock the guard's test trips opens a phase-bias hold generation. Row `sat_clk` with both: every producer channel passes on `g`, `c` and `n` (wire clock 0.5, 0.2, 0.2 mm against 28, 35, 83 mm); the consumer channels still fail (peak 153, 170, 328 mm against 147, 2354, 1186 mm) on a leg consumer 0.9 to 1.1 m from convergence. Knobs-off matrix IDENTICAL to `smx1`; with both armed 22 of 55 cells pass, as off, no verdict changes. Two findings: the guard's same-epoch withdraw-and-re-solve moved G25's clock 12.19 m on `g` when its arcs were carried (the first route, section 5.1); decision 3 on every statistical trip costs the storm consumer on `g` and `n` (section 5.4). One author decision open (section 6).
- **source:** card [SCK](SCK_sat_clock_step_2026-10-25.md) section 10, decisions 1 and 3; decision 4 (the leg consumer resumed 0.7 to 1.2 m off) is the work of a separate session; decision 2 (the guard's phase statistic lost to a same-epoch cold re-realization) is not built here (section 2.3)
- **knobs / posture:** new, all default off: `network.cycle_slip.if_phase_td_sat_clock_event` with `if_phase_td_sat_clock_min_stations` 4, `if_phase_td_sat_clock_min_fraction` 0.75, `if_phase_td_sat_clock_coherence` 0.25; `network.emit_pb_hold_trip_generation`. Postures `g`, `c`, `n` as card SMX. No golden, manifest or loader default changed.
- **dates in the text:** 2026-10-26
- **hashes / run dirs:** source branch `claude/sck-sat-clock-step` on base `7bdcebc`. Exes in `build/x64-Release/` of that worktree: `gnss_processor_scr1.exe` (sha256[:16] `306d0aca842dcccc`, the first route: a forced trip re-solved in the guard) and `gnss_processor_scr2.exe` (`f9fb40389cd560b7`, the fold-side route; every result of section 5.2 on). Labels under `D:/GnssProcessorSupport/gates/smx/`: `sckd1`, `sckd3` (scr1, row `sat_clk`, one knob each); `sckb1`, `sckb` (scr2, row `sat_clk`, decision 1 alone and both); `sck0b`, `sck1b` (scr2, the full matrix, knobs off and both armed; `sck1b/report_vs_sck0b.md`); `sck0`, `sck1` (scr1, superseded, not run to the end); `sckd1r` (refused at resume: `clock_anchor_resolve` is fingerprinted). Legs `D:/GnssProcessorSupport/runs_nom/evleg_smx_<label>_<row>_<posture>`.
- **gates:** (1) default-off identity: the SMX version-1 matrix on the new exe with both knobs off reads IDENTICAL to label `smx1` in every measured value; (2) inertness: every null leg with both knobs on writes a `products.txt` byte-identical to its knobs-off twin; (3) the behaviour bars of section 3, written before any leg ran.

**Outcome.** Question: do a satellite clock event in the slip layer and a hold generation on a tripped clock make row `sat_clk` pass on all three postures without moving another cell? Decisive result: with both, the wire carries the step to 0.5 mm or better on `g`, `c` and `n`, and zwd, iono and phase bias pass; the consumer channels fail on all three, on a leg consumer already about 1 m off in its null. No verdict of the 55 cells changes; the knobs-off matrix is identical to version 1. Verdict: producer bars met, consumer bars missed. Standing: section 6; nothing armed in a golden.

## 1. The question

Card SCK attributed the failure of row `sat_clk` (+0.5 m on every code and phase of G25 at every station, TOW 275910) to three separable mechanisms. Common to every posture: the IF-TD slip layer reads the step as a slip at all 16 stations that track G25 and refloats every G25 arc, and the phase datum re-realizes cold within one epoch, so the clock level falls to the code. On the golden the anchor then re-couples on a code level 0.21 m off. On the native posture the guard loses its phase statistic in the step epoch and never decouples. On every posture the phase-bias hold keeps G25 in its first generation, so the wire's phase-visible clock never carries the step and the consumer's hold reaction refloats nothing. This card asks whether routing the coherent satellite-wide flag set to the clock, with the arcs kept, and declaring the tripped clock as a hold generation, make the row pass on all three postures without moving any other cell of the matrix.

## 2. Design

### 2.1 The satellite clock event (decision 1)

The IF-TD pool (`networkSlipIfTdDriver`, `src/core/network/slip/SlipQc.cpp`) tests, per signal pair kind, each station's between-satellite, between-epoch IF phase difference `dd` against the network median `med` and a per-sample threshold `thr`. A satellite clock step `s` enters `dd` of that satellite at every station where it is not the chain's reference, with the same magnitude everywhere, because the computed value subtracts an a priori clock that does not contain the step. A cycle slip is station-local and its IF magnitude depends on the integers on each band.

Rule, per (pair kind, satellite), armed by `if_phase_td_sat_clock_event`:

- `n_obs` = the satellite's valid non-reference samples (one per station); `F` = the flagged subset; `m` = median of `r = dd - med` over `F`; `d` = 1.4826 MAD of `r` over `F`.
- The satellite is a clock event when `|F| >= min_stations`, `|F| >= ceil(min_fraction * n_obs)` and `d <= coherence * |m|`.
- A flagged sample of an event satellite with `|r - m| <= thr` (it would not flag once the common step is removed) keeps its arc: no detector chain reset, no refloat, no bridge pend, and it stays out of the station-pair scale EWMA, as a flagged sample does. A flagged sample with `|r - m| > thr` is a slip at that station as well and takes the existing path.
- The reference gate's reset of an event satellite at a station where it is the chain's reference keeps its arcs too (no refloat); the detector's own chain reset there is unchanged.
- The event satellites and their common step `m` are an epoch event (`EpochEvents::Slip`), read by the clock anchor guard: an anchored event satellite is tripped on the first pass with reason `slip_clk` (the existing decoupling edge: event row, wire mark, same-epoch withdrawal and re-solve under the redundancy screen); a LOOSE one has its re-coupling count reset.

The rule only removes flags; it never adds one. With the knob off no classification runs and every branch is the pre-card one.

### 2.2 The tripped clock as a hold generation (decision 3)

The guard records, per epoch, the satellites its test tripped (statistic, absolute backstop, or `slip_clk`), apart from the decouplings that retire a frame (gap, datum, external). With `network.emit_pb_hold_trip_generation` armed, the hold (`networkPbHold`, `src/core/network/emit/PbHold.cpp`) starts a new generation on every phase-bias key of such a satellite at that epoch, the same edge a gap opens: the held value becomes the estimate, the phase-visible reference is re-taken, and the consumer refloats the satellite's ambiguities on the stamped generation (`RealTimeEngine::reactToPbHold_`). Frame retirements stay out of the trigger: a datum decoupling acts on every satellite of a constellation and its common move stays on the clock through the hold's de-commoning.

### 2.3 Not built, and known limits

- Decision 2. With decision 1 armed, row `sat_clk` no longer reaches the path (the anchor station's arcs are kept, so no family re-realizes in the step epoch); a cold re-realization coincident with a clock step remains uncovered.
- A phase-only satellite event with the same signature (every station, one magnitude, code unmoved) is routed to the clock as well; the code disagreement is not tested by the rule.
- The reference gate's reset of an event satellite is kept without a magnitude test (the reset key carries none).
- With the clock anchor off the guard does not run, so no trip is recorded and decision 3 has no trigger; every posture in scope arms the anchor.
- A step small enough that fewer than `min_fraction` of the stations flag is left to the existing path.

## 3. Bars and predictions (written 2026-10-26, before any leg ran)

Bars: the frozen SMX version-1 bars (`scripts/smx/matrix.json`), unchanged.

- **B1 (the row).** Row `sat_clk` passes on `g`, `c` and `n` in every channel (clk, pb, cons_peak, cons_plat, zwd, iono, emit) with both knobs armed. The consumer channels are read on the leg consumer of card SCK section 9, which is 0.7 to 1.2 m from convergence; a pass or fail there is reported with that caveat, and the producer channels (clk, pb, zwd, iono, emit) are the primary bar.
- **B2 (no collateral).** With both knobs armed, no other cell of the matrix changes verdict from pass to fail against the knobs-off matrix of the same exe.
- **B3 (identity and inertness).** Gates (1) and (2) of the STATUS block.

Predictions per arm (row `sat_clk`, postures `g`, `c`, `n`):

- **P1, decision 1 alone.** Every G25 arc is kept at the 15 non-reference stations and at the reference station; the guard trips G25 at +0 with reason `slip_clk` on all three postures; the wire carries -0.500 m at +0 and stays within the clk bar from +1 (no overshoot; no cold re-realization at +0 or +1); the zwd and iono channels pass. The consumer still takes the step through the hold: on `c` and `n` it accepts a phase innovation of about 0.5 m (a jump of the order of 1 m); on `g` it rejects the phase row and drifts less than in card SMX, since the code-side overshoot is gone.
- **P2, decision 3 alone.** On `g` and `c` the statistical trip at +0 opens generation 2 of every G25 key and the consumer refloats G25 at +0, so the `c` consumer's 1.5 m jump disappears; the wire still loses the level at +1 (the clk channel still fails on `g` and `c`). On `n` no trip occurs, so nothing changes.
- **P3, both.** B1 holds on the producer channels on all three postures, and the consumer channels improve on all three against card SMX version 1.

## 4. Method

Release exe built from this branch (base `7bdcebc`). Labels under `D:/GnssProcessorSupport/gates/smx/`: `sck0` (both knobs off, full matrix, compared with `smx1`); `sck1` (both armed, full matrix, compared with `sck0`); `sckd1` and `sckd3` (one knob each, row `sat_clk` on `g`, `c`, `n`). The knobs reach every leg of a label through `scenario_matrix.py make --extra`. Both knobs act only on an event, so a leg resumed from a seed written without them is valid once gate (2) holds.

## 5. Results (2026-10-26)

Every number is event minus null on one exe and one snapshot, graded by `scenario_matrix.py grade` against the frozen version-1 bars; "+k" is the k-th 30 s epoch after the step at TOW 275910.

### 5.1 The first route, and a defect of the guard's same-epoch re-solve

Exe `scr1` implemented section 2.1 as pre-registered: the guard tripped the classified satellite on its first pass and re-solved the epoch with its clock prior withdrawn (`refoldSharedPriorDenseDroppingClocks` on `g`; the clock-clone refloat on `c` and `n`). Decision 1 alone (label `sckd1`):

| posture | clk | pb | zwd | iono | cons_peak |
|---|---|---|---|---|---|
| `g` | 1548.6 / 5.0 X | 1614.8 / 192.7 X | 1485.4 / 14.7 X | 9243.7 / 234.5 X | 151.9 / 21.1 X |
| `c` | 0.2 / 5.0 | 90.4 / 257.9 | 1.1 / 18.6 | 1.0 / 230.2 | 2395.3 / 61.4 X |
| `n` | 0.1 / 37.1 | 88.3 / 300.7 | 0.8 / 19.0 | 0.7 / 222.4 | 347.6 / 29.4 X |

(channel value / bar, mm; X a failed bar)

On `g` the classification and the trip ran as designed (15 of 15 flags kept, the reference reset kept, `cta_decouple reason=slip_clk`). The anchored first solve moved G25 by 6.1 cm (guard row `e` -0.0607 m); the re-solve then emitted G25 at -13.617 m against -1.425 m in the null, a move of -12.19 m, and the hold moved its phase biases the same. In the same epoch the leveling carry of G18 L1C at S24000SIM, the G25 L2W phase-datum anchor station, moved by -14.98 m on a carried variance of 3.5e-4 m2 (prior-residual |t| 9683), and G25's post-fit station statistics read 5.5 to 15.6 at the 15 stations of the residual trace. The emitted code biases did not move. In card SMX's leg the same re-solve on `g` emitted -0.500 m exactly, with every G25 arc refloated. The direct check (the same leg with `network.clock_anchor_resolve` false) cannot run on a resumed leg: that key is in the snapshot fingerprint. Section 5.2 gives the comparison that isolates the route.

### 5.2 The fold-side route

Exe `scr2` routes the event before the solve. The decision of `carryFoldDecide` (after PASS A, before PASS B) folds the classified satellite's clock LOOSE when it is anchored, the commit records `slip_clk_pending`, and the guard emits the decoupling edge (`reason=slip_clk`, wire mark, `stepped`) before its test, with nothing withdrawn. The in-test forced trip remains as a backstop (`slip_clk_late`); it fired on no leg. With the same kept arcs, `g` now carries the step (decision 1 alone, `sckb1`: clk 0.5 mm, zwd 12.1 mm, iono 17.8 mm, all within their bars). The two `g` legs differ only in whether the clock prior was folded LOOSE, or folded anchored and then withdrawn. So the 12.19 m of section 5.1 is a property of the guard's same-epoch withdraw-and-re-solve on `g` when the tripped satellite's arcs are carried. A statistical trip of a satellite whose arcs are carried (a clock step below the IF-TD thresholds, or one the rule does not classify) takes that path on the golden; card SMX has no row that drives it.

The response is identical on the three postures (labels `sckb1` and `sckb`): one `sat_clock_event` for G25 at +0 (step 0.496 m on `g` and `c`, 0.495 m on `n`; dispersion 5 to 6 mm; 15 of 15 non-reference flags), 15 `sat_clock_keep` and one `refsat_clock_keep` (S40000SIM), one `slip_clk` decoupling, no G25 refloat, no G25 handover or cold re-realization at +0 or +1, and the re-coupling at +4 (TOW 276030).

Row `sat_clk`, every channel (value, mm; X a failed bar):

| arm | posture | clk | pb | cons_peak | cons_plat | zwd | iono | emit |
|---|---|---|---|---|---|---|---|---|
| SMX v1 (`smx1`) | `g` | 28.4 X | 94.6 | 147.1 X | 105.1 X | 57.4 X | 658.5 X | 0 |
| | `c` | 35.1 X | 122.5 | 2354.4 X | 692.4 X | 121.1 X | 642.4 X | 0 |
| | `n` | 83.0 X | 16.3 | 1186.5 X | 1133.0 X | 138.8 X | 814.9 X | 0 |
| decision 1 (`sckb1`) | `g` | 0.5 | 67.5 | 146.9 X | 104.9 X | 12.1 | 17.8 | 0 |
| | `c` | 0.2 | 90.4 | 2395.2 X | 690.8 X | 1.1 | 1.0 | 0 |
| | `n` | 0.2 | 88.3 | 347.6 X | 263.8 X | 0.8 | 0.9 | 0 |
| decision 3 (`sckd3`) | `g` | 28.4 X | 2.2 | 153.6 X | 111.9 X | 57.4 X | 658.5 X | 0 |
| | `c` | 35.1 X | 2.0 | 152.1 X | 89.3 X | 121.1 X | 642.4 X | 0 |
| | `n` | 83.0 X | 22.4 | 1186.5 X | 1133.0 X | 138.8 X | 814.9 X | 0 |
| both (`sckb`) | `g` | 0.5 | 0.3 | 153.0 X | 111.3 X | 12.1 | 17.8 | 0 |
| | `c` | 0.2 | 0.3 | 169.6 X | 91.0 X | 1.1 | 1.0 | 0 |
| | `n` | 0.2 | 0.1 | 328.0 X | 249.3 X | 0.8 | 0.9 | 0 |

Bars on `g` / `c` / `n`: clk 5.0 / 5.0 / 37.1, pb 192.7 / 257.9 / 300.7, cons 21.1 / 61.4 / 29.4, zwd 14.7 / 18.6 / 19.0, iono 234.5 / 230.2 / 222.4. The decision-3 rows are exe `scr1`; with decision 1 off the two exes run the same code. Decision 3 opened generation 2 on both G25 keys at +0 wherever the guard tripped (`g` and `c` on the statistic alone; all three with decision 1); the `pb` channel then excludes those keys.

### 5.3 The consumer

Consumer position difference (mm) at +0, +5, +20 and +49:

| arm | `g` | `c` | `n` |
|---|---|---|---|
| SMX v1 | 6, 11, 36, 142 | 1504, 2354, 1352, 559 | 1042, 1027, 1020, 1176 |
| decision 1 | 6, 11, 36, 141 | 1505, 2395, 1366, 555 | 30, 70, 135, 345 |
| decision 3 | 7, 18, 45, 148 | 14, 62, 147, 66 | 1042, 1027, 1020, 1176 |
| both | 7, 18, 45, 148 | 15, 112, 167, 66 | 64, 90, 132, 319 |

The null consumer's 3D error over the same window is 1.07 to 1.14 m on `g`, 0.89 to 1.16 m on `c` and 0.89 to 1.00 m on `n` (card SCK section 9).

- On `g` the drift is the same in every arm (141 to 148 mm at +49), including decision 1 alone, where the wire carries the step to 0.5 mm. The drift is the consumer's own handling of G25, not the wire's clock error. This refutes card SCK's attribution of the `g` consumer's 0.14 m to the code-side overshoot (SCK sections 4 and 7).
- With decision 1 alone the hold moved G25's phase biases with its clock on `c` and on `n` (-0.50 m on both), so on neither did the phase-visible clock carry the step. The `c` consumer accepted the 0.5 m phase innovation (1.505 m at +0); the `n` consumer took 30 mm. Its G25 reaction is not read here.
- With both knobs the step-epoch jump is gone on `c` (1504 to 15 mm) and `n` (1042 to 64 mm). What remains grows over the leg after the consumer refloats G25, to 0.17 m on `c` and 0.32 m on `n`, the same shape as on `g`. Whether a converged consumer meets the bars is not measured; the leg consumer is the open item of card SCK section 10, decision 4.

### 5.4 Gates and the rest of the matrix

- **Gate 1 (default-off identity).** `sck0b` (scr2, both knobs off, 65 legs) against `smx1`: IDENTICAL, every measured value and spread of the 55 cells equal.
- **Gate 2 (inertness).** Every null leg with knobs on writes a `products.txt` byte-identical to its version-1 twin (labels `sckd1`, `sckd3`, `sckb1`, `sckb`, on `g`, `c` and `n`), except the `n` null with decision 3. There the guard trips G05 at TOW 276900 on its phase statistic (6.75 against k 6) with no injected event, decision 3 opens generation 2 of G05's two keys, and from that epoch those two P records differ; the consumer's position is unchanged to the millimetre. The trip exists with the knob off; the generation is the knob's designed response.
- **B2 (no collateral).** `sck1b` (both armed) against `sck0b`: 22 of 55 cells pass in both, and no cell changes verdict. The channels that move are `sat_clk` (sections 5.2 and 5.3), the `pb` channel of every `n` row (the `n` null's G05 generation), and the storm row on all three postures.
- **The storm.** In 240 epochs of campaign-rate slips the rule classified nothing (0 `sat_clock_event`), and the guard's trips are the same with the knobs off and on (30 phase-statistic trips and 30 datum decouplings on `g`, 75 phase-statistic trips on `c` and on `n`). Decision 3 opened a generation at every statistical trip (62, 150 and 150 key generations; 32, 50 and 60 keys regenerated in the graded window). Consumer peak, knobs off to on: 80.8 to 182.3 mm on `g`, 923.1 to 610.7 mm on `c`, 294.3 to 2508.4 mm on `n`. The cell fails either way, but a generation on every statistical trip costs the consumer on two postures of three. A statistical trip says that the anchor's prior failed, which a storm's slips produce with no satellite clock event; the classified event says that the satellite's clock moved at most of its stations.

### 5.5 Predictions against results

- **P1 (decision 1 alone).** Kept arcs at all 16 stations, the trip at +0 on all three postures, no cold re-realization, zwd and iono within their bars: held on exe `scr2`; on `scr1` the `g` wire failed (section 5.1). The `c` consumer jump of the order of 1 m: held (1.505 m). The `n` consumer like `c`: refuted (30 mm). The `g` consumer drifting less than in card SMX: refuted (141 against 142 mm at +49).
- **P2 (decision 3 alone).** A generation at +0 on `g` and `c`, and the `c` jump gone: held (14 mm at +0, peak 152 mm). The wire still failing clk on `g` and `c`: held. `n` unchanged: held (identical).
- **P3 (both).** Producer channels passing on all three postures: held. Consumer channels improving on all three: held on `c` (2354 to 170 mm) and `n` (1186 to 328 mm), refuted on `g` (147 to 153 mm).
- **B1** met on the producer channels and missed on the consumer channels; **B2** met; **B3** met (gate 2 with the one designed exception above).

## 6. Standing (2026-10-26)

Both knobs stay default off with ledger verdict UNVALIDATED; no golden, manifest or loader default changed. Open:

1. **The trigger of decision 3 (the author's).** As built, any test trip opens a generation (statistic, absolute backstop, `slip_clk`). Restricting it to the classified event (`slip_clk`, `slip_clk_late`) keeps row `sat_clk` as measured here (its trip is `slip_clk` on every posture), and would make both knobs inert outside a classified event (the storm and the `n` null unchanged). It would also make decision 3 depend on decision 1.
2. **The guard's same-epoch withdraw-and-re-solve on `g` with carried arcs** (section 5.1): a defect of an armed golden mechanism, measured on one leg and not attributed inside `refoldSharedPriorDenseDroppingClocks`. It needs a row of its own in the matrix (a satellite clock step below the IF-TD thresholds, whose arcs stay carried) and a diagnosis.
3. **The consumer channels** wait on a converged leg consumer (card SCK decision 4).

## Annotation (2026-10-26): card ARS, on standing item 2

Card [ARS](ARS_anchor_resolve_coupling_2026-10-26.md) takes item 2. At unit level the guard's re-solve departs from a solve on consistent rows by 17 % of the step, linearly, and the same withdrawal on rows re-expressed LOOSE departs by nothing: the exact coupling rows are conditioned on the anchored clock, and the withdrawal comes after them. The unit fixture reproduces the direction and the growth with stations, not the 12.19 m of section 5.1. A default-off deferral (`network.clock_anchor_defer_coupled`) leaves no storm clock on `g` pushed away from truth (0 of 22, against 16 of 31) and changes no matrix verdict; it is inert on the leg of section 5.2 (byte-identical), whose trip is the `slip_clk` fold.

## 7. 2026-11-06: the consumer channels read on a converged leg consumer (card CRS2)

Card [CRS2](CRS2_resume_hold_and_latches_2026-11-06.md) removed the leg consumer's resume defects (card CRS) and re-ran rows `sat_clk` and `storm` with both knobs of this card and card ARS's deferral (label `crs2bscr`). Row `sat_clk` PASSES on `g` (consumer peak 4.6 mm, plateau 1.0 mm against 18.2; section 5.2 read 153 and 111 mm on the metre-off consumer), misses the consumer peak on `c` by 4 mm (30.6 against 26.4, plateau 7.4) with every producer channel passing, and leaves the `n` consumer 15 cm off (157.8 and 152.6 mm against 28.3; 328 and 249 before) while the wire carries the step to 0.2 mm. The storm stays failed on the producer channels on every posture; its `g` consumer is within bar. The open item of section 6 item 3 is closed by that read; items 1 and 2 stand.
