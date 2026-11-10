# NSP: the census's spread under a neutral change of the wire

**STATUS**

- **id:** NSP
- **topic:** consumer
- **status:** closed
- **summary:** Pre-registered 2026-10-29 (section 5), closed 2026-10-30. The 16-site census does not amplify a neutral change of the wire. The pre-registered arms (`network.parallel_max_threads` 16 to 6 against the pinned 20, `network.parallel_carry` off) do not act on the float-vehicle golden: products byte-identical, the carried-prior fold they thread holds no block (section 7). Amendment A1 re-rounded the roster coordinates (an independent offset within plus or minus 0.05 mm per axis, six seeds per day on DOY 130 and DOY 129, and a 1 nm probe): every consumer's trace changes, yet no census site's hours-2-to-6 p50 moves by more than 0.19 mm (ARHT00ATA s 0.11 mm on DOY 130, 0.014 mm on DOY 129) and no neutral arm trips the clause. Card PIV's DOY 130 ARHT00ATA +40.6 mm reads OUTSIDE the neutral spread (rank 0 of 6, one-sided 99 % bound +0.44 mm): a response to the election's wire, not noise, with the pre-registered caveat that the neutral arms flip none of the election's clock-anchor decisions (section 9). Nothing armed.
- **source:** [PIV](../../refound/PIV_pivot_election_coverage_2026-10-17.md) section 9; [BOF](BOF_built_off_follow_through_2026-10-28.md) sections 4 and 5; the PIV line of [OPEN_ITEMS_BUILT_OFF](../../status/OPEN_ITEMS_BUILT_OFF.md)
- **knobs / posture:** read, not changed: `network.parallel_max_threads` (both goldens pin 20) and `network.parallel_carry` (armed in both goldens); `network.stations[].position_ecef` re-rounded in the arms of amendment A1; the field golden of main `ef9f75d`, the posture of card BOF's PIV arms, moved to DOY 130 by `scripts/mday_golden_config.py`; nothing is armed by this card
- **dates in the text:** 2026-10-29, 2026-10-30
- **hashes / run dirs:** exe `build/x64-Release/gnss_processor_nsp.exe`, a byte copy of card BOF's `gnss_processor_bof.exe` (sha256[:16] 2d84a326fd00fd04, the Release build of main `ef9f75d`); run root `D:/GnssProcessorSupport/runs_nom`: pre-registered arms `_rap_nsp130_{id,t16,t12,t10,t8,t6,ser}` and `_rap_nsp129_{t16,t12,t10,t8,t6,ser}`; amendment A1 arms `_rap_nsp130_u1` to `_u6`, `_rap_nsp130_n101`, `_n102`, `_rap_nsp129_u11` to `_u16`, rosters `nsp_bases/roster_*.json`; census replays `_rap_nspc130_*` and `_rap_nspc129_*`; reports `nsp_reports/`; controls card BOF's `_rap_bof_ctl130_f6` (DOY 130) and `C:/GnssProcessor/runs/_rf0hv16_field6` (DOY 129) with their census replays `_rap_bofc_ctl130_*` and `_rap_bofc_ctl_*`; grader `scripts/neutral_spread.py`; pre-registration commit `5c6af88`, amendment commit `e8adab7`

**Outcome.** Question: how far do the census sites, ARHT00ATA above all, move under a change of the wire that has
no expected effect, and is card PIV's ARHT00ATA +40.6 mm on DOY 130 inside that spread? The thread-count arms first
registered do not act on this golden; re-rounded roster coordinates do, and every consumer's trace changes. No census
site moves by more than 0.19 mm over hours 2 to 6 on either day (ARHT00ATA s 0.11 mm). Verdict: the +40.6 mm is
outside the neutral spread, a response to the election's wire and not noise; the null does not reach the election's
clock-anchor decisions. The clause stands failed; ARHT00ATA's response over more days decides the election
([PIV](../../refound/PIV_pivot_election_coverage_2026-10-17.md) section 10).

## 1. Why a neutral spread

Card BOF grades each census site by the mean of its hourly p50 over hours 2 to 6, paired against a control wire; a
site more than 1 cm worse fails the "no site worse" clause. ARHT00ATA is the census site with the largest error
(hourly p50 0.03 to 0.37 m) and has moved by centimetres under changes of the wire that have nothing to do with it:

| change of the wire | day, window | ARHT00ATA change (mm) | source |
|---|---|---:|---|
| `network.pivot_election: coverage` | DOY 130, hours 2 to 6 | +40.6 | BOF section 2, PIV section 9 |
| the same | DOY 129, hours 2 to 24 | -6.9 | PIV section 9 |
| the same, the card's own check exe | DOY 129, hours 2 to 6 | -5.8 | PIV section 8.6 |
| `network.clock_datum_sum_to_zero` | DOY 129, hours 2 to 6 | +59.2 | PIV section 8.7 |
| `network.cycle_slip.gf_arc_jump` | DOY 129, hours 2 to 6 | -16.8 | BOF section 2 (grade `bof_reports/grade_6h_nsd_arl_piv130.md`) |
| `network.cycle_slip.if_phase_td_reseed_on_flag` | DOY 129, hours 2 to 6 | -0.5 | the same grade |

In card BOF's DOY 130 census of the election the other 15 sites moved between -3.6 and +1.6 mm and WTZZ00DEU by
-0.04 mm; ARHT00ATA alone moved +40.6 mm. Without the spread under a change that should do nothing, that failure
reads neither as an effect nor as noise.

## 2. The neutral perturbations

A neutral arm is the golden with one key changed such that the solve is the same in exact arithmetic and only the
grouping of floating-point sums differs. It perturbs the wire at the rounding level and has no expected systematic
effect. Two keys qualify.

- **`network.parallel_max_threads` (the goldens pin 20).** The thread count of the per-station work (RINEX parse,
  geometry, row assembly), the post-solve seam and the parallel carry. The per-station work and the seam write
  per-station or per-block slots and combine them in a fixed order, so their output does not depend on the count
  (`ApplicationNetworkSetup.cpp`). Under `network.parallel_carry` the carried-prior fold (`foldReducedDdPriors`,
  `NormalEquations.cpp`) sums each block's per-station terms in per-thread partials over contiguous station ranges and
  adds the partials in ascending thread order: the grouping of the sum, and with it the rounding, depends on the
  count. The ledger row of the key records the consequence ("a pinned N keeps counters and hashes comparable across
  runs"). A count other than 20 changes that grouping and possibly Eigen's threading of the carry's dense products
  (`CarryEigenScope`); it enters no model term, gate or threshold.
- **`network.parallel_carry: false`.** The fold then sums a block's stations serially in ascending order and Eigen
  stays on one thread for the carry's products. The ledger row records the effect: a re-association of about 1e-4
  at the solution, nothing else.

Any move of a census site under these arms comes from re-grouped sums alone, carried into the solution and amplified
through the decisions of the network (screens, slip tests, admissions, re-floats) and of the consumer. This is the
pair of arms "that differ only in an inert knob" that card PIV section 9 names, extended to six per day so that a
spread can be estimated.

Values: 16, 12, 10, 8 and 6 threads, and the serial carry as a second mechanism. With 58 roster stations every count
from 2 to 19 partitions a block's stations differently from 20. An identity re-run at 20 threads on DOY 130 checks
that the same exe and count reproduce card BOF's control.

## 3. Arms

| day | base | arms (tag: change) |
|---|---|---|
| DOY 130 | `D:/GnssProcessorSupport/runs_nom/bof_bases/golden_field_doy130.json` (sha256[:16] 59ed54978019cb16; the `ef9f75d` golden moved by `scripts/mday_golden_config.py`, card BOF's base; it equals that transform of `git show ef9f75d:config/golden/golden_field_refound.json` leaf for leaf) | `nsp130_id`: none (re-run of `_rap_bof_ctl130_f6`); `nsp130_t16`, `nsp130_t12`, `nsp130_t10`, `nsp130_t8`, `nsp130_t6`: `network.parallel_max_threads` 16, 12, 10, 8, 6; `nsp130_ser`: `network.parallel_carry` false |
| DOY 129 | `C:/GnssProcessor/runs/_rf0hv16_field6/arm.json` (the `ef9f75d` golden as the control ran it; it differs from the git golden only in the output path and in the encoding of `_comment`) | `nsp129_t16`, `nsp129_t12`, `nsp129_t10`, `nsp129_t8`, `nsp129_t6`, `nsp129_ser`: the same changes |

All 6 h from 00:00, made and run with `scripts/rapid_arm.py` (`RAPID_RUNS_ROOT=D:/GnssProcessorSupport/runs_nom`) on
the one exe. Controls: `_rap_bof_ctl130_f6` and `_rf0hv16_field6`, run at 20 threads on the same exe bytes (card
BOF's NSD arm, a measure-only change, reproduced the DOY 129 control's products byte for byte).

**Evidence that each arm acts** (`python scripts/neutral_spread.py wire`, each arm against its day's control):
products byte identity, the first epoch whose records differ, records present in one wire only, the between-satellite
move over hours 2 to 6 (per epoch and constellation, per signal for the biases, the arm-minus-control difference less
its median over the satellites, in mm: p50, p95 and max for clocks, code biases and phase biases), and the decision
rows of `network_events.csv` (keyed by epoch, tier, event, satellite, station and action) present in one run only,
with the first epoch of such a row. The PIV wire is put through the same table for scale (DOY 130: clock move p50
2.6, p95 19.2 mm; 11439 decision rows in one run only, the first at 00:00:00).

- An arm whose products are byte-identical to its control did not act: it is reported and left out of the spread.
- If the identity re-run differs from `_rap_bof_ctl130_f6`, the same exe and count do not reproduce a run: this is
  reported first, and the re-run joins the DOY 130 neutral sample.

## 4. Census and the spread statistic

Each arm's wire is replayed at the 16 census sites and WTZZ00DEU (`scripts/clock_level/gcensus.py make`, 6 h; DOY 130
with `--day 130 --obs-root D:/GnssProcessorSupport/data_mday`). The consumer base of every replay of a day is that
day's control (`_rap_bof_ctl130_f6`, `bofc_src_ctl129`), so every replay has the control's consumer configuration
and only the wire differs. The changes are taken against card BOF's control replays (`bofc_ctl130`, `bofc_ctl`).
Two checks on the replays: the DOY 130 control wire replayed again (`nspc130_ctlr`) reproduces `bofc_ctl130` trace for
trace, and the PIV wire is replayed again on the control's base (`nspc130_pivr`) so that its change and the neutral
changes share one consumer configuration (card BOF's own-base replay `bofc_piv130` is quoted beside it).

Graded as card BOF graded (`python scripts/neutral_spread.py grade`): x = a site's mean hourly p50 over hours 2 to 6,
d = arm minus control, paired by site. Per site and day, over the K neutral arms (K = 6 unless an arm did not act):

- the changes d_1 to d_K, their mean m and sample SD s (K - 1 degrees of freedom). s estimates the spread of one
  realization; a paired change between two independent realizations spreads by s times the square root of 2;
- the envelope, the largest |d_k|;
- per neutral arm, the sites worse or better by more than 1 cm, and the false-alarm rate of the clause: the share of
  neutral arms with at least one site worse by more than 1 cm;
- per neutral arm, the census paired median (the spread of the median clause) and WTZZ00DEU's change (the spread of
  the held-out consumer's bar).

## 5. The reading of card PIV's DOY 130 clause (fixed before any arm ran)

d_P = ARHT00ATA's change under `network.pivot_election: coverage` on DOY 130: +40.6 mm in card BOF's grade. The
control-base replay `nspc130_pivr` is used if it differs from that by more than 1 mm (both are quoted). Against the
DOY 130 neutral sample of ARHT00ATA:

- r = the number of neutral arms with d_k at least d_P;
- U95 and U99 = m + t s sqrt(1 + 1/K), with t the one-sided 95 % and 99 % quantiles of Student's t at K - 1
  degrees of freedom (2.015 and 3.365 for K = 6): the bounds of a one-sided prediction interval for one more
  neutral change against the same control.

| reading | condition | meaning |
|---|---|---|
| inside | r at least 1, or d_P at most U95 | a change with no expected effect moves ARHT00ATA as far; the failed clause is not evidence against the election |
| outside | r = 0 and d_P above U99 | ARHT00ATA's +40.6 mm is a response to that day's election wire, beyond rounding-level noise |
| undecided | otherwise | six more DOY 130 arms (`parallel_max_threads` 18, 14, 9, 7, 5, 4) are run and the reading is repeated on K = 12 (t 1.796 and 2.718) |

The same reading is printed for every other census site of the DOY 130 PIV census. The DOY 129 spread is reported
beside it and does not enter the DOY 130 reading.

**How far the null reaches.** A re-grouped sum is the smallest change of the wire. The election changes the pivot
and with it many decisions from the first epoch. The neutral sample is read as the noise of a change of the
election's size only where the neutral arms' decisions decorrelate as far as the election's do: at least half of its
11439 decision rows in one run only. Below that, an "outside" reading carries the caveat that a larger neutral
perturbation could spread further. "Inside" needs no such caveat.

"Outside" names a response of ARHT00ATA to the DOY 130 election wire; it does not by itself make the election harmful
(its DOY 129 changes were -6.9 and -5.8 mm). Adoption remains the author's decision.

## 6. What this card does not do

- One posture (the `ef9f75d` golden of card BOF's PIV arms), one exe, two days, 00:00 to 06:00 UT. The goldens since
  card BOF's arming add ocean tide loading and the noon-turn attitude on the field; the spread there is assumed, not
  measured, to be the same.
- It arms nothing and changes no bar. The per-site spread is reported for reading future "no site worse" clauses; a
  site-specific bar, if any, is the author's decision.

## 7. The pre-registered arms, 2026-10-30: neither key acts on this posture

All 13 arms exited 0 (DOY 130 2762 to 2898 s, DOY 129 1527 to 1626 s, seven at a time beside other sessions' runs).
`python scripts/neutral_spread.py wire` (`nsp_reports/wire_130.txt`, `wire_129.txt`):

| day | arms | products.txt | also identical to the control |
|---|---|---|---|
| DOY 130 | `nsp130_id`, `_t16`, `_t12`, `_t10`, `_t8`, `_t6`, `_ser` | 7 of 7 byte-identical to `_rap_bof_ctl130_f6` (sha256[:16] eb02f00f8c87c59c) | `network_events.csv`, `closed_loop_trace.csv`, `network_epoch_status.csv`, `network_solution.csv` in all 7 |
| DOY 129 | `nsp129_t16`, `_t12`, `_t10`, `_t8`, `_t6`, `_ser` | 6 of 6 byte-identical to `_rf0hv16_field6` (bccf08dfa1d49beb) | the same four traces in all 6 |

The thread cap was applied (the setup lines read "thread cap 16" and "Eigen-THREADED across 16 threads" in `nsp130_t16`).
It has nothing to act on: the run-start table "contributions as armed" reads `dd_prior_block ... blocks 0`. On the
float-vehicle posture the carried DD-prior fold that `network.parallel_carry` threads holds no block, and every other
thread-count path writes per-station or per-block slots in a fixed order. No neutral realization came out of section
3, and the census of an identical wire is the control's (the 14 replays of two such wires that completed before the
census was stopped equal the control replays; all 34 moved to `nsp_reports/stopped_census_t16/`).

Two findings stand on their own. The identity re-run reproduces card BOF's control byte for byte: the same exe and
configuration give the same run. And on the float-vehicle goldens the thread count (6 to 20) and the serial carry
leave the products unchanged; the ledger rows of the two keys ("reassociates ~1e-4", "a pinned N keeps counters and
hashes comparable") describe the retired invariant carry, not the current goldens.

## 8. Amendment A1, 2026-10-30 (written before any of its arms ran)

No key of the golden re-groups arithmetic on the float vehicle, so the neutral change moves to the inputs.

- **The roster coordinates re-rounded (the neutral family).** The golden writes each station's ECEF coordinate to
  0.1 mm (four decimals). Each of the 58 roster stations gets an independent offset drawn uniformly in plus or minus
  0.05 mm on each axis: half the last written digit, so the arm's coordinates are another rounding of the same
  coordinates (largest 3D shift 0.072 to 0.081 mm per seed). The offsets are seeded files
  `D:/GnssProcessorSupport/runs_nom/nsp_bases/roster_u<seed>.json` applied by `rapid_arm.py make --roster`; the
  consumer's coordinate, the truth of the grade, is not touched. A satellite's products take a station's offset only
  through its projection, averaged over the stations that see the satellite: of order 0.02 mm between satellites and
  under 0.1 mm at a consumer. The change enters every observation at every epoch; a census move beyond about 0.1 mm is
  the response of the network's and the consumer's decisions.
- **A probe at the rounding level of the arithmetic.** Offsets drawn from a normal law of SD 1 nm per axis (143 of
  the 174 coordinate values change by a few units in the last place; largest 3D shift 3 nm), seeds 101 and 102, DOY
  130 only. Reported beside the family, not pooled with it.

| day | arms (seed) | base |
|---|---|---|
| DOY 130 | `nsp130_u1` to `nsp130_u6` (seeds 1 to 6); probe `nsp130_n101`, `nsp130_n102` | as section 3 |
| DOY 129 | `nsp129_u11` to `nsp129_u16` (seeds 11 to 16) | as section 3 |

Exe, census (consumer base the day's control), statistics, the reading of section 5 (K = 6 from the re-rounded family
of DOY 130) and the decorrelation criterion are unchanged.

## 9. Results of amendment A1, 2026-10-30

All 14 arms exited 0 (1666 to 1711 s, eight at a time); 14 census keys, 238 replays, all exit 0. Outputs in
`D:/GnssProcessorSupport/runs_nom/nsp_reports/`: `wire_130_a1.txt`, `wire_129_a1.txt`, `grade_130.txt` (and `.json`),
`grade_129.txt`, `grade_130_probe.txt`, the arm and census logs `run_arms_a1.log`, `census_driver_a1.log`.

### 9.1 The arms act

`python scripts/neutral_spread.py wire`, each arm against its day's control; the between-satellite move over hours 2
to 6 in mm:

| arms | first wire difference | clock move p50 / p95 / max | phase-bias move p95 | decision rows in one run only (first) |
|---|---|---|---|---|
| DOY 130 re-rounded, seeds 1 to 6 | 00:00:00 | 0.012 to 0.015 / 0.081 to 0.226 / 1.4 to 8.0 | 0.45 to 0.94 | 513 to 729 (00:04:00 to 00:05:00) |
| DOY 129 re-rounded, seeds 11 to 16 | 00:00:00 | 0.009 to 0.015 / 0.031 to 0.050 / 0.1 | 0.16 to 0.27 | 401 to 492 (00:01:30 to 00:03:30) |
| DOY 130 probe, 1 nm, seeds 101 and 102 | 00:00:00 | 0.002 to 0.003 / 0.071 to 0.153 / 2.9 to 3.9 | 0.39 to 0.64 | 457 and 759 (00:03:30, 00:04:00) |
| DOY 130 election (card BOF's `_rap_bof_piv130_f6`), for scale | 00:00:00 | 2.6 / 19.2 / 1162 | 31.5 | 11439 (00:00:00) |

Every census replay of the re-rounded family differs from the control's trace (0 of 204 identical): the change
reaches every consumer. By tier, 90 to 97 % of the neutral arms' differing rows are the measure-only prior-residual
census (`PRIORRES`). Every other differing row (15 to 62 per arm) is at one station, BSHM00ISR: its drops and
re-admissions by the positive-definiteness test (`TOPOLOGY` `sta_pd_dropped`, `sta_readmitted`) and the `MDB` and
`REDUNDANCY` rows that follow them. Its local block's smallest eigenvalue is about 1e-11 against a largest of 1.2e6,
so that test is decided by rounding. The election's 11439 rows are 98.7 % `PRIORRES` too; the other 149 are the clock anchor's
`cta_decouple`, `cta_recouple` and `cta_disc_landed` events (100), `CONGRUENCE` (32), gate engage and release (9),
re-admission seeds (2), one RAIM attribution, the two elections, the pivot, the S-transform and a datum-graph row.

### 9.2 The census spread

Hours 2 to 6, the mean of the hourly p50, each neutral arm minus the day's control; s is the sample SD over the six
arms and the envelope the largest magnitude (mm). The consumer trace prints the error to 0.1 mm, so the 5-hour means
resolve 0.01 mm; spreads below 0.1 mm are at that resolution.

| site | DOY 130 control (mm) | s | envelope | election change | DOY 129 control (mm) | s | envelope |
|---|---:|---:|---:|---:|---:|---:|---:|
| WTZZ00DEU | 39.2 | 0.01 | 0.01 | -0.0 | 13.3 | 0.00 | 0.00 |
| ARHT00ATA | 197.6 | 0.11 | 0.19 | +40.6 | 225.4 | 0.01 | 0.02 |
| SCRZ00BOL | 148.2 | 0.00 | 0.00 | +0.1 | 45.0 | 0.00 | 0.00 |
| HARB00ZAF | 62.2 | 0.07 | 0.18 | +0.1 | 39.0 | 0.00 | 0.00 |
| QUIN00USA | 59.5 | 0.00 | 0.00 | -3.6 | 26.4 | 0.00 | 0.01 |
| ANMG00MYS | 44.1 | 0.00 | 0.00 | -0.1 | 54.4 | 0.00 | 0.01 |
| UTQI00USA | 43.0 | 0.01 | 0.01 | -0.1 | 49.1 | 0.00 | 0.00 |
| DAV100ATA | 39.2 | 0.02 | 0.05 | +0.5 | 129.3 | 0.01 | 0.01 |
| KOKB00USA | 38.9 | 0.00 | 0.00 | -0.4 | 47.8 | 0.01 | 0.02 |
| CEDU00AUS | 36.1 | 0.01 | 0.01 | +0.1 | 42.5 | 0.00 | 0.01 |
| ASCG00SHN | 35.7 | 0.00 | 0.00 | -0.4 | 35.1 | 0.01 | 0.01 |
| WUH200CHN | 30.6 | 0.00 | 0.00 | +1.6 | 45.6 | 0.00 | 0.00 |
| POHN00FSM | 30.3 | 0.00 | 0.00 | -0.1 | 67.8 | 0.00 | 0.00 |
| BAKE00CAN | 30.0 | 0.01 | 0.01 | -0.9 | 16.6 | 0.01 | 0.02 |
| STPM00SPM | 26.8 | 0.00 | 0.00 | +0.1 | 23.9 | 0.00 | 0.00 |
| CHTI00NZL | 23.0 | 0.00 | 0.00 | -0.0 | 42.6 | 0.00 | 0.00 |
| BADG00RUS | 19.6 | 0.01 | 0.01 | -0.2 | 24.9 | 0.00 | 0.00 |

- ARHT00ATA, DOY 130: +0.19, 0.00, +0.01, -0.02, +0.18, -0.06 mm (m +0.05, s 0.11); its hourly p50 equals the
  control's to 0.1 mm in hours 1 to 5 in every arm and moves 216.1 to 217.5 mm against 216.5 mm in hour 6. DOY 129:
  -0.02, +0.02, -0.01, 0.00, -0.01, 0.00 mm (s 0.014).
- Clause false alarms: 0 of 6 neutral arms on each day have a site worse (or better) by more than 1 cm. The census
  paired median is 0.00 mm and WTZZ00DEU within 0.01 mm in all twelve arms; the hour-1 paired median is 0.0 mm.
- The 1 nm probe reads the same: ARHT00ATA +0.17 and +0.01 mm, HARB00ZAF +0.08 and +0.08 mm, no site above 0.17 mm.

### 9.3 The reading of card PIV's DOY 130 clause

| quantity | value |
|---|---|
| d_P, ARHT00ATA under `network.pivot_election: coverage` | +40.63 mm (card BOF's replay; the control-base replay `nspc130_pivr` is byte-identical at all 17 sites) |
| r, neutral arms with d_k at least d_P | 0 of 6 |
| U95, U99 = m + t s sqrt(1 + 1/6) | +0.28, +0.44 mm |
| reading (section 5) | **outside** |

ARHT00ATA's +40.6 mm on DOY 130 is a response to the election's wire, about 90 times the 99 % bound of a neutral
change; it is not noise. Its hourly change under the election is -2.8, +0.8, +45.6, +15.1, +72.9 and +68.7 mm. The
same rule reads every other positive election change as outside its site's neutral spread (WUH200CHN +1.6, DAV100ATA
+0.5 mm and four of +0.1 mm) except HARB00ZAF's +0.1 mm, which one neutral arm reaches; only ARHT00ATA exceeds the
1 cm clause.

The pre-registered caveat applies: the neutral arms change 513 to 729 decision rows against the election's 11439,
below the half (5720) that section 5 asks for a null to stand for a change of the election's size. Section 9.1 says
what lies between: the neutral arms flip no acting decision other than BSHM00ISR's positive-definiteness test, the
election changes the clock anchor's decouple and recouple sequence, gates and re-admissions. The null therefore
excludes rounding noise as the cause and says nothing on whether another change of the clock-anchor sequence would
move ARHT00ATA as far.

### 9.4 What the spread means for the census clauses

- The census does not amplify a neutral change. Re-rounded coordinates reach every consumer (0 of 204 traces
  unchanged) and, like the 1 nm probe, flip hundreds of measure-only rows and one station's positive-definiteness
  test, yet move no site's hours-2-to-6 p50 by more than 0.19 mm. A paired change between two independent neutral
  realizations spreads by at most 0.15 mm (s times the square root of 2): the 1 cm site bar and the 1 mm median and
  WTZZ00DEU bars lie above the census's own noise at every site on both days.
- ARHT00ATA's centimetre moves under the election, the arc-jump flag (-16.8 mm) and the sum-to-zero datum (+59.2 mm)
  are therefore responses to those changes of the wire. A failed "no site worse" clause is a real response of that
  site, not instrument noise.
- Whether a response is systematic is a question over days, not over neutral arms: ARHT00ATA's response to the
  election is -5.8 mm (DOY 129, 6 h, card PIV section 8.6), -6.9 mm (DOY 129, hours 2 to 24) and +40.6 mm (DOY 130,
  6 h).

### 9.5 Side findings

- Card BOF's DOY 130 control is reproduced byte for byte by the same exe and configuration (section 7); every census
  replay is deterministic (the DOY 130 control wire replayed again, `nspc130_ctlr`, equals `bofc_ctl130` at all 17
  sites).
- `network.parallel_max_threads` (6 to 20) and `network.parallel_carry` leave the float-vehicle goldens' products
  unchanged; their ledger rows still describe the retired invariant carry (section 7). Correcting the rows is a ledger
  edit for the author, not made here.
