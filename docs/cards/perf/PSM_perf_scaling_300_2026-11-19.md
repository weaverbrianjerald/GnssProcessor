# PSM: scaling model of the network engine to 300 stations at 1 s

**STATUS**

- **id:** PSM
- **topic:** perf
- **status:** open
- **summary:** A per-stage cost model of the network engine tied to code lines at CHAIN e7bc098, calibrated on existing runs and extrapolated to 300 stations at 30 s and 1 s on three border sizes and two postures: at 1 s every case breaks, no single stage breaks first, 1 s by optimization alone stays closed; next speed packages: the stage profiler, then the chord read-out package, with gates written before measurement.
- **dates in the text:** 2026-11-19
- **knobs / posture:** reference configuration (`config/golden/golden_field_refound.json` lineage, runs_pes2 m7 and x2), adopted configuration of the five-day read (`runs_nom/_rap_ad_n147_2h/rapid.json`: `network.phase_families`, `network.phase_family_codes`, `network.ring_fix.entry_revalidate`, `network.ring_fix.pairs`, `processing.exact_epoch_time`); keys discussed: `network.trace_chain_datum`, `network.trace_dd_fractional`, `network.sbasis_values_first`, `network.redundancy_census_stride`
- **hashes named in the text** (commit / exe / byte-cert): CHAIN e7bc098; MAIN 39b263a (the 305-station report); exe gnss_processor_cha_fp1.exe sha256 da240107c091aa8c; exe gnss_processor_srnint.exe 64543d2d89994cae
- **run dirs named in the text:** `D:/GnssProcessorSupport/runs_nom/_rap_ad_n14*`, `_rap_mD_*`, `_rap_cs11_*`, `D:/GnssProcessorSupport/runs_pes2/_rap_pes2_m7*`, `_rap_pes2_x*`, `_rap_pes2_m2*`, `D:/GnssProcessorSupport/runs_avx2/_rap_avt_*`, `D:/GnssProcessorSupport/runs_avx2def/_rap_srn_*`
- **source:** package speed-scaling-model-300 of the day program of 2026-11-19, `D:/GnssProcessorSupport/gates/day_program_2026-11-19/packages/speed-scaling-model-300/` (the decision paper SCALING_MODEL_300.md, eleven CSV tables and make_tables.py, outside the repository)

**Outcome.** At 300 stations and 30 s, the two-system borders fit (reference configuration 2.4 s per epoch, adopted 3.7 s, quiet AVX2 equivalents) and the all-systems border does not (101 to 111 s). At 1 s every case breaks, and no single stage breaks first: leveling, the chain chord read-outs and assembly share first place inside their bands. The serial share alone is 1.0 s (reference) and 1.9 s (adopted), and the parallel remainder alone is 1.4 to 1.8 s, so 1 s by optimization alone stays closed. The next two packages are the stage profiler and the chord read-out package, each with gates written here.

> The tables named below (`*.csv`) and `make_tables.py` live in the package folder named under **source**, not in the repository. Results are appended to this file under dated headings.

**Ruling asked.** Approve the stage profiler (speed-stage-profiler) as the first speed package, with gates G-P1 and G-P2 of section 10, and the chord read-out package (the chain-frame observer on a cadence, then a parallel `chainChordFloats` loop with the posterior repetition count inside) as the second, with gates C1 to C3 of section 10; the typed carry hot path waits third behind its gates T0 to T2.

**Read at.** Code: CHAIN = `D:/GnssProcessorSupport/worktrees/chain`, branch chain/audit at e7bc098 (every path:line below is this tree). MAIN 5ddc422 was not needed: no stage of the model is user-engine code. Runs (under `D:/GnssProcessorSupport/`): `runs_nom/_rap_ad_n147_2h`, `_rap_ad_n147_2hne`, `_rap_ad_n143` to `_rap_ad_n147`, `_rap_adr_n146`, `_rap_mD_gf`, `_rap_mD_n146`, `_rap_cs11_n147`, `_rap_cs11_n147e`, `_rap_mD_d1`, `_rap_cs11_m1`; `runs_pes2/_rap_pes2_m7a/m7b_{f,s}_hv_io_cx_sv`, `_rap_pes2_x1_x_on3_hv_io_cx`, `_rap_pes2_x2_x_on3_hv_io_cx(_sv)`, `_rap_pes2_m2a/b_base_{f,s}_off`, `_rap_pes2_x1_x_off`; `runs_avx2/_rap_avt_*`; `runs_avx2def/_rap_srn_{f6,b6}`; 220 runs in all in `run_timing_ledger.csv`. Record numbers: the PES record sections 4.1, 4.3, 7.1, 7.7 (CHAIN `docs/refound/PES_parallel_epoch_stages_2026-10-29.md`), AV2 section 5.3, the C2 and FC-1 records, and MAIN `git show 39b263a:docs/REPORT_timing305_allon_30s_10ep.md`. No engine was run. IRD section 11 increments served: IRD-5 (ms per epoch at 60 and 300 stations) and IRD-9 (300 stations).

## 1 The question and why

The end state is 300 stations at 1 s with all systems and every frequency (memory note `project_end_state_vision`). The closed speed doors were measured on the pre-refound posture (`carry_invariant` on, a dense K x K carry, GPS only, 710 shared columns) at 27.72 s per epoch at 305 stations (MAIN 39b263a report). That carry does not exist on the present postures: D0b `dd-build` 0 ms and `install-post` 0 ms over 240 epochs (`runs_nom/_rap_ad_n147_2h/stdout.txt`, run end). The reference configuration now reads 3 994.7 ms per epoch at 305 stations configured (303 present), SSE2, loaded with 15.69 foreign logical processors (`runs_pes2/_rap_pes2_x2_x_on3_hv_io_cx_sv`, `run_timing_ledger.csv`). No record modelled cost per stage against stations, shared columns, carried columns and rate. This paper does, and it ranks the deferred speed packages.

Symbols: N stations; b shared (border) columns; n_l local columns per station; m_s data rows per station; k_s carried columns per station; c_s chord rows per station. Scenarios at 300 stations (`model_inputs.csv`): S1 GPS + Galileo dual frequency (adopted b 337 to 348, reference b 439 to 494); S2 plus BeiDou B1I/B3I (adopted 532 to 572, reference 639 to 724); S3 four systems, every frequency (one column per band, the adopted rule extended: 1 118 to 1 192; every tracked channel, the reference rule: 2 018 to 2 110). The record figure "1 000 to 1 300" for S3 holds only for one column per band.

## 2 The cost model per stage

The border formula, from the column registry (`src/core/network/driver/ApplicationNetworkAssembly.cpp:484-692`): b = sum over satellites in view of (1 clock + modelled phase identities + code identities held by at least 2 present stations). Adopted: b = 6 N_G + 5 N_E = 342 at 32 + 30 satellites, the observed maximum (`model_inputs.csv`). Reference: b = 7 N_G + 9 N_E = 449 at 32 + 25, the observed maximum at 303 stations. At 54 global stations every satellite is already in view, so stations move N at fixed b; systems and frequencies move b (inferred).

Measured dimensions, adopted 2 h (`runs_nom/_rap_ad_n147_2h`, 54 stations, 240 epochs): b median 337, n_l 73.3, m_s 63.4 data rows (the code-formulas part counts 122.7 rows of all kinds, of which 69.1 data rows: a difference of definition, inferred), k_s 47.7 (day runs 48.8 to 49.3), terms per carried column 0.976 to 0.986 b in all 11 runs read (`model_inputs.csv`).

Leading terms per epoch (the 66 rows with every term, cadence and site are in `cost_formulas.csv`):

| stage (D0/D0b bracket) | leading term | code site, CHAIN e7bc098 | threads |
|---|---|---|---|
| assembly: dense formation | N m_s (b + n_l)^2 | `src/core/network/solve/NormalEquations.cpp:1057-1061`; sparse path declined on FMA builds `:1148-1155` | parallel (`:1084`) |
| assembly: border commit | N b^2 | `NormalEquations.cpp:1140` in the loop `solve/SolveStations.cpp:239-246` | serial |
| assembly: exact coupling rows | N k_s 2 b^2, plus one map node per coefficient | `carry/CouplingExact.cpp:184-193` (dense m_new), `carry/Coupling.cpp:308`, `:398-403` | parallel produce (`assembly/StationCarry.cpp:182-200`) |
| assembly: coupling term lookups | N k_s b string-keyed finds | `Coupling.cpp:334-335` (`sharedCarryKey`, `p_of_key.find`), `include/network/carry/CouplingExact.hpp:161` | parallel produce |
| reduce | N (n_l^2 b + n_l b^2) | `NormalEquations.cpp:1404-1446`; serial right-hand side `:1422-1426` | parallel over b/8 column blocks |
| solve, cov-inverse | b^3/3, b^3 | `NormalEquations.cpp:2153`, `:2161` | serial |
| solve-glue | (4/3) b^3 eigenvalues; dense prior fold | `solve/Sbasis.cpp:599-621`, `:770-773`; `solve/Estimator.cpp:963`, `:978` | serial (Eigen pinned to one thread, `driver/ApplicationNetwork.cpp:301`) |
| pre-asm | coupling factor n_S^3/3 and inverse n_S^3, n_S about b | `CouplingExact.cpp:134` (serial), `:59-99` (parallel) | mixed |
| slip-scans | N n_los n_sig^2; pooled IF-TD driver | `slip/PassASlipScan.cpp:106`; `slip/SlipQc.cpp:1016` | parallel; serial |
| leveling | harvest N k_s (b + copies); serial stash walk | `carry/LocalCarryHarvest.cpp:214-258`, ahead `:262-285`; walk `carry/LevelingCarry.cpp:580-800` from `LocalCarryHarvest.cpp:74-79` | mixed |
| raim | N (n_l b + m_s (b + n_l)) | `qc/Raim.cpp:238-253` | serial |
| frame-diag | sweep every 10th epoch N (n_l^2 b + m_s b^2); block cofactor M recomputed per block | `qc/RedundancyCensus.cpp:256`, `:430`; `NormalEquations.cpp:2768-2771` | parallel pre-pass; serial |
| sol-prep | chord floats N [n_l b + 2 c_s (n_l^2 + b n_l + b^2)] | `ar/RingFixDecide.cpp:388-398`; `solve/ChainChordTable.cpp:109-110` | serial |
| stash-status | the same chord floats in the chain-frame observer | `solve/ChainFrame.cpp:936-944`, gate `:1066` | serial |
| dd-frac | enumeration N^2 n_sat n_sig / 2; joint covariance k_dd b^2 + k_dd^2 b / 2, cap 2000 pairs | `diag/DdFrac.cpp:234-259`, `:263-300` | serial |
| traces | 1.085 MB per epoch at 54 stations, linear in N | `logs/` of `_rap_ad_n147_2h` (260.3 MB / 240) | serial writes |

The network code has 15 OpenMP regions (list in the code-formulas part); every dense kernel outside them runs on one core because Eigen is pinned for the whole epoch loop when parallel stages are on (`ApplicationNetwork.cpp:301`).

Two structural facts carry the model. First, each carried column's coupling row is dense over the border: m_new = M_S - F g + z with F dense (`CouplingExact.cpp:184-193`), every nonzero emitted (`Coupling.cpp:398-403`); A_s nnz=340 of 342 (`_rap_ad_n147_2h/stdout.txt:1320`, TOW 259410, station 0). So formation pays m_s b^2 per station whatever the sparse path does. Second, the b^3 kernels do not grow with stations at fixed b, but they are serial and grow cubically with systems and frequencies.

## 3 Machine load

**Rule.** Written to `scratch/timing-ledger/LOAD_RULE.md` before any run was classed: quiet = `timing.txt` with other processors at most 2.5 and system load under 95 %; loaded = above either; unknown = no `timing.txt`, with a concurrency index (other engine runs overlapping at least 10 % of the run's wall, from file times over 16 877 intervals). Index 0 never promotes a run to quiet. Foreign processors = sys_load_mean x 20 / 100 minus own processors (CHAIN `scripts/cost_attrib/run_timed.ps1:44`). The file's last write is 09:11:46 while its addendum is dated 09:20; the ledger was built at 09:29, so the order holds by file time and the stated 09:20 is wrong (skeptic item 12).

**Classes** (`run_timing_ledger.csv`, 220 runs with folders): quiet 0; loaded 184 (`runs_pes2` 118, `runs_avx2` 66; 11.1 to 19.1 foreign processors, 89.5 to 100 % system load); unknown 36 (`runs_nom` 30, `runs_avx2def` 6; concurrency index 1 to 58). The only quiet runs are 15 launch lines of the PES record (0.85 to 2.34 foreign) whose folders were deleted on 2026-11-16; their loop times survive in `gates/pes/*_runs.log`, their sections only as the rows printed in PES 4.1 and 4.3.

**Load moves whole-day timings.** One setup, one exe: adopted `_rap_ad_n143`, `n144`, `n145` launched together at 16:52 (index 17 to 18) read 2 135, 2 196, 2 179 ms per epoch; `n146`, `n147`, `adr_n146` launched at 18:40 (index 9) read 1 800, 1 734, 1 772: a factor 1.27 split by launch batch. The earlier d24 setup on one exe spans 2 553 (`_rap_d24_n145`, index 12) to 4 336 (`_rap_d24_n147`, index 58): a factor 1.70. Hourly means span 1 089 to 4 645 ms per epoch. Per-minute costs of runs on different data days launched together correlate at r = 0.98 to 0.99 in wall time (101 to 104 bins; null with a 15-minute shift -0.48 to +0.44), while runs of the same data day launched apart correlate at -0.18 to +0.09 in data time (timing-ledger part, `wallclock_sync.py`, `datatime_sync.py`). The cost profile follows the launch time, not the data.

**Why shares within a run are firmer.** The within-run p10 of the whole epoch stays 15 to 26 % above the matching quiet means (`p10_check.py`); single parallel sections come close (est.addStation p10 46.1 and 46.9 against quiet 47.9). The adopted 2 h run has a D0 total p90/median of 2.85 against 1.25 for `_rap_mD_gf`; its reduce median is 23.1 ms against a mean of 73.7. Absolute levels follow the machine; the split of one epoch among stages moves less (inferred from the same tables).

## 4 Calibration

Fit rules R1 to R10 are in the header of `scratch/calibrate-extrapolate/make_tables.py`; the copy used here, with the writer's amendments W1 to W5, is `scratch/writer/calibrate_model.py`. In short: 17 stages summing to the loop wall; leading terms of `cost_formulas.csv` with parallel terms divided by S_EFF 4.45 (PES 4.3 speedups); one constant per group; fit on one exe (pes2_c5, SSE2, all loaded) at 58, 60 and 303 stations, beds weighted equally; load corrected to quiet by lambda (quiet / loaded: rest 0.700, est.addStation 0.984, reduce 0.842, frame-diag 0.621 on observed data; 0.671 overall at 303 stations); AVX2 through kappa, the per-stage median over four `_rap_avt` pairs (loop 0.824). Every fit run had 13 to 16 foreign processors, the 303-station run only 4.31 own processors, while S_EFF comes from 58-station quiet runs: thread saturation already shapes the 303-station constants (skeptic; inferred). Whether the header preceded the fit cannot be confirmed from file times (rewritten at 10:45); R7b is post hoc by its own text (skeptic item 12).

**Residuals** (`calibration.csv`, table stage_fit): loaded measured / corrected quiet / model ms per epoch / relative residual.

| stage | observed data, 58 sta (m7a f, b 415) | simulator, 60 sta (m7a s, b 280) | 303 sta (x2_sv, b 439) |
|---|---|---|---|
| assembly | 93.0 / 78.9 / 88.4 / -0.12 | 53.8 / 48.8 / 46.7 / +0.04 | 697.0 / 600.3 / 507.0 / +0.16 |
| reduce | 33.8 / 28.4 / 30.5 / -0.07 | 15.3 / 12.9 / 15.4 / -0.20 | 534.9 / 441.3 / 169.3 / **+0.62** |
| solve | 7.1 / 5.0 / 5.1 / -0.03 | 2.1 / 1.6 / 1.6 / +0.02 | 8.6 / 5.9 / 6.1 / -0.02 |
| cov-inverse | 33.2 / 23.2 / 23.9 / -0.03 | 10.1 / 7.8 / 7.3 / +0.07 | 39.5 / 27.1 / 28.2 / -0.04 |
| solve-glue | 113.7 / 79.6 / 50.9 / +0.36 | 41.0 / 32.0 / 28.1 / +0.12 | 225.8 / 154.9 / 197.9 / -0.28 |
| pre-asm | 41.2 / 28.8 / 29.4 / -0.02 | 24.1 / 18.8 / 17.5 / +0.07 | 118.1 / 81.0 / 87.2 / -0.08 |
| slip-scans | 55.1 / 38.6 / 33.3 / +0.14 | 48.7 / 38.0 / 35.9 / +0.05 | 248.4 / 170.4 / 197.0 / -0.16 |
| leveling | 55.3 / 38.7 / 47.1 / -0.22 | 49.2 / 38.4 / 36.3 / +0.06 | 972.8 / 667.3 / 252.5 / **+0.62** |
| raim | 14.0 / 9.8 / 10.6 / -0.08 | 12.9 / 10.1 / 7.8 / +0.23 | 73.4 / 50.3 / 55.6 / -0.11 |
| frame-diag | 97.7 / 60.6 / 61.7 / -0.02 | 75.0 / 48.0 / 37.9 / +0.21 | 465.2 / 282.9 / 325.7 / -0.15 |
| sol-prep | 11.2 / 7.8 / 7.2 / +0.08 | 8.1 / 6.3 / 5.3 / +0.16 | 48.2 / 33.1 / 38.0 / -0.15 |
| stash-status | 15.2 / 10.6 / 10.8 / -0.01 | 12.1 / 9.4 / 9.0 / +0.05 | 56.2 / 38.5 / 39.7 / -0.03 |
| dd-frac | 31.8 / 22.3 / 26.1 / -0.17 | 44.6 / 34.8 / 17.6 / +0.49 | 282.3 / 193.7 / 205.4 / -0.06 |
| roster | 25.0 / 17.5 / 15.3 / +0.13 | 19.1 / 14.9 / 16.5 / -0.11 | 135.0 / 92.6 / 93.0 / -0.01 |

Two stages exceed the R6 bar of 30 % at 303 stations: the reduce (A303 2.42) and leveling (A303 2.52). The skeptic's refit on per-epoch medians with the processor share divided out (`calibration.csv`, table skeptic_refit; own processors 7.01 at 58 and 4.31 at 303 from `timing.txt`) separates them: the reduce median goes 33.8 to 282.8 ms (8.37x against a formula 5.56x), share-corrected excess 0.93, consistent in N; its 303-station hourly means are 276 then 794 ms, a load excursion in hour 2. Leveling's warm net median goes 53.2 to 983.4 ms (18.5x against 5.46x), excess 3.38, not explained by processor share; on one exe without the parallel harvest (`_rap_pes2_m4a_f_off` to `_rap_pes2_x1_x_on3`) 62.2 to 996.2 (16.0x). So the reduce keeps the formula as its central value (amendment W1) and leveling keeps the anchored growth (N/59)^0.62. The leveling mechanism is unknown; one candidate is heap allocation of about 48 KB of `CoupledShared` vectors per column (kernel CPU 44 % of all CPU in m7a; inferred). The skeptic's 0.8 to 1.25 consistency rule was written after the raw ratios were seen and is not blind.

The b^3 kernels hold on the timed runs: solve 7.1 to 8.6 and cov-inverse 33.2 to 39.5 ms grow 1.16 to 1.21x against a formula 1.18x from b 415 to 439, constants within 2 % (skeptic). The border-ratio check on the two scratch profiles (`calibration.csv`, table border_ratio; exponents 2.79 to 3.26) does not discriminate: the Schur reduce, formula n_l b^2, shows the same ratio 3.19 there (skeptic item 3).

**Posture factor.** PF per stage = [T(adopted) / model] / [T(`_rap_mD_gf`) / model], central on per-epoch medians (R7b), band over the chain controls and the mean-based value (`calibration.csv`, table fit_param): assembly 1.31 (0.80 to 1.88), reduce 1.22 (1.22 to 3.36), leveling 0.67 (0.60 to 0.99), frame-diag 1.16 (0.74 to 1.23), slip-scans 1.22, pre-asm 1.47, dd-frac 1.44. For sol-prep (0.060) and stash-status (9e-5) PF calibrates the chord term, which has no reference counterpart; these are calibrations at 54 stations, not posture ratios (skeptic item 9). The adopted runs ran on exe cha_fp1 and `_rap_mD_gf` on exe S11: no same-exe adopted pair exists.

**Validation** (`calibration.csv`, table validation; model at each run's own dimensions, quiet AVX2, against measured loaded mean and median): `_rap_mD_gf` 464 against 738 / 688; `_rap_ad_n147_2h` 563 against 1 161 / 789; `_rap_cs11_n147` 815 against 942 / 906; `_rap_mD_n146` 853 against 989 / 924. The model sits 8 to 33 % under the loaded medians, inside the quiet-against-loaded gap measured on matched configurations (loaded 1.42 to 1.63 times quiet, timing-ledger part).

## 5 Extrapolation to 300 stations

Quiet AVX2 equivalents, ms per epoch at 30 s, central with the band from repeats, lambda, posture and dimension ranges (`extrapolation.csv`, table stage; the reduce on the formula, amendment W1). Every value is a projection (inferred).

| stage | ref S1 | ref S2 | ref S3 | adopted S1 | adopted S2 | adopted S3 |
|---|---|---|---|---|---|---|
| leveling | 564 (174 to 714) | 1 954 | 25 268 | 741 (215 to 1 322) | 2 562 | 20 393 |
| assembly | 432 (348 to 531) | 1 485 | 29 250 | 499 (269 to 808) | 1 904 | 17 825 |
| frame-diag | 363 (302 to 432) | 1 521 | 32 941 | 480 (276 to 562) | 2 262 | 22 003 |
| stash-status | 40 | 67 | 214 | 437 (304 to 562) | 1 694 | 16 973 |
| sol-prep | 38 | 90 | 666 | 343 (217 to 447) | 1 324 | 13 227 |
| slip-scans | 200 | 344 | 989 | 293 | 461 | 1 212 |
| reduce | 107 (81 to 335) | 367 | 7 803 | 122 (101 to 954) | 469 | 3 897 |
| dd-frac | 171 | 360 | 1 975 | 196 | 427 | 1 360 |
| solve-glue | 162 | 413 | 5 701 | 120 | 300 | 1 347 |
| pre-asm | 83 | 175 | 2 305 | 166 | 298 | 1 132 |
| solve + cov-inverse | 22 | 67 | 1 874 | 8 | 35 | 317 |
| roster | 99 | 172 | 476 | 111 | 175 | 445 |
| **total** | **2 392** (1 701 to 3 084) | **7 249** (4 779 to 9 912) | **111 133** (78 524 to 151 417) | **3 674** (2 144 to 6 436) | **12 221** (6 637 to 22 423) | **101 311** (52 535 to 189 312) |

At 1 s the per-epoch totals barely move, because only the S-basis eigenvector term is event-driven (R9): reference 2 373, 7 189, 109 487; adopted 3 674, 12 221, 101 311 (`extrapolation.csv`, table total_cell). About half of the 44 to 46 event rows per epoch are per-epoch test exceedances (prior_residual 13.9, block_outlier 4.0, mdb_exceedance 3.8 per epoch on `_rap_ad_n147`, `rate_inputs.csv`) that stay per epoch at 1 s; within-epoch replays are 0 of 240 and `n_adapt` 0 of 2 880 (skeptic item 14). No 1 s network run exists on disk to check the rate assumptions, by two searches over all 13 run roots (`rate_inputs.csv`, last two rows): search 1 read 18 416 `arm.json` files (17 155 with a network station list); none sets `gnss_rate_s` and every input file name carries the daily token 01D, 0 hits; search 2 read the epoch spacing of the run's own trace in 19 058 folders: 2 485 have a network trace, all at 30.0 s, 0 under 29 s.

The quiet-equivalent ratio adopted against reference is 1.21 at 54 to 58 stations (563 against 464 ms, validation rows) and 1.54 at 300 stations S1 (3 674 against 2 392); the adopted chain chord read-outs are most of the difference.

**Trace volume** (`extrapolation.csv`, table trace_volume): 6.0 MB per epoch at S1 300, 17 GB per day at 30 s, 521 GB per day at 1 s (6 MB/s); S3 21.4 MB per epoch, 1.8 TB per day at 1 s. D: had 494 GB free at the skeptic's check and 480 GB by df at 11:50 on 2026-11-19: a full-trace S1 day at 1 s cannot be written on this machine before compute is reached.

**Memory** (`extrapolation.csv`, table memory; model only, no run records a working set, two searches by the skeptic): per-station b x b blocks 0.27 to 0.29 GB (adopted S1) to 9.8 to 10.7 GB (reference S3); coupling rows at 112 bytes per term 0.53 to 0.56 GB (adopted S1) to 5.2 to 6.3 GB (S3). 128 GB holds all of these, but the serial commit reads N b^2 x 8 bytes per epoch, 10.7 GB at reference S3 (inferred bandwidth limit at 1 s).

## 6 Crossing table: stations at which each stage passes 1 000 ms per epoch

Central values at the high-b and low-b edge, AVX2 quiet equivalent; the formula-only and anchored edges widen these in `crossing.csv` (columns `_N_min`, `_N_max`).

| stage | ref S1 | ref S2 | ref S3 | adopted S1 | adopted S2 | adopted S3 |
|---|---|---|---|---|---|---|
| leveling | 405 to 453 (edge 1 463) | 180 to 219 | 27 to 38 | 352 to 371 (edge 1 059) | 158 to 178 | 34 to 48 |
| assembly | 610 to 794 | 171 to 241 | 9 to 11 | 575 to 629 | 143 to 174 | 15 to 19 |
| frame-diag | 737 to 934 | 168 to 234 | 8 to 10 | 600 to 652 | 121 to 146 | 12 to 16 |
| stash-status | 8 432 to 8 724 | 5 109 to 5 663 | 2 339 to 2 702 | 656 to 717 | 163 to 193 | 16 to 19 |
| sol-prep | 7 307 to 8 547 | 2 980 to 3 726 | 420 to 485 | 838 to 916 | 208 to 247 | 21 to 25 |
| reduce | 2 451 to 3 211 (edge 643) | 691 to 978 | 34 to 44 | 2 368 to 2 577 (edge 629) | 578 to 710 | 67 to 89 |
| slip-scans | 1 284 to 1 344 | 760 to 843 | 296 to 310 | 910 to 940 | 601 to 620 | 245 to 257 |
| solve-glue | 2 225 to 2 634 | 811 to 1 074 | 1 | 2 554 to 2 719 | 975 to 1 155 | 115 to 235 |
| dd-frac | 6 145 to 6 505 | 3 777 to 4 342 | 53 to 61 | 4 847 to 4 965 | 2 868 to 3 124 | 101 to 128 |

At S3 on every channel the b^3 kernels alone exceed 1 000 ms at any N (cov-inverse 1.3 to 1.7 s, solve-glue 4.9 to 6.6 s).

## 7 Breaking stage per case, with margin

Contenders are the stages whose band reaches the central value of the largest stage (amendment W2); the adopted sol-prep and stash-status are also ranked as one mechanism, the chord read-outs (`extrapolation.csv`, table total_cell, columns `contenders`, `serial_ms`, `serial_production_ms_inferred`, `parallel_remainder_ms`).

| case | total, central | largest stage and contenders | margin of the largest stage | serial share | production serial (inferred) | parallel remainder |
|---|---|---|---|---|---|---|
| ref S1, 1 s | 2 373 | leveling 564 (174 to 713) alone | -436 ms to 1 000 | 1 009 | 838 | 1 364 |
| adopted S1, 1 s | 3 674 | chord read-outs 780 (520 to 1 008), leveling 741 (215 to 1 321), assembly 499 (268 to 808), reduce 122 (high edge 954) | -220 | 1 924 | 1 026 | 1 750 |
| ref S2, 1 s | 7 189 | leveling 1 954 (565 to 2 632) | +954 | 2 091 | 1 731 | 5 098 |
| adopted S2, 1 s | 12 221 | chord read-outs 3 018, leveling 2 562, assembly 1 904 | +2 018 | 5 240 | 2 097 | 6 981 |
| ref S3, 30 s | 111 133 | frame-diag 32 941, assembly 29 250, leveling 25 268 | +2 941 to 30 000 | 16 663 | 14 689 | 94 470 |
| adopted S3, 30 s | 101 311 | chord read-outs 30 200, leveling 20 393, assembly 17 825 | +200 to 30 000 | 37 873 | 9 305 | 63 439 |

S1 and S2 fit at 30 s (largest S2 total 12.2 s against 30 s). What survives as the breaking statement: at S1 and 300 stations no single stage passes 1 000 ms; leveling, the chord read-outs and assembly overlap inside their bands, and both the serial share and the parallel remainder exceed or approach 1 s on their own. With more stations at S1, leveling crosses 1 000 ms first in the central model (352 to 453 stations), with assembly and frame-diag next (575 to 934).

**The serial floor is softer than the raw share.** The rule for the production serial column was written in the script header before the column was computed (W3): remove the chord term of stash-status (the measure-only observer `networkChainFrameObserve`, gated by `trace_chain_datum` or `trace_rpb_states`, `ChainFrame.cpp:1056-1066`; its chord memory is read only inside `ChainFrame.cpp`, lines 512, 554 and 1074, and declared at `include/network/solve/ChainFrameCounters.hpp:29`, two greps of `src` and `include`), remove dd-frac (a diagnostic), and make the sol-prep chord loop parallel. Reference S1 then falls from 1 009 to 838 ms and adopted S1 from 1 924 to 1 026 ms (the skeptic's own estimate: about 950).

**S1 at 1 s stays closed to optimization alone.** Even with the production serial floor, reference S1 needs the parallel remainder of 1 364 ms cut to 162 ms, a parallel speedup of 8.4 on top of today's 20 threads; adopted S1 has no room at all (floor 1 026 ms). Doubling cores gives 2.80 s (adopted S1) and 1.69 s (reference S1). The 1 s direction needs a change in kind or hardware on the parallel per-station terms as well as the serial cuts (skeptic item 5).

## 8 Changes in kind for the breaking stages

Checked against `closed_doors.csv` (13 doors with posture, number, load and source) and `options.csv` (21 rows). Hardware is its own column.

| stage | change | size | gives up | closed-door check | hardware |
|---|---|---|---|---|---|
| leveling | split the bracket with the stage profiler first | small | nothing | none | cores help only its parallel 83 to 94 % |
| leveling | typed identities, no string copies in the serial stash walk (`LevelingCarry.cpp:520`, `:615`, `:627`, `:791`, `:802`, `:860`) | medium | nothing if order kept | none (PES 7.1 removed only the harvest map lookups) | single core |
| assembly | exact factored coupling: one shared F plus per-station low-rank terms (`CouplingExact.cpp:148-208`, `Coupling.cpp:308`, `:398-403`) | large | byte identity (new baseline pair) | the truncated or projected form IS C2 D-b / L4 (closed); the exact form is open only under C2 section 6.1's probe (rank of Sigma_ss or of the coupling rows) | removes N k_s b^2 |
| assembly | sparse formation | n/a | n/a | CLOSED (AV2 5.3: +13.9 / +10.4 ms; rows dense by structure) | n/a |
| frame-diag | one M = N_rr^-1 N_sr^T per station per sweep, shared by every LOS block (`NormalEquations.cpp:2768-2771`) | small to medium | nothing (memcmp proof needed) | none | removes the (n_los + 1) n_l^2 b term of S2 and S3 |
| chord read-outs | `trace_chain_datum` on a cadence or off in a production profile (`ChainFrame.cpp:1066`) | config, one key | a measure-only trace | partial: the old config-stripping door (old posture, at most 8 %); a new per-scope measurement is required | none |
| chord read-outs | parallel station loop for `chainChordFloats` in RingFixDecide (`RingFixDecide.cpp:388-398`) | small to medium | nothing if gathered in station order | none | cores |
| reduce (S2, S3) | cache-blocked reduce keeping the per-column order (`NormalEquations.cpp:1404-1446`) | medium | nothing (inferred) | none: `parallel_reduce` (regrouped partials) stays closed | memory bandwidth at S2 and S3 only |
| b^3 kernels | threaded border factorization, inverse and eigenvalues (Eigen pinned at `ApplicationNetwork.cpp:301`) | small code | determinism to be shown | L1 parallel Cholesky closed at 1.06x; its reopen condition (a factorization is the wall) is met only at every-channel S3 | cores, AVX-512 |
| solve-glue | event-driven rank tracking (`Sbasis.cpp:599-621`, `RankGate.cpp:144`, `:190`) | medium | per-epoch screen of an unpredicted near-null direction | none | removes (4/3) b^3 per epoch |
| serial at 1 s | measure-only checks and traces on a cadence in seconds (`driver/EpochObservers.cpp:104-110`) | medium | latency of instruments that feed nothing | a 30 s carry harvest IS FC-1 (closed); only the measure-only part is open | none |
| serial at 1 s | dd-frac off in a production profile | config | the DD fractional trace | partial: "RT profile drops dd-frac" (old posture, unmeasured) | none |
| all | split by system or band | n/a | joint terms | CLOSED in principle (C2 L2) | n/a |
| platform | more cores, AVX-512, memory channels | platform | a new AVX-512 baseline pair | none (AVX2 default done, AV2 section 7) | i9-9820X, 10 cores, 20 logical, L3 16.5 MB, 128 GB DDR4-2666; AVX-512 assumed from the model, not probed |

Items that look new but are a closed door in other words (closed-doors-options part): a projected, truncated, low-rank or compressed coupling row or border (C2 D-b, L4; also against the exact-update rulings of CTU and EXL); a fast clock at 1 s with slow states at 30 s, or any 30 s harvest refresh (FC-1); station groups with their own borders (C2 L2); a per-thread parallel reduce (`parallel_reduce`); a blocked dense factorization at S1 or S2 (L1); dropping traces for speed without a new per-scope measurement; a sparse formation on AVX2 (AV2 5.3). Event-driven rank tracking at the adopted S3 border scales to 0.70 to 0.85 s SSE2 (about 0.6 s AVX2) for eigenvalues alone; only every-channel S3 passes 1 s, at 4.1 to 4.6 s (inferred from PES 7.7, 37 ms at order 420; skeptic item 19).

## 9 The manager's three figures, recomputed

| figure | as stated | recomputed | source |
|---|---|---|---|
| adopted integer setup against reference at one station count | 1.4 to 3.7 times | **1.1 to 1.6.** The four scout pairs are right as arithmetic (1.44 `_rap_cs11_n147` / `_rap_pes2_m7a_f_hv_io_cx_sv`; 1.78 `_rap_ad_n147_2h` / same; 3.42 `_rap_mD_d1` / `_rap_pes2_m7a_s_hv_io_cx_sv`; 3.66 `_rap_cs11_m1` / same) but mix builds, load classes, data days, and on the simulator 8 against 20 threads and no PES key against seven (`arm.json`). On one exe (S11): 1.285 and 1.312 by D0 median, 1.276 and 1.339 by loop mean (`_rap_cs11_n147`, `_rap_mD_n146` against `_rap_mD_gf`; chain setup without phase families). Adopted single-Galileo against `_rap_mD_gf` (two exes): 1.09 by median, 1.57 by mean. Model, quiet: 1.21 at 54 to 58 stations, 1.54 at 300 S1. | `calibration.csv`, table posture_pair; `scratch/writer/regen/timing-ledger/pairs_adopted_vs_reference.csv` |
| the solve itself | 2 to 9 ms per epoch | **holds as a cross-build range**: 2.43 (`_rap_ad_n147_2h`, AVX2), 4.50 (`_rap_mD_gf`, AVX2), 7.12 (`_rap_pes2_m7a_f_hv_io_cx_sv`, SSE2), 8.63 (`_rap_pes2_x2_x_on3_hv_io_cx_sv`, SSE2); 165 two-hour runs at 55 to 60 stations 1.2 to 13.0 (median 2.96). It is the LDLT and point solve only (`NormalEquations.cpp:2149-2166`). The rest of the border algebra in solve-glue is not small: 75.9, 82.5, 113.7 and 225.8 ms per epoch on the same four runs. | `calibration.csv`, table headline_solve; `run_timing_ledger.csv` |
| each carried column's coupling row: 340 to 433 terms through string-keyed lookups, share unmeasured | 340 to 433 | **the range is b, not posture.** 340 is one debug row (`_rap_ad_n147_2h/stdout.txt:1320`). Mean terms per row 331.5 at 335.6 keys in S (adopted 2 h), 413.8 at 416.1 (reference 58), 435.7 at 438.2 (305); per carried column 328.7, 410.9, 432.7 (0.975 to 0.986 b). 99.4 % of terms resolve through `sharedCarryKey` plus a `std::map<std::string,int>` find on `p_of_key` (`Coupling.cpp:334-335`, `CouplingExact.hpp:161`); 0.6 % (pinned) through `resolveSharedCol` (`Coupling.cpp:353`). Volume 847 466 to 895 577 terms per epoch (adopted), 3.71 million (305). Share unmeasured: upper bound assembly-wall 14.6 to 16.9 % of the loop (adopted), tighter bound (assembly-wall minus est.addStation) 8.7 to 10.5 %; estimate 0.3 to 4.2 % at 55 stations, 22 to 345 ms per epoch at 300 stations (inferred). | `calibration.csv`, table headline_coupling_row; `hot_path_bounds.csv`; `model_inputs.csv` |

## 10 Options for the next two packages, and the recommendation

| option | evidence on disk | cost | what it settles |
|---|---|---|---|
| stage profiler (speed-stage-profiler) | leveling grows 16 to 18.5x for 5.2x stations with no known site (`calibration.csv`, skeptic_refit); the leveling bracket is 11 to 14 % of the adopted loop and 24.4 % at 305 (`hot_path_bounds.csv`); harvest copies explain under 2 ms (hot-path part); 18 section names exist, the observer loop is untimed (critic) | 3.0 M tokens and six 2 h runs (program.json, dropped list); key in `GnssConfigLoadNetwork.cpp` per the critic | splits leveling and assembly; supplies the gate measurements of every package below |
| chord read-out package (observer cadence, parallel `chainChordFloats`, repetition count inside) | chord read-outs 131.8 of 788.7 ms per epoch median, 16.7 % (`_rap_ad_n147_2h`; sol-prep 71.5, stash-status 60.3, `scratch/writer/regen/calibrate-extrapolate/summary.json`); 2.95 and 1.95 calls per station-epoch (posterior-cache counts); both loops serial (`RingFixDecide.cpp:388-398`, `ChainFrame.cpp:936-944`); 780 ms projected at adopted S1 300 | small to medium (inferred): one config decision and one loop | removes the largest adopted serial item; carries the posterior cache's P1 count |
| posterior cache (speed-station-posterior-cache) | `reconstructStation` 11 callers at 1.000 call per station-epoch, 1.9 % of the loop (reference profile, loaded); the D0 `cov-consumers` section 12.7 % of the adopted 2 h loop; repetition unmeasured | 2.5 M tokens; same `Estimator.cpp` as the typed hot path | exact reuse if requests repeat |
| typed carry hot path (speed-typed-carry-hot-path) | 0.85 to 0.90 M terms per epoch (adopted), T0 test value 21.6 to 24.0 % on loaded runs (`hot_path_bounds.csv`); lookup share estimate 0.3 to 4.2 % (inferred) | 4.5 M tokens; after chain-resume-refactor and carried-edge-records | a constant factor on the N k_s b lookups; leaves the exact-row arithmetic and map inserts |
| change in kind: exact factored coupling | dense rows (A_s nnz 340 of 342; `CouplingExact.cpp:184-193`) | large; a new baseline pair | removes N k_s b^2 and the dense m_s b^2 part; open only under the C2 section 6.1 rank probe |

**Recommendation.** First the stage profiler, second the chord read-out package; the typed hot path third; the posterior cache inside the second package as a count, built only past P1; the factored coupling as a design probe only. The gates below were written on 2026-11-19, before any profiler measurement.

- **G-P1 (profiler accepted):** on profiler run (a) (adopted DOY 147, 2 h) and (b) (the 303-station reference), the timers attribute at least 90 % of the leveling and assembly-wall brackets, and the overhead is at most 3 % of the loop wall against the same exe with timers off.
- **G-P2 (leveling build):** if the unattributed or string-walk share of leveling is at least 30 % at 303 stations, the typed and copy-free stash walk is built next; otherwise the leveling split names the next site. The earlier gate G-P3 (blocked reduce at 2.0x its formula) is dropped: the reduce excess is load (skeptic item 1).
- **C1 (observer, config only):** if the stash-status chord read-outs are at least 3 % of the loop wall in run (a), the production reference configuration runs `trace_chain_datum` on a cadence or off (a decision for the author; the diagnostic configuration keeps it).
- **C2 (parallel chord loop):** build the parallel `chainChordFloats` loop in RingFixDecide if, with the observer off, its chord read-outs are at least 5 % of the loop wall at 55 stations in run (a), or project to at least 150 ms per epoch at adopted S1 300. Today's provisional reading on a loaded run: 9.1 % (71.5 of 788.7 ms median) and 340 ms projected.
- **C3 (acceptance):** products.txt and every CSV identical to the serial loop on run (a) and on one day run; the sol-prep wall at least 2x shorter at 20 threads.
- **P1 and P2 (posterior cache, inside the package):** count repeated requests per epoch (same function, station, columns, frame generation and x_s bytes) on run (a) with traces off; build the exact-request cache only if the repeated serial time is at least 3 % of the loop wall at 55 stations, or projects to at least 50 ms per epoch at 300 stations; the per-(station, column) memo needs the same bars at column level and a memcmp proof.
- **T0 to T2 (typed hot path):** T0 drops it if assembly-wall minus est.addStation plus leveling is under 8 % of the loop wall on run (a); T1 builds only if (ns saved per term from the disabled micro-benchmark on recorded layouts of 336 and 438 keys) x (terms per epoch) / (measured parallel efficiency) is at least 3 % of the loop wall at 55 stations or at least 50 ms per epoch at 300 stations, b 440; T2 scopes it to `p_of_key` and `q_outside` (`Coupling.cpp:378`, `CouplingExact.hpp:183`), and puts a dense S-indexed emit accumulator first if emit inserts plus exact-row arithmetic exceed twice the lookups.
- **S3 design gate:** a factored-coupling prototype only if a probe shows the coupling rows or Sigma_ss have numerical rank at most b/4 at 1e-12 relative.

## 11 What remains unproven, and the night runs that would prove each

All night runs: Release, AboveNormal, one engine at a time, launched through CHAIN `scripts/cost_attrib/run_timed.ps1` so each folder has `timing.txt`; frozen exe `D:/GnssProcessorSupport/gates/exe/x64-Release/gnss_processor_cha_fp1.exe`, sha256 da240107c091aa8c2bc3d486b3239209863f19ab90846cb3ce4987da3c2b1ba0 (read today); folders under `D:/GnssProcessorSupport/runs_nom/` named `_rap_n19_spd_*`; compared against this paper's calibration table. `timing.txt` records no memory value (`run_timed.ps1:4` lists what it writes); the peak working set needs one line added to that script by its owner (inferred: `PeakWorkingSet64` of the process).

| unproven | night run | bar |
|---|---|---|
| the adopted posture at 300 stations (PF and the leveling growth at N 300; chain election, ring-fix cap 12, cascade rows) | **N1** `_rap_n19_spd_ad303`: base `runs_pes2/_rap_pes2_x2_x_on3_hv_io_cx_sv/arm.json`, plus every key by which `runs_nom/_rap_ad_n147_2h/arm.json` differs from the reference configuration (the five keys of its `rapid.json`, `network.phase_families`, `phase_family_codes`, `ring_fix.entry_revalidate`, `ring_fix.pairs`, `processing.exact_epoch_time`, on top of the key diff between `_rap_d24_n147/arm.json` and the reference configuration), station list and data paths kept from the base, 2 h | completes; each stage inside its adopted S1 band (section 5) after lambda = 1; exploratory, no accuracy claim (DOY 129 of 2024 has 25 Galileo satellites, not 30) |
| quiet levels on the current build, replacing lambda and R7b | **N2** `_rap_n19_spd_gf` (the `_rap_mD_gf` arm alone), **N3** `_rap_n19_spd_x303` (the 303-station reference alone), **N4** `_rap_n19_spd_ad2h` (the `_rap_ad_n147_2h` arm alone) | foreign processors at most 2.5; quiet loop medians within 15 % of the model at each run's own dimensions (`calibration.csv`: validation rows for N2 and N4, stage_fit model column times kappa for N3) |
| a larger border | **N5** `_rap_n19_spd_b6`: the `runs_avx2def/_rap_srn_b6` arm alone (b 611), 2 h | b^3 constants within 10 % of the b 415 fit; n_l^2 b cofactor term at n_l 76 |
| the hot-path and leveling shares | **N6** profiler run (a) plus the disabled micro-benchmark, after the profiler key lands | gates G-P1, G-P2, T0, T1 |
| the trace share and the production profile | **N7** `_rap_n19_spd_prod`: the N4 arm with `trace_chain_datum` and `trace_dd_fractional` off (exploratory: two keys) | the stash-status and dd-frac walls fall by the model's chord and dd-frac terms within 20 % |

What no night run settles: the S3 border at 1 100 to 2 100 columns (no run exceeds b 618; a four-system network at size is needed), the event rates at 1 s (no 1 s network run exists), and the cost of one string-keyed lookup (needs the micro-benchmark build).

## 12 Skeptic corrections

Each item of `scratch/skeptic/corrections.csv` and what it changed here.

1. **Reduce excess in N is load** (item 1): R6 kept as an edge only (W1); reduce at reference S1 107 ms instead of 259; G-P3 dropped; the blocked reduce kept for S2 and S3 only.
2. **Leveling grows faster than its formula** (item 2): confirmed and kept; mechanism stated as unknown with one inferred candidate.
3. **Border-ratio test does not discriminate** (item 3): marked in `calibration.csv`; the b^3 claim rests on the timed runs.
4. **No single first stage at S1** (item 4): section 7 lists contenders and ranks the chord read-outs as one mechanism (W2).
5. **Serial floor softer** (item 5): production serial column added (W3): 838 and 1 026 ms.
6. **S1 at 1 s closed to optimization alone** (item 6 in the file, item 5 in the text): stated in section 7 and in `options.csv` and `closed_doors.csv`.
7. **Multi-rate option contradictory** (item 7): restated; the 30 s harvest is FC-1.
8. **Posture factor** (items 8 and 9): stated as 1.1 to 1.6; the scout's 1.4 to 3.7 replaced.
9. **Solve figure** (item 10): kept as a cross-build range, with the solve-glue remainder added.
10. **Coupling rows 340 to 433** (item 11): the range is b; 340 is one debug row.
11. **Covariance read-out call jump 433 to 3 757** (item 12): cold to warm, not posture; linear in N through k_s (inferred).
12. **Chord passes two, not three** (item 13): noted in `cost_formulas.csv` (column writer_note); absorbed by the PF calibration at 54 stations.
13. **Events split** (item 14): noted in `cost_formulas.csv` and section 5; R9 unaffected.
14. **Trace disk** (item 15): a test-run limit before compute; D: free space read again (480 GB).
15. **Memory unvalidated** (item 16): marked in `extrapolation.csv`; peak working set added to the night runs.
16. **Lambda mixes exes** (item 17): stated; N2 to N4 replace it.
17. **Rule timestamps** (item 18): stated in sections 3 and 4.
18. **Rank tracking at S3** (item 19): corrected in `candidates.csv` and section 8.

Disagreement kept: none on numbers. The skeptic's production-serial estimate (about 950 ms) and this paper's rule-based 1 026 ms differ by how the parallel chord loop is credited (S_EFF 4.45 here); both leave adopted S1 at about 1 s of serial work.

## Files and how they were made

Beside this paper: `cost_formulas.csv`, `model_inputs.csv`, `rate_inputs.csv`, `run_timing_ledger.csv`, `calibration.csv` (tables stage_fit, fit_param, load_lambda, validation, border_ratio, skeptic_refit, posture_pair, headline_solve, headline_coupling_row), `extrapolation.csv` (tables stage, total_cell, trace_volume, memory), `crossing.csv`, `closed_doors.csv`, `candidates.csv`, `options.csv`, `hot_path_bounds.csv`, and `make_tables.py`. `python make_tables.py` copies each part's scripts into `scratch/writer/regen/`, reruns them on the run folders (timing ledger, model inputs, cost formulas, hot-path counts, closed doors), runs the amended calibration `scratch/writer/calibrate_model.py`, and writes the eleven CSVs; it prints one line per step and one line per CSV with its row count and md5 (log `scratch/writer/make_tables.log`). Runs of the script (logs and md5 lists in `scratch/writer/`): the first full run (1 060 s) reproduced the parts' timing ledger cell for cell (220 runs, 0 differing cells); the second full run (1 426 s) gave the same md5 for the seven tables whose assembly code had not changed in between; two assembly-only runs gave identical md5 for all eleven; the final full run with the final script (1 000 s) printed the same row count and md5 for all eleven tables as the assembly run before it (`scratch/writer/md5_run4.txt` against `md5_run5.txt`: identical). Prose cells inherited from the parts pass through a plain-word rule (`SCRUB` in `make_tables.py`); identifiers keep their spelling.

## Decisions for the author

1. **Next two speed packages.** Recommendation: the stage profiler first (gates G-P1, G-P2), the chord read-out package second (gates C1 to C3, with P1 and P2 inside); the typed carry hot path third behind T0 to T2.
2. **Production profile of the chain-frame observer.** Recommendation: allow the production reference configuration to run `trace_chain_datum` on a cadence or off if C1 is met; the diagnostic configuration keeps it every epoch.
3. **Night set for quiet calibration (N1 to N5, N7).** Recommendation: run N2 to N4 first, one engine alone, since every constant in this paper carries foreign load; N1 next, as the only test of the adopted posture at 300 stations.
4. **The 1 s direction.** Recommendation: record that 1 s at 300 stations is closed to optimization alone on every border (the model agrees with the MAIN 39b263a verdict); speed work aims first at 300 stations at 30 s with margin on S2, and at the changes in kind that remove per-station terms (factored coupling under its rank probe) for 1 s.
5. **Peak working set in `run_timed.ps1`.** Recommendation: add it before the night set, so the memory table can be checked.

## Not checked

- No quiet run of any current exe exists; every absolute time carries foreign load, and the lambda corrections mix exes (pes_c4 quiet, pes2 loaded).
- No same-exe adopted against reference pair; no adopted run above 60 stations; no BeiDou or four-system network at size; S2 and S3 per-station sizes and k_s are rule projections.
- The cause of the leveling growth in N; the share of the string-keyed lookups; whether chord requests repeat between RingFixDecide and ChainFrame (`form_configured_pairs` defaults to false, `src/core/config/GnssConfigLoadNetwork.cpp:1651`, but the two take `tree_arcs` from different sources).
- The cost of rebuilding the OpenMP team (a code comment states about 9 ms per team-size change, `NormalEquations.cpp:1428-1431`; not verified).
- Event rates at 1 s; GLONASS (no run read; its inter-frequency bias is a per-station term outside the border).
- Search 1 for 1 s runs is weak on its own (no `arm.json` sets `gnss_rate_s`; the rate comes from the data); search 2 carries the absence.
- The emission of Galileo member copies in `network_solution.csv` (116 to 120 rows above the border count) is inferred, not traced to a line.

---

## Filing note, 2026-11-20

Filed on 2026-11-20 from the package draft named under **source**, unchanged apart from the removal of its
integrator section. The ruling it asks (the stage profiler first, then the chord read-out package) is the author's.
