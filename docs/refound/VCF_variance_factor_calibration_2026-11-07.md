# VCF: a per-class variance factor for the network producer

**STATUS**

- **id:** VCF
- **topic:** raim-qc
- **status:** open
- **summary:** Pre-registered 2026-11-07 from build B1 of card SRN (findings F1 and F8). Stage 1: a measure-only per-class variance factor, the redundancy-weighted sum of squared standardized residuals and its robust companion, by constellation and signal for code and phase, on the redundancy census's sweep. Stage 2, only if the field code factor is stable by hour: the factor replaces the fixed scale at three readers (the within-epoch sigma0, the post-fit T4 bar, the URA scale) behind its own key. Results pending.
- **source:** card SRN (`docs/refound/SRN_scientific_review_network_2026-11-06.md` in the worktree `D:/GnssProcessorSupport/worktrees/scirev`, branch `review/network-engine`), findings F1 and F8, build B1, sections 3.2, 3.4 and 4; the reviewer reports `D:/GnssProcessorSupport/srn/wave1.json` (D2-01 to D2-05, D2-08, D4-05, D6-01 to D6-04, D6-08, D6-09, D8-03, D8-04) and the second-model verdicts `D:/GnssProcessorSupport/srn/wave2_verdicts.json`; cards [T3 null](../cards/raim-qc/T3-NULL-DERIVATION.md), [T3 field](../cards/raim-qc/T3-FIELD-AND-COLLATERALS.md), [RPD](RPD_replay_station_prior_2026-10-01.md), [RPA](RPA_raim_pool_data_rows_2026-10-08.md), `docs/SPEC_carry_vce_apply.md`
- **dates in the text:** 2026-11-07
- **knobs / posture:** new `network.vcf` (bool, default false, measure-only), `network.vcf_ewma_alpha` (default 0.1), `network.vcf_apply` (bool, default false, requires `network.vcf`); the goldens unchanged, nothing armed
- **hashes / run dirs:** pending

**Outcome.** Pending.

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
