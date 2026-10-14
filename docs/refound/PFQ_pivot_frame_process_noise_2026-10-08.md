# PFQ: the carried clocks' process noise in the pivot frame

**STATUS**

- **id:** PFQ
- **topic:** network-vehicle
- **status:** closed
- **summary:** Pivot-frame clock noise FALSIFIED standalone, PASSES on card SCL's clock clone (hour-2 common level 0.051 to 0.011 m), neutral with X2. Closed, default off; it arms with the clone, the author's call.
- **source:** the theory audit of 2026-10-07/08 against Snow, *Adjustment Computations* (2026-01-05), chapters 15.9 (S-transformations) and 18 (the dynamic linear model); [SCL2](SCL2_level_residue_under_the_clone_2026-10-06.md) sections 3.2 and 11 (the measured common part: the pinned pivot's own a priori walk); [CTU](CTU_coupling_time_update_2026-09-28.md) section 13.2 (its batch fixture pins the pivot in the batch and the recursion alike); [Q3-PHYSICAL-FLOOR](../cards/clock/Q3-PHYSICAL-FLOOR.md) (q measured with the common mode removed); [PAC](PAC_common_drift_pivot_clock_2026-10-01.md)
- **knobs / posture:** `network.clock_pivot_frame_q` (new, default off, fingerprinted, solve-affecting; requires `network.shared_carry_full_cov` and the per-constellation satellite-clock pin datum). Arms on the golden posture; the primary bed is card SCL2's walk bed (`runs/_scl2_data_rw`, every satellite's a priori clock walking at q = 5e-6 m2/s, the pivot included) with `network.clock_anchor_q` 5e-6 and card SCL2's measure-only level monitors. Nothing armed on the goldens.
- **dates in the text:** 2026-10-08
- **hashes / run dirs:** exes (sha256[:16]) `gnss_processor_pfq0.exe` `8f736a74e57fea3d` (the tree before the change), `gnss_processor_pfq2.exe` `7049231af7aecc58` (the build graded), `gnss_processor_pfq3attrib.exe` `145f10f0a824d441` (the attribution, a temporary `PFQ_SCOPE` switch removed from the source after its build), `gnss_processor_pfq4.exe` `4bd59e4a32b2a5d0` (the final tree); `gnss_processor_pfq5.exe` `c669c9b0f1f75c80` (the joint clone walk, section 6); arms `runs/_rap_pfq_{g0b,off,on}_{f,s}`, `runs/_rap_pfq_wk_{off,on,fold,rows,all3}_s`, `runs/_rap_pfq_bc_{off,on}_s`, `runs/_rap_pfq_{cl0,cl,clp}_wk_s`, `runs/_rap_pfq_{cl,clp}_bc_s`, `runs/_rap_pfq_x2{,p}_{wk,bc}_s` (2 h each), smoke `runs/_rap_pfq_smoke_s`; unit gate `tests/test_pivot_frame_q.cpp` (suite `PivotFrameProcessNoiseTest`)
- **gates (frozen at pre-registration):** U0, G0 to G5 in section 3

**Outcome.** Question: does carrying the pinned pivot's walk in the clocks' process noise remove card SCL2's common level part? Standalone, no: the per-row time update drops the shared walk between rows (walk-bed consumer +14 mm). On card SCL's clock clone, one joint walk factor, yes: hour-2 common part 0.051 to 0.011 m, consumer within 2 mm. With card SCL2's shared history, neutral within 3 mm. Default off; it arms with the clone, the author's decision on the self-contained clock posture; the whole-border clone is card [BCL](BCL_previous_border_on_clones_2026-10-08.md), parked. Status reviewed 2026-10-13.

## 1. The question

The clock datum pins one satellite clock per constellation every epoch (`SbasisDeclaration::PerConstellationSatClockPin`). Every other carried clock is then x_j = c_j - c_p, its correction relative to the pivot's. If each satellite's correction walks independently at rate q_j, the state equation in that basis (Snow 15.9 applied to the state equation of chapter 18, Q_S = S Q S') gives each carried clock the increment q_j dt_j + q_p dt_j, and two carried clocks of the constellation the covariance q_p min(dt_j, dt_k): the pivot's walk over the overlap of their intervals. The receiver clocks absorb the pivot's level, so the phase and code biases are unaffected.

The fold adds q_j dt_j on the diagonal only (`NetworkClockAnchor.cpp` `clockAnchorFoldDecide`; the dense fold's `cov_eff` in `networkInjectD2CarryPrior`; the exact update's `q_s` in `networkCouplingExactPrepare`). Card Q3 measured q with the common mode removed, so it is the per-satellite rate; card CTU's batch fixture pins the pivot in both the batch and the recursion, so its equality cannot see the term. Along the constellation's common direction the missing noise is about N q_p / q_j of what is added: about 50 on the COD golden (card SCL2 measured the pivot's own a priori step at 1.2 mm rms per epoch) and far more on the broadcast bed (13.8 mm).

The question: does carrying the pivot's walk in the clocks' process noise remove the common part of the level error that card SCL2 measured, without cost elsewhere?

## 2. The change

One knob, `network.clock_pivot_frame_q`. With it on, the fold decision of every folded satellite clock of a constellation, the pivot excepted, adds r_p dt_j, where r_p is the pivot's own rate as the decision already forms it (the anchor's q when the pivot is anchored, the legacy rate when it is loose) and dt_j the interval the clock's own increment covers. The dense fold and the exact update's Q_S add r_p min(dt_j, dt_k) between every pair of such clocks. The pivot is the one the stashed posterior was pinned on. Nothing new is tuned: the term is the S-transform of the per-satellite rates the producer already uses.

## 3. Gates (frozen)

- **U0 (unit).** (a) The helper's increments equal S Q S' of the absolute-frame per-satellite increments, unequal intervals included. (b) A pivot-frame Kalman recursion with the term reproduces the absolute-frame recursion (pivot pinned by a datum row each epoch) to 1e-9 on every estimate and covariance; without it, the stated variance of the common direction falls below the absolute frame's and the normalized error along it exceeds 1.5 on a 500-epoch simulation. (c) The exact update with the pairwise Q_S equals a brute-force M'BM with the dense Q_S.
- **G0 (off identity).** The knob off, the new exe against the pre-change exe of the same tree: products, closed-loop trace and epoch status byte-identical on both golden beds over 2 h.
- **G1 (liveness, invariant 4).** On every ON arm the run-end counters are non-zero: clocks given the term, dense pairs, exact-update pairs, and the pivot rate's source (anchored, loose, no pivot entry) counted.
- **G2 (primary, the walk bed, 2 h).** `scl2_level.py decomp` with `walk_rw.csv`, ON against OFF of the same exe: the common part in hour 2 at least one third lower; the de-commoned rms in hour 2 not higher by more than 10 %; the consumer's 3D p50 in hour 2 not worse by more than 5 mm.
- **G3 (the broadcast bed, 2 h, report).** The same reads with `runs/_scl_data_bc` and `walk_bc.csv`; the direction is predicted to match G2, no bar.
- **G4 (do no harm, both golden beds, 2 h).** ON against OFF: the consumer 3D p50 in hour 2 not worse by more than 5 mm on either bed; emitted epochs equal; anchor trips and gate withholds not doubled.
- **G5 (cost).** Wall time within +5 % on the field golden.

<!-- Results are appended below this line under dated headings. -->

## 4. Correction at the smoke (2026-10-08, before any gate ran)

The 10-epoch smoke (`runs/_rap_pfq_smoke_s`, walk bed, knob on) was live (435 dense pairs per epoch, about 30 clocks) but counted "no pivot entry" on all 12 epochs: the stash skips pinned columns (`NetworkRunContext.cpp`, the D2 stash loop), so the pinned pivot is never carried and has no decision of its own. The build then fell back to the legacy rate, 1e-3 m2/s, which under the armed anchor is the LOOSE decoupling bound and not a walk (35,000 times the anchor's q). Section 2's "the pivot's own rate as the decision already forms it" therefore has no referent for a pinned pivot. Corrected before any arm: r_p is the model's physical per-satellite rate, the anchor's q with the anchor armed and the legacy rate without it (a carried pivot, which no current posture has, would use its own decided rate). The counter `pivot_model` records it. The gates in section 3 are unchanged.

## 5. Result (2026-10-08)

Ten 2 h arms on `gnss_processor_pfq2.exe` against the frozen gates, all emitting every epoch with exit 0; the walk bed's off arm reproduces card SCL2's `scl2d_d3off` over its first two hours to the printed digit (common 0.083 / 0.044 m, de-commoned 0.109 / 0.096 m).

| gate | reading | verdict |
| --- | --- | --- |
| U0 | 3 of 3: the helpers equal S Q S' with unequal intervals; the pivot-frame recursion equals the absolute frame to 1e-9 (estimates) and 1e-12 (covariances), and without the term its true error variance along the common direction is 2.8 times the stated one while a contrast of two non-pivot clocks is at 1.006; the exact update with the pairwise Q_S equals the brute force | PASS |
| G0 | `pfq_g0b_{f,s}` (pfq0) against `pfq_off_{f,s}` (pfq2): products, closed-loop trace and epoch status byte-identical on both beds | PASS |
| G1 | every ON arm: 240 decision passes, 7052 to 7328 clock key-epochs with the term, 417 to 455 dense pairs per epoch, the pivot rate the model's (pinned pivot) on every constellation-epoch | PASS |
| G2 | walk bed, hour 2, ON against OFF: common part 0.044 to 0.047 m (bar: at most 0.029); de-commoned 0.096 to 0.102 m (+6 %, bar 10 %); consumer 3D p50 0.073 to 0.087 m (+14 mm, bar 5 mm). No anchor trip, floor fold or re-float on either arm; the IF code spread in hour 2 0.105 to 0.113 m | FAIL |
| G3 | broadcast bed (report): common part in hour 2 0.030 to 0.040 m; de-commoned 0.105 m both; consumer hour 1 0.219 to 0.255 m, hour 2 0.075 m both | wrong direction |
| G4 | both goldens: consumer p50 equal to 0.1 mm (field 0.02385 m, sim 0.0985 m from hour 1), the same trips (2 / 0), re-floats (7 / 21 field), emitted epochs (240 / 241) and no withhold; the products differ, so the term is live but negligible at q 2.9e-8 | PASS |
| G5 | wall time 120 s ON against 137 s OFF on the field, both under the same concurrent load | no cost seen |

**Attribution (post hoc, not pre-registered).** A temporary switch put the term in one half only, on the walk bed (the unset control reproduced `pfq_wk_on_s` byte for byte):

| arm | consumer p50, hour 1 / 2 (m) | common part, hours 1 / 2 / tail (m) | de-commoned, hour 2 (m) |
| --- | --- | --- | --- |
| off | 0.269 / 0.073 | 0.083 / 0.044 / 0.020 | 0.096 |
| border fold only | 0.261 / **0.055** | 0.093 / 0.140 / 0.210 | 0.100 |
| coupling rows only | 0.284 / 0.085 | 0.089 / 0.084 / 0.105 | 0.101 |
| both (the build) | 0.289 / 0.087 | 0.079 / 0.047 / 0.052 | 0.102 |

**Reading.** The pivot's walk is common to every satellite clock, so after the exact update it is common to every carried coupling row of every station: the time-update covariance it adds between rows is as large as what it adds to each row. The producer's per-row form keeps the diagonal and drops the rest (card WSX; the solver review of 2026-10-08, item 2). Adding a perfectly common term to a recursion that treats it as independent row noise misstates the rows' information along exactly the direction the term is about, and the rows half is where the consumer loses. The border fold alone, with the correct common noise on the shared prior, gains 18 mm in hour 2, but with the rows unaware of the walk the code-visible level then drifts without bound (0.21 m at the tail), so it is not a posture either. The term is correct physics for the border prior; it can only be armed together with a recursion that carries the between-row covariance, the augmented previous border (the previous epoch's border as states), which keeps the arrowhead exact with the walk applied once in the filter.

**Standing.** `network.clock_pivot_frame_q` stays in the tree default off (fingerprinted, interlocked, counted), as the piece the augmented previous border will need; nothing armed, no golden or ledger change. Re-test it with that card, where the pre-registered G2 is the bar.

## 6. The augmented previous border, and PFQ on it (pre-registered 2026-10-08)

**What exists.** The augmented previous border (the previous epoch's border carried as states, the walk applied once in the border prior) is card SCL's clock clone for the clocks (`network.carry_coupling_clock_clone`, B1; mode 4 of the unit network is exact, mode 5 with the producer's per-station harvest keeps a residue that card SCL2's shared history X2 compresses). It clones the clocks only. On the goldens the phase-bias and code-bias carries walk at 1e-12 and 1e-10 m2/s with no floor (3e-11 and 3e-9 m2 per epoch), so their time-update term is negligible in steady state; extending the clone to them is left to the author's decision (section 7), and this section builds the part both paths need.

**The gap for PFQ.** B1's walk prior is one independent factor per clock, c_s(t) - c_s(t-1) at its fold variance, and its rows' epoch drops every off-diagonal noise pair that touches a cloned clock. With PFQ armed, the pivot's walk shared between two clocks therefore had no place on the clone, and the stand-ins (time t-1 states) carried the C1 fold's pivot pairs, which belong to this epoch's walk.

**The change (default off with either knob off).** With both `network.clock_pivot_frame_q` and the clone armed: the pivot pairs are removed from every covariance entry with a stood-in clone on either side; the stood-in clocks' walks fold as one joint factor A x = mean with the covariance diag(Q_s) + r_p min(dt_a, dt_b) between clocks of a constellation (`NetworkEstimator::addCloneWalkJoint`, information A' Qw^-1 A), checked positive definite before anything folds; card Q3's in-epoch refloat withdraws the whole joint factor and folds the factor over the kept walks (the walk vector's marginal), and a restore reverses it; the replay folds the captured joint factor. A pair between a stood-in clone and a clock folded on its own column (a seam satellite, a tiny walk) has no place in the factorization and is counted as lost. With PFQ off the joint covariance is empty and every path calls the per-walk form exactly as before.

**Gates (frozen).**
- **U0b.** The joint factor equals the brute force A' Qw^-1 A (and A' Qw^-1 mean) to 1e-9 relative, sign -1 withdraws it to 1e-9, a diagonal Qw folds the per-walk information, and a Qw that is not positive definite throws with nothing folded (`PivotFrameProcessNoiseTest.JointCloneWalkFactorIsTheInformationForm`, written and passing before this pre-registration).
- **G0b.** Card SCL2's clone recipe on the walk bed (`scl2d_d3`: the clone, the dense handover S-transform, the anchor's datum keep, the smoothed hold, the level monitors, q 5e-6) with PFQ off: the new exe byte-identical to `gnss_processor_pfq4.exe` over 2 h (products, closed-loop trace, epoch status).
- **G1b.** The clone + PFQ arm: joint walk folds on every fold epoch, pairs counted, no positive-definite failure.
- **G2b (primary, walk bed, 2 h).** Clone + PFQ against the clone alone: the common part in hour 2 at least one third lower; the de-commoned rms in hour 2 not higher by more than 10 %; the consumer 3D p50 in hour 2 not worse by more than 5 mm.
- **G3b (broadcast bed `scl2d_d1` recipe, 2 h, report).** The same reads; the direction is predicted to match G2b.

### 6.1 Result (2026-10-08, `gnss_processor_pfq5.exe`, 2 h, the sim from 00:30:15)

| gate | reading | verdict |
| --- | --- | --- |
| U0b | 4 of 4 in `PivotFrameProcessNoiseTest`; the clone, coupling, shared-history, smoothed-hold-clone and anchor suites unchanged (57 tests) | PASS |
| G0b | `pfq_cl0_wk_s` (pfq4) against `pfq_cl_wk_s` (pfq5), clone on, PFQ off: products, closed-loop trace and epoch status byte-identical | PASS |
| G1b | `pfq_clp_wk_s`: 240 joint folds, 99 969 walk pairs, 0 joint covariances that did not factor, 86 pairs lost (a stood-in clone against a clock on its own column) | PASS |
| G2b | walk bed, hour 2, clone + PFQ against the clone: common part 0.051 to 0.011 m (bar: at most 0.034); de-commoned 0.048 to 0.047 m; consumer 3D p50 0.046 to 0.044 m | PASS |
| G3b | broadcast bed, hour 2: common part 0.047 to 0.022 m; de-commoned 0.042 to 0.040 m; consumer hour 1 0.129 to 0.124 m, hour 2 0.058 to 0.060 m; one anchor trip (phase) against none | same direction |

The level by 120-epoch block (m; `scl2_level.py decomp`; the third block is the run's last epoch):

| arm | pivot frame | common | de-commoned |
| --- | --- | --- | --- |
| walk, no clone (`pfq_wk_off_s`) | 0.137 0.105 0.098 | 0.083 0.044 0.020 | 0.109 0.096 0.096 |
| walk, clone (`pfq_cl_wk_s`) | 0.106 0.070 0.063 | 0.073 0.051 0.039 | 0.077 0.048 0.050 |
| walk, clone + PFQ (`pfq_clp_wk_s`) | 0.105 **0.049** 0.049 | 0.072 **0.011** 0.001 | 0.077 0.047 0.049 |
| broadcast, no clone (`pfq_bc_off_s`) | 0.140 0.110 0.106 | 0.082 0.030 0.006 | 0.113 0.105 0.106 |
| broadcast, clone (`pfq_cl_bc_s`) | 0.109 0.063 0.044 | 0.078 0.047 0.013 | 0.077 0.042 0.042 |
| broadcast, clone + PFQ (`pfq_clp_bc_s`) | 0.106 **0.045** 0.041 | 0.073 **0.022** 0.009 | 0.077 0.040 0.040 |

The walk-bed clone arm reproduces card SCL2's `scl2d_d3` over its first two hours to the printed digit. The IF code spread in hour 2 (truth-free) moves 0.068 to 0.067 m (walk) and 0.064 to 0.062 m (broadcast).

**Reading.** The prediction of section 1 holds once the time update keeps the walk shared: with the clocks carried as clones, the pivot's walk enters once, in the joint walk factor, and the common part that card SCL2 attributed to the pinned pivot's own a priori walk is removed (0.011 m in hour 2, 0.001 m at the end of the walk-bed run), with the de-commoned level and the consumer unchanged. Sections 5 and 6 together are the attribution: the term is right, and it needs the augmented border.

## 7. Standing and what is owed (2026-10-08)

- `network.clock_pivot_frame_q` stays default off. It is correct physics for a pinned clock datum, and on the clone it does what the theory says; it belongs armed whenever the clone is armed. The clone itself is not on the goldens (card SCL: it is for a broadcast-grade clock walk, and at the golden q it is slightly worse than the per-row form), so arming both is the author's decision on the self-contained clock posture (cards SCL, SCL2, SCL2B, NBP), with the 6 h pair and the censuses there.
- **Owed, the author's decision: the clone over the whole border.** Extending the clone from the clocks to the phase and code biases would make the time update exact for them too. On the goldens they walk at 1e-12 and 1e-10 m2/s with no floor (3e-11 and 3e-9 m2 per epoch), so the steady-state gain is nil; the gain would be at the D-route events (a departed, re-initialized or re-referenced key, where card CTU re-attributes the whole stashed variance through P per row). Not built.
- The harvest still marginalizes the clones per station (mode 5's residue); card SCL2's shared history (X2) is the existing compression of that term and was not combined with PFQ here.

## 8. With card SCL2's shared history (2026-10-08, report: no bars were frozen)

The author asked for the combination of section 7's last item. Four 2 h arms on `gnss_processor_pfq5.exe`, card SCL2's X2 (`network.carry_coupling_shared_history`, k = 1) added to the section 6 recipe with and without the term (`runs/_rap_pfq_x2_{wk,bc}_s`, `runs/_rap_pfq_x2p_{wk,bc}_s`), against the section 6 arms. Each X2 arm differs from its section 6 control by that one knob. Exit 0 on all four; X2 live (29.3 history columns per epoch, 0 compression failures); the term live (240 joint folds, 0 not factored).

Hour 2 (m; `scl2_level.py decomp`, the consumer `p50.py` hour 1 / hour 2):

| bed | arm | pivot frame | common | de-commoned | consumer |
| --- | --- | --- | --- | --- | --- |
| walk | clone | 0.070 | 0.051 | 0.048 | 0.202 / 0.046 |
| walk | clone + PFQ | 0.049 | 0.011 | 0.047 | 0.203 / 0.044 |
| walk | clone + X2 | 0.030 | 0.018 | 0.025 | 0.216 / 0.070 |
| walk | clone + X2 + PFQ | 0.032 | 0.021 | 0.025 | 0.212 / 0.071 |
| broadcast | clone | 0.063 | 0.047 | 0.042 | 0.129 / 0.058 |
| broadcast | clone + PFQ | 0.045 | 0.022 | 0.040 | 0.124 / 0.060 |
| broadcast | clone + X2 | 0.030 | 0.017 | 0.024 | 0.105 / 0.086 |
| broadcast | clone + X2 + PFQ | 0.028 | 0.014 | 0.024 | 0.106 / 0.089 |

**Reading.** Under X2 the level is no longer held at an hour-1 value: every satellite's level follows its code, the pinned pivot's included, so the common part is the pivot's code realization (card SCL2 section 11, reading 2) and not its a priori walk. The term then has nothing left to remove: with X2 it moves the hour-2 reads by at most 3 mm in either direction on both beds. X2's own hour-2 consumer cost on these beds (+24 and +28 mm against the clone without it) is card SCL2's G7 finding and is unchanged by the term. The term remains the correct statement of the carried clocks' noise in the pivot frame; on the recursion with the shared history it is measurably neutral at 2 h.
