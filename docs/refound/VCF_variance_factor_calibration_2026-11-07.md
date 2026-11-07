# VCF: a per-class variance factor for the network producer

**STATUS**

- **id:** VCF
- **topic:** raim-qc
- **status:** closed
- **summary:** Closed 2026-11-07 (build B1 of card SRN, findings F1 and F8). Stage 1 built and validated measure-only (`network.vcf`): a per-class variance factor on the redundancy census sweep, sum w v^2 / sum r and its robust companion by constellation and signal. The kernel recovers the sim code law to 1 %; the field code noise is 0.35 of the a priori sigma (0.45 as a second moment), flat by hour; the phase factor is not identifiable (equal across bands by algebra, 0.4 to 0.6 on a sim whose phase error exceeds the a priori). Stage 2 built behind two keys: the URA reader (`network.vcf_apply`) changes only the U records (0.37 to 0.40 of the a priori on the field), UNVALIDATED for want of a truth grade; the RAIM readers (`network.vcf_apply_phase`) pass the user and census bars but raise T4 refloats 69 % on a non-measured scale and detect nothing more on a satellite clock step, KEEP_OFF. Nothing armed.
- **source:** card SRN (`docs/refound/SRN_scientific_review_network_2026-11-06.md` in the worktree `D:/GnssProcessorSupport/worktrees/scirev`, branch `review/network-engine`), findings F1 and F8, build B1, sections 3.2, 3.4 and 4; the reviewer reports `D:/GnssProcessorSupport/srn/wave1.json` (D2-01 to D2-05, D2-08, D4-05, D6-01 to D6-04, D6-08, D6-09, D8-03, D8-04) and the second-model verdicts `D:/GnssProcessorSupport/srn/wave2_verdicts.json`; cards [T3 null](../cards/raim-qc/T3-NULL-DERIVATION.md), [T3 field](../cards/raim-qc/T3-FIELD-AND-COLLATERALS.md), [RPD](RPD_replay_station_prior_2026-10-01.md), [RPA](RPA_raim_pool_data_rows_2026-10-08.md), `docs/SPEC_carry_vce_apply.md`
- **dates in the text:** 2026-11-07
- **knobs / posture:** new `network.vcf` (TRACE, default false, measure-only), `network.vcf_ewma_alpha` (PARAM, 0.1), `network.vcf_apply` (UNVALIDATED, default false, requires `network.vcf`), `network.vcf_apply_phase` (KEEP_OFF, default false, requires `network.vcf_apply`); the goldens and the manifest unchanged, nothing armed
- **hashes / run dirs:** base main `adef08b`; branch `worktree-wf_c49deb5b-2f3-5` commits `1785acb` (pre-registration), `9cc67b0` (stage 1), `eb3150b` (stage 2); exes `gnss_processor_vcf.exe` sha256[:16] `74f3a31f43de22b8` (stage 1) and `gnss_processor_vcf2.exe` `7159193fc9b86f35` (stage 2), built in the worktree; runs `D:/GnssProcessorSupport/runs_nom/_rap_vcf_{smoke_f01,off_f2,off_s2,on_f6,on_s6,ctl_f6,ctl_s6}`, `_rap_vcf2_{ctl,apply,phase}_{f2,s2}`, `_rap_vcf2_{ctl,apply,phase}_f6`, `_rap_vcf2_clk{5,1}_{ctl,phase}`, census `_rap_vcfc_{ctl,phase}_<site>`; review controls `_rap_srn_ctl_{f2,s2,f6}`

**Outcome.** Question: what is the network producer's variance factor, class by class, and does it make a better scale for the QC thresholds and the URA than the fixed a priori one? The code factor is measurable and stable: 1.00 on the sim, whose generator uses the assumed law, and 0.35 in sigma on the field. The phase factor is not: it reads absorption by the carried ambiguities. Applied to the URA it changes only the U records; applied to the RAIM pools it refloats 69 % more arcs and catches no clock step. Stage 1 is kept as an instrument; neither reader is recommended for arming.

## 1. The question and what the review settled (2026-11-07)

No variance factor is estimated or applied anywhere in the network producer. Every code row is weighted at 0.3 m over sin(el) and every phase row at 2 mm (3 mm on the sim) (`src/core/model/StochasticModel.cpp` 122 to 137; the network takes the variance unchanged, `src/core/network/assembly/StationRows.cpp` 178). Every acting threshold and every product sigma therefore reads in a priori units. Card SRN measured the consequences: the within-epoch sigma0 read 1.0 on 1,440 of 1,440 epochs and adapted nothing; the T4 identification declares at 6 sigma0 with sigma0 = 1; the URA is the formal clock sigma times a scale fixed at 1.

The second-model verification narrowed the finding, and the design below takes the narrowed form:

1. Only the code scale is established: about 2.4 times pessimistic in sigma (6 in variance) on two independent statistics, the standardized post-fit code residuals (robust 0.25 to 0.30, rms 0.42 without BSHM00ISR) and the estimable block-Baarda sigma0 (0.41 on the field against 0.88 to 0.90 on the sim, whose generator draws code from the assumed law).
2. The phase scale is not identifiable from unnormalized post-fit residuals. With a free per-line-of-sight slant ionosphere and equal phase weights the two phase residuals of a line of sight obey v_L1 + gamma v_L2 = 0 (correlation -0.9999 on the field), and the sim's phase statistic reads 0.31 although its generator's phase noise (0.5 mm white plus 4.4 mm zenith coloured, tau 300 s) exceeds the 3 mm a priori.
3. The within-epoch tier is inert because its trigger (3.0) is a median over the pool in the same units as the floored sigma0; it is a network-wide backstop by design, and no local fault moves a median.
4. The anchor guard's deciding statistic is the phase-space one with its own 0.02 m floor (a 0.12 m bar on 95 to 99 % of rows); its clock-space denominator projects per-satellite marginals as independent, so it is not the exact predicted-residual variance. The anchor guard is not a reader of this card.
5. The T4 flags at HKSL00HKG and BSHM00ISR are event-driven (their measure-only station factors read 0.44 and 0.22 against a median of 0.29), so a per-station factor is not the lever for them.
6. The GPS code tail (C1C rms 1.2 to 2.1 m) is one station, BSHM00ISR (C1C rms 10.9 m over 4,284 rows); without it GPS and Galileo code agree.

## 2. Design (pre-registered 2026-11-07, before any code)

### 2.1 Stage 1: the per-class factor, measure-only (`network.vcf`)

**Host.** The redundancy-census sweep (`network/qc/RedundancyCensus.cpp`, `network.redundancy_census_stride`, 10 on both goldens: one sweep per 5 minutes, plus the census's targeted escalations and trip-forced sweeps). The sweep already holds, per swept station, the local redundancy of every row, and with the block-Baarda rider armed (both goldens) the post-fit residual. The factor is a third rider on that sweep: no second sweep, no second covariance product. When neither rider forms the residual, the vcf rider forms it with the same expression.

**Rows.** Observation rows of a swept station whose tag kind is `DataCode` (the code classes) or `DataPhase` (the phase classes), with w_i > 0 and a finite residual. Rows outside the nominal stochastic model are counted, never pooled:

- `DataCodeCompanion` rows (the second code channel's companion and fallback rows, weighted at sigma times `network.second_code_sigma_scale`, 10 on the goldens) form their own reported aggregate class `code_companion`;
- `DataPhaseSecond` rows (a second phase channel) form `phase_second`;
- rows on a line of sight carrying a deweight this epoch (the unvetted-line-of-sight phase scale, `NetStation::phase_sigma_scale`; the gross-line-of-sight code scale, `NetStation::gl_code_sigma_scale`) are counted as `n_dw` and left out.

**Kernels.** For one row, v_i = y_i - a_s,i' x_s - a_r,i' x_r (`R.y - R.A_s x_s - R.A_r x_r`, the residual of `network_pf_rows.csv` and of the block-Baarda rider), w_i its information weight (`R.w`), and r_i = 1 - w_i a_i' Cov a_i its local redundancy, from `NetworkNormalEquations::rowRedundancy` (`src/core/network/solve/NormalEquations.cpp` 2691 to 2760): Cov is the datum-frame shared covariance with the exact gauges zeroed and the station's N_rr with every folded prior, so the carried priors and the datum are drawn (D6-03, recorded as sound). Per class c and swept epoch t, over its n_c rows:

- the redundancy-weighted factor: f_c = sum_i w_i v_i^2 / sum_i r_i. For a correctly weighted model E[v' W v] = sigma0^2 tr(W Q_v) = sigma0^2 sum_i r_i (Baarda 1968). Per class it is the simplified variance-component estimator (Förstner 1979): exact when all classes share one factor, and the first step of LS-VCE (Teunissen and Amiri-Simkooei 2008) without the cross-class trace terms. Code rows are about 150 times lighter than phase rows, so phase noise leaks into a code residual only through the phase-determined parameters (millimetres against decimetres); the code factor is insensitive to that approximation. It is a second moment and inherits every tail.
- the robust companion: g_c = (1.4826 med_i |v_i| sqrt(w_i / r_i))^2 over rows with r_i > 1e-6. |v_i| sqrt(w_i / r_i) is the w-test statistic in a priori units, unit-variance under a calibrated model, so g_c is consistent for the same factor under Gaussian noise and is not moved by one station's tail.
- the redundancy per row rho_c = sum_i r_i / n_c, and n_c, n_dw, n_unchk (rows with r_i <= 1e-6).

**Classes.** Code and phase by constellation and signal (GPS C1C, C1W, C2W, L1C, L1W, L2W; Galileo C1C, C1X, C5Q, C5X, L1C, L1X, L5Q, L5X; any other signal the roster admits gets its own class), the aggregates `code` and `phase` over both constellations, and the counted classes `code_companion` and `phase_second`. A factor is a variance ratio; its root u_c = sqrt(f_c) is the scale (a sigma ratio). The SRN numbers (0.29 to 0.49 per signal) are scales.

**Identifiability.** A class is flagged identifiable when rho_c >= 0.1 and n_c >= 50. The flag is necessary, not sufficient, for phase: under the free slant ionosphere the geometry-free direction of the phase pair has no redundancy and the per-signal phase factors are equal by algebra once divided by r_i (the ionosphere column's leverage is 1/(1+gamma^2) on band 1 and gamma^2/(1+gamma^2) on band 2), which is itself a check of the kernel; the redundancy that remains is the ionosphere-free direction's, supplied by the carried ambiguity priors, which also absorb the coloured part of the phase noise. A phase factor below 1 is therefore read as a measurement only where the sim, whose phase law is known, reads its own phase factor consistently with that law.

**EWMA.** Per class and statistic, e_c(t) = alpha f_c(t) + (1 - alpha) e_c(t - 1) over swept epochs on which the class pooled at least 50 rows, seeded by the first such sweep; alpha = `network.vcf_ewma_alpha` (0.1: an e-folding of 10 sweeps, 50 minutes at stride 10).

**Outputs.** `logs/network_vcf.csv`, one row per (swept epoch, class): week, tow, class, sys, signal, kind, n, sum_wv2, sum_r, rho, f, g, ewma_f, ewma_g, identifiable, n_dw, n_unchk. A run-end table per class: the final EWMAs of both statistics, the hourly medians of f and g (hours from the first sweep), the row and sweep counts and the identifiability, and the passes / sweeps / rows denominators (standing invariant 4). The carried-prior families' factors are the prior-residual census's own run-end lines (sigma0 per family), quoted beside the table in the results.

**What is not done.** The prior-residual census's `pl` numerator (D6-08, `src/core/network/carry/LevelingCarry.cpp` 841 to 856) omits sum_j m_j (s_now,j - s_prev,j) under the conditional coupling, and on the exact route the pivot-deficit terms; its denominator also differs from the fold's (dep_var + tu_var + var_add). The coupling coefficients exist only inside PASS B's coupling builder and are not retained per arc at the harvest, so an aligned numerator spans the coupling builder, the exact update, the arc entity and the harvest. That is not a contained change; it is reported and left, and the ladder is quoted as the census prints it.

### 2.2 Stage 2: the factor as the scale of three readers (`network.vcf_apply`)

Built only if stage 1's field code factor is stable by hour: the hourly median of the robust code factor within 20 % of its EWMA at the end of that hour, from hour 2, on both beds.

**The applying scales.** u_code = sqrt(e_g(code)) and u_phase = sqrt(e_g(phase)), the robust aggregate EWMAs (the redundancy-weighted factor is tail-dominated on GPS code by BSHM00ISR, finding 6 of section 1; it is reported beside). A scale applies only when its class is identifiable and has at least 3 sweeps in its EWMA; otherwise it is 1, the a priori model. The readers read the EWMA through the previous sweep (the factor is formed in SB_FRAMEDIAG, after RAIM).

**Readers.** The solve's weights and the carried variances are not touched.

1. The within-epoch sigma0 (`RaimWithinEpoch.cpp` 258 to 262): each pooled row's statistic is divided by u_phase at the push (the pool is phase observation rows and the carried-prior rows of the phase family), so sigma0 = max(floor, 1.4826 med) is in calibrated units and the trigger 3.0, `raim_k` and the A4 ceiling mean calibrated sigma. `network.raim_sigma0_floor` keeps its meaning (a floor in units of the calibrated model) and its default.
2. The post-fit T4 bar (`Raim.cpp` 1353): the post-fit pool's rows are divided by u_phase at the push, so the cells, the median polish and sigma0 are in calibrated units and the bar k = 6 is 6 calibrated sigma.
3. The URA scale (`SolPrep.cpp` 526 to 535, the EWMA folded at `Raim.cpp` 437 to 439): the EWMA's input is u_code instead of the floored post-fit sigma0, and the emitted scale is the EWMA without the max(1, ...) clamp, so URA = u_code times the formal clock sigma (the clock level is code-determined).

Each reader logs once which scale it uses. The anchor guard is left alone (section 1, item 4).

**Caveat recorded before the run.** Both RAIM pools are mostly carried-prior rows (94 % of the field post-fit pool), whose own factors the prior-residual census reports at 0.06 to 5,000 by family; dividing them by the phase observation scale is the prescription of build B1, not a calibration of those rows.

### 2.3 Gates (pre-registered 2026-11-07)

- **G0 smoke:** a 10-epoch field arm with `network.vcf` on, exit 0, the trace written.
- **G1 off identity:** the 2 h field and sim arms of the build with every new key off against the review executable's controls `_rap_srn_ctl_f2` / `_rap_srn_ctl_s2` (`gnss_processor_srn.exe`, main `adef08b`): `products.txt` without `#` lines identical, every pre-existing `logs/*.csv` identical.
- **G2 stage 1 on, 6 h field and sim:** products identical to a same-executable control; the per-class table. Expected (scales): field code 0.25 to 0.5, phase 0.3 to 0.6; sim code 1.0 (within 5 %), phase 0.2 to 0.5. The sim code reading is the kernel's calibration check: a sim code scale outside 0.95 to 1.05 fails the instrument.
- **G3 stage 2 decision:** the hour-by-hour stability rule of section 2.2 on the field and sim code factor.
- **G4 stage 2 (only if G3 passes):** quiet sim 2 h: sigma0 near 1 by class, nothing adapts, T4 refloats not above the control's; card SMX's satellite clock step bed: the anchor guard (unchanged) and the within-epoch tier read at the step epoch against the control; field 2 h then 6 h against a same-executable control: user `pos_err_3d_m` p50 by hour within 2 mm in hours 2 to 6, within-epoch adaptations and T4 refloats per hour, URA medians by hour, the 16-site census median within 0.5 mm. A failure stops at stage 1 and leaves `network.vcf_apply` UNVALIDATED.

## 3. Amendment before stage 2 (2026-11-07, written after reading the stage 1 sweeps, before any stage 2 run)

The stage 1 arms (section 4) read the code factor as pre-registered: the sim code scale is 0.99 to 1.01 by hour (the kernel's calibration check), the field code scale 0.35 to 0.36 by hour (robust) and 0.44 to 0.46 (weighted), within 4 % of its EWMA in every hour from hour 2 on both beds, so G3 passes for code and stage 2 is built. The phase factor does not read as a measurement. On the sim, whose generator's phase error (0.5 mm white plus a coloured part of 4.4 mm at zenith) exceeds the 3 mm a priori, the phase scale climbs 0.41, 0.46, 0.49, 0.59, 0.59, 0.62 by hour and fails the 20 % stability rule in hours 2 and 3; on the field it climbs 0.41 to 0.54. Its redundancy per row (0.36) passes the 0.1 bar, so the bar alone would have let it apply; by section 2.1 it does not, because the bed with a known law contradicts it. The per-signal phase factors are equal across the two bands of a constellation to three decimals (field E L1C 0.520 and E L5Q 0.519; sim G L1C 0.612 and G L2W 0.613), the algebra of the free slant ionosphere, as section 2.1 predicted.

Stage 2 is therefore built with two keys:

- `network.vcf_apply`: the URA reader only, fed the code scale (the one established factor);
- `network.vcf_apply_phase` (requires `network.vcf_apply`): the phase scale also divides the within-epoch and post-fit RAIM pools, the readers 1 and 2 of section 2.2. It is built so that the prescription of build B1 can be run and graded, not because the evidence supports it; the prediction written here is that it makes the post-fit T4 bar about twice as sensitive on both beds (6 calibrated sigma = 6 x 0.5 a priori units) and refloats more arcs, with the sim's post-ignition zero lost.

Gates G4 as pre-registered, run for `network.vcf_apply` alone (arm `apply`) and with `network.vcf_apply_phase` (arm `phase`). For `apply` the RAIM readers are unchanged by construction, so the gate reduces to: the solve and the user identical to the control (the URA is written to the products but read by no consumer on the goldens), the URA medians by hour, and the satellite clock step bed is not run (the anchor guard and the within-epoch tier are untouched). For `phase` the full G4 applies.

## 4. Stage 1 results (2026-11-07)

Executable `build/x64-Release/gnss_processor_vcf.exe` (sha256[:16] `74f3a31f43de22b8`, branch commit `9cc67b0`), arms under `D:/GnssProcessorSupport/runs_nom/_rap_vcf_*`, each a golden copy with one change; sim arms start at 00:30:15. Tables from `logs/network_vcf.csv` (hours from the first sweep, one sweep per 10 epochs, 72 sweeps in 6 h); all numbers are scales (square roots of the factors), 1 = the a priori model.

**G0 smoke** (`_rap_vcf_smoke_f01`, 12 epochs): exit 0; two sweeps written; the first epoch's phase rows have a redundancy per row of 2e-11 (the cold free ambiguities) and are flagged unidentifiable, as designed. PASS.

**G1 off identity** (`_rap_vcf_off_{f2,s2}` against the review executable's `_rap_srn_ctl_{f2,s2}`): `products.txt` without `#` lines identical (113,888 and 80,690 records); of the 34 trace files 31 identical, and the three others (`network_datum_graph.csv`, `network_run_summary.csv`, `network_service_health.csv`) identical once their wall-time columns are removed. PASS.

**G2 stage 1 on, 6 h** (`_rap_vcf_on_{f6,s6}` against the same-executable controls `_rap_vcf_ctl_{f6,s6}`): products identical (343,049 and 241,008 records), every common trace identical up to the wall-time columns, `network_vcf.csv` the only new file. The rider pooled 263,639 field rows (6 left out for a deweight, 2,034 uncheckable at r <= 1e-6). The control `_rap_vcf_ctl_f6` is itself identical to the review executable's `_rap_srn_ctl_f6`. PASS.

| bed | class | rows per sweep | redundancy per row | robust scale by hour 1 to 6 | weighted scale by hour 1 to 6 | final EWMA robust / weighted |
| --- | --- | --- | --- | --- | --- | --- |
| field | code | 1650 | 0.990 | 0.360 0.363 0.355 0.348 0.354 0.359 | 0.453 0.452 0.450 0.461 0.447 0.433 | 0.354 / 0.466 |
| field | phase | 1663 | 0.362 | 0.408 0.466 0.537 0.534 0.516 0.546 | 0.529 0.588 0.687 0.649 0.589 0.629 | 0.543 / 0.633 |
| field | code_companion | 345 | 1.000 | 0.049 0.048 0.048 0.047 0.047 0.049 | 0.053 0.054 0.053 0.052 0.052 0.053 | 0.048 / 0.055 |
| field | G C1C | 328 | 0.991 | 0.449 0.429 0.423 0.425 0.445 0.457 | 0.528 0.532 0.519 0.495 0.521 0.535 | 0.452 / 0.552 |
| field | G C1W | 150 | 0.994 | 0.412 0.388 0.384 0.378 0.395 0.383 | 0.431 0.417 0.423 0.406 0.412 0.416 | 0.391 / 0.480 |
| field | G C2W | 477 | 0.986 | 0.369 0.380 0.385 0.373 0.376 0.376 | 0.440 0.442 0.448 0.433 0.439 0.439 | 0.377 / 0.505 |
| field | E C1C | 156 | 0.994 | 0.337 0.334 0.335 0.340 0.305 0.319 | 0.421 0.412 0.487 0.516 0.390 0.369 | 0.321 / 0.391 |
| field | E C1X | 194 | 0.995 | 0.375 0.394 0.381 0.380 0.357 0.379 | 0.420 0.413 0.424 0.405 0.423 0.432 | 0.381 / 0.427 |
| field | E C5Q | 166 | 0.986 | 0.256 0.245 0.247 0.266 0.253 0.251 | 0.341 0.457 0.386 0.532 0.419 0.358 | 0.255 / 0.384 |
| field | E C5X | 194 | 0.986 | 0.292 0.324 0.301 0.302 0.314 0.291 | 0.382 0.405 0.395 0.375 0.382 0.361 | 0.300 / 0.371 |
| field | G L1C | 326 | 0.542 | 0.355 0.419 0.504 0.538 0.514 0.541 | 0.515 0.583 0.723 0.669 0.590 0.622 | 0.533 / 0.643 |
| field | G L1W | 150 | 0.541 | 0.396 0.468 0.511 0.514 0.502 0.487 | 0.484 0.553 0.603 0.616 0.586 0.583 | 0.511 / 0.598 |
| field | G L2W | 478 | 0.200 | 0.380 0.433 0.522 0.529 0.508 0.533 | 0.489 0.589 0.702 0.660 0.589 0.627 | 0.525 / 0.629 |
| field | E L1C | 170 | 0.514 | 0.409 0.470 0.548 0.521 0.508 0.519 | 0.527 0.583 0.700 0.644 0.602 0.631 | 0.533 / 0.629 |
| field | E L1X | 194 | 0.538 | 0.467 0.492 0.546 0.564 0.582 0.595 | 0.538 0.556 0.648 0.602 0.604 0.657 | 0.611 / 0.646 |
| field | E L5Q | 165 | 0.163 | 0.401 0.473 0.551 0.508 0.515 0.522 | 0.527 0.584 0.699 0.644 0.602 0.630 | 0.537 / 0.629 |
| field | E L5X | 194 | 0.167 | 0.461 0.490 0.544 0.559 0.581 0.593 | 0.538 0.556 0.648 0.602 0.604 0.657 | 0.611 / 0.645 |
| sim | code | 1774 | 0.991 | 1.014 1.009 1.010 1.012 0.993 0.994 | 1.005 0.993 0.994 0.999 1.000 0.998 | 0.997 / 0.999 |
| sim | phase | 1774 | 0.361 | 0.413 0.456 0.492 0.594 0.590 0.622 | 0.424 0.488 0.526 0.604 0.623 0.633 | 0.639 / 0.646 |
| sim | G C1C | 486 | 0.994 | 1.015 0.965 0.979 1.012 1.003 1.018 | 1.012 0.982 0.998 1.007 1.004 1.005 | 1.009 / 0.997 |
| sim | G C2W | 486 | 0.986 | 1.013 1.013 1.022 1.032 1.015 1.002 | 1.002 1.014 1.009 1.034 1.003 1.013 | 0.998 / 1.010 |
| sim | E C1C | 397 | 0.995 | 0.981 1.029 1.018 0.973 0.983 0.995 | 1.004 1.004 0.992 0.969 1.008 1.007 | 0.987 / 0.990 |
| sim | E C5Q | 397 | 0.988 | 1.002 1.025 0.985 1.001 0.979 1.009 | 1.000 0.990 1.004 1.009 0.986 1.002 | 0.990 / 0.995 |
| sim | G L1C | 486 | 0.535 | 0.393 0.435 0.487 0.572 0.563 0.587 | 0.419 0.469 0.521 0.559 0.570 0.594 | 0.612 / 0.615 |
| sim | G L2W | 486 | 0.198 | 0.395 0.431 0.489 0.566 0.558 0.584 | 0.421 0.470 0.522 0.560 0.570 0.596 | 0.613 / 0.615 |
| sim | E L1C | 397 | 0.540 | 0.423 0.492 0.516 0.640 0.634 0.659 | 0.443 0.513 0.527 0.659 0.669 0.680 | 0.673 / 0.682 |
| sim | E L5Q | 397 | 0.168 | 0.428 0.499 0.523 0.630 0.633 0.660 | 0.445 0.516 0.526 0.659 0.670 0.679 | 0.672 / 0.682 |

Readings against the pre-registered expectations:

1. **The kernel is calibrated.** The sim code scale reads 0.993 to 1.014 by hour on both statistics and 0.97 to 1.03 on every signal (expected 0.95 to 1.05): with the redundancy drawn, the factor recovers the generator's code law. The instrument check passes.
2. **Field code: 0.35 robust, 0.45 weighted** (expected 0.25 to 0.5), flat by hour. The two statistics differ by 1.27 in scale, the signature of a heavier-than-Gaussian tail (on the sim they agree to 1 %). Per signal, Galileo E5a is the quietest (C5Q 0.25, C5X 0.30) and GPS C1C the noisiest (0.42 to 0.46), the E1/E5a spread of the review (D2-02). The BSHM00ISR channel does not dominate the weighted factor at the sweep epochs (its 10.9 m rms is set by a few rows up to 416 m, which a sweep on one epoch in ten mostly misses). The second code channel's companions read 0.048 at sigma times 10, which is 0.48 of the canonical a priori: the same noise as the canonical rows (D2-03).
3. **Phase: 0.41 to 0.55 on the field, 0.41 to 0.62 on the sim, rising by hour on both** (expected field 0.3 to 0.6, sim 0.2 to 0.5: the sim reads above its expectation from hour 4). The per-signal phase factors of a constellation are equal across its two bands to three decimals, so the kernel reproduces the free-ionosphere algebra; the redundancy per row (0.36, of which the band-2 or band-5 rows carry 0.16 to 0.20) clears the 0.1 bar. The sim reads 0.4 to 0.6 although its phase error exceeds the a priori, and the scale grows as the carried ambiguities tighten: the factor tracks how much of the coloured error the float ambiguities and the free slant ionosphere absorb, not the noise level. By the rule of section 2.1 the phase factor is not a measurement.
4. **The carried-prior families** as the prior-residual census reports them (its per-family robust sigma0, a scale): field pl 0.165, pb 0.190, cb 0.035; sim pl 0.209, pb 0.216, cb 0.031 (the zwd and rxcb families are not fed on this posture). The field `pl` ladder is the one card SRN recorded (CLEAN 13.94 % above 4 sigma0, b-cell 97.9 %, the worst row MBAR00UGA G09 L2W at TOW 262380, prior tension 67): unchanged, because the numerator was not aligned (section 2.1: not contained).
5. **G3, the stage-2 precondition:** the hourly median of the robust code factor over its EWMA at the hour's end reads field 0.991, 0.981, 0.961, 0.996, 1.028 and sim 1.002, 1.002, 1.012, 1.005, 0.995 in hours 2 to 6: within 20 % on both beds. PASS: stage 2 is built (section 3). The same ratio for phase fails on the sim (0.722 in hour 2).

## 5. Stage 2 results (2026-11-07)

Executable `build/x64-Release/gnss_processor_vcf2.exe` (sha256[:16] `7159193fc9b86f35`, branch commit `eb3150b`), same-executable controls `_rap_vcf2_ctl_{f2,s2,f6}`. The controls are themselves identical to the review executable's `_rap_srn_ctl_{f2,s2}` (products, and the traces up to their wall-time columns): the off identity holds on this executable too. Grades from `network_epoch_status.csv` (`n_adapt`), `network_events.csv` (tier `T4_attrib`), the `U` records of `products.txt` and `closed_loop_trace.csv`; hours from the run start.

**Arm `apply` (`network.vcf_apply`, the URA reader only).** Field 2 h and 6 h, sim 2 h: every product record other than `U` identical to the control, the consumer identical at every epoch (the goldens' consumer reads no URA), no adaptation, the T4 refloats the control's. The URA EWMA is fed the code scale from the first warm sweep (field 0.398 at TOW 259830, sim 1.050 at TOW 261690; the sim's 1.05 is its first sweep's draw, settling to 1.00). URA median by hour:

| bed | arm | h1 | h2 | h3 | h4 | h5 | h6 | consumer 3D p50 h2 to h6 (both arms) |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| field | control | 126.2 | 65.0 | 44.3 | 35.3 | 28.6 | 23.0 mm | 0.012 0.023 0.012 0.017 0.012 m |
| field | apply | 51.2 | 24.1 | 16.1 | 12.5 | 10.1 | 8.2 mm | identical |
| sim | control | 69.3 | 39.0 | | | | mm (2 h) | identical |
| sim | apply | 72.2 | 39.6 | | | | mm (2 h) | identical |

The URA is now the code scale times the formal clock sigma: 0.37 to 0.40 of the a priori URA on the field and the a priori URA on the sim, where the code law is the assumed one. Whether 8 mm in hour 6 is the realized clock error of the product is not graded here: no truth grade of the U record exists (the sim grade of D2-08's arm G is owed).

**Arm `phase` (`network.vcf_apply_phase`, the RAIM pools also read the phase scale).**

- *Quiet sim, 2 h:* the pools read the phase scale 0.334 from TOW 261990; within-epoch sigma0 1.0 on every epoch (the pool divided by 0.33 stays under the floor), 0 adaptations, 0 T4 refloats (control 0); every record other than `U` identical to the control, the consumer identical. Nothing adapts. PASS.
- *Field, 2 h then 6 h:* the pools read the phase scale 0.453 from TOW 260130. Within-epoch adaptations 0 in every hour (both arms): sigma0 stays at the floor of 1.0, so the trigger is not reached. T4 refloats by hour 36, 6, 13, 17, 5, 16 (93) against the control's 26, 4, 11, 4, 2, 8 (55): the bar of 6 calibrated sigma is 2.7 a priori units, and the refloats rise 69 %, as section 3 predicted. The products move (clock records: median 0.5 mm, maximum 10.7 mm over 36,411 changed records); the consumer 3D p50 is identical in every hour to 0.1 mm (largest per-epoch change 0.2 mm). PASS of the 2 mm bar.
- *16-site census* (`scripts/clock_level/gcensus.py`, consumer replays of the two 6 h wires on the control's consumer, keys `vcfc_ctl` and `vcfc_phase`): median site p50 by hour 0.142, 0.050, 0.036, 0.029, 0.032, 0.030 m on both; paired hours 2 to 6 median +0.0 mm, no site better or worse by 1 cm; the largest change ARHT00ATA +2.3 mm (its uncovered-L1W mechanism of card ARHT). PASS of the 0.5 mm bar.
- *Satellite clock step* (card SMX's `sat_clock_step` injector on a fresh 2 h sim arm, G25 at TOW 264630, one hour after the 00:30:15 start, so the phase scale is warm; 0.5 m, the matrix's built step, and 0.1 m; arms `_rap_vcf2_clk{5,1}_{ctl,phase}`): the phase arm is identical to its control in every record other than `U` and at every consumer epoch. At the step epoch the within-epoch sigma0 reads 1.0 with 0 adaptations and no T4 refloat on both arms; the anchor guard sees e_clk = -0.24 mm on G25 (the step never reaches the clock innovation it tests); the 0.5 m step raises 10 IF-TD slip flags and the wire carries -0.012 m of the step at +3 epochs, -0.050 m at +10 and -0.165 m at +49 (card SMX's failure reproduced); the 0.1 m step raises no flag and the wire reads +0.008 m at +2 and -0.044 m at +49. The calibrated pools add no detection at either size, as the review's verdict on a median trigger predicted.

## 6. Verdict and what is owed (2026-11-07)

1. **Stage 1 (`network.vcf`) works and is the deliverable.** The kernel recovers the sim's code law to 1 %, and the field's code noise is 0.35 of the a priori sigma (robust; 0.45 as a second moment), flat by hour, quietest on Galileo E5a and noisiest on GPS C1C. The phase factor is not identifiable from post-fit residuals on this posture: the per-signal factors are equal by algebra, and the sim, whose phase error exceeds the a priori, reads 0.4 to 0.6 and climbs as the carried ambiguities tighten. Measure-only and byte-identical when off; ledger TRACE.
2. **Stage 2's URA reader (`network.vcf_apply`) passes every gate it can be put to** (products and users identical but for the U records, which shrink to 0.37 to 0.40 of the a priori on the field) and stays UNVALIDATED: the calibrated URA has no truth grade.
3. **Stage 2's RAIM readers (`network.vcf_apply_phase`) pass the user and census bars and should stay off.** They divide the pools by a phase scale that stage 1 showed is not a measurement, raise the field's T4 refloats 69 %, and detect nothing more on the clock-step bed; within-epoch adaptations stay at 0 because the floor still binds. Ledger KEEP_OFF.
4. **The anchor guard and the within-epoch trigger are not variance-factor problems on this posture.** A satellite clock step is absorbed by IF-TD slips and by the held clock before any of the three statistics sees it; the response belongs to cards SCK and SCR.

Owed: a truth grade of the URA on the sim (D2-08 arm G) before `network.vcf_apply` is armed; the prior-residual census's `pl` numerator and denominator aligned with the fold (a solve-adjacent card of its own, D6-08); a per-class factor of the carried priors from the same sweep (the redundancy kernel draws them; the census's per-family sigma0 is the present stand-in); if a phase factor is wanted, a design whose phase redundancy is not supplied by the carried ambiguities themselves (for example the time-differenced phase), graded on a bed whose phase law is white.
