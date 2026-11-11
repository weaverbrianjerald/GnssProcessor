# L5B6: GPS L5 in the network over 6 h, with and without the freed arc's variance floor (continuation of card L5B)

**STATUS**

- **id:** L5B6
- **topic:** network-vehicle
- **status:** open
- **summary:** PRE-REGISTERED 2026-11-10. Continuation of card [L5B](L5B_band5_receiver_delay_2026-10-10.md) (that file stands at the 60 KB bar; its section 16 points here). Stage A: the four 2 h field runs of L5B section 15 repeated on the goldens of main `b6bbea7`, then 6 h on field DOY 129, field DOY 132 and the sim: dual control, dual plus `network.amb_float_drop_var_m2` 100, the three-band `t1on` keys, `t1on` plus the floor. Stage B (measure only): the three-band user on stage A's corrections and the roof receiver's band-5 drop. Nothing armed; run keys only; no C++ change.
- **source:** card L5B sections 12 to 15 (the `t1on` keys, the event at TOW 260040, the floor's 2 h pass); card [SMX2](../record/refound/SMX2_scenario_matrix_current_golden_2026-11-09.md) sections 5.3, 5.6 and 6 (the 2 h grader, the floor's costs K1 and card SLV's); card [ALD](../record/refound/ALD_anchor_level_datum_2026-10-23.md) section 5 (gates 4 and 5); card [R1D](../record/refound/R1D_station_level_datum_2026-10-23.md) (the three-band bars (B)); the author's directive that L5 is never closed
- **knobs / posture:** the goldens of main `b6bbea7` (`config/golden/golden_field_refound.json`, `config/golden/golden_sim_refound.json`); the `t1on` keys (`processing.frequency_mode` triple, the GPS band-5 types C5Q, C5X, C5I, L5Q, L5X, L5I and their D and S types appended to `processing.signal_priorities.GPS`, `network.sat_gf_extra_band_free`, `network.estimate_rx_extra_band_code_delay`); the floor `network.amb_float_drop_var_m2` 100. DOY 132 from `scripts/mday_golden_config.py` (obs root `D:/GnssProcessorSupport/data_mday`). Sim runs with `network.start_offset_hours` 0.504167.
- **dates in the text:** 2026-11-10
- **hashes / run dirs:** branch `l5/six-hour-read` (docs only) from main `b6bbea7`; exe `gnss_processor_l5read.exe`, a renamed copy of `D:/GnssProcessorSupport/gates/exe_keep/int1110/gnss_processor_int1110b.exe` (sha256[:16] `28e4286ee0805093`); runs `D:/GnssProcessorSupport/runs_nom/_rap_l56_<arm>_<bed>` (arms `ctl`, `A`, `t1on`, `t1onA`; beds `f2` DOY 129 2 h, `f6` DOY 129 6 h, `g6` DOY 132 6 h, `s6` sim 6 h from 00:30:15)
- **gates (frozen at pre-registration):** section 2

**Outcome.** Pre-registered; no result yet.

> Results are appended TO THIS FILE, below the text.

## 1. The question

Card L5B section 15 and card SMX2 section 5.6 found that the floor `network.amb_float_drop_var_m2` 100 removes the three-band event at TOW 260040 (MAJU00MHL, G08) and passes the three-band bars of card R1D (B) and the dual bar on 2 h of DOY 129, on the goldens of main `27d6ece`. Not read there: hours 3 to 6, the 16 held-out sites, a second day, the sim. Main `b6bbea7` armed more keys since (the wet-delay link and the phase colour state among them). The question: does the 2 h result hold on today's goldens, and what do 6 h on two field days and the sim say for and against the floor, with and without GPS L5? The floor's arming is the author's decision; this card reports, it does not recommend.

In every `t1on` run the held-out user is itself three-band (it reads the run's `processing.frequency_mode`; on `_rap_smx2_d_t1onA_f2` its GPS L5X rows enter at WTZZ00DEU), so the three-band user's numbers mix the network's and the user's share; stage B separates them.

## 2. Stage A: runs and frozen bars

**Runs** (one exe, `scripts/rapid_arm.py make`, every configuration a golden copy with only the keys under test and the run paths changed; at most 4 processes at once):

| bed | base | runs | control |
| --- | --- | --- | --- |
| `f2` | field golden, DOY 129, 2 h from its start | `ctl`, `A`, `t1on`, `t1onA` | `_rap_l56_ctl_f2` |
| `f6` | field golden, DOY 129, 6 h | `A`, `t1on`, `t1onA` (and `ctl` unless `_rap_int1110b_f6`'s configuration is content-identical, in which case that run is the control) | `_rap_l56_ctl_f6` or `_rap_int1110b_f6` |
| `g6` | field golden moved to DOY 132, 6 h | `ctl`, `A`, `t1on`, `t1onA` | `_rap_l56_ctl_g6` |
| `s6` | sim golden, 6 h from 00:30:15 | `A`, `t1on`, `t1onA` (and `ctl` on the same rule against `_rap_int1110b_s6`, which started at the golden's start and is therefore re-run) | `_rap_l56_ctl_s6` |

**Bars (frozen).** Each run against its bed's dual control, as in card SMX2 section 5.6 (grader of that section generalized to N hours: the held-out user's 3D p50 by hour from `logs/closed_loop_trace.csv`; the clock wire from `scripts/ftc_step_attrib.py --r3 --systems G,E`, row `e_clk`, 'hours 2-6', all and user-tracked satellites; the GPS band 1 / 2 / 5 code offsets from `scripts/clock_level/band5_offsets.py`, hours 2 to end; clock-anchor decouples over the run and at TOW 260040).

- **(H2) The 2 h repeat holds** when, on `f2`, `t1onA` passes the three-band bars below in hours 1 and 2, `A` passes the dual bar, and `t1onA` has at most 3 decouples at TOW 260040 with MAJU00MHL's GPS_DCB and GPS_IFB5 moves from 260010 to 260040 each at most a tenth of `t1on`'s (card ALD gate 4).
- **(D) The dual bar** (`A` against `ctl`): held-out user within 0.01 m in every hour 1 to 6; clock wire at most 1.10 times, all and tracked.
- **(T) The three-band bars of card R1D (B)** (`t1on` and `t1onA` against `ctl`): held-out user within 0.03 m in every hour 1 to 6; clock wire at most 1.10 times, all and tracked; band 1 and band 2 offset stations at most one more than the control.
- **(C) The 16 sites** (`scripts/clock_level/gcensus.py`, each run's own configuration as the user's, so the `t1on` users are three-band): the paired change of each site's mean hourly p50 over hours 2 to 6 and of hour 1 against the bed's dual control, better / worse at 1 cm, the median; every site worse by more than 1 cm is listed. Reported as measured; not a pass or fail on its own (never adopt on one instrument).
- **(S) The sim truth grades:** SZZ000SIM by hour from 00:30:15 (the trace's error is against truth) and the satellite clock against truth (`scripts/simnet_clock_truth_grade.py`, level and phase-visible rms by hour). The sim bed has no event at TOW 260040, no gaps and L5 on every satellite: it tests the algebra, not the field event.

Every bar is reported passed or failed; every receiver that gets worse is listed. A failure is reported as one; L5 stays SEQUENCED whatever the result, with the return plan stated.

## 3. Stage B: L5 at the user (measure only)

(a) A three-band and a two-band user on each stage A correction file at WTZZ00DEU (and the 16 sites where stage A's census already holds the three-band user): does it run, does GPS L5 enter the solution, what changes by hour. (b) The roof receiver (AsteRx-U, LEIAR25 NONE) on the recorded session of 2026-11-10 (`D:/GnssProcessorSupport/runs/rt_agu_2026-11-10/relay_cas/`): confirm that GPS L5 is dropped for want of a G05 calibration under `require_atx_coverage`, and state what a fallback from the G02 calibration would use. The smallest change for each is proposed, not built.
