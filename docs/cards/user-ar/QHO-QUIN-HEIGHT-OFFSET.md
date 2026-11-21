# QHO: the height offset of the user at QUIN00USA

**STATUS**

- **id:** QHO
- **topic:** user-ar
- **status:** open
- **summary:** Pre-registered 2026-11-21 at the managing session's request. The fixing user at QUIN00USA reads +3.43 cm in height on five warm starts (days 143 to 147 of 2026) where standard PPP reads +0.05 cm. The cause is sought in the user's path, cheapest test first (constellation split, term-by-term receiver models, elevation signature, network side last); any fix meets the bar of section 4.
- **dates:** 2026-11-21 (pre-registered). The comparison of existing runs and the first configuration-only runs began on 2026-11-20 and 2026-11-21 in card UFQ's working folder, before this file existed; section 5 reports them with their run names.
- **knobs / posture:** the replay users of the poster's refresh experiment (`_rap_agu3_rarm1120x_<doy>_quin`, fixing, and `_rap_agu3_rarm1120f_<doy>_quin`, never fixing, on `arm1120`'s network products) and its standard PPP sets (`_rap_agu3_pppGh3_<doy>_quin`, `_rap_agu3_pppFh3_<doy>_quin`); arms are configuration-only copies with one key changed.
- **code:** none (configuration and grading only).
- **run dirs:** `D:\GnssProcessorSupport\runs_nom\_rap_ufqq_<arm>_<doy>_quin`; tools in `D:\GnssProcessorSupport\gates\day_program_2026-11-20\user_fix\quin\`.
- **source:** the managing session's request of 2026-11-21 (after card [UFQ](UFQ-USER-FIX-QUALITY.md) item 1); the AGU refresh experiment (`presentation/agu26/figures/v3_integer/tools/refresh_agu.py`, reference `refresh_arm1120`); memory note on the receivers without Galileo antenna calibrations (BADG00RUS, KOKB00USA, QUIN00USA).

**Outcome.** Open; pre-registered. The fixing user at QUIN00USA sits 3.43 cm high on five warm starts while standard PPP is reported at 0.05 cm; the other 16 test receivers show no such gap. The card looks for the term the user's path applies differently from standard PPP, in the order of the managing session's notes: the antenna entries both engines resolve, a GPS-only and a Galileo-only user, the receiver models term by term, the elevation signature with the wet delay, and the network side last. A fix must bring QUIN inside the other receivers' spread without moving them, default off.

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
