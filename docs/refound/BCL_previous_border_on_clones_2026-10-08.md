# BCL: the whole previous border on clones (the phase and code biases, and the departed keys)

**STATUS**

- **id:** BCL
- **topic:** network-vehicle
- **status:** parked
- **summary:** Card SCL's clock clone extended to the biases (BCL-a) and to every key card CTU re-attributes (BCL-d); built default off, byte-identical off. BCL-a is nil (the phase biases never pass the screen; the code biases do and change nothing) at +40 % wall time. BCL-d is exact at the event epochs but FAILED G2 on the held-out consumer (+15 / +17 mm in hour 2); the 16-station census is neutral. Nothing armed.
- **source:** [SCL](SCL_self_contained_clock_level_2026-10-03.md) sections 4, 4.1 and 5 (B1, the clock clone); [CTU](CTU_coupling_time_update_2026-09-28.md) sections 4 and 9.1 (the per-row rule and its D routes); [SCL2](SCL2_level_residue_under_the_clone_2026-10-06.md) sections 9 to 11 (X2, the harvest's residue); [PFQ](PFQ_pivot_frame_process_noise_2026-10-08.md) sections 6 and 7 (the joint walk factor; this card is section 7's owed item, requested by the author on 2026-10-08)
- **knobs / posture:** new, default off, fingerprinted (armed-only tokens), solve-affecting: `network.carry_coupling_bias_clone` (BCL-a) and `network.carry_coupling_departed_clone` (BCL-d); each requires `network.carry_coupling_clock_clone`. Arms on card SCL2's clone recipe with card PFQ armed (`runs/_rap_pfq_clp_{wk,bc}_s` are the controls). Nothing armed on the goldens.
- **dates in the text:** 2026-10-08
- **hashes / run dirs:** control exe `gnss_processor_pfq5.exe` (`c669c9b0f1f75c80`); `gnss_processor_bcl1.exe` (`ef8e75f269ca3b11`, the build graded), `gnss_processor_bcl2attrib.exe` (`97026934ff35e85a`, a temporary route switch, removed from the source after its build); arms `runs/_rap_bcl_*` (2 h; smoke `runs/_rap_bcl_smoke_s`), census replays `runs/_rap_bsc_*`; unit gate `tests/test_border_clone.cpp` (suite `BorderCloneTest`)
- **gates (frozen at pre-registration):** U0, G0 to G5 in section 4

**Outcome.** Question: does carrying the whole previous border on clones, the biases (BCL-a) and the keys card CTU re-attributes (BCL-d), change anything? BCL-a is nil, as predicted: the phase biases never pass the walk screen on the float vehicle and the code biases' walk is negligible, at 40 % more wall time. BCL-d is exact at the event epochs (U0) but the held-out consumer loses 15 to 17 mm in hour 2 after a satellite's eclipse exclusion, so G2 FAILED; the 16-station census is neutral (section 5). Both default off, nothing armed (section 6).

## 1. The question

Card SCL's clone (B1) carries the previous epoch's satellite clocks as border states for one epoch: the coupling rows keep their harvested clock coefficients on the clones, and each clock's walk enters the border prior once. Two parts of the stashed border are still on card CTU's per-row rule, which re-expresses each row on the moved border separately and drops the time-update covariance between rows:

1. **The walking bias keys (S).** The satellite phase and code biases fold with a walk of 1e-12 and 1e-10 m2/s (3e-11 and 3e-9 m2 per 30 s epoch). Their stashed variances are small (the phase biases near 1e-9 m2), so the walk is a few percent of the stated variance per epoch, but in absolute terms it accumulates to about 0.06 mm (phase) and 0.6 mm (code) per hour.
2. **The keys the fold does not carry (D).** A departed key (its column absent this epoch), a reinit, a re-referenced phase-bias frame and a skipped key (held, parked, screened) are re-attributed per row through the stashed block, and the conditional variance of the departed part is added to each row on its own. On the walk bed over 2 h that route took 13 964 departed and 5 596 re-referenced terms against 33.1 million updated in S (`runs/_rap_pfq_cl_wk_s`, the card CTU run-end line).

With both on clones, card CTU's update becomes the identity on every stashed term with a carried entry: each row keeps its harvested coefficients, and every time-update effect is in the border prior (the stashed block over the clones, jointly, plus the walk factors). What remains approximate is the harvest's per-station marginalization of the clones, which card SCL2's shared history compresses.

The question: does the whole previous border on clones change anything measurable, and at what cost?

**Prediction (written before any code).** Steady state nil for both halves: the bias walks are too small to matter in absolute terms, and the D route is about 0.06 % of the terms. Any effect of BCL-d is at the event epochs (a satellite's setting or return, a phase-datum re-reference, a reinit), and B1's per-station harvest returns most of the dropped term one epoch later. BCL-a costs about 120 border columns on the GPS dual-frequency posture (a phase and a code bias per band per satellite), and with them a larger per-station Schur update.

## 2. The change

**BCL-a, `network.carry_coupling_bias_clone`.** Each satellite phase bias and code bias with a row in the stashed dense block and a carried entry gets a clone column, registered with B1's clocks after every other column (key `sat|PBC|sig` or `sat|CBC|sig`, label `PBC:` or `CBC:`, so no family classifier reads it as a bias). In the rows' epoch a bias key in S whose walk passes B1's screen (positive, above 1e-9 of its stashed variance, not a seam key) moves onto its clone at Q = 0, exactly as B1 moves a clock. At the fold its entry in the stashed block becomes the clone (mean the stashed value, variance the stashed variance) and its walk b(t) - b(t-1) folds at the fold's own variance, in the same joint walk factor as the clocks (`NetworkEstimator::addCloneWalkJoint`; card PFQ's pairs stay between clocks only). Card SMH's rows advance on the stood-in bias clones as they do on the clocks'.

**BCL-d, `network.carry_coupling_departed_clone`.** Every stashed key with a carried entry gets a clone column (a clock through B1's key) when it is on a D route or may be: the keys whose column is absent, the reinit-marked clocks B1 skips, and, with BCL-a off, the bias keys. In the rows' epoch every D key with a registered clone moves into S on its clone at Q = 0, so no term is re-attributed. At the fold each such clone takes the stashed block's row and column as a time t-1 state (covariance with a stood-in clone from the block before any handover transform, with a current key the same, and zero with a reinit key) and no walk factor: the link between the departed value and any current column is the one the per-row rule already assumed absent. A clone that B1 or BCL-a moved in the rows' epoch but whose key does not fold as decided (retired by a clock-pivot handover, screened, re-expressed after the rows were built) takes the same prior instead of being left free. A joint fold that does not factor with the D clones is retried without them (counted), and those clones are then free as under B1.

Nothing else changes. The rank gate and the S-basis completion read the clones as nuisance (B1's gate view), the harvest marginalizes them through the posterior regression (a bias clone takes no part in the clock-family deficit), card SCL2's history compresses them with B1's clones (its width cap stays k per cloned clock), and a clone is never stashed, snapshotted or emitted. Either knob off keeps every path byte-identical.

## 3. What is not covered

Card SMH's rows keep card CTU's re-attribution for D keys (their S keys follow BCL-a); the refloat of card Q3 is unchanged (it frees clocks only); a seam key stays on its own column; a D clone of a clock in a constellation whose dense handover transform failed its self-check takes the block before the transform, which that epoch's fold does not use (counted).

## 4. Gates (frozen)

- **U0 (unit, written and passing before the arms).** (a) A bias clone registers last with its key and label, idempotently, and is never emitted. (b) Priors fold by identity onto a bias clone; a bias walk factor is the information form of b(t) - b(t-1) and withdraws exactly; a joint factor over a clock and a bias walk equals A' Qw^-1 A. (c) The departed clone is exact: two rows sharing a departed key, folded on the D clone with the stashed block, give the batch posterior to 1e-9 relative, while card CTU's per-row re-attribution of the same rows does not (its cross-row term is dropped). (d) The gate view marginalizes a referenced bias clone and keeps the declared gauge null. (e) Both knobs load default off and refuse to load without B1.
- **G0 (off identity).** Both knobs off, the new exe against `gnss_processor_pfq5.exe` over 2 h: products, closed-loop trace and epoch status byte-identical on the sim and field goldens and on the clone + PFQ walk-bed arm.
- **G1 (liveness, invariant 4).** BCL-a: bias clones stood in and bias walk factors folded on every fold epoch; BCL-d: D clones by route above zero, card CTU's re-attributed terms at most 1 % of the control's, free D clones and fallbacks counted.
- **G2 (do no harm, primary).** Walk bed and broadcast bed, 2 h, each knob alone and both against the clone + PFQ control: the consumer 3D p50 in hour 2 not worse by more than 5 mm; the de-commoned level in hour 2 not higher by more than 10 %; the pivot-frame level in hour 2 not worse by more than 5 mm; emitted epochs equal; anchor trips not doubled.
- **G3 (the prediction, report).** BCL-a moves every hour-2 read by at most 2 mm; BCL-d moves the hourly reads by at most 2 mm and card CTU's per-row inflation mass by at least its D-route share.
- **G4 (cost, report).** Wall time of each arm against the control, under the same load.
- **G5 (with card SCL2's shared history, report).** The three arms repeated with `network.carry_coupling_shared_history`, against `runs/_rap_pfq_x2p_{wk,bc}_s`.

<!-- Results are appended below this line under dated headings. -->

## 5. Result (2026-10-08, `gnss_processor_bcl1.exe`, 2 h, the sim from 00:30:15)

**The build.** `NetworkEstimator::registerClone` / `cloneColumn` (a bias clone keyed `sat|PBC|sig` or `sat|CBC|sig`, labelled `PBC:` or `CBC:`; a clock clone is card SCL's), `CloneWalk::kind` and `signal` (the walk factors take any kind; card PFQ's pairs and card Q3's refloat stay with clock walks), the two knobs with their loader interlock, fingerprint tokens and manifest entries, and in `filter/NetworkClockClone` the registration, the rows' epoch, the stand-ins, the time t-1 priors, the end-of-fold isolation and the harvest's gauge membership, plus card SMH's bias stand-ins (`NetworkPbHoldClone.cpp`). The gate view now forms whenever any clone is registered (it returned early without a clock clone). A clone that falls from the stand-ins takes the time t-1 prior only when its key does not fold on its own column (retired, screened) or folds decorrelated (a reinit): with the key's own row in the block, c(t) and c(t-1) would differ only by the walk, and a walk at or below the screen makes the block singular. Tests `tests/test_border_clone.cpp` (suite `BorderCloneTest`, 5 cases).

| gate | reading | verdict |
| --- | --- | --- |
| U0 | 5 of 5. (c): two rows sharing a departed key, folded on its clone, give the batch posterior of the continuing clock to 1e-9 (value 0.111770, variance 2.2374e-2); card CTU's per-row re-attribution gives 0.113398 and 2.6667e-2, and with its dropped cross-row term (5.1e-3 m2) restored it is the batch again. Full Debug suite 2879, 2878 passed, 1 fixture skip | PASS |
| G0 | both knobs off, `gnss_processor_bcl1.exe` against `gnss_processor_pfq5.exe`: products, closed-loop trace and epoch status byte-identical on the sim golden (`bcl_g0{a,b}_s`), the field golden (`bcl_g0{a,b}_f`) and the clone + PFQ arms of both beds (`bcl_g0c_{wk,bc}` against `pfq_clp_{wk,bc}_s`) | PASS |
| G1, BCL-a | 240 of 240 epochs, 121.5 clones registered per epoch; 60.7 bias keys per epoch stand in with a walk, 60.7 are screened, 0 moved | PASS (see the reading) |
| G1, BCL-d | 21 keys on their clones over 7 epochs (15 departed, 6 re-referenced), 0 without a clone, 0 rebuilt or retried; card CTU's re-attributed terms 19 558 to 0; its per-row inflation mass 110.4 to 0.003 m2 | PASS |
| G2, BCL-a | walk / broadcast, hour 2: consumer 0.044 to 0.043 / 0.060 to 0.059 m; de-commoned 0.047 to 0.050 (+6 %) / 0.040 to 0.041; pivot frame 0.049 to 0.051 / 0.045 to 0.046 | PASS |
| G2, BCL-d | consumer 0.044 to 0.059 / 0.060 to 0.077 m (+15 / +17 mm, bar 5); de-commoned 0.047 to 0.049 / 0.040 to 0.040; pivot frame 0.049 to 0.050 / 0.045 to 0.044 | FAIL |
| G2, both | consumer 0.044 to 0.057 / 0.060 to 0.075 m; de-commoned 0.047 to 0.052 (+11 %, bar 10 %) / 0.040 to 0.042 | FAIL |
| G3 | BCL-a: one read moved by 3 mm (the walk bed's de-commoned level), the rest by at most 2 mm. BCL-d: the consumer moved by 15 to 17 mm in hour 2 (hour 1: 0.203 to 0.173 on the walk bed, 0.124 to 0.163 on the broadcast bed); the inflation mass left card CTU's update entirely, and the harvest's added conditional variance rose 23.2 to 146.7 m2 | missed for both |
| G4 | one batch of six concurrent runs: control 190 / 155 s, BCL-a 261 / 223 s (+37 / +44 %), BCL-d 191 / 149 s, both 262 / 222 s | report |
| G5 | with X2, against `pfq_x2p_{wk,bc}_s`, hour 1 / 2 consumer: BCL-a 0.209 / 0.071 (control 0.212 / 0.071) and 0.110 / 0.094 (0.106 / 0.089); BCL-d 0.175 / 0.084 and 0.179 / 0.110; both 0.175 / 0.083 and 0.180 / 0.112; every level read within 1 mm of its control | report |

**Attribution (post hoc, not pre-registered).** A temporary switch put BCL-d on one route only (`gnss_processor_bcl2attrib.exe`, `97026934ff35e85a`, environment variable `BCL_ATTRIB_ROUTE`; the unset control `bcl_dctl_wk` reproduced `bcl_d_wk` byte for byte; the switch was removed from the source after its build). The departed route alone (`bcl_d1_{wk,bc}`) carries the consumer change: 0.170 / 0.053 and 0.162 / 0.074 m by hour. The re-referenced route alone (`bcl_d3_{wk,bc}`) is neutral: 0.208 / 0.046 and 0.123 / 0.061. The departed keys are three satellites' eclipse exclusions (G20 at TOW 262230, G12 at 264600, G25 at 267570: each leaves the layout with its clock, two phase biases and two code biases), and the held-out consumer first moves at G20's. A 2 h sim census of the BCL-d wire against the control wire on each bed (16 in-network consumers replaying from the wire's first epoch, the wire's own arm as the base; tags `bsc_<station>_{c0wk,dwk,c0bc,dbc}`): walk bed, median hour 1 -0.0037 m and hour 2 +0.0002 m, no station worse by more than 0.02 m, one better; broadcast bed -0.0026 and +0.0014 m, one station worse in hour 2 (S09, +0.023 m), one better in hour 1.

**Reading.**

1. BCL-a is in effect a code-bias clone on this posture. The phase biases never pass card SCL's screen: on the float vehicle their carried variance holds the ambiguity level (tens of m2 at the start, far above 0.03 m2 throughout), so their 3e-11 m2 walk is below 1e-9 of it and the per-row update is exact to that order. The code biases (carried variance about 2e-5 m2) stand in, and their 3e-9 m2 walk changes nothing measurable. The prediction of section 1 holds: nil effect, at 40 % more wall time.
2. BCL-d does what U0 says at the epochs where card CTU re-attributes: the rows keep their terms, and the epoch is exact. The per-station harvest then marginalizes the clones one epoch later, adding a conditional variance (+124 m2 over the run) of the size of what card CTU's update added (110 m2), as section 1 predicted. The held-out consumer's 15 to 17 mm in hour 2 starts at one eclipse exclusion and persists through the hold's generations; the in-network census on the same wires shows no systematic change on either bed. The pre-registered bar is the held-out consumer, so G2 stands FAILED.

## 6. Standing (2026-10-08)

- Both knobs stay in the tree default off, counted, fingerprinted and byte-identical off; nothing armed, no golden or ledger change.
- BCL-a is not recommended: on the float-vehicle posture it clones only the code biases and buys nothing for its cost.
- BCL-d failed G2 on one consumer and is neutral on the census. Before any arming decision it would need a read over more than one realization of the events: the 6 h census, or the field bed with its own eclipse exclusions. Not scheduled.
