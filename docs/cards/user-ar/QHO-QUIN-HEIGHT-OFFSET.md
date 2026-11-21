# QHO: the height offset of the user at QUIN00USA

**STATUS**

- **id:** QHO
- **topic:** user-ar
- **status:** parked
- **summary:** Pre-registered 2026-11-21 at the managing session's request. The fixing user at QUIN00USA reads +3.43 cm in height on five warm starts (days 143 to 147 of 2026) where standard PPP reads +0.05 cm. The cause is sought in the user's path, cheapest test first (constellation split, term-by-term receiver models, elevation signature, network side last); any fix meets the bar of section 4. Result 2026-11-21 (section 5): no defect in the user's path; the question compared two elevation masks, and QUIN carries an elevation-dependent receiver-side term at the users' 15 degree mask in both paths. Parked 2026-11-21 by the managing session: a site term, next step named in the Outcome.
- **dates:** 2026-11-21 (pre-registered; results, section 5; parked). The comparison of existing runs and the first configuration-only runs began on 2026-11-20 and 2026-11-21 in card UFQ's working folder, before this file existed; section 5 reports them with their run names.
- **knobs / posture:** the replay users of the poster's refresh experiment (`_rap_agu3_rarm1120x_<doy>_quin`, fixing, and `_rap_agu3_rarm1120f_<doy>_quin`, never fixing, on `arm1120`'s network products) and its standard PPP sets (`_rap_agu3_pppGh3_<doy>_quin`, `_rap_agu3_pppFh3_<doy>_quin`); arms are configuration-only copies with one key changed.
- **code:** none (configuration and grading only).
- **run dirs:** `D:\GnssProcessorSupport\runs_nom\_rap_ufqq_<arm>_<doy>_quin`; tools in `D:\GnssProcessorSupport\gates\day_program_2026-11-20\user_fix\quin\`.
- **source:** the managing session's request of 2026-11-21 (after card [UFQ](../../record/cards/user-ar/UFQ-USER-FIX-QUALITY.md) item 1); the AGU refresh experiment (`presentation/agu26/figures/v3_integer/tools/refresh_agu.py`, reference `refresh_arm1120`); memory note on the receivers without Galileo antenna calibrations (BADG00RUS, KOKB00USA, QUIN00USA).

**Outcome.** Parked. No defect in the user's path: at one elevation mask the user at QUIN00USA equals standard PPP (10 degrees: medians 0.05 and 0.05 cm; 15 degrees: means 3.34 and 3.41 cm; wet delays within 0.2 cm); the question compared the user at 15 degrees with a PPP set at 10. QUIN carries a receiver-side term that changes sign with elevation (10, 15, 25 degrees: 0.67, 3.34, 1.12 cm), in GPS and Galileo alike: the antenna pattern or near-field reflections under the snow radome, whose ATX entry is a GPS-only calibration. Next step: residuals against elevation at QUIN, compared with KOKB00USA (same antenna family).

> Results are appended TO THIS FILE, below the text.

---

## 1. Question

Why does the fixing user at QUIN00USA read +3.43 cm in height on the five warm starts of the poster's refresh experiment
(03:00 to 06:00, days 143 to 147 of 2026) where standard PPP reads +0.05 cm, and which term of the user's path makes the
difference?

## 2. Statistic and reference

- Height error: the median up error over minutes 60 to 180 of each 3 h start; the five days' median and mean.
- Reference: the IGS mean of the period (`presentation/agu26/period2026/coords/test_coords_2026.json`, a marker
  coordinate) reduced to the antenna reference point with the file's RINEX `ANTENNA: DELTA H/E/N`, the reduction both
  paths' graders apply (`grade_ppp26.marker_to_arp`, the twin of `Coordinates::markerToArp`).
- Wet delay: the user's per-epoch `zwd=` of `logs/rt_engine.log`; standard PPP's `tropo_m` of `logs/state_trace.csv`;
  the median over the same minutes.
- Settings compared: each arm differs from its base in one key; the user and the PPP are compared only at one
  elevation mask and one window.

## 3. Method (the managing session's notes, cheapest first)

1. Read which ATX entries QUIN's antenna and radome resolve to in both engines' logs, then split by constellation on one
   day (the same user, GPS only and Galileo only). An offset that sits in one system points at the receiver antenna
   calibration of that system's signals.
2. Compare what the two solutions apply at the receiver, term by term: antenna offset and pattern per signal,
   eccentricity (marker to ARP), solid tide, ocean loading, the wet delay. Standard PPP is ionosphere-free on L1/L2 and
   E1/E5a with external products; the user is uncombined on the network's products. A term applied in one path and
   not in the other is the finding.
3. Elevation signature: the same user at other elevation masks. An offset that grows or shrinks with the mask, paired
   with a wet delay change of about a third of the height change and of opposite sign, marks an elevation-dependent
   model term (pattern or mapping), not a constant.
4. Only then the network side: QUIN's nearest network receivers and whether the offset follows the satellites' biases
   (a steady height offset at one site is expected to be a receiver-side term).

## 4. Bar for a fix

The height offset at QUIN on the five days inside the spread of the other 16 receivers; no other receiver moved by more
than its day-to-day spread; default off and identical when off. The cause and the size of its term are reported before
anything larger than a configuration change is built.

## Results, 2026-11-21

### 5. The user equals standard PPP at each mask; QUIN's offset is a receiver-side elevation term of both paths

Runs: the branch's engine without user keys (`gnss_processor_ufqp.exe`, sha256[:16] 3db51558cf096e7a; it reproduces
`arm1120`'s users except the trace columns card UFQ item 3 declared), each arm a copy of
`_rap_agu3_rarm1120x_<doy>_quin` with one key changed: `ufqq_ppplike` (the standard PPP's troposphere a priori,
gradient noise, relinearization, ionosphere noise and observation path), `ufqq_noiono` (network ionosphere off),
`ufqq_antu_145` and `ufqq_antp_145` (10 epochs, antenna loggers at debug; the user and a copy of `pppGh3`),
`ufqq_gps_145` and `ufqq_gal_145` (one constellation), `ufqq_m10` and `ufqq_m25` (elevation mask 10 and 25 degrees;
the users and `pppGh3` run at 15, `pppFh3` at 10). Every run exit 0. Tools `quin_series.py`, `upsites.py`,
`antfreq.py`.

**The antenna both engines resolve.** Both logs: receiver type `ASH701945E_MSNOW` matched to the ATX entry
`ASH701945E_M SNOW` (a GPS-only robot calibration, G01 and G02); GPS from its own frequencies; Galileo through the
cross-system fallback, E01 to G01 and E05 to G02, for the offset and the pattern (243 and 270 lines of each kind in 10
epochs, no other fallback). Both engines' model lines agree (troposphere, relativity, Shapiro, solid tide, ocean
loading from the same SIF FES2014b rows, pole tide, satellite and receiver offset and pattern, wind-up); the one
listed difference is the code and phase bias source (external products in standard PPP, the network's in the user).

**Height and wet delay by elevation mask** (cm; median of minutes 60 to 180 of each start):

| QUIN | 143 | 144 | 145 | 146 | 147 | Median | Mean |
|---|---|---|---|---|---|---|---|
| User, 10 deg, up (`ufqq_m10`) | -0.51 | -0.17 | 3.28 | 0.71 | 0.05 | 0.05 | 0.67 |
| Standard PPP, 10 deg, up (`pppFh3`) | -0.38 | 0.05 | 4.08 | 1.10 | -0.15 | 0.05 | 0.94 |
| User, 15 deg, up (`rarm1120x`) | 1.85 | 2.36 | 5.40 | 3.66 | 3.43 | 3.43 | 3.34 |
| Standard PPP, 15 deg, up (`pppGh3`) | 1.53 | 2.50 | 5.95 | 4.53 | 2.52 | 2.52 | 3.41 |
| User, 25 deg, up (`ufqq_m25`) | 1.33 | 1.49 | 3.41 | -2.12 | 1.47 | 1.47 | 1.12 |
| User, 10 deg, wet delay | 10.41 | 5.59 | 6.83 | 6.19 | 7.04 | 6.83 | 7.21 |
| Standard PPP, 10 deg, wet delay | 10.54 | 5.61 | 6.78 | 6.07 | 7.12 | 6.78 | 7.23 |
| User, 15 deg, wet delay | 9.61 | 4.74 | 5.63 | 5.14 | 5.59 | 5.59 | 6.14 |
| Standard PPP, 15 deg, wet delay | 9.89 | 4.97 | 5.81 | 5.04 | 5.90 | 5.81 | 6.32 |
| User, 25 deg, wet delay | 9.52 | 4.98 | 6.05 | 7.25 | 6.07 | 6.07 | 6.77 |

**Day 145, one constellation (15 deg):** GPS and Galileo +5.40 cm up, GPS only +6.91, Galileo only +8.42 (wet delay
5.63, 5.04, 5.31 cm).

**What the runs settle.**
1. **No defect in the user's path.** At one mask the user equals standard PPP: 10 degrees, medians 0.05 and 0.05 cm,
   means 0.67 and 0.94, each day within 0.8 cm, wet delay 7.21 and 7.23; 15 degrees, means 3.34 and 3.41, each day
   within 0.9 cm, wet delay 6.14 and 6.32. The user's own settings (`ufqq_ppplike`: 3.34) and the network ionosphere
   (`ufqq_noiono`: 3.34) do not move it. Standard PPP runs on external products, so the network's corrections are
   not the cause either (step 4 of the method is answered without a network run).
2. **The two numbers of the question come from two masks.** +3.43 cm is the median of the user at 15 degrees; +0.05
   cm is the median of the standard PPP set at 10 degrees (`pppFh3`). The poster grades the set at the users' mask
   (`pppGh3`, +3.41 cm mean at QUIN), so its user and PPP numbers at QUIN agree.
3. **QUIN's offset at 15 degrees is a receiver-side term that depends on elevation, in both systems and both paths.**
   It is highest at 15 degrees on each of the five days (10, 15, 25 degrees: means 0.67, 3.34, 1.12 cm), and each
   step trades the wet delay against the height with the opposite sign (10 to 15: height +2.67, wet delay -1.07; 15
   to 25: -2.22 and +0.63; 0.40 and 0.28 of the height change). The observations between 10 and 15 degrees pull the
   height down and those between 15 and 25 degrees pull it up: the signature of a term that changes sign with
   elevation (antenna pattern or near-field multipath) rather than of a mapping-function error, whose effect grows
   monotonically toward the horizon. GPS alone and Galileo alone both carry it, so the Galileo antenna fallback is not the cause.
4. **Size, against the other receivers.** Standard PPP, 15 minus 10 degrees, five-day mean of the up error: QUIN
   +2.47 cm, then CIBG -1.60, DAV1 +1.46 and UTQI -1.24; the other thirteen within 0.62 cm. At 15 degrees QUIN (user
   +3.34, PPP +3.41) lies outside the other 16 receivers (user -2.90 to +1.09, PPP -5.11 to +1.44); at 10 degrees
   (PPP +0.94) inside (PPP -5.07 to +1.54). The user's difference from standard PPP at QUIN (-0.07 cm) is among the
   smallest of the 17 receivers (from -1.78 at CIBG to +1.23 at WTZZ, SCRZ +4.13 where standard PPP itself reads
   -5.11).

**Reading.** Nothing in the user's path explains the offset; a fix there would be a fix of the site. A configuration
change that meets the bar of section 4 is not shown: a 10 degree mask brings QUIN inside the spread in standard PPP,
but it is a posture change for every user (admission of low satellites, integer fixing) and would need the 85 starts
with the bar. Which term at QUIN changes sign with elevation (the radome calibration of one antenna, the station's
low-elevation environment) is a site question, open; a per-elevation residual comparison of QUIN with a quiet
receiver of the same antenna family (KOKB00USA, ASH701945G_M) would be the next step.
