# IRDE part 2: why A1 holds receivers' wet delay metres off (section 9 of card IRDE)

**STATUS**

- **id:** IRDE-part2
- **topic:** network-vehicle
- **status:** open
- **summary:** Section 9 of card IRDE (2026-11-07), moved here verbatim on 2026-11-07 because that file reached the 60 KB bar; the receiver floor as the cause of A1's wet-delay state and of most of the DCB spread.
- **source:** [IRDE](IRDE_integer_recovering_datum_slip_2026-11-06.md) sections 5.3 and 8 (the posture and the rows the question follows); the header and Outcome of that card summarize this section

Sections 1 to 8 are in [IRDE](IRDE_integer_recovering_datum_slip_2026-11-06.md). Results of this section are appended below.

## 9. 2026-11-07: why A1 holds receivers' wet delay metres off

A question relayed at the author's word after section 5.3: why does the `ird_tr` posture hold receivers' wet delay metres off, does the phase-bias floor cause it, is that a defect in the sim posture, the floor or the float solve, can the golden or the field bed reach it, and what does it imply for reading I-b under A1, for IRD-5b (D-N) and for the GF-datum defect (D0)? The 2026-11-02 ruling and every bar stand: nothing here reopens or changes them, no code changes, and what bears on the ruling is reported in section 9.8 for the author. The arms are one knob each on A1krhRs on `ird_tr` unless named, on the exe of section 8, `_rap_ird4z_*`.

### 9.1 The answer

1. **The floor causes it.** The receiver phase-bias floor is a zero-mean prior on a bias that the chain's frame makes tens of metres. Its ionosphere-free part is paid for by the wet delay, and the phase carries then hold the result for tens of minutes under a formal sigma of 2 cm. Its size is predicted from a floor-off arm alone with nothing fitted: at sigma 1 from arms already run, at 3 m and 10 m before those arms ran.
2. **The defect is in the floor as it sits on the chain.** The float solve does what its rows say, and nothing in the sim posture is specific to it.
3. **Neither golden reaches it,** because the floor's weight needs `phase_datum_eq34`, off in both. **The field does with the chain on:** an archived arm reads wet delay up to +29.3 m at a formal 2 cm. No field arm has the floor off, so the floor is not proved the cause there.
4. **Under A1, I-b reads this state.** With the floor at 3 m, at 10 m or off, `ird_tr` passes I-b at the slip epoch. The undeclared satellite steps are the wet-delay differences of the receivers above them.
5. **The GF-datum defect is the same prior's other half:** the receivers' DCB moves one for one against the bias's geometry-free part. **D-N's trigger** does not fire with the rows armed, but that NEES cannot see the wet-delay state.

### 9.2 The mechanism

- **The frame.** Tree edges pinned at zero targets at birth put integer path sums into the biases. With the floor off the receivers' ionosphere-free bias has a median |value| of 20.0 m at the first epoch (sd 28 m, 39 m by hour 2). Its two bands behave as independent path sums: corr(GF, IF) 0.973, slope 0.463, where two independent bands of equal variance give 0.971 and 0.461.
- **The floor.** `network.phase_datum_eq34_rpb_prior_sigma_m`, 1 m per band, folded on every non-root receiver's bias each epoch. Its weight needs the chain (`src/core/network/driver/Knobs.cpp:343`).
- **The ionosphere-free part moves the wet delay.** With the slant ionosphere free, the two per-band priors act on the ionosphere-free bias as a prior of weight 0.1127 / sigma^2 (2.98 m at sigma 1). At a birth the arcs are free and T is known through code alone (median formal 0.97 m). Code holds dt + mbar T and the tree edges are pinned, so a shift dT_r moves the biases below r by (m^s - mbar) dT_r. T is the cheapest free variable that relieves the prior. Minimizing the sum of dT^2 / s_T^2 and W (delta0 + d delta)^2 gives the shift (`floor_predict.py`).
- **The geometry-free part moves the DCB.** On a tree edge code and phase fix only the difference of the receiver's geometry-free phase bias and its DCB (the slant ionosphere is free and the ambiguities are pinned). With the floor off the DCB stays near 0 and the difference sits in the bias. The floor forces the bias to the min-norm split of its ionosphere-free value (corr(GF, IF) 1.000, slope 0.462 against 0.461), and the rest goes to the DCB.
- **A release repeats it.** A release leaves T to code again (formal 1.3 m) with the floor acting. S45000SIM at the re-host of TOW 262830 goes from -3.61 m to +11.33 m in two epochs, and with the floor off from +0.03 m to +1.60 m. As its arcs regain precision the floor-off error is +0.10 m at TOW 263430 and the floor-on error +3.32 m. A cold re-host at TOW 263640 starts again, and by TOW 264630 it reads +8.15 m at a formal 0.02 m (floor off -0.30 m).

### 9.3 The tests

Predictions were written before the arms ran. The first-epoch shift of T against sigma 0 was computed from a floor-off arm with nothing fitted: from A0fkrhRs on a slip-free bed before the run, and recomputed from sigma 0 after it, with the same numbers.

| arm | first-epoch shift, rms, max, m (predicted) | error from 15 min, p50, p90, max, m | per-epoch step p95, p99, m | DCB sd, h1, h2, m | I-b at the slip epoch: WL max, pairs; integer | consumer h1, h2, m |
| --- | --- | --- | --- | --- | --- | --- |
| sigma 1 (A1krhRs) | 3.59, 19.76 (3.52, 17.07) | 0.371, 2.02, 12.5 | 0.373, 1.85 | 9.12, 9.49 | FAILED: 0.385, 86; 146 | 0.186, 0.118 |
| sigma 3 | 0.73, 4.74 (0.62, 3.62) | 0.038, 0.182, 3.30 | 0.166, 1.18 | 3.30, 3.32 | PASS: 0.026; 0 | 0.222, 0.030 |
| sigma 10 | 0.09, 0.51 (0.06, 0.38) | 0.025, 0.156, 3.76 | 0.175, 1.22 | 2.93, 2.93 | PASS: 0.026; 0 | 0.269, 0.033 |
| sigma 100 | 0.003, 0.016 | 0.025, 0.166, 4.21 | 0.180, 1.26 | 0.75, 0.76 | PASS | 0.277, 0.031 |
| sigma 0 | 0 | 0.024, 0.166, 4.22 | 0.180, 1.26 | 0.67, 0.70 | PASS: 0.026; 0 | 0.277, 0.031 |
| birth only | 3.59, 19.76 | 0.185, 1.14, 8.74 | 0.286, 1.59 | 6.58, 6.43 | FAILED: 0.071, 14; 90 | 0.131, 0.036 |
| seed round | 0.15, 0.44 | 0.217, 7.14, 421 | 1.48, 5.99 | 1.72, 4.28 | FAILED: 0.446, 118; 164 | 0.278, 0.637 |
| `carry_zwd` | 3.59, 19.76 | 0.674, 2.88, 8.29 | 0.107, 0.954 | 8.16, 7.94 | FAILED: 0.124, 84; 29 | 0.187, 0.032 |
| code std 0.6 m | 6.77, 26.11 (8.79, 42.73) | 0.425, 3.37, 23.5 | 0.724, 2.88 | 26.0, 26.8 | FAILED: 0.238, 151; 103 | 0.361, 0.032 |
| code std 0.15 m | 1.49, 9.42 (1.24, 6.91) | 0.184, 0.789, 4.83 | 0.214, 1.30 | 4.81, 4.79 | PASS: 0.025; 0 | 0.154, 0.056 |

The code-std rows are read against sigma 0 at the default code weight, with s_T scaled in the prediction, and those two predictions were made after the arms. All 240 epochs are emitted on every arm.

| prediction | verdict |
| --- | --- |
| P1, the floor sets the first-epoch shift; falsified above 1.2 m rms at sigma 3 or a correlation below 0.7 | PASS at sigma 1 and 3 (correlation 0.875 and 0.735, permutation null p95 0.569 and 0.664). At sigma 10 the size holds (0.09 against 0.06 m) and the correlation misses, 0.646 against a null p95 of 0.697: the test has no power at that size. The microscope twin, a separate check: rms 0.44 against 0.41 m, correlation 0.950, null p95 0.323 |
| P2, the floor sets the whole run; sigma 10 near sigma 0, max below 2 m after hour 1 | The trend holds at every survey epoch within 1 mm. The max clause FAILED: sigma 10 reads 3.76 m at TOW 267570 on S14000SIM, where sigma 0 reads 4.22 m, a release leaving T to code (after hour 1, p99 0.778 against 0.794 m; sigma 1, 6.05 m) |
| P3, birth only keeps the first-epoch shift and later errors | PASS: identical first epoch; after hour 1 p50 0.139, max 6.35 m |
| P4, seed round has no tension at the first epoch | PASS: 0.15 m rms with the floor on. From hour 1 T reaches 421 m and the RPB NEES 76.9, as decision D-S recorded (IRDB section 3.5); not traced here |
| P5, a carried wet delay keeps the first epoch and shrinks the jumps | PASS: identical first epoch, step p95 0.373 to 0.107. The error then stops decaying (p50 0.63 to 0.67 m to the end of the run) |
| P6, the family is not the cause | PASS: the T trace of the single-tree family with the floor on is identical to A1krhRs's, and A1 with the floor off to A0fkrhRs's (the pair family reproduces A0 where both bands track, IRDC section 5) |
| P7, only code removes the error: the fitted decay rate scales with I_code | FAILED as stated: g0 0.0175 per epoch moves to 0.0148 (code std 0.6 m) and 0.0296 (0.15 m), a factor under 2. The reasoning was wrong: the arcs' carried variances come from code too, so the gain I_code / (I_code + I_arc) barely moves (0.041, 0.049, 0.055) |
| P8, I-b passes at the slip epoch with the floor off and at 10 m | PASS, and at 3 m |

One knob excludes another remover: `filter.initial_std.troposphere` 0.5 to 5 m leaves the products and every network trace byte-identical (only the consumer's traces move), so T has no prior in the producer.

### 9.4 What holds the error, and what stays unexplained

- **Established.** After a birth the error sits under a stiff solve: S45000SIM stands at +8.15 m with a formal 0.02 m. Code asks for the whole correction: on that receiver the slope of its ionosphere-free code residuals on the wet mapping is -1.08 times the error (correlation -0.894) and its phase rows' slope is near 0. From TOW 264000 on, 22.1 % of code rows stand above 3 sigma and 7.6 % above 5 (floor off 0.3 % and 0).
- **Falsified: frozen arcs.** A model in which each arc keeps the share it baked (`lockin.py`, geometry alone) tracks S45000SIM for about half an hour (+8.03 m measured, +8.71 predicted, 25 min on) and then diverges (+3.92, +8.92 at 53 min). Over 50 min the arcs' ionosphere-free values move rms 4.54 m (floor off 0.44), and after satellite and station effects 0.81 m (floor off 0.069), at a formal 0.19 m. G18 at S45000SIM moves -11.7 m.
- **Falsified: each arc pulled by its own code row.** Arc steps against the arc's own code residual correlate at -0.06 (S45000SIM) and 0.06 (six receivers; null p95 0.018), at 0.03 to 0.05 of the predicted Kalman gain.
- **Fitted, not proved: a collective mode.** The error decays at g0 0.0175 per epoch, a time constant of 57 epochs (29 min), over the quiet epochs of 55 receivers. The step correlates at 0.37 with the prediction -g e, with g = I_code / (I_code + I_arc) (null p95 0.035), but at 0.15 of its size, since I_arc does not separate the pinned edges. By elimination the remover is code: T has no prior and the phase rows do not know T. No arm isolates that (P7).
- **The floor's per-epoch fold works against the decay.** Birth only decays faster (g0 0.0270) and halves the error after hour 1 (p50 0.139 against 0.279 m).
- **The consumer follows the motion, not the level.** `carry_zwd` keeps the error (p50 0.67 m) yet reads 0.032 m in hour 2, and birth only 0.036 m, against 0.118 m at sigma 1 and 0.031 m with the floor off.
- **Not traced.** Why a cold re-host grows the error again under a falling formal sigma (section 9.2's TOW 263640); and why the code slope stands at 2.51 m (receiver median) when the error's p50 is 0.28 m (correlation -0.23 over all receivers and epochs), which is what satellites carrying other receivers' errors would give (section 9.5), not checked further.

### 9.5 The satellites, and I-b under A1

Each tree edge (r, s) is a pinned phase row, so against a control the satellite's ionosphere-free visible phase moves by m^s_r dT_r + X_r, one unknown per receiver. Walking the tree, the parent satellite gives X_r and every child is predicted with no fitted number (`sat_vs_t.py`). `ird_tr` against its slip-free control at the slip epoch: measured rms 0.85 m, residual 0.35 m, correlation 0.946, slope 1.40; with no wet-delay term the residual is 0.95 m. 2 min later: 0.62, 0.30, 0.878, 1.29, and 0.57 with no term. So section 5.3's undeclared satellites are the wet-delay differences of the receivers above them, and section 8.5's `ird_msr` steps carry the same signature.

**Reading.** Under A1 a row's I-b is dominated by this state and does not grade the row. The floor-off arms and the arms below grade the row. At floor sigma 3 m on the row fixtures (previous reading at sigma 1 in brackets): `ird_msr`'s split passes (0.485, 261 pairs; 733 integer steps), `ird_t1`'s slip reads WL 0.190 on 74 pairs with 38 per-band integer steps (0.497, 146; 224), `ird_ms`'s split WL 0.164 on 122 with 24 (0.174, 126; 47), and the merges WL 0.076 on 24 with 0 (0.074, 23; 0) and WL 0.427 on 221 with 95 (0.411, 213; 297), the same sizes as the floor-off arms of section 8.4. The bars are not relaxed by this reading; A1 is the arm the gate names.

### 9.6 Reach: the goldens and the field

- **The goldens.** Both have `phase_datum_eq34` false, so the floor's weight is 0. The sim golden's wet delay reads p50 0.005 m, max 0.021 m at the last survey epoch. The field golden after hour 1 ranges from -0.111 to +0.447 m at a formal 5 mm, with no truth and no metre-level state.
- **A truth-free reading.** The receiver median of |code slope| after hour 1 (`code_slope_census.py`): A1krhRs 2.51 m (p90 4.04, 56 of 60 receivers above 0.5 m); floor off 0.145 (0.406, 2); sigma 3 0.179 (0.421, 4); sim golden 0.122 (0.320, 0); field golden 0.137 (0.351, 3 of 58). It separates the postures and reads the field golden with the sim golden. It is not a per-receiver error estimate (section 9.4).
- **The field reaches it with the chain on.** Archived arm `_rap_prm_nom14_qf` (2026-10-09: the field golden plus `phase_datum_eq34` and `_carry_shift`, the floor at its default, 2 h) reads after hour 1: p1 -5.24, p99 25.06 m; GOPE00CZE +29.29, CHUR00CAN +26.85, GOP600CZE and JPLM00USA +11.8 m at a formal 18 to 41 mm; 28 of 58 receivers beyond 1 m; code slope 1.49 m (p90 7.95, 51 of 58 above 0.5 m). The retired posture (`_cm_a0`, carry-invariant vehicle) reads -47 to +44 m. No field arm has the chain on and the floor off, and card IRD section 6 allows no new one before IRD-6 has passed, so the floor is not proved the cause on the field. A one-knob pair would settle it and is the author's to allow.
- **Blind instruments on this bed.** The block-Baarda census reports no block in either arm, because every block is uncheckable on the free bed. RAIM's T4 attribution does react: 64 drop rows with the floor on (26 on S16000SIM, 20 on S23000SIM, 14 on S45000SIM), 0 with it off or at 3 m and 10 m. The RPB time-link NEES reads 0.768 against 0.549.

### 9.7 D0 and D-N

**D0.** The DCB shift against the floor-off arm follows the shift of the geometry-free bias with slope -1 and no fit (correlation, slope at the first epoch and at 2 h): sigma 100 -1.000, -0.99 and -0.962, -0.84; sigma 10 -0.999, -0.99 and -0.988, -0.96; sigma 3 -0.956, -1.08 and -0.976, -0.94; sigma 1 -0.967, -1.73 and -0.885, -1.18, where T's own moves enter. This test was formed after the first table and before the correlation was computed. So the DCB spread that decision D0 records is largely the floor's own, not an independent defect. Card IRDB section 3.5 read "with the floor on or off" before the rows: floor off 0.68, 3.09 m on r3c. With the rows armed the floor-off spread does not grow.

D0's beds with every event row armed (arms A1krhRsm, floor knob only; D0's bars: DCB sd at most 0.17 m, W5 no higher, consumer within +10 % of the floor-on arm in hours 1 and 2):

| bed | floor | consumer h1, h2, m | DCB sd h1, h2, m | W5 | RPB NEES | wet-delay error after hour 1: p50, p99, max, m |
| --- | --- | --- | --- | --- | --- | --- |
| r3c | 1 m | 0.179, 0.111 | 9.15, 9.49 | 0 | 0.746 | 0.290, 5.56, 15.1 |
| r3c | 3 m | 0.222 (+24 %), 0.030 (-73 %) | 3.30, 3.32 | 0 | 0.569 | 0.028, 0.617, 3.13 |
| r3c | off | 0.277 (+55 %), 0.031 (-72 %) | 0.67, 0.70 | 0 | 0.556 | 0.022, 0.733, 4.21 |
| golden sim | 1 m | 0.089, 0.035 | 7.03, 6.94 | 0 | 0.650 | 0.088, 0.874, 1.80 |
| golden sim | 3 m | 0.092 (+3 %), 0.033 (-6 %) | 3.35, 3.38 | 0 | 0.603 | 0.012, 0.087, 0.15 |
| golden sim | off | 0.093 (+4 %), 0.033 (-6 %) | 0.40, 0.40 | 0 | 0.590 | 0.009, 0.056, 0.09 |
| golden sim | no chain | 0.099, 0.033 | 0.07, 0.05 | 0 | n/a | 0.006, 0.028, 0.06 |

No switch passes D0's bars as they stand: the DCB sd stays above 0.17 m on both beds at every setting (off 0.70 and 0.40 m, against 0.05 m with no chain), and on r3c hour 1 stands +24 % and +55 %. The readings have moved, though. Before the rows the floor-off DCB reached 3.09 and 4.24 m by hour 2, and it now holds 0.70 and 0.40 m. On r3c the floor-on consumer in hour 2 is 3.6 times the floor-off one, where on the golden sim the two match. These are readings: D0 is not re-decided here.

**D-N.** Card IRD section 10: "An RPB NEES above 2 on the sim makes IRD-5b owed before the D1 verdict". A0, with no rows, read 1.35, 2.31, 2.58, 1.11 on the microscope, r2, r3c and the golden sim. A1 with every row reads 0.086, 0.766, 0.746 and 0.650 on the same beds. The trigger does not fire on the arm going forward. It is also blind to this state: the bias is held stiffly, so metres of wet-delay error read 0.768. IRD-5b's gate (NEES within [0.5, 2] on r3c and the golden bed) reads inside it; the microscope's 0.086 is below the range and outside the gate's beds.

### 9.8 What bears on the ruling, for the author

The 2026-11-02 ruling closed the certification rule as a float defect from events and the RPB floor, and IRDC section 7.3 holds the certified branch on A1 pending D0 ("the floor biases the floats from the first epoch on every bed"). This section supplies the mechanism of that bias; the ruling's text stands. Three things are the author's to weigh, and none is acted on here:

1. D0's verdict "the floor stays" was measured before the event rows, and they change its readings (section 9.7).
2. D0 offered the floor on, off and birth only. Sigma was not among the switches. On these beds 3 m gives the floor-off reading for the wet delay and I-b, with the DCB spread between the two (3.3 m) and on r3c an hour 1 consumer between the two (0.222 m, against 0.179 at 1 m and 0.277 off). The hour 1 consumer on r3c and its siblings is what the floor buys; on the golden sim all three settings match.
3. The prior's form is open: a zero-mean prior on a bias that holds integer path sums is the inconsistency, whatever its size. Candidates are not built or graded here.

**Not covered.** One fixture for the sweep (`ird_tr`), 2 h; the D0 beds at sigma 0 and 3 only; no new field arm. The decay of section 9.4 is a fit; the first-epoch test is the falsifiable one.
