# ARL: the integer-ambiguity ladder on the simulated network

**STATUS**

- **id:** ARL
- **topic:** network-vehicle
- **status:** open
- **summary:** Can the network producer recover the true integer ambiguities, and what stops it? A ladder on the simulated network, one layer of complexity per rung, 2 h runs, graded against truth. Rung 0 (clean data: no ionosphere, biases, troposphere, multipath or phase colour) with the golden posture's free slant ionosphere: floats 0.24 cycle from truth, no epoch fixed. Rung 0 with the estimator told the ionosphere is zero (`network.sim_zero_iono_sigma_m`, test-only, default off): floats 0.006 cycle, every epoch fixed, 99.89 % of kept fixes correct; the 14 apparent wrong fixes are one arc whose wind-up starts across the branch cut between the simulator (10 degree mask) and the producer (15 degrees), so every kept fix is correct in the producer's convention (section 4), and the grader now counts each arc's branch. Rung 1 (section 5): the satellite and receiver code biases, then the phase biases, then both, each on the rung-0 zero-ionosphere posture: every epoch fixed and every kept fix correct on all three. Rung 2 (section 6): the ionosphere on in the fixture and free in the estimator (no GIM, no external product): the wide-lane and ionosphere-free parts of the floats are precise (0.06 and 0.04 cycle) but the entry-by-entry search fixes nothing; a partial fix in the decorrelated space (`network.ambiguity_resolution.decorrelated_partial`, new, default off) fixes 94 of 110 settled epochs and determines 5 572 single-signal L1/L2 integers, every one correct. Of the pairs left float (section 7), 25 % have the wide lane fixed and 32 % do not; a Melbourne-Wubbena wide lane averaged over the arc is no better than the float's own (0.145 against 0.148 cycle p50), and the limit is arc age: both classes need more time, not an MW input. Rung 3 (section 8): the troposphere changes nothing; the golden bed's phase colour cuts the settled pairs fixed from 42 % to 19 %; one unflagged 25-cycle slip at 76 degrees is detected at its epoch and every later fix on the arc is correct; no wrong fix on any rung. The search's star basis reaches about a fifth of the network's independent integers.
- **source:** the author's request (2026-10-09): "turn off all iono, hardware bias, etc., then we can easily check if integer ambiguities are recoverable"; [project memory on network AR] the September carry-blind covariance finding (14.5 % wrong fixes on the retired triple-frequency posture); the L1/L2 geometry (a narrow-lane integer is 99.3 % ionosphere)
- **knobs / posture:** `config/golden/golden_sim_refound.json` pointed at the rung fixtures (`config/sim/arl/arl_r0_producer.json`), with `network.ambiguity_resolution.enable` and `.trace` (the producer's search, log-only: nothing is written back); `network.sim_zero_iono_sigma_m` (new, test-only, default 0); simulator `corrections.signal_bias_scope` (new, test-only, default `both`: `code` or `phase` stages the satellite OSB one side at a time); `network.ambiguity_resolution.decorrelated_partial` and `.decorrelated_determined_sigma_cyc` (new, default off and 0.05)
- **dates in the text:** 2026-10-09
- **hashes / run dirs:** fixture `runs/simarl_r0` (`config/sim/arl/arl_r0_clean_2h.json`); runs `runs/_rap_arl_r0`, `_rap_arl_r0b`, `_rap_arl_r0z`; off identity `runs/_rap_arl_off_f2` against `runs/_rap_g15_f2`; rung 1 fixtures `runs/simarl_r1{a,b,c}` (`config/sim/arl/arl_r1{a,b,c}_2h.json`), runs `runs/_rap_arl_r1{a,b,c}` (`config/sim/arl/arl_r1{a,b,c}_producer.json`); rung 2 fixture `runs/simarl_r2` (`config/sim/arl/arl_r2_2h.json`), runs `runs/_rap_arl_r2a`, `_rap_arl_r0ca`, `_rap_arl_r2dp`, `_rap_arl_r0zdp`, off identity `_rap_arl_r2off` against `_rap_arl_r2a` (exe `gnss_processor_arl2.exe`); pair columns `runs/_rap_arl_r2dp3` (exe `gnss_processor_arl3.exe`); rung 3 fixtures `runs/simarl_r3{a,b,c}` (`config/sim/arl/arl_r3{a,b,c}_2h.json`), runs `runs/_rap_arl_r3{a,b,c}dp`
- **gates (frozen at pre-registration):** section 2

**Outcome.** Rung 0 answered: integers are recoverable on clean data once the estimator's ionosphere matches the data. With the slant ionosphere free, the floats converge only at the code rate (0.24 cycle after 1 to 2 h, nothing fixed); tied to zero, they sit 0.006 cycle from truth and the search fixes every epoch at 99.89 % correct. The only apparent wrong fixes come from one arc whose wind-up branch differs by one cycle between the simulator's truth and the producer (section 4): every kept fix is correct. Rung 1 answered: the satellite and receiver hardware biases, code, phase and both together, leave the result unchanged (0.006 cycle, every epoch fixed, no wrong fix); the producer absorbs them. Rung 2 answered for the search: with the ionosphere free, the integers sit in the uncombined float's wide-lane and ionosphere-free directions, and the entry-by-entry search could not reach them; a partial fix in the decorrelated space determines 42 % of the settled single-signal integers with no wrong fix and no external information. Open: the remaining 58 %, and the first hour. Section 7: the remaining pairs are limited by arc age (time), not by missing wide-lane information; an MW wide lane adds nothing the uncombined float lacks. Rung 3 (section 8): troposphere neutral, phase colour the costly layer, one big slip handled; every fix still correct. Open: the star basis covers about a fifth of the network's integers.

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

## 8. 2026-10-09: rung 3 (troposphere, phase colour, one slip)

Three cumulative fixtures, each the previous plus one layer from the golden bed (`simnet60_quiet_col_24h.json`, copied verbatim): **3a** the troposphere (ZWD 0.12 m, random walk 0.01 m, tau 3600 s); **3b** the coloured phase error (band-common Gauss-Markov, tau 300 s, 4.4 to 6.0 mm at zenith by latitude class); **3c** one scripted slip, S43000SIM / G22 / L1C +25 cycles at TOW 261930 (15 minutes into the run, G22 at 76 degrees), no LLI flag. Producer: the rung-2 arm with `decorrelated_partial` (exe `gnss_processor_arl3.exe`).

| rung | settled epochs with a fix | single-signal DDs fixed, correct | pairs: fixed / WL only / WL free | narrow-lane float error p50 (WL-only class) |
| --- | --- | --- | --- | --- |
| 2 (`_rap_arl_r2dp3`) | 94 of 110 | 5 572, 5 572 | 42.4 / 25.4 / 32.2 % | 0.068 |
| 3a troposphere | 94 of 110 | 5 574, 5 574 | 42.4 / 25.5 / 32.1 % | 0.068 |
| 3b + phase colour | 75 of 110 | 2 434, 2 434 | 18.5 / 33.9 / 47.6 % | 0.098 |
| 3c + one slip | 75 of 110 | 2 496, 2 496 | 19.0 / 33.4 / 47.6 % | 0.100 |

- **The troposphere is neutral**: the estimated ZWD absorbs it.
- **Phase colour is the costly layer**: the share of pairs fixed falls from 42 % to 19 %, the narrow-lane float error in the wide-lane-only class grows from 0.068 to 0.098 cycle, and among pairs with the wide lane free the narrow-lane error grows from 0.062 to 0.147. No fix is wrong: the gates hold the fixing back rather than accepting coloured floats.
- **The slip is handled.** At TOW 261930 the producer flags G22 at S43000SIM by the Melbourne-Wubbena test (`T0_mw_detect`) and the ionosphere-free time difference (`T1_if_slip`), refloats both bands (band scope `common_band`, no exoneration of L2W), and the arc re-converges. Graded against the truth that includes the +25 cycles, the arc's double differences after the slip have float error p50 0.074 (L1C) and 0.103 cycle (L2W), and all 86 fixes on it are correct.
- **The search sees a fifth of the network.** The arc enters the search basis only at TOW 262980, when the reference station changes to one that sees G22. The basis is a star per (constellation, signal): one reference satellite and one reference station, one DD per (station, satellite) that both the station and the reference station track. At TOW 264000 it holds 136 DDs from 190 undifferenced ambiguities, while the network carries 460 ambiguities per band; about 370 per band are independent (ambiguities minus stations minus satellites plus one). Every fix rate on this card is a share of the star's 68 per band. Building a full independent basis (a spanning tree of baselines, each with its own common reference satellite) is the step that makes the search a network search.

## 9. 2026-10-09: a spanning-tree basis for the search (first version)

`network.ambiguity_resolution.tree_basis` (new, default off; the star is unchanged when off): per (constellation, signal) a greedy spanning tree of station baselines, rooted at the station with the most satellites; each outside station is attached to the in-tree station it shares the most satellites with (at least two), and the baseline contributes one DD per common satellite against the first common one. Each baseline brings in a new station's ambiguities, so the set is independent by construction. Exe `gnss_processor_arl4.exe`, with `decorrelated_partial`; runs `runs/_rap_arl_r2tree`, `runs/_rap_arl_r3ctree`.

| settled hour | star (`_r2dp3` / `_r3cdp`) | tree |
| --- | --- | --- |
| DDs searched, both bands, p50 | 112 | 682 |
| rung 2: epochs with a fix; single-signal DDs fixed, correct | 94 of 110; 5 572, 5 572 | 110 of 110; 6 508, 6 508 |
| rung 3c: epochs with a fix; single-signal DDs fixed, correct | 75 of 110; 2 496, 2 496 | 110 of 110; 3 854, 3 854 |
| first hour, rung 3c: epochs; fixed, correct | 53 of 120; 772, 772 | 76 of 120; 1 540, 1 540 |
| run time, 2 h of data | about 105 s | about 560 s |

- Six times the basis, more integers fixed in absolute terms, every epoch fixing, and no wrong fix. The share of pairs fixed is lower (rung 2: 8.6 % fixed, 19.1 % wide lane only, 72.2 % wide lane free; rung 3c: 5.1 / 13.2 / 81.7 %): the tree reaches the young and long baselines the star never saw, and one joint ratio test over about 680 dimensions is stricter than over 112.
- Open: the shares (a ratio test per baseline or per block, or a lower joint threshold, against the 0.9999 bootstrap bar), the run time of the larger search, one owner shared with the chain datum, then the harder slips.

## 10. 2026-10-09: the search block by block (one block per baseline)

`network.ambiguity_resolution.block_search` (new, default off; with `decorrelated_partial`): the search rows of one baseline (its L1 and L2 DDs, keyed by constellation, sta_r and ref_sta) are fixed together by `ils::ilsFixDecorrelatedPartial` on their marginal covariance, each block with its own bootstrap (0.9999) and ratio test (3.0), in `netar::blockDecorrelatedFix`. The trace's ratio is the smallest accepted block ratio and its p_succ the product of the accepted blocks' bootstrap rates. Two unit tests (suite `BlockDecorrelatedFix`). Exe `gnss_processor_arl5.exe`; runs `runs/_rap_arl_r2blk`, `runs/_rap_arl_r3cblk` (tree basis, decorrelated, block search).

| settled hour | tree, one joint search (section 9) | tree, block search |
| --- | --- | --- |
| rung 2: single-signal DDs fixed, correct | 6 508, 6 508 | **37 072, 37 072** |
| rung 2: pairs fixed / WL only / WL free | 8.6 / 19.1 / 72.2 % | **49.2 / 27.6 / 23.2 %** |
| rung 3c: single-signal DDs fixed, correct | 3 854, 3 854 | **36 212, 36 212** |
| rung 3c: pairs fixed / WL only / WL free | 5.1 / 13.2 / 81.7 % | **48.1 / 28.3 / 23.7 %** |
| rung 3c first hour: epochs with a fix; fixed, correct | 76 of 120; 1 540, 1 540 | 109 of 120; 10 128, 10 128 |
| run time, 2 h of data | about 560 s | about 189 s |

- **One weak baseline no longer refuses the network.** On the realistic rung (phase colour and a slip) about half of all the tree's pairs are fixed in the settled hour, against 19 % of the star's much smaller basis (section 8), with no wrong fix in 46 340 kept integers over the run.
- **Faster**: the cost of the joint search (one ~680-dimensional search, shrunk one component at a time on a failed ratio test) is gone; what remains is the covariance readback and the rest of the run (the star without the decorrelated search ran about 190 s).
- **Caveat**: each block is fixed on its marginal covariance, ignoring the correlation with the other baselines. Each block's bootstrap rate is at least 0.9999, so an epoch's bound is the product (about 60 blocks: about 0.994); the run's zero wrong fixes are the measurement, not a guarantee.
- Open: the shared owner with the chain datum; the harder slips; feeding the fixes back (Phase 3).

## 11. 2026-10-10: 300 stations, and the search made cheap

**Fixture** `runs/simarl_n300` (`config/sim/arl/arl_n300_2h.json`): rung 3b (every hardware bias, the GIM-shaped ionosphere, the troposphere, phase colour; no slip) on the 300-station roster of `simnet300_quiet_12h.json` plus its held-out consumer; receiver phase biases as on the 60-station rungs; one network-level phase colour (5.0 mm at zenith, tau 300 s). Producer `config/sim/arl/arl_n300_producer.json` (`scripts/simnet_make_producer_config.py` from the rung-3c block arm; elected datum station S14801SIM), with `decorrelated_partial`, `tree_basis` and `block_search`.

**Integers at 300 stations** (`runs/_rap_arl_n300blk`, settled hour): 4 022 DDs searched per epoch (p50) from about 5 000 undifferenced ambiguities; 110 of 110 epochs fixed; **312 218 single-signal integers fixed, every one correct** (first hour: 117 of 120 epochs, 108 744, all correct). Pairs: 70.6 % fixed, 5.3 % wide lane only, 24.1 % wide lane free (60 stations: 48.1 / 28.3 / 23.7 %): the denser network gives shorter baselines.

**Cost.** Two changes, both with identical decisions (every trace row: 938 094 at 300 stations, 159 036 at 60; the 60-station products and trace byte-identical):

- `NetworkNormalEquations::blockConditionalCovariances` (and its `NetworkEstimator` wrapper): in block mode the producer reads back only each baseline's block of the joint conditional covariance (the same uncorrected expression, with Cov_free M formed as one product) and differences it in place, instead of the full k x k readback (k about 5 000) and two dense products with a 4 000 x 5 000 differencing matrix. Used when `block_search` is armed without the carry-aware covariance and without `ils_max_dim`.
- The spanning tree is built by Prim's algorithm on satellite bitmasks (each outside station keeps its best in-tree partner), O(stations^2) popcounts, with the same tie-breaking as the exhaustive scan.

| 2 h, ms per epoch | 60 stations | 300 stations |
| --- | --- | --- |
| producer, search off | not run | 863 (`_rap_arl_n300off`) |
| tree + blocks, full readback | 633 (`_r3cblk`) | 39 624 (`_n300blk`) |
| + block readback | 302 (`_r3cblk6`) | 7 678 (`_n300blk6`) |
| + Prim tree | 286 (`_r3cblk7`) | **1 106** (`_n300blk7`) |

At 300 stations the integer step now costs about 0.24 s per epoch on top of the producer's 0.86 s (exes `gnss_processor_arl6.exe`, `gnss_processor_arl7.exe`). (At 60 stations no search-off arm was run; `_rap_arl_r2off`, 663 ms per epoch, ran the entry-by-entry search on the star.)

## 12. 2026-10-10: the slip ladder (eleven slips, the search log-only)

Each rung is rung 3b plus one scripted slip at epoch 31 (TOW 261930), paired exactly with the no-slip control `runs/simarl_r3b` (scripted slips use no random stream: only the slipped station's RINEX differs and every trace is identical before the slip). Fixtures `runs/simarl_{s1,s2,s3,s3b,s4,s4b,s5,s6a,s6b,s7,s8}` (`config/sim/arl/arl_<rung>_2h.json`); runs `runs/_rap_arl_<rung>` and the control `runs/_rap_arl_r3bblk7`, all with the tree basis, the decorrelated fix and the block search (exe `gnss_processor_slips.exe`, a copy of `gnss_processor_arl7.exe`). Targets were chosen off the hidden roles: G14 at S43000SIM is not the MW pivot, the IF-TD reference, its phase anchor or the search's reference satellite. Analysed by one analyst and one independent skeptic per rung (workflow `arl-slip-ladder`); the numbers below are the skeptic-corrected ones.

| rung: slip | detected by, latency | response | slipped arc after; wrong fixes (kept, 2 h) |
| --- | --- | --- | --- |
| S1: G14 L1C +25, no LLI, 57 deg | IF-TD pool arm (12.1 m against 0.107 m), 0 epochs; an MW echo refloats the arc again one epoch later | both bands, twice | ionosphere-shaped offset -0.25 / -0.31 cycle decaying to -0.07 at 95 min; 0 wrong (46 408) |
| S2: G14 L1C +1 | IF-TD pool arm (4.6x the floor), 0 epochs; MW silent | both bands (L2W freed though clean) | offset peaks -0.61 / -0.78 cycle at +6 epochs; 0 wrong (46 406) |
| S3: G14 L2W +1 with LLI | LLI drain corroborated by the raw GF step, plus IF-TD, 0 epochs | L2W only (rule F3) | within 0.007 cycle of the control; 0 wrong (46 440) |
| S3b: false LLI on L2W, no slip | LLI drain on station credibility alone (every witness flat), 0 epochs | L2W freed with no slip | within 0.007 cycle; 0 wrong (46 440) |
| S4: (+2, +2), MW-blind | IF-TD pool arm (2.0x), 0 epochs | both bands | offset decays over 1 to 2 h; 0 wrong (46 406) |
| S4b: (+1, +1), IF step equal to the floor | IF-TD pool arm at 1.035x (this epoch's noise); replayed on control noise the detection rate is 41 to 47 %, 18 % at 15 to 20 deg | both bands | offset -0.27 / -0.35 cycle; 0 wrong (46 406) |
| S5: G26 at S29000SIM (+2, +2), 17 deg | IF-TD pool arm (2.0x), 0 epochs | both bands; a receiver-DCB gauge move at two stations | 0 wrong (46 406) |
| S6a: G22 L1C +1 (the IF-TD reference and MW pivot) | IF-TD reference gate (8.9x), 0 epochs | both bands; 4 dropped MW verdicts on clean arcs (pivot fan-out) | wide lane re-determined at +4 epochs; 0 wrong (46 446) |
| S6b: G17 L1C +1 on a phase-datum anchor arc at the clock datum station | IF-TD pool arm (4.4x), 0 epochs | both bands; continuous anchor handover to S33000SIM in the slip epoch | first fix 6 epochs later than the control; 0 wrong (48 538) |
| S7: G14 L1C +2 twice (the second in the post-flag epoch) | first: IF-TD, 0 epochs; second: no IF-TD sample (chain reset), MW silent, caught after the solve by RAIM T4 | second response also drops G05 and G13 at the station (no slip) | 0 wrong (46 348) |
| **S8: (+4, +5), blind to MW and IF-TD** | **nothing before the solve**: IF-TD 0.51x the floor, MW flag at +3 epochs dropped as an unknown integer, raw GF step -0.46 m wired to nothing | none | **6 wrong single-signal fixes** (one pair, S43000SIM/G14 against S30000SIM/G05, fixed (-120, +3) against true (-117, +7) at 261990 to 262050) and 16 wrong wide-lane determinations up to +450 s |

- **The ordinary detection path works.** Ten of the eleven slips are flagged at their epoch on the right arc, and the search makes no wrong fix on them.
- **The one failure is the class every pre-solve detector is blind to.** On S8 the free slant ionosphere absorbs -0.709 of the -0.711 m dispersive step; the float moves only about half a cycle while the truth moves four and five, and lands near a wrong lattice point that looks more fixable than the control's float did. The search's own gates cannot tell (identical sigma, ratio 3.2 to 3.45 against a correct-fix median of 3.21). A held integer there would lock about 0.63 m of slant-ionosphere error into the station's clock and phase biases.
- **Defects ranked before any hold** (analysis section 3): (1) no pre-solve trigger for slips under the IF-TD floor and MW-blind (an arc-scoped geometry-free jump test, r = dGF(t) - dGF(last clean step) above 0.04 m, flags every slipped rung at its epoch including S8, S4b and S7's second repetition, with 0 flags in 115 901 clean sim samples; on the field the fixed 0.04 m test flags 1.2 % of samples, 72 % of them at SCTB00ATA and CAS100ATA, so the field needs a per-arc noise scale); (2) the IF-TD chain reset leaves a blind epoch after every flag; (3) the search reads no slip state; (4) RAIM T4 attribution spreads through the whitened carried-prior row to clean arcs at the station (S7: G05, G13); (5) MW duplicate, late and false releases (S1 echo, the pivot fan-out in S6a, two false refloats on the clean control at S35000SIM); (6) both bands released on single-band slips and a false LLI released unconditionally; (7) no single complete release seam (T4 drops outside the relevel latch, anchor arcs respond as births); (8) a release moves the gauge and creates information; (9) telemetry (the per-epoch refloat columns read 0 in every run).
- Untested: the (1,1) rate over many events, the settled-hour twins (no rung released a held single integer: every slip came before the arc's first fix), the bridged gap of 120 to 300 s, and the variants (3,4), (7,9) and the S8 shape on an anchor arc.
