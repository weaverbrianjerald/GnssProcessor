# CRO: a multi-epoch oracle of the carry recursion across arc events

**STATUS**

- **id:** CRO
- **topic:** network-vehicle
- **status:** closed
- **summary:** A unit test (`tests/network/test_carry_recursion_oracle.cpp`, suite `CarryRecursionOracle`, eight cases) that compares the shipped per-station carry recursion of the golden posture (the exact coupling update in its per-row form, the float vehicle's bounded drop, the READMIT seed, the diagonal clock-pivot S-transform) with a joint information-form filter over one synthetic network and a 60-epoch window, with four events injected one at a time and then together. Tests only: no knob, no arm, no Release build. Results in section 8: the exact claims hold to rounding; the drop and the diagonal kernel depart more than their documents state; a READMIT seed across a handover of its constellation lands in the outgoing frame (a defect candidate, field count owed).
- **source:** the scientific review card SRN (`D:/GnssProcessorSupport/worktrees/scirev/docs/refound/SRN_scientific_review_network_2026-11-06.md`, section 2.2), finding D4-11 in `D:/GnssProcessorSupport/srn/wave1.json`, confirmed by both lenses in `D:/GnssProcessorSupport/srn/wave2_verdicts.json`; the design documents whose claims are measured: card [CTU](CTU_coupling_time_update_2026-09-28.md) sections 4 and 9.2, card [SCL2](SCL2_level_residue_under_the_clone_2026-10-06.md) section 3, the [RF-1 design](RF-1_float_ambiguities_design.md) section 3.4, `include/network/solve/HandoverStransform.hpp` (the diagonal kernel's dropped cross term) and `include/network/carry/CouplingExact.hpp`.
- **dates in the text:** 2026-11-07
- **knobs / posture:** none added. The candidate reproduces the golden posture's recursion: `network.carry_coupling_exact_update` (per-row form), `network.shared_carry_full_cov` (dense border fold), `network.ambiguity_vehicle` `float_kf` with `network.amb_float_drop_decouple_always` and both floors at 0, `network.readmit_seed_horizon_s` 3900 with the Q3 anchor's q 2.9e-8 m^2/s, `network.handover_stransform` with `handover_stransform_before_rows` and `handover_stransform_dense` off.
- **hashes / run dirs:** branch `worktree-wf_c49deb5b-2f3-6` from main `284ec29`; no run directories (unit test only).

**Outcome.** Question: does the shipped per-station carry recursion track a joint filter across arc events? The reference equals the batch, and the factorized joint form equals the reference through births and erasures, to 5e-9 m. The shipped form's steady residue is 2.7 mm; a birth adds nothing. The drop's kept mean moves the border 5.6 cm and persists (erase: 1.0 mm). The READMIT seed costs 6.6 cm, conservatively. The diagonal handover kernel leaves the incoming pivot's phase bias 15 % over-confident (NEES 22); the dense kernel removes it. A clock parked across a handover is not re-expressed: 0.68 m at the return. Owed: field counts, a fix card.

> Results are appended TO THIS FILE, below the text.

---

## 1. The question

No unit test compares the shipped per-station recursion with a joint filter over several epochs that include an ambiguity-arc birth, a slip answered by the vehicle's drop, a satellite departure and return through the READMIT seed, and a clock-pivot handover (finding D4-11). The exact-update fixture (`tests/network/test_coupling_exact_update.cpp`) holds every line of sight for 60 epochs; its event scenarios (a decoupled clock, a satellite absent for ten epochs and returning fresh) are checked against the batch for the joint reference recursion only, and the shipped form (its mode 3) only on the event-free scenario. Two design approximations are documented and were never sized in a controlled fixture: the drop keeps the pre-slip mean at the arc's first-harvest variance (RF-1 design section 3.4: "a prior at birth variance is dominated by the epoch's data"), and the diagonal handover kernel over-states each between-satellite clock contrast by 2 var_q for one fold (`HandoverStransform.hpp`, the header's rule 2; measured as one-step algebra by `HandoverStransformDense.DiagonalKernelOverstatesAClockContrastByTwiceVarQ`).

## 2. The fixture

Five satellites (G01 to G05, index 0 to 4), three stations, two bands (GPS L1 / L2, alpha = (f1/f2)^2), phase (3 mm) and code (0.30 m) on every line of sight, white noise, a free receiver clock per station and epoch and a free slant ionosphere per line of sight and epoch (both eliminated each epoch), one phase bias per satellite and band, one float ambiguity per arc; station 0 holds the phase datum (its ambiguities known), the clock pivot is pinned at 0 and every other satellite clock walks at q = 2.9e-8 m^2/s over 30 s epochs (the Q3 anchor's rate). Sixty epochs; one random realization (seed fixed), drawn in a fixed order so that every scenario shares the same noise. The truth is generated in the estimator's frame: the pivot's clock is 0, the others walk independently, and at a handover the clocks are re-expressed (c' = c - c_q) before the next step. This is the model the producer's fold assumes (a diagonal walk on the non-pivot clocks), so the reference and the candidate differ only in the recursion.

Events (satellite and station indices from 0):

| id | event | epochs | detail |
|---|---|---|---|
| A | arc birth | 20 | the line of sight (station 2, satellite 3) is first observed at epoch 20 (the satellite rises at that station) |
| B | slip | 30 | both bands of (station 1, satellite 3) slip by 5 L1 cycles (0.951 m) and 3 L2 cycles (0.733 m) |
| C | departure and return | 40 to 49 absent, back at 50 | satellite 2 leaves the whole network and returns |
| D | clock-pivot handover | 45 | the pivot satellite 0 sets (absent from 45 on) and the pivot moves to satellite 1 |
| ALL | A, B, C and D together | | satellite 2 is parked across the handover |

## 3. The reference

A joint information-form Kalman filter over the whole state: every non-pivot satellite clock (an absent satellite's clock included: it keeps walking, unobserved), every phase-bias segment, every ambiguity arc. Per epoch: the time update (the walk on the clocks), new columns at zero information, the epoch's rows with the receiver clocks and slant ionosphere eliminated by a Schur complement, the posterior by inversion. Event handling is exact for the model:

- **A:** the arc's column enters at zero information.
- **B:** after a detected slip the ambiguity is a new parameter (Blewitt 1990; the DIA adaptation step): a new arc column at zero information; the old arc is never observed again.
- **C:** the clock is continuous across the gap (its walk accumulates q over 11 epochs); the phase bias returns as a new segment, which is the producer's rule (only clock and code bias park; `BorderStash.cpp`, the park loop: "a stiff PB seed would conflict with that pin"); the satellite's carried ambiguities stay continuous (the truth does not slip).
- **D:** the exact S-transform of the whole state, S P S^T with S = I - u e_q^T over the clock family (the incoming pivot's row removed); the outgoing pivot, known in the old frame and absent after the handover, has no column.

The reference is verified against the batch (every epoch's rows and the walk ties at once, the handover's tie written in the new frame) over epochs 1 to k at checkpoint epochs, to rounding.

## 4. The candidate (the shipped per-station form)

The existing fixture's mode 3, generalized to a changing set of arcs and columns:

- **Rows.** One conditional row per carried arc, N = nhat + M^T s + e, re-expressed each epoch through `couplingExactRow` (the one rule: S updated, D re-attributed through P), with the per-row inflation `var_add` on the diagonal only; the within-station harvest covariance kept, the cross-station one dropped. The rows' pinned clock-pivot term (the gauge projection's coefficient, -sum of the clock coefficients) and the update's `clk_deficit` go to the stash's pivot when it is in the layout, and are spread evenly over the S clocks when it left (`Coupling.cpp`, `exactRowIngredients`).
- **Border fold.** The dense prior N(s, P_SS with its diagonal replaced by var + q dt) over the carried keys present (`BorderPrior.cpp`, the dense branch).
- **A:** no row at the arc's first epoch; `var_first` is its first harvest's conditional variance.
- **B:** at the slip epoch `ambFloatDropFloored` (shipped; floors 0, `decouple_always` true) on a `LocalCarry` built from the arc's row: the row folds as N(value_abs kept, var restored), with no border terms and no within-station cross term; the next harvest rebuilds the row.
- **C:** the departed columns take the D route in the rows and leave the fold; the clock entry is parked at the last stash's value and variance with the park time of that stash; at the return the clock is seeded on the diagonal at var + q * gap (`carryFoldDecide`'s Seeded rule; under the anchor `clockAnchorFoldDecide` gives the anchor's q over the per-satellite gap, the same number); the phase bias returns fresh.
- **D:** the rows are built before the transform, against the stash in the old frame (the incoming pivot in S, its column pinned); then the shipped diagonal kernel `applyClockStransform` (mechanism `Handover`, membership from `NetworkSbasis::clockNullDirection` over the registered clocks) runs on a `NetworkCarryState` holding the carried entries and any parked entry; the dense fold takes the re-expressed values and diagonal with the stash block's off-diagonals; the incoming pivot's prior is folded and conditioned on its pin.

A second configuration of the same code, the joint block (mode 0: the cross-station harvest covariance and every row pair's time-update covariance kept) with a slip answered by erase and rebirth, is the factorized form of the reference and must equal it.

## 5. Instruments

Per epoch, over the border columns both sides hold (the present non-pivot clocks and the present phase biases): the difference d = candidate - reference (max |d| in metres, and z = max |d_i| / sigma_ref,i); the formal sigma of each (the ratio candidate / reference, min and max over the columns); NEES = d^T P_ref^-1 d against the border dimension n_b. At the handover fold: for every pair of non-pivot clocks in the candidate's dense prior, the contrast variance ratio, candidate prior over the reference's predicted prior, before and after the pin conditioning; and, inside the candidate, the excess of the folded contrast over the exact re-expression of its own carried block (the documented 2 var_q).

Each event is read in the six epochs from the event on, against the same window of the event-free baseline (the steady per-station residue).

## 6. Assertions (frozen before the build)

- **Exact (rounding):** (E1) the reference equals the batch at the checkpoints of every scenario, values to 1e-8 m and covariances to 1e-8 of the diagonal; (E2) the factorized joint recursion with erase-and-rebirth equals the reference at every epoch of the baseline and of scenarios A and B, at the same tolerance; (E3) inside the candidate at the handover fold, the folded contrast exceeds the exact re-expression of its own block by 2 var_q to rounding.
- **Measured (the design approximations):** per scenario, max |d| and NEES over the window and over each event's six epochs, and the contrast ratios, each bounded at no more than twice its measured value, with a comment stating what it measures.
- **Expectations from the documents, recorded, not gates:** A equals the baseline (a birth carries no prior); B's kept mean decays as w_prior / (w_prior + k w_data), small on the border (card SRN D4-06: "sub-millimetre on the satellite level", not measured); C's seed drops the parked clock's covariance with the rest and the gap's smoothing of its value, expected at the millimetre; D over-states the contrasts by 2 var_q in the prior of one fold. Card SRN's suggested bar (border means within 0.1 reference sigma one epoch after the event) is read against z.

## 7. Not modelled

The anchor's variance floor (1e-5 m^2) and guard (`clock_anchor_k`, `clock_anchor_resolve`), the clock clone and the shared history (off on the goldens), receiver and satellite code biases, the 1e10 pin weight (a hard pin here), the gap re-entry response that would also drop the returning satellite's arcs (its operator is event B's), Galileo, and more than three stations (the cross-station term grows with the stations per satellite: card WSX).

## 8. Results, 2026-11-07

### 8.1 What was built

`tests/network/test_carry_recursion_oracle.cpp` (suite `CarryRecursionOracle`, eight cases) and its entry in `CMakeLists.txt` (with `/bigobj`, as the other run-context test files). The candidate calls the shipped functions where one is callable without a run context: `couplingExactFactor`, `couplingExactRow`, `couplingExactCross`, `ambFloatDropFloored`, `applyClockStransform`, `applyClockStransformDense`, `NetworkSbasis::clockNullDirection`. Four rules are mirrored in the test because their code needs a run context: the dense fold of `BorderPrior.cpp` (the stash block with its diagonal replaced by var + q dt), the READMIT seed's var + q * gap (`carryFoldDecide`), the deficit spread of `exactRowIngredients` (`Coupling.cpp`) and the park of `BorderStash.cpp`. Options of the candidate, used as controls and decompositions: the joint block; the slip answered by erase and rebirth; three handover forms (the diagonal kernel after the rows, the golden; the dense kernel after the rows, `network.handover_stransform_dense`; a test-side exact form, the dense kernel with the rows re-expressed in the new frame first); the kernel's mechanism (`Handover`, or the wide `HandoverBeforeRows` traversal); the READMIT park off; and the pinned pivot's prior marginalized instead of conditioned on its pin. The fixture builders of `test_coupling_exact_update.cpp` are not shared: the arc lifecycle, the time-varying pivot and the clock continuous across a gap change every builder.

### 8.2 The exact assertions

| gate | statement | measured | bound |
|---|---|---|---|
| E1 | reference = batch over epochs 1..k, six scenarios, at epochs 20, 30, 40, 45, 50 and 60 | values 2.2e-9 m, covariances 1.2e-9 of the diagonal | 1e-8 |
| E2 | factorized joint recursion with erase and rebirth = reference, every epoch, scenarios baseline, A, B, A+B | values 4.9e-9 m, covariances 3.4e-10 | 1e-8 |
| E3 | diagonal kernel: folded contrast minus the exact re-expression of the candidate's own block | 2.000000 var_q, every pair | 1e-10 var_q |

### 8.3 The measured table

z is the largest |d_i| / sigma_ref,i; the sigma ratio is candidate over reference per column (min .. max); NEES is the largest of an epoch in the window, against the border dimension n_b. Each asserted row's bounds in the test are at most twice the measured value.

| scenario | window (epochs) | max abs d (m) | z | NEES (n_b) | sigma ratio | column that sets it |
|---|---|---|---|---|---|---|
| baseline | 1..60 | 2.70e-3 (epoch 60) | 0.029 | 1.01 (14) | 1.0000 .. 1.0001 | |
| A birth | 20..25 | 6.74e-4 (baseline 6.55e-4) | 0.0046 | 0.057 (14) | 1.0000 .. 1.0000 | |
| B drop | 30..35 | 5.60e-2 | 0.24 | 0.30 (14) | 0.9878 .. 1.0000 | slipped satellite's L2 phase bias |
| B drop | 46..60 | 4.02e-2 | 0.22 | 1.15 (14) | 0.9910 .. 1.0001 | |
| B, erase control | 30..35 | 1.02e-3 | 0.008 | 0.20 (14) | 1.0000 .. 1.0000 | |
| C departure | 40..45 | 1.96e-3 | 0.018 | 1.05 (11) | 1.0000 .. 1.0001 | |
| C seeded return | 50..55 | 6.58e-2 | 0.32 | 3.02 (14) | 0.973 .. 1.408 | returning satellite's new L1 phase bias |
| C seeded return | 56..60 | 1.09e-1 (epoch 57) | 0.57 | 2.53 (14) | 0.963 .. 1.340 | the same |
| C, fresh control | 50..55 | 2.07 | 8.79 | 123 (14) | 1.000 .. 4.80 | |
| D diagonal kernel | 45..50 | 3.51e-2 | 0.18 | 22.1 (11) | 0.852 .. 1.000 | incoming pivot's L1 phase bias (tight) |
| D diagonal kernel | 51..60 | 2.91e-2 | 0.16 | 3.07 (11) | 0.865 .. 1.001 | the same |
| D, dense kernel | 45..50 | 3.46e-3 | 0.018 | 0.56 (11) | 1.0001 .. 1.0003 | |
| D, exact form | 45..50 | 2.41e-3 | 0.023 | 1.13 (11) | 1.0000 .. 1.0001 | |
| D, pin marginalized | 45..50 | 3.39e-2 | 0.17 | 22.1 (11) | 0.862 .. 1.000 | (diagnostic, not asserted) |
| ALL golden | 1..49 | 7.31e-2 | 0.37 | 16.5 (8) | 0.854 .. 1.000 | incoming pivot's phase bias |
| ALL golden | 50..55 | 6.78e-1 (epoch 50) | 3.05 | 14.7 (11) | 0.859 .. 1.351 | returning satellite's new phase bias |
| ALL, wide traversal | 50..55 | 1.03e-1 | 0.55 | 3.12 (11) | 0.861 .. 1.637 | |
| ALL, exact form | 50..55 | 9.26e-2 | 0.50 | 0.93 (11) | 0.970 .. 1.674 | |

The handover fold (event D): each between-satellite clock contrast of the candidate's dense prior is 3.00 times the reference's predicted variance (var_q = 1.21e-2 m^2 is about one contrast in this fixture), before and after the pin conditioning. The drop (event B) acts on both bands, keeps the value and restores the L1 arc's variance from 1.70e-2 to 4.93e-1 m^2 (its first harvest), on a kept value 1.165 m from the post-slip truth. The READMIT seed (event C) folds at 1.37e-2 m^2 over the 330 s gap.

### 8.4 Reading

- **Rounding.** E1, E2 and E3. A birth equals the baseline, and the departure edge (the rows' D route and the fold's marginalization) stays at the steady residue. The exact coupling update in its joint form is exact through births, erasures and departures over 60 epochs; the shipped per-station form's steady residue is 2.7 mm by epoch 60 with every sigma within 7e-5 of the reference's, at three stations (the residue grows with the stations per satellite, card WSX).
- **Design-sized, as documented in kind.** The READMIT seed (C) drops the parked clock's covariance with the rest and the gap's smoothing of its value: the returning satellite's phase bias differs by 6.6 cm (z 0.29 one epoch after the return, 0.57 at its worst seven epochs later), stated up to 1.41 times looser than the reference, so the departure is on the conservative side; against a fresh return it removes 97 % of the miss. The diagonal kernel's contrast over-statement is exactly the documented 2 var_q.
- **Design-sized, larger than the documents claim.** The drop (B): the RF-1 design (section 3.4) and card SRN (D4-06) read the kept mean as costing little ("sub-millimetre on the satellite level", inferred there, not measured). Here a 1.17 m kept-value error at the restored 0.49 m^2 moves the border 5.6 cm (z 0.24) and does not decay in two or three folds: 4.0 cm remains 16 to 30 epochs later, in the code-determined clock and phase-bias split, while the same form with erase and rebirth stays at 1.0 mm. The size scales with the kept value's error, and one slipped line of sight is a third of its satellite's stations here against about a twentieth on the field golden; that scaling is not measured. Card SRN's bar (0.1 reference sigma one epoch after the event) is missed by B (z 0.22) and C (0.29); D meets it one epoch after (0.06) and misses it from epoch 48 (0.18).
- **Over-confident, not documented.** The diagonal kernel (D) is documented as conservative for the clocks ("dropping the cross term is conservative") with the clock-bias cross-covariances left stale. The measured consequence of those stale covariances is an over-confident phase bias of the incoming pivot: 15 % tight in sigma, NEES 22 at the handover epoch, 3.1 six epochs later, the column still 12 % tight at epoch 60. The dense kernel under the same rows removes it (NEES 0.56, every sigma within 2.5e-4), and the exact form adds little to that, so the rows' spread of the departed pivot's term is not material here; marginalizing the pinned pivot's prior instead of conditioning on its pin does not remove it.
- **A frame defect candidate (ALL).** The golden kernel (`applyClockStransform`, mechanism `Handover`) takes its membership from the registered clocks, so a clock parked by READMIT across a handover of its constellation stays in the outgoing frame, and the fold's release keeps the park (`parked_retired` excludes a released constellation, `BorderPrior.cpp`). The seed then lands c0 = 0.371 m off at a sigma of 0.117 m, and the returning satellite's phase bias misses the reference by 0.68 m (z 3.05). The `CarryPrior::t_park_s` documentation states that a parked entry "is walked by every frame re-base that walks prev_carry"; this walk skips it. The dense kernel and the wide traversal walk it by -c0, which brings the return to the size of event C alone (0.10 m). In the producer the anchor guard (`clock_anchor_k` 6, `clock_anchor_abs_m` 2, `clock_anchor_resolve`) is expected to trip on a seed this far off and re-solve the satellite loose, and the READMIT seed check counts it; neither is modelled here, and no run measured how often a seed follows a handover of its constellation.

### 8.5 What is owed

- **Field count (no run made, by the task's scope):** in the 6 h golden pair, the READMIT seeds whose constellation had a STRANSFORM between the park and the seed, with the seed-check outliers and anchor trips at those seeds. The candidate producer fix (a card of its own, default off) walks the parked clocks of the changed constellation in the diagonal kernel by the dense kernel's diagonal rule.
- **The dense kernel:** this oracle supports `network.handover_stransform_dense` (UNVALIDATED) as the cure of event D's over-confidence; its own arm is card SRN's D4-07 falsification.
- **The drop's scaling:** event B with more stations per satellite (the fixture's `kR` is 3) and the slip size swept.
- **Not modelled (section 7):** the anchor's floor and guard, the clock clone, the gap re-entry response of the returning satellite's arcs.

### 8.6 The suite (2026-11-07)

Debug `gnss_tests.exe` built in the worktree from `599b75c` (the test commit): 3,841 cases in 524 suites, 3,808 passed, 33 skipped (fixture-gated), 0 failed, 2,975 s; the new suite's eight cases are the eight added to the 3,833 of main `284ec29`, and pass in about 63 s on their own. No Release build and no data run (the task's scope).
