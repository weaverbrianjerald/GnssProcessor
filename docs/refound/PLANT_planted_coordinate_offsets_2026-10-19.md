# PLANT: planted offsets in one station's held coordinate

**STATUS**

- **id:** PLANT
- **topic:** network-vehicle
- **status:** closed
- **summary:** CLOSED 2026-10-19, measurement only: the held coordinate of one sim station (S48000SIM, 579 km from the held-out consumer) planted wrong by 1 cm, 10 cm, 1 m, 10 m and 1 km, East and Up, against a same-exe control. The wire moves in proportion to the size up to 10 m, 28.5 mm of satellite-clock RMS per metre East and 6.2 mm per metre Up, and the consumer by up to 3.4 mm per metre; no tier acts below 1 km. The congruence screen names the station at 10 m from the first sweep and at 1 m only after hours; at 1 km Up the gross line-of-sight screen declares the station alone at epoch 1, at 1 km East it declares 47 other stations and the consumer ends 335 m off. Nothing built, nothing armed.
- **source:** the author's request of 2026-10-19 (plant known offsets at one station of the sim arms); [SCU](SCU_station_coordinates_unknowns_2026-09-22.md) sections 4 to 6 (the detector, the proposed release, gate G1); [SNX](SNX_sinex_station_coordinates_2026-10-19.md) section 4 (the measured offsets of the held coordinates)
- **knobs / posture:** no knob built or changed, no golden or ledger edited. One development arm sets `network.gross_los_station_tier` on (ledger row UNVALIDATED, default off). Posture: the sim golden `config/golden/golden_sim_refound.json` (60 stations, GPS + Galileo, `SZZ000SIM` held-out cold consumer), started at 00:30:15 (`network.start_offset_hours` 0.504167)
- **dates in the text:** 2026-10-19
- **hashes / run dirs:** exe `gnss_processor_plant.exe`, a copy of `build/x64-Release/gnss_processor.exe` taken at main `5e95c84` (file time 05:45), sha256 `3d6c679e8068a39879c582b4495e069c569a3706049826f96f6123e2267a288e`; base golden sha256 `81eb338fc36bff58100284543583cf170331aac01a6a8ad4ee2a0761b8ebef52` (main `3e4a53a`); arms `_rap_plant_{ctl,ctl2,e1cm,u1cm,e10cm,u10cm,e1m,u1m,e3m,u3m,e10m,u10m,e1km,u1km}_s2`, `_rap_plant_e1km_stier_s2`, `_rap_plant_{ctl,e10cm,u10cm,e1m,u1m}_s6`, census replays `pcs_<site>_{ctl,e10cm2,u10cm2,e1m2,u1m2,e10m2}` (8 sites on the 2 h wires), all under `D:/GnssProcessorSupport/runs_nom/`; instruments `scripts/plant_arm.py`, `scripts/plant_read.py`, `scripts/plant_census.py`; read outputs and logs under `D:/GnssProcessorSupport/gates/plant/`

**Outcome.** Question: what does a wrong held coordinate at one network station do to the emitted products and to the held-out consumer, and which screen catches it and names the station? Result on the sim golden: up to 10 m the wire moves in proportion to the size (28.5 mm of satellite-clock RMS per metre East, 6.2 per metre Up) and the consumer by up to 3.4 mm per metre; no tier acts below 1 km. The congruence screen names the station at 10 m from the first sweep and at 1 m East only from hour 3; at 1 km East the gross-LOS screen declares 47 other stations and the consumer ends 335 m off. Standing: card SCU's release needs a persistent-statistic trigger and a first-epoch gate (section 6). Nothing armed.

## 1. The question

A network station holds a configured coordinate. Card SCU found one such coordinate 1.6 m off its data (MIZU00JPN) and proposed constrained coordinate states with a release (its section 6, gate G1: errors from 0.05 to 1.5 m at the four stations nearest the sim consumer); card UPO had shown on the field what a wrong held height does (the wet delay takes about a quarter of it, the satellite clocks the rest, regionally). Neither card has a response curve: how the wire and the consumer move with the size of the error, from a size nobody would notice to one that breaks the solve, and which armed screen reacts at which size. The question of this card: a held coordinate is wrong by 1 cm, 10 cm, 1 m, 10 m or 1 km; what does each size do to the emitted products and to the held-out consumer, which screen catches it, and does it name the station? The 1 km row tests the gross-blunder end of the range.

## 2. The bed and the method

**Bed.** The sim golden `config/golden/golden_sim_refound.json` (60 stations, truth known, held-out consumer `SZZ000SIM`, GPS + Galileo, float vehicle), started at 00:30:15 as the golden's note prescribes. The golden at main `3e4a53a` pins `network.sparse_station_normals` (bit-identical to the dense products by its ledger row); the exe is a copy of the Release build made at main `5e95c84`, one commit earlier, which reads the same key, so exe and golden agree on the posture. Only that copy was run (`rapid_arm.py run`, at most three arms at a time, `RAPID_RUNS_ROOT=D:/GnssProcessorSupport/runs_nom`); cold start in every arm.

**The station.** S48000SIM (33.70 N, 17.14 W, height 818 m), a plain `network` station (the datum station is S30000SIM). It is the roster station nearest the consumer, 579 km away (the next is 2,425 km), so it sees the satellites the consumer sees and gives the largest consumer effect the roster can produce; a station farther away dilutes by distance. Its coverage is typical: 14 satellites per epoch at the median (roster median 14, quartiles 13.75 and 16), 6,512 code rows in the 2 h control against a roster median of 6,939, its RINEX file 95 % of the median size.

**The plant.** `network.stations[].position_ecef` of S48000SIM only. The RINEX files and the simulator's truth are unchanged, so the held coordinate is the true one plus the offset. Offsets are in the local East and Up of the WGS84 frame at the station (the estimator's own `enuToEcefMatrix`; sim stations have a 0/0/0 antenna eccentricity, so `network.station_marker_to_arp` moves nothing), positive sign. Sizes 0.01, 0.1, 1, 10 and 1,000 m; a 3 m pair brackets the 1 to 10 m gap. `scripts/plant_arm.py make` builds each arm through `rapid_arm.py make` and then edits the one array element that `rapid_arm` cannot set; it refuses to finish unless the arm differs from the golden only in that station's `position_ecef` (two components for East, three for Up) and the run paths (`product_output_path`, `max_duration_s`, `start_offset_hours`, and in the one tier arm the knob named). The structural diff is stored in each arm's `rapid.json`.

**Arms.** Control and a repeat control; East and Up at each size, 2 h each; 6 h for the control and for 0.1 m and 1 m East and Up (the reading moves at both sizes); a 3 m pair (East and Up) to bracket the gap between 1 and 10 m; one development arm, 1 km East with `network.gross_los_station_tier` on. Wall times of the 2 h arms were 277 to 2,783 s on a machine shared with other long runs (the control pair ran in the first wave, in 277 s).

**Read.** `scripts/plant_read.py` compares each arm with the same-exe control on the same epochs; hours are counted from the first emitted epoch (hour 1 is the start artifact and ignition, settled claims come from hour 2 on). Quantities: the satellite clock difference arm minus control as a per-epoch RMS over satellites after removing the per-epoch common mean (datum-invariant), the same for the phase biases (per constellation and signal) and the code biases; the consumer's 3D error against truth by hour and the per-epoch vector shift between arm and control; the screens at the planted station from `network_events.csv`, `network_congruence.csv`, `network_raim.csv`, `network_raim_attrib.csv`, `network_run_summary.csv` and `stdout.txt`; the station's own post-fit RMS, wet delay and receiver clock; an in-network consumer census (`scripts/plant_census.py`; section 3.5).

Two statistics of the congruence screen (`network.congruence_screen`, measure-only, armed in both goldens) recur. Its sweeps run every 10th epoch (every 5 minutes, `redundancy_census_stride` 10) over every station; `t` is the per-sweep statistic of a station's coordinate-offset test, reported (as an event) above 4; `geo(t)` is its run-level statistic, the geometric mean of the calibrated statistic over the sweeps divided by its null value, 1 for a station with nothing wrong. "Persistent over 4 from minute X" means every sweep from X to the end of the run is above 4 and at least three sweeps follow. The RAIM attribution (`network_raim_attrib.csv`) lists, per evaluation, the entities (STA, SAT, ARC rows) whose residual effect stands out, with a prominence (robust sigmas above the median of the entities of that kind, `RobustStats.hpp`) so that a spike can be attributed to a bad satellite, station or signal; a row "acted" when the attribution drove a refloat (none of the STA rows did in the arms below 1 km); `network_raim.csv` lists the (satellite, station) pairs whose normalised residual passes the RAIM bar (`spiked`) with the number of stations spiked on that satellite (the vote needs three, `min_sta`).

**Determinism.** The repeat control is byte-identical to the control in `products.txt`, `closed_loop_trace.csv`, `network_solution.csv`, `network_epoch_status.csv`, `network_events.csv`, `network_congruence.csv` and `network_pf_keyed.csv`, so every difference below is the plant. The 1 cm arms differ from the control by 0.3 mm (East) and 0.07 mm (Up) of satellite-clock RMS and by at most 0.2 mm in the consumer's median shift.

## 3. Results

### 3.1 The wire and the consumer, 2 h arms

Table A, 2 h arms (hour 2 unless stated; control p50 by hour in the header):

| size | dir | wire: satellite clock RMS, h1 / h2 (mm) | phase bias RMS h2 (mm) | code bias max (mm) | consumer 3D p50, h1 / h2 (mm), control 99.5 / 32.8 | consumer shift, median h2 (mm) | consumer mean shift E / N / U, h2 (mm) |
| --- | --- | --- | --- | --- | --- | --- | --- |
| 1cm | E | 0.333 / 0.289 | 0.549 | 0.0200 | 99.5 / 32.8 | 0 | 0.0264 / 0 / -0.0248 |
| 1cm | U | 0.0397 / 0.0660 | 0.0782 | 0.0180 | 99.5 / 32.8 | 0 | 0 / 0 / 0.00496 |
| 10cm | E | 3.31 / 2.85 | 5.49 | 0.0254 | 99.2 / 32.7 | 0.283 | 0.245 / -0.00413 / -0.217 |
| 10cm | U | 0.283 / 0.615 | 0.724 | 0.0276 | 99.5 / 32.9 | 0.1000 | -0.0215 / 0 / 0.0595 |
| 1m | E | 33.2 / 28.5 | 54.9 | 0.0359 | 94.0 / 30.8 | 3.40 | 2.68 / 0.0322 / -2.67 |
| 1m | U | 2.77 / 6.17 | 7.22 | 0.0408 | 99.9 / 33.3 | 0.608 | -0.104 / -0.00744 / 0.530 |
| 3m | E | 99.6 / 85.6 | 165 | 0.0573 | 107 / 31.9 | 9.04 | 7.61 / -0.0678 / -7.04 |
| 3m | U | 8.29 / 18.6 | 21.7 | 0.0474 | 101 / 32.3 | 0.806 | 0.440 / 0.00992 / 0.226 |
| 10m | E | 332 / 285 | 549 | 0.136 | 237 / 33.2 | 25.1 | 23.1 / 0.0182 / -19.7 |
| 10m | U | 27.6 / 61.6 | 72.3 | 0.0999 | 103 / 36.3 | 3.71 | 0.203 / 0.0636 / 3.43 |
| 1km | E | 83017 / 66966 | 93562 | 8.45 | 262561 / 335198 | 335208 | 264072 / 7384 / -189161 |
| 1km | U | 549 / 336 | 3034 | 1.11 | 141 / 30.0 | 5.60 | 2.63 / 1.55 / -0.670 |

- **Linear up to 10 m.** The satellite-clock RMS in hour 2 is 28.9, 28.5, 28.5, 28.5 mm per metre of East offset at 1 cm, 10 cm, 1 m and 10 m, and 6.6, 6.2, 6.2, 6.2 mm per metre of Up; the phase-bias RMS is 54.9 mm per metre East and 7.2 per metre Up. The code biases move by at most 0.14 mm (they are pinned to the broadcast group delay). East costs the wire four to eight times what Up does: the planted station absorbs Up in its receiver clock (0.99 of the offset) and wet delay (0.19), and nothing of the same kind exists for a horizontal error (section 3.3).
- **The contamination is regional.** On the satellites the station tracks (14 of 56) the clock RMS is 3.2 times that on the others at 1 m East (53.6 against 16.8 mm in hour 2) and 8 times at 1 m Up (13.4 against 1.65 mm).
- **The consumer 579 km away moves little.** The median per-epoch shift of its solution in hour 2 is 0.28, 3.4 and 25 mm for 0.1, 1 and 10 m East, and 0.1, 0.61 and 3.7 mm for Up; below 10 cm nothing exceeds 0.3 mm. The hour-1 ignition is the sensitive part: at 10 m East the consumer's p50 is 237 mm against 99 mm and the median shift 200 mm (largest 1.19 m), then in hour 2 the p50 is back at the control (33.2 against 32.8 mm) with the shift at 25 mm and a mean of +23 mm East. The 3D p50 by hour moves by 5.5 mm in hour 1 and 2.0 mm in hour 2 at most for an offset of 1 m (East: 94.0 against 99.5 mm and 30.8 against 32.8 mm), in the direction of an improvement.
- **1 km breaks the solve in one direction.** Up: the wire's clock RMS is 549 mm in hour 1 and 336 mm in hour 2, the consumer's median shift 82 mm in hour 1 and 5.6 mm in hour 2 (p50 30.0 against 32.8 mm), after screens acted (section 3.2). East: the clock RMS is 83 m and 67 m, the consumer's p50 262 m and 335 m, its mean error 264 m East and -189 m Up in hour 2.

### 3.2 Which screen reacts, and whether it names the station

Table B, 2 h arms:

| size | dir | congruence geo(t) at the station (largest of the other 59) | sweeps over t > 4 (of all) | persistent over 4 from (min) | CONGRUENCE events naming it, arm / control | attribution rows at it / next station, first with prominence over 10 (min) | RAIM spike rows at it, first (min) | gross-LOS declarations: at it / at others (stations) | its rows deweighted or absent h2 (%) | warning naming it |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| 1cm | E | 0.659 (1.40) | 2 of 25 | - | 2 / 2 | 1 / 13, - | 0, - | 0 / 0 (0) | 0 | 0 |
| 1cm | U | 0.657 (1.40) | 2 of 25 | - | 2 / 2 | 1 / 13, - | 0, - | 0 / 0 (0) | 0 | 0 |
| 10cm | E | 0.683 (1.40) | 2 of 25 | - | 2 / 2 | 1 / 13, - | 0, - | 0 / 0 (0) | 0 | 0 |
| 10cm | U | 0.651 (1.40) | 2 of 25 | - | 2 / 2 | 1 / 13, - | 0, - | 0 / 0 (0) | 0 | 0 |
| 1m | E | 2.28 (1.38) | 4 of 25 | - | 4 / 2 | 8 / 14, - | 0, - | 0 / 0 (0) | 0 | 0 |
| 1m | U | 1.11 (1.39) | 3 of 25 | - | 3 / 2 | 4 / 13, - | 0, - | 0 / 0 (0) | 0 | 0 |
| 3m | E | 11.8 (1.35) | 19 of 25 | 105 | 10 / 2 | 210 / 13, 26.0 | 0, - | 0 / 0 (0) | 0 | 0 |
| 3m | U | 3.98 (1.37) | 11 of 25 | - | 7 / 2 | 138 / 13, 45.5 | 0, - | 0 / 0 (0) | 0 | 0 |
| 10m | E | 112 (2.15) | 25 of 25 | 0 | 13 / 2 | 232 / 13, 6.5 | 168, 57.5 | 0 / 0 (0) | 0 | 0 |
| 10m | U | 27.1 (2.05) | 21 of 25 | 50 | 11 / 2 | 229 / 13, 8.5 | 81, 43.5 | 0 / 0 (0) | 0 | 0 |
| 1km | E | 1030 (212) | 25 of 25 | 0 | 13 / 2 | 206 / 228, 3.0 | 2615, 3.5 | 0 / 337 (47) | 10 | 0 |
| 1km | U | 2140 (1.24) | 25 of 25 | 0 | 13 / 2 | 91 / 14, 3.0 | 983, 4.0 | 17 / 0 (0) | 28 | 0 |

- **Nothing reacts below 1 m.** At 1 cm and 10 cm every event count and RAIM row equals the control's, the run-end counters differ by at most 3 (prefit-census row-cell exceedances, 76,689 in the control), and the congruence statistic at the station (geo(t) 0.65 to 0.68) is the control's. In every 2 h arm no station is dropped (`n_sta_pd_dropped` 0, 60 of 60 in the solve at the minimum), 241 of 241 epochs are emitted and none withheld, and no warning names the station. The control has 63 warning lines; so has every arm up to 1 m and the 10 m East arm, 64 the 10 m Up arm and 68 the 1 km East arm, the added lines being network-wide statements of the gross-LOS screen, the clock anchor and the estimability census.
- **1 m: a statistic, not an event.** At 1 m East the congruence statistic of the station is the largest of the roster (geo(t) 2.28 against 1.38 for the next) in the 2 h arm, below the event line in the median (t 1.50 in hour 1, 2.35 in hour 2); its first sweep above t = 4 (minute 65) is the control's own, and the sweeps stay above 4 persistently only from minute 160 in the 6 h arm (section 3.4). At 1 m Up it is not distinguishable in 2 h (rank 5 by median t in hour 2) and becomes persistent at minute 290. The station's own post-fit phase RMS is 1.7 times its control's at 1 m East (rank 1 of 60) and 1.3 times at 1 m Up (rank 21).
- **10 m: named, observed, not acted on.** The congruence screen is above t = 4 at every sweep at 10 m East (25 of 25, 13 events naming the station, 2 in the control) and at 21 of 25 sweeps at 10 m Up (persistent from minute 50), with a run statistic of 112 and 27 against at most 2.2 for the next station. The RAIM attribution table lists the station as the dominant station entity from minute 4.5 (232 rows against 13 for the next station, 1 in the control; prominence over 10 from minute 6.5). `network_raim.csv` gains 168 (East) and 81 (Up) spike rows at the station, all with one station spiked: the vote needs three (`raim_min_stations`), so no satellite is refloated; the within-epoch tier excludes no station, no epoch is withheld, the gross-LOS screen declares nothing, the measurement-admission census is silent and no event names the station in the prefit tier. A 10 m error is therefore carried through the solve with every response silent, and costs the wire 285 mm (East) and 62 mm (Up) of clock RMS in hour 2.
- **What the congruence screen does not do is measure.** Its hour-2 median offset at the station reads (East -0.4, North +1.3, Up +72) cm for +10 m East and (East -3.8, North +1.6, Up +52) cm for +10 m Up, and 0.1 to 0.3 cm of East for +1 m East in 2 h (1.5 cm over 6 h). It detects and cannot size, as card SCU section 4 found on the field: the carried float ambiguities and the station clock and wet delay absorb the offset, and the post-fit rows keep only its change with the geometry.
- **1 km Up: the cold bound acts, on the right station.** At the first epoch the gross-LOS cold-start bound (12 m, inside the first 120 emitted epochs) declares 13 lines of sight of S48000SIM (17 over the run, none at any other station: the screen's declarations name one station), erases their carried levels and folds their code rows at sigma x 100: 28 % of the station's rows are deweighted in hour 2. The station stays in the solve (60 of 60). The tier T4 attribution (617 arc events) names only this station, and the RAIM attribution lists it as the dominant station (91 rows against 14). The wire is still 336 mm off in hour 2.
- **1 km East: the screen stands down at the station and declares the others.** At the first epoch 14 of the station's 15 lines of sight are over the 12 m bound, which is more than the 50 % of the whole-station guard, and the screen declines to declare any of them (`gross_los_cold_standdown`, 120 events, then `gross_los_station_standdown`, 121 events from minute 60: every epoch). The station tier that would declare the station is off. With the station's rows at full weight the satellite clocks move to meet them, the other stations' lines of sight go over the bar, and 337 of them are declared at 47 other stations, 87 of them (at 32 stations) at the first epoch; the station itself is never declared. The wire carries 9,765 discontinuity marks and 77 withheld satellite-epochs against none; 681 measurement-admission channel-epochs exceed the 1,000 m bound; the congruence screen has the station first (geo(t) 1,030) with 18 other stations above 4 (the next at 212), and the RAIM table has 73,971 rows (2,615 at this station), 27,267 of them spiked and 67,092 refloated, with up to 12 stations spiked on one satellite. No warning names the station; its row in the congruence table, which lists every station, reads geo(t) 1,030.

### 3.3 What the planted station absorbs

Table C, 2 h arms:

| size | dir | wet delay change h2 (mm) | receiver clock change h2 (mm) | own post-fit code RMS, arm / control (mm), rank of 60 | own post-fit phase RMS, arm / control (mm), rank of 60 |
| --- | --- | --- | --- | --- | --- |
| 1cm | E | -1.07 | 3.43 | 994 / 993 (19) | 227 / 226 (52) |
| 1cm | U | -1.93 | 9.93 | 994 / 993 (19) | 226 / 226 (52) |
| 10cm | E | -10.7 | 34.3 | 992 / 993 (21) | 231 / 226 (50) |
| 10cm | U | -19.3 | 99.2 | 995 / 993 (18) | 226 / 226 (52) |
| 1m | E | -107 | 343 | 1069 / 993 (1) | 384 / 226 (1) |
| 1m | U | -193 | 992 | 1022 / 993 (1) | 292 / 226 (21) |
| 3m | E | -320 | 1028 | 1683 / 993 (1) | 915 / 226 (1) |
| 3m | U | -578 | 2977 | 1262 / 993 (1) | 678 / 226 (1) |
| 10m | E | -1067 | 3428 | 4724 / 993 (1) | 2948 / 226 (1) |
| 10m | U | -1928 | 9925 | 2684 / 993 (1) | 2210 / 226 (1) |
| 1km | E | -59746 | 266701 | 201289 / 993 (1) | 15854 / 226 (1) |
| 1km | U | -488290 | 1430657 | 4865 / 993 (1) | 7450 / 226 (1) |

- The receiver clock takes the Up offset almost entirely (0.99 of it at 10 cm, 1 m and 10 m) and the wet delay 0.19 to 0.20 (negative sign: a station held too high gets a smaller wet delay), against about a quarter in card UPO's field reading. A horizontal offset has no such sink: the clock takes 0.34 and the wet delay 0.11 of an East offset, and the remainder goes to the satellite clocks.
- The post-fit residuals of the station stay at the control's below 1 m (phase RMS within 2 % at 10 cm) and rise from 1 m; at 10 m the code RMS is 2.7 to 4.8 times and the phase RMS 9.8 to 13 times the control's, with the station first of 60 on both.

### 3.4 The 6 h arms: the statistic grows, the wire does not heal

Table B, 6 h arms (the sweeps are those of 6 h):

| size | dir | congruence geo(t) at the station (largest of the other 59) | sweeps over t > 4 (of all) | persistent over 4 from (min) | CONGRUENCE events naming it, arm / control | attribution rows at it / next station, first with prominence over 10 (min) | RAIM spike rows at it, first (min) | gross-LOS declarations: at it / at others (stations) | its rows deweighted or absent h2 (%) | warning naming it |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| 10cm | E | 1.02 (1.38) | 5 of 72 | - | 5 / 4 | 9 / 21, - | 0, - | 0 / 0 (0) | 0 | 0 |
| 10cm | U | 0.823 (1.38) | 4 of 72 | - | 4 / 4 | 10 / 21, - | 0, - | 0 / 0 (0) | 0 | 0 |
| 1m | E | 10.5 (1.45) | 46 of 72 | 160 | 26 / 4 | 337 / 22, 209.0 | 54, 329.5 | 0 / 0 (0) | 0 | 0 |
| 1m | U | 4.57 (1.43) | 39 of 72 | 290 | 22 / 4 | 295 / 20, 208.0 | 0, - | 0 / 0 (0) | 0 | 0 |

Table D, by hour, 6 h arms (consumer columns are arm / control; hours 1 and 2 reproduce the 2 h arms):

| size | dir | quantity | h1 | h2 | h3 | h4 | h5 | h6 |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| 10cm | E | consumer 3D p50, arm / control (mm) | 99.2 / 99.5 | 32.5 / 32.6 | 39.8 / 39.8 | 16.0 / 16.0 | 17.9 / 18.0 | 15.2 / 15.2 |
| 10cm | E | consumer shift, median (mm) | 2.02 | 0.283 | 0.100 | 0.141 | 0.173 | 0.100 |
| 10cm | E | satellite clock RMS (mm) | 3.31 | 2.85 | 2.66 | 3.20 | 3.98 | 4.00 |
| 10cm | E | phase bias RMS (mm) | 5.63 | 5.49 | 5.30 | 5.15 | 5.44 | 5.44 |
| 10cm | E | congruence median t at the station (rank of 60) | 0.216 (58) | 0.890 (30) | 0.855 (26) | 0.947 (26) | 0.706 (42) | 1.39 (5) |
| 10cm | E | RAIM attribution rows at the station | 1 | 0 | 2 | 1 | 1 | 4 |
| 10cm | E | own post-fit phase RMS, arm / control (mm) | 188 / 186 | 231 / 227 | 268 / 253 | 354 / 344 | 362 / 353 | 464 / 377 |
| 10cm | U | consumer 3D p50, arm / control (mm) | 99.5 / 99.5 | 32.7 / 32.6 | 39.8 / 39.8 | 16.1 / 16.0 | 17.9 / 18.0 | 15.2 / 15.2 |
| 10cm | U | consumer shift, median (mm) | 0.100 | 0.1000 | 0.1000 | 0 | 0 | 0.1000 |
| 10cm | U | satellite clock RMS (mm) | 0.283 | 0.613 | 0.728 | 0.805 | 0.982 | 0.953 |
| 10cm | U | phase bias RMS (mm) | 0.484 | 0.723 | 1.03 | 0.976 | 1.13 | 1.21 |
| 10cm | U | congruence median t at the station (rank of 60) | 0.179 (58) | 0.935 (24) | 0.653 (45) | 0.708 (45) | 0.757 (39) | 0.529 (51) |
| 10cm | U | RAIM attribution rows at the station | 1 | 0 | 2 | 2 | 1 | 4 |
| 10cm | U | own post-fit phase RMS, arm / control (mm) | 183 / 186 | 226 / 227 | 254 / 253 | 340 / 344 | 341 / 353 | 394 / 377 |
| 1m | E | consumer 3D p50, arm / control (mm) | 94.0 / 99.5 | 30.6 / 32.6 | 39.1 / 39.8 | 16.5 / 16.0 | 18.4 / 18.0 | 15.2 / 15.2 |
| 1m | E | consumer shift, median (mm) | 19.9 | 3.40 | 1.32 | 1.16 | 1.07 | 1.02 |
| 1m | E | satellite clock RMS (mm) | 33.2 | 28.6 | 26.6 | 31.3 | 38.7 | 38.9 |
| 1m | E | phase bias RMS (mm) | 56.4 | 54.9 | 53.1 | 51.5 | 54.5 | 54.6 |
| 1m | E | congruence median t at the station (rank of 60) | 1.50 (1) | 2.07 (1) | 4.86 (1) | 12.0 (1) | 19.8 (1) | 67.5 (1) |
| 1m | E | RAIM attribution rows at the station | 1 | 6 | 65 | 71 | 74 | 120 |
| 1m | E | own post-fit phase RMS, arm / control (mm) | 291 / 186 | 382 / 227 | 554 / 253 | 888 / 344 | 1259 / 353 | 2368 / 377 |
| 1m | U | consumer 3D p50, arm / control (mm) | 99.9 / 99.5 | 33.2 / 32.6 | 40.3 / 39.8 | 15.9 / 16.0 | 18.1 / 18.0 | 15.1 / 15.2 |
| 1m | U | consumer shift, median (mm) | 0.640 | 0.608 | 0.600 | 0.510 | 0.393 | 0.316 |
| 1m | U | satellite clock RMS (mm) | 2.77 | 6.14 | 7.28 | 8.00 | 9.84 | 9.58 |
| 1m | U | phase bias RMS (mm) | 4.83 | 7.20 | 10.3 | 9.50 | 11.2 | 11.9 |
| 1m | U | congruence median t at the station (rank of 60) | 0.257 (54) | 1.53 (3) | 1.20 (8) | 11.7 (1) | 9.31 (1) | 15.1 (1) |
| 1m | U | RAIM attribution rows at the station | 2 | 2 | 12 | 59 | 100 | 120 |
| 1m | U | own post-fit phase RMS, arm / control (mm) | 214 / 186 | 292 / 227 | 399 / 253 | 1025 / 344 | 1306 / 353 | 1639 / 377 |

- **10 cm: nothing in six hours.** The satellite-clock RMS is 2.7 to 4.0 mm (East) and 0.28 to 0.98 mm (Up) by hour; the consumer's p50 equals the control's to 0.1 mm in every hour from hour 2 (hour 4: 16.0 mm in both arms) and its median shift is 0.10 to 0.28 mm; the congruence statistic of the station is 1.02 (East) and 0.82 (Up) against 1.38 for the next station, with no persistent excursion; the RAIM attribution lists the station 0 to 4 times per hour, a clean station's rate. The tight prior of card SCU (0.01 to 0.02 m) covers this size at no cost.
- **1 m: the wire does not heal, and the detection statistic grows with time.** The satellite-clock RMS stays at 27 to 39 mm (East) and rises from 2.8 to 9.6 mm (Up) over the six hours, the phase-bias RMS at 51 to 56 mm and from 4.8 to 11.9 mm. The consumer settles instead: from hour 3 its p50 is within 0.5 mm of the control's (hours 4 to 6, East: 16.5, 18.4 and 15.2 against 16.0, 18.0 and 15.2 mm) and its median shift falls to 1.0 mm (East) and 0.3 mm (Up) by hour 6. The median congruence t at the station is 1.5, 2.1, 4.9, 12.0, 19.8 and 67.5 by hour East (first of 60 throughout) and 0.26, 1.5, 1.2, 11.7, 9.3 and 15.1 Up (first from hour 4); its sweeps stay above 4 persistently from minute 160 (East) and 290 (Up), and the run statistic is 10.5 and 4.6 against 1.45 for the next station. The RAIM attribution lists the station 65 times in hour 3 (East) and 59 in hour 4 (Up), then 120 per hour; the spike rows (one station spiked of the three the vote needs) begin at minute 329.5 for East (54 rows) and never for Up. The station's own post-fit phase RMS grows to 2.4 m (East) and 1.6 m (Up) in hour 6 against 0.38 m: the offset shows more as the satellites move, since only its change with the geometry survives in the residuals. In six hours no tier acts: the station is never declared, deweighted or dropped.
- **The sim reading of the field's blunder.** The field's MIZU00JPN, held 1.6 m off, read geo(t) 21.6 to 29.1 in 6 h runs (card SCU section 4); the sim at 1 m East reads 10.5. The congruence statistic is the same instrument at the same order of magnitude on both beds.

### 3.5 The census: how a user elsewhere sees the planted wire

The standing census (`scripts/clock_level/simcensus6h.py`: 16 in-network stations replayed as consumers for 6 h on a wire) was started on the 6 h wires and stopped by this card's author after three replays, because each took 800 to 1,400 s on the shared machine (about two hours for one wire). The reading below is the reduced form made by `scripts/plant_census.py`: 8 of the 16 sites (S53000SIM, S45000SIM, S37000SIM, S60000SIM, S17000SIM, S29000SIM, S01000SIM and S13000SIM, at 3,024 to 12,013 km from the planted station; the nearest census site is 3,024 km away, so none is as close as the held-out consumer), replayed as in-network consumers for 2 h from the start of each 2 h wire, every replay on the configuration of the control arm and only the wire changed, graded as the paired change of each site's 3D p50 by hour against the replay on the control wire (key `ctl`). The planted station is not a census site.

| wire (arm) | median 3D p50 over the 8 sites, hour 1 / hour 2 (m) | change in hour 1: median, better / worse, largest | change in hour 2: median, better / worse, largest |
| --- | --- | --- | --- |
| control | 0.098 / 0.033 | | |
| East 0.1 m | 0.098 / 0.033 | -0.1 mm, 5 / 3, -1.4 mm (S01000SIM) | 0.0 mm, 2 / 4, +0.2 mm (S13000SIM) |
| Up 0.1 m | 0.098 / 0.033 | 0.0 mm, 2 / 2, -0.2 mm (S13000SIM) | 0.0 mm, 1 / 1, +0.1 mm (S13000SIM) |
| East 1 m | 0.097 / 0.034 | -1.6 mm, 5 / 3, -4.9 mm (S13000SIM) | +0.4 mm, 3 / 5, +1.6 mm (S17000SIM, 8,198 km) |
| Up 1 m | 0.098 / 0.033 | 0.0 mm, 4 / 2, -1.0 mm (S01000SIM) | 0.0 mm, 4 / 4, +0.2 mm (S17000SIM) |
| East 10 m | 0.145 / 0.037 | +30.8 mm, 1 / 7, +101.1 mm (S37000SIM, 7,228 km) | -1.6 mm, 5 / 3, +15.3 mm (S37000SIM) |

- **0.1 m and 1 m: nothing reaches the census.** The median change is below 2 mm in hour 1 and below 0.5 mm in hour 2 at both sizes and both directions, of mixed sign (the largest single-site change is 4.9 mm in hour 1 and 1.6 mm in hour 2). The consumer 579 km away sees 3.4 mm at 1 m East; the sites 3,000 km and more away see none that the median can show.
- **10 m East: the ignition hour reaches every distance measured, the settled hour does not.** In hour 1 seven of eight sites are worse, by 30.8 mm at the median on a p50 of 98 mm (31 %) and by 101.1 mm at the largest, S37000SIM at 7,228 km; by site in order of distance (3,024 to 12,013 km) the hour-1 changes are +81, -26, +101, +40, +2, +22, +21 and +80 mm, with no decay over that range (the farthest site reads +79.5 mm); in hour 2 the median returns to the control (-1.6 mm, 5 better, 3 worse, largest +15.3 mm). A user that starts from a wire with a 10 m error at one station pays in the first hour at seven of eight sites between 3,000 and 12,000 km, and not after it.
- The reduced census is 8 of 16 sites and 2 h; the standing 16-site 6 h form was not completed. The sites are in the network (their data helped form the wire), so absolute errors are optimistic and only the paired change is read.

### 3.6 The 1 km East case with the station tier on, and the 3 m bracket

**3 m (2 h).** East: the wire's clock RMS is 85.6 mm in hour 2 (28.5 mm per metre again), the consumer's median shift 61 mm in hour 1 and 9.0 mm in hour 2 (p50 107 mm in hour 1 against 99.5); the congruence statistic is 11.8 against 1.35, over t = 4 at 19 of 25 sweeps and persistent from minute 105 with 10 events naming the station, and the attribution table lists it 210 times (prominence over 10 from minute 26); no tier acts. Up: clock RMS 18.6 mm (6.2 per metre), consumer shift 1.9 mm in hour 1 and 0.8 mm in hour 2, congruence statistic 3.98 against 1.37 (11 of 25 sweeps over 4, not persistent in 2 h), attribution 138 rows (prominence over 10 from minute 45.5). With the 1 m (East from minute 160 over six hours) and 10 m (minute 0) arms, the persistence time of the congruence statistic against size is 160, 105 and 0 minutes for East and 290, more than 120 and 50 for Up at 1, 3 and 10 m.

**1 km East with `network.gross_los_station_tier` on (2 h, a development arm, not an adoption).** The first hour is identical to the arm with the tier off (consumer p50 262 m, clock RMS 83 m): the station tier, like the ordinary screen, waits for the 120-epoch maturity horizon. At TOW 264660, the 121st emitted epoch, the screen declares 17 stations whole at once, S48000SIM among them with the highest fraction of its lines of sight over the bar (11 of 12, 0.92; the next 0.87), and the consumer's error falls from 288 m at that epoch to 46 m two epochs later, 7.4 m at emitted epoch 130 and 0.50 m at epoch 150 (counted from 0), 0.12 m at the end. Hour 2: clock RMS 7.8 m (67 m with the tier off), consumer p50 517 mm. Four of the 17 are released later (S32000SIM, S37000SIM, S45000SIM, S54000SIM); 1,932 station-epochs are spent declared. The existing tier is therefore a detector of the km class after the horizon, not before it, and it removes the culprit together with sixteen stations whose only fault was the poison; the products of hour 1 are not repaired.

## 4. The table by size and direction

| size | dir | caught by, and at which epoch | names the station | wire: satellite-clock RMS h2 (mm), phase-bias RMS h2 (mm) | consumer change: median shift h1 / h2 (mm), p50 change h1 / h2 (mm) |
| --- | --- | --- | --- | --- | --- |
| 1cm | E | nothing | no | 0.289, 0.549 | 0.224 / 0, +0.0 / +0.0 |
| 1cm | U | nothing | no | 0.0660, 0.0782 | 0 / 0, +0.0 / +0.0 |
| 10cm | E | nothing, in 2 h and in 6 h | no | 2.85, 5.49 | 2.02 / 0.283, -0.2 / -0.1 |
| 10cm | U | nothing, in 2 h and in 6 h | no | 0.615, 0.724 | 0.100 / 0.1000, +0.0 / +0.1 |
| 1m | E | no tier; the congruence statistic is first of 60 but is over t = 4 persistently only from minute 160 (6 h arm), the RAIM attribution lists the station from hour 3 | the run-end congruence table (geo(t) 2.28 in 2 h, 10.5 in 6 h); events 4 against 2 in 2 h; no warning | 28.5, 54.9 | 19.9 / 3.40, -5.5 / -2.0 |
| 1m | U | no tier; congruence persistent from minute 290 (6 h arm), attribution from hour 4 | the run-end table (1.11 in 2 h, 4.57 in 6 h); no warning | 6.17, 7.22 | 0.640 / 0.608, +0.5 / +0.5 |
| 3m | E | no tier; congruence persistent from minute 105, attribution (prominence over 10) from minute 26 | events 10 against 2, attribution rows; no warning | 85.6, 165 | 60.8 / 9.04, +7.4 / -0.9 |
| 3m | U | no tier; congruence over 4 at 11 of 25 sweeps, not persistent in 2 h; attribution from minute 45.5 | events 7 against 2, attribution rows; no warning | 18.6, 21.7 | 1.90 / 0.806, +1.4 / -0.5 |
| 10m | E | no tier acts; congruence persistent from the first sweep (minute 0), attribution from minute 6.5, RAIM spike rows (one station) from minute 57.5 | events 13 against 2, attribution rows (232 against 13 for the next station); no warning | 285, 549 | 200 / 25.1, +137.7 / +0.4 |
| 10m | U | no tier acts; congruence persistent from minute 50, attribution from minute 8.5, RAIM spike rows from minute 43.5 | events 11 against 2, attribution rows (229 against 13); no warning | 61.6, 72.3 | 6.14 / 3.71, +3.3 / +3.5 |
| 1km | E | gross-LOS stands the station down from minute 0 and declares 47 other stations (337 lines of sight); congruence lists it first (geo(t) 1,030) with 18 others above 4 | no, as the fault: the declarations are at other stations; its congruence row is the first of 19 flagged | 66966, 93562 | 262485 / 335208, +262462.0 / +335165.0 |
| 1km | U | gross-LOS cold bound at the first epoch (minute 0): 13 lines of sight at once, 17 in all | yes, alone: all 17 declarations and the 617 T4 attribution events name it; no warning | 336, 3034 | 82.3 / 5.60, +41.4 / -2.8 |

## 5. Where the network stops absorbing silently

Silent absorption holds to 1 m. Below it no screen differs from the control's, no event count differs and the prefit-census counter moves by at most 3 (1 cm and 10 cm) and 20 (1 m) of 76,689; the wire moves by at most 29 mm of clock RMS and the consumer by at most 3.4 mm per metre; the congruence statistic at the station is the control's (0.65 to 0.68 in 2 h and 0.82 to 1.02 in 6 h at 10 cm and below, against 1.40 and 1.38 for the next station). Reaction begins between 1 and 10 m and is observation only: at 1 m East the statistic ranks the station first but crosses the event line persistently only after 2.7 h (minute 160) and the attribution table lists it only from hour 3; at 3 m East the screen is persistent from minute 105 and the attribution rows exceed prominence 10 from minute 26; at 10 m it is persistent from the first sweep (East) or minute 50 (Up), the attribution table names the station from minute 6.5 and 8.5, and the RAIM vote sees spikes at the station alone (one of the three stations it needs) from minute 57.5 and 43.5. No response of any tier occurs up to 10 m: no declaration, refloat, withhold, deweight or drop, and no warning names the station. Action begins at the kilometre scale, where the offset pushes code residuals over the 12 m cold bound. Up: it acts on the right station at the first epoch (17 declarations, all at S48000SIM, 28 % of its rows deweighted, the consumer back at the control's level in hour 2), and the station stays in the solve with the wire 336 mm off. East: the km case does not behave as expected. Rows are not rejected (10 to 11 % are absent from the post-fit set, none deweighted), the station is not removed (60 of 60 in the solve, 93 to 105 m RMS code residuals at its rows), and the screen declares 337 lines of sight at 47 other stations instead; the network is poisoned from the first epoch (consumer 136 m at epoch 1, 262 m and 335 m by hour) and the wire does not recover in 2 h. The existing remedy for a whole-station fault, `network.gross_los_station_tier`, is off by default and, when on, acts only after the first 120 epochs (section 3.6).

## 6. For card SCU's design

Card SCU's static release list (K3, first increment) needs no detector and is safe at every size measured here; its tight prior (K2, 0.01 to 0.02 m) is right, since below 0.1 m nothing in the wire or the consumer moves (clock RMS 2.9 mm and consumer shift 0.3 mm at 0.1 m East, less for Up). What an automatic release must detect, from these arms: (1) a persistent station statistic, not an event. The congruence screen is the only instrument that names the station at 1 m to 10 m (geo(t) 112 and 27 at 10 m East and Up and 11.8 at 3 m East in 2 h, 10.5 and 4.6 at 1 m East and Up in 6 h, against at most 2.2 and 1.45 for the next station), but at 1 m its t exceeds 4 persistently only from minute 160 (East) and 290 (Up), so a release trigger at that size needs a window of hours, and below 1 m there is nothing to detect and nothing lost. (2) A state that estimates rather than reads: the screen's offset estimate reads 0.2 cm of East for 1 m planted East (1.5 cm over 6 h), 52 cm of Up for 10 m Up (and a spurious 72 cm of Up for 10 m East), so the size must come from a coordinate state estimated together with the ambiguities. (3) Horizontal components first: East costs the wire 28.5 mm of clock RMS per metre against 6.2 for Up, because the planted station's receiver clock (0.99 of the offset) and wet delay (0.19) absorb Up and nothing absorbs East; a release of Up alone would leave the larger part of the damage. (4) A decision at the first epochs for the km class, which is a gate and not a release: at 1 km East the wire is 83 m off in hour 1 and the consumer 262 m, the whole-station guard stands the station down, the screen declares 47 other stations, and the existing station tier (`network.gross_los_station_tier`, default off, UNVALIDATED) declares only after the 120-epoch maturity horizon, 17 stations at once, which then brings the consumer from 288 m to 0.5 m in 30 epochs. A coordinate gate before the first solve (for example the station's own code-only position against its held coordinate; not measured here) is the detector for that case. (5) An order of release, one station at a time with a re-evaluation: at 1 km East the congruence ranking has the planted station first (geo(t) 1,030) with 18 others above 4 (the next at 212), all lit by the poison, so releasing every flagged station together would release the victims with the culprit; the discriminator of card SCU section 4.1 (a station's own error against one its region shares) is needed here too. (6) Both edges as events (invariant 7): in these arms the station is neither dropped nor deweighted below 1 km, so a release at 10 m would be the first response of any kind to a fault that otherwise travels silently through the solve, costing the wire 285 mm (East) and 62 mm (Up) of clock RMS in hour 2; the release prior must exceed the largest error the roster can carry (10 m covers the metre class).

## 7. The coordinate source

Card SNX measured the held coordinates against the IGS weekly combined SINEX of week 2313 (its section 4): for the 54 compared roster stations the median 3D offset is 7.1 mm, four stations are 5.4 to 6.4 cm off (MAC100AUS 64.0, PARK00AUS 62.2, GCGO00USA 56.3, SOLO00SLB 54.5 mm) and MIZU00JPN, dropped from the roster by card SCU, is 1.68 m off. Read against the coefficients of section 3.1, an offset of 6 cm East costs the wire about 1.7 mm of satellite-clock RMS and the consumer about 0.2 mm, an offset of 5 mm nothing measurable: the cm-class differences between the SSC extrapolation and the weekly SINEX sit in the row of the table where no tier moves and no consumer notices, so for the producer a SINEX roster is a matter of correctness and of the truth of the grading, not of accuracy of the wire, as card SNX section 8.1 expected. The metre class is the row that matters: a 1.7 m error of the MIZU00JPN kind costs about 48 mm of clock RMS (East, linear coefficient) and 6 mm of consumer shift at the sim geometry, is seen by the congruence statistic only after about two hours (persistent from minute 160 at 1 m East and 105 at 3 m East in these arms; geo(t) 21.6 to 29.1 over 6 h on the field, card SCU section 4), and is acted on by no tier. The practical use of a second coordinate source, SINEX or the ITRF2020 SSC the roster holds now, is as an audit: a roster station whose two sources differ by more than 0.1 m is a station to look at before the run, which costs nothing at run time and catches the class that nothing in the producer catches below a kilometre. Nothing was downloaded for this card.

## 8. Limits

- One bed (the sim golden: 60 stations worldwide, GPS + Galileo, the simulator's coloured phase error and multipath), one station, one sign of offset, offsets along East and Up only (North was not planted), cold start, 2 h arms (6 h for four of them). The sensitivities are those of this roster and this station: a denser network dilutes the per-station share, a station nearer the consumer or tracking fewer satellites changes the consumer coefficient.
- The congruence statistic is calibrated by the network's own roster; on the field its baseline is wider (the roster median of geo(t) is 0.87 to 0.91 and a few stations stand at 3 to 8 for reasons that are not coordinates, card SCU section 4.1), so the sim separations here are an upper bound on what a threshold can claim on the field. The field's MIZU00JPN at 1.6 m read 21.6 to 29.1 (card SCU section 4), the same order as the sim's East 1 m at 6 h (10.5).
- The consumer is started cold with the wire; its settled sensitivity is read from hours 3 to 6 of the 6 h arms only.
- The runs shared the machine with other workflows; wall times are not timings.

## 9. Reproduction

```
python scripts/plant_arm.py make --base config/golden/golden_sim_refound.json --tag plant_e1m_s2 \
    --station S48000SIM --east 1 --hours 2 --set network.start_offset_hours=0.504167
python scripts/rapid_arm.py run --tags plant_ctl_s2 plant_e1m_s2 ... --jobs 3 --exe <the Release copy>
python scripts/plant_read.py table <control dir> <arm dir> ... --json out.json
python scripts/plant_read.py card out.json --hour 2
python scripts/plant_census.py make <key> <wire tag> --base <control arm.json> --hours 2 --sites s53,s45,s37,s60,s17,s29,s01,s13
python scripts/plant_census.py grade <control key> <key> ... --base <control arm.json> --hours 2 --sites s53,s45,s37,s60,s17,s29,s01,s13
```
