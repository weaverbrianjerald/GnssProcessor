# VIN: validation instruments (the consumer's formal sigma, a convergence grade, the headline and the sim start)

**STATUS**

- **id:** VIN
- **topic:** harness
- **status:** closed
- **summary:** Build B5 of card SRN (findings F5 and F12). Measure-only: four formal-sigma columns appended to `closed_loop_trace.csv` (both writers, one shared schema), a position NEES grader, a convergence grader, the co-location distances and the census beside the held-out user, and proposed text for CLAUDE.md and the sim golden's start. No estimator change; no knob. Closed 2026-11-07: all four gates pass; the position covariance is consistent at both users in hours 2 to 6; the census converges in float-PPP time (median 30.5 min to 0.1 m, 94 min to a sustained 0.05 m); the sim pair of record starts inside the start artifact; nothing armed, CLAUDE.md and the manifest unchanged.
- **source:** card SRN (`D:/GnssProcessorSupport/worktrees/scirev/docs/refound/SRN_scientific_review_network_2026-11-06.md`, F5, F12, build B5) with the second-model verdicts D9-02, D9-03 and D9-07 (`D:/GnssProcessorSupport/srn/wave2_verdicts.json`); cards [MSC](MSC_multi_site_consumer_2026-09-21.md) (the census is the standing instrument), [IRD](IRD_integer_recovering_datum_2026-10-13.md) bar K-c (the convergence bars never adopted), [NIS](NIS_consumer_innovation_screen_2026-10-17.md) section 8 item 2 (the consumer's formal covariance is uncalibrated against its innovation spread)
- **dates in the text:** 2026-11-07
- **knobs / posture:** none; the field and sim goldens unchanged (sim arm with `network.start_offset_hours` 0.504167)
- **code:** `include/realtime/ConsumerTrace.hpp`, `src/core/realtime/ConsumerTrace.cpp` (the schema), `StateManager::getPositionCovariance`, `RealTimeEngine::getPositionCovariance`, the writers in `src/core/network/emit/EmitStore.cpp`, `src/core/network/diag/RunTraces.cpp` and `src/core/app/ApplicationConsumerReplay.cpp`, `tests/test_consumer_trace.cpp`; graders `scripts/consumer_nees.py`, `scripts/convergence_grade.py`, `scripts/consumer_colocation.py`, `scripts/clock_level/gcensus.py` (`--convergence`, `--nees`)
- **hashes / run dirs:** branch `worktree-wf_c49deb5b-2f3-4` from main `adef08b`; exe `gnss_processor_vin.exe` (Release of the branch, sha256[:16] `a761b2682e73eb4c`); arms `D:/GnssProcessorSupport/runs_nom/_rap_vin_{f0,s0}` (smokes), `_rap_vin_f6`, `_rap_vin_s6` (from 00:30:15), `_rap_vin_s6z` (sim from 00:00:30); controls `_rap_srn_ctl_{f6,s6}` (`gnss_processor_srn.exe`, main `adef08b`); census `_rap_bofc_ctl_<site>` (wire `bofc_src_ctl129`); baseline `C:/GnssProcessor/runs/_rf0hv17_{field,sim}6`
- **gates (frozen at pre-registration):** section 3

**Outcome.** Question: how does the consumer's formal position sigma compare with its actual error, how fast does a cold consumer converge, and how representative is the held-out user? Built: the formal sigma in the consumer trace (four columns, products and other traces identical) and three graders. The position NEES is consistent at the field and sim users in hours 2 to 6 and mildly over-confident in hour 1. The census needs a median 30.5 min to reach 0.1 m and 94 min to hold 5 cm (ARHT00ATA never); WTZZ00DEU 6 and 40 min, best of 17, co-location a candidate cause only. The sim pair of record starts inside the start artifact. Proposed, not applied: CLAUDE.md baseline text, the sim golden's start pin.

> Results are appended TO THIS FILE, below the text.

---

## 1. Questions

1. How does the consumer's actual position error compare with its own formal covariance, per hour and per axis, at the field user and the sim user (finding F5, verdict D9-07)? No consumer sigma is written today, so the ratio has never been graded.
2. How long does a cold consumer take to converge on the day-129 control wire, at the held-out user and over the 16-site census (F5, D9-03)? The reviewer's figures (median 28.8 min to the first epoch under 0.1 m, p90 82, max 101; 77.5 min to a sustained 10 minutes under 0.05 m) are to be reproduced or corrected.
3. How far is WTZZ00DEU from the roster, and how does it read beside the census (F5, D9-02)? The verdict corrects the review's count: 1.6 m to WTZR00DEU, 67 m to WTZS00DEU, 7 roster stations within 350 km and 11 within 460 km.
4. What does starting the sim golden at 00:30:15 change for the numbers of record (F12)?

## 2. Design

### 2.1 Formal sigma in the consumer trace

- The consumer is a `RealTimeEngine`. Its position is the first three elements of the `StateManager` state vector and the reported position is `ppp_state_.position`, synchronized from that vector at the end of every epoch (after an accepted AR fix, from the fixed state). The matching formal covariance is the 3x3 block of the `StateManager` covariance at `getPositionIndex()` (after a fix, the fixed covariance).
- New read-only accessors: `StateManager::getPositionCovariance()` (the block, no full copy) and `RealTimeEngine::getPositionCovariance()`.
- New shared schema `include/realtime/ConsumerTrace.hpp` / `src/core/realtime/ConsumerTrace.cpp`: the header, a row struct, the AR gate label, the ECEF-to-local rotation of the covariance and a pure formatter. The two writers (the network emit/store seam, `src/core/network/emit/EmitStore.cpp`, and the consumer replay, `src/core/app/ApplicationConsumerReplay.cpp`) duplicated the header literal, the gate switch and the row format; both now call the shared code (one owner of the schema).
- Columns appended after `p_succ`: `sig_e_m,sig_n_m,sig_u_m,sig_3d_m`. The local frame is the error's frame (east, north, up at the truth coordinate, `Coordinates::enuToEcefMatrix`); `P_enu = R^T P R`; `sig_3d_m` is the root of the covariance trace (rotation invariant). Precision 4 dp, as the error columns.
- The first fourteen columns keep their bytes (same format specifiers; a unit test pins a legacy line). Readers: every reader under `scripts/` and `tests/` was checked for positional parsing; all read by name or by index below 14 or test `len(row) >= n`, so an appended column breaks none. The byte-identity gates (`scripts/nom_td_gate.py`, `scripts/prm_ab.py`) compare `closed_loop_trace.csv` whole: across the VIN boundary (a pre-VIN reference executable against a post-VIN candidate) they report the file as different by the four trailing columns; same-build comparisons are unaffected.

### 2.2 Graders

- `scripts/consumer_nees.py <run_dir>`: per hour (from the first consumer epoch) and overall, per axis the mean of z^2 = (e / sigma)^2, the diagonal NEES (sum over E, N, U of z^2; its expectation is 3 under consistency whatever the cross-correlations, since E[e_i^2] = sigma_i^2 per axis), and the ratio of the rms error to the median formal sigma per axis. The cross-covariances are not written, so the full-matrix NEES is not available; the diagonal form has the same mean. `--selftest` checks a synthetic consistent trace (NEES about 3, ratio about 1) and an over-confident one (sigma halved: ratio about 2, NEES about 12).
- `scripts/convergence_grade.py <run_dir | census prefix | directory of runs>`: per `closed_loop_trace.csv`, the time from the first consumer epoch to (a) the first epoch under 0.1 m 3D, (b) the start of the first 10-minute run under 0.1 m, (c) the start of the first 10-minute run under 0.05 m, and (d) the 3D p50 and p90 pooled over hours 2 to 6 of the run's own clock. A run is consecutive rows under the threshold with no row gap above 1.5 cadences; its length convention is `span` (last minus first epoch at least 600 s: 21 epochs at 30 s) by default, `count` (rows times cadence at least 600 s: 20 epochs) on request. A site that never converges is "never" and counts as +inf in the percentiles. Over a census: the median and the nearest-rank p90 across sites, and the per-site table. `--selftest` checks the definitions on synthetic traces.
- `scripts/clock_level/gcensus.py grade ... --convergence` and `--nees` call the two graders' functions per site, so each statistic has one owner.
- `scripts/consumer_colocation.py <arm.json>`: the distance from the consumer's coordinate to every roster station of the config, nearest first, and the counts within given radii.

### 2.3 Runs

- Release build of this branch, renamed copy `gnss_processor_vin.exe`; 10-epoch smokes; then 6 h copies of the field golden (`vin_f6`) and of the sim golden with `network.start_offset_hours` 0.504167 (`vin_s6`).
- Identity against the review executable's control arms `_rap_srn_ctl_f6` and `_rap_srn_ctl_s6` (`gnss_processor_srn.exe`, main `adef08b`, the same source as this branch apart from the trace change): `products.txt` with `#` lines stripped identical, every pre-existing `logs/*.csv` identical, and `closed_loop_trace.csv` identical in its first fourteen columns.
- The NEES graded per hour at both users. The convergence grader on `D:/GnssProcessorSupport/runs_nom/_rap_bofc_ctl_<site>` (17 replays) and on `C:/GnssProcessor/runs/_rf0hv17_field6`.

## 3. Gates (frozen)

- **G1, schema.** Debug `gnss_tests.exe`: the new suite `ConsumerTraceSchema` passes and the rest of the suite stays at 0 failures (3,833 cases before this card, 33 skipped).
- **G2, identity.** On both 6 h arms: `products.txt` (without `#` lines) and every pre-existing `logs/*.csv` byte-identical to the review executable's control; `closed_loop_trace.csv` identical in columns 1 to 14 and carrying the four new columns on every row.
- **G3, grader selftests.** `consumer_nees.py --selftest` and `convergence_grade.py --selftest` pass.
- **G4, reproduction.** The convergence grader's census medians are compared with the reviewer's (28.8 / 82 / 101 min; 77.5 min) and with verdict D9-03 (30.5 min and 94 min over the 16 census sites with ARHT00ATA counted as never); the card states which convention reproduces which number.

Reading of the NEES (frozen convention, not a bar). Errors are strongly time-correlated, so a chi-square interval on the epoch count would be meaningless; the reading is by the ratio r = rms(e) / median(sigma) per axis: **consistent** for 0.5 <= r <= 2, **over-confident** for r > 2 (formal sigma too small), **pessimistic** for r < 0.5. The error is against the published coordinate, so a frame or coordinate bias (millimetres) enters the actual error; at a decimetre-converging float consumer it is small against hour 1 and comparable to the settled error, which the card reads beside the ratio.

---

## 2026-11-07: results

### 4. Convergence (questions 2; gate G4)

`py -3.14 scripts/convergence_grade.py D:/GnssProcessorSupport/runs_nom/_rap_bofc_ctl` (the 17 replays of the day-129 control wire `bofc_src_ctl129`, one cold start at 00:00 per site, 6 h, 720 rows each, BADG00RUS 719); minutes from the first consumer epoch; p50 and p90 in metres over hours 2 to 6 of the run's own clock (pooled epochs). The `count` convention gives the same table on this census.

| site | first < 0.10 m | 10-min run < 0.10 m | 10-min run < 0.05 m | p50 h2-6 | p90 h2-6 |
| --- | --- | --- | --- | --- | --- |
| WTZZ00DEU (held out) | 6.0 | 17.5 | 39.5 | 0.0128 | 0.0213 |
| ANMG00MYS | 46.5 | 46.5 | 63.5 | 0.0537 | 0.0686 |
| ARHT00ATA | never | never | never | 0.2194 | 0.3699 |
| ASCG00SHN | 27.5 | 28.5 | 39.5 | 0.0301 | 0.0672 |
| BADG00RUS | 6.0 | 17.5 | 40.5 | 0.0243 | 0.0396 |
| BAKE00CAN | 30.0 | 30.0 | 35.5 | 0.0162 | 0.0288 |
| CEDU00AUS | 24.5 | 24.5 | 169.5 | 0.0383 | 0.0650 |
| CHTI00NZL | 89.5 | 89.5 | 100.5 | 0.0301 | 0.0949 |
| DAV100ATA | 37.0 | 37.0 | 266.0 | 0.1214 | 0.2611 |
| HARB00ZAF | 32.0 | 32.0 | 124.0 | 0.0381 | 0.0582 |
| KOKB00USA | 74.5 | 77.0 | 87.5 | 0.0526 | 0.0767 |
| POHN00FSM | 101.0 | 101.0 | 130.5 | 0.0479 | 0.1525 |
| QUIN00USA | 23.0 | 23.0 | 63.0 | 0.0263 | 0.0378 |
| SCRZ00BOL | 26.0 | 69.0 | 114.0 | 0.0442 | 0.0810 |
| STPM00SPM | 12.5 | 22.5 | 33.5 | 0.0189 | 0.0525 |
| UTQI00USA | 14.0 | 27.0 | 67.5 | 0.0475 | 0.0784 |
| WUH200CHN | 31.0 | 48.0 | 192.0 | 0.0490 | 0.0693 |

Census (the 16 sites, WTZZ00DEU not pooled, ARHT00ATA as never = +inf, median and nearest-rank p90):

| statistic | median | p90 | max | never |
| --- | --- | --- | --- | --- |
| first epoch < 0.10 m (min) | 30.5 | 101.0 | never | 1 |
| first 10-min run < 0.10 m (min) | 34.5 | 101.0 | never | 1 |
| first 10-min run < 0.05 m (min) | 94.0 | 266.0 | never | 1 |
| hours 2-6 3D, site p50 (m) | 0.0412 | | | |
| hours 2-6 3D, site p90 (m) | 0.0689 | | | |

The baseline pair's field run `C:/GnssProcessor/runs/_rf0hv17_field6` (the in-process consumer at WTZZ00DEU, producer and consumer starting together at 00:00): 6.0 min to the first epoch under 0.1 m, 17.5 min to a 10-minute run under 0.1 m, 40.5 min to one under 0.05 m, hours 2 to 6 p50 0.0138 m and p90 0.0246 m. The review executable's control `_rap_srn_ctl_f6` gives the same numbers (its consumer trace equals the baseline's).

**Reproduction (G4).** Both sets of earlier numbers reproduce exactly, each under its own pooling:
- The reviewer's figures (28.8 / 82 / 101 min to the first epoch under 0.1 m; 77.5 min to a sustained 0.05 m) are the median, the linearly interpolated p90 and the maximum over 16 consumers made of WTZZ00DEU and the 15 census sites that converge, ARHT00ATA left out: sorted first-epoch times 6, 6, 12.5, 14, 23, 24.5, 26, 27.5, 30, 31, 32, 37, 46.5, 74.5, 89.5, 101 give median 28.75, interpolated p90 82.0 and max 101; the run times under 0.05 m give median (67.5 + 87.5) / 2 = 77.5 under the `span` convention.
- Verdict D9-03's 30.5 and 94 min are the census of the 16 sites with ARHT00ATA counted as never and WTZZ00DEU not pooled, which is this grader's default. This card adopts that convention: the held-out user is not a census member, and a site that never converges is the worst site, not a missing one.

Reading. The census converges in float-PPP time: half the sites take more than half an hour to reach 0.1 m and more than an hour and a half to hold 5 cm for ten minutes; at the p90 the times are 1.7 and 4.4 hours, and one site in 16 never reaches 0.1 m in 6 h. The times are joint producer-ignition plus user-convergence times: each replay starts at the wire's own start, whose first hour is the producer's ignition, so the user's own convergence on a settled wire is not separated here (a consumer started at hour 3 of the wire, as in card IRD bar K-c, would measure it).

### 5. The held-out user beside the census (question 3)

**Co-location** (`py -3.14 scripts/consumer_colocation.py`, the roster `network.stations[].position_ecef` of `config/golden/golden_field_refound.json`, 58 stations, against `network.closed_loop.station.position_ecef`): WTZR00DEU 1.6 m (chord), WTZS00DEU 67.1 m, GOP600CZE and GOPE00CZE 162.4 km, LEIJ00DEU 248.4 km, GRAZ00AUT 302.0 km, FFMJ00DEU 322.1 km, POTS00DEU 360.1 km, WROC00POL 370.6 km, PTBB00DEU 389.9 km, BOR100POL 457.2 km, then SVTL00RUS at 1655 km. Within 350 km (great circle): 7 stations; within 460 km: 11; within 1000 km: 11. This confirms verdict D9-02 and corrects the review's count of 14.

Nearest roster station of each census site (great circle): KOKB00USA 0.0 km (KOKV00USA), ARHT00ATA 3.1 km (SCTB00ATA), STPM00SPM 280 km, BADG00RUS 556 km, BAKE00CAN 629 km, QUIN00USA 686 km, UTQI00USA 802 km, WUH200CHN 905 km, SCRZ00BOL 1008 km, CHTI00NZL 1099 km, HARB00ZAF 1138 km, ANMG00MYS 1215 km, ASCG00SHN 1294 km, CEDU00AUS 1364 km, DAV100ATA 1403 km, POHN00FSM 1452 km.

**The census beside WTZZ00DEU** on the day-129 control wire, by the standing census statistic (`gcensus.py grade bofc_ctl`: per site the mean over hours 2 to 6 of the hourly 3D p50, hours from 00:00): WTZZ00DEU 0.0133 m; census median 0.0438 m, nearest-rank p90 0.1293 m (DAV100ATA), max 0.2254 m (ARHT00ATA); median site p50 by hour 0.136 0.054 0.041 0.036 0.040 0.037 against WTZZ00DEU 0.057 0.012 0.018 0.010 0.015 0.011. WTZZ00DEU ranks first of 17 replayed consumers; BAKE00CAN is second at 0.0166 m.

Reading. The held-out user is 3.3 times better than the census median on this day and converges five times faster to 0.1 m. Co-location is a candidate cause and not an established one: BAKE00CAN reaches 0.017 m with its nearest roster station 629 km away, BADG00RUS also reaches 0.1 m in 6 minutes at 556 km, while the two co-located census sites read 0.048 m (KOKB00USA, 0.0 km) and 0.225 m (ARHT00ATA, 3.1 km, the worst). Verdict D9-02 also records that on DOY 130 WTZZ00DEU sits near the census median (0.039 against 0.0375 m), and that the golden consumer receives no network atmosphere (the SH-VTEC model that `consume_network_iono` acts through is not built when `estimate_iono_sh` is false), so a co-located user gains only through the realization of the satellite clock, phase and code biases along nearby lines of sight. Card MSC (2026-09-21) already ruled WTZZ00DEU unrepresentative and made the census the adoption instrument; what this card adds is that the statement of record should carry the census median, its p90 and the convergence medians beside the held-out user (section 6.1).

### 6. Formal against actual at the two users (question 1; gates G2, G3)

**Identity (G2: PASS on both arms).** Against the review executable's controls (`_rap_srn_ctl_f6`, `_rap_srn_ctl_s6`, `gnss_processor_srn.exe`, main `adef08b`): `products.txt` without `#` lines identical (field 343,050 lines, sim 241,009); the same 34 `logs/*.csv` in each run, 33 byte-identical once the wall-clock columns that `scripts/prm_ab.py` names in `VOLATILE_COLS` are dropped (`network_datum_graph.csv` `ms`, `network_run_summary.csv` `loop_ms` and `ms_per_epoch`, `network_service_health.csv` `ms_per_epoch_window`); `closed_loop_trace.csv` identical in columns 1 to 14 over its 722 lines and 18 columns wide on every row. Exit 0 on both (field 5,319 s, sim 4,223 s wall on a loaded machine).

**Selftests (G3: PASS).** `consumer_nees.py --selftest` (9 checks; the over-confident case uses sigma times 0.4, ratio 2.5, NEES 18.75, rather than the halved sigma of section 2.2, which would sit on the reading boundary r = 2), `convergence_grade.py --selftest` (11 checks), `consumer_colocation.py --selftest`.

**The NEES per hour** (`py -3.14 scripts/consumer_nees.py <run>`; hours from the consumer's first epoch; z2 the mean of (e / sigma)^2 per axis, NEES their sum, r = rms(e) / median(sigma), sigma the median formal sigma in metres).

Field, `_rap_vin_f6` (WTZZ00DEU, the golden from 00:00):

| hour | z2 E | z2 N | z2 U | NEES | r E | r N | r U | sigma E | sigma N | sigma U | reading |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| 1 | 0.97 | 1.60 | 1.75 | 4.32 | 2.58 | 4.78 | 9.71 | 0.0315 | 0.0178 | 0.0263 | over-confident |
| 2 | 0.97 | 1.35 | 0.26 | 2.59 | 0.91 | 1.12 | 0.56 | 0.0083 | 0.0078 | 0.0134 | consistent |
| 3 | 3.33 | 2.76 | 1.13 | 7.22 | 1.84 | 1.63 | 1.12 | 0.0060 | 0.0063 | 0.0168 | consistent |
| 4 | 1.07 | 2.71 | 0.20 | 3.98 | 1.07 | 1.67 | 0.47 | 0.0055 | 0.0060 | 0.0122 | pessimistic |
| 5 | 1.22 | 3.65 | 0.42 | 5.30 | 1.12 | 1.94 | 0.63 | 0.0054 | 0.0069 | 0.0131 | consistent |
| 6 | 0.62 | 2.78 | 0.20 | 3.60 | 0.77 | 1.69 | 0.42 | 0.0046 | 0.0061 | 0.0128 | pessimistic |
| all | 1.37 | 2.47 | 0.66 | 4.50 | 5.84 | 5.30 | 7.87 | 0.0058 | 0.0068 | 0.0133 | over-confident |

Sim, `_rap_vin_s6` (SZZ000SIM, the golden from 00:30:15, first consumer epoch 00:31:00):

| hour | z2 E | z2 N | z2 U | NEES | r E | r N | r U | sigma E | sigma N | sigma U | reading |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| 1 | 1.75 | 2.00 | 1.79 | 5.54 | 7.94 | 6.66 | 1.94 | 0.0311 | 0.0194 | 0.0748 | over-confident |
| 2 | 1.04 | 0.98 | 1.93 | 3.94 | 0.97 | 0.97 | 1.43 | 0.0116 | 0.0105 | 0.0271 | consistent |
| 3 | 1.34 | 0.83 | 3.10 | 5.26 | 1.16 | 0.92 | 1.78 | 0.0092 | 0.0086 | 0.0213 | consistent |
| 4 | 0.48 | 0.27 | 0.81 | 1.57 | 0.70 | 0.52 | 0.89 | 0.0081 | 0.0085 | 0.0192 | consistent |
| 5 | 0.83 | 0.73 | 0.76 | 2.32 | 0.91 | 0.83 | 0.86 | 0.0080 | 0.0083 | 0.0203 | consistent |
| 6 | 0.30 | 0.66 | 0.76 | 1.72 | 0.54 | 0.81 | 0.86 | 0.0081 | 0.0082 | 0.0215 | consistent |
| all | 0.96 | 0.91 | 1.53 | 3.39 | 12.20 | 6.26 | 3.00 | 0.0083 | 0.0085 | 0.0214 | over-confident |

Bias against scatter over hours 2 to 6 (600 rows; mean error, its standard deviation, the median formal sigma, and z2 about the mean):

| user | axis | mean (mm) | sd (mm) | sigma (mm) | sd / sigma | z2 about mean |
| --- | --- | --- | --- | --- | --- | --- |
| field | E | -2.3 | 6.9 | 5.6 | 1.23 | 1.35 |
| field | N | +9.0 | 5.7 | 6.6 | 0.86 | 0.56 |
| field | U | +4.8 | 9.3 | 13.1 | 0.71 | 0.38 |
| sim | E | -1.3 | 8.2 | 8.2 | 1.00 | 0.80 |
| sim | N | +0.0 | 7.5 | 8.5 | 0.88 | 0.69 |
| sim | U | +11.5 | 25.3 | 21.1 | 1.20 | 1.33 |

Reading.
- **Hours 2 to 6: consistent at both users, by the frozen convention.** Every hourly r lies in [0.5, 2] except the field Up in hours 4 and 6 (0.47 and 0.42: pessimistic). The scatter about the mean is within a factor 1.25 of the formal sigma on every axis of both users. The field North reads z2 2.7 to 3.7 in hours 3 to 6 only through a standing +9.0 mm North offset: about its mean the North z2 is 0.56. A standing offset of this size is within the reach of the grading coordinate and of the network's frame realization (card SNX: WTZZ00DEU's held coordinate is 4.5 mm from the IGS weekly SINEX in 3D), so it is not read as a covariance defect. The field Up formal sigma is pessimistic by a factor of about 1.4 (sd 9.3 against 13.1 mm); the sim Up is close (1.20).
- **Hour 1: mildly over-confident in z2 (1.0 to 2.0 per axis, NEES 4.3 and 5.5 against 3); the r column overstates it.** In hour 1 the formal sigma falls by two orders of magnitude (2.8 m to centimetres in the vertical at the field user), so the rms error, dominated by the first epochs, divided by the median sigma, dominated by the later ones, is not a consistency statistic in a converging hour. The frozen r reading says over-confident in hour 1 and over the whole run for this reason alone. z2 is the per-epoch statistic and is the one to read in hour 1. The pre-registered convention is kept as written and this limitation is recorded beside it: r is meaningful only where the formal sigma is roughly constant (hours 2 to 6).
- **Against card NIS.** NIS found the consumer's formal covariance uncalibrated against its innovation spread: formal phase sigmas of 4 to 15 mm against an epoch-to-epoch innovation spread of decimetres at ARHT00ATA. The position NEES does not contradict that record. A measurement model can be too tight row by row (unmodelled wire and multipath error per line of sight) and still yield a position covariance of about the right size once the process noise, the robust R inflations and the averaging over many rows act. The two measure different things: the position sigma is calibrated at these two users in the settled hours, the per-row model is not, which matters for screens and integrity bounds (NIS) and not for this position grade. The census sites are not graded here (their replays predate the sigma columns); a census NEES is `gcensus.py grade <key> --nees` on replays made with a post-VIN executable.

### 7. The sim golden's start (question 4)

The sim golden carries no `network.start_offset_hours` (the loader default 0), so the generated `config/golden/golden_sim_refound.json` starts at the fixture's first epoch: the consumer's first row is at 00:00:30. The rule recorded in CLAUDE.md is to grade the sim from 00:30:15 (`start_offset_hours` 0.504167), and every sim arm of card SRN set it by hand. (`scripts/clock_level/simcensus6h.py`'s docstring describes the sim golden as carrying 0.504167; the generated golden does not.)

A third arm isolates the start at fixed code: `vin_s6z`, the sim golden copied with nothing changed (00:00:30), on `gnss_processor_vin.exe`. Its `products.txt` (without `#` lines) is byte-identical to the baseline of record `C:/GnssProcessor/runs/_rf0hv17_sim6` (md5 `5927b436fc61f534b4d6a48437ddb4d6` for both) and its consumer reads the same hourly numbers. So the difference between the baseline of record and the 00:30:15 arm is the start alone:

| sim user | first row | hourly 3D p50 (m), hours 1 to 6 | first < 0.10 m | 10-min < 0.10 m | 10-min < 0.05 m | p50 / p90 h2-6 (m) | NEES h1 / h2 |
| --- | --- | --- | --- | --- | --- | --- | --- |
| `_rf0hv17_sim6` and `vin_s6z` | 00:00:30 | 0.137 0.072 0.031 0.024 0.026 0.018 | 30.5 | 80.5 | 102.5 | 0.0272 / 0.0704 | 49.3 / 8.1 (vin_s6z) |
| `vin_s6` and `srn_ctl_s6` | 00:31:00 | 0.099 0.033 0.040 0.016 0.018 0.015 | 26.5 | 26.5 | 87.5 | 0.0219 / 0.0494 | 5.5 / 3.9 |

Reading. Inside the start artifact the sim user's hour 1 is 38 % worse, hour 2 more than twice as bad, and its position covariance strongly over-confident in hours 1 and 2 (NEES 49 and 8 against 3; z2 North 31 in hour 1): the artifact is an error the filter does not model, as the record says. From hour 3 the two starts read alike in size. Hours are counted from each run's own first epoch, so the two rows compare the same elapsed hours over different clock hours of the fixture.

### 8. Proposed text (proposals only; CLAUDE.md, the manifest and the goldens are unchanged)

#### 8.1 CLAUDE.md, the baseline statement

Proposed replacement for the paragraph beginning "**Baseline: card BOF**", up to and including "sim products identical to `_rf0hv16_sim6`." (the sentences that follow it are kept unchanged):

> **Baseline: card BOF (2026-10-28, `docs/refound/BOF_built_off_follow_through_2026-10-28.md`), after card MRF, on card GAD's Galileo posture (2026-10-13).** Pair `runs/_rf0hv17_{field,sim}6` (exe `gnss_processor_bofarm.exe`, sha256[:16] 91143c81de9a9547). The quality statement is the 16-site census on the day-129 control wire (`runs_nom/_rap_bofc_ctl_<site>`, one cold start at 00:00 per site, 6 h; `gcensus.py grade bofc_ctl --convergence`, card VIN): hours 2 to 6 site p50 median 0.044 m (p90 0.129 m, worst ARHT00ATA 0.225 m); first epoch under 0.1 m 3D at a median 30.5 min (p90 101 min, ARHT00ATA never in 6 h); a 10-minute run under 0.05 m at a median 94 min (p90 266 min). These are joint producer-ignition plus user-convergence times. Beside it the held-out user WTZZ00DEU (1.6 m from roster station WTZR00DEU and 67 m from WTZS00DEU, 7 roster stations within 350 km; the best of 17 replayed consumers on this day, near the census median on DOY 130): 0.059 0.012 0.023 0.012 0.017 0.012 m by hour, 6.0 min to 0.1 m, 40.5 min to a 10-minute run under 0.05 m. The formal position sigma (`closed_loop_trace.csv` `sig_*_m`, `scripts/consumer_nees.py`) is consistent with the actual error at both users in hours 2 to 6 (card VIN). Sim: the pair of record starts at 00:00:30, inside the fixture's start artifact; from 00:30:15 the sim user reads 0.099 0.033 0.040 0.016 0.018 0.015 m by hour (card VIN section 7).

And one bullet under "Grading": "**Convergence and consistency.** Beside the hours 2 to 6 census delta, report the census convergence medians (`gcensus.py grade ... --convergence`: first epoch under 0.1 m, 10-minute runs under 0.1 m and 0.05 m, a site that never converges counted as never) and, on runs from a post-VIN executable, the per-hour NEES at the held-out users (`--nees`, `scripts/consumer_nees.py`)."

#### 8.2 The sim golden's start (manifest)

Proposed change to `config/golden/golden_manifest.json`, entry `golden_sim_refound`: add the pin `"network.start_offset_hours": 0.504167` under `pins`, and extend the description by "starts at 00:30:15, after the fixture's start artifact (card VIN section 7)". No ledger change is needed (`network.start_offset_hours` has a CORE row); `make_golden_config.py` regenerates the sim golden with the one key added, and `--verify` is unaffected (the key is not a RECOMMEND row).

Consequence for the numbers of record:
- The sim pair of record `_rf0hv17_sim6` (and the earlier `_rf0hv16_sim6`, `_rf0hv15_sim6`) becomes a different statistic from every arm of the regenerated golden. Its consumer moves from 0.137 0.072 0.031 0.024 0.026 0.018 to 0.099 0.033 0.040 0.016 0.018 0.015 m by hour on the same code. The new sim baseline is the 00:30:15 run: `_rap_vin_s6` on this branch, `_rap_srn_ctl_s6` on main `adef08b` (identical in `products.txt` and every trace but the four new consumer columns).
- The run ends at 06:30:15 instead of 06:00:30 (`max_duration_s` 21600 counts from the start).
- Every sim arm made from the regenerated golden inherits the offset; an arm that sets `start_offset_hours` itself is unchanged. `scripts/clock_level/simcensus6h.py` replays already apply the base arm's offset from the wire's first epoch (card SCL2B section 9), so a census made on the regenerated golden starts at 01:01:30 unless `SIMCENSUS_FROM_START=1`; keys made before and after the change are different statistics.
- No snapshot or ledger migration follows (the start is not a fingerprinted knob); the field goldens are untouched.

### 9. Gates

| gate | result | numbers |
| --- | --- | --- |
| G1 schema | PASS | Debug `gnss_tests.exe`: 3,839 cases in 524 suites, 3,806 passed, 33 skipped (fixture-gated), 0 failed; the new suite `ConsumerTraceSchema` 6 of 6 |
| G2 identity | PASS | both 6 h arms: `products.txt` identical, 33 of 33 other CSVs identical (wall-clock columns dropped), consumer trace columns 1 to 14 identical, 18 columns on every row |
| G3 selftests | PASS | NEES 9 of 9, convergence 11 of 11, co-location PASS |
| G4 reproduction | PASS | reviewer 28.8 / 82 / 101 and 77.5 min reproduce with WTZZ00DEU pooled and ARHT00ATA dropped; D9-03's 30.5 and 94 min are this grader's default census |

### 10. Owed

- A census NEES: the 16 census replays on disk predate the sigma columns; `gcensus.py make` with a post-VIN executable, then `grade <key> --nees`.
- The user's own convergence on a settled wire (a consumer started at hour 3 of the wire, card IRD bar K-c) is not measured here; the census times are joint producer-ignition plus user-convergence times.
- The author's decisions on section 8.1 (CLAUDE.md) and section 8.2 (the sim golden's start pin and a new sim baseline).
- `scripts/nom_td_gate.py` and `scripts/prm_ab.py` compare `closed_loop_trace.csv` whole, so a comparison across the VIN boundary (a pre-VIN reference against a post-VIN candidate) reports that file as different by its four trailing columns; same-build comparisons are unaffected. A projection onto the first fourteen columns was not added (one owner of the comparison rules; the author may prefer a re-baselined reference).
- `scripts/clock_level/simcensus6h.py`'s docstring says the sim golden carries 0.504167; it does not until section 8.2 is applied.
