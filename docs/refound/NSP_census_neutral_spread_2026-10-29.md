# NSP: the census's spread under a neutral change of the wire

**STATUS**

- **id:** NSP
- **topic:** consumer
- **status:** open
- **summary:** Pre-registered 2026-10-29, before any arm ran. ARHT00ATA decides every "no site worse" clause of the 16-site census and moves by centimetres under unrelated wire changes; its spread under a change with no expected effect has never been measured. Neutral arms: the golden with only `network.parallel_max_threads` changed (16, 12, 10, 8, 6 against the pinned 20) or `network.parallel_carry` off, two keys that re-group the floating-point sums of the carried-prior fold and change nothing else. Six 6 h arms per day on DOY 130 and DOY 129, one exe, consumer-only census replays graded as card BOF graded; the reading of card PIV's DOY 130 clause (ARHT00ATA +40.6 mm) is fixed in section 5.
- **source:** [PIV](PIV_pivot_election_coverage_2026-10-17.md) section 9; [BOF](BOF_built_off_follow_through_2026-10-28.md) sections 4 and 5; the PIV line of [OPEN_ITEMS_BUILT_OFF](../status/OPEN_ITEMS_BUILT_OFF.md)
- **knobs / posture:** read, not changed: `network.parallel_max_threads` (both goldens pin 20) and `network.parallel_carry` (armed in both goldens); the field golden of main `ef9f75d`, the posture of card BOF's PIV arms, moved to DOY 130 by `scripts/mday_golden_config.py`; nothing is armed by this card
- **dates in the text:** 2026-10-29
- **hashes / run dirs:** exe `build/x64-Release/gnss_processor_nsp.exe`, a byte copy of card BOF's `gnss_processor_bof.exe` (sha256[:16] 2d84a326fd00fd04, the Release build of main `ef9f75d`); run root `D:/GnssProcessorSupport/runs_nom`: arms `_rap_nsp130_*` and `_rap_nsp129_*`, census replays `_rap_nspc130_*` and `_rap_nspc129_*`, reports `nsp_reports/`; controls card BOF's `_rap_bof_ctl130_f6` (DOY 130) and `C:/GnssProcessor/runs/_rf0hv16_field6` (DOY 129) with their census replays `_rap_bofc_ctl130_*` and `_rap_bofc_ctl_*`; grader `scripts/neutral_spread.py`

**Outcome.** Question: how far do the census sites, ARHT00ATA above all, move under a change of the wire that has
no expected effect, and is card PIV's ARHT00ATA +40.6 mm on DOY 130 inside that spread? Pending: the arms run after
this pre-registration.

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
3, and the census of an identical wire is the control's (the 17 replays of two such wires that completed before the
census was stopped equal the control replays; moved to `nsp_reports/stopped_census_t16/`).

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
