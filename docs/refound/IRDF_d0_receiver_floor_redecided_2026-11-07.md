# IRDF: decision D0 re-decided with the event rows armed

**STATUS**

- **id:** IRDF
- **topic:** network-vehicle
- **status:** closed
- **summary:** Pre-registered and graded 2026-11-07. Card IRD's decision D0 (the receiver phase-bias floor) re-decided at the author's word on the arm going forward (every event row armed), after card IRDE section 9 traced the floor as the cause of A1's wet-delay state. As written neither candidate passes (the DCB bar on both beds; floor off also the float consumer in r3c hour 1), so the floor stays and the GF-datum defect is recorded. The readings bear on the rule: the DCB bar lies below the fixture's true spread, the floor-off spread is the receiver phase-bias time link's double count, the hour-1 gain is one site's, and at held-out sites and at a 40 degree mask the floor costs. RULED 2026-11-09 (section 9): the floor goes off on the arm going forward, with the DCB clause read against truth and the consumer clause at the held-out sites; nothing is armed.
- **source:** [IRD](IRD_integer_recovering_datum_2026-10-13.md) section 10 (D0) and C3; [IRDB](IRDB_integer_recovering_datum_continued_2026-10-30.md) section 3.5 (the first verdict); [IRDC](IRDC_integer_recovering_datum_events_2026-11-02.md) section 7.3 (the hold that waits on D0); [IRDE](IRDE_integer_recovering_datum_slip_2026-11-06.md) section 9 (the mechanism)
- **knobs / posture:** `network.phase_datum_eq34_rpb_prior_sigma_m`, `network.phase_datum_eq34_rpb_prior_birth_only`; the base arm A1krhRsm of `scripts/ird_make_arms.py`; added readings: `network.phase_datum_eq34_rpb_q_m2s` 0 (the mechanism test) and `processing.elevation_mask_deg` 40 (the rank test). No golden change
- **dates in the text:** 2026-11-07, 2026-11-09
- **hashes / run dirs:** exe `D:/GnssProcessorSupport/gates/exe/x64-Release/gnss_processor_ird4ms.exe` (sha256[:16] `4547066185234026`); arms `_rap_ird4z_*` and the r3c floor-on reference `_rap_ird4ms_a1krhRsm_r3c`, replays `_rap_d0c_*`; added after pre-registration: held-out wires `_rap_d0w_*` and their replays `_rap_d0h_*`, the no-link arms `_rap_ird4z_off_q0_*`, the 40 degree mask arms `_rap_ird4z_m40_*`; all under `D:/GnssProcessorSupport/runs_nom/`
- **gates (frozen at pre-registration):** section 2; the bars are card IRD's, unchanged

**Outcome.** Question: with every event row armed, does the receiver floor stay, go off or change? Decisive result: neither of the rule's candidates passes as written (DCB sd 0.40 to 0.70 m with the floor off and 5.3 to 6.6 m birth only, against 0.17 m; floor off +55 % at the float consumer in r3c hour 1). Verdict: the floor stays and the GF-datum defect is recorded. Standing: the bar lies below the fixture's true DCB spread (0.538 m), the floor-off spread is the time link's double count, and at held-out sites floor off equals the no-chain wire; the author ruled the floor off on 2026-11-09 ([section 9](#9-2026-11-09-the-authors-ruling)).

## 1. Why

Asked after card IRDE section 9, "Should D0 (the receiver floor) be re-decided before arm C?", the author answered: "Yes, re-decide D0". The first verdict (IRDB section 3.5) was measured before the event rows and kept the floor because neither switch passed. IRDE section 9 then traced A1's metre-level wet-delay errors to the floor, and showed that under A1 a row's I-b reads that state.

## 2. The rule and what is measured

The rule, card IRD section 10, verbatim: "The floor goes off if A0f, on r3c and the golden bed, shows a DCB sd <= 3x the golden's (0.17 m), W5 withholds no higher than A0's, and float-consumer p50 within +10 % of A0's in hours 1 and 2. Otherwise build birth_only and re-measure. Otherwise keep the floor and record the GF-datum defect, which blocks the ionosphere follow-up."

One reading of the rule is declared: the reference is A1krhRsm with the floor at its default in place of A0, because that is the arm going forward (A1 reproduces A0 where both bands track).

**Candidates.** The rule's two: floor off (sigma 0) and birth only (sigma 1). Readings outside the rule's list: sigma 3 m and 100 m.

**Bars.** As quoted, per candidate on r3c and the golden sim. A clause that fails is reported as failed. No bar moves without the author's ruling.

**Readings, frozen before grading.** Each per candidate, against the floor-on arm and against the no-chain reference R1 on the same exe, on the same two beds:

1. **Multi-site replay.** 16 in-network consumers per wire (S01, S05, ..., S57 and S60), each replayed from the wire's first epoch. The statistic is each site's p50 in hour 1 and hour 2, then the paired change against the floor-on wire: median and sign counts. A replay of the held-out consumer reproduces the in-loop trace (240 of 240 rows identical on `ird4ms_a1krhRsm_r3c`).
2. **DCB against truth.** The sd of (estimate less truth) across receivers, in the sign the microscope shows, using each fixture's `truth_rx_bias.csv`.
3. **Integer instruments.** Reference-free double differences, the search's would-be fixes against truth, the stop rule's event-free chords.
4. **The wet delay against truth,** after hour 1.
5. **Rank.** Station positive-definite rejections and emitted epochs with the floor off.
6. **The row fixtures.** I-b per row with the floor off, from the arms already run (`ird_t1`, `ird_tr`, `ird_ms`, `ird_msr` and the microscope twins), and the rows' consumer clause.

**Seen before this pre-registration.** The bars at sigma 1, 3 and 0 on both beds (IRDE section 9.7), the row fixtures at sigma 0 and 3 (IRDE sections 8.4 and 9.5), hour 1 of the held-out consumer by 5 min bin on those arms, and the fixture's true DCB spread (sd 0.538 m). Not seen: the replays, birth only and sigma 100 m on these beds, the DCB error table on all arms, and the integer instruments on these beds.

**What the verdict will say.** Whether each candidate passes the rule as written. If none does, the rule's last sentence is what stands, and any evidence that the rule should be read otherwise goes to the author as a question, with the recommendation stated and nothing armed.

## 3. 2026-11-07: the verdict as written

The arms are A1krhRsm with one knob each, 2 h, on the exe of the STATUS block; the no-chain reference R1 runs on the same exe. The floor-on reference (sigma 1 m) reads a float consumer of 0.179, 0.111 m on r3c and 0.089, 0.035 m on the golden sim, and a DCB sd of 9.15, 9.49 and 7.03, 6.94 m.

| candidate | DCB sd h1, h2, m: r3c; golden sim (bar 0.17) | W5 | float consumer h1, h2, m, against the floor-on arm (bar +10 %): r3c; golden sim | verdict |
| --- | --- | --- | --- | --- |
| floor off (sigma 0) | FAILED: 0.67, 0.70; 0.40, 0.40 | pass: 0, every epoch emitted | FAILED in r3c hour 1: 0.277 (+55 %), 0.031 (-72 %); pass: 0.093 (+4 %), 0.033 (-6 %) | FAIL (DCB on both beds; r3c hour 1) |
| birth only | FAILED: 6.58, 6.44; 5.34, 5.28 | pass | pass: 0.127 (-29 %), 0.036 (-68 %); 0.074 (-17 %), 0.028 (-20 %) | FAIL (DCB on both beds) |

Neither candidate passes, so the rule's last sentence stands: **the floor stays, and the GF-datum defect is recorded; it blocks the ionosphere follow-up.** No knob, arm definition or bar changes. Against A0's own numbers (card IRDB section 3.5: 0.417 m on r3c and 0.071 m on the golden sim in hour 1) floor off would pass r3c (-34 %) and fail the golden sim (+31 %): under either reference the consumer clause fails on one bed in hour 1.

## 4. The readings

Readings 1 to 6 are the frozen ones. Four were added after pre-registration and are labelled so: the replay at held-out sites (section 4.1), the 40 degree mask (section 4.5), and the no-link arms and the bias's formal sigma (section 5.1).

### 4.1 Multi-site replay (reading 1)

Sixteen in-network sites per wire; the median of the site p50s in hours 1 and 2, and the paired change against the floor-on wire (median, and sites better of 16):

| bed, wire | site p50 median h1, h2, m | change against floor on, m | better of 16 |
| --- | --- | --- | --- |
| r3c, floor on | 0.689, 0.131 | | |
| r3c, floor off | 0.200, 0.064 | -0.497, -0.058 | 16, 16 |
| r3c, sigma 3 | 0.207, 0.065 | -0.435, -0.059 | 16, 16 |
| r3c, birth only | 0.482, 0.107 | -0.099, -0.026 | 11, 12 |
| r3c, no chain | 0.200, 0.064 | -0.495, -0.057 | 16, 15 |
| golden sim, floor on | 0.213, 0.049 | | |
| golden sim, floor off | 0.147, 0.034 | -0.098, -0.012 | 10, 14 |
| golden sim, no chain | 0.108, 0.038 | -0.094, -0.007 | 12, 10 |

Against the no-chain wire, floor off reads -0.0002 and -0.0019 m on r3c (8 and 11 of 16 better, no site beyond 0.011 m) and +0.020 and -0.002 m on the golden sim (3 and 11 of 16 better, largest +0.166 m at S37000SIM in hour 1). Birth only reads +0.321 and +0.039 m on r3c (15 of 16 worse in both hours, +3.043 m at S45000SIM), and sigma 3 +0.017 and -0.0003 m (+0.595 m at S45000SIM).

**Held-out sites (added).** A site inside the network shares its own error with the wire: S45000SIM, the receiver with the largest wet-delay error, moves most. Eight sites were held out, S04, S11, S18, S25, S32, S39, S46 and S53 (chosen by number before any was read). Each wire was re-run on the other 52 stations and replayed at the eight, and SZZ000SIM is read from the wire's own trace.

| bed, wire | site p50 median h1, h2, m | against floor on: change, m; better of 9 | against no chain: change, m; largest worse, m |
| --- | --- | --- | --- |
| r3c, floor on | 0.652, 0.133 | | +0.475, +0.064; +2.129 (S11) |
| r3c, floor off | 0.226, 0.055 | -0.474, -0.064; 8, 9 | -0.001, +0.001; +0.006 |
| r3c, birth only | 0.503, 0.133 | -0.071, -0.036; 7, 6 | +0.205, +0.065; +1.445 (S11) |
| r3c, no chain | 0.227, 0.055 | -0.475, -0.064; 8, 9 | |
| golden sim, floor on | 0.266, 0.067 | | +0.115, +0.028; +0.424 (S53) |
| golden sim, floor off | 0.128, 0.033 | -0.125, -0.025; 6, 8 | +0.001, -0.002; +0.027 (S25, hour 2) |
| golden sim, birth only | 0.243, 0.064 | -0.029, -0.007; 8, 8 | +0.079, +0.019; +0.231 (S25) |
| golden sim, no chain | 0.118, 0.034 | -0.115, -0.028; 7, 7 | |

The one held-out site where the floor helps in r3c hour 1 is SZZ000SIM (0.179 against 0.276 m). On the golden sim the three sites that read worse with the floor off do so by at most 0.009 m. Floor off is within 0.013 m (r3c) and 0.027 m (golden sim) of the no-chain wire at every held-out site.

### 4.2 The DCB against truth (reading 2)

The fixture's true DCB (C2W less C1C, `truth_rx_bias.csv`) has sd 0.538 m across receivers on both beds. The trace's DCB has the opposite sign (the microscope with the floor off: correlation -0.888), so the error is the estimate plus the truth. The sd of the error across receivers at the end of hour 2, m:

| bed | floor on | sigma 3 | sigma 100 | floor off | birth only | floor off, no link (added) | no chain |
| --- | --- | --- | --- | --- | --- | --- | --- |
| r3c | 9.530 | 3.209 | 0.820 | 0.823 | 6.449 | 0.538 | 0.540 |
| golden sim | 6.950 | 3.249 | 0.620 | 0.618 | 5.264 | 0.538 | 0.547 |

On the microscope, where the zero-ionosphere rows make the DCB estimable, the error is 1.195 m with the floor on and 0.248 m with it off. No arm resolves the DCB on the two free beds (floor off: correlation with truth -0.13 and -0.16). The no-chain arm's spread (0.01 to 0.07 m) is its repeated 10 m zero-mean prior (formal exactly 10/sqrt(n) from t+210 s), and its error equals the truth's spread. A correct estimate would read 0.538 m and fail the 0.17 m bar, so on these beds the bar measures agreement with the prior.

### 4.3 Integer instruments (reading 3)

Window h0_12 of the search grader; the stop rule as stated for the microscope (event-free chords, p50 at most 0.020 cycle per band). Errors in cycles:

| bed, arm | reference-free DD error p50, share above 0.25 | search: kept, wrong, ceiling | search DD error p50 | stop: event-free p50 L1C, L2W |
| --- | --- | --- | --- | --- |
| r3c, floor on | 2.566, 0.809 | 5 180, 3 035, 50.1 % | 4.629 | 0.241, 0.247: FAIL |
| r3c, sigma 3 | 0.359, 0.581 | 22 092, 726, 51.5 % | 0.672 | 0.245, 0.242: FAIL |
| r3c, sigma 100 | 0.303, 0.545 | 28 160, 234, 51.2 % | 0.555 | 0.246, 0.258: FAIL |
| r3c, floor off | 0.304, 0.545 | 28 164, 228, 51.2 % | 0.556 | 0.250, 0.258: FAIL |
| r3c, birth only | 1.462, 0.771 | 7 676, 2 084, 50.3 % | 2.679 | 0.252, 0.248: FAIL |
| r3c, floor off, no link (added) | 0.243, 0.493 | 30 386, 0, 54.2 % | 0.424 | 0.192, 0.220: FAIL |
| r3c, no chain | 0.157, 0.375 | 56 626, 0, 58.4 % | 0.280 | not run |
| golden sim, floor on | 1.234, 0.758 | 21 698, 8 436, 50.2 % | 2.085 | 0.249, 0.242: FAIL |
| golden sim, sigma 3 | 0.284, 0.530 | 88 900, 924, 51.4 % | 0.494 | 0.264, 0.242: FAIL |
| golden sim, sigma 100 | 0.260, 0.509 | 103 104, 124, 52.5 % | 0.434 | 0.244, 0.260: FAIL |
| golden sim, floor off | 0.260, 0.509 | 103 088, 124, 52.5 % | 0.434 | 0.244, 0.259: FAIL |
| golden sim, birth only | 0.719, 0.695 | 39 062, 6 905, 49.8 % | 1.284 | 0.241, 0.250: FAIL |
| golden sim, floor off, no link (added) | 0.239, 0.489 | 101 340, 0, 53.3 % | 0.386 | 0.217, 0.240: FAIL |
| golden sim, no chain | 0.156, 0.373 | 140 418, 0, 58.8 % | 0.263 | not run |

The event-free chords number 28 680 (r3c) and 28 661 (golden sim) on every chain arm. Floor off cuts the wrong would-be fixes from 3 035 to 228 and from 8 436 to 124, and stays behind the no-chain reference: reference-free 0.304 against 0.157 cycle, ceiling 51 % against 58 %. By 10 min bin on r3c the floor-off reference-free p50 stalls (0.445, 0.262, 0.292, 0.321, 0.285, 0.259, 0.257, 0.287), where no chain keeps improving (0.449, 0.270, 0.199, 0.180, 0.149, 0.132, 0.127, 0.124) and floor on stays at cycles (3.216, 4.749, 4.024, 2.924, 2.732, 2.272, 2.377, 2.379).

### 4.4 The wet delay against truth (reading 4)

Error after hour 1 over all receivers and epochs, p50, p90, p99, max, m:

| arm | r3c | golden sim |
| --- | --- | --- |
| floor on | 0.290, 1.460, 5.564, 15.12 | 0.088, 0.338, 0.874, 1.80 |
| sigma 3 | 0.028, 0.154, 0.617, 3.13 | 0.012, 0.036, 0.087, 0.15 |
| sigma 100 | 0.022, 0.179, 0.736, 4.21 | 0.009, 0.023, 0.056, 0.09 |
| floor off | 0.022, 0.178, 0.733, 4.21 | 0.009, 0.023, 0.056, 0.09 |
| birth only | 0.132, 0.747, 2.920, 6.26 | 0.043, 0.174, 0.503, 0.81 |
| floor off, no link (added) | 0.015, 0.068, 0.439, 3.74 | 0.008, 0.020, 0.037, 0.06 |
| no chain | 0.008, 0.022, 0.041, 0.07 | 0.006, 0.016, 0.028, 0.06 |

### 4.5 Rank (reading 5)

No station block is dropped and every epoch is emitted on every arm of sections 3 and 4 (`network_run_summary.csv`: `n_sta_pd_dropped` 0, 60 of 60 stations in the solve).

**The floor's stated role (added).** The floor's code comment (the eq34 rank floor in `src/core/network/solve/PhaseDatum.cpp`) gives it a rank role in blocks with few satellites or one signal, citing a field A/B in which the datum-invariant DD step went from 0.085 to 36 to 118 cycles per epoch without it. The comment dates from 2026-07-11 (`6fa0cd1`), before the receiver code-bias datum (`network.rx_code_bias_datum`, 10 m, added 2026-08-05 in `2862318`), which is armed on every arm here and holds the receiver's geometry-free direction. The beds of the rule never reach that case, so the elevation mask went to 40 degrees on r3c (`processing.elevation_mask_deg`), with the floor on, off, at 100 m and with no chain. Prediction, written before the run: the floor-off arm drops no more station blocks and withholds no more epochs than the floor-on arm, and its reference-free DD error is no larger.

| arm, 40 degree mask | blocks dropped | emitted | float consumer h1, h2, m | DCB sd h1, h2, m | wet-delay error after hour 1: p50, p90, p99, m | reference-free DD error p50; search wrong of kept |
| --- | --- | --- | --- | --- | --- | --- |
| floor on | 122 | 240 of 240 | 6.401, 9.164 | 13.31, 13.28 | 1.72, 13.95, 111.9 | 0.419; 251 of 798 |
| floor off | 122 | 240 | 0.843, 0.936 | 0.54, 0.52 | 0.417, 4.39, 38.3 | 0.085; 78 of 2 850 |
| sigma 100 | 122 | 240 | 0.845, 0.938 | 0.72, 0.70 | 0.411, 4.30, 37.4 | 0.084; 98 of 2 874 |
| no chain | 122 | 240 | 0.794, 0.895 | 0.04, 0.03 | 0.056, 0.351, 3.28 | 0.020; 0 of 7 174 |

The prediction holds on all three clauses. The drops are the same 122 on every arm, and the null eigenvectors that name them carry receiver clock (53 %) and wet delay (47 %) and none of the phase bias, the ambiguities or the ionosphere: high satellites leave clock and wet delay nearly collinear, with or without the chain. With few satellites the floor decides no rejection, and the floor-on wire is the one that fails, by metres.

### 4.6 The row fixtures (reading 6)

From card IRDE (sections 4, 5.2, 8.4 and 9.3), the floor off on the row fixtures (bars 0.05 wide lane on the free bed, 0.02 per band on the microscope):

- **Slip row.** `ird_tr` PASS (wide lane 0.026, 0 integer steps). `ird_t1` wide lane 0.213 on 74 pairs, the size those pairs reach without the row, with 56 per-band integer steps and 0 against the slip-free control. The microscope twins: 0 integer steps and E1 0.038 and 0.037 against the 0.02 bar, FAILED, at the quiet epochs' own level (p50 0.039 and 0.024).
- **Merge and split row.** `ird_msr`'s split PASS; `ird_ms`'s split FAILED (wide lane 0.189 on 129, 33 integer steps); the microscope splits E1 0.053 and 0.120 with 0 integer steps, FAILED; the merges FAILED on every bed (wide lane 0.080 and 0.485, E1 0.134 and 0.367), with 131 integer steps on `ird_msr`.
- **The rows' consumer clause** passes with the floor off: 0.277, 0.031 m on the free bed and 0.124, 0.045 m on the microscope, the same as without the rows to within 1 mm.

With the floor on the same rows fail I-b on most fixtures, and under A1 their I-b reads the floor's wet-delay state rather than the row (card IRDE section 9.5).

## 5. Why the two clauses fail

### 5.1 The DCB clause: the receiver phase bias's time link

The floor-off spread is not information about the DCB. The carries start at the eighth epoch (t+210 s). With the floor off on r3c the DCB's spread is below 0.01 m through t+180 s, then 0.435, 0.455, 0.605 and 0.682 m at t+210 to t+300 s, and 0.656 to 0.696 m from t+900 s to the end: set in the first four linked epochs, then frozen. Over the same epochs the receiver phase bias's formal sigma collapses once its time link (`network.phase_datum_eq34_rpb_q_m2s`, default 3e-6 m^2/s) folds the previous epoch's conditional variance as a new prior.

**The mechanism test (added).** The floor off with `network.phase_datum_eq34_rpb_q_m2s` 0, which removes the link and the bias's carry (`_rap_ird4z_off_q0_r3c`, `_gs`). Prediction, written before the run: the receiver DCB holds only its repeated 10 m prior, so its sd falls to the no-chain level (0.02 m on r3c, 0.05 to 0.07 m on the golden sim) and its formal follows 10/sqrt(n). Result: PASS. The DCB sd stays below 0.01 m at every epoch on both beds, the formal reads 7.071 m at t+210 s and 2.000 m at t+900 s, and the error against truth is the truth's own spread (0.538 m). The float consumer reads 0.281, 0.031 m (r3c) and 0.091, 0.033 m (golden sim), every epoch is emitted with no block dropped, and the wrong would-be fixes fall to 0 on both beds (sections 4.3 and 4.4).

The bias's formal sigma with the floor off on r3c, p50 over the unpinned receivers, without and with the link (added):

| t, s | 0 | 180 | 210 | 240 | 300 | 450 | 600 | 900 | 1800 | 3600 | 7170 |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| no link, m | 29.3 | 22.1 | 16.3 | 13.8 | 11.3 | 8.33 | 7.00 | 5.62 | 3.95 | 2.99 | 2.14 |
| link, m | 29.3 | 22.1 | 10.8 | 5.97 | 2.44 | 0.277 | 0.072 | 0.021 | 0.018 | 0.018 | 0.018 |

Without the link the formal falls as data accrue. With it the formal falls by a factor of 0.5 to 0.76 per epoch over the first 10 min with no geometry-free information behind it, and stands 119 times below the block's own value at 2 h; the floor-on arm and the golden sim read the same 0.018 to 0.022 m. The harvest (`networkHarvestRxPhaseBias`, `src/core/network/solve/ChainDatum.cpp`) folds the diagonal conditional variance while the block's carried columns (the ambiguities, the slant ionosphere, the receiver code bias in the block) already hold that information, so it is counted twice each epoch. The receiver code-bias carry recorded the same defect for its own diagonal fold ("the level information doubled per epoch on the sim bed", `src/core/network/carry/RxCodeBiasCarry.cpp`) and moved its columns into the block. The link also carries the root row's target (`applyRootRow`): with the link off a root handover finds no carry and pins at zero, as before the row. No root row holds a target on any arm here (`station.eq34_root_rpb_target` rows max 0), so the no-link arms lose nothing on these beds; they are a mechanism test, not a candidate.

So the GF-datum defect under the chain has two parts: the floor's (metres, card IRDE section 9.7) and the time link's (with the floor off, a DCB error of 0.82 and 0.62 m against 0.54 m for the prior alone, and 228 and 124 wrong would-be fixes).

### 5.2 The consumer clause: one site

The float consumer is one receiver, SZZ000SIM. In r3c hour 1 the floor helps it between minutes 10 and 45 (by 5 min bin, floor on 0.718, 0.361, 0.109, 0.110, 0.185, 0.271, 0.242, 0.166, 0.141, 0.147, 0.178, 0.190 m; floor off 0.767, 0.312, 0.293, 0.278, 0.347, 0.334, 0.288, 0.259, 0.224, 0.140, 0.064, 0.045 m), and there floor off reads as the no-chain wire (0.277 against 0.289 m). At the eight other held-out sites the floor costs 0.474 m (median) in hour 1, and on r3c it costs at all 16 in-network sites (section 4.1). The clause's hour-1 failure is SZZ000SIM's own gain.

## 6. Notes on the record

The earlier text stands; these readings qualify it.

- Card IRD C3: the floor "does not bias the emitted P in steady state". Under A1 it holds the wet delay metres off for tens of minutes (card IRDE section 9), and with few satellites by tens of metres (section 4.5).
- Card IRDE section 9.2: "With the floor off the DCB stays near 0". The floor-off spread is 0.40 to 0.70 m against 0.01 to 0.07 m with no chain, and it is the time link's (section 5.1).
- Card IRDE section 9.8, item 2: "The hour 1 consumer on r3c and its siblings is what the floor buys". At held-out sites that gain is SZZ000SIM's alone (section 5.2).
- The floor's rank rationale (its code comment) predates the receiver code-bias datum and does not appear at a 40 degree mask (section 4.5).

## 7. For the author

As written the floor stays. The evidence that the rule should be read otherwise:

1. **The DCB bar cannot be met by a correct estimate on these beds** (true spread 0.538 m against 0.17 m): it measures agreement with the 10 m zero-mean prior. Read against truth with the bar's own factor (three times the no-chain arm's error: 1.62 and 1.64 m), floor off passes (0.82, 0.62 m), and the floor (9.53, 6.95 m) and birth only (6.45, 5.26 m) fail.
2. **The consumer clause's failure is one site's** (section 5.2). Read at the held-out sites with the bar's own +10 %, floor off passes on both beds (-65 % and -59 % on r3c, -52 % and -51 % on the golden sim), and it equals the no-chain wire within 0.027 m at every one.
3. **The floor's rank role does not appear:** no block is dropped on the beds, and at a 40 degree mask the drops are the receiver clock's on every arm while the floor-on wire fails by metres.
4. **The floor-off spread that remains is the time link's double count** (section 5.1). Removing the link passes the DCB clause as written on both beds; it also removes the root row's carry, so it is a mechanism test, not a candidate.

**Recommendation.** The floor goes off (sigma 0, the rule's own switch) on the arm going forward, with D0's DCB clause read against truth and its consumer clause at the held-out sites, as items 1 and 2 state. The GF-datum defect stays recorded, as the time link's double count, and still blocks the ionosphere follow-up; its fix (the receiver phase bias carried as a column of the block, as the receiver code bias is) is a build owed before that follow-up. Neither golden has the chain on, so the field is untouched; a field pair (chain on, floor on and off) is the author's to allow after IRD-6 (card IRD section 6).

## 8. Standing

Open: the author's ruling on section 7. Until then A1 keeps the floor (`scripts/ird_make_arms.py`), the GF-datum defect stays recorded and blocks the ionosphere follow-up, and the hold of card IRDC section 7.3 stands. Nothing is armed.

**Not covered.** Two sim beds and 2 h; one fixture at a 40 degree mask; the held-out replay was not run for sigma 3, sigma 100 or the no-link arms; no field arm (card IRD section 6). The time link's double count is shown by the formal sigma and the no-link arms; the build that would remove it is not designed here.

## 9. 2026-11-09: the author's ruling

"Yes, D0 floor off." On 2026-11-09 the author accepted the recommendation of section 7:

- The floor goes off (`network.phase_datum_eq34_rpb_prior_sigma_m` 0, the rule's own switch) on the arm going forward.
- D0's DCB clause is read against truth, with the bar's own factor (three times the no-chain arm's error), and its consumer clause at the held-out sites, as items 1 and 2 of section 7 state. The verdict as written (section 3) stays in the record.
- The GF-datum defect stays recorded, as the time link's double count (section 5.1), and still blocks the ionosphere follow-up. Its fix, the receiver phase bias carried as a column of the station block, is the build owed before that follow-up.

Not changed by the ruling: neither golden runs the chain, and a field pair (chain on, floor on and off) stays the author's to allow after IRD-6. Owed: the floor off in the arm definitions (`scripts/ird_make_arms.py`), the event rows' I-b read again on that arm, and the in-block build. Until the arm definitions change, A1's recorded numbers are the floor-on ones.
