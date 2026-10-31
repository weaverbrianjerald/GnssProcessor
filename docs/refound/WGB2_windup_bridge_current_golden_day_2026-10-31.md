# WGB2: the wind-up gap bridge on the current golden and over a day

**STATUS**

- **id:** WGB2
- **topic:** network-vehicle
- **status:** closed
- **summary:** Continuation of [WGB](WGB_windup_gap_bridge_2026-10-17.md), which is at the size bar; pre-registered in WGB section 13, closed 2026-10-31. The bridge against same-exe controls on the field golden of card BOF (exe `gnss_processor_bofarm.exe`), DOY 129, 6 h and 24 h. G1 to G3 of WGB section 6 hold on both pairs (day: 267 of 280 gaps bridged, 13 refused in one umbra event, restarts 2417 to 2150, arcs refloated 547 to 23, consumer within 0.2 mm in every hour, 16-site census median -0.01 mm). Independent recount of every attempt's whole-cycle count with an own SP3 reader, Sun, attitude and dipole angle: 61 and 280 attempts, equal counts for 48 of 48 and 91 of 91 listed bridged gaps, 2 and 45 nonzero counts equal to the run-end counts, with the accumulator origin of amendment A2; the pre-registered origin rule gives 90 of 91 on the day and 46 nonzero against 45, a fault of the check's origin model shown from the project's own traces (section 1). Benefit bar (1 mm, section 13) not met. Verdict: the mechanism is correct and inert; KEEP_OFF recommended, the author decides. Deferrals triaged (section 8).
- **source:** [WGB](WGB_windup_gap_bridge_2026-10-17.md) sections 6, 8, 9, 12 and 13; [BOF](BOF_built_off_follow_through_2026-10-28.md); [NSP](NSP_census_neutral_spread_2026-10-29.md)
- **knobs / posture:** `estimation.phase_windup_gap_bridge` (ledger row UNVALIDATED, default false), read, not changed; nothing is armed or removed by this card
- **dates in the text:** 2026-10-31
- **hashes / run dirs:** exe `D:/GnssProcessorSupport/gates/exe/x64-Release/gnss_processor_wgbclose.exe` (a copy of `gnss_processor_bofarm.exe`, sha256[:16] `91143c81de9a9547`); run root `D:/GnssProcessorSupport/runs_nom`: arms `_rap_wgbc_{ctl,on}_f{6,24}`, smokes `_rap_wgbc_s_{ctl,on}`, census replays `_rap_wgbcc_{ctl,on}6_*` and `_rap_wgbcc_{ctl,on}24_*`; scratch, reports and outputs `D:/GnssProcessorSupport/gates/wgbclose/` (named per section below); injected single-receiver run `gates/wgbclose/inject/`; checker `scripts/windup_gap_independent_check.py`; every attempt in [WGB2_attempt_tables](WGB2_attempt_tables.md)

**Outcome.** Question: does the wind-up gap bridge hold on the golden of cards MRF and BOF over a day with GPS noon turns, and are its whole-cycle counts right? Against same-exe controls, 6 h and 24 h, G1 to G3 hold on both pairs (day: 267 of 280 gaps bridged, 13 refused in one umbra event, arcs refloated 547 to 23, consumer within 0.2 mm in every hour, census median -0.01 mm), and no benefit appears (1 mm needed). An independent recount equals the bridge's count for 91 of 91 listed gaps and 45 of 45 nonzero counts after amendment A2 (pre-registered origin rule: 90 of 91, 46 against 45). Verdict: correct and inert; KEEP_OFF recommended, the author decides.

## 1. Amendments to the pre-registration

**A1 (written before the day pair was read).** The independent check was validated on two single-receiver runs of the project before the day pair finished (the gap run of WGB section 7, and a run with three holes placed over cut crossings, section 5). In the second, one gap (E26, TOW 266280) differs from the check by one cycle: the epoch's raw angle is +3.133 rad in the check and -3.140 rad in the project's trace, on either side of the +-pi cut, and the two bridged angles (raw angle plus 2 pi N) agree to 0.0102 rad, the usual model difference of the raw angle (median 9e-4 cycle, largest 1.0e-2 cycle against the project's trace, WGB section 13). The count N is defined against the epoch's raw angle, so it is representation-dependent within that difference.

Amendment. I1 counts a bridged attempt as agreeing when the counts are equal, or differ by the flip of the cut: the check's raw angle at the epoch lies within 0.07 rad (above the largest difference seen, 0.065 rad) of +pi and the project's count is the check's plus 1, or lies within it of -pi and the project's count is the check's minus 1. I2 compares the run-end count of nonzero bridged gaps with the interval the same allowance gives (an attempt near the cut may carry either count). Both readings are reported: literal equality, which is the pre-registration, and the amended one. The 6 h pair had been read (48 of 48 equal counts, no attempt within 0.07 rad of the cut), so the amendment does not touch it; the day pair had not.

**A2 (written after the day table was read under the pre-registered origin rule).** Under that rule, which takes the origin of a pair without a restart event from the receiver data (a phase observation of any signal above the mask, outside the umbra), the 91 listed bridged attempts of the day pair give 90 equal counts and one disagreement: BSHM00ISR G27 at TOW 304230, gap 540 s, project 0, check -1 (K -1). The project's traces show the cause on the check's side. G27 is tracked at BSHM00ISR from TOW 298710 on L2L and L5Q only (the RINEX at TOW 299100 holds no L1 phase), one band of the two the dual-frequency golden needs, and L5 is not in its signal list; L1C and L2W appear together later, and the first carried ambiguity (L1C) is born at TOW 301170 (`network_amb_state.csv`; `network_slip_raw.csv` is continuous from 301170), after the raw angle crossed the -pi cut between 301110 and 301140. The project's accumulator was therefore seeded after the crossing (K 0), the check's, seeded 41 min earlier, was not. Amendment: the origin of a pair without a restart event is the first birth of a carried ambiguity of the bridge arm in `network_amb_state.csv`, the epoch the project first modelled the pair; the receiver-data epoch (dual-frequency, the golden's signal list) stays as the earlier alternative, and an attempt whose count differs between the two origins is flagged. Both readings are reported, the pre-registered rule and A2; the 6 h pair gives 48 of 48 under either.

A third correction concerns the selection of attempts, not the method: the first run of the day table selected them by the gap rounded to a second, which put 9 gaps of 899.99999 s (an attempt of the project, a gap under 900 s) among the gaps beyond the horizon, so that 271 of the 280 attempts were found; the selection uses the printed gap, and the run-end counters (280 attempts, 267 bridged, 13 refused) are met exactly.

## 2. Runs and identity of the twin

Exe `gnss_processor_wgbclose.exe` for every run (WGB section 13). Wall times are not timings: the box was shared with other sessions for the whole stage.

| arm | tag | length | exit | wall s | epochs emitted |
| --- | --- | --- | --- | --- | --- |
| C6 | `_rap_wgbc_ctl_f6` | 6 h | 0 | 3592.6 | 720 |
| W6 | `_rap_wgbc_on_f6` | 6 h | 0 | 3590.7 | 720 |
| C24 | `_rap_wgbc_ctl_f24` | 24 h | 0 | 26725.2 | 2880 |
| W24 | `_rap_wgbc_on_f24` | 24 h | 0 | 26978.5 | 2880 |

The twin C6 equals the baseline `runs/_rf0hv17_field6` (exe `gnss_processor_bofarm.exe`, no census key) byte for byte in `products.txt` (11,345,974 bytes, sha256[:16] `bf16e9474305e2de`) and in `logs/closed_loop_trace.csv` (57,047 bytes, `eb2eca92f04e9906`), so the census key changes nothing and C6 stands for the baseline (`gates/wgbclose/identity_c6_vs_baseline.txt`). C24 is a new run of the same arm for 24 h (the control of card BOF, `_rap_bof_ctl_f24`, is the golden before card BOF, WGB section 13). Smokes of 10 epochs, `_rap_wgbc_s_{ctl,on}`: exit 0, products identical, the knob-on smoke prints the run-end bridge line.

## 3. G1, the on path

| | C6 | W6 | C24 | W24 |
| --- | --- | --- | --- | --- |
| forward transitions; in horizon | 600755; 600616 | the same | 2421952; 2419535 | the same |
| out-of-horizon transitions (between the horizons, beyond both) | 139 (61, 78) | 139 (61, 78) | 2417 (280, 2137) | 2417 (280, 2137) |
| accumulator restarts | 139 | 91 | 2417 | 2150 |
| transitions stepped by 0.9 cycle or more (max) | 2 (0.994) | 0 (0.832) | 73 (1.382) | 61 (1.382) |
| restart events answered; arcs refloated | 60; 116 | 12; 23 | 278; 547 | 12; 23 |
| attempts (stations with an attempt or a restart beyond the horizon) | | 61 (33) | | 280 (58) |
| bridged / refused | | 48 / 13 | | 267 / 13 |
| refusals by reason | | umbra 13 | | umbra 13 (one event) |
| bridged over a nonzero count | | 2 | | 45 |
| virtual epochs evaluated | | 1869 | | 10244 |

Source: `gates/wgbclose/g1_w6.txt` and `g1_w24.txt`, read from the run-end lines of the four `stdout.txt`. In both pairs the attempts equal bridged plus refused and equal the gaps the census counts between the horizons in both arms (61; 280); the restarts fall by the bridged count (139 to 91, minus 48; 2417 to 2150, minus 267); the answered events and the refloated arcs fall with them; no refusal is a no-state, step or closure refusal. The 13 umbra refusals of the day are the 13 of the 6 h pair (G20 at TOW 262920 at 13 stations), the only umbra gap of the day. The day shows no noon-turn refusal (0): the day did not exercise that guard (section 5.3), which is stated, not a miss. **G1 holds for both pairs.** The 6 h numbers equal those of WGB sections 9, 10 and 12 on the golden of 2026-10-17, count for count: the MW re-flag skip and card BOF move none of them.

## 4. G2 and G3

**G2, WTZZ00DEU 3D p50 by hour (m).** `gates/wgbclose/g2_w6.txt`, `g2_w24.txt` (the repository tool `p50.py` gives the same values to the third decimal: `p50_6h.txt`, `p50_24h.txt`).

| hour | 1 | 2 | 3 | 4 | 5 | 6 | mean 2 to 6 |
| --- | --- | --- | --- | --- | --- | --- | --- |
| C6 | 0.0588 | 0.0123 | 0.0231 | 0.0123 | 0.0165 | 0.0120 | 0.01522 |
| W6 | 0.0588 | 0.0122 | 0.0232 | 0.0123 | 0.0165 | 0.0119 | 0.01522 |

| hour | 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 | 9 | 10 | 11 | 12 |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| C24 | 0.0588 | 0.0123 | 0.0229 | 0.0122 | 0.0166 | 0.0120 | 0.0159 | 0.0085 | 0.0077 | 0.0105 | 0.0097 | 0.0080 |
| W24 | 0.0588 | 0.0123 | 0.0231 | 0.0123 | 0.0165 | 0.0120 | 0.0159 | 0.0085 | 0.0078 | 0.0105 | 0.0097 | 0.0080 |
| change, mm | +0.0 | -0.0 | +0.2 | +0.0 | -0.1 | -0.0 | +0.0 | +0.0 | +0.0 | +0.0 | +0.0 | +0.0 |

| hour | 13 | 14 | 15 | 16 | 17 | 18 | 19 | 20 | 21 | 22 | 23 | 24 |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| C24 | 0.0116 | 0.0122 | 0.0080 | 0.0112 | 0.0103 | 0.0174 | 0.0120 | 0.0132 | 0.0164 | 0.0290 | 0.0348 | 0.0296 |
| W24 | 0.0116 | 0.0122 | 0.0081 | 0.0112 | 0.0103 | 0.0176 | 0.0120 | 0.0132 | 0.0164 | 0.0290 | 0.0348 | 0.0296 |
| change, mm | +0.0 | +0.0 | +0.0 | +0.0 | +0.0 | +0.2 | +0.0 | +0.0 | +0.0 | +0.0 | +0.0 | +0.0 |

The worst hour moves by +0.1 mm in the 6 h pair and +0.2 mm (hours 3 and 18) in the day pair, against a bar of +10 mm; the mean hourly p50 over hours 2 to 24 is 0.01488 m (C24) and 0.01490 m (W24). **G2 holds for both pairs.**

**G3, the 16-site census.** `gcensus.py make wgbcc_{ctl,on}6` and `wgbcc_{ctl,on}24 --hours 24`, 17 replays per arm and pair, all 68 exit 0, graded by `gates/wgbclose/g3_report.py` (`g3_6h.txt`, `g3_24h.txt`; the census tool's own text: `census_grade_6h.txt`, `census_grade_24h.txt`, where the label reads "hours 2-6" for the day too). Paired change of the mean hourly p50 over hours 2 to 6 (W6) and 2 to 24 (W24):

| | median | mean | best | worst | better / worse at 1 cm | worse by 2 cm | WTZZ00DEU |
| --- | --- | --- | --- | --- | --- | --- | --- |
| 6 h | +0.00 mm | -0.01 mm | -0.17 mm (DAV1) | +0.09 mm (ARHT00ATA) | 0 / 0 | 0 | +0.00 mm |
| day | -0.01 mm | -0.07 mm | -0.35 mm (POHN) | +0.06 mm (HARB) | 0 / 0 | 0 | +0.015 mm |

The bar allows a median of +5 mm and 3 sites worse by 2 cm. The median site p50 by hour is 0.142 0.050 0.036 0.029 0.032 0.030 m (C6) and 0.142 0.050 0.036 0.029 0.032 0.029 m (W6); over the day the 24 values of C24 and W24 agree to the third decimal except hour 24 (0.028 and 0.027 m). In the 6 h pair no site moves by more than 0.17 mm, inside the neutral spread of 0.19 mm that [NSP](NSP_census_neutral_spread_2026-10-29.md) measured over hours 2 to 6. In the day pair four sites improve by 0.27 to 0.35 mm (POHN, ARHT00ATA, WUH2, QUIN) and none worsens by more than 0.06 mm; the neutral spread over 24 h was not measured, so those four moves are above the only spread known and below any size that matters to the bars. **G3 holds for both pairs.** The benefit clause of the verdict rule (census median at most -1 mm and WTZZ00DEU at least 1 mm better, on the day pair) is not met: -0.01 mm and +0.015 mm.

## 5. The independent recount

### 5.1 The check against the project's own traces

Three single-receiver checks of the recount itself, all on GCGO00USA of DOY 129 (GPS and Galileo, accumulated wind-up), none of them an arm of this card:

- **Raw angle** (`validate-trace`, `gates/wgbclose/validate_trace.txt`): against the `wrapped_rad` column of the project's trace of the continuous run (`gates/windupgap/ppp_final/cont/logs/windup_trace.csv`), 3940 rows: median difference 8.9e-4 cycle, 95th percentile 3.3e-3, maximum 1.04e-2 (E21, E27). A frame or sign error would show as a quarter or a half cycle.
- **The gap run of WGB section 7** (`validate-gap-run`, `validate_gap_run.txt`): 16 satellites return after a 630 s hole with the bridge on; the project's count is (accumulated - wrapped) / 2 pi in its own trace. The recount is equal for 16 of 16, including E13 and E14 (-1 each, K -1, J 0).
- **An injected run with three holes** (`gates/wgbclose/inject/`, `validate_gap_run_holes.txt`): the same file with 40 epochs removed in three blocks placed over cut crossings of G21, G02 and E26 (gaps of 510 s, 450 s and 330 s, every satellite losing the epochs), bridge on, exe of this card. 44 gaps come back bridged; the recount is equal for 43 and equal up to the cut for 1, none differs. 11 of the 44 carry a nonzero count, and 3 gaps have the cut inside them (J -1): G21 at TOW 263880 and G02 at TOW 265200, equal counts, and E26 at TOW 266280, the case of amendment A1.

`--selftest` of the script (33 checks: Sun at the equinoxes and solstices, the dipole angle on analytic geometry, the unwrapping and the K and J split, the interpolation, the umbra and beta, the cut rule, the readers) passes (`selftest.txt`).

### 5.2 The attempts of the two bridge arms

`gates/wgbclose/check_w6_summary.txt`, `check_w24_summary.txt` (A2 rule) and `check_w24_registered_summary.txt` (the pre-registered rule); every attempt in [WGB2_attempt_tables](WGB2_attempt_tables.md).

| | W6 | W24 |
| --- | --- | --- |
| attempts: run-end counter; in the check | 61; 61 | 280; 280 |
| bridged; refused (the run-end counters, equal in the check) | 48; 13 | 267; 13 |
| run-end lines: attempts listed; bridged with their count | 61; 48 | 100; 91 (180 attempts not listed) |
| I1, pre-registered origin rule: equal counts | 48 of 48 | **90 of 91** (one disagreement) |
| I1 with A2: equal counts; equal up to the cut; disagreements | 48; 0; 0 | 91; 0; 0 |
| nonzero count among the listed: bridge; check | 2; 2 | 12; 12 (pre-registered rule: 13) |
| I2: nonzero over all bridged, run-end count; check | 2; 2 | 45; 45 (pre-registered rule: **46**) |
| the check's range once the cut is allowed for | 2 to 2 | 40 to 46 |
| gaps with the cut inside (J not 0); counts carried in (K not 0) | 0; 2 | 5; 40 |
| bridged attempts within 0.07 rad of the cut | 0 | 6 |
| accumulator origin: last restart; first birth | 8; 40 | 127; 140 |
| attempts whose count changes with the earlier receiver-data origin | 0 | 0 |
| independent umbra in the gap: not bridged; bridged | 13 of 13; 0 of 48 | 13 of 13; 0 of 267 |
| largest 15 s step in a bridged gap | 0.001 cycle | 0.004 cycle |

**6 h.** The two nonzero counts are SGOC00LKA G14 (TOW 278100, gap 570 s) and KIT300UZB G22 (TOW 279690, gap 480 s), -1 each, K -1 and J 0 in the recount, the two the earlier arms expected.

**Day.** The 12 listed bridged gaps with a nonzero count, bridge and recount equal in all:

| station | sat | TOW | gap s | bridge | recount | agreement |
| --- | --- | --- | --- | --- | --- | --- |
| CEBR00ESP | G01 | 304110 | 510 | -1 | -1 (K -1, J 0) | yes |
| BSHM00ISR | G16 | 304230 | 540 | -1 | -1 (K -1, J 0) | yes |
| BSHM00ISR | G18 | 304260 | 570 | -1 | -1 (K -1, J 0) | yes |
| BSHM00ISR | G01 | 304350 | 660 | -1 | -1 (K -1, J 0) | yes |
| MAC100AUS | E02 | 312450 | 540 | 1 | 1 (K 0, J 1) | yes |
| LMMF00MTQ | E14 | 312690 | 540 | -1 | -1 (K 0, J -1) | yes |
| LMMF00MTQ | E14 | 313590 | 720 | -1 | -1 (K -1, J 0) | yes |
| LMMF00MTQ | E14 | 314490 | 900 | -1 | -1 (K -1, J 0) | yes |
| LMMF00MTQ | E14 | 315390 | 360 | -1 | -1 (K -1, J 0) | yes |
| LMMF00MTQ | E14 | 316290 | 540 | -1 | -1 (K -1, J 0) | yes |
| LMMF00MTQ | E14 | 317190 | 720 | -1 | -1 (K -1, J 0) | yes |
| LMMF00MTQ | E14 | 318090 | 900 | -1 | -1 (K -1, J 0) | yes |

The gaps of 900 in the table are 899.99999 s. The cut is crossed inside the gap in 5 attempts of the day, two of them listed (MAC100AUS E02 +1 and LMMF00MTQ E14 -1, both equal to the bridge; both within 0.07 rad of the cut, with equal counts) and three not listed (BOR100POL E18 TOW 291180, WARK00NZL E18 TOW 308160, SCTB00ATA E18 TOW 308190): the first field gaps with the cut inside them. Of the 176 bridged attempts the lines do not list, 33 have a nonzero recount (E18 23, G18 5, G22 2, G14, E31 and G06 one each); with the 12 listed that is 45, the run-end count. The recount of those 176 is therefore checked in aggregate, not one by one (the run-end list is capped at 100 attempts). The nonzero counts are 24 of -1 and 21 of +1, 32 on Galileo satellites (E18 23, E14 7) and 13 on GPS; 180 of the 267 bridged gaps are E14 and E18.

**The pre-registered rule on the day.** I1 is missed by one attempt (BSHM00ISR G27, TOW 304230) and I2 by one count (46 against 45); the cause is the check's origin model (section 1, A2), shown from the project's `network_amb_state.csv` and `network_slip_raw.csv`. With the origin of A2 both bars hold on both pairs, and the earlier receiver-data origin changes no count.

### 5.3 The guard-set limit

The bridge refuses a walk only in the Earth's umbra and in the noon-turn window of a GPS Block IIF or IIR (`SatAttitude::inNoonTurnWindow` returns false for every other system; the wind-up passes `enable_orbit_normal` false, so Galileo satellites and the BeiDou satellites other than the BeiDou-2 GEO are bridged on the nominal yaw-steering law). What the day exercised:

- **Umbra.** 13 refusals, one event (G20, TOW 262920, 13 stations); the recount finds the satellite in the umbra inside the gap for 13 of 13 refusals and for none of the 267 bridged gaps.
- **Noon turn.** 0 refusals. The GPS satellites that turn on DOY 129 are G01 (11:00 to 11:05 and 22:58 to 23:06 UT), G25 (08:37 to 09:00 and 20:36 to 21:01) and G26 (11:16 to 11:33 and 23:15 to 23:30) (`gates/wgbclose/gps_noon_windows_doy129.txt`, `attitude_beta_check.py windows`). Six attempts fall on these satellites (CEBR00ESP and BSHM00ISR G01, BSHM00ISR G26, GCGO00USA G25, MAJU00MHL G26, BOGT00COL G25), none overlaps a window; the nearest, BOGT00COL G25 at TOW 333330, ends 62 s before the G25 turn starts (nominal yaw rate 0.048 degree per second against the Block IIF limit of 0.11). The largest nominal yaw rate over any bridged GPS gap is 0.089 degree per second (BSHM00ISR G16). The guard's noon-turn refusal is therefore not exercised by a field day; its unit cases (WGB section 4) stand.
- **Beyond the guard.** 20 bridged attempts have |beta| under 5 degrees at some point of the gap, 12 on GPS (the largest nominal yaw rate among them is that of BOGT00COL G25, 0.048 degree per second) and 8 on Galileo (E30, E21, E24, E02 twice, E31 three times, |beta| 2.9 to 3.7 degrees). The recount uses the same nominal law as the bridge, so its agreement does not test that law for those Galileo satellites, whose real attitude at low beta may differ from it; BeiDou is not in the golden.

## 6. The network residuals

A datum-invariant check, as in WGB section 9.3: the post-fit phase residual of the arcs that return from an attempted gap, over the 10 minutes after the gap, in both arms (`gates/wgbclose/pf_bridged.py`, `pf_bridged_w6.txt`, `pf_bridged_w24.txt`; the rows are the same in both arms). Bridged arcs: W6 1.905 mm (control) and 1.907 mm (bridge), 1093 rows, 20 arcs; W24 3.378 mm and 3.395 mm, 6183 rows, 87 arcs. The 13 arcs the umbra refused, restarted in both arms: 1.058 mm in both pairs, 474 rows. The bridge changes the residual of the arcs it keeps by 0.02 mm, 0.5 % over the day.

## 7. Bars and verdict

| bar | 6 h pair | day pair |
| --- | --- | --- |
| G1, on path | holds | holds (no noon-turn refusal: the guard was not exercised) |
| G2, consumer (no hour worse by 10 mm) | holds, worst +0.1 mm | holds, worst +0.2 mm |
| G3, census (median at most +5 mm, at most 3 sites worse by 2 cm) | holds, +0.00 mm, 0 sites | holds, -0.01 mm, 0 sites |
| I1, pre-registered origin rule | holds, 48 of 48 | **missed**, 90 of 91 |
| I2, pre-registered origin rule | holds, 2 = 2 | **missed**, 46 against 45 |
| I1 and I2 with A1 and A2 | hold | hold, 91 of 91 and 45 = 45 |
| benefit (day pair: census median at most -1 mm, WTZZ00DEU 1 mm better) | | not shown (-0.01 mm, +0.015 mm) |

Verdict by the rule of WGB section 13. The mechanism is correct to the extent a recount under the nominal attitude law can show: every listed integer is right, the aggregate of the unlisted ones is right, the cut is handled (five gaps with the cut inside in the field, two listed and equal; three more in the injected run), the umbra is refused and the day's gaps stayed clear of the noon turns. It does not regress anything the field measures (G1 to G3). It shows no benefit: 524 refloated arcs a day are removed (547 to 23) and the held-out consumer moves by at most 0.2 mm in an hour and the census by -0.01 mm at the median (four of 16 sites improve by 0.27 to 0.35 mm over the day, none worsens by more than 0.06 mm), against a benefit bar of 1 mm. The pre-registered I1 and I2 are missed on the day by one attempt; the cause lies in the check's origin model and is shown from the project's own traces, but the correction A2 was made after the table was read, and that is stated. By the rule: correct, no regression, no benefit, so **KEEP_OFF**. A wrong integer would have been REMOVE; none was found.

## 8. Deferrals, triaged (standing invariant 3)

| deferral | triage | data |
| --- | --- | --- |
| warm-resume re-derivation of the accumulator | field-inert | neither golden has a snapshot or resume key (checked by key walk, `config/golden/golden_{field,sim}_refound.json`), so no golden run resumes; the accumulator `prev_windup_` is a member of `GnssCorrections` that no snapshot code reads (the warm-resume code keeps only the wind-up response drain live), so a resumed run reseeds every satellite from its raw angle with the bridge on or off. The deferral is a feature (re-deriving the integer at resume), not a defect of the bridge |
| satellite-state provider unit test | field-inert, register line | the provider (`Geometry::getLookBackState`) is exercised by every attempt (1869 virtual epochs in the 6 h arm, 10244 in the day arm, no 'no state' refusal), and the recount checks what it produces against an independent orbit, Sun and attitude for 91 listed gaps and 45 nonzero counts. A unit test with orbit products stays owed if the knob is kept |
| independent simulator bed | not built, covered | the simulator injects wind-up through the same function (`EphemerisProvider::phaseWindupMetres` calls `computePhaseWindUp` without a bridge), so it cannot grade the option (WGB section 3); the independent recount, the injected single-receiver run and the field arms stand in |

## 9. Standing and disposition

Standing: `estimation.phase_windup_gap_bridge` stays default off. Recommended ledger verdict KEEP_OFF in place of UNVALIDATED: the question the card was opened to answer is answered (correct, inert on the float vehicle), and arming needs a benefit this posture does not show. The alternatives are the author's: leave UNVALIDATED, or REMOVE the code. What REMOVE deletes: the merge `76160b8` added 879 lines of source and header (`WindUpGapBridge.{hpp,cpp}` 309, the rest hooks in `GnssCorrections`, `Antenna`, `PppModel`, `ModelGeometry`, `SatAttitude`, the loader, two run-end reports), 636 lines of tests (24 cases) and three ledger rows. What KEEP_OFF keeps: a verified mechanism for a future posture that carries integer ambiguities across gaps, where a restart step would cost an integer (the float vehicle of the goldens carries none).

Owed, for the registers: the provider unit test if the knob is kept; a per-attempt trace (station, satellite, epoch, gap, outcome, count) if the knob is kept, because the run-end list holds 100 attempts and the day has 280, so 176 bridged counts are checked in aggregate here; the independent check needs `numpy` (`py -3.14` on this box) and the bridge arm's `network_amb_state.csv` for the A2 origin.
