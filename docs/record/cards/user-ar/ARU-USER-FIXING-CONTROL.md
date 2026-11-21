# ARU: integer fixing at the user, a post-processed control first

**STATUS**

- **id:** ARU
- **topic:** user-ar
- **status:** closed
- **summary:** The user engine has never produced a correct integer fix. Stage A measures, with no fixing, whether the float ambiguities of a single receiver on one analysis centre's final products with published phase biases (CODE MGX final orbit, 30 s clock, OSB code and phase biases) are integer within their own formal covariance, with the position estimated and with the position held at the IGS coordinate. Stage B turns fixing on only if stage A passes, against bars frozen below. Stage A result (2026-11-10, section 4): float integer (A1 26 of 30, A2 30 of 30 system-days with the position estimated), covariance optimistic by a factor of about 2 to 3 (A3 15 of 30), stage A not passed as frozen; the MW wide-lane source of the search is wrong by 2 cycles on pairs with one GPS III satellite (satellite antenna offset per frequency omitted). Correction (section 6): stage A searched the narrowlane row, on which the float misses by 0.26 (GPS) and 0.19 (Galileo) cycle in hour 2; the integer grade holds for the ionosphere-free combination, the row stage B searches. Stage B (2026-11-11, section 7), first three receivers: fails as frozen (B1 fails on 1 of 15 receiver-days, B2 on 2 of 15); fixed wide lanes independently confirmed; first failing term a held integer with no release or slip test through a disturbed arc (BAKE00CAN DOY 145); the 14 other receivers and the streams not run. Rerun (2026-11-12, section 8): the failure re-traced to a single-signal refloat that left the other signal held, plus no release of a held pair; with a release (`hold_release_cyc` 0.25) and a partner refloat, B1 passes 15 of 15, B2 still fails on the same 2 receiver-days, the new wide-lane bar B4 fails on a half-cycle witness offset at STPM00SPM; the partner refloat is blind to the post-fit screen's resets (defect, 8.6). Second rerun with the release rules keyed on the ambiguity generation (8.7, 8.8): the partner refloat acts at the BAKE00CAN event, B1 15 of 15, B2 fails on 3 of 15 (BAKE00CAN DOY 145 joins, height 4 to 6 mm lower after the refloats), B4 unchanged. Continued in card [ARUB](ARUB-USER-FIXING-CONTROL-CONTINUED.md) (this file nears the 60 KB bar), from 2026-11-13: the EOT20 grid that 8.10 compared against is applied with every phase sign reversed (a defect in `OceanTideModel::parseEot20Load`), and the WTZZ00DEU values given as east in 8.10 are north.
- **dates:** 2026-11-10 (pre-registration), 2026-11-11 (stage B set-up frozen, section 5; corrections, section 6; stage B results, section 7), 2026-11-12 (B1 failure re-traced and stage B rerun frozen, section 8; rerun results 8.6; second rerun frozen 8.7, results 8.8, trace 8.9, height offset 8.10), 2026-11-13 (continued in card ARUB)
- **knobs / posture:** four new, default off: `estimation.ambiguity_resolution.wl_from_float`, `estimation.ambiguity_resolution.nl_sigma_floor_cyc` (section 5), `estimation.ambiguity_resolution.hold_release_cyc`, `estimation.ambiguity_resolution.refloat_partner_signal` (section 8). Stage A runs `estimation.ambiguity_resolution.mode` 1 with `ratio_threshold` 1e9 (the search runs and logs, nothing is accepted) and the resolver log at debug.
- **code:** `src/core/filter/AmbiguityResolver.cpp` (debug line `CLCOV`: the formal covariance of the double differences beside the existing `CLFRAC` values; logged only at debug, no other output changes); stage B: `floatWidelane_`, `addSharedReferenceFloor_` (section 5)
- **run dirs:** `D:\GnssProcessorSupport\runs_nom\_rap_aru_*`; tools and grades `D:\GnssProcessorSupport\presentation\agu26\fixing_control\`
- **source:** the live fixing sessions of 2026-11-10 (`D:\GnssProcessorSupport\presentation\agu26\figures\live_fixing_note.md`: 0 fixes in 27 946 searches, wide lane integer for GPS, ionosphere-free narrow lane near uniform under a formal sigma of 0.005 to 0.04 cycles); card [SSR-BIAS-SIGN](../../../cards/realtime/SSR-BIAS-SIGN.md); the July 2026 wrong integers on ALGO00CAN ([CONSUMER-AR-BLUNDER](../consumer/CONSUMER-AR-BLUNDER.md))

**Outcome.** Stage A (section 4, corrected in section 6): on CODE final products the ionosphere-free combination of the user float IS integer (the narrowlane row that stage A's searches used is not: 0.26 and 0.19 cycle in hour 2). Narrow lane within 0.10 cycle on 30 of 30 receiver-days (median 0.045 cycle with the position estimated, 0.034 held). The covariance bar A3 fails on most receiver-days: the miss levels off near 5 mm while the formal sigma keeps falling, so stage B was not started. First model term that prevents a correct fix: the MW wide lane fed to the WL-constrained search omits the satellite antenna offset per frequency, 2 cycles wrong on every pair with one GPS III satellite (5 526 of 5 526 pair-epochs). Stage B (section 7) with the float wide lane, a 0.04-cycle floor and hold variance 1e-7, on 3 receivers and 5 days: fixing improves the median 3D error over minutes 60 to 360 on 13 of 15 receiver-days (7.3 against 10.9 mm), but fails the frozen bars. One pair at BAKE00CAN fixed to integers one narrow-lane cycle apart from the two cold starts, after a single-signal refloat left the other signal held and nothing released the hold (re-traced in section 8; the slip account of section 7.2 is not supported). Rerun with a release for held pairs (section 8.6): B1 passes on 15 of 15 receiver-days, partly because the disputed pair stays float; B2 fails on the same 2 receiver-days with no position change above 0.05 mm; B4 fails on a half-cycle offset of the independent wide lane at STPM00SPM. The partner refloat did not see the post-fit screen's resets (a defect, fixed in 8.7). Second rerun (8.8): the partner refloat acts where it was built to, B1 passes on 15 of 15, but B2 now fails on 3 of 15 receiver-days (BAKE00CAN DOY 145 joins, 6.5 against 4.4 mm, a height offset of 4 to 6 mm after the refloats, same integers on every pair fixed in both runs) and B4 fails as before. Trace (8.9): no wrong integer; the earlier B2 pass at BAKE00CAN DOY 145 rested on a 5.2 mm jump from an inconsistent state, and without it the fixed height settles below the float as at STPM00SPM DOY 144, a common height offset that fixing exposes sooner. Offset traced (8.10, diagnostic): mostly the EOT20 grid ocean tide loading of the user configuration (SIF FES2014b loading is off in it, armed in the network's field configuration); with the SIF loading the mean absolute up error falls from 6.2 to 4.2 mm fixed and from 7.0 to 4.0 mm float, and B2 would fail on 6 of 15 receiver-days, mostly WTZZ00DEU. Continued in card [ARUB](ARUB-USER-FIXING-CONTROL-CONTINUED.md): the EOT20 loading of that comparison is phase-reversed (a defect), so it is no base for the WTZZ00DEU result; with the SIF diurnal translation removed the WTZZ00DEU fixed mean absolute up error is 2.9 mm. Open.

> Results are appended TO THIS FILE, below the text.

---

## 1. Why a post-processed control first

On the live streams nothing has fixed, and the float narrow lane holds constant non-integer offsets that
differ between a static and a kinematic engine on the same data. A stream mixes several suspects at once:
the correction source, its signal mapping, the time handling of the engine and the user models. Final
products with phase biases made for integer fixing remove the stream suspects. If the float is not integer
there, the fault is in the user engine's model or covariance; if it is, the streams are the next suspect.

## 2. Set-up

- Receivers: test receivers of the period 23 to 27 May 2026 (`D:\GnssProcessorSupport\presentation\agu26\figures\period2026_note.md`), first WTZZ00DEU (JAVAD, GPS L1C/L2W, Galileo L1X/L5X), BAKE00CAN (Septentrio, GPS L1C/L2W, Galileo L1C/L5Q) and STPM00SPM (Trimble, GPS L1C/L2W, Galileo L1X/L5X). Coordinates: the IGS daily mean of the period (`period2026\coords\test_coords_2026.json`), reduced from the marker to the antenna reference point with the RINEX `ANTENNA: DELTA H/E/N`.
- Products: `C:\GnssProcessor\data\2026-<doy>\products\COD0MGXFIN_*` (orbit 5 min, clock 30 s, OSB, ERP). The OSB file of DOY 145 holds phase biases for 32 GPS satellites on L1C, L1W, L2W (and L1L, L1X, L2L, L2S, L2X) and for 30 Galileo satellites on L1C, L1X, L5Q, L5X, one value per day.
- Engine: `config/ppp_rtk_internal.json` (case D of the period note) with GPS + Galileo, `frequency_mode` dual, `channel_mode` single, between-satellite differences, code and phase biases applied, accumulated wind-up, static position (`position_q` 0). Variant `est`: position estimated from 100 m. Variant `hold`: `receiver.reference_position_ecef` at the antenna reference point with an initial sigma of 0.1 mm. Generator `fixing_control\tools\make_ctl.py`.
- Grader `fixing_control\tools\grade_ctl.py`: per epoch and satellite pair against the resolver's reference satellite (highest elevation, 20 degree fixing mask), the wide lane W = n1 - n2 and the ionosphere-free narrow lane with W rounded, n1_hat = (f1 n1 - f2 n2 - f2 round(W)) / (f1 - f2), each with its distance to the nearest integer and its formal sigma from `CLCOV`.

## 3. Bars, frozen before the graded runs

**Stage A** (the float is integer within its covariance), per receiver-day, graded over minutes 60 to 120
of a 2 h run, per system (GPS, Galileo), the last value of each pair in each 10-minute window:

- **A1 wide lane:** median distance to the nearest integer at most 0.10 cycle.
- **A2 narrow lane:** median distance to the nearest integer at most 0.10 narrow-lane cycle (uniform reads
  0.25), and at least 75 % of the pair-windows within 0.20 cycle.
- **A3 covariance:** the median of (narrow-lane distance / its formal sigma) at most 2.0 (an honest
  covariance gives a half-normal ratio with median 0.67).

A pass needs A1 to A3 on the position-held run (the corrections are applied consistently) AND on the
position-estimated run (fixing happens with the position estimated). A held pass with an estimated fail
places the fault in the position and troposphere path or the covariance; a held fail places it in a model
term (bias sign or signal mapping, wind-up, attitude, antenna offsets per frequency, the clock reference of
the bias set), bisected one change per run.

**Stage B** (fixing on; run only when stage A passes):

- **B1 repeatability:** the same arcs fix to the same integers from two cold starts one hour apart.
- **B2 no harm:** with `hold_variance` 1e-7, the fixed position after convergence (minutes 60 to 360) is no
  worse than the float position at every receiver-day (median 3D error, same run settings otherwise).
- **B3 reported, not gated:** time to 10 cm and the first-hour error, float against fixed, for the 17 test
  receivers and 5 days.
- The two recorded stream sessions (`D:\GnssProcessorSupport\runs\rt_agu_2026-11-10\relay_cnes`,
  `relay_cas`) are graded on B1 and B2 after the post-processed stage passes, through a time-faithful replay.

A fix count is never evidence of correct integers; `ratio = inf` is a sentinel; the search is in cycles.

## 4. Stage A results (2026-11-10)

**Runs.** `D:\GnssProcessorSupport\runs_nom\_rap_aru_a1_{est,hold}_<doy>_<site>` (30 runs: WTZZ00DEU,
BAKE00CAN, STPM00SPM, DOY 143 to 147, 2 h from 00:00, all exit 0) and `_rap_aru_a1gal_est_<doy>_<site>` (15 runs,
the same with the search restricted to Galileo, `estimation.ambiguity_resolution.active_systems` ["Galileo"]).
Executable `D:\GnssProcessorSupport\gates\exe_aru\gnss_processor_aru.exe`, Release, sha256
4b8aae390a5a9733c1a354de78540327188664d48ded7e059104b169730bdf72, built on branch `ar/user-control` from main
`b6bbea7` plus the `CLCOV` trace line. Identity: `_rap_aru_idchk_main_145_wtzz` (the configuration of
`_rap_aru_a1_est_145_wtzz` on the main build `gnss_processor_p26.exe`, sha256 28e4286e...) gives byte-identical
`state_trace.csv`, `ambiguity_trace.csv` and `measurement_residuals.csv`; the resolver logs differ only by the
`CLCOV` lines. Grades: `D:\GnssProcessorSupport\presentation\agu26\fixing_control\grade_a1.{txt,json}` and
`grade_a1gal.{txt,json}` (`tools\agg_a1.py`, `tools\grade_ctl.py`).

**4.1 The bars.** Minutes 60 to 120, per system, pair-windows as frozen in section 3.

| variant | system | A1 wide lane | A2 narrow lane | A3 covariance | wide lane median, range | narrow lane median, range | miss / sigma median, range |
| --- | --- | --- | --- | --- | --- | --- | --- |
| position estimated | GPS | 13 of 15 | 15 of 15 | 9 of 15 | 0.037 to 0.127 | 0.046 (0.021 to 0.095) | 1.94 (0.60 to 3.54) |
| position estimated | Galileo | 13 of 15 | 15 of 15 | 6 of 15 | 0.006 to 0.115 | 0.045 (0.016 to 0.080) | 2.09 (0.58 to 3.80) |
| position held | GPS | 13 of 15 | 15 of 15 | 5 of 15 | 0.037 to 0.127 | 0.034 (0.017 to 0.082) | 2.53 (1.19 to 5.20) |
| position held | Galileo | 13 of 15 | 15 of 15 | 3 of 15 | 0.007 to 0.115 | 0.038 (0.008 to 0.089) | 3.74 (1.00 to 8.86) |

Receiver-days passing every bar on both systems: 5 of 15 with the position estimated (BAKE00CAN 144 to 147,
WTZZ00DEU 144), 2 of 15 held (BAKE00CAN 143, 147). A1 fails on WTZZ00DEU GPS on DOY 145 and 146 (0.109, 0.127:
constant offsets on single pairs, G25-G12 at -0.32 on DOY 143) and on STPM00SPM Galileo on DOY 144 and 147 (0.115,
0.102); not traced. Stage A is therefore not passed as frozen, and stage B was not started.

**4.2 The float is integer.** Over the last hour most pairs hold a constant fraction near zero (spread under 0.1
cycle and mean within 0.15, DOY 143, position estimated: WTZZ00DEU 5 of 6 GPS and 6 of 6 Galileo pairs, BAKE00CAN
11 of 12 and 6 of 6, STPM00SPM 11 of 11 and 5 of 6).
Static 3D position error in hour 2, median 1.17 cm (0.44 to 1.99 cm). On these products the corrections are applied
consistently: bias signs and signal mapping, wind-up, attitude and antenna offsets per frequency leave no
narrow-lane offset of 0.1 cycle or more. Holding the position at the IGS coordinate lowers the narrow-lane miss only
from 0.045 to 0.034 cycle, so the position and troposphere path is not the binding term either.

**4.3 Why the formal sigma is below the miss.** Median over the 30 runs and both systems per 10-minute window:

| window (min) | 0-10 | 20-30 | 40-50 | 60-70 | 80-90 | 110-120 |
| --- | --- | --- | --- | --- | --- | --- |
| estimated: narrow-lane miss | 0.198 | 0.137 | 0.062 | 0.047 | 0.055 | 0.051 |
| estimated: formal sigma | 0.302 | 0.085 | 0.054 | 0.036 | 0.026 | 0.022 |
| held: narrow-lane miss | 0.075 | 0.061 | 0.034 | 0.030 | 0.038 | 0.033 |
| held: formal sigma | 0.038 | 0.016 | 0.012 | 0.011 | 0.010 | 0.010 |

The miss stops falling after about 40 minutes at about 0.045 narrow-lane cycle (5 mm) with the position estimated
and 0.031 (3 mm) held, while the formal sigma keeps shrinking as if every 30 s epoch brought independent
information. The per-epoch weights are not too tight: the post-fit phase residuals are below their stated sigma
(standardized RMS 0.38 to 0.56 in hour 2 at the three receivers on DOY 143, `tools\resid_scale.py`). The optimism is
the time-correlated part of the phase error (multipath, troposphere and receiver antenna mismodelling, orbit and
clock residuals), which the filter carries with no process noise on the ambiguities (`ambiguity_q` 1e-12) and almost
none on the troposphere (1e-9). The wide-lane sigma is honest (miss 0.05 to 0.07, sigma 0.05). An honest narrow-lane
covariance needs a floor near 0.04 cycle, a factor of about 2 to 3 over the formal sigma in hour 2.

**4.4 The first model term that prevents a correct fix: the MW wide lane omits the satellite antenna offset per
frequency.** With .BIA products the resolver takes the WL-constrained route: it imposes the MW detector's
double-differenced wide-lane integer (`WLDUMP dd_wl`) and searches only the narrow lane. That integer was compared
with the float states' wide lane (`CLFRAC` n1 - n2) of the same pair at the same epoch (`tools\agg_a1.py`):

| pairs | position estimated | position held |
| --- | --- | --- |
| GPS, one satellite of Block III | 5 526 of 5 526 disagree | 6 148 of 6 148 |
| GPS, other | 0 of 7 109 | 0 of 7 852 |
| Galileo | 132 of 13 338 | 132 of 14 495 |

- The disagreement is exactly 2 cycles, of the sign of the Block III satellite's side (G13 against G12 at
  WTZZ00DEU on DOY 143: float 4.016, MW integer 2 at every epoch). A direct computation from the RINEX file and the
  OSB file, with no engine involved (`tools\mw_direct.py`), gives G13 - G12 = +1.741 cycles: the raw MW agrees with
  the detector to its rounding and differs from the float by 2.275 cycles.
- Cause: `igs20.atx` gives the Block III satellites a z offset of 1.116 m on L1 and 0.612 m on L2 (G13 and G20,
  SVN G083 and G082; G23 1.184 and 0.707 m); the other GPS blocks have one value for both. The float model applies
  the offset per frequency to code and phase; the MW detector works on raw observables
  (`MwSlipDetector::computeSatMwBias` corrects only the four OSBs). In the MW combination a difference d between the
  L1 and L2 offsets enters as (f1/(f1-f2) - f1/(f1+f2)) d = 3.97 d: 3.97 x 0.504 m = 2.0 m, 2.3 wide-lane cycles of
  0.862 m at a nadir angle near 10 degrees, as measured.
- Galileo: the same term at a smaller size. E21 has an E1 minus E5a offset of 177 mm, E28 and E29 51 and 53 mm; the
  125 mm difference is 0.66 Galileo wide-lane cycle, which flips the rounded integer on E28 - E21 and E29 - E21 at
  WTZZ00DEU on DOY 144 (132 pair-epochs, -1 cycle).
- Effect on the search: a wide lane wrong by 2 cycles moves the narrow-lane target by 2 f2/(f1 - f2) = 7.06 cycles,
  a fraction of only 0.06, so the narrow-lane search cannot see it. A fix through this route sets the L1 integer of
  the Block III pair wrong by 7 cycles and the L2 integer by 5 cycles, while the ionosphere-free ambiguity moves by
  only 0.06 narrow-lane cycle (6 mm): wrong integers the position barely shows, absorbed by the slant ionosphere.
  With every GPS pair in the search, the ratio reached 3.0 in 10 of 30 runs (maximum 16.5); the later gates (success
  rate, per-pair cap) did not run at the 1e9 threshold, so whether such a fix would have been accepted is not known.
- Without Block III pairs (Galileo only) the search ratio exceeds 3.0 in 537 of 1 790 hour-2 searches and at some
  epoch in 12 of 15 runs (maximum 5 206 at WTZZ00DEU on DOY 147). Whether those integers are correct is the
  question of stage B.

**4.5 What stage B needs, in order.** (1) A wide-lane source consistent with the float model: the MW detector
corrected for the satellite (and receiver) antenna offset per frequency, or the wide lane taken from the float
states, default off and identical when off. (2) A narrow-lane covariance with an error floor, or the success-rate
gate evaluated on an inflated covariance. (3) The B1 and B2 bars of section 3, unchanged. Not done here: stage B, the
stream sessions and their replay, the 14 other test receivers, the CAS Galileo signal mapping, a wide-lane source
on stream biases, the Debug unit suite on this branch (the change is one debug log line).

## 5. Stage B set-up, frozen before the graded runs (2026-11-11)

**Entry.** Stage A did not pass as frozen (bar A3, section 4.1). On 2026-11-11 the author decided to build the two
changes of section 4.5 and to run stage B. With the floor of 0.04 cycle added to the formal narrow-lane sigma, A3
reads 14 of 15 (estimated, GPS), 15 of 15 (estimated, Galileo), 15 of 15 (held, GPS) and 14 of 15 (held, Galileo),
median miss over sigma 0.89, 1.01, 0.82 and 0.88 (`fixing_control\grade_a3_floor004.{txt,json}`,
`tools\a3_floor.py`). This is not a pass of A3: the floor was chosen from the same runs (section 4.3). Stage B is
the test.

**Code** (branch `ar/user-control`; both keys default off, ledger rows UNVALIDATED):

- `estimation.ambiguity_resolution.wl_from_float`: the WL-constrained search rounds the float states' DD wide lane
  w = DD_1/lambda_1 - DD_2/lambda_2 instead of imposing the MW detector's integer. A candidate enters only if
  |w - round(w)| + 3 sigma_w <= `wl_max_frac` (0.40, the form the MW path uses), sigma_w from the filter
  covariance. The route also runs with no MW integer (no .BIA file, as on streams). Log line `WLFLOAT` per
  candidate.
- `estimation.ambiguity_resolution.nl_sigma_floor_cyc`: the searched N_1 covariance becomes
  Q + f^2/2 (I + M), M the block of candidates sharing a reference satellite (each DD gains f^2, DDs on one
  reference correlate by f^2/2). The ratio test, the bootstrap success rate, the per-pair cap and the partial
  search all use it; the filter and the fix's pseudo-observation do not.
- `src/core/filter/AmbiguityResolver.cpp` (`floatWidelane_`, `addSharedReferenceFloor_`), config plumbing,
  `tests/test_ar_float_widelane.cpp` (suite `ArFloatWidelaneTest`), two fields in
  `tests/test_ambiguity_resolver_setup.cpp`.
- Identity with both keys absent: `_rap_aru_idchk_arub_145_wtzz` (new executable) against
  `_rap_aru_a1_est_145_wtzz` (stage A executable): `state_trace.csv`, `ambiguity_trace.csv` and
  `measurement_residuals.csv` byte-identical, resolver log identical apart from time stamps.
- Executable `D:\GnssProcessorSupport\gates\exe_arub\gnss_processor_arub.exe`, Release, sha256
  21caae183dd496d9a14bf5ac783f6b0f909a25fa401f6dd7319430044c123dac.

**Runs** (`fixing_control\tools\make_ctl.py`, variant `est`, 6 h, static, GPS + Galileo):

- Fixed, `_rap_aru_b6fix_est_<doy>_<site>`, from 00:00: the stage A estimated configuration with
  `ratio_threshold` 5.0 (the base configuration's value, written out because the generator's stage A default is
  1e9), `hold_variance` 1e-7, `wl_from_float` true, `nl_sigma_floor_cyc` 0.04, `nl_iono_free_row` true (the row
  graded in stage A), `enable_partial_ar` true. Unchanged: `min_success_rate` 0.999, `nl_max_per_pair_frac` 0.15,
  `ratio_upper_bound` 1e4, `min_epochs` 10, `max_position_sigma_m` 0.05, fixing elevation mask 20 degrees, resolver
  log at debug.
- Float, `_rap_aru_b6flt_est_<doy>_<site>`: the same with fixing `mode` 0.
- Late, `_rap_aru_b6late_est_<doy>_<site>`: the fixed configuration on a copy of the RINEX file without the epochs
  before 01:00 (in the run folder, same file name), 6 h.
- Order: the three stage A receivers on the 5 days first. The 14 other test receivers run only if those 15
  receiver-days pass B1 and B2; otherwise stage B has failed and the rest is not run.

**Grading** (`fixing_control\tools\grade_b.py`):

- B1: the fixed integers (L1 and L2) of the run from 00:00 and the run from 01:00, per satellite pair and arc. An
  arc ends where either satellite's ambiguity is re-seeded in either run (formal sigma up by more than a factor 2
  above 5 cm, or a gap over 60 s, in `ambiguity_trace.csv`). The late run's integers are re-expressed against the
  early run's reference satellite through the same epoch's fix when the two differ. Pass: at least one pair-arc
  compared and none fixed to different integers in the two runs, or to two values in one run.
- B2: median 3D error against the antenna reference point over minutes 60 to 360, fixed run at most equal to the
  float run, at every receiver-day. No tolerance was frozen in section 3, so none is applied; the difference is
  reported in mm.
- B3 (reported): time to 10 cm (the first epoch after which the 3D error stays below 0.10 m to the end of the
  run) and the 3D error at minute 60, fixed and float.
- Fix events: the per-pair lines of an accepted full-set fix (`NL pair ... N_1_fixed= dd_wl=`) or partial fix
  (`[PAR] NL pair ...`) at debug, kept only when the same epoch logs a success and no upper-bound rejection.

**Mechanics check before freezing.** No spare day carries both observations and a bias file, so one receiver-day
of the graded set was used, WTZZ00DEU on DOY 143, 2 h: `_rap_aru_bdev_est_143_wtzz`, `_rap_aru_bdev1h_est_143_wtzz`,
`_rap_aru_bdevflt_est_143_wtzz` (`fixing_control\grade_bdev.json`). Its purpose was to see fixes accepted, logged
and parsed. The first attempt still carried the stage A threshold 1e9 (0 fixes); the threshold was then written
out as 5.0. Result, not graded: first fix at 16.5 minutes in both runs, 18 pair-arcs compared, none different;
median 3D error over minutes 60 to 120, 8.1 mm fixed and 13.6 mm float. No setting was changed in response to these
numbers.

## 6. Corrections to section 4 (2026-11-11)

An independent re-check of stage A by another session (a separate parser on the run logs) reproduced the numbers of
section 4 and raised the points below. Each was checked here against the logs, `igs20.atx` and the code before it was
written. Section 4 stays as written; this section governs where they differ.

1. **The stage A searches ran on the narrowlane row, not the ionosphere-free row.** `main_log.log` of
   `_rap_aru_a1_est_143_wtzz` and `_rap_aru_a1gal_est_143_wtzz` reads `nl_row=narrowlane` (`nl_iono_free_row`
   defaults to false). The grade of sections 4.1 to 4.3 is on the ionosphere-free combination of the float states,
   so "the float is integer" holds for that combination only. On the narrowlane row the search used, with the float
   wide lane rounded, the hour-2 median miss is 0.264 cycle for GPS and 0.186 for Galileo (`_rap_aru_a1_est_*`, 447
   and 483 pair-windows; the re-check: 0.27 and 0.19 against formal sigma 0.18 and 0.16). Consequences:
   - the search ratios of stage A (section 4.4: the GPS ratio counts, 537 of 1 790 Galileo-only searches) come from
     a row the float does not support; they are no sign of fixable integers;
   - on the narrowlane row a 2-cycle wide-lane error moves the N_1 target by 2 x 0.438 = 0.876 cycle; a fix sets L1
     and L2 one cycle off in opposite directions and the ionosphere-free ambiguity one wide-lane wavelength (0.862
     m) off. The "7 and 5 cycles, millimetres in the position" of section 4.4 holds for the ionosphere-free row only;
   - stage B searches on the ionosphere-free row (`nl_iono_free_row` true), frozen in section 5 (commit `eb50158`)
     before any graded run, so the floor of 0.04 cycle and the 0.15 per-pair cap act on the row that was graded.
2. **The float wide lane is not an independent source.** It carries no antenna omission (the re-check: float
   fraction 0.043 on GPS III pairs), but it shares the float's ionosphere error (1.49 wide-lane cycles per metre of L1
   slant error) and would hide any term per frequency the float model gets wrong. A wide lane from the observations
   and the bias file with the satellite and receiver antenna offsets per frequency projected on the line of sight is
   an independent witness; it is reported with stage B where computed, not graded (it was not in the frozen bars).
3. **Ratio counts, section 4.4.** The WL-constrained ratio reached 3.0 at some epoch in 13 of 30 runs (5 estimated,
   8 held; `grade_a1.json`, maximum 16.54 at `_rap_aru_a1_hold_144_bake` minute 5), not 10 of 30. In hour 2 alone:
   5 of 30 runs, 50 of 3 600 searches.
4. **Which GPS III pairs.** The wide-lane disagreement is on pairs with exactly one GPS III satellite. Pairs of two
   GPS III satellites agree (307 pair-epochs estimated). The rounded float minus the MW integer is +2 when the first
   satellite is GPS III (3 944 pair-epochs) and -2 when the reference is (1 582).
5. **Galileo size.** The 125 mm difference of E1 minus E5a offsets between E21 and E28 is 0.56 Galileo wide-lane
   cycle (coefficient f1/(f1-f5) - f1/(f1+f5) = 3.38, wavelength 0.751 m), not 0.66 (the GPS coefficient was used).
   Between a satellite with nearly equal offsets and a large-offset one it reaches 0.71 cycle (E11 at 19.9 mm against
   E21 at 177.0 mm, `igs20.atx`), so on Galileo the term can round either way, not only as a smaller GPS case.
6. **Galileo-only search, whole run against hour 2.** "At some epoch in 12 of 15 runs" and "maximum 5 206" are over
   the whole run; the maximum is at minute 50.5 (`_rap_aru_a1gal_est_147_wtzz`). In hour 2 the ratio passed 3.0 in 11
   of 15 runs (537 of 1 790 searches, unchanged). By point 1 these are narrowlane-row ratios.

## 7. Stage B results, first three receivers (2026-11-11)

**Runs.** `D:\GnssProcessorSupport\runs_nom\_rap_aru_b6{fix,late,flt}_est_<doy>_<site>`, WTZZ00DEU, BAKE00CAN and
STPM00SPM on DOY 143 to 147, 45 runs, all exit 0, executable `gnss_processor_arub.exe` (section 5). Grades:
`D:\GnssProcessorSupport\presentation\agu26\fixing_control\grade_b6_phase1.json` (`tools\grade_b.py`), wide-lane
witness `witness_b6_phase1.jsonl` (`tools\witness_wl.py`). Every fixed run searched on the ionosphere-free row
(`nl_row=iono-free` in `main_log.log`).

**7.1 The bars.** Stage B fails as frozen: B1 fails on 1 of 15 receiver-days, B2 on 2 of 15. By section 5 the 14 other
test receivers were not run, and the stream sessions were not replayed.

| receiver | DOY | B2 median 3D, min 60-360, fixed / float (mm) | B2 | B1 pair-arcs compared, different | B1 | time to 10 cm fixed / float (min) | 3D at min 60 fixed / float (mm) |
| --- | --- | --- | --- | --- | --- | --- | --- |
| BAKE00CAN | 143 | 7.8 / 7.6 | fail (+0.2) | 66, 0 | pass | 17.0 / 17.0 | 8.3 / 26.4 |
| BAKE00CAN | 144 | 5.6 / 8.5 | pass | 61, 0 | pass | 9.5 / 9.5 | 5.7 / 24.2 |
| BAKE00CAN | 145 | 4.2 / 4.4 | pass | 72, 1 | fail | 14.5 / 14.5 | 6.4 / 12.3 |
| BAKE00CAN | 146 | 6.4 / 7.0 | pass | 68, 0 | pass | 17.0 / 10.5 | 2.0 / 24.8 |
| BAKE00CAN | 147 | 4.6 / 7.6 | pass | 59, 0 | pass | 13.0 / 13.0 | 4.4 / 16.6 |
| STPM00SPM | 143 | 10.4 / 16.4 | pass | 68, 0 | pass | 12.0 / 12.0 | 11.0 / 12.4 |
| STPM00SPM | 144 | 19.9 / 14.2 | fail (+5.8) | 80, 0 | pass | 8.5 / 8.5 | 13.4 / 3.8 |
| STPM00SPM | 145 | 13.3 / 13.4 | pass | 71, 0 | pass | 15.5 / 15.5 | 17.2 / 24.6 |
| STPM00SPM | 146 | 7.3 / 10.9 | pass | 64, 0 | pass | 24.5 / 24.5 | 10.4 / 12.4 |
| STPM00SPM | 147 | 9.4 / 12.1 | pass | 53, 0 | pass | 15.0 / 15.0 | 10.8 / 10.0 |
| WTZZ00DEU | 143 | 3.6 / 6.5 | pass | 70, 0 | pass | 11.0 / 11.0 | 14.4 / 10.8 |
| WTZZ00DEU | 144 | 3.3 / 6.6 | pass | 54, 0 | pass | 18.0 / 18.0 | 20.8 / 13.4 |
| WTZZ00DEU | 145 | 5.9 / 12.9 | pass | 66, 0 | pass | 6.5 / 28.5 | 7.9 / 13.8 |
| WTZZ00DEU | 146 | 11.8 / 13.8 | pass | 31, 0 | pass | 9.5 / 9.5 | 26.7 / 10.9 |
| WTZZ00DEU | 147 | 11.5 / 15.3 | pass | 21, 0 | pass | 10.5 / 10.5 | 9.0 / 27.1 |

- Fixing works mechanically: the first fix comes at minute 16.0 to 29.0 in the runs from 00:00 (16.5 to 19.5 minutes
  after the start in the runs from 01:00), soon after the position-sigma gate opens. The fixed median 3D error over
  minutes 60 to 360 is lower than the float on 13 of 15 receiver-days (median over the 15: 7.3 mm fixed, 10.9 mm
  float).
- B1: 904 pair-arcs compared over the 15 receiver-days, 1 fixed to different integers. Checked on the raw fix lines
  without re-expression, exactly one pair-arc in the 30 fixed runs took two values: G06-G31 in the BAKE00CAN DOY 145
  run from 01:00. The two counts of "two values" that the grader lists for that run from 01:00 on DOY 143 come from
  re-expression through a reference pair and are not in the raw lines.
- B3 is reported for the 3 receivers only (the 17 were not run). Time to 10 cm is the same, fixed and float, on 13
  of 15 receiver-days. On 14 of 15 the fixed run's error is below 10 cm for good before its first fix, so fixing
  cannot shorten it with this position-sigma gate.

**7.2 The B1 failure: a held integer carried through a disturbed arc.** BAKE00CAN (64 degrees north), DOY 145, run
from 01:00. At 04:10:30 the full set of 11 pairs fixes (ratio 40.2) and G06-G31 takes N1 0, wide lane 4 (float
-0.017). From the next epoch the same pair reads 0.20 cycle from its held integer, and the per-pair cap rejects each
later full set for 10 minutes (partial fixes without that pair are accepted) while its hold stays in the state
(`hold_variance` 1e-7). At 04:21 the pair re-fixes
to N1 -1, wide lane 4: L1 and L2 both one cycle lower, one narrow-lane cycle in the ionosphere-free ambiguity. The
run from 00:00 kept N1 0. Between 04:10 and 04:14 the geometry-free combination of G31 (L1W - L2W) steps by 2 to 8
cm per 30 s epoch, with no loss-of-lock flag in the RINEX file, no slip flagged by the engine and no re-seed in
`ambiguity_trace.csv`. A (1, 1) slip keeps the wide lane, so the MW test cannot see it, and its geometry-free step
(-5.4 cm) hides among steps of that size. Which of the two integers is right is not known. The term: a fixed and
held integer has no release when the float contradicts it beyond the per-pair cap, and no slip test that can see a
wide-lane-preserving slip under disturbed ionosphere (standing invariant 1).

**7.3 The B2 failures.** STPM00SPM DOY 144: the fixed run is 4 to 9 mm worse than the float in each hour from hour
2. Its mean up error is -12.1, -19.8 and -19.8 mm in hours 2, 4 and 6; the run from 01:00 reads -20.8, -22.7 and
-18.9; the float -2.0, -13.2 and -15.6. The two fixed runs agree with each other, the wide-lane witness agrees with
every clear window, and the float drifts toward the same height. The fixed solution reaches a common height offset
of about 2 cm earlier than the float; its source (the reference coordinate, the receiver antenna model or the
troposphere) is not traced. BAKE00CAN DOY 143: 0.2 mm worse, equal from hour 3 (8.9 / 8.8, 7.8 / 7.5, 7.5 / 7.2,
7.7 / 7.5 mm); a tie that the strict bar fails.

**7.4 Independent wide-lane witness** (section 6 point 2, reported, not graded). Over the 30 fixed runs, minutes 60
to 420: 3 027 clear GPS and 3 216 clear Galileo pair-windows (witness steady and near an integer, one fixed value);
1 GPS window more than 0.5 cycle from the fixed wide lane (G12-G28, STPM00SPM DOY 146 run from 01:00, witness
-150.344, fixed -151, window sd 0.22); 481 GPS and 349 Galileo windows unclear. Median |witness - fixed| per run and
system 0.041 to 0.173 cycle. Control: without the antenna terms the witness reads +1.7 on G13-G12 at WTZZ00DEU against a
fixed 4 (`_rap_aru_bdev_est_143_wtzz`), the stage A error. The fixed wide lanes are independently confirmed. The
narrow lane has no independent check here: B1 tests repeatability, not truth.

**7.5 Not done.** The 14 other test receivers (B1, B2, B3), stopped by the rule of section 5. The stream replays:
`realtime_tools\replay_relay.py` is written (time-faithful, per-stream ports, a later engine connects as a cold
start) but not run, and no stream configuration with the two keys exists. The CAS Galileo signal mapping. A slip
test or release rule for held integers (7.2). The STPM00SPM height offset (7.3).

**7.6 Unit suite.** Debug `gnss_tests.exe` at commit `eb50158`: 4 261 tests in 562 suites, 4 224 passed, 37 skipped,
0 failed (`fixing_control\gtest_debug_eb50158.txt`), the new suite `ArFloatWidelaneTest` included.

## 8. Stage B rerun with a release for held pairs, set-up frozen before the graded runs (2026-11-12)

**8.1 Correction to section 7.2: the B1 failure re-traced.** Section 7.2 stays as written; this paragraph governs
where they differ. Sources: `ambiguity_trace.csv`, `kalman_filter.log`, `main_log.log` and the resolver log of
`_rap_aru_b6fix_est_145_bake` (from 00:00) and `_rap_aru_b6late_est_145_bake` (from 01:00), and the independent
wide lane of section 7.4 per epoch (`tools\witness_wl.py --dump-pair G06-G31`).

- The independent wide lane of G06-G31 reads about 3.0 until 03:31, about 4.0 from 03:32 (the GPS pivot of both runs
  is handed over twice at 03:32:00, G28 to G03 to G25), about 3.0 from 03:37:30 to 03:40:00 (2.68 to 3.47) and about 4.0
  again from 03:40:30 to the end. The pair's wide lane changed three times in nine minutes.
- At 03:37:00 the post-fit screen of both runs flagged the L1W rows of G04, G31 and G03 against the pivot G25 and
  refloated G31 L1W alone; G31 L2W kept a sigma of 0.45 mm. One epoch later G31 L1W re-settled 0.155 m from its
  old value at a 3.3 mm sigma. A one-cycle slip on L2 that the free slant ionosphere carries into L1 moves the L1
  ambiguity by lambda_2 / gamma = 0.148 m; the wide-lane drop at 03:37:30 is consistent with such a slip.
- Both runs fixed G06-G31 to wide lane 3, N_1 0 (03:38:00 and 03:38:30), consistent with the witness at that time.
- At the pivot handovers of 03:41 (in the run from 01:00, G31 to G03 at 03:41:00 and G03 to G28 at 03:41:30) the
  runs diverge. The run from 00:00 refloats both G31 signals (sigma 1000 m on L1W and L2W at 03:41:00); they reconverge
  on the code and the pair fixes wide lane 4, N_1 0 at 03:47:00. The run from 01:00 refloats G31 L1W only; G31 L2W
  goes from 0.09 m to a 0.2 mm sigma in one epoch, still carrying the wide lane 3 of 03:38:30. Its float wide lane
  then reads 3.74 to 3.87 at sigma 0.005 to 0.017 from 03:41:30 to 04:10:00 (`WLFLOAT`), 0.7 to 0.9 cycle from the
  held integer, and nothing releases the hold.
- At 04:10:30 the run from 01:00 fixes the full set again with wide lane 4, N_1 0. The ambiguity states move only
  part of the way (DD L1 from +0.447 to +0.040 cycle, DD L2 from -3.420 to -3.607 cycle, `ambiguity_trace.csv`),
  the next search reads the pair 0.8 cycle from N_1 0, the per-pair cap rejects each full set for 10 minutes, and at
  04:21:00 the pair re-fixes to N_1 -1, wide lane 4.
- The (1, 1) slip of G31 at 04:10 proposed in section 7.2 is not supported: in the run from 00:00 the pairs G03-G31,
  G04-G31 and G26-G31 keep their integers within 0.01 cycle through 04:08 to 04:30.

Two terms, in order: (a) a single-signal refloat by the post-fit screen or the pivot handover, while the
satellite's other signal stays held, re-determines the refloated ambiguity through the ionosphere from the kept
signal and gives a wide lane that is neither integer nor honestly uncertain (3.74 to 3.87 at sigma 0.005); (b) a
held pair has no release when the float contradicts it.

**8.2 Correction to section 7.4.** The witness grade of section 7.4 counted 10-minute windows with one fixed value
only, so a window in which a pair fixed two different wide lanes was "unclear" and never compared. The rerun adds a
per-fix grade (B4, 8.5). On the same 30 runs it is reported with the rerun, as the baseline.

**8.3 Code** (branch `ar/user-control`; both keys default off, ledger rows UNVALIDATED):

- `estimation.ambiguity_resolution.hold_release_cyc` (default 0, off): the resolver remembers the integers (N_1 and
  the wide lane) of every pair an accepted WL-constrained fix held. Each epoch the search runs it compares them
  with the float state: the wide lane DD_1/lambda_1 - DD_2/lambda_2 against the held one, and the
  ionosphere-free combination against the held one in narrow-lane cycles. Either difference above the threshold
  contradicts the hold (log `HOLDREL`). The satellite refloated is the reference when at least two of its held
  pairs and more than half of them contradict, otherwise the pair's non-reference satellite; all its ambiguities
  are reset to the initial ambiguity sigma after the epoch's fix is installed, its pairs sit out that epoch's search
  and their holds are forgotten. A hold is also forgotten when an arc of either satellite on either signal ends or
  begins.
- `estimation.ambiguity_resolution.refloat_partner_signal` (default false): an arc that begins on one signal of a
  satellite (new key or refloat) while another signal of that satellite has carried on for at least three epochs
  refloats all its signals (log `PARTNERREL`). The three-epoch bar keeps the rule from answering its own refloat.
- `min_arc_info` now gates on both signals of a pair (it read the higher frequency only). It is 0 in every
  configuration of this card and in the base configuration, so no run changes.
- Run totals: the `AR release` line of `main_log.log` (satellites and keys refloated, run totals of both rules).
- `src/core/filter/AmbiguityResolver.cpp` (`refreshArcAges_`, `resolve` / `resolveCore_`, the release pre-pass of
  `resolveWlConstrained`, `holdMisfit_`, `chooseReleaseSatellites_`, `refloatSatellites`),
  `src/core/app/PppEpochUpdate.cpp` and `src/core/realtime/RealTimeEngineEstimate.cpp` (apply the refloat after the
  fix), config plumbing, `tests/test_ar_hold_release.cpp` (suite `ArHoldReleaseTest`, 6 cases), two fields in
  `tests/test_ambiguity_resolver_setup.cpp`.
- Identity with both keys absent: `_rap_aru_idchk_aruc_145_bake` (new executable) against
  `_rap_aru_b6fix_est_145_bake` (section 5 executable, same configuration): `state_trace.csv`, `ambiguity_trace.csv`
  and `measurement_residuals.csv` byte-identical, resolver log identical.
- Executable `D:\GnssProcessorSupport\gates\exe_aruc\gnss_processor_aruc.exe`, Release, sha256
  7e57946d7c97c05166f2f8f421012abba2a466b78020a29ac1e9d87cc0de6523.

**8.4 Runs** (`fixing_control\tools\make_ctl.py`, variant `est`, 6 h, static, GPS + Galileo): the fixed and late
configurations of section 5 with `hold_release_cyc` 0.25 and `refloat_partner_signal` true, nothing else changed:
`_rap_aru_b7fix_est_<doy>_<site>` (from 00:00) and `_rap_aru_b7late_est_<doy>_<site>` (from 01:00). The float runs
of section 7 (`_rap_aru_b6flt_est_<doy>_<site>`) are the float reference: with fixing off neither key acts, and the
identity check above covers the executable. Order as in section 5: WTZZ00DEU, BAKE00CAN and STPM00SPM on DOY 143 to
147 first; the 14 other test receivers only if those 15 receiver-days pass B1, B2 and B4.

**8.5 Bars and grading**, frozen here:

- B1, B2 and B3 exactly as in section 5 (`tools\grade_b.py`), B2 against the section 7 float runs.
- B4, new: no fix event over minutes 0 to 360 of any fixed run (from 00:00 and from 01:00) whose independent wide
  lane is clear and more than 0.5 cycle from the fixed wide lane. Clear: at least 10 witness epochs within 300 s of
  the fix epoch with a standard deviation under 0.25 cycle. Witness as in section 7.4 (RINEX, OSB, satellite and
  receiver antenna offsets per frequency on the line of sight). Tool: `tools\witness_wl.py --per-fix --from-min 0
  --to-min 360`. Mechanics check before freezing: on `_rap_aru_bdev_est_143_wtzz` the grade finds 0 disagreeing
  events with the antenna terms and 15 events in 5 episodes, all on pairs with one GPS III satellite, without them
  (the stage A error); on the two BAKE00CAN DOY 145 runs of section 7 it finds 0 (the 03:38 events fall in windows
  that span the wide-lane changes and are not clear).
- Reported: per run, the satellites refloated by each rule (the run totals of `AR release`). A rule that never acts
  on the 30 runs is reported as inert, not as passing.
- A fix count is never evidence of correct integers. B1 tests repeatability, B4 the wide lane; the narrow lane
  still has no independent check.

**8.6 Results of the rerun (2026-11-12).** Runs `D:\GnssProcessorSupport\runs_nom\_rap_aru_b7{fix,late}_est_<doy>_<site>`,
30 runs, all exit 0 (18 of them relaunched after the batch process was stopped by a session restart before they
started; none had begun). Grades: `fixing_control\grade_b7.json` (`tools\grade_b.py b7fix b6flt b7late`),
`fixing_control\witness_b4_b7.jsonl`; baseline B4 on the section 7 runs `fixing_control\witness_b4_phase1.jsonl`.
Stage B fails as frozen again: B2 on 2 of 15 receiver-days and B4 on 8 of 30 runs. B1 passes on 15 of 15.

- B1: 901 pair-arcs compared, none fixed to different integers. At BAKE00CAN DOY 145 the release acted at 03:32,
  03:37 and 03:40:30 (pairs G03, G04, G06 and G32 against G31, then G06 against G31 and G06 against G28; G31, then
  G06 twice refloated). In the run from 00:00 G06-G31 refixed to N_1 0, wide lane 4 at 03:47:00 and kept it; in the
  run from 01:00 G06 was never fixed again: its float wide lane against G31 stays at 4.14 to 4.15 (sigma 0.03 to
  0.08) to the end of the run, admitted, but in no accepted set. The pass is real but partly by absence: the pair that
  differed in section 7 is not compared, 70 pair-arcs at BAKE00CAN DOY 145 against 72 in section 7.
- B2: unchanged from section 7. BAKE00CAN DOY 143 7.8 against 7.6 mm (+0.2), STPM00SPM DOY 144 19.9 against 14.2 mm
  (+5.7). Median over the 15 receiver-days 7.3 mm fixed, 10.9 mm float; fixed better on 13 of 15. No receiver-day's
  fixed median moved by more than 0.05 mm from section 7.
- B3: time to 10 cm and the 3D error at minute 60 as in section 7.1 to 0.1 mm and 0.5 minute.
- B4: 24 disagreeing fix events in 8 runs, exactly the 24 events of the section 7 runs. 23 sit 0.50 to 0.56 cycle
  from the fixed wide lane (the witness near a half cycle: STPM00SPM in 7 runs, WTZZ00DEU G28-G25 in 1); one sits at
  0.655 (G12-G28, STPM00SPM DOY 146 run from 01:00, witness -150.345, fixed -151, sd 0.214, the case of section 7.4).
  The bar sits on a witness offset of about half a cycle on some STPM00SPM pairs, which the mechanics check of 8.5
  (BAKE00CAN and WTZZ00DEU only) did not show. B4 as frozen fails; it does not show a wrong fixed wide lane beyond
  the one 0.655 case.
- Telemetry (`AR release`, run totals): the hold release acted in 12 of 30 runs (39 satellites refloated: 21 in the
  runs from 00:00, 18 in the runs from 01:00), mostly at pivot handovers and slips where the misfit is several cycles
  (03:32 at BAKE00CAN: d_if 3.1 to 3.4 cycles); the partner refloat in 8 runs (14 satellites), all at STPM00SPM.
- Defect in the build of 8.3: the partner refloat never acts on the post-fit screen's resets, the case it was built
  for. `KalmanOutlierResponse.cpp` records its single-row reset with `noteAmbiguityDiscontinuity` (a generation
  bump), not with the pending-reseed mark, and `refreshArcAges_` restarts an arc only on that mark. The same blind
  spot holds for `min_arc_info` and for forgetting a hold. At BAKE00CAN it acted 0 times in both runs. A fix keys the
  arc restart on the ambiguity generation (`StateManager::getAmbiguityGeneration`); generations also move on a
  pivot re-expression, so that change needs its own identity and rerun.
- Unit suite at `a95e535`, Debug: 4 267 tests in 563 suites, 4 230 passed, 37 skipped, 0 failed
  (`fixing_control\gtest_debug_aruc.txt`).
- Not done: the 14 other test receivers (stopped by the rule of section 5), the stream replays, the generation-keyed
  arc restart, the STPM00SPM height offset and the half-cycle witness offset at STPM00SPM.

**8.7 Second rerun, set-up frozen before the graded runs (2026-11-12).** Entry: the author decided on 2026-11-12
to fix the defect of 8.6 and rerun.

- Code: the two release rules now see every discontinuity of an ambiguity through its continuity generation
  (`StateManager::getAmbiguityGeneration`). `refreshArcAges_` keeps, per key and only while a release key is on,
  the generation seen last epoch and the epochs since it last changed (`key_gen_`). Partner refloat: a key whose
  generation changes this epoch (birth, a refloat with the pending-reseed mark, or a `noteAmbiguityDiscontinuity`
  of the post-fit screen) while another key of the satellite has kept its generation for at least three epochs.
  A hold is forgotten when any key of either satellite changes generation or leaves the state. `arcs_`, the
  `ARCAGE` line and `min_arc_info` are unchanged. Three cases added to `ArHoldReleaseTest` (9 in all): a
  discontinuity recorded with `noteAmbiguityDiscontinuity` alone triggers the partner refloat, which then stays
  quiet; off requests nothing; `refloatSatellites` resets only the listed satellite.
- Identity with both keys absent: `_rap_aru_idchk_arud_145_bake` against `_rap_aru_b6fix_est_145_bake`: `state_trace.csv`,
  `ambiguity_trace.csv` and `measurement_residuals.csv` byte-identical, resolver log identical.
- Executable `D:\GnssProcessorSupport\gates\exe_arud\gnss_processor_arud.exe`, Release, sha256 97fa08fa9cd66906bc8080fbb52adb645c8c2ea7b2ee53dbafc12ba321723d80.
- Runs: `_rap_aru_b8fix_est_<doy>_<site>` and `_rap_aru_b8late_est_<doy>_<site>`, the configurations of 8.4
  unchanged (`hold_release_cyc` 0.25, `refloat_partner_signal` true), the same 3 receivers and 5 days; float
  reference the section 7 float runs. Order rule of section 5.
- Bars: B1 to B4 exactly as in 8.5. B4 is kept as frozen although 8.6 showed that it sits on a half-cycle offset
  of the witness at STPM00SPM. Reported beside it, not graded and added after 8.6: the fix events whose clear
  witness is more than 0.6 cycle from the fixed wide lane.
- Mechanics check, reported: the partner refloat acts at BAKE00CAN DOY 145 on G31 at 03:37:00 in both runs (the
  case it was built for).

**8.8 Results of the second rerun (2026-11-12).** Runs `D:\GnssProcessorSupport\runs_nom\_rap_aru_b8{fix,late}_est_<doy>_<site>`,
30 runs, all exit 0, executable of 8.7. Grades: `fixing_control\grade_b8.json` (`tools\grade_b.py b8fix b6flt
b8late`), `fixing_control\witness_b4_b8.jsonl`. Stage B fails as frozen: B2 on 3 of 15 receiver-days and B4 on 8 of
30 runs. B1 passes on 15 of 15.

- Mechanics check: passed. At BAKE00CAN DOY 145 the partner refloat acts in both runs on G28 (03:33:00), G32
  (03:35:30), G03 and G31 (03:37:00), and the hold release acts once per run (6 and 5 times in 8.6). Over the 30
  runs the partner refloat acted in 12 runs (26 satellites), the hold release in 12 runs (30 satellites).
- B1: 908 pair-arcs compared, none fixed to different integers. The grader's "two values in one run" counts at
  BAKE00CAN DOY 146 (4, run from 00:00, also in sections 7 and 8.6) come from its arc bounds, which take the resets of
  the run from 00:00 only after the run from 01:00 begins; with each run's own resets no pair-arc in either run takes
  two values.
- B2: fails at BAKE00CAN DOY 143 (7.8 against 7.6 mm, as before), STPM00SPM DOY 144 (19.9 against 14.2 mm, as before)
  and now BAKE00CAN DOY 145 (6.5 against 4.4 mm; 4.2 mm fixed in sections 7 and 8.6). Median over the 15
  receiver-days 7.3 mm fixed, 10.9 mm float; fixed better on 12 of 15. No other receiver-day moved by more than
  0.05 mm from 8.6.
- BAKE00CAN DOY 145, hour by hour (median 3D / mean up, mm, from `state_trace.csv`): run from 00:00 of 8.6, hours 5
  and 6, 6.0 / -5.1 and 3.9 / -2.6; this rerun 8.0 / -7.5 and 8.3 / -8.4; run from 01:00 of this rerun 9.6 / -9.2
  and 9.9 / -10.0; float 3.9 / -3.5 and 4.4 / -3.9. Hours 2 and 3 are unchanged; the move starts with the refloats of
  03:33 to 03:37. Every pair fixed in both 8.6 and this rerun after 03:30 carries the same integers at the same
  epochs; the runs differ in which pairs are fixed and when (for example G28-G26 fixed only here, G28-G31 at
  wide lane -3 only here where 8.6 also fixed -4). The height offset of 4 to 6 mm is not traced to a term; both
  runs of this rerun agree, so B1 cannot see it.
- B4: the same 24 events in the same 8 runs as in 8.6 and section 7. Beyond 0.6 cycle (reported, not graded): 1, the
  G12-G28 case at STPM00SPM DOY 146.
- B3: as in 8.6.
- Unit suite at `b319c99`, Debug: 4 270 tests in 563 suites, 4 233 passed, 37 skipped, 0 failed, the 9 cases of
  `ArHoldReleaseTest` among them (`fixing_control\gtest_debug_arud.txt`).
- Not done: the 14 other test receivers (stopped by the rule of section 5), the stream replays, the trace of the
  BAKE00CAN DOY 145 height move, the STPM00SPM height offset and the half-cycle witness offset at STPM00SPM.

**8.9 Trace of the BAKE00CAN DOY 145 height move (2026-11-12).** Tools `fixing_control\tools\trace_split.py`
(position difference of two runs per epoch) and `tools\nl_check.py` (held integers against the converged float run
of the same receiver-day, the misfit of `holdMisfit_`; the float run makes no fixing decision but shares the models,
so it is a consistency check, not a truth).

- The two fixed runs from 00:00 (8.6 `b7fix`, 8.8 `b8fix`) part at 03:36:30 by 0.2 mm, by 1.9 mm in up at 03:38:00
  (the 8.6 run moves up 1.6 mm), and by 6.9 mm at 04:51:30: there the 8.6 run's up error jumps from -7.0 to -1.8 mm in
  one epoch, at an accepted 11-pair fix, while the 8.8 run stays at -8.6 mm. Both runs fall in height between 03:40
  and 04:50 (-2.4 to -7.0 mm and -4.2 to -8.6 mm); the float run falls 2 mm (-2.0 to -4.1 mm).
- The 04:51:30 fix of the 8.6 run and the 04:50:00 fix of the 8.8 run hold the same 11 pairs with the same integers,
  floats within 0.011 cycle. In the 8.6 run the next epoch reads G28-G31 0.40 narrow-lane cycle from the integer just
  fixed (d_wl -0.14) and the hold release refloats G28: the jump comes from a tension between that fix and state
  outside the fixed set. The 8.6 run had held G28-G31 at wide lane -4 from 03:38 (the independent wide lane reads
  about -3.9 until 03:30 and -3.0 from 03:40) and released it at 03:41; the 8.8 run never held -4 (its partner refloat
  reset G28 at 03:33) and fixed -3 at 04:49. The source of the tension is not resolved further.
- Every integer held in the last hour (05:00 to 06:00) agrees with the float run within 0.063 narrow-lane cycle in both
  runs (16 pairs, same integers in both); over 03:36 to 04:52 within 0.25 (the largest, G16-G31 and G16-G04, on a rising
  satellite). No held integer of the 8.8 run is contradicted by the float, and its wide lanes pass B4 at BAKE00CAN.
- Reading: the B2 pass at BAKE00CAN DOY 145 in sections 7 and 8.6 rested on the 5.2 mm jump of 04:51:30, which came
  from an inconsistent state, not from a better integer set. Without it the fixed solution settles 4 to 6 mm below the
  float in height with integers the float agrees with, the pattern of STPM00SPM DOY 144 (section 7.3): the fixed
  solution reaches a lower height sooner and the float drifts toward it (-2.0 to -3.9 mm over the run). The term is a
  height offset common to fixed and float, which fixing exposes earlier; its source (the reference coordinate, the
  receiver antenna model at BAKE00CAN and STPM00SPM, or the troposphere) is not traced. B2 as frozen counts it against
  fixing.

**8.10 Trace of the common height offset (2026-11-12, diagnostic, not graded).** Mean up error over the last hour,
fixed (8.8) and float (section 7): STPM00SPM -8 to -20 mm and BAKE00CAN -5 to -13 mm on all five days, WTZZ00DEU near 0
on DOY 143 to 145 and -7 to -13 on DOY 146 to 147, fixed and float within a few mm of each other on most days
(`tools\height_table.py`). The reference (mean of the IGS daily solutions) scatters by 2.1, 2.5 and 3.0 mm in up per
day at the three receivers, and both receiver antennas carry G01, G02, E01 and E05 in `igs20.atx`, so neither explains it.

- Source: the user configuration takes ocean tide loading from the EOT20 grid (radial only, `data_download.nc_path`),
  because `model_corrections.otl_from_sif` is off in it; the network's field configuration armed the SIF FES2014b
  three-component coefficients on 2026-10-28 (card OTL, which found EOT20 within a few mm of the SIF in the open ocean
  and up to 10 mm off near land). SIF M2 vertical amplitude: STPM00SPM 19.8 mm, BAKE00CAN 5.3 mm (K1 6.8), WTZZ00DEU 5.5.
- One-change float runs on DOY 144, 145 and 147 at the three receivers (`_rap_aru_{htoff,htsif,httq8}_est_<doy>_<site>`,
  27 runs; `tools\height_compare.py`), last-hour up minus the section 7 float: loading off +8.0 to +13.6 mm at
  STPM00SPM, +2.7 to +6.3 at BAKE00CAN, -2.0 to +2.1 at WTZZ00DEU; SIF loading +2.3 to +9.1, +4.6 to +5.9, -2.0 to +7.4;
  `troposphere_q` 1e-8 -0.4 to +0.3 mm (WTZZ00DEU DOY 145 -1.2). The EOT20 term, not the troposphere noise, carries the
  STPM00SPM and BAKE00CAN offset; switching the loading off is closer to the SIF there than the grid is.
- All 15 receiver-days with `otl_from_sif` (`_rap_aru_b9{fix,late,flt}_est_<doy>_<site>`, the 8.8 configurations plus
  that key, 45 runs, all exit 0; `fixing_control\grade_b9.json`). Mean absolute up error over hours 3 to 6: fixed 6.2 to
  4.2 mm, float 7.0 to 4.0 mm (BAKE00CAN 6.0 to 3.2 and 3.2 to 2.3, STPM00SPM 9.3 to 5.0 and 8.8 to 4.6, WTZZ00DEU 3.4
  to 4.5 and 8.9 to 5.3). B1 as in 8.5: 903 pair-arcs, none different. B2 as in 8.5 (reported only): fails on 6 of 15
  (WTZZ00DEU DOY 143, 144, 146 and 147, STPM00SPM DOY 144 13.6 against 7.4 mm, BAKE00CAN DOY 145 3.7 against 3.5 mm);
  median 3D over the 15, 6.8 mm fixed and 8.3 mm float; fixed better on 9 of 15.
- Reading: the common height offset at STPM00SPM and BAKE00CAN is mostly the EOT20 tide loading of the user
  configuration. With the SIF loading both solutions reach about 4 mm in up and the float gains more than the fixed:
  the margin that fixing showed came partly from settling sooner on a mis-modelled tide. At WTZZ00DEU the fixed
  solution gets worse with the SIF on DOY 143 and 144 (up +0.5 to -8.1 and -1.9 to -7.8 mm, east -0.6 to -2.9 and -1.4 to
  -3.0 mm); not traced. Not tested: the network's other two loading keys (nodal corrections, removal of the diurnal
  translation), and what is left at STPM00SPM DOY 144 (fixed up -13.5 mm against float -6.7 mm with the SIF).

## 2026-11-20: closed

Closed by the managing session's choice of 2026-11-20, under the author's deferral of the consolidation decisions ("I will defer to your choices on the consolidation 'decisions for the author'"); applied by the docs consolidation on branch `docs/consolidate-2026-11-20`, in the batch of the user series where the card's own text shows nothing owed. The user-engine code of this line is on main since 2026-11-17. The line is continued in ARUB.
