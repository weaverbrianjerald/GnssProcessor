# VIN: validation instruments (the consumer's formal sigma, a convergence grade, the headline and the sim start)

**STATUS**

- **id:** VIN
- **topic:** harness
- **status:** open
- **summary:** Build B5 of card SRN (findings F5 and F12). Measure-only: four formal-sigma columns appended to `closed_loop_trace.csv` (both writers, one shared schema), a position NEES grader, a convergence grader, the co-location distances and the census beside the held-out user, and proposed text for CLAUDE.md and the sim golden's start. No estimator change; no knob.
- **source:** card SRN (`D:/GnssProcessorSupport/worktrees/scirev/docs/refound/SRN_scientific_review_network_2026-11-06.md`, F5, F12, build B5) with the second-model verdicts D9-02, D9-03 and D9-07 (`D:/GnssProcessorSupport/srn/wave2_verdicts.json`); cards [MSC](MSC_multi_site_consumer_2026-09-21.md) (the census is the standing instrument), [IRD](IRD_integer_recovering_datum_2026-10-13.md) bar K-c (the convergence bars never adopted), [NIS](NIS_consumer_innovation_screen_2026-10-17.md) section 8 item 2 (the consumer's formal covariance is uncalibrated against its innovation spread)
- **dates in the text:** 2026-11-07
- **knobs / posture:** none; the field and sim goldens unchanged (sim arm with `network.start_offset_hours` 0.504167)
- **hashes / run dirs:** section 7
- **gates (frozen at pre-registration):** section 3

**Outcome.** (Pre-registration; the paragraph is revised at close.)

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
