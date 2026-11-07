# ARS: the clock anchor's same-epoch re-solve against the exact coupling rows

**STATUS**

- **id:** ARS
- **topic:** clock
- **status:** closed
- **summary:** CLOSED 2026-10-26: ATTRIBUTED in direction, not in size; the deferral `network.clock_anchor_defer_coupled` built default off, UNVALIDATED, passing its three bars (storm `g`: 0 of 22 tripped clocks pushed away from truth against 16 of 31; no matrix verdict changed; `c` and `n` byte-identical), with P1's trip clause and P2's second clause falsified; arming it is the author's decision. Pre-registration: on the sim golden posture `g`, the guarded clock anchor's same-epoch withdraw-and-re-solve moves a tripped satellite clock by metres when the epoch's coupling rows carry that clock as a border column: 12.19 m on G25 in card SCR's leg with the arcs kept, 2 to 3 m on several satellites in card SMX's storm leg, where 16 of 31 re-solved clocks end farther than 0.3 m from truth and none closer. Hypothesis: the exact coupling update re-expresses every carried local row on the border with the fold's decided inflation (the anchored q·dt for an anchored clock), and the guard then withdraws the clock's prior from the shared block after those rows were built and reduced, so the re-solve combines rows conditioned on an anchored clock with a clock that has no prior. Measurement, attribution, and a candidate response behind a default-off knob.
- **source:** card [SCR](SCR_sat_clock_step_response_2026-10-26.md) section 5.1 and section 6 item 2; the author's direction of 2026-10-26 to take the defect in the session that found it
- **knobs / posture:** `network.clock_anchor_defer_coupled` (built, default off, ledger UNVALIDATED). Postures `g`, `c`, `n` as card SMX; the knobs of card SCR off unless stated.
- **dates in the text:** 2026-10-26
- **hashes / run dirs:** exe `gnss_processor_ars1.exe` sha256 `f5a196a0b9cb4a1f` (labels `ars0`, `ars1`, `arsb`), `gnss_processor_ars2.exe` sha256 `67d1a0da133bdfbe` (label `ars2`, the run-end warning); labels `D:/GnssProcessorSupport/gates/smx/<label>/`, legs `D:/GnssProcessorSupport/runs_nom/evleg_smx_<label>_<row>_<posture>`; unit suite `tests/network/test_clock_anchor_coupled_resolve.cpp`
- **gates:** (1) default-off identity: the SMX version-1 matrix on the new exe reads IDENTICAL to label `smx1`; (2) the bars of section 4, written before any leg with the knob ran.

**Outcome.** Question: is the metre-level clock error of the guard's same-epoch re-solve on `g` the exact coupling rows' conditioning on the anchored clock, and does deferring the correction remove it? At unit level the guard's path departs from consistent LOOSE rows by 17 % of the step, linearly, and the same withdrawal on LOOSE rows by nothing; the leg's 24-fold size is not reproduced (1.4-fold at 48 stations). The deferral passes B1, B2 and B3; P1's trip clause and P2's second clause failed. Standing: the defect stays on the golden, the knob off and UNVALIDATED (section 6).

## 1. The question

Is the metre-level clock error that the guard's same-epoch re-solve produces on `g` a consequence of the exact coupling rows being conditioned on the anchored clock, and does deferring the value correction to the next epoch (when the fold decides the clock LOOSE and the rows are re-expressed for it) remove it without a cost elsewhere in the matrix?

## 2. Evidence before this card (knobs off unless stated)

- **Card SCR's leg** `evleg_smx_sckd1_sat_clk_g` (exe `scr1`, the satellite clock event routed through the guard's re-solve, every G25 arc kept): the anchored first solve moved G25 by 6.1 cm; the re-solve through `refoldSharedPriorDenseDroppingClocks` moved it by -12.19 m, the leveling carry of G18 L1C at S24000SIM by -14.98 m (prior-residual |t| 9683), and left G25's post-fit station statistics at 5.5 to 15.6. The same arcs with the clock folded LOOSE before the solve (exe `scr2`) carry the step to 0.5 mm.
- **Card SMX's storm leg** (label `sck0b`, knobs off, identical to `smx1`): every same-epoch re-solve, classified by the de-commoned clock against the null (the quiet fixture's truth clock is the a priori, which the null holds to about 2 cm) before the trip epoch and at it:

| posture | withdrawal path | re-solved clocks | pushed away > 0.3 m | brought closer > 0.3 m | largest at the trip epoch |
|---|---|---|---|---|---|
| `g` | shared dense block (`refoldSharedPriorDenseDroppingClocks`) | 31 | 16 | 0 | 3.21 m (G31), -3.02 m (G28), both at 275970 |
| `c` | clock clone (`networkClockCloneRefloat`) | 75 | 4 | 1 | -1.41 m (G28, 275970) |
| `n` | clock clone | 75 | 15 | 1 | 1.36 m (G32, 275940) |

  The storm's trips are false alarms in the sense that matters here: the fixture's satellite clocks do not move. On `g` the errors persist (G28 -2.27 m three epochs later, about -2.2 m to the leg's end).
- **The rows.** `include/network/carry/CouplingExact.hpp`: a carried local is stashed as a conditional row on the shared border, l = r + Mᵀs + e, and re-expressed each epoch over the columns S that the fold carries with a finite inflation Q_S, with F = (P_SS + Q_S)⁻¹; the rows are built in PASS B from the fold decision (`networkCouplingExactPrepare` reads it). The guard runs after the solve (`SolveGlue` stage 12) and withdraws only the shared prior (`src/core/network/solve/ClockAnchor.cpp`, the value correction). On `c` and `n` the rows put a cloned clock on its clone, and the guard withdraws the clone's walk factor instead.

## 3. Method

1. **Attribution at unit level.** A synthetic border with a clock and a phase bias, carried locals with harvested rows, data carrying a clock step: the solve after the guard's withdrawal compared with the solve whose rows are re-expressed with the clock LOOSE (the next epoch's consistent prior). The defect is attributed when the first departs from the second by an amplification that the rows' coefficients explain.
2. **Candidate response.** `network.clock_anchor_defer_coupled`: with the exact coupling update armed, a tripped clock whose own column is in the epoch's coupling set S is decoupled and marked but not withdrawn in the epoch; it folds LOOSE from the next epoch, with rows re-expressed for that decision. A clock whose rows reference its clone keeps the clone withdrawal. Counter, event and run-end line; default off.
3. **Legs.** The matrix rows `storm` and `sat_clk` on `g`, `c` and `n` with the knob, against the knobs-off labels `sck0b` (and `smx1`); the full matrix with the knob for collateral; the full matrix with the knob off for gate (1).

## 4. Bars and predictions (written 2026-10-26, before any leg with the knob ran)

- **B1 (the defect).** Storm on `g` with the knob: no tripped clock ends farther than 0.3 m from truth at its trip epoch than at the epoch before (0 against 16 of 31).
- **B2 (no collateral).** With the knob, no cell of the matrix changes verdict from pass to fail against `sck0b`.
- **B3 (scope).** On `c` and `n` every leg with the knob writes a `products.txt` byte-identical to its knobs-off twin (the clone path is not deferred).
- **P1.** Storm `g`: the same trips (30 on the phase statistic), no same-epoch re-solve, the storm's clk, zwd and iono channels lower than in `sck0b` (803, 4276, 24107 mm); the cell still fails.
- **P2.** Row `sat_clk` on `g` (SCR knobs off): the trip epoch emits the anchored value, so the clk channel's peak rises from 28.4 mm (the re-solve emitted -0.500 m exactly there, the arcs having been refloated); from +1 the wire is as in card SMX version 1 or closer to the step.
- **P3.** Row `sat_clk` on `g` with both SCR knobs: unchanged (its trip is folded LOOSE before the solve, nothing to defer).

## 5. Results (2026-10-26)

Exe `gnss_processor_ars1.exe` (sha256 `f5a196a0b9cb4a1f`; the Debug suite 3664 cases, 3631 passed, 33 fixture-gated skips, 0 failed). Labels under `D:/GnssProcessorSupport/gates/smx/`: `ars0` (knobs off), `ars1` (`network.clock_anchor_defer_coupled` true, recorded as the plan's `extra`), `arsb` (row `sat_clk` on `g` with both knobs of card SCR and the deferral). Legs: `D:/GnssProcessorSupport/runs_nom/evleg_smx_<label>_<row>_<posture>`.

### 5.1 Attribution at unit level

`tests/network/test_clock_anchor_coupled_resolve.cpp`, suite `ClockAnchorCoupledResolve` (6 cases): S = 5 satellites, R = 6 stations, one band, the real estimator, the real row re-expression (`couplingExactRow`) and the real withdrawal (`refoldSharedPriorDenseDroppingClocks`). Clock error of the step satellite against truth, m:

| case | (a) guard path | (b) consistent reference | (c) first solve | (d) LOOSE rows, same withdrawal | abs(a - b) |
|---|---|---|---|---|---|
| no step | +0.259 | +0.257 | +0.257 | +0.257 | 0.002 |
| step 0.5 m | +0.343 | +0.257 | +0.058 | +0.257 | 0.086 |
| step 3 m | +0.763 | +0.257 | -0.934 | +0.257 | 0.506 |

- (b) moves by the step against the no-step run to 2 mm. Its +0.257 m is the fixture's noise draw (formal sd 0.12 m), common to every path.
- The guard's path departs from the reference by 17 % of the step at both sizes (linear in the step). The same withdrawal applied to rows re-expressed LOOSE changes nothing (abs(d - b) at most 0.4 mm). The departure is the rows' conditioning on the anchored clock, not the withdrawal: the anchored rows keep a clock coefficient of 0.733 on their own column, the LOOSE rows 0.000.
- The departure grows with the rows' clock coefficient (satellites 3 to 12: 0.075 to 0.095 m), as the rows' residual variance shrinks (multiplier 100 to 0.01: 0.002 to 0.171 m), and with the stations carrying rows (R = 3, 4, 6, 12, 24, 48: 0.032, 0.053, 0.086, 0.186, 0.368, 0.695 m).
- The size is not reproduced. At R = 48 the departure is 1.4 times the step; card SCR's leg moved G25 by 24 times its 0.5 m step at 60 stations. The fixture has one band, no ionosphere and no leveling carry. It explains the direction and the scaling, not the size.

### 5.2 Gate 1 (default-off identity)

`report --label ars0 --control smx1`: IDENTICAL, 55 shared cells, every measured value and spread equal; 22 of 55 cells pass.

### 5.3 The matrix with the deferral

`report --label ars1 --control ars0`: 22 of 55 cells pass with and without the knob. Twelve channels differ, all in two cells, `storm`/`g` and `sat_clk`/`g`, which fail on both labels. Every other leg's `products.txt` is byte-identical between the labels (57 of 59 legs).

| cell | label | clk | pb | cons_peak | cons_plat | zwd | iono |
|---|---|---|---|---|---|---|---|
| `storm` `g` | `ars0` | 803.2 | 1022.4 | 80.8 | 68.6 | 4276.2 | 24107.0 |
| `storm` `g` | `ars1` | 191.9 | 1199.8 | 74.6 | 61.7 | 3589.9 | 20934.6 |
| `sat_clk` `g` | `ars0` | 28.4 | 94.6 | 147.1 | 105.1 | 57.4 | 658.5 |
| `sat_clk` `g` | `ars1` | 67.6 | 66.6 | 146.4 | 104.3 | 46.2 | 690.1 |

Event minus null, mm. Bars: `storm` `g` clk 11.2, pb 222.1, consumer 25.1, zwd 9.3, iono 221.5; `sat_clk` `g` clk 5.0, pb 192.7, consumer 21.1, zwd 14.7, iono 234.5.

**Storm, `g`.** The anchor's run-end: knobs off, 61 trips (31 on the phase statistic, 30 datum) and 21 in-epoch re-solves; with the deferral, 52 trips (22 statistic, 30 datum), 0 re-solves, 22 deferred. Each tripped clock, de-commoned against its label's null and classified as in section 2 (the epoch before the trip against the horizon):

| label | tripped clocks | horizon | pushed away > 0.3 m | brought closer > 0.3 m | within 0.3 m |
|---|---|---|---|---|---|
| `ars0` (re-solved) | 31 | trip epoch | 16 | 0 | 15 |
| | | +30 s | 15 | 0 | 16 |
| | | +90 s | 15 | 0 | 16 |
| | | leg end | 13 | 0 | 18 |
| `ars1` (deferred) | 22 (21 GPS, E14) | trip epoch, +30 s (the LOOSE fold), +90 s, leg end | 0 | 0 | 22 |

- The RMS over the GPS clocks of the de-commoned error, per 15 min of the leg: knobs off 0.44, 0.64, 0.74, 0.92, 0.88, 0.83, 0.82, 0.80 m; with the deferral 0.09, 0.17, 0.21, 0.22, 0.21, 0.19, 0.19, 0.18 m.
- The two trip sets share 15 trips and diverge after the first re-solve. G21 trips 13 times on both legs. The knobs-off leg's burst of seven satellites at TOW 278220 (G10, G15, G18, G23, G25, G29, G32; G15 to 3.68 m) has no counterpart with the deferral.
- The largest clock error left on the deferral's leg is G25's 1.11 m (TOW 277980). G25 is not tripped on either leg (its phase statistic 0.74 and 0.94 at TOW 277860, under k = 6; the run-end's "largest |w| 64.55" is the clock-space statistic), and its error grows alike on both legs between TOW 277800 and 277980 (0.49 to 0.93 m knobs off, 0.51 to 1.11 m with the deferral), with prior-residual reports of 3.9 m at 550 sigma on its leveling carry: the storm's own slip response, not the guard.
- **The pb channel's rise (1022 to 1200 mm) is not a measure of the deferral.** Both peaks are G26's phase biases after G26 sets and returns: both legs open a `gap` hold generation at TOW 280350, the grader compares the biases because the generation numbers match, and the restarted level differs from the null's by -4.7 m (knobs off) and -8.8 m (with the deferral). A new generation's level is a realization the consumer refloats on (standing invariant 5).

**Row `sat_clk`, `g` (card SCR's knobs off).** The IF-TD pool refloats every G25 arc at the step on both labels; the guard trips G25 at TOW 275910. G25's de-commoned clock minus the expected -0.5 m, mm (positive: short of the step):

| epoch | `ars0` (re-solved) | `ars1` (deferred) |
|---|---|---|
| 275910 (trip) | +0.1 | +500.4 |
| +30 s | -100.6 | +313.2 |
| +60 s | -163.3 | +205.6 |
| +90 s | -185.1 | +152.6 |
| +120 s | -214.4 | +99.1 |
| +150 s | -183.5 | +110.8 |
| leg end (277410) | -83.9 | +28.2 |

The deferred trip epoch emits the anchored clock, marked (`cta_disc_landed`), and the clock reaches the step from the anchored side as the LOOSE fold admits the code and the refloated arcs. The re-solve lands on the step at the trip epoch and then overshoots, as card SMX version 1 recorded.

### 5.4 Scope (B3) and the row with card SCR's knobs (P3)

- `c` and `n`: `products.txt` byte-identical on 38 of 38 legs between `ars1` and `ars0`; the state and consumer snapshots differ only in their wall-clock write stamp. The storm legs re-solve 33 (`c`) and 26 (`n`) times on both labels and defer none: the rows reference the clone.
- `arsb`: `sat_clk` `g` and its null are byte-identical to label `sckb` (exe `scr2`). With card SCR's knobs that leg has 0 statistic trips and 0 re-solves (its trip is the `slip_clk` fold), so the deferral has nothing to act on.

### 5.5 Bars and predictions

| item | result | verdict |
|---|---|---|
| B1 | 0 of 22 deferred clocks pushed beyond 0.3 m at the trip epoch, nor at +30 s, +90 s or the leg end, against 16 of 31 re-solved | PASS |
| B2 | 22 of 55 cells pass with and without the knob; the two cells that move fail on both labels | PASS |
| B3 | `c`, `n`: 38 of 38 legs byte-identical | PASS |
| P1 | no re-solve; clk 803 to 192, zwd 4276 to 3590, iono 24107 to 20935 mm; the cell still fails. The trip clause was miswritten (the knobs-off run has 31 statistic trips; the 30 are its datum trips) and is falsified as well: 22 statistic trips with the deferral | HELD, except the trip clause (FALSIFIED) |
| P2 | clk 28.4 to 67.6 mm, the trip epoch carrying the anchored clock 0.500 m short of the step. For two epochs after it the wire is farther from the step than version 1 (313 and 206 mm against 101 and 163); closer from +90 s (153 against 185) and at the leg end (28 against 84) | first clause HELD, second FALSIFIED at +30 s and +60 s |
| P3 | byte-identical | HELD |

### 5.6 The run-end warning with the deferral

On `ars1` the anchor's run-end warned "22 FAULT trip(s) (nis+abs) but ZERO in-epoch re-solves" and named a prior-dominated column or a missing captured prior as the cause; with the knob the cause is the deferral, which the summary line above it already counts. Exe `gnss_processor_ars2.exe` (sha256 `67d1a0da133bdfbe`) counts the fault-class share of the deferrals (`cta_resolve_deferred_fault`; a backstop trip of a classified clock event is not in it) and leaves it out of the warning. Label `ars2` (row `storm` on `g` with the knob): `products.txt` of the leg and its null byte-identical to `ars1`, the warning absent, the summary line unchanged. With the knob off the counter is 0 and the warning's condition is the old one. Debug suite on `ars2`'s tree: 3664 cases, 3631 passed, 33 fixture-gated skips, 0 failed.

## 6. Standing (2026-10-26)

The knob stays default off with ledger verdict UNVALIDATED; no golden, manifest or loader default changed.

1. **The defect stands on the golden.** Posture `g` arms the exact coupling update and the same-epoch re-solve together, so a statistical trip whose rows hold the clock's own column is re-solved against rows conditioned on the anchored clock: on the storm, 16 of 31 re-solved clocks pushed away from truth and none brought closer. Arming the deferral is the author's decision, through its ledger row and the generator.
2. **The deferral's cost.** On a true step with refloated arcs (row `sat_clk`, card SCR's knobs off) the trip epoch ships the anchored clock, 0.5 m short and marked, and the next two epochs are farther from the step than the re-solve's; from +90 s it is closer. With card SCR's knobs the classified event folds LOOSE before the solve and the deferral is inert. A clock step below the IF-TD thresholds with its arcs carried (the matrix row card SCR asked for) is still not a row of the matrix; it is the case where the deferral and the re-solve differ most.
3. **The size is unattributed.** The unit fixture's departure is 1.4 times the step at 48 stations, the leg's 24 times. The terms the fixture leaves out (a second band, the ionosphere, the leveling carry) are named, not measured.
4. **A grader trap of the matrix.** The pb channel compares two legs' biases whenever their generation numbers match, so a generation both legs restart (`gap`) is compared at two fresh levels (section 5.3, G26). A grade on the generations that began before the event would not count it. The matrix's grade is frozen at version 1 and is not changed here.

## 2026-11-06: the consumer channels read on a converged leg consumer (card CRS2)

Card [CRS2](CRS2_resume_hold_and_latches_2026-11-06.md) re-ran the storm and the satellite clock step with `network.clock_anchor_defer_coupled` over card SCR's two knobs on a leg consumer freed of card CRS's resume defects (label `crs2bscr`): the storm's `g` wire clock reads 191.8 mm against 782.8 without the deferral (bar 11.2, still failed), its `g` consumer 12.8 mm peak and 2.0 plateau (within the 20.0 bar; 182 and 177 mm on the metre-off consumer), and row `sat_clk` passes on `g` with every channel in bar.
