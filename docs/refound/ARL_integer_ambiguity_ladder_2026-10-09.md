# ARL: the integer-ambiguity ladder on the simulated network

**STATUS**

- **id:** ARL
- **topic:** network-vehicle
- **status:** open
- **summary:** Can the network producer recover the true integer ambiguities, and what stops it? A ladder on the simulated network, one layer of complexity per rung, 2 h runs, graded against truth. Rung 0 (clean data: no ionosphere, biases, troposphere, multipath or phase colour) with the golden posture's free slant ionosphere: floats 0.24 cycle from truth, no epoch fixed. Rung 0 with the estimator told the ionosphere is zero (`network.sim_zero_iono_sigma_m`, test-only, default off): floats 0.006 cycle, every epoch fixed, 99.89 % of kept fixes correct; the 14 wrong fixes are one double difference sitting on the wrong integer at sigma 0.01 cycle (a carried-state defect, open).
- **source:** the author's request (2026-10-09): "turn off all iono, hardware bias, etc., then we can easily check if integer ambiguities are recoverable"; [project memory on network AR] the September carry-blind covariance finding (14.5 % wrong fixes on the retired triple-frequency posture); the L1/L2 geometry (a narrow-lane integer is 99.3 % ionosphere)
- **knobs / posture:** `config/golden/golden_sim_refound.json` pointed at the rung fixtures (`config/sim/arl/arl_r0_producer.json`), with `network.ambiguity_resolution.enable` and `.trace` (the producer's search, log-only: nothing is written back); `network.sim_zero_iono_sigma_m` (new, test-only, default 0)
- **dates in the text:** 2026-10-09
- **hashes / run dirs:** fixture `runs/simarl_r0` (`config/sim/arl/arl_r0_clean_2h.json`); runs `runs/_rap_arl_r0`, `_rap_arl_r0b`, `_rap_arl_r0z`; off identity `runs/_rap_arl_off_f2` against `runs/_rap_g15_f2`
- **gates (frozen at pre-registration):** section 2

**Outcome.** Rung 0 answered: integers are recoverable on clean data once the estimator's ionosphere matches the data. With the slant ionosphere free, the floats converge only at the code rate (0.24 cycle after 1 to 2 h, nothing fixed); tied to zero, they sit 0.006 cycle from truth and the search fixes every epoch at 99.89 % correct. One double difference holds a whole-cycle error with a 0.01-cycle sigma: a defect of ours, the first item of the ladder's estimator column.

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
