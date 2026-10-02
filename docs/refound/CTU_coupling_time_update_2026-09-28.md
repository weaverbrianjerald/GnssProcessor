# CTU: the coupling rows' time update made consistent (the clock level's root)

**STATUS**

- **id:** CTU
- **topic:** network-vehicle
- **status:** open
- **summary:** FOLLOW-UP 2026-09-29 (section 14), nothing armed: the sim ignition damage is a FIXTURE artifact (the 24 h sim fixture's first epochs carry orbit extrapolation; started at 00:30:15 the control converges to 0.14 m and freezes); the diagonal receiver code-bias carry counted the border twice (level information 2^k), fixed by `network.carry_rx_code_bias_in_block`; a truth-free level instrument (split-half NEES) is in the monitor; the anchor guard has a level-blind phase-space statistic; the field first-fold jump was a float-vehicle drop that did not decouple (`network.amb_float_drop_decouple_always`). Clean-start sim with the exact update: G1 and G2 PASS (0.03 m at 2 h, NIS 1.07); field G4 0.167 m against 0.15 (FAIL by 1.7 cm), G1 trips and G8 cost FAIL; G5 FAIL (a switch-on from a control snapshot keeps the level, held by the restored ambiguities); the consumer is worse with the exact update under every hold setting (the hold needs a level-aware form). Earlier: BUILT 2026-09-28, default off, not armed (section 13): the exact update removes the floor collapse (sim 63 % to 0 %) and moves the sim level from 1.15 to 0.28 m at 2 h, but G1, G2 and G4 fail; the within-station row covariance was removed after it drove a drift; the ignition damage common to every arm, the anchor guard tripping on the level's own motion, and a level instrument valid with carried ambiguities are next. PRE-REGISTERED 2026-09-28 from the outside review of the brief "Clock Level Handoff" (card CLV2 section 6.8: the author chose a consistent recursion over the out-of-solve corrections of cards CLV and CLV2). The diagnosis stands in the code: a clock term in a carried coupling row is time-updated with the legacy 1e-3 m2/s (`couplingQ`) while the border folds the same clock at the anchor's 2.9e-8 m2/s with a 1e-5 m2 floor, so kappa is 3.3e-4 on the clock term and 1 on the phase-bias term, and every carried ambiguity row, with the station's two phase rows, fixes the satellite clock and the slant ionosphere at phase precision (section 2). The switch-off arms confirm it (sim level 1.15 to 0.19 m at epoch 240; 4602 of 7327 clock folds at the anchor floor in the control, 0 with the update off). The proposed fix, the exact conditional time update, is right in kind and incomplete in three places, each of which fails a field arm on its own: the inherited border covariance is already collapsed along the level and must be raised to the code-only bound 1/I_L at the switch-on and at every seam (section 5); the phase-bias hold must change in the same card, because a converging level under the unchanged hold is the switch-off consumer at 2.58 m (section 6); and a decoupled or departed clock needs the exact re-attribution through P, where the scalar q to infinity rule is the lock again (section 4). Build card for the implementing session: section 9 (the build), section 10 (the gates, frozen before any arm).
- **source:** [CLV](CLV_clock_level_2026-09-22.md) sections 1, 2, 6.3, 6.4 item 1 and 8.3; [CLV2](CLV2_clock_level_batch2_2026-09-25.md) sections 5, 6 and 6.8; [Card 1d](../cards/network-vehicle/CARD-1D-COUPLING-CARRY.md) sections 2, 4 and 5; [Q3 design](../cards/clock/Q3-ANCHOR-DESIGN.md) sections 1 and 2.1; [RF-1 design](RF-1_float_ambiguities_design.md) sections 1.3 and 1.4; [PBH](PBH_phase_bias_hold_2026-09-19.md) sections 1 to 3; the brief (https://claude.ai/artifact/KnSDdZsT8ZzwHJfZkUhGn4) and the review page (https://claude.ai/artifact/7qKBCQLjqxvgHNt11xJLE6), both private pages of the author
- **knobs / posture:** section 14 adds, all default off and byte-identical off: `network.carry_rx_code_bias_in_block`, `network.dense_fold_kappa` (default 1e-6), `network.clock_level_innov_window_ep` (the monitor's instrument window), `network.clock_anchor_guard_space` (`clock` | `phase_shadow` | `phase`) with `network.clock_anchor_phase_sigma_floor_m`, `network.amb_float_drop_decouple_always`, `network.clock_level_restore_raise` with `network.clock_level_restore_credit_ep`; the arms use the existing `network.rank_gate_tol` 1e-11 and `network.start_offset_hours` 0.504167. Section 9: proposed, all default off and byte-identical off, on the RF-0hv working bases: `network.carry_coupling_exact_update` (the exact update, section 9.1), `network.clock_level_monitor` (measure-only accumulation of the code-only level information, section 9.3), `network.clock_level_bound_raise` (the rank-one raise of the border covariance along the level direction, section 9.3); the hold read uses the existing `network.emit_pb_hold`; nothing armed, no golden, no ledger row
- **dates in the text:** 2026-09-28
- **hashes / run dirs:** section 14: final exe `gnss_processor_ctu13.exe` sha256 prefix `aa5ebde2834ceb8e` (graded arms also on `_ctu8` `9534061c1420a82c`, `_ctu10` `641f766b5bbff093`, `_ctu12` `8194c84890edcaa9`); ignition arms `runs/_rap_ig_*`, clean-start arms `runs/_rap_i3_*`, `_rap_i4_*`, `_rap_i5_*`, field arms `runs/_rap_z_*`, `_rap_z6_*`, `_rap_z7_*`, `_rap_z8_*`, identity `runs/_rap_z13_*`; G5 seed `runs/evseed_ctu_g5d`, legs `runs/evleg_ctu_g5d_{ctl,ex,exnr,exq,exq1,exq10}`. The build (section 13): exe `gnss_processor_ctu4.exe` sha256 prefix `7d7765a11e842544`, arms `runs/_rap_y_{g0,ex}_{s,f}`, `runs/_rap_y_{exh0,h0}_s`, `runs/_rap_x_{g0,ex,mon,exmon}_{s,f}`, bisection `runs/_rap_b_{nocross,noinfl,local,qclk}_s`; the read is against main `001e3da`; the switch-off arms are the brief's, `runs/_rap_ctu_{s,f}{0,1,2,3}` (0 control, 1 time update off, 2 hold off, 3 both off), hybrid products `runs/_hyb_ctu_*`, replays `runs/_rap_rpl_*`, exe `gnss_processor_sk.exe` sha256 prefix `7a52f580cf47b189`; the phase-exact re-base of CLV2 section 6 is on branch `wip/clv2-station-share` commit `8d33b73` and is not merged

## 1. Verdict

The diagnosis holds and the code shows it. A clock term in a carried row is time-updated with
q = 1e-3 m2/s (`CouplingRowBuilder::couplingQ`, `src/core/filter/NetworkCoupling.cpp:66`, reading
`filter.process_noise.sat_clock_q` through `src/core/app/NetworkRunContext.cpp:50`; golden line
153), while the border folds the same clock at the anchor's 2.9e-8 m2/s with a 1e-5 m2 floor
(`clockAnchorFoldVariance`, called from the dense fold at `NetworkRunContext.cpp:3729`). With the
carried clock variance at the floor, kappa_clock = 1e-5 / (1e-5 + 0.03) = 3.3e-4 and kappa_pb is
about 1. Section 2 shows in two lines of the observation model why that asymmetry pins the level
at phase precision, and the switch-off arms confirm it: the sim level goes from 1.15 to 0.19 m at
epoch 240 with nothing else changed.

The framing of the brief's Q1 is right. The level is estimable from code only, the estimator
states it at phase precision, and no basis change can repair a precision error in an estimable
function. It is not a rank deficiency.

The proposed fix is right in kind and incomplete in three places. Each of the three would fail a
field arm on its own.

1. **The inherited covariance decides the outcome (Q3).** The exact update conditions on the
   border covariance P, and the old law has already collapsed P along the level to the anchor
   floor: 4602 of 7327 clock folds sit at the floor in the sim control, 0 with the update off. On
   that P the exact law converges the level at the anchor's random-walk rate, a time constant of
   several hours, not at the code's rate. The level variance has to be raised to the code-only
   bound at the switch-on and at every seam, and that bound is the accumulated ionosphere-free
   code information card CLV already computes per satellite.
2. **The hold changes in the same card (Q4).** Once the level converges inside the clock record,
   the current hold freezes that convergence into the wire's phase rows. The switch-off consumer,
   sim 2.58 m in hour 1 against 0.60 and field 1.99 against 0.56, is that, measured. The hold's
   transfer rule belongs to this card, and it needs no wire declaration.
3. **The special cases are where the scalar form is the lock (Q2).** For a decoupled or departed
   clock the shipped rule (q to infinity: drop the clock coefficient, keep the phase-bias
   coefficient) is the mechanism of section 2 once more, at every anchor trip and every departure.
   The exact form re-attributes the clock coefficient to the correlated phase-bias columns through
   P. Keeping the previous border as a nuisance block for one epoch makes every case exact, at a
   cost that is small because the border is satellite-sized.

## 2. The lock, in the observation model

```
phase   phi_j = rho + T + dt_r - c + p_j + N_j - alpha_j I      alpha_1 = 1, alpha_2 = gamma = (f1/f2)^2 = 1.647
code    P_j   = rho + T + dt_r - c + b_j + alpha_j I

slant ionosphere from the two code rows, given the border (row weights w_j):
        dI/dc = kappa' = sum_j w_j alpha_j / sum_j w_j alpha_j^2 = 0.71 at equal weights

harvested regression of N_j on its own satellite (the phase row, I substituted):
        M_c = 1 + alpha_j kappa'        M_p = -1

exact row     N_j - (1 + alpha_j kappa') c + p_j = r_j   with the phase row  ->  alpha_j (I - kappa' c) = r_j - phi_j + ...
shrunk row    N_j + p_j = r_j                             with the phase row  ->  c + alpha_j I = r_j - phi_j + ...,  j = 1, 2
```

The exact row and the phase row together constrain the combination I - kappa'c on each line of
sight. That combination is the station's memory of the level (CLV2 section 6.3), and it leaves c
to the code rows. The shrunk row and the two phase rows are two equations in the two unknowns c
and I, both at the rows' precision: the level of every satellite is fixed by every station at
every epoch, and the code, with a variance four orders of magnitude larger, cannot move it. The
code-residual pattern CLV section 1 measured, +0.29 e on L1 and -0.17 e on L2, is (1 - kappa') e
and (1 - gamma kappa') e at kappa' = 0.71: the same share, read the other way.

The unit test `TimeUpdateTurnsAnAmbiguityRowIntoALevelStatement`
(`tests/test_clock_level_carry.cpp:220`) asserts this on a fixture without the ionosphere term.
The field rows carry alpha_j kappa' as well; it is the coefficient a consistent update needs and
the shrink destroys.

Three findings already in the record are this one mechanism:

- The rigid give-back (CLV2 section 6.2). Rows that pin p and I drag c back through the phase
  rows; the kept fraction rising from 0.54 at the arm to 0.89 at 2 h is the rows stiffening as
  their conditional variance contracts.
- The free C1W family (CLV section 6.3). A code-bias coefficient keeps kappa about 1
  (q_cb x dt = 3e-9 m2), so the rows pin b_C1W together with p. The follow share and R-A2 were
  substitutes, built by hand, for the exact regression's co-movement of that column.
- `network.clock_level_station_kappa` = 0.713 (CLV2 section 6.4) is alpha_j kappa' at equal code
  weights, applied by hand to the ambiguities and the held datum. That it made the wire
  phase-exact is the strongest evidence for the consistent recursion, which supplies kappa' per
  station from the station's own weights and needs no re-base.

## 3. Q1: the framing, and the other carried factors

The shrink is sufficient. The switch-off arms set kappa = 1 on every present term and changed
nothing else; the sim level went from 1.15 to 0.19 m at epoch 240 and the residual spread from
0.97 to 0.08 m. The 0.19 m is not the target: kappa = 1 ignores the walk, the anchor guard trips
(field NIS trips 1 to 8; the brief's post-fit re-floats 18 to 41) and the level is re-drawn at each
trip. The target is the code-only bound of section 5.

| arm | clock folds at the anchor floor | anchor trips (NIS) |
| --- | --- | --- |
| sim control, `_rap_ctu_s0` | 4602 of 7327 | 1 |
| sim, time update off, `_rap_ctu_s1` | 0 of 7327 | 2 |
| field control, `_rap_ctu_f0` | 3243 of 7328 | 1 |
| field, time update off, `_rap_ctu_f1` | 0 of 7328 | 8 |

(the "Card Q3" run-end line of each arm's `stdout.txt`)

The other candidates, in order:

- **The anchor's floor.** A cap on one clock's marginal information per fold, not a statement
  about the level. It binds in 63 percent of the sim control's folds (44 on the field) because
  the rows have collapsed the clock's variance below it, and in none of the update-off folds. The
  floor count is the symptom counter of the collapse and belongs in this card's run-end line.
- **The border fold.** Consistent: the stashed posterior with the diagonal raised by q x dt and by
  the floors, off-diagonals kept (`NetworkRunContext.cpp` 3714 to 3790). Raising a diagonal never
  states a level.
- **The held datum.** One station's ambiguity per (satellite, signal) held constant fixes -c + p_j
  against the datum and leaves c + p_j free. Not a lock; the continuous handover pins the successor
  at its own carried value, which is continuity, not information.
- **The receiver-side carries** (receiver clock, receiver code bias, ZWD rows) regress on
  satellite clocks and get the same shrink. Second order, at code precision; the same fix covers
  them.
- **The local-local half** (F3 off on this posture, RF-1 design section 1.3). Within-station
  conditional correlations are dropped, which over-counts the station-common directions, not the
  level. It remains after this fix and is named as remaining (section 11).

The second half of Q1: yes, and nothing beyond the exact regression is needed. The posterior
correlation between N_j and the level, with the slant ionosphere marginalized every epoch, is the
coefficient alpha_j kappa' in M. The exact rows co-move with the level; the shrunk rows pin it. The
per-station re-base of CLV2 section 6 is what the exact rows do on their own.

## 4. Q2: the exact update, every case, and what the per-row form drops

```
l = r + M' s + e,   e ~ N(0, C)             s+ = s + w,   w ~ N(0, Q)           (M' here is M transposed)
w | s+  ~  N( Q (P+Q)^-1 (s+ - s_hat),  (P^-1 + Q^-1)^-1 )
l | s+  =  r' + M_new' s+ + e'          M_new = (P+Q)^-1 P M        Var e' = C + M' (P^-1 + Q^-1)^-1 M

one rule for every case   (S: columns present with finite noise;  D: decoupled or departed)
M_new_S = (P_SS + Q_S)^-1 P_S,all M           Var += M' [ P - P_all,S (P_SS + Q_S)^-1 P_S,all ] M
```

The brief's form is the correct conditional time update: the regression of l on s+ after the
border has moved, with P the joint border posterior in the frame the row was harvested in. The
one rule above covers the rest. With D empty it is the brief's form. With P block-diagonal it is
the shipped departed-column variance (`NetworkCoupling.cpp` 288 to 314). For a departed or
decoupled clock whose phase biases are correlated with it along the level, P_pc about P_pp, it
gives M_new_p = M_p + (P_pc / P_pp) M_c, about M_p + M_c: the row keeps its co-movement with the
level instead of pinning p. That re-attribution is the whole difference between the exact form and
the scalar q to infinity rule, and it is where the scalar rule re-creates the lock at every anchor
trip and every departure. A datum-pinned column (p_prev = 0, no noise) stays outside S with its
coefficient and its pinned value, as card 1e does.

**The per-row inflation is not guaranteed conservative.** It drops M_i' S M_k between rows,
S = (P^-1 + Q^-1)^-1, which is bounded by Q. On this posture S is the anchored clock's q x dt =
8.7e-7 m2 (0.9 mm) and about zero on phase and code biases, so the dropped term is one error of
0.9 mm shared by every row on the satellite. Sixty rows (30 stations, two signals) at a
conditional variance of 2.5e-5 m2, treated as independent, claim the clock's walk at a variance
(v + M2 S) / (v + n M2 S), about a third of the truth or less. The rows count the walk about three
times too tight: a stiffer-than-measured carry, the class Q3 design section 1.1 names. The
instrument is the anchor's NIS trip rate against the control, the bound is the variance floor, and
the exact cure is the augmented block below; the per-station joint whitening of card 1b does not
reach it, since the rows sharing a satellite sit at different stations. The level itself is not
touched by the dropped term: Q has no component along a constant.

**The augmented block.** Keep the previous border s_old as a nuisance block for one epoch: prior
N(s_hat, P), tie rows s+ - s_old ~ N(0, Q), the carried rows unchanged with their exact M on s_old,
and s_old marginalized by a Schur complement after the solve. A decoupled clock has no tie row; a
pinned column is fixed; a departed column has no s+ column and is marginalized with its prior. This
is stochastic cloning, and the process-noise form of the square-root information filter. It is
exact for the border side, the kappa, the inflation and the card 1f gauge restore all disappear,
and the cost is a border twice as wide in the station reductions.

## 5. Q3: the inherited covariance decides whether the fix works

Yes, an overconfident P makes the exact update leak, and the leak is not small. The regression
E[w | s+] = Q (P+Q)^-1 (s+ - s_hat) attributes any move of the border away from its prior mean to
the walk, in proportion to Q / (P+Q). With P at the floor, 1e-5 m2, and Q at 8.7e-7 m2, eight
percent of every clock coefficient is dropped, and by section 2 that fraction of every row is a
level statement at the row's precision: about 6 cm per row, about 1 cm per satellite per epoch
over sixty rows. Against the code's 0.5 m per epoch the level then moves toward the code with a
gain of a few 1e-4 per epoch. In the augmented form the same thing reads more simply: the level of
s+ inherits the level variance of s_old plus one epoch of q, and grows by q x dt per epoch toward
the anchor's random-walk steady state, a time constant of several hours at 2.9e-8 m2/s. Either
way, the exact law switched on over the inherited P keeps the freeze through every 2 h check and
most of a 6 h run.

The inherited P is the rule, not the exception: it is what the old law produces at every harvest,
what every snapshot carries, and what the control shows. Two orders of magnitude separate it from
the honest number at 2 h: the code-only bound is R/N, about 0.5 m2 / 240 = 2e-3 m2 (R from CLV
section 1's 0.69 m single-epoch solve), against a few 1e-5 at the floor.

**The bound is the safeguard, and card CLV already computes it.** The level is estimable from the
ionosphere-free code only, so for any consistent estimator the posterior variance along u_s obeys
P_L >= 1 / I_L, with I_L the accumulated code-only information of that satellite's level.
`clockLevelTwoWay` returns it per epoch and `ClockLevelSat::info` accumulates it
(`src/core/filter/NetworkClockLevel.cpp` 165 and 823 to 831). Three uses follow:

1. At the switch-on, at a warm resume and at every re-seed of a clock or phase-bias carry, raise P
   along u_s to at least 1 / I_L, once, as a rank-one raise on the stashed block, never a set
   (card OWD O2). A cold start that runs the exact law from epoch 1 needs no raise on the border:
   the ignition epochs carry no rows, and P at the first harvest is the code-only batch.
2. On every run, the monitor P_L / (1 / I_L) per satellite per epoch, which must not fall below 1
   after the first harvest. It is truth-free, so it is the field's consistency instrument for the
   level; on the control it reads far below 1, which is the freeze's fingerprint without a truth.
3. On the sim, the normalized error along u_s against truth, e_L^2 / P_L averaged over satellites,
   near 1, and e_L itself tracking sqrt(R/N).

This retires CLV's corrector and keeps its accumulator, which is the better disposition: the
machinery that measured the freeze becomes the instrument that proves its absence.

## 6. Q4: the level coordinate, and what the hold must do

The component is well defined as a coordinate. In the estimator's basis c is the code-defined
clock (the IF pin on C1C and C2W), p_j is the offset between it and the phase-defined clock of
signal j, and (c, p_1, p_2) decompose uniquely into the level (all three together), the physical
clock (c alone) and the inter-signal difference (p_1 - p_2). The physics assigns process noise to
c alone, which is what the goldens do (q_pb = 1e-12, the anchor's q on c). A linear change of
variables to a phase-defined clock leaves a Kalman filter invariant when Q is transformed with it,
so a phase-clock product changes nothing in the estimator; on the wire it moves the phase-bias
wander into the clock record, where the consumer cannot hold it. The recursion is the fix, not the
basis.

What one epoch's update cannot give is the attribution of the phase-bias increment between
convergence and wander; only the posterior precision separates them. That settles the product
question in a way the brief's item 3 does not state:

- With a consistent recursion the level moves inside the estimated clock every epoch, gradually
  and undeclared. The current hold keeps P_j and lets the wire's phase row move by the whole
  convergence (`src/core/filter/NetworkPbHold.cpp` 145 to 152: the held value follows only the
  declared step). That is the reverse hybrid of the brief's table: held phase biases identical,
  the clock record carrying 1.1 m of level convergence on the sim, the consumer at 2.58 m. A field
  arm of the recursion under the unchanged hold measures the hold, not the recursion.
- "Pass level motion" means passing the phase-bias state's own increment into the held value each
  epoch. Since that increment is the level's convergence plus the estimator's own wander, the rule
  is the same as not holding the phase bias at all. It is correct exactly when the wander is gone,
  which is what item 2 of the brief measures. No declaration is needed on the wire in either case:
  C and P move together and the phase row is unchanged.
- So the order of the plan changes: item 2 (the phase side with the hold off, on the sim against
  truth) is graded on the same build as item 1, and the hold's disposition, retired or a hold on
  the residual wander read there, ships in this card before any consumer number is quoted. A field
  consumer grade of item 1 alone is not interpretable.

One more thing the consumer will see. Its code rows move by the level's convergence while its
phase rows do not, and it has absorbed the old level into its own ionosphere and ambiguities at
the same share kappa'. With a gradual convergence its code rows re-determine that direction epoch
by epoch, and the step response of CLV2's `level_step_inflate` is not needed. Hour 1 of a cold
consumer still pays for whatever level the first epochs' code drew, on both beds; that is the
code's own noise, not a defect.

## 7. Q5: the literature

- Teunissen, Khodabandeh, Psychas (2021), A generalized Kalman filter with its precision in
  recursive form when the stochastic model is misspecified, J Geod 95:108,
  https://doi.org/10.1007/s00190-021-01562-0. The recursion of the actual error variance under a
  misspecified stochastic model is the tool that would have predicted the freeze from the kappa
  mismatch analytically, and it is the right frame for the sim's normalized-error gate.
- Khodabandeh, Teunissen (2015), An analytical study of PPP-RTK corrections: precision, correlation
  and user-impact, J Geod 89, https://doi.org/10.1007/s00190-015-0838-9. The code-only precision of
  the satellite clock and phase-bias corrections, the bound of section 5, in closed form for the
  undifferenced uncombined model.
- Psychas, Khodabandeh, Teunissen (2024), Multi-epoch PPP-RTK corrections: temporal characteristics,
  pitfalls and user-impact, J Geod 98:15, https://doi.org/10.1007/s00190-024-01823-8. The
  between-satellite clock convergence across epochs and its user impact, the closest published
  counterpart of cards CLV and CLV2. A 2026 J Geod paper on the multi-epoch estimability of
  satellite phase biases and S-basis choices (https://doi.org/10.1007/s00190-026-02091-4) was not
  read and looks relevant to Q4.
- Odijk, Zhang, Khodabandeh, Odolinski, Teunissen (2016), J Geod 90:15. Estimability of the
  undifferenced uncombined network model; the ground for "estimable, therefore not a datum
  problem".
- Dyer, McReynolds (1969), Extension of square-root filtering to include process noise, JOTA 3;
  Bierman (1977), Factorization Methods for Discrete Sequential Estimation. The information-form
  time update with process noise adjoins the noise as unknowns and eliminates them by an
  orthogonal transformation: the augmented block of section 4 in its classical form.
- Roumeliotis, Burdick (2002), Stochastic cloning, ICRA; Mourikis, Roumeliotis (2007), the MSC-KF,
  ICRA. Carrying a copy of the previous state so that measurements relative to it stay exact
  after a process-noise step: the same augmented block in the robotics literature.
- Sibley, Matthies, Sukhatme (2010), Sliding window filter with application to planetary landing,
  J Field Robotics 27; Kaess et al. (2012), iSAM2, IJRR 31. Conditional factorization with
  marginalization of separator states; the Bayes tree is the exact structure of this producer's
  carry, without process noise on the separator.
- Julier, Uhlmann (1997), covariance intersection, ACC. The standard consistent fusion under unknown
  cross-correlation: the honest answer when cross-row terms are dropped, and pessimistic, which is
  why the exact augmented form is preferable here.
- Huang, Mourikis, Roumeliotis (2010), Observability-based rules for designing consistent EKF SLAM
  estimators, IJRR 29. Spurious information along weakly observable directions from an
  inconsistent recursion: the same failure class as a level pinned by shrunk rows, with the same
  remedy of checking the estimator's information against the model's.

The cheapest consistent approximation with a name is the Schmidt-Kalman "consider" treatment
(Schmidt 1966): carry a nuisance block's covariance but never update its mean. It is not needed
here, because the nuisance block is small.

## 8. Q6: cost at 300 stations

The dense covariance over 25 000 states is never formed by any of the forms above. The factorized
carry needs P over the border only, which is satellite-sized: about 32 satellites times one clock,
two phase biases and one or two code biases, 150 to 200 columns on a GPS-only posture, and it does
not grow with the number of stations. `prev_carry_cov.P` already carries it (the dense block
registers every stashed clock, phase-bias and code-bias entry under `shared_carry_full_cov`,
golden line 290; the Fix-A phase hold that would divert the phase biases to the diagonal fold is
inert on this posture, `NetworkRunContext.cpp:386`).

| form | per epoch at 300 stations | against |
| --- | --- | --- |
| matrix update with per-row inflation | one factorization of P + Q (about 160^3) and one triangular solve per row (160^2 times about 6000 rows): 1.5e8 flops, well under 0.1 s | 26.7 s per epoch measured at 305 stations |
| augmented previous border | the border doubles in the station reductions and in the border solve (320 columns); the station blocks are unchanged | the reductions' border terms grow 2 to 4 times, still small against the local blocks |

So the answer to Q6 is yes, and stronger than the question: the exact form is affordable at that
scale, the approximation is not needed for cost, and the only real cost is the plumbing of a twin
column set in the estimator.

## 9. The build (for the implementing session)

Every knob default off and byte-identical off (invariant 6); one build in the main tree at a
time; new `.cpp` files need a `CMakeLists.txt` entry; one new test file per new suite; rapid mode
(2 h arms from `config/golden/golden_{sim,field}_refound.json`, one knob per arm; 6 h only for
adoption). The seam map of `CLAUDE.md` applies: the update lives in the coupling builder, the raise
in the stash, the monitor beside card CLV's seam, the telemetry in `NetworkTeardown`, counters
and carried state in `NetworkRunContext`.

**9.1 `network.carry_coupling_exact_update`** (bool; requires `carry_coupling_time_update` and
`shared_carry_full_cov`, loader-refused otherwise; fingerprinted like the time update). In
`CouplingRowBuilder` (`src/core/filter/NetworkCoupling.cpp`, the scalar path
`scalarRowIngredients` 268 to 354, the joint path 165 to 257, and `buildLocalBlock`, which shares
the scalar ingredients):

1. One Q, read from the fold. The D2 fold records, per stashed key, the inflation it applied,
   var_eff minus the stashed variance (the anchor's q x dt_sat for an anchored clock, the loose
   rate for a loose one, the reinit variance for a flagged one, infinity for a decoupled column
   and for a key it skipped), on `ctx.carry` (a map keyed like `sharedCarryKey`). The rows read
   that map instead of `couplingQ`; `couplingQ` stays for the SH terms and for the scalar path.
2. The one rule of section 4, over the row's present terms with P from `prev_carry_cov.P`: the
   surviving set S is the present columns with finite Q; D is the decoupled, departed,
   reinit-diverted and re-admission-diverted columns (the three `c = -1` routes at 288 to 314 and
   188 to 194); pinned columns (`p_prev = 0`, card 1e) stay outside both with their coefficient
   and `s_prev`. The coefficients of D are re-attributed to S through P, the variance by the
   bracket term. P must be in the frame of the harvest's `cov_df` (`NetworkRunContext.cpp` 4287
   to 4291); if the stashed `cov` and the harvest's `cov_df` differ by the datum transform, the
   builder reads the harvest frame.
3. Per-row inflation first (the exact marginal for each row, cross-row terms dropped). The
   augmented previous-border block (section 4) is the escalation if the over-count instruments
   of section 10 trip, and in any case the batch reference of the unit gate.
4. The card 1f gauge restore (`tuGaugeRestore`, 81 to 120) is not applied on the exact path. The
   W5 null-space gate is the check that the exact rows are gauge-invariant without it; if they
   are not, P's gauge treatment is the defect, not the rows.
5. Counters (invariant 4): terms updated exactly, terms re-attributed (by route: decoupled,
   departed, reinit, re-admission), terms pinned outside, rows whose (P_SS + Q_S) failed to factor
   (the row then takes the scalar path, counted), and the per-row inflation's mass. Run-end line
   in `NetworkTeardown`.

**9.2 The batch gate** (`tests/test_coupling_exact_update.cpp`, suite `CouplingExactUpdateTest`).
The card 1d fixture (`tests/test_network_estimator.cpp` 2790 to 2925) carries clocks only, one
band, no ionosphere and no phase-bias column, which is why it passed the scalar form; card 1d's
own text says the same of its predecessor. The new fixture: three satellites, each with a clock
and two phase-bias columns; two stations with two-band phase and code rows, a free slant
ionosphere per line of sight and epoch, and carried ambiguities; a held datum (one station's
ambiguities pinned); the anchor's random walk on the clocks (q = 2.9e-8 m2/s, 30 s); sixty
epochs. Cases: the level contrast's value and variance equal the batch (all epochs' rows plus the
walk pseudo-observations) to 1e-8; the shrunk form fails by metres; a satellite decoupled at
epoch 30 (no tie) and one departed for ten epochs both equal the batch; q = 0 reproduces the raw
rows to 1e-13; the per-row inflation is never below the exact marginal variance of its row.

**9.3 `network.clock_level_monitor`** (bool; mutually exclusive with `clock_level_carry`; trace
class, solve byte-identical) and **`network.clock_level_bound_raise`** (bool; requires the
monitor; fingerprinted). The monitor runs card CLV's pair build, two-way fit and accumulation
(`networkClockLevelRebase` steps 1 to 3, `NetworkClockLevel.cpp` 471 to 679) and applies no shift:
`I_L` per satellite with the horizon and the cap as today, and per epoch the border posterior's
variance along u_s, P_L = u_s' P u_s over the satellite's clock and phase-bias columns of
`prev_carry_cov.P` (the contrast against the constellation, so the datum is out). Trace
`logs/network_level_bound.csv` (week, tow, sat, p_level_m2, inv_info_m2, ratio, raised_m2) under
`trace_events`; run-end line with the ratio's min and median over satellites and the count below
1 after the first harvest. The raise: at the stash, for every satellite whose ratio is below 1,
P += (1 / I_L - P_L) u_s u_s' on the stashed block (raise, never set: card OWD O2), counted per
satellite with the raised amount (invariant 7, the re-coupling of the level to the code), and
applied to a warm-resume snapshot's block on restore and to every re-seed of a clock or
phase-bias entry (the READMIT seed at var + q x gap included). The corrector of cards CLV and CLV2
(`clock_level_carry` and its sub-knobs) stays built and off; it is retired by this card's result,
not by this build.

**9.4 The hold.** No new knob for the read: arms with `network.emit_pb_hold: false` on the sim
give the estimator's own phase side against truth (PBH read 3's instrument, `scripts/ftc_step_attrib.py`,
the within-arc phase-row error against the clock alone). The disposition follows the read: if the
within-arc phase-row error on the exact build is at the clock's own (sim 0.003 m against 0.020 on
the control), the hold is retired for this posture; if a residual wander remains, the hold's
generation semantics stay and its held value follows the estimator's phase bias (which is the
hold off) while the wander is read as a remaining inconsistency. Either way the C-record `lvl=`
declaration and the CLV transfer in `NetworkPbHold.cpp` are not exercised by this card.

**9.5 Arms** (2 h, both beds, the goldens as base, run paths and one knob changed each):
`x_g0` (nothing; identity), `x_ex` (9.1 alone), `x_mon` (9.3 monitor alone: solve byte-identical
to `x_g0`), `x_exmon` (9.1 + monitor), `x_exraise` (9.1 + monitor + raise), `x_exraise_h0` (the
same, hold off; sim first), then the consumer and census keys only after 9.4's disposition. Every
arm from a cold start; a warm-resume arm of `x_exraise` from a control snapshot is the seam gate
of section 10.

## 10. The gates (frozen before any arm runs)

| gate | arms | bar |
| --- | --- | --- |
| U0 | the suite | `CouplingExactUpdateTest` passes with the cases of 9.2; the full Release suite green; `python scripts/docs_index_check.py` and `make_golden_config.py --verify` pass |
| G0 | `x_g0` on both beds; `x_mon` against `x_g0` | `products.txt` (comments stripped), `closed_loop_trace.csv` and `network_epoch_status.csv` identical to `runs/_rf0hv11_{field,sim}6` over the window; the monitor arm's solve artifacts identical to `x_g0` (trace only) |
| G1 structure | `x_ex`, `x_exraise` on the sim | clock folds at the anchor floor at or below 5 percent of the folds (control 63, update off 0); anchor trips at or below the control's plus 1 (control 1; the kappa = 1 arm 2 on the sim, 8 on the field); post-fit re-floats at or below the control's plus 10 percent; the W5 gate sees the declared null directions without the card 1f restore; the exact-update line shows terms updated on every kept epoch |
| G2 truth | `x_exraise` on the sim | datum-free level error rms across satellites at epochs 16 / 60 / 240 tracking sqrt(R/N) with R = 0.5 m2: at or below 0.25 / 0.15 / 0.08 m (control 1.17 / 1.14 / 1.15; the kappa = 1 arm 0.19 at 240); normalized error along u_s, averaged over satellites, between 0.5 and 2 from epoch 60; IF code-residual median spread in hour 2 at or below 0.10 m (control 0.97); `x_ex` alone reported against these bars (the inherited-P prediction of section 5 says it fails them) |
| G3 bound | `x_exraise` on both beds | the ratio P_L / (1 / I_L) at or above 1 for every satellite after the first harvest, the raise count reported per satellite; on the control the same ratio reported (expected far below 1) |
| G4 field | `x_exraise` on the field | IF code-residual median spread per satellite at or below 0.15 m in hour 2 (control 0.27; the re-base of card CLV 0.17); the anchor's trips and floor count as G1 |
| G5 seam | a warm resume of `x_exraise` from a control snapshot at 1 h, graded with a cold-consumer replay (`splice_wire.py`) | the ratio of G3 at or above 1 from the first epoch after the restore; the raise applied on restore counted |
| G6 hold | `x_exraise_h0` on the sim | the within-arc phase-row error against truth (PBH read 3) reported; the disposition of 9.4 recorded before any consumer number |
| G7 consumer | `x_exraise` with the hold disposition, both beds | sim hour 1 at or below 0.60 m and hours 2 to 6 not worse than 0.103 0.096 0.056 0.055 0.049 by 0.010; WTZZ00DEU not worse than 0.560 0.059 0.027 0.030 0.039 0.048 by 0.010 in any hour; the 16-site census (`census2h.py` on the 2 h wire) not worse than key `rf3gl2nb` at more sites than better |
| G8 cost | `x_exraise` against `x_g0` | `loop_ms` within 5 percent on the field |

A bar missed is a result, not a stop: the next arm changes one thing against it, and the card
records the reading that failed. G7 is not run before G6's disposition is written.

## 11. What this card does not explain, and what remains after it

- The Galileo E12 family split (8 m between the pinned pilot family and the free C1X family,
  CLV section 6.3): a pin and datum question of the second family, not a time-update question.
- A C1W user's 0.36 to 0.39 m against CODE on the baseline wire (CLV section 8.3): in part the
  definition of C1W minus C1C in CODE's own products.
- The fragile consumers QUIN00USA and ARHT00ATA (CLV2 section 6.5, card PBF): consumer-side.
- The local-local half of the carry (F3) and the phase-stiff birth variance of a new arc (RF-1
  design section 1.4): remaining inconsistencies on the station side, untouched by a border-side
  fix.
- Whether the phase-bias wander the hold hides is this leak: plausible, since a re-injected
  overconfident row is the textbook source of a random walk in carried ambiguities, and G6 is
  the test.

## 12. Pointers (verified on main `001e3da`)

- Code: `src/core/filter/NetworkCoupling.cpp`: `couplingQ` 64 to 79 (a clock term returns
  `q_clock`, line 66); the scalar kappa 321 to 328, the joint kappa 223 to 231, the departed
  variance 288 to 314, the gauge restore 81 to 120. `src/core/app/NetworkRunContext.cpp`: 50
  (`q_clock` from `filter.process_noise.sat_clock`), 119 (`fullcov_on`), 122 (`carry_cpl_tu`), 386
  (the Fix-A hold sigma, inert here); the dense fold 3714 to 3790 (the anchor at 3729, the
  diagonal set to var_eff); the harvest's `p_prev` 4287 to 4291; the dense stash 6380 to 6384 and
  6538 to 6551. `src/core/filter/NetworkClockAnchor.cpp`: 112 and 132 (a loose clock folds at
  `q_clock`). `src/core/filter/NetworkClockLevel.cpp`: 165 (per-epoch information), 823 to 831
  (its accumulation), 69 to 74 (the horizon decay). `src/core/filter/NetworkPbHold.cpp`: 145 to
  152 (the held value follows only the declared step).
- Config: `config/golden/golden_sim_refound.json`: 153 `sat_clock_q` 0.001; 290
  `shared_carry_full_cov`; 291 `carry_coupling_time_update`; 302 to 313 the anchor (q 2.9e-8,
  floor 1e-5); 941 `emit_pb_hold`.
- Tests: `tests/test_clock_level_carry.cpp:220`; `tests/test_network_estimator.cpp` 2790 to 2925
  (the card 1d fixture) and 2930 (the gate).
- Runs: `runs/_rap_ctu_{s,f}{0,1}/stdout.txt`, the "Card Q3" run-end line (folds at the floor,
  trips).

## 13. Results, 2026-09-28 (the implementing session)

Built on main `001e3da` plus this card's working tree, default off and byte-identical off. Final
exe `gnss_processor_ctu4.exe` (sha256 prefix `7d7765a11e842544`); the first arms ran on
`gnss_processor_ctu.exe` (`a5c46f1c49bce44e`) and `_ctu2` (`aa49a734260e0e38`), the bisection on
`_ctu3` (`4534d30b1d6568c3`). Every arm is 2 h from a cold start on
`config/golden/golden_{sim,field}_refound.json`, made with `scripts/rapid_arm.py`.

### 13.1 What was built

- `network.carry_coupling_exact_update` (section 9.1). New pure module
  `filter/NetworkCouplingExact` (`couplingExactFactor`, `couplingExactRow`,
  `couplingExactCross`). The rule is evaluated as m_new = M_S - F Q_S M_S + F P_S,D M_D with
  F = (P_SS + Q_S)^-1, so a small inflation moves a row by a small correction and q = 0 returns
  the harvested row exactly; the variance as m_new'g + g'z + (M_D'P_DD M_D - u'z), free of the
  cancellation of M'PM - v'Fv. The rows take the rule when they were harvested at the stash the
  dense block belongs to (2 of 214 635 rows on the sim, 60 of 202 091 on the field did not, and
  kept card 1d's kappa, counted). The time-update covariance between rows is dropped, across and
  within stations (item 3; see 13.4).
- One Q from the fold (item 1). The rows are built in PASS B, before the fold runs, so the
  inflation comes from a side-effect-free mirror of the dense fold's per-key disposition
  (`networkCouplingExactPrepare`, beside `networkInjectD2CarryPrior`; the anchor's decision
  through a new `clockAnchorFoldPeek`), and the fold checks every key against it. The only clause
  the mirror cannot see, a clock-datum handover, is counted separately.
- The clock gauge (an addition to item 4). The rank gate declares one null direction per
  constellation on this posture, the uniform satellite-clock shift, and in the stash's datum the
  pivot clock is pinned outside P. The exact rule moves the clock coefficients over S, so the
  row's clock-family sum changes and the row observes the declared gauge. The deficit of each row
  (the harvested clock coefficients over S and D minus the new ones over S) is moved onto the
  pinned pivot, which is the exact update written in the pivot-relative coordinates the datum
  uses; the row keeps its harvest identity along the gauge with no card 1f restore. The phase-bias
  and code-bias families are closed before the gate by the held datum and the code-bias rows, so
  they are updated as the rule gives and not restored.
- `network.clock_level_monitor` and `network.clock_level_bound_raise` (section 9.3), beside card
  CLV's seam (`networkClockLevelMonitor`). CLV's pair build is now a helper
  (`clockLevelBuildPairs`) shared with the re-base, which is unchanged. The monitor accumulates
  I_L without CLV's horizon decay or cap: the level has no process noise, and a decaying bound
  would make the raise a per-epoch level variance, the random walk CLV section 6 warns about.
  P_L = u'Pu / (u'u)^2 over the satellite's clock and phase-bias rows of the stashed block. Trace
  `logs/network_level_bound.csv`. The raise adds (1/I_L - P_L) u u' to the block and the carried
  variances where the ratio is below 1.
- Run-end lines for both knobs in `NetworkTeardown`; fingerprint tokens `carrycplexact`,
  `clmraise`; loader interlocks (exact requires the card 1d update and the dense carry and refuses
  card 1b; the monitor requires the dense carry and refuses `clock_level_carry`; the raise requires
  the monitor).

### 13.2 U0: the unit gate

`tests/test_coupling_exact_update.cpp`, suite `CouplingExactUpdateTest`, 8 cases: q = 0 returns
the raw row to 1e-13; a diagonal P is card 1d's kappa; the inflation and the between-row terms
equal a brute-force M'BM with D routed as departed and as reinit; the pivot deficit keeps the
clock sum; the anchor peek equals the fold in every state the fold distinguishes and writes
nothing. The network fixture (three satellites with a clock and two phase biases, three stations,
two bands, free slant ionosphere per line of sight and epoch, station 0's ambiguities held, pivot
clock pinned, the anchor's walk, 60 epochs): the reference recursion (one joint local block)
equals the batch on every border value and covariance to 1e-8, with a clock decoupled at epoch 30
and with a satellite departed for ten epochs; the ambiguities agree to 1.1e-8 m (a common
per-station L2 offset, a code-determined direction where sixty inversions accumulate rounding),
so they are held to 1e-7. The card 1d form at the legacy clock rate claims the level at 2.6 mm
against the batch's 94 mm (variance ratio 7.7e-4) and sits up to 45 of its own sigmas from the
batch. The per-station form, the producer's, holds the level within 0.4 mm of the batch with a
variance ratio of 1.0000: section 4's claim that the dropped cross-station term leaves the level
untouched, measured. Full Debug suite 2680 passed, 1 fixture-gated skip.

### 13.3 The gates

| gate | reading | verdict |
| --- | --- | --- |
| U0 | 8 of 8; suite green | PASS (docs and golden verify at the record) |
| G0 | `_rap_y_g0_{s,f}` identical to `_rf0hv11_{sim,field}6` over 240 epochs (products, closed-loop trace, epoch status); monitor alone identical to off on both beds (`_rap_x_mon_*` vs `_rap_x_g0_*`), exact plus monitor identical to exact | PASS |
| G1 | sim: floor folds 62.8 % to 0.0 %; trips 1 to 4; RAIM-attrib re-floats 0 to 3 epochs / 6 ambiguities; 0 gate withholds; the fold check 0 of 37 581 keys mismatched (largest dQ 0), 0 orphaned deficits. Field: floor 44.3 % to 0.0 %; trips 1 to 12; re-floats 16 epochs / 28 ambiguities to 10 / 31 (bar 30.8) | FAIL on the trips (both beds) and the re-floats (sim; field by one ambiguity) |
| G2 | sim datum-free level at epochs 16 / 60 / 240: 0.68 / 0.31 / 0.28 m (control 1.17 / 1.14 / 1.15; update off 0.66 / 0.27 / 0.19); IF code-residual spread in hour 2 0.13 m (control 0.97); normalized error along the level about 185 at epoch 240 | FAIL |
| G3 | ratio P_L / (1/I_L): sim min 2.78, median 4.92; field min 0.31, median 3.13, 7 % below 1. Control: sim median 2.23 (10 % below 1), field median 0.41 (75 % below 1) | reported; see 13.4 (f) |
| G4 | field IF code-residual spread in hour 2: 0.28 m (control 0.27, update off 0.17) | FAIL |
| G5 | not run (the raise never fires on a cold start, 13.4 (f)) | open |
| G6 | 13.4 (h) | recorded: the wander remains |
| G7 | not run as a grade; the 2 h consumer is in 13.4 (h) for the record | not run |
| G8 | wall time +5.3 % sim, +5.1 % field (the section-timing trace is not armed on the golden) | borderline |

### 13.4 Readings

(a) The mirror is exact on both beds (0 disposition and 0 inflation mismatches over every checked
key, no handover in either window), so the rows did use the fold's own Q.

(b) Without the pivot deficit the rows observe the declared clock gauge and the gate would count
it; with it, 0 gate withholds and 0 orphans on both beds.

(c) The within-station time-update covariance drove a drift. The first build added the rows'
M_i'BM_k to each station's jointly whitened block, beyond item 3's per-row form. On the sim the
level converged to 0.31 m at epoch 60 and then drifted to 0.97 m at 240 in smooth per-satellite
ramps of 1 to 2.5 m over the second hour (G28 -0.33 to -2.47 m, G10 -0.11 to -1.92 m), with the
IF code-residual spread at 0.77 m. Bisection, sim, one change each (`runs/_rap_b_*_s`):

| variant | level @ 60 / 120 / 240 (m) | IF spread | SBC completions | re-floats |
| --- | --- | --- | --- | --- |
| exact with the within-station terms (`_rap_x_exmon_s`) | 0.31 / 0.65 / 0.97 | 0.77 | 253 | 14 / 70 |
| per-row form (`_rap_b_nocross_s`, the shipped form) | 0.31 / 0.27 / 0.28 | 0.13 | 77 | 3 / 6 |
| per-row, own columns only (`_rap_b_local_s`) | 0.35 / 0.30 / 0.31 | 0.20 | 1 | 3 / 6 |
| no inflation (`_rap_b_noinfl_s`) | 0.39 / 1.21 / 1.81 | 1.47 | 152 | 23 / 110 |
| walk on the clocks only (`_rap_b_qclk_s`) | identical to the first row | | | |

The within-station terms were removed; `couplingExactCross` stays as the unit gate's joint
reference. The walk on the phase and code biases plays no part (last row).

(d) SBC completions (sim 0 to 77, field 19 to 101) read as a symptom. The S-basis completion
counts eigenvalues of the reduced normals below tol x lambda_max (tol 1e-9). The completed
direction is an L2W phase-bias common mode over a subset of satellites with an eigenvalue of
about 2e-4, below the relative cutoff, which puts lambda_max above 2.2e5 on those epochs (1.05e5
at epoch 1 on both arms; the estimability dump covers the first evaluations only, so the
control's later lambda_max and the direction's eigenvalue there were not measured). The pin's
weight, 1e3, is far softer than the phase-bias carry, and the per-row form converges with 77
completions as well as the own-columns form does with 1 (13.4 (c)).

(e) The ignition is common to every arm and precedes the time update. The datum-free sim level
over epochs 1 to 7 is 0.69 0.69 1.00 1.30 1.50 1.30 1.19 m in the control, the update-off arm
and the exact arm alike; the arms part from epoch 8 (the control freezes at 1.17 m, the other two
decay). The code alone predicts 0.35 m at epoch 4. The G2 bars at epochs 16 and 60 measure this
damage more than the recursion.

(f) The bound of section 5 is not a lower bound here, and the raise never fires on a cold start.
P_L >= 1/I_L holds when the slant ionosphere is free every epoch. With the ambiguities carried
the ionosphere has memory through them, each code frequency then measures the level, and the
IF-only information understates what a consistent estimator may hold by up to about 18 times
(the IF noise amplification). Measured on the sim: the exact arm's ratio never falls below 2.78,
while its level error is about 185 times its claimed variance at epoch 240 (13 times the claimed
sigma); the control reads a median of 2.23 while its level sits 1.2 m off with a claimed 2.5 cm.
The freeze and the remaining inconsistency are in the mean, and the ratio does not see them; on
the sim the normalized error against truth does (`levelnees.py` of the session scratchpad: the
pivot-frame level error per satellite against the monitor's P_L). The section 5 mechanism
(an overconfident inherited P) is not what limits a cold start: the raise has nothing to raise
(0 raises on either bed), so `x_exraise` is `x_exmon`.

(g) The anchor guard trips on the level's own motion. The Q3 guard models a clock walk of about
1 mm per 30 s; a level converging at centimetres per epoch reads as a fault. Trips rise from 1 to 4
(sim) and 1 to 12 (field), each a decouple and a re-seed. On the field the exact arm's level
leaves by 1.8 to 2.2 m at epochs 8 to 16 (the control holds 0.30) before it returns, and the
IF code-residual spread ends at the control's (0.28 against 0.27). This is the leading suspect
for the field.

(h) The hold (G6, section 9.4). `scripts/ftc_step_attrib.py --r3` against the sim truth,
consumer-tracked satellites, difference-median de-commoning, within-arc RMS, the arm's second
hour: exact build with the hold off (`_rap_y_exh0_s`) e_phi 0.0224, e_clk 0.0123, e_pb 0.0175 m;
the control with the hold off (`_rap_y_h0_s`) e_phi 0.0214, e_clk 0.0023, e_pb 0.0242 m. The
phase-bias wander (about 2 cm) is not the leak this card removes: it is present at the same size
with and without the exact update. By 9.4 the hold's generation semantics stay and the wander is
a remaining inconsistency. Consumer p50 by hour, for the record only (G7 is not graded): sim
control 0.60 / 0.10, exact with the hold 3.02 / 0.90, exact with the hold off 1.17 / 0.65, control
with the hold off 1.95 / 0.83 m; field control 0.56 / 0.06, exact with the hold 10.42 / 3.03 m.

### 13.5 Disposition

Built, default off, not armed; no golden, ledger or posture change. The exact update removes the
mechanism section 2 names (floor folds 63 % to 0 %, the level leaves the ignition value on the
sim, 1.15 to 0.28 m at 2 h, the IF code spread 0.97 to 0.13 m), but it does not give a converged,
consistent level: G1, G2 and G4 fail. What stands between it and the gates, in order:

1. The ignition damage of 13.4 (e), common to every arm (epochs 2 to 5 double the single-epoch
   level error).
2. The anchor guard and the level direction (13.4 (g)): the guard's statistic should be formed in
   the phase-visible complement, where the clock walk lives, not on a clock that is allowed to
   converge at code rate.
3. A truth-free level instrument that holds with carried ambiguities (13.4 (f)); until then the
   sim's normalized error against truth is the grade, and the field has no level grade.
4. G2, G4 and G5 re-run after 1 and 2; the hold read on a 6 h hold-off arm (hours 2 to 6), then
   G7.

Session scratchpad graders: `ctu_g1.py` (G1 from the run-end lines), `levelpath.py`
(per-satellite datum-free level paths), `levelnees.py` (the pivot-frame normalized error),
`ident2h.py` (G0).

## 14. Follow-up, 2026-09-29

The section 13.5 follow-up (the ignition, the guard on the level, the level instrument, G2, G4 and
G5 again) is in card [CTU2](CTU2_level_recursion_followup_2026-09-29.md), because this file reached
the size bar. In brief: the sim's ignition damage is a fixture artifact (orbit extrapolation at the
24 h fixture's start); the receiver code-bias carry counted the border twice and is fixed in the
block; the monitor carries a truth-free level instrument; the guard has a level-blind phase
statistic; the field's first-fold jump was a drop that did not decouple. On a clean sim start G1 and
G2 pass; G4 misses by 1.7 cm on the field; G5 fails, held by the restored ambiguities; the consumer
is worse with the exact update under every hold setting.

## 15. Annotation (2026-10-01): armed on the goldens by card EXL

The exact update (`network.carry_coupling_exact_update`) with the companions of card CTU2 section 7
(`carry_rx_code_bias_in_block`, `amb_float_drop_decouple_always`, `clock_anchor_guard_space` phase,
`rank_gate_tol` 1e-11) is armed on the goldens together with card LAH's phase-clock hold, the
author's decision after card AIL ([EXL](EXL_exact_update_lah_promotion_2026-10-01.md)). G4 and G8 of
section 10 were re-stated relative to the golden (field IF spread 0.164 against 0.266 m; epoch time
+8.2 % field, +4.5 % sim) and G5 is disclosed (fresh convergence only). The `dense_fold_kappa`
default these sections quote as 1e-6 has been 1e-12 since card SMH section 9.

## 16. Annotation (2026-10-02): the mirror retired by card PRM phase 1b

The mirror of section 13.1 (`networkCouplingExactPrepare`'s clause chain and `clockAnchorFoldPeek`)
is replaced by the fold's own decision, made once before PASS B (`networkCarryDecide`) and read by
the rows and by the fold ([PRM](PRM_parameter_registry_2026-09-29.md) section 15). Before its
removal the decision matched the mirror and the fold bit for bit on every key of ten same-build
arms, the 6 h goldens across the clock-pivot handover included, and the run is byte-identical with
it removed. The handover clause the mirror could not see is the S-transform: its 31 inflation
mismatches at TOW 276150 are the 31 re-expressed GPS clocks, with the largest |dQ| equal to the
transform's `var_q`. The run-end fold line now counts the keys of S a handover re-expressed or
retired after the rows were built; those rows are still not conditioned on the re-expressed
variance (owed to card PRM phase 2).
