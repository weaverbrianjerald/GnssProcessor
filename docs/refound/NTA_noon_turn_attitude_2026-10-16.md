# NTA: one attitude model for the satellite PCO and the phase wind-up, and the GPS noon turn

**STATUS**

- **id:** NTA
- **topic:** network-vehicle
- **status:** open
- **summary:** BUILT 2026-10-16, CHECKED 2026-10-17, default off, UNVALIDATED, not armed. The beta-angle sign of `SatAttitude::computeBeta` is validated against an independent computation (336 reference rows, seven GPS satellites, both signs: 0.010 deg at most, no sign mismatch), and again against cssrlib run with its own Sun, frames and nominal yaw frame (0.010 deg, no sign mismatch); the check re-derived both from scratch on 1536 rows each (32 satellites, 5e-6 deg, no sign mismatch). The phase wind-up now takes its satellite frame from `SatAttitude`, bit-identical to the old inline copy: knob off is identical to main on both golden 2 h arms and on the noon windows, except one declared toggles log line. GPS Block IIF and IIR noon turns (Kouba 2009) are built behind `processing.enable_noon_turn_attitude` for the PCO and the wind-up alike. The goldens (00:00 to 06:00 UT) hold no GPS noon turn and the knob on is inert there (identical 2 h arms). Graded 2 h field windows, knob on against off: the turning G25's post-fit phase residual falls from 7.09 to 4.86 mm and the held-out consumer's hour-2 p50 from 17.8 to 15.6 mm; G26 and G01 move by 0.2 mm or less.
- **source:** the queued M-PAGES build 3 ([OPEN_ITEMS](../status/OPEN_ITEMS.md), "Queued after NOM"); card [OWD](OWD_owed_defects_2026-09-22.md) O7 (the Galileo yaw law, left nominal here); [GAD](GAD_galileo_adoption_2026-10-13.md) section 4; Kouba (2009), "A simplified yaw-attitude model for eclipsing GPS satellites", GPS Solutions 13
- **knobs / posture:** `processing.enable_noon_turn_attitude` (ledger row, UNVALIDATED, default false); `processing.enable_orbit_normal_attitude` and `estimation.enable_sun_frame_correction` untouched
- **dates in the text:** 2026-10-16; the validation day is DOY 129 of 2024
- **hashes / run dirs:** branch `mpg/attitude` from main `4318fe3`; reference exe `gnss_processor_attitude_ref.exe` (the unmodified base); reference rows by `scripts/attitude_beta_check.py`; scratch under `D:/GnssProcessorSupport/gates/attitude/`; check stage (section 8): candidate `gnss_processor_attitude.exe` (branch `b9ff57c`), control `gnss_processor_attitude_ctl.exe` (main `4318fe3`), arms `runs_nom/_rap_attchk_*`, reference tools and logs `gates/attitude/check/`

**Outcome.** Question: do the satellite PCO and the wind-up share one attitude model, is `computeBeta` right in sign, and can the GPS Block IIF and IIR noon turns be modelled behind a default-off knob? The sign is right: against SP3 orbit normals in the inertial frame, an Almanac Sun and a velocity-free broadcast-element beta, the routine agrees within 0.010 deg on 336 rows over both signs, and cssrlib, run with its own Sun and frames, gives the same agreement. The wind-up consumes `SatAttitude`, bit-identical off. The noon-turn law reproduces an independent rate-limited yaw follower to 0.01 deg in 11 of 11 windows. The check (section 8) re-derived the sign from scratch and graded two 2 h field windows: the turning satellite's post-fit phase residual falls 31 % (G25, lag to 88 deg) and 12 % (G26), the held-out consumer's hour-2 p50 moves 17.8 to 15.6 mm (G25) and 13.6 to 13.3 mm (G26); knob off is identical to main, and the goldens see no turn. Verdict: check passed. Standing: UNVALIDATED, not armed; section 7 and section 8.6 list what is owed.

## 1. What was built

| piece | where | note |
| --- | --- | --- |
| beta, orbit angle | `SatAttitude::orbitGeometry` | always inertial velocity; the sign convention is that of section 2 |
| yaw law | `SatAttitude::noonTurnYaw` | stateless closed form (section 3) |
| law per satellite | `SatAttitude::noonTurnLaw` | from the ATX satellite type: Block IIF 0.11 deg/s, Block IIR, IIR-A, IIR-B, IIR-M 0.20 deg/s |
| frame | `SatAttitude::applyNoonTurn` | rotates X and Y about nadir by (actual minus nominal yaw); mode `NoonTurn` |
| shared helpers | `SatAttitude::prnOf`, `usesLegacyOrbitNormal`, `velocityFrame` | the PCO and the wind-up had each its own copy |
| PCO | `Antenna::getAntennaCorrection` | applies the law after the satellite ATX entry is selected, so the block is the one the PCO itself uses |
| wind-up | `GnssCorrections::computePhaseWindUp` | builds its frame with `SatAttitude`; new trailing argument `noon_turn_law` (null by default) |
| knob | `processing.enable_noon_turn_attitude` | getter `enableNoonTurnAttitude()`, in the toggles log line as `noon_turn_att` |
| tests | `tests/test_noon_turn_attitude.cpp` | suites `NoonTurnBetaReference`, `NoonTurnLawTable`, `NoonTurnYawLaw`, `NoonTurnFrame`, `NoonTurnWindUp`, `NoonTurnConfig` |
| reference tool | `scripts/attitude_beta_check.py` | `table`, `noon`, `csv`, `compare`, `follower`, `windows` |

No file of the network producer, `MeasurementModel*`, `StateInitializer`, `StateManager` or `PppModel` is touched.

## 2. The beta sign

**Convention.** beta is the Sun's elevation above the orbit plane, positive on the +(r x v) side, with r x v in the inertial frame. The yaw angle of the body X axis is positive about the nadir (+Z) axis, measured from the velocity direction; the nominal (Sun-tracking) yaw is atan2(-tan beta, sin mu), mu the orbit angle from the orbit midnight (0 at midnight, 180 deg at noon). The yaw-steering frame of `SatAttitude::compute` (j = k x e_sun, i = j x k) was derived to sit at exactly this yaw, and the check below confirms it numerically.

**Independent computation** (`scripts/attitude_beta_check.py`, no project code): the CODE MGX 5 min SP3 of DOY 129 of 2024 rotated to the inertial true-of-date frame with the Greenwich apparent sidereal angle; the Sun from the Astronomical Almanac low-precision series (about 0.01 deg); the orbit normal as r x (r(t+5 min) - r(t-5 min)), exact for a planar orbit and free of any velocity-frame convention; and a second source with no orbit at all, the broadcast inclination and node, h = (sin i sin W, -sin i cos W, cos i).

**Result** (7 satellites G01, G03, G10, G13, G25, G26, G32; 48 samples each, 30 min apart; beta from -2.6 to +71.8 deg, both signs):

| quantity | worst difference |
| --- | ---: |
| `computeBeta(..., correct_velocity_frame = true)` against the SP3 value | 0.0097 deg |
| broadcast-element beta against the SP3 value | 0.0025 deg |
| X axis of the yaw-steering frame against the Kouba nominal yaw (angle between the two axes) | 0.0021 deg |
| sign mismatches with beta beyond 1 deg | 0 of 336 |

The noon times follow from the same computation: G25 08:40 UT and G26 11:21 UT on DOY 129, matching the times the item named. Eight of the rows (both signs, one inside a G25 turn) are embedded in `tests/test_noon_turn_attitude.cpp` as the standing external anchor.

**Second reference: cssrlib.** `scripts/attitude_beta_check.py cssrcsv` rebuilds the same 336 rows from cssrlib alone: its Sun (`sunmoonpos_eci`), its IAU-1980 precession, nutation and sidereal rotation (`eci2ecef`) for the inertial orbit normal, and its nominal yaw frame (`orb2ecef`, the frame of its `windupcorr`) for the X axis. The C++ routines are fed cssrlib's ECEF Sun, so both sides use one Sun. cssrlib has no beta routine and no noon-turn model, and POINT holds no satellite attitude model, so these are the only external comparisons available for the frame; the noon-turn law itself has only the rate-limited follower of section 3.

| quantity (cssrlib Sun and frames) | worst difference |
| --- | ---: |
| `computeBeta(..., correct_velocity_frame = true)` against the cssrlib-chain value | 0.0096 deg |
| X axis of `SatAttitude::compute` against the X axis of cssrlib `orb2ecef` | 0.0020 deg |
| sign mismatches with beta beyond 1 deg | 0 of 336 |
| ECEF velocity as is (`correct_velocity_frame` false) against the cssrlib-chain value | 21.7 deg, 44 sign mismatches of 336 |

cssrlib's own Sun differs from the Almanac Sun by about 0.6 deg (its series is referred to the equinox of date and is then rotated through the J2000 precession matrix, which applies the precession twice), so its beta differs from the Almanac beta by up to about 0.3 deg. That does not enter the comparison: the satellite and the Sun pass through one matrix on each side. The legacy-flag figures of the finding below are reproduced with both references (Almanac: 21.8 deg, 47 of 336).

**Finding: the velocity frame.** `getSatellitePV` returns the ECEF velocity, whose cross product with the position is not the orbit normal. With `correct_velocity_frame` false, which is how `GnssConfig::enableSunFrameCorrection()` (default off, unarmed in both goldens) feeds `SatAttitude::compute` and `computeBeta`, the same rows give a beta wrong by up to 21.8 deg and the wrong sign on 47 of 336 rows. That flag therefore also decides whether `processing.enable_orbit_normal_attitude` selects its BeiDou and QZSS modes from a right or a wrong beta; it conflates the precession fix with the velocity fix. The noon-turn geometry always converts the velocity and does not depend on the flag. The orbit-normal knob is default off and outside the goldens; the conflation is recorded in section 7.

## 3. The noon-turn law

Near the orbit noon the nominal yaw sweeps up to 180 deg, at a peak rate mu_dot / |tan beta| (mu_dot the orbit angular rate, 0.00836 deg/s for GPS). The controller follows it only while that stays below its maximum rate R, that is for |beta| >= beta0 = atan(mu_dot / R): 4.346 deg for Block IIF (R = 0.11 deg/s) and 2.394 deg for Block IIR (0.20 deg/s). Below beta0 the satellite follows the nominal yaw until the rate it would need reaches R, then yaws at R, against the sign of beta, until the ramp meets the nominal yaw again (Kouba 2009).

`noonTurnYaw` solves this without state. With u the orbit angle from the noon and tb = |tan beta|, the turn starts at u = -acos(c), c the positive root of R c^2 + mu_dot tb c - R (1 + tb^2) = 0 (the rate crossing, exact); the yaw there is the nominal one and the ramp runs at R from it; the end is the first u beyond the noon where the ramp meets the nominal curve, found by bisection on a bracket that holds by construction. Outside the window the yaw is the nominal one and the frame comes back bit-identical. The orbit angle advances at the constant mu_dot, as in the reference model.

| check | result |
| --- | --- |
| rate inside the turn, 12 (beta, block) cases sampled every 0.5 s of orbit time | R within 1 % |
| continuity at start and end | actual minus nominal below 1e-6 deg |
| symmetry | yaw(-beta) = -yaw(beta) |
| beta = 0 | starts at the noon, lasts 180 deg / R (1636 s IIF, 900 s IIR) |
| window against an independent rate-limited follower (a time-stepped controller, 0.25 s) | start and end within 0.01 deg in 11 of 11 windows |
| threshold | no turn above beta0, a turn just below, IIF at 4.0 deg turns where IIR does not |

Examples (orbit angles, duration, largest lag behind the nominal yaw): Block IIF at beta 2.44 deg turns from 177.84 to 186.20 deg, 1000 s, 26 deg; at beta -0.75 deg from 178.36 to 189.84 deg, 1374 s, 88 deg; at beta -0.28 deg 122 deg. A Block IIR at beta -2.21 deg, close to its 2.39 deg threshold, lags by 1.7 deg only.

**Not modelled.** Eclipse and midnight turns (a satellite in the umbra is excluded at the geometry by default, and the midnight turn of a low-beta GPS satellite lies in the umbra); yaw biases and the sign-flip rule at beta near 0; Block IIA and III; GLONASS; BeiDou; Galileo (card OWD O7, the FOC law is a separate item and stays owed). The constant orbit rate makes the turn timing exact only for a circular orbit; the GPS eccentricity of up to 0.02 moves it by seconds.

## 4. One model for the PCO and the wind-up

Before: `Antenna` took the frame from `SatAttitude::compute`, while `GnssCorrections::computePhaseWindUp` built its own copy of the same three branches (yaw steering, BeiDou-2 GEO orbit normal, velocity fallback), with the note that nothing checked that the two agreed.

Now `computePhaseWindUp` calls `SatAttitude::compute` for the yaw-steering and the GEO orbit-normal cases and `SatAttitude::velocityFrame` for the case without a Sun direction, the same expressions in the same operand order, so every result is bit-identical. Gates: the existing wind-up suite `PhaseWindUpSignTest` (all three branches) is unchanged and passes, `NoonTurnWindUp.NoLawIsBitIdenticalToTheDefaultAndToTheNominalFrame` compares the new argument at null with the default, and section 6 carries the same-build smoke. The wind-up keeps passing the orbit-normal flag as false (BeiDou-2 GEO orbit normal, yaw steering for the rest), so `processing.enable_orbit_normal_attitude` still moves the PCO frame only; making the wind-up follow it would move BeiDou and QZSS wind-up when that flag is on and is recorded in section 7.

With the noon-turn knob on, `Antenna::getAntennaCorrection` picks the law from the type string of the satellite ATX entry it selected for the epoch (`G25_BLOCKIIF_G062` gives Block IIF), applies `applyNoonTurn` to the PCO frame and hands the same law to the wind-up, so both see one attitude. A satellite without an ATX entry or without a law keeps the nominal frame.

## 5. What the goldens and the field days see

GPS noons on DOY 129 of 2024 for the Block IIF and IIR satellites whose ATX entry covers the day (`scripts/attitude_beta_check.py windows`; times UT, beta in degrees):

| satellite | block | noon | beta | turn |
| --- | --- | --- | ---: | --- |
| G01 | IIR-M | 11.04 h | -2.21 | 11:01 to 11:05 |
| G01 | IIR-M | 23.01 h | -1.75 | 22:58 to 23:06 |
| G25 | IIF | 08.67 h | -0.75 | 08:37 to 09:00 |
| G25 | IIF | 20.64 h | -0.28 | 20:36 to 21:01 |
| G26 | IIF | 11.35 h | +2.44 | 11:17 to 11:33 |
| G26 | IIF | 23.32 h | +2.90 | 23:15 to 23:30 |

No other Block IIF or IIR satellite has |beta| below its threshold at its noon that day. The G01 turns are the weakest (lag under 2 deg), the G26 ones moderate (16 to 26 deg) and the G25 ones the strong case (88 and 122 deg). In the golden window, 00:00 to 06:00 UT, the noons of those blocks fall at: G16 0.38 h (beta -5.80), G27 0.56 h (-35.97), G21 1.47 h (-25.59), G08 1.59 h (-35.06), G02 1.95 h (-25.61), G07 2.62 h (+46.09), G03 3.08 h (+21.87), G30 3.55 h (+45.89), G09 4.41 h (+66.88), G17 4.59 h (-37.15), G22 4.97 h (-5.74), G06 5.13 h (-23.22), G19 5.33 h (-37.54). The smallest |beta| is 5.74 deg (G22, Block IIR, threshold 2.39) and 5.80 deg (G16, Block IIR); for a Block IIF the threshold is 4.35. **The golden windows therefore see no GPS noon turn, and the knob is inert on both goldens.** The sim fixture is generated for the same day (year 2024, DOY 129) from the same products. The Galileo noon turns that fall in the window (E21, E27 and E31, card OWD O7) stay nominal here by design.

A single 2 h window cannot hold both noons the item named, 08:40 and 11:20 UT are 2.7 h apart: the G25 turn needs a window around 08:00 to 10:00 UT, and the G26 turn, with the G01 turn of 11:01 to 11:05, one around 10:30 to 12:30 UT.

## 6. Gates

Unit gate: `tests/test_noon_turn_attitude.cpp`, 19 cases in six suites, all passing; the existing `PhaseWindUpSignTest`, `SatAttitude` and `OrbitEnhancements` suites pass unchanged. The full Debug suite on the branch (VS 2026): 3167 cases in 422 suites, 3143 passed, 24 skipped (fixture-gated, the worktree holds no `data/` fixtures: the SIF and bootstrap fixtures), none failed.

Release smoke (10 epochs, 5 min, 58- and 60-station goldens, two exes: the unmodified branch point `gnss_processor_attitude_ref.exe` and the candidate `gnss_processor_attitude.exe`, knob unset; runs `runs_nom/_rap_attsm_*`, all exit 0):

| arm | start | reference against candidate (knob off) |
| --- | --- | --- |
| field golden | 00:00 GPS | `products.txt` and 31 of 34 CSVs byte-identical |
| field golden, noon window | 08:37:15 GPS (`start_offset_hours` 8.620833) | `products.txt` and 31 of 34 CSVs byte-identical |
| sim golden | 00:30:15 (`start_offset_hours` 0.504167) | `products.txt` and 31 of 34 CSVs byte-identical |

The three CSVs that differ in every pair (`network_run_summary.csv`, `network_service_health.csv`, `network_datum_graph.csv`) differ only in their wall-clock columns (`loop_ms`, `ms_per_epoch`, `ms_per_epoch_window`, the last column of the datum graph); with those columns removed they are identical. The off path is therefore byte-identical on the products and every solution trace, on both beds.

Knob on against off, noon window (08:37:15 GPS, inside the G25 turn from the first epoch; `_rap_attsm_n_on` against `_rap_attsm_n_off_new`): 26 of the 34 CSVs and `products.txt` differ, as expected of a joint solve. The phase post-fit residual (`network_pf_rows.csv`, 18 971 matched rows) changes most for the turning satellite: G25 2.07 mm rms over 601 rows, against 0.64 mm for the next satellite (G20) and 0.08 mm at the median satellite. The knob reaches the PCO and the wind-up through `Antenna` and acts on the turning satellite. This is a mechanics check on a cold 10-epoch start; it grades nothing (the consumer's 3D error differs by a few millimetres between the arms over the ignition transient).

## 7. Owed

- **The graded windows.** Knob on against knob off on the field golden, 2 h, one window per noon: the held-out consumer by hour and the per-satellite phase residual of the turning satellites (G25, then G26 and G01). Expected size: a yaw lag of tens of degrees moves the wind-up of the turning satellite by up to a few centimetres; most of it is common to the stations and lands in the satellite phase clock, so a consumer effect is expected to be small. Not armed on any evidence of this card.
- **Proposed windows for that stage.** Field golden, 30 s epochs, `start_offset_hours` with the half-epoch back-off, each window placing its turns in hour 2, past the ignition hour. G25 (08:37 to 09:00 UT, lag up to 88 deg): start 07:30:15 GPS, `start_offset_hours` 7.504167, to 09:30:15. G26 (11:17 to 11:33 UT, 16 to 26 deg) with the G01 turn (11:01 to 11:05 UT, lag under 2 deg): start 10:00:15, `start_offset_hours` 10.004167, to 12:00:15. The turning satellites are read in `network_pf_rows.csv`; the held-out consumer WTZZ00DEU by hour is the second instrument. The knob is read from the config only, so the control arm is the same exe with the key unset.
- **Fingerprint.** The knob is not in the network or real-time snapshot fingerprints (the files belong to the network reorganization). A resumed run must not span the knob; the knob is a fresh-convergence one.
- **Galileo yaw law** (card OWD O7), **Block IIA and III**, **GLONASS** and **BeiDou** manoeuvre laws, **midnight turns** for runs with `processing.exclude_eclipsing_satellites` off.
- **The orbit-normal knob.** `processing.enable_orbit_normal_attitude` derives its beta from `correct_velocity_frame`, which is tied to `estimation.enable_sun_frame_correction`; with that flag off the BeiDou and QZSS mode selection reads a wrong beta (section 2). The wind-up ignores the orbit-normal flag. Both are one-line changes that move BeiDou and QZSS output when the orbit-normal flag is on, so they are a decision.
- **Wind-up through gaps** (queued build 4) needs this card's predicate: `SatAttitude::noonTurnYaw(...).active` and its start and end angles are the window in which the accumulator must not be advanced through virtual epochs.

## 8. Check stage, 2026-10-17

Independent re-check of sections 2 to 6 and the graded windows of section 7. Branch `mpg/attitude` at `b9ff57c`; control `main` at `4318fe3` (worktree `attitude_ctl`, built with `scripts/wt_build.ps1`). Candidate exe `gnss_processor_attitude.exe` (sha256 `3e04983e`), control exe `gnss_processor_attitude_ctl.exe` (`4de0017d`), both Release, in `D:/GnssProcessorSupport/gates/exe/x64-Release/`. Arms are `scripts/rapid_arm.py make` copies of the golden configs (2 h, 30 s epochs, only the stated knob or start offset changed), runs `runs_nom/_rap_attchk_*`. The reference tools of this stage are scratch, in `D:/GnssProcessorSupport/gates/attitude/check/` (`ref_geom.py`, `cxx_probe.cpp`, `analyze_probe.py`, `cssr_rows.py`, `noons.py`, `grade_noon.py`, `bins.py`). No source file was edited.

### 8.1 The beta sign (re-derived, not re-run)

The reference was rewritten from scratch rather than rerun from `scripts/attitude_beta_check.py`: another Sun (Meeus, ch. 25), the Greenwich mean sidereal angle alone for ECEF to inertial, the orbit normal from the SP3 chord r x (r(t+150 s) - r(t-150 s)) in the inertial frame, SP3 positions by 10-node Lagrange interpolation, ECEF velocity by a central difference of that interpolant. The current `SatAttitude.cpp` was compiled unchanged into a probe and fed these inputs, for all 32 GPS satellites, 48 epochs each (30 min apart, DOY 129 of 2024, 1536 rows, beta from -37.6 to +71.8 deg). The second reference is cssrlib alone (its `sunmoonpos`, its IAU-1980 `eci2ecef` for the inertial chord, its `orb2ecef` for the nominal X axis). POINT holds no satellite attitude model (its yaw is the receiver platform's), so cssrlib is the only external code.

| quantity | own reference | cssrlib |
| --- | ---: | ---: |
| `computeBeta(..., true)`, largest difference | 4.6e-6 deg | 3.1e-6 deg |
| sign mismatches, beta beyond 1 deg | 0 of 1492 | 0 of 1488 |
| `orbitGeometry` orbit angle, largest difference | 8.8e-6 deg | 2.4e-6 deg |
| `compute` X axis against the reference IGS X axis (Z = -r, Y = Z x e_sun, X = Y x Z; cssrlib `orb2ecef`), largest angle | 1.2e-6 deg | 1.2e-6 deg |
| `computeBeta(..., false)`, ECEF velocity as is | up to 30.0 deg, 105 sign errors | up to 30.0 deg, 102 sign errors |

The Sun enters the routine and the reference alike, so these figures test the sign, the frame algebra and the velocity conversion; the Sun series is a separate, smaller term (on the 336 rows common to the Almanac table of section 2 and this one, the Meeus and Almanac betas differ by 0.0044 deg at most). The geometric yaw of the X axis (angle from the inertial velocity direction about +Z, positive about the nadir axis) equals Kouba's atan2(-tan beta, sin mu) to the print resolution of 1e-6 deg on all 7539 rows, which fixes the sign convention of the yaw used by the noon-turn law. The beta sign is validated; the legacy-flag finding of section 2 is reproduced (30 deg, about 7 % wrong signs).

### 8.2 The noon-turn law against an independent follower

Dense rows (2 s) around the three noons of DOY 129, the C++ turned frame (`applyNoonTurn`) reduced to a geometric yaw with the reference geometry, and a time-stepped rate-limited follower of the unwrapped nominal yaw (1 s, written for this check) as the reference:

| satellite | block, rate | beta at noon | largest lag | turn, C++ | turn, follower | largest yaw difference | rms |
| --- | --- | ---: | ---: | --- | --- | ---: | ---: |
| G25 | IIF, 0.11 deg/s | -0.74 | 88.1 deg | 08:37:08 to 09:00:12 | 08:37:16 to 09:00:02 | 1.08 deg | 0.27 deg |
| G26 | IIF, 0.11 deg/s | +2.44 | 25.4 deg | 11:16:50 to 11:33:38 | 11:17:04 to 11:33:22 | 1.20 deg | 0.35 deg |
| G01 | IIR-M, 0.20 deg/s | -2.20 | 1.6 deg | 11:01:14 to 11:05:12 | 11:01:26 to 11:05:06 | 0.22 deg | 0.05 deg |

The direction of the turn is right (a reversed sense would differ from the follower by twice the lag, 176 and 51 deg for G25 and G26). The C++ window is 8 to 14 s wider per side: it maps orbit angle to time with the constant mean orbit rate, the follower runs on the real SP3 motion (eccentricity), the difference stated in section 3. Outside the turn the turned frame is bit-identical to the nominal one on all three windows. The independent noon table (own ephemeris, ATX parsed directly) reproduces section 5: six turning noons on the day (G25 08:40:23 and 20:38:41, G01 11:02:29 and 23:00:52, G26 11:21:10 and 23:19:29), no other IIF or IIR satellite below its threshold.

### 8.3 Knob off against the control

`scripts/nom_td_gate.py --whole` (main's copy, `NOM_GATE_DROP_POSTURE=1`), control exe against candidate exe, same arm:

| arm | start | epochs emitted | products, 34 traces, manifest, main log, consumer log, exit code | `stdout.txt` |
| --- | --- | ---: | --- | --- |
| field golden | 00:00:00 | 240 | identical | 1 line differs |
| sim golden | 00:30:15 (`start_offset_hours` 0.504167) | 241 | identical | 1 line differs |
| field noon window, G25 | 07:30:15 (`start_offset_hours` 7.504167) | 240 | identical | 1 line differs |
| field noon window, G26 and G01 | 10:00:15 (`start_offset_hours` 10.004167) | 240 | identical | 1 line differs |

The one line, the same in all four pairs, is the `gnss_config` toggles line, which now ends in `, noon_turn_att=off` (the toggle-log convention for a new user-visible flag). With that line dropped (`NOM_GATE_DROP_RE`), the gate prints `RESULT IDENTICAL` on all four pairs. The two noon-window pairs put real turning satellites through the refactored wind-up with the knob off. The consumer's 3D p50 by hour is 0.06 and 0.01 m on the field arm and 0.10 and 0.03 m on the sim arm, the same in both exes.

### 8.4 What the goldens see

The golden windows hold no noon turn. Own-ephemeris noons of the 26 Block IIF and IIR satellites valid on the day, 00:00 to 06:00 UT (the field golden): 13 noons, none below its threshold; the two smallest are G22 (IIR-A) at 04:58:28 with beta -5.74 deg and G16 (IIR-A) at 00:22:56 with -5.79 deg, both against 2.39 deg; the next, G03 (IIF), has +21.88 deg against 4.35. The sim golden window (00:30:15 to 06:30:15) holds 12 noons, the smallest G22 at 5.74 deg and then G03 at 21.88 deg. The 2 h arms hold 5 (field, smallest 5.79 deg) and 4 (sim, smallest 25.59 deg). The code confirms it: `noonTurnLaw`, `orbitGeometry` and `applyNoonTurn`, run over all 26 satellites every 60 s from 00:00 to 06:30 UT (10 166 rows), give no frame in mode `NoonTurn` and no frame that differs from the nominal one in a single bit.
The one near-threshold noon of the golden window outside the law is G14, Block IIIA, at 04:17:22 with beta -2.62 deg; Block III has no law here (section 7), so the knob does not touch it.
Knob on in the same two 2 h arms (candidate exe, `processing.enable_noon_turn_attitude` true) against the knob-off runs: the gate reads `RESULT IDENTICAL` on the field bed (240 epochs) and on the sim bed (241 epochs), with only the toggles line dropped (it prints `noon_turn_att=on`). Products, all 34 traces, the manifest and every log are identical, so the knob is inert on both goldens, end to end.

### 8.5 Graded windows, knob on against knob off

Candidate exe, field golden, 2 h, the key unset or `true`; hour 1 is the start transient and identical in both arms (the first turn begins after 08:37 and 11:01). Held-out consumer WTZZ00DEU, 3D error:

| window | hour | p50 off | p50 on | rms off | rms on | paired median change | epochs better / worse |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| G25: 07:30:15 to 09:30:15 | 1 | 0.0529 m | 0.0529 m | 0.1196 m | 0.1196 m | 0.00 mm | 0 / 0 |
| G25 | 2 | 0.0178 m | 0.0156 m | 0.0196 m | 0.0183 m | -1.55 mm | 78 / 21 |
| G26 and G01: 10:00:15 to 12:00:15 | 1 | 0.0636 m | 0.0636 m | 0.1170 m | 0.1170 m | 0.00 mm | 0 / 0 |
| G26 and G01 | 2 | 0.0136 m | 0.0133 m | 0.0234 m | 0.0232 m | -0.20 mm | 75 / 33 |

Inside the turn windows (46, 34 and 8 epochs): G25 3D mean 19.0 to 16.0 mm (paired change -3.0 mm; the east and up means move toward zero by 2.6 and 3.2 mm); G26 20.9 to 21.0 mm (+0.1 mm); G01 15.5 to 15.2 mm (-0.2 mm).

Phase post-fit residual of the turning satellites (`network_pf_rows.csv`, phase rows paired on epoch, station and signal; 57 satellites):

| satellite, window | rows | rms off | rms on | change | rms of the on-minus-off difference | other satellites, same window (median, 90th percentile, largest) |
| --- | ---: | ---: | ---: | ---: | ---: | --- |
| G25, 08:37:08 to 09:00:12 | 2565 | 7.09 mm | 4.86 mm | -2.23 mm (-31 %) | 5.69 mm | 0.26, 0.64, 1.46 mm |
| G25, rest of the run | 10 742 | 2.57 mm | 2.33 mm | -0.24 mm | 1.14 mm | |
| G26, 11:16:50 to 11:33:38 | 1762 | 1.83 mm | 1.61 mm | -0.22 mm (-12 %) | 1.08 mm | 0.06, 0.16, 0.21 mm |
| G01, 11:01:14 to 11:05:12 | 276 | 0.91 mm | 0.83 mm | -0.07 mm | 0.19 mm | 0.007, 0.024, 0.027 mm |

Census of the whole G25 run: three of 57 satellites change by more than 0.5 mm rms (G25 2.70, G05 0.73, G20 0.71), 13 by more than 0.25 mm, median 0.15 mm; hour 2 over the satellites other than G25, G26 and G01: 1.575 to 1.527 mm. G26 run: no satellite beyond G26 (0.44 mm) above 0.1 mm. The effect is concentrated on the turning satellite and its size follows the lag (88, 25 and 1.6 deg: 5.7, 1.1 and 0.19 mm).

Time structure of G25 (2 min bins, rms off to on): 08:40 12.0 to 3.3 mm, 08:42 12.9 to 4.8, 08:44 10.9 to 9.5, then the knob-on arm is worse for about eight minutes (08:48 3.8 to 5.3, 08:50 2.9 to 4.5, 08:52 2.3 to 4.2), and better again from 08:54 (3.8 to 2.7) to 09:20 (2.9 to 2.6). The residual of both arms stays at 3 to 7 mm from 08:58 to 09:12 and above its pre-turn level of 1 to 2 mm until about 09:20. The satellite phase bias on the wire moves by up to +88 mm (mean +55 mm over the turn, other satellites 9 mm rms, 99th percentile 30 mm) and settles at +9 mm after the turn; G26 moves -0.4 mm (others 2.5 mm rms), G01 +0.3 mm (others 0.16 mm).

### 8.6 Reading

- The strong turn (G25, lag to 88 deg) is seen by the network and by the consumer, in the right direction: the turning satellite's residual falls by a third and the consumer's hour-2 p50 by 2.2 mm of 17.8. The moderate and weak turns (G26, G01) are seen in the turning satellite's residual and nowhere else.
- The model does not explain the turn completely. The residual of G25 stays above its pre-turn level for about 20 minutes after the turn in both arms, and the knob-on arm is worse than the nominal arm for about eight minutes in the middle of the turn. Candidates, none tested: the real controller start and end times against the closed form (a few seconds, section 8.2), the yaw bias and the sign rule near beta 0 (section 3, not modelled), the accumulated wind-up unwrapping through a fast sweep in the nominal arm, a satellite phase clock that absorbs part of the error in either arm.
- The consumer shares the producer's attitude model (one process, one config), so the closed-loop consumer measures consistency of the pair more than the model against the sky. An external consumer on the wire is the sharper instrument.
- The simulator's truth path does not carry the turn: `EphemerisProvider::phaseWindupMetres` calls `computePhaseWindUp` without a law. The sim bed therefore cannot grade the knob until the law is plumbed there; the field bed is the only instrument.
- Not run: the 16-site census (not part of this check), a Galileo noon-turn arm (nominal by design), a resumed run across the knob.

### 8.7 Owed after the check

- Before any arming decision: the 16-site census, a consumer that does not share the producer's attitude model, and the structure of the G25 residual (the eight minutes in which the knob is worse, the elevated residual after the turn): the controller's start and end times and a yaw bias are the first things to test.
- The simulator's truth path carrying the law, so that the sim bed can grade the knob.
- Unchanged from section 7: the snapshot fingerprints, the Galileo yaw law (card OWD O7), Block IIA and III (G14, Block IIIA, passes beta -2.62 deg at 04:17 UT inside the golden window), GLONASS, BeiDou, midnight turns, the orbit-normal decision, the predicate for the wind-up through gaps. The graded windows and the proposed windows of section 7 are done by sections 8.3 to 8.5.

### 8.8 Verdict of the check

Pass. The beta sign is validated against two independent computations; the wind-up consumes `SatAttitude`; the knob off is identical to main on both golden 2 h arms (one declared log line) and on the noon windows; the goldens contain no noon turn and the knob on is inert there; the graded windows ran to completion and the turning satellites move in the expected direction. UNVALIDATED, not armed, `config/golden/` untouched.
