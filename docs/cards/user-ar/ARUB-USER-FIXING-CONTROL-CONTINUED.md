# ARUB: integer fixing at the user, a post-processed control first, continued

**STATUS**

- **id:** ARUB
- **topic:** user-ar
- **status:** open
- **summary:** Card ARU's continuation (that file nears the 60 KB bar), from its section 8.10 on; the bars, set-ups and results of stages A and B stay in card ARU sections 1 to 8. Trace of the WTZZ00DEU height with the SIF tide loading (2026-11-13, section 2, diagnostic, not graded): every pair-arc fixed under both tide sources holds the same integers; the base of the comparison, the EOT20 grid as the engine reads it, has every phase sign reversed (`OceanTideModel::parseEot20Load`; 597 SIF stations, all eight main constituents, M2 RMS phasor difference to the SIF 18.8 mm as read and 3.5 mm with the phase negated), and at WTZZ00DEU, M2 phase -89.7 deg, it applies nearly the opposite of the M2 loading. The 14 user configurations in `config/` that name a tide source all name that grid. Removing the diurnal translation from the SIF values (card OTL section 9) raises the WTZZ00DEU fixed height by 3.5 and 2.4 mm on DOY 143 and 144. With the network's three loading keys (b11; the nodal corrections add at most 0.4 mm) the mean absolute up error over hours 3 to 6 is 3.7 mm fixed and 4.0 mm float (EOT20 as read 6.2 and 7.0), the median 3D error 6.5 against 8.1 mm, B1 908 pair-arcs with none different, and B2 fails on 6 of 15 receiver-days, 4 of them at WTZZ00DEU, where a north error of up to 10.8 mm stays under every tide source (reported only). Section 3 (`4d4dd3a`): the EOT20 sign fixed; F1 and F2 pass; no integer changes (1 137 pair-arcs before and after); the fixed runs move in up alone, by -7.2 to +8.0 mm per receiver-day over hours 3 to 6; the mean absolute up error falls to 4.4 mm fixed and 5.8 mm float; the corrected grid carries the diurnal translation of the SIF values (applied up within 1.3 mm RMS of the SIF with it) and no horizontal loading, so the SIF without the translation stays ahead (3.7 / 3.9 mm). Stage B fails as frozen: B1 passes on 15 of 15 (897 pair-arcs), B2 fails on 2 of 15 (STPM00SPM DOY 144, BAKE00CAN DOY 145), B4 on 8 of 30. Section 4 (2026-11-13, diagnostic): the B2 failure at STPM00SPM DOY 144 is the float's, not the fix's: every settled solution of hours 2 to 6 reads 9 to 14 mm low, and the combined float reads higher only while its Galileo ambiguities converge (GPS only, fixed 10.7 against float 15.7 mm; Galileo only, 13.9 against 6.1); the low height is local to the receiver and that morning (not at STJO00CAN, not elevation-dependent, not the troposphere states or gradients) and fades over the day, cause not identified; section 4.1 rules out the surface weather (a dry, clear morning; the same pressure on DOY 145 and at St. John's without a dip). Section 5 (2026-11-13, diagnostic): B4's half-cycle witness offsets are GPS III pairs (23 of 25 events) at the two receivers with a receiver-dependent GPS III wide-lane code offset (STPM00SPM, Trimble, -0.12 to -0.22 cycle; WTZZ00DEU, Javad, +0.04 to +0.15; BAKE00CAN, Septentrio, none), shared by the engine's float and absent from the ionosphere-free phase: a smaller rounding margin, not wrong integers. Side finding: BADG00RUS, KOKB00USA and QUIN00USA have no Galileo frequencies in `igs20.atx`, and the receiver cross-system fallback is off in these configurations. Corrects card ARU section 8.10: its WTZZ00DEU "east" values are north.
- **dates:** 2026-11-13 (registered as the continuation; section 2; section 3, the EOT20 sign fixed and the rerun set up and frozen; section 3.1, its results; section 4, the STPM00SPM DOY 144 trace; section 4.1, the weather of that morning; section 5, the B4 witness offsets; section 5.1, a correction to section 5 and card IBW drafted)
- **knobs / posture:** none new. The runs of section 2 use the existing `model_corrections.otl_from_sif`, `model_corrections.otl_remove_diurnal_translation` and `model_corrections.otl_nodal_corrections` (ledger RECOMMEND_SCOPED, armed in the network's field configuration; off in every user configuration).
- **code:** `src/tools/otl_diag.cpp`, `lookup --phases`: the radial amplitude and phase lag of M2, S2, N2, K2, K1, O1, P1 and Q1 from the SIF record and from the EOT20 synthesis (the default output of `lookup` unchanged). Section 3: `OceanTideModel::parseEot20Load` takes the phase lag as atan2(Im, Re) (a defect fix that moves every run on the EOT20 grid), the matching inverse in `src/sim/OtlField.cpp`, and `tests/test_eot20_phase_convention.cpp`.
- **run dirs:** `D:\GnssProcessorSupport\runs_nom\_rap_aru_{otleot,otlsif,b10fix,b10flt,b11fix,b11late,b11flt,b12fix,b12late,b12flt}_est_<doy>_<site>` and `_rap_aru_b12ctl_est_143_wtzz`; section 4: `_rap_aru_s144*_est_<doy>_<site>` (37 runs); executable `D:\GnssProcessorSupport\gates\exe_arue\gnss_processor_arue.exe`; tools and grades `D:\GnssProcessorSupport\presentation\agu26\fixing_control\` (`tools\`, `otl_phase\`, `grade_b10.json`, `grade_b11.json`, `grade_b12.json`, `grade_b8_vs_otleot.json`, `witness_b4_b12.jsonl`)
- **source:** card [ARU](ARU-USER-FIXING-CONTROL.md) section 8.10 (the WTZZ00DEU fixed height worse with the SIF loading on DOY 143 and 144); card [OTL](../../record/refound/OTL_ocean_tide_loading_source_2026-10-16.md) sections 4, 8.4 and 9

**Outcome.** Three traces of card ARU's open items (2026-11-13). The WTZZ00DEU fixed height: the base of that
comparison was defective, the engine reading every EOT20 phase with the wrong sign; fixed on the branch (section 3),
no integer changes, heights move by up to 8 mm, mean absolute up error 4.4 / 5.8 mm (SIF without the translation
3.7 / 3.9). The STPM00SPM DOY 144 B2 failure (section 4): the float looks better only while its Galileo ambiguities
converge; the receiver's low height that morning is local, not the weather, cause unknown. The B4 failures
(section 5): a receiver-dependent GPS III code offset that the witness and the engine share, not wrong integers.
Stage B fails as frozen. Standing: open.

> Results are appended TO THIS FILE, below the text.

---

## 1. Scope

Card ARU nears the 60 KB bar; its results continue here. The bars B1 to B4 stay as frozen in card ARU sections 5 and
8.5, and nothing is frozen anew in this file. Section 2 is diagnostic: its runs were not set up before a graded run and
grade nothing, and the B1 and B2 values it gives are reported only.

## 2. Trace of the WTZZ00DEU height with the SIF tide loading (2026-11-13, diagnostic, not graded)

Question (card ARU section 8.10): with the SIF loading in place of the EOT20 grid, the WTZZ00DEU fixed solution gets
worse on DOY 143 and 144 (mean up over hours 3 to 6, +0.5 to -8.1 and -1.9 to -7.8 mm) while its float improves. Why?

- **Integers.** The same-arc comparison of the B1 bar between the fixed runs of the two sources (`tools\otl_int_compare.py`;
  `_rap_aru_b8fix_est_<doy>_<site>` against `_rap_aru_b9fix_est_<doy>_<site>`, and `b9fix` against `b10fix`): no pair-arc
  fixed to different integers, on any of the 15 receiver-days (WTZZ00DEU 87, 69, 74, 77 and 53 pair-arcs compared,
  first fixes at the same minute). The move is the tide model's alone.
- **What each source applies.** Copies of the section 7 float (EOT20) and of the b9 float (SIF) with the `ocean_tide`
  logger at trace (`_rap_aru_{otleot,otlsif}_est_<doy>_<site>`, 30 runs, all exit 0; `tools\otl_series.py`). Mean up
  loading over hours 3 to 6 at WTZZ00DEU: EOT20 -1.9, -3.2, -3.4, -2.5 and -1.0 mm, SIF +5.1, +5.2, +4.1, +2.3 and
  +0.2 mm (DOY 143 to 147). The static position does not follow the window mean of the change one for one (the fixed
  height moves -8.6 to +7.5 mm, the float -3.4 to +9.7 mm), but the two sources disagree in sign.
- **The EOT20 phases are read with the wrong sign.** M2, amplitude and Greenwich phase lag, SIF against the EOT20
  synthesis: WTZZ00DEU 5.54 mm at -89.7 deg against 4.65 mm at +92.6 deg, STPM00SPM 19.83 at +166.4 against 19.85 at
  -167.2, BAKE00CAN 5.33 at +125.0 against 5.41 at -139.4. Over the 597 stations of `igs20_SIF.json`
  (`gnss_otl_diag lookup --phases`, `tools\otl_phase_stats.py`; `fixing_control\otl_phase\phases_igs20.csv`,
  `phase_stats_igs20.txt`; constituents under 0.2 mm left out):

  | constituent | stations | median phase difference, as read | with the EOT20 phase negated | within 20 deg, as read / negated | RMS phasor difference, as read / negated (mm) | amplitude ratio EOT20 / SIF |
  | --- | --- | --- | --- | --- | --- | --- |
  | M2 | 595 | 106.9 | 2.7 | 0.06 / 0.84 | 18.79 / 3.48 | 1.05 |
  | S2 | 580 | 128.0 | 3.6 | 0.09 / 0.83 | 13.44 / 10.94 | 1.07 |
  | N2 | 593 | 113.1 | 4.0 | 0.06 / 0.83 | 3.80 / 0.74 | 1.05 |
  | K2 | 540 | 123.7 | 6.0 | 0.07 / 0.82 | 2.36 / 0.63 | 1.07 |
  | K1 | 596 | 96.9 | 2.3 | 0.10 / 0.94 | 13.74 / 1.48 | 1.04 |
  | O1 | 597 | 76.3 | 1.9 | 0.08 / 0.98 | 8.15 / 0.77 | 1.05 |
  | P1 | 593 | 92.0 | 3.8 | 0.08 / 0.89 | 5.87 / 3.76 | 1.10 |
  | Q1 | 583 | 69.1 | 4.2 | 0.11 / 0.94 | 1.45 / 0.25 | 1.04 |

  The SIF reading was checked against HARDISP and against data in card OTL section 9. `OceanTideModel::parseEot20Load`
  takes the phase lag as atan2(-Im, Re) of the grid's complex value; the comparison says the file needs atan2(Im, Re).
  Card OTL compared EOT20 amplitudes only (section 4), and its one EOT20 arm read neutral to harmful (section 8.4).
  Of the user configurations in `config/`, 14 (PPP, PPP-RTK, real-time and baseline, among them `ppp_internal.json` and
  `ppp_rtk_internal.json`, the base of these runs) set `data_download.nc_path` to this grid; the 5 others name no tide
  source, and none sets `otl_from_sif`. Every user run on those 14 applies the reversed phases (radial only). Applied up, EOT20 against the SIF without the diurnal translation, within the 6 h windows: largest difference
  6.9 to 12.9 mm at WTZZ00DEU, 7.4 to 15.2 at STPM00SPM and 7.0 to 14.8 at BAKE00CAN, RMS 4.8 to 10.8 mm; at WTZZ00DEU
  the two series are in anti-phase.
- **Reading of card ARU section 8.10 at WTZZ00DEU.** The base of that comparison applied nearly the opposite of the M2
  loading there, so its fixed heights near zero on DOY 143 to 145 are no evidence for it. The section's attribution of
  the offset at STPM00SPM and BAKE00CAN to the EOT20 loading stands, with this sharpening: the EOT20 loading as applied
  is phase-reversed, a defect, not only a coarser model.
- **The diurnal translation.** Card OTL section 9 found the net translation of the SIF diurnal loading absent from
  data built on CODE orbits and clocks, which these runs use. The b9 runs plus `otl_remove_diurnal_translation`
  (`_rap_aru_b10{fix,flt}_est_<doy>_<site>`, 30 runs, all exit 0; `fixing_control\grade_b10.json`): the WTZZ00DEU fixed
  up over hours 3 to 6 goes from -8.1 to -4.7 and from -7.8 to -5.4 mm on DOY 143 and 144, the float from -7.5 to -6.6
  and from -3.0 to -2.5 mm. Mean absolute up error over hours 3 to 6 (`tools\abs_up_summary.py`), fixed / float:

  | tide loading | all 15 | BAKE00CAN | STPM00SPM | WTZZ00DEU | fixed closer to zero in up |
  | --- | --- | --- | --- | --- | --- |
  | EOT20 grid as read (8.8 fixed, section 7 float) | 6.2 / 7.0 | 6.0 / 3.2 | 9.3 / 8.8 | 3.4 / 8.9 | 8 of 15 |
  | SIF (b9) | 4.2 / 4.0 | 3.2 / 2.3 | 5.0 / 4.6 | 4.5 / 5.3 | 8 of 15 |
  | SIF, diurnal translation removed (b10) | 3.7 / 3.9 | 3.5 / 2.1 | 4.7 / 4.5 | 2.9 / 5.3 | 8 of 15 |
  | SIF, translation removed, nodal corrections (b11, the network's three keys) | 3.7 / 4.0 | 3.5 / 2.2 | 4.8 / 4.5 | 2.8 / 5.3 | 9 of 15 |

  B2 as in card ARU section 8.5 (median 3D over minutes 60 to 360, reported only) with b10: fails on 6 of 15, WTZZ00DEU
  DOY 143, 144, 146 and 147 (7.4 against 7.1, 7.7 against 4.9, 9.2 against 6.9 and 13.7 against 12.9 mm), STPM00SPM
  DOY 144 (12.0 against 7.4) and BAKE00CAN DOY 146 (5.0 against 4.0).
- **The network's three loading keys.** b10 plus `otl_nodal_corrections` (`_rap_aru_b11{fix,late,flt}_est_<doy>_<site>`,
  45 runs, all exit 0; `fixing_control\grade_b11.json`): the nodal factors change the applied loading and the positions
  by at most 0.4 mm. B1 as in card ARU section 8.5 (reported only): 908 pair-arcs, none different. B2 fails on the same
  6 of 15 as b10: WTZZ00DEU DOY 143, 144, 146 and 147 (7.1 against 6.9, 7.5 against 4.8, 9.3 against 7.2 and 13.9 against
  13.3 mm), STPM00SPM DOY 144 (11.9 against 7.4) and BAKE00CAN DOY 146 (4.9 against 4.0). Median 3D over the 15, fixed /
  float: EOT20 as read (8.8) 7.3 / 10.9 mm, SIF 6.8 / 8.3, b10 and b11 6.5 / 8.1. The b10 and b11 fixed runs hold the
  same integers on every pair-arc.
- **Correction to card ARU section 8.10.** The WTZZ00DEU values given there as east (-0.6 to -2.9 and -1.4 to -3.0 mm on
  DOY 143 and 144) are north: `tools\height_table.py` printed east, north and up under a header reading north, east and
  up (`tools\trace_split.py` had the same slip in its dN and dE labels; both fixed 2026-11-13). The east moved from -1.7
  to -3.4 and from -0.1 to -1.0 mm. No other value in card ARU was read from those columns.
- **What is left at WTZZ00DEU.** With the network's three keys the fixed solution still reads 4.3 and 5.1 mm low in up on
  DOY 143 and 144 (float 6.3 and 2.4 mm low), and both solutions read north by +4.5 and +3.4 mm on DOY 146 and +10.5 and
  +10.8 mm on DOY 147 (fixed and float). The north error is there under every tide source (EOT20 as read: +9.7 and
  +10.5 mm on DOY 147), and the 3D failures of B2 at WTZZ00DEU on DOY 146 and 147 come mostly from it. No integer error
  shows in the hours checked (`tools\nl_check.py`, held integers against the b11 float, hour 3 and the last hour of
  DOY 143 and 144): the ionosphere-free misfit is at most 0.17
  narrow-lane cycle in the last hour of DOY 143 and 144 and 0.26 in hour 3 of DOY 144, the wide-lane misfit at most 0.28
  cycle, against at most 0.04 and 0.12 at STPM00SPM DOY 144. Not traced further.
- **Not done.** The EOT20 sign is not corrected: a one-sign change to the default tide source of every user
  configuration that moves every run with the EOT20 grid, the author's decision. No run uses corrected EOT20 phases.
  The 14 other receivers and the streams were not run.

## 3. The EOT20 phase sign fixed: rerun set-up, frozen before the graded runs (2026-11-13)

Entry: the author decided on 2026-11-13 to fix the sign found in section 2 and rerun the 15 receiver-days. Section 1
predates this section: the checks below are frozen here, and B1 to B4 are those of card ARU 8.5.

- **Code** (branch `ar/user-control`, no new key): `OceanTideModel::parseEot20Load` takes the Greenwich phase lag as
  atan2(Im, Re) of the interpolated grid value, so the radial signal is Re cos(chi) + Im sin(chi). The simulator's blend
  between anchor points (`src/sim/OtlField.cpp`, `toComplex` and `fromComplex`) takes the matching inverse; the blend
  of conjugates is the conjugate of the blend, so that edit alone changes no output, while the anchor values it blends
  change with the reader. New test file `tests/test_eot20_phase_convention.cpp`, suite `Eot20PhaseConventionTest`: on
  a synthetic two-by-two grid written by the test the reader returns |Z| and atan2(Im, Re) for every BLQ constituent;
  when the EOT20 file is present (skipped otherwise) the M2, K1 and O1 up phasors at WTZZ00DEU, STPM00SPM and BAKE00CAN
  agree with the SIF values to within 30 % of the SIF amplitude plus 0.3 mm (from the section 2 values: largest
  difference 1.35 mm, BAKE00CAN M2, with the fixed sign; every one of the nine cases fails with the reversed sign).
- **What moves.** Every run that takes its loading from the EOT20 grid: the 14 user configurations of section 2, and
  the simulator with `corrections.ocean_tide` (default off). Not the three network configurations (`otl_from_sif` or
  tide loading off, no grid path set) and not the sim fixture `simnet60_quiet_col_24h` (no tide loading).
- **Executable** `D:\GnssProcessorSupport\gates\exe_arue\gnss_processor_arue.exe`, Release, built from `d6a4696` plus
  this change. The engine sources of `arud` (card ARU 8.7) and `d6a4696` are the same: the one source change between
  them, `src/tools/otl_diag.cpp`, is another target.
- **Runs**, 45, in the order of card ARU section 5: `_rap_aru_b12fix_est_<doy>_<site>` and
  `_rap_aru_b12late_est_<doy>_<site>`, copies of the b8 configurations (card ARU 8.7) with only the run paths changed;
  `_rap_aru_b12flt_est_<doy>_<site>`, copies of the EOT20 float runs with the tide trace of section 2 (`otleot`, the
  card ARU section 7 float plus the `ocean_tide` logger) with only the run paths changed. Control
  `_rap_aru_b12ctl_est_143_wtzz`, a copy of `_rap_aru_b11fix_est_143_wtzz` (SIF loading).
- **Checks, frozen here.** F1, the reader: `gnss_otl_diag lookup --phases` built with the change, over the 597
  stations of section 2, gives a median phase difference to the SIF of at most 10 deg for each of the eight main
  constituents, and at least 80 % of the stations within 20 deg for M2 (as read in section 2: 69 to 128 deg, and
  6 %). F2, the identity: `b12ctl` writes `state_trace.csv`, `ambiguity_trace.csv` and `measurement_residuals.csv`
  byte-identical to `_rap_aru_b11fix_est_143_wtzz`. B1 to B4 exactly as in card ARU 8.5, on b12, B2 against `b12flt`.
- **Reported, no rule.** The size of the move, `b12fix` against `b8fix` and `b12flt` against `otleot`: per
  receiver-day the change of the mean east, north and up over hours 3 to 6 and of the B2 median; the change of the
  applied up loading (largest and RMS over the 6 h); the applied up against the SIF series (`b10flt`); the mean
  absolute up error over hours 3 to 6 beside the rows of the section 2 table; the totals of the release rules.
- **Expected, not a rule.** The corrected grid's applied up follows the SIF series to within a few millimetres, so the
  mean absolute up error should fall from 6.2 / 7.0 mm toward the SIF rows of section 2 (3.7 to 4.2 mm fixed). The
  north error at WTZZ00DEU on DOY 146 and 147 is there under every tide source, and the grid carries no horizontal
  loading, so B2 is expected to fail there.

### 3.1 Results (2026-11-13)

Runs `D:\GnssProcessorSupport\runs_nom\_rap_aru_b12{fix,late,flt}_est_<doy>_<site>` and `_rap_aru_b12ctl_est_143_wtzz`,
46 runs, all exit 0 (`fixing_control\batch_b12.txt`), executable of section 3 built from the tree of `4d4dd3a`, sha256
7f605d41cae695911972100b279c6b83f65751e4707954b43c9828c7a0e82f14. Grades: `fixing_control\grade_b12.json`
(`tools\grade_b.py b12fix b12flt b12late`) and `fixing_control\witness_b4_b12.jsonl`; for the size of the move, the old
fixed runs graded against the old float with the trace, `fixing_control\grade_b8_vs_otleot.json`. F1 and F2 pass.
Stage B fails as frozen: B2 on 2 of 15 receiver-days and B4 on 8 of 30 runs; B1 passes on 15 of 15.

- **F1: passed.** `fixing_control\otl_phase\phases_igs20_fixed.csv` and `phase_stats_igs20_fixed.txt`
  (`gnss_otl_diag_arue.exe lookup --phases`, `tools\otl_phase_stats.py`): median phase difference to the SIF 2.7 (M2),
  3.6 (S2), 4.0 (N2), 6.0 (K2), 2.3 (K1), 1.9 (O1), 3.8 (P1) and 4.2 deg (Q1); M2 within 20 deg at 84 % of the
  stations. Every number of the section 2 table, with its two phase columns exchanged.
- **F2: passed.** `b12ctl` against `_rap_aru_b11fix_est_143_wtzz`: `state_trace.csv`, `ambiguity_trace.csv` and
  `measurement_residuals.csv` byte-identical.
- **Unit suites at `4d4dd3a`, Debug:** `gnss_tests` 4 272 tests in 564 suites, 4 235 passed, 37 skipped, 0 failed, both
  cases of `Eot20PhaseConventionTest` among them (the grid present, so the second ran); `gnss_simulator_selftest` 99
  of 99 (`fixing_control\gtest_debug_arue.txt`).
- **B1:** 897 pair-arcs compared, none fixed to different integers. The grader's two-value count at BAKE00CAN DOY 146
  (4, run from 00:00) is the arc-bound artefact of card ARU 8.8.
- **B2:** fails at BAKE00CAN DOY 145 (4.0 against 3.5 mm) and STPM00SPM DOY 144 (14.7 against 8.9 mm); passes on the
  other 13, the five WTZZ00DEU days among them. Median over the 15 receiver-days 7.4 mm fixed, 9.3 mm float (EOT20 as
  read: 7.3 and 10.9); fixed better on 13 of 15 (12). At WTZZ00DEU the pass comes with a worse float (below), which
  loosens the bar.
- **B4:** the same 8 runs and the same episodes (pairs and fixed wide lanes) as in card ARU 8.8, 25 events against 24:
  fix epochs move by up to 20 min (STPM00SPM DOY 145, run from 01:00: G25-G28 fixed at 03:26:00 and 03:26:30 instead of
  03:46:00). Beyond 0.6 cycle (reported, not graded): 1, the G12-G28 case at STPM00SPM DOY 146.
- **B3:** the fixed time to 10 cm is unchanged on 12 of 15 receiver-days (BAKE00CAN DOY 145 14.5 to 15.0 min, DOY 146
  17.0 to 10.5, STPM00SPM DOY 146 24.5 to 24.0); the fixed 3D error at minute 60 moves by -7.3 to +6.6 mm.
- **Release rules:** the totals of card ARU 8.8, unchanged: partner refloat in 12 runs (26 satellites), hold release in
  12 runs (30 satellites).
- **Integers across the fix** (added after the results, not in the frozen list): the fixed runs from 00:00 before and
  after the fix hold the same integers on every pair-arc fixed in both (`tools\otl_int_compare.py --a b8fix --b
  b12fix`: 1 137 pair-arcs on the 15 receiver-days, none different).
- **Size of the move.** Applied up loading over the 6 h, new against old (`tools\otl_diff_stats.py otleot b12flt`):
  largest change 9.1 to 10.8 mm at WTZZ00DEU, 10.7 to 13.8 at STPM00SPM and 9.3 to 18.0 at BAKE00CAN; RMS 5.8 to
  10.9 mm. Means over hours 3 to 6 (`tools\otl_series.py otleot b12flt --pos b8fix:b12fix,otleot:b12flt`), mm, DOY
  143 to 147:

  | receiver | applied up change | fixed up change | float up change |
  | --- | --- | --- | --- |
  | WTZZ00DEU | +6.4, +7.8, +7.1, +4.9, +1.7 | -7.2, -5.0, -1.4, +2.9, +6.2 | -7.3, -4.7, -0.7, +3.2, +6.5 |
  | STPM00SPM | -10.6, -11.2, -10.6, -9.2, -7.7 | +8.0, +5.8, +4.0, +2.1, +0.9 | +7.9, +5.5, +3.4, +2.2, +1.0 |
  | BAKE00CAN | -6.0, -7.6, -9.5, -11.1, -12.4 | +0.5, +0.9, +2.2, +2.9, +3.4 | +0.3, +0.9, +1.8, +3.0, +4.0 |

  The fixed east and north move by at most 0.1 mm (the grid carries no horizontal loading), the float east by -1.3 to
  +1.6 mm and the float north by at most 0.4 mm. As in section 2, a static position does not follow the window mean of
  the change one for one. B2 median per receiver-day, fixed `b8fix` to `b12fix`: -5.2 to +5.0 mm (WTZZ00DEU +4.9 and
  +5.0 on DOY 143 and 144, STPM00SPM -4.5, -5.2 and -4.1 on DOY 143 to 145); float `otleot` to `b12flt`: -5.4 to
  +6.2 mm (`grade_b8_vs_otleot.json` against `grade_b12.json`).
- **Mean absolute up error over hours 3 to 6** (`tools\abs_up_summary.py`), fixed / float, beside the rows of section
  2: corrected grid 4.4 / 5.8 mm, BAKE00CAN 4.0 / 2.7, STPM00SPM 5.1 / 5.3, WTZZ00DEU 4.1 / 9.5, fixed closer to zero
  on 8 of 15 (EOT20 as read 6.2 / 7.0, SIF 4.2 / 4.0, SIF without the translation 3.7 / 3.9).
- **Against the SIF series** (`tools\otl_diff_stats.py`, over the 6 h): the corrected grid's applied up differs from
  the SIF without the translation (`b10flt`) by at most 1.9 to 6.2 mm, RMS 1.1 to 3.6 mm; added after the results, from
  the SIF as distributed (`otlsif`, translation included) by at most 0.3 to 1.8 mm, RMS 0.2 to 1.3 mm. The grid's
  loading carries the same diurnal translation as the SIF values, the term card OTL section 9 found absent from data
  built on CODE products. `otl_remove_diurnal_translation` acts on the SIF branch of `ModelGeometry.cpp` only.
- **The horizontal loading** (added after the results; `tools\otl_series.py otlsif b12flt --pos
  b9fix:b12fix,otlsif:b12flt`): with the applied up within 1.4 mm of the SIF's in the mean over hours 3 to 6, the
  WTZZ00DEU float up reads 3.3 to 5.1 mm lower with the grid than with the SIF (b9) and its fixed up moves by -1.3 to
  +1.4 mm; at STPM00SPM and BAKE00CAN the float up moves by -1.9 to +1.1 mm. The fixed horizontal moves by up to 3.5 mm.
- **Reading.** The fix acts as built: the reader gives the SIF phases, every run on the grid moves in up alone (here by
  up to 8 mm in the mean over hours 3 to 6), and no integer changes. The corrected grid stays behind the SIF without
  the translation (4.4 / 5.8 against 3.7 / 3.9 mm): it carries the diurnal translation and no horizontal loading, and
  the WTZZ00DEU float is the most sensitive to the missing horizontal loading. Expected against found (section 3): the
  fixed error reached the SIF rows (4.4 against 3.7 to 4.2 mm), the float did not (5.8, through WTZZ00DEU); B2 at
  WTZZ00DEU passed on all five days, against the expectation, on a worse float.
- **Not done.** The 14 other test receivers and the stream replays (stopped by the rule of card ARU section 5). No
  removal of the diurnal translation from the grid's loading and no horizontal loading for the grid. The user
  configurations in `config/` still name the grid, and main still reads it with the reversed sign (this fix is on the
  branch only).

## 4. Trace of the STPM00SPM DOY 144 fixed height (2026-11-13, diagnostic, not graded)

Question: B2 fails at STPM00SPM DOY 144 under every tide model of sections 2 and 3 (median 3D over minutes 60 to 360,
fixed against float: 19.9 against 14.2 mm in card ARU 8.8, 11.9 against 7.4 with b11, 14.7 against 8.9 with b12). Why
is the fixed run worse than the float there? Runs `D:\GnssProcessorSupport\runs_nom\_rap_aru_s144*_est_<doy>_<site>`,
37 runs, all exit 0 (`fixing_control\batch_s144*.txt`), executable of section 3; each is a copy of a b11 configuration
(SIF loading, translation removed, nodal corrections) with the one change named (builders `tools\make_s144.py`,
`make_s144b.py`, `make_s144k.py`, `make_s144j.py`, `make_s144jg.py`).

- **The fixed runs agree with every settled solution.** Mean up by hour, hours 2 to 6 (`tools\trace_split.py` positions
  against the reference of `meta.json`): b11 fixed from 00:00 -10.2, -13.1, -12.1, -12.0 and -11.1 mm, from 01:00
  (hours 3 to 6) -12.0, -12.0, -11.2 and -9.9, float -0.8, -4.3, -6.5, -8.2 and -6.9. One system at a time
  (`processing.active_systems`; `s144g`, `s144e`): GPS fixed -12.8, -12.8, -8.7, -9.8 and -10.5, GPS float -14.0,
  -17.7, -12.7, -13.5 and -12.7; Galileo fixed -11.8, -14.0, -14.3, -12.9 and -11.1, Galileo float +24.8, +10.4, -0.9,
  -4.5 and -4.3 (+48.6 in hour 1). Median 3D over minutes 60 to 360, fixed / float: GPS 10.7 / 15.7 mm, Galileo
  13.9 / 6.1, both 11.9 / 7.4. The combined float reads higher than every settled solution while its Galileo part
  converges from above; the Galileo float passes the reference height in hours 3 and 4, which the median of B2
  rewards. With GPS alone the fixed run is better than the float.
- **The whole day** (`s144fix24`, `s144flt24`, `processing.max_duration_s` 86400): the fixed height stays at -13.1 to
  -11.1 mm through hour 6 and rises to -2.8 mm by hour 24; the float reaches -8.2 mm in hour 5 and -1.7 mm in hour 24;
  over hours 19 to 24 the two differ by at most 2.1 mm. A kinematic fixed run (`s144kin`, `filter.process_noise.position_q`
  1e-7 m^2/s, 24 h) reads -11.9 mm on average over hours 2 to 6 and -1.0 mm over hours 7 to 24; its epoch heights
  scatter by 12.2 mm (standard deviation over hours 2 to 24; STPM00SPM DOY 145 10.0 mm, BAKE00CAN DOY 144 7.0 mm). The
  height has no consistent tie to the applied tide: regression of the kinematic height on the applied up
  (`otl_phase\series_b11_<doy>_<site>.csv` from `gnss_otl_diag series`, within 0.09 mm of the engine's trace), -0.35 on
  DOY 144 and -0.06 on DOY 145.
- **What the low height of hours 2 to 6 is not.**
  - The reference: the IGS daily solution of DOY 144 lies 2.6 mm above the five-day mean that is the reference
    (`IGS0OPSSNX_20261440000_01D_01D_SOL.SNX`).
  - The troposphere: the fixed and float wet delays agree within 0.5 mm in hours 3 and 4 while the heights differ by 6
    to 9 mm; with gradients estimated (`s144tg`, `estimation.estimate_tropo_gradients`) the fixed height over hours 3 to
    6 is -11.1 mm and the float -5.2 mm.
  - An elevation-dependent error (the a priori hydrostatic delay, from a standard atmosphere with the Niell mapping in
    this configuration, or multipath): with cutoffs of 15 and 20 deg (`s144m15`, `s144m20`) the fixed height over hours
    3 to 6 is -12.2 and -16.9 mm against -12.1, while the control day DOY 145 moves by -2.4 and -4.0 mm.
  - One satellite: the largest mean phase residual of the fixed run over hours 3 to 6 is 3.0 mm (G03-G31 L1C; RMS of
    the pair means 0.88 mm fixed, 0.70 float), and both systems show the low height.
  - A regional signal: STJO00CAN (St. John's, about 270 km away), GPS only (`s144jg`, reference the mean of its five
    IGS daily solutions), reads -6.8, -6.6 and -3.0 mm fixed on DOY 143 to 145, against STPM00SPM -0.3, -10.5 and
    -3.8 mm (GPS only, `s144g`).
- **Reading.** At STPM00SPM DOY 144 the fixed solution is not the faulty one: every settled solution of hours 2 to 6
  (both fixed runs, GPS fixed and float, Galileo fixed) reads 9 to 14 mm low, and the combined float reads higher only
  while its Galileo ambiguities converge. B2 as frozen counts that crossing against fixing. The low height belongs to
  this receiver and this morning, is shared by GPS and Galileo, does not depend on the cutoff, and fades over the day.
  Its physical cause is not identified from the data on disk: STPM00SPM has no meteorological RINEX file, and surface
  weather at the receiver (water or snow on the radome, a local pressure event) is the next check.
- **Side finding: Galileo receiver antenna offsets.** The STJO00CAN antenna (TWIVC6150 NONE) has only GPS and GLONASS
  frequencies in `igs20.atx`. With `model_corrections.enable_receiver_cross_system_atx_fallback` off, as in every
  configuration of cards ARU and ARUB, its GPS + Galileo runs (`s144j`) carry no Galileo receiver antenna offset and
  read +26.8, +58.1 and +35.8 mm high over hours 3 to 6. Three of the 17 test receivers have the same gap: BADG00RUS
  (JAVRINGANT_DM JVDM), KOKB00USA (ASH701945G_M NONE) and QUIN00USA (ASH701945E_M SNOW). None of them has run in stage
  B; the network-mode runs behind the poster, each with one receiver held out as the user, set the fallback
  (`_rap_agu26_c144c_<site>`). The 14 other receivers need the fallback, or GPS only, before any graded run.
- **Not done.** Weather data for St. Pierre on 24 May 2026 (not on disk; a download needs the author's go-ahead). The
  search restricted to one system with both systems in the float (`estimation.ambiguity_resolution.active_systems`).
  The 14 other test receivers.

### 4.1 Weather on the morning of 24 May (2026-11-13)

Entry: the author asked on 2026-11-13 for the weather at St. Pierre on the morning of 24 May. Source: the Iowa
Environmental Mesonet ASOS/METAR archive, airports LFVP (Saint-Pierre, next to STPM00SPM) and CYYT (St. John's, next to
STJO00CAN), 00:00 to 06:00 UTC of 23 to 27 May 2026; extract `fixing_control\weather_lfvp_cyyt_mornings.txt`.

- **No water, snow or frost on the radome.** LFVP reports CAVOK in every report from 00:00 to 06:00 on 24 May: no
  precipitation, temperature 4 to 9 deg C, dew point 1 to 2 deg C. The first rain of the five mornings falls on DOY 146
  from 06:00, then on DOY 147.
- **The pressure does not set DOY 144 apart.** QNH over 00:00 to 06:00: 1016 hPa on DOY 143, 1027 to 1029 on DOY 144,
  1029 to 1030 on DOY 145, 1023 falling to 1018 on DOY 146, 1008 to 1012 on DOY 147. DOY 145 has the pressure of DOY 144
  and a normal height (fixed -2.3 mm over hours 3 to 6, against -12.1), and St. John's went through the same rise
  (1009.5 to 1010.2 hPa on DOY 143, 1025.7 to 1027.1 on DOY 144, 1028.1 to 1029.1 on DOY 145) with no dip at STJO00CAN
  (section 4, GPS only). So neither term driven by the pressure explains the dip: not the standard-atmosphere a priori
  hydrostatic delay of this configuration (about 35 mm too small on DOY 144 and 38 mm on DOY 145, at 2.28 mm per hPa),
  not non-tidal atmospheric loading.
- **The data of the morning.** The fixed run uses 15.7 phase pairs per epoch on average over hours 2 to 6 (minimum 13),
  against 14.2 and 14.4 on DOY 143 and 145: no satellite is lost on DOY 144.
- **Reading.** Surface weather is ruled out as the cause of the low STPM00SPM height on the morning of DOY 144. The
  cause stays unidentified; what is left is local to the receiver and not the weather (its environment, for example a
  temporary reflector, or its data that morning), with nothing more on disk to test it.

## 5. Trace of the half-cycle witness offsets behind B4 (2026-11-13, diagnostic, not graded)

Question: B4 fails on 8 of 30 runs in every rerun (card ARU 8.6 and 8.8, section 3.1 here), on episodes where the
independent wide lane sits about half a cycle from the fixed one, 7 of the 8 runs at STPM00SPM. What puts the witness
there? Tools (`fixing_control\tools\`): `gps3_wl_frac.py` (the MW of the witness from the RINEX file alone, per
satellite pair in 10-minute windows) and `b4_offsets.py` (the B4 comparison of every clear fix event, split by satellite
block); runs of section 3 (b12).

- **The episodes are GPS III pairs.** Of the 25 disagreeing events of section 3.1, 23 are on GPS pairs, and every one of
  those pairs has one GPS III satellite and one older satellite (G12-G28, G25-G28, G24-G23 and G04-G26 at STPM00SPM,
  G28-G25 at WTZZ00DEU); the other 2 are E26-E29 at STPM00SPM DOY 145. Satellite elevations at the events 22 to 83 deg.
- **The offset belongs to the receiver.** Every clear GPS fix event of the 30 fixed runs, witness minus fixed wide lane,
  read GPS III against older (`b4_offsets.py`, on the signals each run fixed: L1C/L2W at STPM00SPM, whose RINEX file has
  no L1W, and L1W/L2W at WTZZ00DEU and BAKE00CAN):

  | receiver (type) | GPS III against older: events, mean, beyond 0.5 | older against older: events, mean, beyond 0.5 | two GPS III: mean |
  | --- | --- | --- | --- |
  | STPM00SPM (TRIMBLE ALLOY 6.43) | 1 179, -0.153, 14 | 1 265, +0.068, 0 | -0.007 |
  | WTZZ00DEU (JAVAD TRE_3 DELTA) | 1 237, +0.118, 9 | 2 008, -0.029, 0 | -0.075 |
  | BAKE00CAN (SEPT POLARX5S) | 876, -0.001, 0 | 1 080, +0.010, 0 | +0.012 |

  The GPS III satellites stand apart by -0.22 cycle at STPM00SPM and +0.15 at WTZZ00DEU, not at BAKE00CAN, and two
  GPS III satellites together carry no offset. Over all windows (`gps3_wl_frac.py`, circular mean of the window
  fractions, GPS III against older minus older against older): STPM00SPM -0.12 to -0.14 on the five days (per satellite
  -0.03, G21, to -0.25, G04; G28 -0.15 to -0.19), WTZZ00DEU +0.04 to +0.05, BAKE00CAN -0.01 to -0.03.
- **Not the antenna model.** CODE made the products with IGS20_2417 (`APC_MODEL` of the OSB file, `PCV` of the SP3
  header), and the GPS satellite entries of the local `igs20.atx` equal those of `igs20.atx.week2417.bak`. The GPS III
  satellites have an L1 to L2 offset difference of 0.48 to 0.51 m (2.2 to 2.35 wide-lane cycles in the MW), which the
  witness applies; at BAKE00CAN they sit with the older blocks.
- **Shared by the engine.** In the last hour of STPM00SPM DOY 145 the witness and the engine's float wide lane agree
  within 0.06 cycle on G28-G26, 0.12 on G04-G26 and 0.32 on G18-G26 (0.04 and 0.03 on G03-G26 and G31-G26); without its
  antenna terms the witness is 2.36 to 2.46 cycles off on those GPS III pairs. The witness is not independent of this
  term.
- **In the code, not the phase.** The ionosphere-free misfit of the held integers on GPS III pairs stays in the ordinary
  range (STPM00SPM DOY 144: at most 0.074 narrow-lane cycle in hour 3 and 0.033 in the last hour, G28, G13 and G04 pairs
  among them; `tools\nl_check.py`). A wrong wide-lane integer would leave about 0.47 narrow-lane cycle there. The offset
  moves the wide lane, not the ionosphere-free phase.
- **Reading.** The half-cycle witness offsets are excursions past 0.5 of a receiver-dependent GPS III wide-lane offset:
  a code bias of the Trimble (-0.12 to -0.22 cycle) and Javad (+0.04 to +0.15) receivers on the GPS III satellites that
  CODE's network-mean OSB does not remove and the engine does not model (one receiver code bias per signal and system).
  The witness and the engine share it, so B4 marks a smaller rounding margin for GPS III wide lanes at those receivers,
  not a wrong integer. B4 as frozen counts it.
- **Not done.** The two Galileo events. A model term for a receiver code bias that depends on the satellite block (a
  design question for the author). The receiver types of the 14 other test receivers.

### 5.1 Correction to section 5 (2026-11-13)

Section 5 stays as written; this paragraph governs. Its reading says the engine models "one receiver code bias per
signal and system". In the single-difference user runs of cards ARU and ARUB no receiver code-bias state is born
(`state_manager` log: receiver code bias born no, its condition `uncombined_enabled and estimation.estimate_ifb`): the
receiver code bias cancels between satellites. The satellite code biases are the OSB values; a per-satellite code-bias
state (`estimation.estimate_sat_biases`) is created only for a code signal the OSB file does not cover
(`MeasurementModelStates.cpp`), and the CODE file of these days covers every code signal the runs use (GPS C1C, C1W and
C2W, Galileo C1C, C1X, C5Q and C5X). So nothing in the model can take up a receiver code bias that differs between
GPS III and the older blocks. The rest of section 5 stands. The design question it leaves is drafted in card
[IBW](IBW-USER-INTER-BLOCK-WIDELANE-OFFSET.md).
