# ARL: the integer-ambiguity ladder on the simulated network

**STATUS**

- **id:** ARL
- **topic:** network-vehicle
- **status:** open
- **summary:** Can the network producer recover the true integer ambiguities, and what stops it? A ladder on the simulated network, one layer of complexity per rung, 2 h runs, graded against truth. Rung 0 (clean data: no ionosphere, biases, troposphere, multipath or phase colour) with the golden posture's free slant ionosphere: floats 0.24 cycle from truth, no epoch fixed. Rung 0 with the estimator told the ionosphere is zero (`network.sim_zero_iono_sigma_m`, test-only, default off): floats 0.006 cycle, every epoch fixed, 99.89 % of kept fixes correct; the 14 apparent wrong fixes are one arc whose wind-up starts across the branch cut between the simulator (10 degree mask) and the producer (15 degrees), so every kept fix is correct in the producer's convention (section 4), and the grader now counts each arc's branch. Rung 1 (section 5): the satellite and receiver code biases, then the phase biases, then both, each on the rung-0 zero-ionosphere posture: every epoch fixed and every kept fix correct on all three. Rung 2 (section 6): the ionosphere on in the fixture and free in the estimator (no GIM, no external product): the wide-lane and ionosphere-free parts of the floats are precise (0.06 and 0.04 cycle) but the entry-by-entry search fixes nothing; a partial fix in the decorrelated space (`network.ambiguity_resolution.decorrelated_partial`, new, default off) fixes 94 of 110 settled epochs and determines 5 572 single-signal L1/L2 integers, every one correct. Of the pairs left float (section 7), 25 % have the wide lane fixed and 32 % do not; a Melbourne-Wubbena wide lane averaged over the arc is no better than the float's own (0.145 against 0.148 cycle p50), and the limit is arc age: both classes need more time, not an MW input.
- **source:** the author's request (2026-10-09): "turn off all iono, hardware bias, etc., then we can easily check if integer ambiguities are recoverable"; [project memory on network AR] the September carry-blind covariance finding (14.5 % wrong fixes on the retired triple-frequency posture); the L1/L2 geometry (a narrow-lane integer is 99.3 % ionosphere)
- **knobs / posture:** `config/golden/golden_sim_refound.json` pointed at the rung fixtures (`config/sim/arl/arl_r0_producer.json`), with `network.ambiguity_resolution.enable` and `.trace` (the producer's search, log-only: nothing is written back); `network.sim_zero_iono_sigma_m` (new, test-only, default 0); simulator `corrections.signal_bias_scope` (new, test-only, default `both`: `code` or `phase` stages the satellite OSB one side at a time); `network.ambiguity_resolution.decorrelated_partial` and `.decorrelated_determined_sigma_cyc` (new, default off and 0.05)
- **dates in the text:** 2026-10-09
- **hashes / run dirs:** fixture `runs/simarl_r0` (`config/sim/arl/arl_r0_clean_2h.json`); runs `runs/_rap_arl_r0`, `_rap_arl_r0b`, `_rap_arl_r0z`; off identity `runs/_rap_arl_off_f2` against `runs/_rap_g15_f2`; rung 1 fixtures `runs/simarl_r1{a,b,c}` (`config/sim/arl/arl_r1{a,b,c}_2h.json`), runs `runs/_rap_arl_r1{a,b,c}` (`config/sim/arl/arl_r1{a,b,c}_producer.json`); rung 2 fixture `runs/simarl_r2` (`config/sim/arl/arl_r2_2h.json`), runs `runs/_rap_arl_r2a`, `_rap_arl_r0ca`, `_rap_arl_r2dp`, `_rap_arl_r0zdp`, off identity `_rap_arl_r2off` against `_rap_arl_r2a` (exe `gnss_processor_arl2.exe`); pair columns `runs/_rap_arl_r2dp3` (exe `gnss_processor_arl3.exe`)
- **gates (frozen at pre-registration):** section 2

**Outcome.** Rung 0 answered: integers are recoverable on clean data once the estimator's ionosphere matches the data. With the slant ionosphere free, the floats converge only at the code rate (0.24 cycle after 1 to 2 h, nothing fixed); tied to zero, they sit 0.006 cycle from truth and the search fixes every epoch at 99.89 % correct. The only apparent wrong fixes come from one arc whose wind-up branch differs by one cycle between the simulator's truth and the producer (section 4): every kept fix is correct. Rung 1 answered: the satellite and receiver hardware biases, code, phase and both together, leave the result unchanged (0.006 cycle, every epoch fixed, no wrong fix); the producer absorbs them. Rung 2 answered for the search: with the ionosphere free, the integers sit in the uncombined float's wide-lane and ionosphere-free directions, and the entry-by-entry search could not reach them; a partial fix in the decorrelated space determines 42 % of the settled single-signal integers with no wrong fix and no external information. Open: the remaining 58 %, and the first hour. Section 7: the remaining pairs are limited by arc age (time), not by missing wide-lane information; an MW wide lane adds nothing the uncombined float lacks.

## 1. The ladder

One change per rung, on both sides when a layer needs a matching model: the fixture first (`gnss_simulator synth-network`, 60 stations and the held-out SZZ000SIM, GPS L1/L2, 2 h from 00:30:30), then the producer. Each rung is graded by `scripts/simnet_producer_ar_grade.py <fixture> --run <run> --settle-h 1`: the float double differences against truth, the share rounding would get right, the search's fix rate and its correct-fix rate (a wrong fix accepted at a reported success rate of 1 is the failure that matters).

- **Rung 0:** white noise only (code 0.3 m, phase 0.5 mm at zenith). No ionosphere (analytic background at 0 TECU, storm 0), no ZWD, no satellite or receiver signal biases, no code or phase multipath, no phase colour, no slips.
- **Rung 1:** satellite and receiver hardware biases on (they cancel in a double difference).
- **Rung 2:** ionosphere on; the estimator's slant ionosphere free, then constrained (how much absolute ionosphere information the integers need: the field's question).
- **Rung 3 and on:** troposphere, phase colour, slips (today's golden bed).

## 2. Gates (frozen)

- Each rung: the float error, the rounding ceiling, the fix rate and the correct-fix rate, in the settled hour and over the run; every wrong fix listed with its identity.
- The knob: off byte-identical in products and traces on the field golden; on, its rows counted by the "contributions as armed" table.

<!-- Results are appended below this line under dated headings. -->

## 3. 2026-10-09: rung 0

Fixture `runs/simarl_r0`: the ionosphere is zero in every truth record (`truth_atmo.csv`: slant TEC 0 everywhere), GPS observations C1C, L1C, C2W, L2W. Producer exe `gnss_processor_arl.exe` (main `03461e4` plus the knob).

| arm | settled hour: float error p50, within 0.25 cycle | fix rate | correct among kept | whole 2 h: float p50 |
| --- | --- | --- | --- | --- |
| `_rap_arl_r0`: golden posture (slant ionosphere free) | 0.241 cycle, 62.9 % (std 0.80) | 0 of 110 epochs (ratio p50 1.08) | none kept | 0.431 cycle |
| `_rap_arl_r0b`: + slant-ionosphere carry, q 1e-12 | 0.242 cycle, 62.9 % | 0 | none kept | unchanged |
| `_rap_arl_r0z`: + `network.sim_zero_iono_sigma_m` 0.001 | **0.006 cycle, 100.0 % (std 0.037)** | **110 of 110 (ratio p50 11.5)** | **13 023 of 13 037 = 99.89 %** | 0.026 cycle, 100 % correct over the first window |

- **The free slant ionosphere is what hides the integers.** With L1/L2 a narrow-lane integer is 99.3 % ionosphere, so a free per-line-of-sight ionosphere leaves the floats converging at the code rate; the clean data alone did not help. The carry of arm `r0b` acts only at relevels on this posture (74 priors in 2 h) and changes nothing.
- **The zero-ionosphere rows** (`network.sim_zero_iono_sigma_m`, one row per slant-ionosphere column aimed at absolute zero; 484 rows per epoch, counted as `sim_zero_iono` in the contributions table) make the floats integer to 0.006 cycle and the search fixes every epoch.
- **The 14 wrong fixes are one double difference:** S03000SIM/G20 against S01000SIM/G29, both bands, from TOW 267180 to 267360, the float itself one cycle off (e = -0.98) at a reported sigma of 0.011 to 0.015 cycle. A whole-cycle error in a float with a 0.01-cycle sigma is a carried-state defect (a re-seed, a reference change or a carry hand-off), not noise. Owed: its attribution, before rung 1.
- **The knob off** (`runs/_rap_arl_off_f2` against `runs/_rap_g15_f2`, field 2 h): `products.txt` and every CSV identical; the run log and manifest differ only by the new kind's line in the contributions table (0 rows).

## 4. 2026-10-09: the one-cycle double difference is a wind-up branch, not a defect

The 14 wrong fixes of section 3 all belong to S03000SIM/G20. Read from `network_amb_state.csv`, `network_pf_rows.csv` and the fixture's truth:

- G20 rises at S03000SIM at TOW 264810 (the simulator's 10 degree mask) and enters the producer's solve at 265890, at 15.01 degrees (the producer's mask). The carried ambiguity is born there, already off by -0.94 cycle on L1C and -0.95 on L2W (-0.179 m and -0.232 m: a ratio of 1.30, the wavelength ratio 1.28, an equal number of cycles on both bands, not an ionosphere-shaped error), with a birth variance of 0.0016 m2 (determined by the station's other carried phase arcs), and it never moves. Every other S03000SIM satellite and every other station's G20 is within 0.06 cycle.
- The simulator accumulates G20's wind-up at S03000SIM continuously from the rise: -0.471 cycle at 264810, **-0.501 at 265890**, -0.530 at the end (`truth_windup.csv`). The producer starts its own accumulator (`estimation.accumulate_phase_windup`) at the arc's first epoch with the wrapped value, so -0.501 becomes +0.499: one cycle above the simulator's branch, on both bands. The float absorbs -1 cycle from the first epoch, exactly the error measured.
- The fixed integers (-40 on L1C, +5 on L2W against the simulator's -39 and +6) are therefore the right integers in the producer's own wind-up convention; the grader's truth carries the simulator's branch. An integer absorbed into one station's ambiguity is invisible to the products' consumers.

**Rung 0, corrected: every kept fix correct (13 037 of 13 037 once the arc's branch is counted).** Owed to the grader (`scripts/simnet_producer_ar_grade.py`): count the wind-up branch of each arc at the producer's first epoch (the nearest integer to the producer's wrapped start minus the simulator's accumulated value), so a branch crossing near the mask is not graded as a wrong fix. The producer is not changed.

## 5. 2026-10-09: the grader's wind-up branch, and rung 1 (hardware biases)

**The grader** (`scripts/simnet_producer_ar_grade.py`, commit `540d8b5`) now grades each arc against the producer's integer, N_sim + k with k = round(wu_sim(t0)), the simulator's accumulated wind-up at the arc's first producer epoch. The arc start is read from `logs/network_slip_raw.csv` (one row per modelled dual-frequency phase line of sight, from the run's first epoch; a gap over the accumulator's 300 s horizon starts a new arc). `logs/network_amb_state.csv` is the wrong source: an arc is carried there later than its wind-up starts (S09000SIM/G11 at 261210 against 261030), and with it 30 arcs were shifted and 92 good fixes graded wrong. `--no-windup-branch` keeps the raw integers; the selftest covers both. Rung 0 regraded (`runs/_rap_arl_r0z`): 15 of 730 arcs on a shifted branch (S03000SIM/G20 and 14 G12 returns at 267720), 13 037 of 13 037 kept fixes correct in the settled hour, 13 957 of 13 957 in the first.

**The simulator** gains `corrections.signal_bias_scope` (`both`, the default; `code`; `phase`): with `corrections.signal_bias` the day's COD MGX OSB reaches only the named observables, and `truth_bias.csv` records 0 for the other. The rung-0 fixture regenerated with the new simulator is byte-identical (610 RINEX and truth files).

**Rung 1.** One fixture per layer, each rung 0 plus the biases named; the producer is the rung-0 zero-ionosphere arm (`network.sim_zero_iono_sigma_m` 0.001) pointed at the fixture, exe `gnss_processor_arl.exe`. Satellite biases: the COD MGX OSB product (GPS C1C, C2W, L1C, L2W; G01 for example C1C -1.906 m, C2W -2.800 m, L1C -0.192 m, L2W -0.285 m). Receiver code biases: the golden bed's per-station values (`simnet60_quiet_col_24h.json`, C2W up to about 0.5 m). Receiver phase biases: deterministic per station, L1C 0.08 sin(0.7 i) m and L2W 0.09 cos(1.3 i) m (fractions of a cycle). The producer does not apply the product OSB (`enable_code_phase_bias` false); it estimates the biases.

| arm | biases | settled hour: float p50, within 0.25 cycle | fix rate | correct among kept | first hour: correct among kept |
| --- | --- | --- | --- | --- | --- |
| `_rap_arl_r0z` | none | 0.006 cycle, 100.0 % | 110 of 110 (ratio p50 11.5) | 13 037 of 13 037 | 13 957 of 13 957 |
| `_rap_arl_r1a` | code (satellite and receiver) | 0.006 cycle, 100.0 % | 110 of 110 (11.4) | 13 037 of 13 037 | 13 965 of 13 965 |
| `_rap_arl_r1b` | phase (satellite and receiver) | 0.006 cycle, 100.0 % | 110 of 110 (11.6) | 13 037 of 13 037 | 13 965 of 13 965 |
| `_rap_arl_r1c` | code and phase | 0.006 cycle, 100.0 % | 110 of 110 (11.5) | 13 037 of 13 037 | 13 966 of 13 966 |

- **The hardware biases do not touch the integers.** Every rung fixes every settled epoch with no wrong fix; the float double differences are unchanged to the third decimal (unwrapped std 0.018 cycle on all four arms). The satellite and receiver phase biases are absorbed by the carried ambiguities and phase-bias states and cancel in the double difference, as they must.
- **The code biases make the code fit better, not worse.** Settled per-station post-fit code rms (`network_pf_keyed.csv`, normalised) p50 1.06 on rung 1a against 1.60 on rungs 0 and 1b. The producer pins each satellite's geometry-free code bias at the broadcast group delay (`sat_gf_datum broadcast_tgd`); a fixture with no code bias contradicts that pin by the whole group delay, while the product's real biases nearly match it. Rung 0's code residuals carried that contradiction; it did not reach the integers.
- Next, rung 2: the ionosphere on in the fixture, with the estimator's slant ionosphere free and then constrained.

## 6. 2026-10-09: rung 2 (the ionosphere), and a partial fix in the decorrelated space

**Fixture** `runs/simarl_r2` (`config/sim/arl/arl_r2_2h.json`): rung 1c (all hardware biases) plus the golden bed's ionosphere block verbatim (the simulator draws the slant TEC from the day's GIM at each pierce point: slant TEC p50 35, max 80 TECU at S02000SIM). The GIM shapes the simulator's truth only; the producer has no GIM, no ionosphere weighting and a free slant ionosphere per line of sight. The ionosphere is in the observations (G01 at S02000SIM: L1 code +8.7 m, L1 phase about -53 cycles against rung 1c's file).

**The golden posture** (`_rap_arl_r2a`: the rung-0 producer on this fixture, ionosphere free): identical to rung 0's free-ionosphere arm to the third decimal (float p50 0.241 cycle, 0 of 110 epochs fixed, ratio p50 1.08). A free slant ionosphere per line of sight absorbs any ionosphere exactly, so the field's source and shape do not reach this posture.

**Where the integers are.** The single-signal float errors split into their dual-frequency parts (`scripts/simnet_producer_ar_lanes.py`, a grading lens; the producer stays uncombined). Settled hour, `_rap_arl_r2a`:

| part | p50 error | p90 | within 0.25 |
| --- | --- | --- | --- |
| L1 single-signal | 0.214 cycle | 0.834 | 54.7 % |
| wide lane, L1 - L2 (86 cm) | 0.060 cycle | 0.235 | 91.0 % |
| ionosphere-free part, in narrow-lane cycles (10.7 cm) | 0.040 cycle | 0.151 | 97.0 % |

The floats carry the integers; the search does not reach them. Its formal per-band sigma is honest (L1 p50 0.310 cycle against the 0.214 error), and the carry-aware covariance (`_rap_arl_r0ca`, `network.carry_aware_covariance`) changes nothing. Two gates of the entry-by-entry search (`ils::ilsFix` with `ils::selectFixableSubset`) cannot pass under a free ionosphere: the fallback drops the single-signal DD of largest variance one at a time, which breaks the L1/L2 pairs the information is in, and the 0.10-cycle per-entry cap (`nl_max_per_pair_frac`) rejects every correct fix whose single-signal float sits 0.2 to 0.3 cycle from its integer.

**The build** (`ils::ilsFixDecorrelatedPartial`, `network.ambiguity_resolution.decorrelated_partial`, default off). The covariance is decorrelated (Q = Lt D L and the MLAMBDA reduction); the largest trailing set of decorrelated components whose bootstrap success rate is at least `min_success_rate` (0.9999) is searched, shrunk by one component while the ratio test (3.0) fails; the single-signal floats are conditioned on the fixed components; a single-signal DD is reported fixed when its conditional sigma is at most `decorrelated_determined_sigma_cyc` (0.05) and its conditional value lies within `nl_max_per_pair_frac` of an integer. The per-DD trace and the grader are unchanged. Unit tests `tests/test_ils_decorrelated_partial.cpp` (4 cases: the free-ionosphere pair geometry recovers every integer where `ilsFix` fixes none; imprecise floats fix nothing; one precise pair beside one imprecise pair fixes only the precise one). Off: products and every trace of `_rap_arl_r2off` identical to `_rap_arl_r2a` except the wall-clock timing columns.

| arm | settled hour: epochs with a fix | single-signal DDs fixed | correct | first hour: epochs, fixed, correct |
| --- | --- | --- | --- | --- |
| `_rap_arl_r2a`, entry-by-entry (golden) | 0 of 110 | 0 | n/a | 0 of 120 |
| `_rap_arl_r2dp`, decorrelated | **94 of 110** (ratio p50 3.13) | **5 572 of 13 138 (42 %)**, 2 786 per band | **5 572** | 59 of 120; 1 770; 1 770 |
| `_rap_arl_r0z`, zero ionosphere, entry-by-entry | 110 of 110 | 13 037 | 13 037 | 120 of 120; 13 957; 13 957 |
| `_rap_arl_r0zdp`, zero ionosphere, decorrelated | 110 of 110 | 13 084 | 13 084 | 120 of 120; 16 610; 16 610 |

- **Single-signal integers come out of the uncombined float with the ionosphere free**, with no ionosphere constraint and no external product: 42 % of the settled double differences, every one correct, L1 and L2 always together (a single-signal integer is determined only when both its wide-lane and ionosphere-free directions are fixed). Per epoch the search fixes 82 of about 112 decorrelated components at the median (0 to 114).
- **The ratio test is the binding gate** (p50 3.13 against the 3.0 threshold on fixed epochs); the bootstrap rate of the fixed set is 1.0000.
- On clean data the decorrelated search fixes a little more than the entry-by-entry one (13 084 against 13 037 settled; 16 610 against 13 957 in the first hour) with no wrong fix.
- Open: the 58 % left float and the first hour (49 % of epochs); then the troposphere, phase colour and slips (rung 3), where the float's wide-lane and ionosphere-free precision is the question.

## 7. 2026-10-09: the pairs left float, and whether a Melbourne-Wubbena wide lane would fix them

**Instrument.** With `decorrelated_partial` armed, the per-DD trace gains four pair columns (`include/logging/NetworkRunTraces.hpp`, `kProducerArHeaderDecorrelated`; `netar::pairLaneColumns`): the pair's wide-lane formal sigma before the fix, its wide lane conditioned on the fixed decorrelated components (value and sigma), and the conditioned narrow-lane sigma ((f1 a1 - f2 a2) / (f1 - f2) narrow-lane cycles). Off, the header and rows are unchanged; on, the first 19 columns and the products of `runs/_rap_arl_r2dp3` are identical to `_rap_arl_r2dp`. Two unit tests (suite `ProducerArPairLanes`). `scripts/simnet_producer_ar_pairs.py` sorts every pair-epoch against truth; `scripts/simnet_mw_widelane.py` forms the double-differenced Melbourne-Wubbena wide lane from the fixture's RINEX, averaged causally over each producer arc, for the same pair-epochs.

**The pairs** (settled hour, 6 569 pair-epochs; a wide lane is fixed when its conditional sigma is at most 0.05 cycle):

| class | share | integer correct | float truth error p50 / p90: wide lane, narrow lane (cycles) |
| --- | --- | --- | --- |
| single-signal integers fixed | 42.4 % | 2 786 of 2 786 | |
| wide lane fixed, narrow lane not | 25.4 % | 1 670 of 1 670 (wide lane) | 0.052 / 0.141, 0.068 / 0.201 |
| wide lane not fixed | 32.2 % | | 0.148 / 0.484, 0.062 / 0.193 |

Over the whole run (first hour included) the shares are 24.6 %, 21.9 % and 50.3 %, with 3.3 % of pair-epochs on epochs that fixed nothing.

**The Melbourne-Wubbena wide lane** (geometry, clocks, troposphere, ionosphere and wind-up cancel; every hardware bias cancels in the double difference; undifferenced scatter 0.559 wide-lane cycle per epoch):

| class | MW error p50 / p90 | MW rounds to truth | MW formal sigma p50 | float wide lane error p50 / p90 |
| --- | --- | --- | --- | --- |
| single-signal integers fixed | 0.054 / 0.133 | 100.0 % | 0.089 | 0.041 / 0.128 |
| wide lane fixed only | 0.075 / 0.174 | 100.0 % | 0.109 | 0.052 / 0.141 |
| wide lane not fixed | 0.145 / 0.487 | 90.8 % | 0.238 | 0.148 / 0.484 |

- **An MW wide-lane input would not help.** Where the search leaves the wide lane free, the arc-averaged MW wide lane is as poor as the float's own (0.145 against 0.148 cycle p50, 0.487 against 0.484 p90); where the wide lane is fixed, the float is better than MW. The uncombined filter already holds every bit of code information MW carries. (Earlier MW work on this project carried the wide lane as filter states, the lane layer; this measures MW only as an input to the search.)
- **The limit is arc age.** The MW formal sigma is the per-epoch double-difference noise over the square root of the youngest of the four arcs; its medians correspond to about 22 epochs (11 min) on the youngest arc of a "wide lane not fixed" pair, about 105 (53 min) for "wide lane fixed only" and about 158 (79 min) for a fixed pair. A wide lane needs roughly a quarter to half an hour of arc at this code noise (0.3 m at zenith), and the narrow lane about an hour; rising satellites keep a steady share of young arcs in every epoch.
- **What the narrow-lane class lacks is precision, not bias:** its narrow-lane float error (0.068 p50) is as small as that of the pairs whose wide lane is still free (0.062), and every wide lane in the class is correct. The bootstrap rate of 0.9999 and the ratio test (p50 3.13 on fixed epochs) hold it back until the arc lengthens.
- Next: rung 3 (troposphere, phase colour, slips), where the question is whether this time scale holds; and, separately, whether an ionosphere constraint between neighbouring stations shortens it (a slant-ionosphere prior narrows the ionosphere direction that both lanes share).
