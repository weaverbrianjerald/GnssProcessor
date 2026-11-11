# L5B6: GPS L5 in the network over 6 h, with and without the freed arc's variance floor (continuation of card L5B)

**STATUS**

- **id:** L5B6
- **topic:** network-vehicle
- **status:** open
- **summary:** PRE-REGISTERED 2026-11-10. Continuation of card [L5B](L5B_band5_receiver_delay_2026-10-10.md) (that file stands at the 60 KB bar; its section 16 points here). Stage A: the four 2 h field runs of L5B section 15 repeated on the goldens of main `b6bbea7`, then 6 h on field DOY 129, field DOY 132 and the sim: dual control, dual plus `network.amb_float_drop_var_m2` 100, the three-band `t1on` keys, `t1on` plus the floor. Stage B (measure only): the three-band user on stage A's corrections and the roof receiver's band-5 drop. Nothing armed; run keys only; no C++ change. Stage A result (section 4): the 2 h result holds; at 6 h the floor keeps the event away on DOY 129, passes the dual bar on both days and the sim with the 16 sites neutral, and, corrected 2026-11-11 (section 5: the first grader's "tracked" clock column was a Galileo row), the three-band run with the floor passes bar (T) on DOY 129 at 2 h and 6 h and on the sim, and fails it on DOY 132 only (user +0.057 and +0.032 m in hours 1 and 2); the three-band users at the 16 sites lose 7 to 11 mm in hours 2 to 6. L5 SEQUENCED.
- **source:** card L5B sections 12 to 15 (the `t1on` keys, the event at TOW 260040, the floor's 2 h pass); card [SMX2](../record/refound/SMX2_scenario_matrix_current_golden_2026-11-09.md) sections 5.3, 5.6 and 6 (the 2 h grader, the floor's costs K1 and card SLV's); card [ALD](../record/refound/ALD_anchor_level_datum_2026-10-23.md) section 5 (gates 4 and 5); card [R1D](../record/refound/R1D_station_level_datum_2026-10-23.md) (the three-band bars (B)); the author's directive that L5 is never closed
- **knobs / posture:** the goldens of main `b6bbea7` (`config/golden/golden_field_refound.json`, `config/golden/golden_sim_refound.json`); the `t1on` keys (`processing.frequency_mode` triple, the GPS band-5 types C5Q, C5X, C5I, L5Q, L5X, L5I and their D and S types appended to `processing.signal_priorities.GPS`, `network.sat_gf_extra_band_free`, `network.estimate_rx_extra_band_code_delay`); the floor `network.amb_float_drop_var_m2` 100. DOY 132 from `scripts/mday_golden_config.py` (obs root `D:/GnssProcessorSupport/data_mday`). Sim runs with `network.start_offset_hours` 0.504167.
- **dates in the text:** 2026-11-10, 2026-11-11
- **hashes / run dirs:** branch `l5/six-hour-read` (docs only) from main `b6bbea7`; exe `gnss_processor_l5read.exe`, a renamed copy of `D:/GnssProcessorSupport/gates/exe_keep/int1110/gnss_processor_int1110b.exe` (sha256[:16] `28e4286ee0805093`); runs `D:/GnssProcessorSupport/runs_nom/_rap_l56_<arm>_<bed>` (arms `ctl`, `A`, `t1on`, `t1onA`; beds `f2` DOY 129 2 h, `f6` DOY 129 6 h, `g6` DOY 132 6 h, `s6` sim 6 h from 00:30:15)
- **gates (frozen at pre-registration):** section 2

**Outcome.** Question: does the floor's 2 h pass for three-band GPS hold over 6 h on two field days and the sim? The 2 h repeat on main `b6bbea7` holds. At 6 h the floor removes the DOY 129 event, halves the anchor decouples on DOY 132, and is neutral on the dual network and the 16 sites. With the floor the three-band network passes on DOY 129 and the sim and misses only the user in hours 1 and 2 on DOY 132 (corrected 2026-11-11, section 5: the 1.17 times first reported on DOY 129 was a Galileo row; GPS reads 0.56); three-band users at the 16 sites are 7 to 11 mm worse. The sim passes and barely exercises the floor. L5 SEQUENCED; stage B separates network and user.

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

## 4. 2026-11-10: stage A results

All 15 runs exit 0 with every epoch emitted (240 of 240 at 2 h, 720 of 720 at 6 h) and none withheld; the 136 replays of the 16 sites exit 0. Graders: `grade_l56.py` (card SMX2 section 5.6's grader over N hours) and `offs_names.py` (the offset stations by name), session scratchpad folder `l56/`; the 16 sites `scripts/clock_level/gcensus.py`, keys `l56c_<arm>_<bed>` (runs `_rap_l56c_<arm>_<bed>_<site>`); sim clock truth `scripts/simnet_clock_truth_grade.py --fixture runs/simnet60_quiet_col_24h`. Wall times are not read (two other sessions shared the machine).

### 4.1 The 2 h repeat on main `b6bbea7` (`f2`)

| run | user, hours 1 and 2 (m) | GPS clock wire, all / tracked (m) | GPS band 1 / 2 / 5 offsets, stations (median \|mean\|, m) | decouples (at 260040) | MAJU00MHL, 260010 to 260040: GPS_DCB / GPS_IFB5 (m) |
| --- | --- | --- | --- | --- | --- |
| `_rap_l56_ctl_f2` | 0.0585 0.0126 | 0.0349 / 0.0087 | 1 (0.014) / 0 (0.012) / - | 3 (2) | -0.17 / - |
| `_rap_l56_A_f2` | 0.0555 0.0127 | 0.0362 / 0.0081 | 1 (0.014) / 0 (0.013) / - | 0 (0) | -0.09 / - |
| `_rap_l56_t1on_f2` | 0.0762 0.0393 | 0.0401 / 0.0080 | 2 (0.023) / 2 (0.016) / 3 (0.029) | 18 (15) | -3.84 / +9.98 |
| `_rap_l56_t1onA_f2` | 0.0794 0.0391 | 0.0171 / 0.0084 | 1 (0.017) / 0 (0.015) / 3 (0.024) | 0 (0) | -0.10 / +0.26 |

Bars: (D) `A` PASS (user -0.0030 and +0.0001 m, clock wire 1.04 and 0.93 times); (T) `t1on` FAIL (clock wire 1.15 times all, band 1 / 2 +1 / +2 stations), `t1onA` PASS (user +0.0209 and +0.0265 m, clock wire 0.49 and 0.97 times, offsets +0 / +0); event: `t1onA` 0 decouples at 260040, MAJU00MHL move ratios 0.025 (DCB) and 0.026 (IFB5) of `t1on`'s (bars 3 and 0.10). **(H2) holds:** the 2 h result of card L5B section 15 stands on today's goldens, within a few millimetres of every number.

### 4.2 Field DOY 129, 6 h (`f6`; control `_rap_int1110b_f6`)

| run | user by hour 1 to 6 (m) | clock wire, all / tracked (m) | band 1 / 2 / 5 offsets, stations | decouples (at 260040) |
| --- | --- | --- | --- | --- |
| `_rap_int1110b_f6` | 0.0585 0.0126 0.0205 0.0117 0.0171 0.0132 | 0.0568 / 0.0103 | 0 / 0 / - | 59 (2) |
| `_rap_l56_A_f6` | 0.0555 0.0127 0.0187 0.0113 0.0172 0.0142 | 0.0570 / 0.0098 | 0 / 0 / - | 55 (0) |
| `_rap_l56_t1on_f6` | 0.0762 0.0393 0.0308 0.0141 0.0232 0.0188 | 0.1172 / 0.0120 | 5 / 2 / 3 | 74 (15) |
| `_rap_l56_t1onA_f6` | 0.0794 0.0391 0.0331 0.0142 0.0222 0.0176 | 0.0529 / 0.0120 | 1 / 0 / 2 | 55 (0) |

Bars: (D) `A` PASS (user within 0.0030 m in every hour, clock wire 1.00 and 0.95 times). (T) `t1on` FAIL (clock wire 2.06 and 1.17 times, band 1 / 2 +5 / +2 stations: band 1 CAS100ATA, GAMB00PYF, GCGO00USA, LCK400IND -0.648 m, MAJU00MHL +0.308 m; band 2 LCK400IND, MAJU00MHL). `t1onA` FAILS one clause: tracked clock wire 1.17 times (bar 1.10); user within 0.0265 m in every hour (bar 0.03), clock wire all 0.93 times, band 1 / 2 +1 / +0 (NIUM00NIU -0.103 m on band 1, also above 0.10 m on DOY 132's dual control). The tracked clock wire is 0.0120 m on both three-band runs: it comes with band 5, not with the floor.

### 4.3 Field DOY 132, 6 h (`g6`; 56 stations, MAC100AUS and LCK400IND have no RINEX that day)

| run | user by hour 1 to 6 (m) | clock wire, all / tracked (m) | band 1 / 2 / 5 offsets, stations | decouples |
| --- | --- | --- | --- | --- |
| `_rap_l56_ctl_g6` | 0.0711 0.0161 0.0152 0.0181 0.0198 0.0086 | 0.0502 / 0.0125 | 1 / 0 / - | 103 |
| `_rap_l56_A_g6` | 0.0616 0.0185 0.0114 0.0115 0.0155 0.0080 | 0.0469 / 0.0114 | 1 / 0 / - | 56 |
| `_rap_l56_t1on_g6` | 0.1424 0.0457 0.0161 0.0239 0.0226 0.0234 | 0.0531 / 0.0133 | 1 / 0 / 1 | 134 |
| `_rap_l56_t1onA_g6` | 0.1282 0.0478 0.0193 0.0143 0.0190 0.0226 | 0.0492 / 0.0118 | 1 / 0 / 0 | 56 |

Bars: (D) `A` PASS (user -0.0095 to +0.0024 m by hour, clock wire 0.93 and 0.91 times). (T) `t1on` FAIL (user +0.0713 m in hour 1; clock wire 1.06 and 1.06 times, offsets +0 / +0). `t1onA` FAILS the user clause: +0.0571 m in hour 1 and +0.0316 m in hour 2 (bar 0.03); hours 3 to 6 within 0.0140 m; clock wire 0.98 and 0.94 times, offsets +0 / +0. The band-1 station on every DOY 132 run is NIUM00NIU.

### 4.4 Sim, 6 h from 00:30:15 (`s6`)

| run | SZZ000SIM by hour 1 to 6 (m) | clock wire, all / tracked (m) | satellite clock against truth, level rms by hour (m) | per-satellite mean rms, hours 2 to 6 (m) |
| --- | --- | --- | --- | --- |
| `_rap_l56_ctl_s6` | 0.0994 0.0320 0.0396 0.0161 0.0183 0.0149 | 0.0177 / 0.0062 | 0.0808 0.0283 0.0219 0.0193 0.0178 0.0166 | 0.0177 |
| `_rap_l56_A_s6` | equal to the control at 4 decimals | 0.0177 / 0.0062 | equal to the control | 0.0177 |
| `_rap_l56_t1on_s6` | 0.0989 0.0360 0.0435 0.0156 0.0176 0.0163 | 0.0180 / 0.0062 | 0.0806 0.0284 0.0221 0.0201 0.0182 0.0167 | 0.0181 |
| `_rap_l56_t1onA_s6` | equal to `t1on` at 4 decimals | 0.0180 / 0.0062 | equal to `t1on` | 0.0181 |

Bars: (D) PASS; (T) PASS for both (user within 0.0040 m in every hour, clock wire 1.02 and 1.00 times, offsets 0 / 0). The products of `A` and the control first differ at line 75 097, those of `t1onA` and `t1on` at line 89 165: on this fixture (no gaps, no real slips) the floor is close to inert.

### 4.5 The 16 sites (C)

Paired change of each site's mean hourly p50 over hours 2 to 6, against the day's dual control. Every user of a three-band key is three-band (the run's own configuration), so these numbers mix the network's share and the user's; stage B separates them.

| key | 16-site median by hour (m) | hours 2 to 6: median, better / worse at 1 cm | hour 1: median, better / worse | sites worse by more than 1 cm, hours 2 to 6 (m) |
| --- | --- | --- | --- | --- |
| `l56c_ctl_f6` | 0.132 0.045 0.033 0.028 0.031 0.027 | | | |
| `l56c_A_f6` | 0.125 0.043 0.033 0.028 0.032 0.027 | +0.0000, 0 / 0 | -0.0023, 3 / 0 | none |
| `l56c_t1on_f6` | 0.136 0.058 0.040 0.036 0.035 0.043 | +0.0082, 1 / 5 | -0.0032, 7 / 4 | CHTI +0.023, SCRZ +0.037, STPM +0.013, KOKB +0.044, BAKE +0.015 |
| `l56c_t1onA_f6` | 0.137 0.059 0.042 0.038 0.034 0.043 | +0.0072, 1 / 7 | -0.0024, 7 / 6 | CHTI +0.022, SCRZ +0.096, QUIN +0.011, STPM +0.013, CEDU +0.017, KOKB +0.046, BAKE +0.014 |
| `l56c_ctl_g6` | 0.103 0.056 0.045 0.028 0.036 0.028 | | | |
| `l56c_A_g6` | 0.100 0.060 0.046 0.028 0.036 0.027 | -0.0009, 1 / 0 | -0.0009, 1 / 2 | none |
| `l56c_t1on_g6` | 0.095 0.066 0.047 0.049 0.045 0.043 | +0.0109, 1 / 9 | -0.0020, 7 / 7 | SCRZ +0.107, ANMG +0.011, QUIN +0.017, HARB +0.013, POHN +0.015, BADG +0.014, CEDU +0.012, ARHT +0.016, BAKE +0.011 |
| `l56c_t1onA_g6` | 0.090 0.067 0.048 0.049 0.046 0.046 | +0.0098, 1 / 8 | -0.0070, 8 / 7 | SCRZ +0.129, ANMG +0.016, QUIN +0.016, POHN +0.015, BADG +0.013, KOKB +0.011, ARHT +0.014, BAKE +0.012 |

The one site better by more than 1 cm in each three-band key: POHN (-0.015 and -0.016 m) on DOY 129, UTQI (-0.016 and -0.014 m) on DOY 132.

### 4.6 Reading (stage A)

- **For the floor.** At 6 h as at 2 h it removes the three-band event at TOW 260040 (0 decouples; MAJU00MHL's move 3 % of the run without it). On DOY 129 it brings the three-band clock wire from 2.06 to 0.93 times the control and the band 1 / 2 offset stations from +5 / +2 to +1 / +0. It halves the clock-anchor decouples on DOY 132 (dual 103 to 56, three-band 134 to 56). It passes the dual bar on both field days and the sim, with the 16 sites neutral on both days (0 worse).
- **Against the floor, or not answered here.** It does not make the three-band network pass at 6 h: on DOY 129 the tracked clock wire stays at 1.17 times (equal with or without it); on DOY 132 the three-band user misses by 0.027 and 0.002 m in hours 1 and 2. It does not change the 16-site cost of the three-band keys (median +0.0072 against +0.0082 m on DOY 129, +0.0098 against +0.0109 m on DOY 132). The sim does not exercise it. Card SMX2's K1 (the guard's miss of a 3 m clock step on E34) and card SLV's open failures were not tested here and stand.
- **GPS L5 at 6 h.** With the floor, the three-band network is stable on two field days and the sim (no event, no offset growth, clock wire within the bar on all satellites). The remaining field cost sits at the three-band users (+7 to +10 mm at the 16 sites, SCRZ +0.10 to +0.13 m), in the tracked clock wire on DOY 129, and in hours 1 and 2 on DOY 132. Card L5B section 7 put the three-band user's cost on the Block IIF L5 phase; whether that is still the share is stage B's question. L5 SEQUENCED, not yet: the return plan is stage B (separate the network's and the user's share; then the user's IIF L5 phase and the roof receiver's band-5 drop).

## 5. 2026-11-11: correction to section 4, the "tracked" clock column

An independent re-check of section 4 (another session, from the run folders) found that every "tracked" clock value in sections 4.1 to 4.4 is a Galileo value. The defect was verified here before this section was written. `grade_l56.py`'s `wire()` set its section to "tracked" at the first "consumer-TRACKED" line of `ftc_step_attrib.py --r3 --systems G,E`, never reset it, and kept the last "hours 2-6 e_clk" row per section. That tool prints eight such rows (GPS, then Galileo; each: all, tracked, all de-commoned, tracked de-commoned), so the "all" column was the GPS all-satellite row (correct) and the "tracked" column was the Galileo tracked de-commoned row. The grader now keys each row on its `[system X]` block and its own section header (scratchpad `l56/grade_l56.py`; the first version kept as `grade_l56_v1_defective.py`). The user, the all-satellite clock wire, the offsets, the decouples, the MAJU00MHL ratios, the sim truth grades and the 16-site table were reproduced by the re-check and are unchanged. Sections 4.1 to 4.6 stay as written; this section supersedes their tracked column and the verdicts built on it.

### 5.1 The GPS rows, hours 2 to 6 (m)

All / tracked are the rows the frozen bar (T) names; the de-commoned rows (DIFFMEDIAN) are the datum-invariant ones, reported beside them. Galileo tracked de-commoned is the value section 4 printed as "tracked".

| run | GPS all | GPS tracked | GPS all, de-commoned | GPS tracked, de-commoned | Galileo tracked, de-commoned |
| --- | --- | --- | --- | --- | --- |
| `_rap_l56_ctl_f2` | 0.0349 | 0.0305 | 0.0133 | 0.0086 | 0.0087 |
| `_rap_l56_A_f2` | 0.0362 | 0.0317 | 0.0132 | 0.0084 | 0.0081 |
| `_rap_l56_t1on_f2` | 0.0401 | 0.0276 | 0.0334 | 0.0266 | 0.0080 |
| `_rap_l56_t1onA_f2` | 0.0171 | 0.0143 | 0.0141 | 0.0090 | 0.0084 |
| `_rap_int1110b_f6` | 0.0568 | 0.0413 | 0.0295 | 0.0177 | 0.0103 |
| `_rap_l56_A_f6` | 0.0570 | 0.0408 | 0.0290 | 0.0185 | 0.0098 |
| `_rap_l56_t1on_f6` | 0.1172 | 0.0367 | 0.0822 | 0.0316 | 0.0120 |
| `_rap_l56_t1onA_f6` | 0.0529 | 0.0231 | 0.0359 | 0.0185 | 0.0120 |
| `_rap_l56_ctl_g6` | 0.0502 | 0.0375 | 0.0370 | 0.0200 | 0.0125 |
| `_rap_l56_A_g6` | 0.0469 | 0.0354 | 0.0367 | 0.0162 | 0.0114 |
| `_rap_l56_t1on_g6` | 0.0531 | 0.0371 | 0.0493 | 0.0354 | 0.0133 |
| `_rap_l56_t1onA_g6` | 0.0492 | 0.0381 | 0.0402 | 0.0210 | 0.0118 |
| `_rap_l56_ctl_s6` | 0.0177 | 0.0105 | 0.0100 | 0.0084 | 0.0062 |
| `_rap_l56_A_s6` | 0.0177 | 0.0105 | 0.0100 | 0.0085 | 0.0062 |
| `_rap_l56_t1on_s6` | 0.0180 | 0.0104 | 0.0097 | 0.0085 | 0.0062 |
| `_rap_l56_t1onA_s6` | 0.0180 | 0.0104 | 0.0097 | 0.0084 | 0.0062 |

### 5.2 Corrected verdicts

| bed | `A` (D) | `t1on` (T) | `t1onA` (T) | `t1onA` against the control, de-commoned all / tracked (reported) |
| --- | --- | --- | --- | --- |
| `f2` | PASS (1.04 / 1.04) | FAIL (all 1.15; offsets +1 / +2) | PASS (0.49 / 0.47) | 1.06 / 1.05 |
| `f6` | PASS (1.00 / 0.99) | FAIL (all 2.06; offsets +5 / +2) | **PASS** (0.93 / 0.56; user within 0.0265 m; offsets +1 / +0) | 1.22 / 1.05 |
| `g6` | PASS (0.93 / 0.94) | FAIL (user +0.0713 m in hour 1) | FAIL (user +0.0571 and +0.0316 m in hours 1 and 2; clock 0.98 / 1.02) | 1.09 / 1.05 |
| `s6` | PASS (1.00 / 1.00) | PASS (1.02 / 0.99) | PASS (1.02 / 0.99) | 0.97 / 1.00 |

What changes: `t1onA_f6` passes (T) as the bar is written (section 4.2 said it failed one clause at 1.17 times). "The three-band runs fail one clause on each real day" (Outcome, STATUS summary, section 4.6) holds for DOY 132 only. "The tracked clock wire is equal with or without the floor, it comes with band 5" (sections 4.2 and 4.6) holds for the Galileo tracked de-commoned value (0.0120 m on both three-band runs against 0.0103 m), not for GPS: the floor lowers the GPS tracked value from 0.0367 to 0.0231 m. The de-commoned GPS all-satellite row of `t1onA_f6` is 1.22 times the control's; this is not a clause of bar (T) and is reported so it is visible.

### 5.3 The same fault in card SMX2 section 5.6 (reported, not edited)

The four runs of card SMX2 section 5.6 were re-graded with the corrected parser. Its "tracked" column (0.0089 / 0.0087 / 0.0088 / 0.0094) equals the Galileo tracked de-commoned row exactly. The GPS rows, hours 2 to 6 (all / tracked / all de-commoned / tracked de-commoned, m):

| run | GPS | Galileo tracked, de-commoned |
| --- | --- | --- |
| `_rap_smx2_d_ctl_f2` | 0.0372 / 0.0314 / 0.0135 / 0.0099 | 0.0089 |
| `_rap_smx2_d_A_f2` | 0.0382 / 0.0323 / 0.0132 / 0.0095 | 0.0087 |
| `_rap_smx2_d_t1on_f2` | 0.0435 / 0.0273 / 0.0337 / 0.0269 | 0.0088 |
| `_rap_smx2_d_t1onA_f2` | 0.0166 / 0.0147 / 0.0138 / 0.0082 | 0.0094 |

The GPS tracked de-commoned value of `t1on` is 2.7 times the control's, a jump the printed column could not show. Card SMX2, card L5B section 15 and card ALD are left as written; a dated note there is the author's decision.

### 5.4 Smaller points, verified

- The floor on two bands leaves the 16 sites neutral in hours 2 to 6 only. In hour 1 of DOY 132, `A` has STPM +0.034 m and HARB +0.010 m worse and DAV1 0.034 m better (section 4.5's counts 1 / 2).
- The floor on three bands moves single sites: SCRZ +0.059 m (DOY 129) and +0.022 m (DOY 132) in hours 2 to 6; in hour 1 of DOY 129, SCRZ +0.366 m, KOKB -0.098 m, DAV1 -0.051 m. "It does not change the 16-site cost" (section 4.6) holds for the median, not for SCRZ. Section 6 shows SCRZ's three-band loss is the user's.
- No run folder records the executable's path or hash (`rapid.json` has none; the startup manifest carries only `build.compiled` "Nov 10 2026 08:26:53", equal in every run). The hash was recorded after the runs, from the file still in place, in `D:/GnssProcessorSupport/runs_nom/_l56_exe_sha256.txt`: sha256 `28e4286ee0805093b116fa229bfcbf75ded71108be2597fa2472f6d9c232af88`, equal to its source copy `gates/exe_keep/int1110/gnss_processor_int1110b.exe`.

### 5.5 Note on card L5B section 15

Card L5B section 15's result paragraph (2026-11-09) quotes card SMX2 section 5.6's ratios: the dual bar "1.03 (all) and 0.98 (tracked)" and the floor arm "clock wire 0.45 and 1.06 times". Its tracked values are the Galileo tracked de-commoned row (section 5.3 above). With the GPS tracked row the ratios read 1.03 / 1.03 (dual bar) and 0.45 / 0.47 (the floor arm); `t1on` reads 1.17 / 0.87. Every verdict of that paragraph stands (dual bar pass, the floor arm pass, `t1on` fail). The note sits here because card L5B is at the size bar; its section 16 points to this card. Card SMX2 carries the same note as its section 7 (author's request, 2026-11-11).
