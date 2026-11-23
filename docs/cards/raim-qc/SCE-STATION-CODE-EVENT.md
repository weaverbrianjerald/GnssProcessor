# SCE: one receiver's free-running code loop, and the station that stands down for 77 minutes

**STATUS**

- **id:** SCE
- **topic:** raim-qc
- **status:** open
- **summary:** Pre-registered 2026-11-23 by the managing session's order (in the author's absence). On 2024 day 129 the
  receiver of BSHM00ISR (SEPT POLARX5 5.5.0) lets G22's C1C code loop run free for three and a half minutes
  (02:47:30 to 02:51:00, single band, short of the range by 27 m growing to 1,340 m, about 7 m/s). The network takes
  three of those rows before the gross line-of-sight screen withholds the line of sight; the line of sight comes back
  with a second jump; with the CODE bias file read, the station's clock then stays 10 to 41 m off its trend and the
  screen's own declarations, with the erase response, keep the station from recovering until 04:08 (the marks of card
  GLM). Stage A0 measures from existing runs only: no build, no gate run.
- **dates:** 2026-11-23 (pre-registered, before any reading of stage A0); 2026-11-23 (stage A0 results, section 5).
- **knobs / posture:** none changed. The reference files of 2026-11-22 as run in card BRP: `runs_nom/_rap_brp_f129_c0_6`
  (with the .BIA) and `runs_nom/_rap_brp_f129_c0nb_6` (without); for item 6 the reference runs of 2024 days 129, 130,
  132 and 2026 days 143 to 147.
- **code:** none (branch `qc/station-code-event-2026-11-23` from main `33c5fef` holds the card).
- **run dirs:** none new. Tools and reads: `D:\GnssProcessorSupport\gates\station_code_event\` (order
  `manager_ORDER.md`, report `REPORT_0.md`); scripts under `D:\GnssProcessorSupport\runs_nom\_irdu_grades\scripts_s20\`.
- **source:** the managing session's order of 2026-11-23 04:13 (`gates\station_code_event\manager_ORDER.md`); card
  BRP's trace of 2026-11-23 (section "day 129: why BSHM00ISR's GPS lines of sight are declared longer with the .BIA
  read"); the managing session's check of G22's code against its range rate
  (`gates\broadcast_reference\manager_check\manager_bshm_g22_0245_0255.txt`); standing invariants 1 and 7 of
  `CLAUDE.md`; card GLM (branch `net/gross-los-emit-marks-2026-11-22`).

**Outcome.** Stage A0 (section 5, report `gates/station_code_event/REPORT_0.md`): the admission's 1,000 m bound let three drifting rows in (514, 730, 945 m from the station's median) while the screen's station stand-down held every declaration; the re-entry vet that exists default off (`network.adm_revet`, 200 m) would have refused the first. The second jump is G22's L1C level carried from the drifting epochs (-1,522 m) and folded back at the admission's heal (invariant 1). With the bias file the clock then stays about 40 m off for 74 minutes under the screen's own declarations (invariant 7). Only BSHM00ISR does this on the reference days. Candidates described, not built.

> Results are appended TO THIS FILE, below the text.

---

## 1. Question

Why does one drifting code observation on one satellite at one station reach the solution, and why does the station
then stay off for 77 minutes? How often does that happen on the reference days, and what part does the CODE bias
file play?

## 2. The finding that opens the card

- At BSHM00ISR, G22's C1W and C2W are present to 02:47:00; at 02:47:30 only C1C is left; at 02:48:00 the satellite
  has no record; from 02:48:30 to 02:51:00 C1C alone; from 02:51:30 all three are back.
- Against the range rate before and after (11,497 m per 30 s at 02:47:00 rising 18.5 m per epoch to 11,682 m at
  02:52:00) the C1C of those epochs is short by about 27 m at 02:47:30, 500 m at 02:48:30, 720 m at 02:49:00, 930 m at
  02:49:30, 1,140 m at 02:50:00 and 1,340 m at 02:50:30, and back within about 25 m at 02:51:00.
- The network took three of these rows (residuals -145, -296, -375 m, prefit -722 m) before the screen withheld the
  line of sight at 02:50:00. With the bias file read, the station's clock was 9.9 m off its trend at 02:50:00 and 41 m
  off after 02:53:30; its other GPS lines of sight read 10 m and more off, were declared and erased, and the
  declarations held to 04:08. Without the file the clock stayed within 4 m and two satellites stayed declared.

## 3. Stage A0: readings from existing runs (fixed before they are made)

1. **Timeline**, 02:47:00 to 02:54:30 for G22 and on to 04:08 for the station, per epoch, both runs side by side:
   which of G22's rows entered the solve, with what prefit and weight; what each check saw and did (prefit census,
   RAIM within the epoch and its attribution, the screen's statistic and its count of epochs in a row, the cold bound,
   the station tier); the station's clock and receiver code-bias columns against their trend, with sigmas; the number
   of the station's code rows left in the solve.
2. **The rule that waited**: why rows of -500 to -900 m entered three times; the rule by file and line, and what it
   needed to act at the first epoch.
3. **The return**: when G22 was released or seeded again after 02:50:00, from which epoch's code, with what value
   against the code of 02:51:30 onward, and what makes the +118 / +184 m at 02:53:00.
4. **The hold**: why it lasts 77 minutes and why it ends at 04:08 (code rows left per epoch, the clock's sigma, the
   release that ends it).
5. **The observation file**: signal strength and loss-of-lock flags of G22's C1C and L1C from 02:47:30 to 02:51:00, and
   whether the engine reads any of them there.
6. **How often**, in the reference runs of 2024 days 129 (6 h and 24 h), 130, 132 and 2026 days 143 to 147: every code
   row with a prefit beyond 50 m that entered a solve, with what followed; every episode in which three or more lines
   of sight of one station are declared within two epochs or the station tier fires (station, time, duration, the
   first row that was off, the clock's largest departure from its trend).
7. **The bias file's part**: the station's receiver code-bias columns and sigmas with and without the file at
   BSHM00ISR and at one ordinary station, and which difference lets the clock go; settled by the code and the traces
   if they settle it, otherwise one pair of 6 h arms is named and not run.

## 4. Candidates (described in the report, not built)

(a) A bound on a code row's prefit that acts at once, its value from item 6. (b) A test of a code row against its own
carrier or the satellite's range rate. (c) A response at the level of the station: many lines of sight of one station
off by a common amount is the station's clock or code-bias block, levelled again as one declared event. (d) A
returning line of sight seeded only from epochs that passed the checks. Each with its place, the event at both edges
with counters, and the simulator test (a code ramp of about 7 m/s on one satellite at one station for three and a
half minutes, single band, as a new injector kind beside `code_step` of the GLM branch).

---
## 5. 2026-11-23, stage A0 results

The full report with its tables: `D:/GnssProcessorSupport/gates/station_code_event/REPORT_0.md`; reads in its `reads/`
folder; scripts `sce_timeline.py`, `sce_scan.py`, `sce_obs_flags.py` under `runs_nom/_irdu_grades/scripts_s20/`.

1. **Timeline.** G22's C1C alone enters at 02:47:30 (27 m short, absorbed by its free slant ionosphere), is absent at
   02:48:00, and enters at 02:48:30, 02:49:00, 02:49:30 with post-fit -145, -296, -375 m; the station clock departs
   -3.2 m in both runs, then -7.5 / -14.0 and -12.4 / -18.2 m (with / without the .BIA). At 02:50:00 the admission
   withholds band 1; with the file the screen declares G13, G15, G17, G19, G30. At 02:53:00 the clock jumps +36 / +7 m;
   with the file it stays 37 to 45 m above the run without the file until 04:07:30 (sigma 0.08 to 0.32 m), with 13 to
   24 code rows of the station at weight and 13 to 25 deweighted.
2. **Rules that waited.** The admission bound (`MeasurementAdmission.cpp:123`, 1,000 m) acted at the fourth row; the
   screen (`GrossLos.cpp:251`, two epochs) was held by its station stand-down (`GrossLos.cpp:443`, more than half the
   lines over, because G22 had already moved the clock); RAIM acts only on a satellite spiked at 3 stations
   (`Raim.cpp:587`); the prefit census defers a return epoch and is measure only. The re-entry vet `network.adm_revet`
   (default off, UNVALIDATED, 200 m) would by its rule have refused the row of 02:48:30.
3. **Return.** At the admission's heal (02:53:00, raw code 2.6 m from the station's median) G22's L1C level harvested
   from the drifting epochs (-1,522 / -1,515 m) is folded back with variance 0.149 m2; the solve puts it into G22's
   ionosphere (-154 / -191 m), its code residual (+119 / +184 m) and the clock (+36 / +7 m) before the attribution
   erases it.
4. **Hold.** No release test ends it: G15 leaves and G11 re-enters at 04:07:30, the clock's sigma rises to 2.65 m
   and the clock moves 36 m back; the releases follow at 04:08:00.
5. **Observation file.** S1C 37 to 42 dB-Hz throughout; L1C flagged loss of lock at 02:48:30 only; L1C minus C1C
   within 2 m during the drift (the carrier ran with the code). The engine reads the flag, not the signal strength.
6. **How often.** On 2024 days 129, 130, 132 every gross code row but two and both station episodes are at BSHM00ISR
   (day 129 02:49 and, in the 24 h run, 13:29; drifts of about 210 m per epoch also at 03:54 on day 129 and 02:35 on
   day 132); none on day 130; none on 2026 days 143 to 147 (BSHM00ISR not in that network). Outside those drifts and
   whole-station offsets the network's worst admission distance per epoch is at most 155 m (99th percentile 41 to 64 m).
7. **Bias file.** With the file every station carries receiver DCB columns (coefficients in the ratio of the slant
   ionosphere's, `ReceiverBias.cpp:228`) and a looser ISC (0.85 against 0.09 m); the same structure at ordinary
   stations. The analysis does not settle which part keeps BSHM00ISR's clock off; named pair of 6 h arms, not run:
   day 129 c0 with the .BIA against the same with the .BIA's `SATELLITE_CLOCK_REFERENCE_OBSERVABLES` lines removed.

Candidates (a) to (d) as in section 4, with places, edges and the simulator injector `code_ramp`, are described in
the report. Nothing is built, armed or run.

---
## 6. 2026-11-23, stage A1: two existing keys as arms (registered before the first run)

Order: `gates/station_code_event/manager_ORDER_A1.md` (the managing session, in the author's absence). No build; nothing
merged, pushed or armed.

- **Engine:** main's `gnss_processor_int1122.exe` (compiled code of 1bb4964), sha256[:16] a0fadbb1564612b9, 4 threads
  (`processing.eigen_threads` 4, `network.parallel_max_threads` 4).
- **Controls (same engine):** `runs_irm/_rap_a22h_{f129,f130,f132,s}_6`.
- **Arms** (`scripts/rapid_arm.py make` from this branch on the controls' bases: `config/golden/golden_field_refound.json`
  and `golden_sim_refound.json` of 33c5fef, blob sha256[:16] fc8b458792917d06 and ed13a578404744d9 as in the controls'
  `rapid.json`; `runs_irm/_days22_cbg/golden_field_doy130.json` and `..._doy132.json` for days 130 and 132; the sim
  with `network.start_offset_hours` 0.504167), each differing from its control by one key and the output paths, tags
  in `runs_nom`:
  - **V:** `network.adm_revet` true (`sce_{f129,f132,f130,s}_v_6`);
  - **B:** `network.adm_bound_m` 300 (`sce_{f129,f132,f130,s}_b_6`);
  - **K (S4, information):** day 129 with a copy of the .BIA whose four `SATELLITE_CLOCK_REFERENCE_OBSERVABLES` lines
    are removed, every bias entry kept (`sce_f129_k_6`, data root `runs_nom/_sce_data_bia_noclkref`).
- **Order:** day 129 V and B first (one ticket, 10-epoch smoke of V first); then the arm or arms that meet S1 on day
  132, day 130 and the simulator; K last. Gate labels `rr-sce-...`, priority 5, `--wait-min 1440`.

Readings (as ordered; one sharpened, marked):

- **S1, day 129, each arm against its control.**
  (a) G22's rows of 02:48:30, 02:49:00 and 02:49:30 at BSHM00ISR are not in the solve (`network_pf_rows.csv`); the
  admission's events name the station, the satellite and the epoch; its counters are not zero.
  (b) **Sharpened before the run:** BSHM00ISR's clock stays within **6 m** (ordered: 5 m) of its trend from 02:47 to
  04:10. The trend is a quadratic fitted on the arm's own clock over 02:30:00 to 02:47:00 and 04:10:00 to 04:30:00
  (fit rms 0.55 m on the controls). Reason: on the run without the .BIA, whose code residuals there are under 1 m, the
  clock departs up to 4.3 m from this trend outside G22's epochs (trend error), so 5 m would leave 0.7 m of margin;
  the failing cases read 16.6 m (without the .BIA) and 44.0 m (the control).
  (c) No station stand-down at BSHM00ISR from 02:47 to 04:10; lines of sight declared there, G22 apart: at most 2.
  (d) Marks in the product in hour 4 (03:00 to 04:00, `manager_retro_marks.py`): at most 50 (control 790).
  (e) Products identical to the control up to the first epoch at which the key acts. Every row the key refuses in the
  6 h is listed with its distance; rows refused whose post-fit residual in the control stays under 50 m: 0.
  (f) Built-in user by hour: hours 4 and 5 at most 15 mm (control 32.8 and 38.0 mm); hours 2, 3 and 6 within +5 mm of
  the control.
  (g) Held share by hour from hour 3: at least the control's less 0.5 point, GPS and Galileo.
  (h) Exit 0, 720 epochs emitted.
- **S2, day 132.** G22's rows at BSHM00ISR from 02:35:30 (a return: re-entry events at 02:34:30 and 02:35:30 in the
  control) beyond the arm's bound are not in the solve; (e), (f) with hours 2 to 6 within +5 mm, (g) and (h) as above.
- **S3, day 130 and the simulator.** Rows refused: listed; if none, products and every trace identical to the control
  (zero then means nothing reached the key); the simulator's truth audit with held, right and wrong counts.
- **S4, information.** Arm K against the control: BSHM00ISR's clock against its trend and the declarations, side by
  side.
- **Outcome rule.** An arm that meets S1 to S3 is a candidate for the reference files, decided by the managing
  session under the arming rule. The return (the level folded back at the heal) and the stand-down that nothing ends
  stay open whatever S1 shows (candidates (c) and (d), with the `code_ramp` event, for a later build).
