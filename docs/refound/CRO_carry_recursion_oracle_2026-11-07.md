# CRO: a multi-epoch oracle of the carry recursion across arc events

**STATUS**

- **id:** CRO
- **topic:** network-vehicle
- **status:** open
- **summary:** A unit test (`tests/network/test_carry_recursion_oracle.cpp`, suite `CarryRecursionOracle`) that compares the shipped per-station carry recursion of the golden posture (the exact coupling update in its per-row form, the float vehicle's bounded drop, the READMIT seed, the diagonal clock-pivot S-transform) with a joint information-form filter over one synthetic network and a 60-epoch window, with four events injected one at a time and then together. Tests only: no knob, no arm, no Release build.
- **source:** the scientific review card SRN (`D:/GnssProcessorSupport/worktrees/scirev/docs/refound/SRN_scientific_review_network_2026-11-06.md`, section 2.2), finding D4-11 in `D:/GnssProcessorSupport/srn/wave1.json`, confirmed by both lenses in `D:/GnssProcessorSupport/srn/wave2_verdicts.json`; the design documents whose claims are measured: card [CTU](CTU_coupling_time_update_2026-09-28.md) sections 4 and 9.2, card [SCL2](SCL2_level_residue_under_the_clone_2026-10-06.md) section 3, the [RF-1 design](RF-1_float_ambiguities_design.md) section 3.4, `include/network/solve/HandoverStransform.hpp` (the diagonal kernel's dropped cross term) and `include/network/carry/CouplingExact.hpp`.
- **dates in the text:** 2026-11-07
- **knobs / posture:** none added. The candidate reproduces the golden posture's recursion: `network.carry_coupling_exact_update` (per-row form), `network.shared_carry_full_cov` (dense border fold), `network.ambiguity_vehicle` `float_kf` with `network.amb_float_drop_decouple_always` and both floors at 0, `network.readmit_seed_horizon_s` 3900 with the Q3 anchor's q 2.9e-8 m^2/s, `network.handover_stransform` with `handover_stransform_before_rows` and `handover_stransform_dense` off.
- **hashes / run dirs:** branch `worktree-wf_c49deb5b-2f3-6` from main `284ec29`; no run directories (unit test only).

**Outcome.** Pre-registered 2026-11-07; results pending.

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
