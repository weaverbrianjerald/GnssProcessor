# SNX: the IGS weekly SINEX against the held station coordinates, and the plan for a coordinate source

**STATUS**

- **id:** SNX
- **topic:** network-vehicle
- **status:** open
- **summary:** 2026-10-19: the IGS weekly combined SINEX of GPS week 2313 against the coordinates the field runs hold (58 roster stations, WTZZ00DEU, 16 census sites; MIZU00JPN added as a suspect). The offsets reach the centimetre level: median 3D 6.0 mm, 20 of 71 stations above 1 cm, 4 above 5 cm, MIZU00JPN 1.68 m. Frame and marker reduction are not the cause. A default-off coordinate source is planned (section 8); nothing is built. Addendum 2026-10-19: the NGS SIF post-seismic term double counted the displacement accumulated by 2020.0 and is fixed on branch fix/sif-psd; the SIF is then within 3.1 mm per horizontal component of the SINEX at seven of the eight roster stations with an active term (MIZU00JPN 1.48 m to 4.0 mm in 3D). Phase 1 arms (card [SNXB](SNXB_sinex_roster_arms_2026-10-19.md) section 10, 2026-10-19): the roster at its SINEX coordinates moves the emitted satellite clocks by 5 to 7 mm RMS (a per-satellite level) and no consumer by more than 2.4 mm; scoring on the SINEX instead of the held coordinates moves the census median by -1.35 mm (CEDU00AUS -11.5 mm).
- **source:** the author's request of 2026-10-19 (one download approved, then this plan); [SCU](SCU_station_coordinates_unknowns_2026-09-22.md) sections 3, 4.1 and 7; [UPO](UPO_consumer_up_offset_2026-09-22.md) section 4
- **knobs / posture:** none built in the engine. Phase 1 tooling is built (card SNXB section 10: `scripts/roster_from_sinex.py`, `rapid_arm.py make --roster`, `gcensus.py --sites`, `scripts/clock_level/snx_roster_grade.py`, commit `69e3dfd` on branch `card/snxroster`); nothing is armed. Proposed: a roster file written by `scripts/roster_from_sinex.py` and spliced into arm configs (phase 1, no engine change); a loader key `network.coordinate_source` only if phase 1 shows an effect (section 8.2). Posture: the field golden `config/golden/golden_field_refound.json` (58 stations, GPS + Galileo; baseline pair `runs/_rf0hv15_{field,sim}6`)
- **dates in the text:** 2026-10-19
- **hashes / run dirs:** file `IGS0OPSSNX_20241260000_07D_07D_SOL.SNX.gz`, 9,995,953 bytes, sha256 `74a412dc1d09db48...`; decompressed 25,683,008 bytes, sha256 `b05b5dbe80ea8958...`. Download copy `D:/GnssProcessorSupport/downloads/sinex_2313/`; working copy `data/2024-129/products/` (git-ignored); decompressed copy for the resolver cross-check `D:/GnssProcessorSupport/gates/sinex/work/`. Instruments and outputs `D:/GnssProcessorSupport/gates/sinex/`: `cddis_auth.py`, `cddis_list.py`, `cddis_get.py`, `snx_parse.py`, `snx_compare.py`, `snx_extra.py`, `ssc_inspect.py`, `out/sinex_vs_held.csv`. Phase 1 arms (card SNXB section 10): instruments and roster files `D:/GnssProcessorSupport/gates/snxroster/`, arms and census replays `D:/GnssProcessorSupport/runs_nom/_rap_snxr*`, exe `D:/GnssProcessorSupport/gates/exe/x64-Release/gnss_processor_snxroster.exe` (sha256 `3d6c679e8068a39879c582b4495e069c569a3706049826f96f6123e2267a288e`).

**Outcome.** Question: do the coordinates the field runs hold (the ITRF2020 SSC, extrapolated 3.35 years past its last window) agree with the current IGS weekly combined SINEX? Result: they differ at the centimetre level. Of 71 stations (54 roster, WTZZ00DEU, 16 census) the median 3D offset is 6.0 mm, 20 exceed 1 cm and 4 exceed 5 cm (MAC100AUS 64 mm, PARK00AUS 62, GCGO00USA 56, SOLO00SLB 54); WTZZ00DEU is 4.5 mm off; MIZU00JPN is 1.68 m off. Frame (at most 1.3 mm) and marker versus ARP (71 of 71 identical) are not the cause. The SINEX does not explain the Asian Up bias, MAJU00MHL or POHN00FSM. A default-off coordinate source is planned in section 8. Addendum: the disagreement of the SIF with the SINEX (section 6: MAC100AUS, GCGO00USA, NIUM00NIU, COCO00AUS) was a defect of the SIF evaluator, not of the SIF; fixed (the addendum at the end), after which the SIF is a second source within a few millimetres of the SINEX, and the held offsets at GCGO00USA, COCO00AUS and NIUM00NIU equal the post-seismic term the SSC lacks. Section 10 of the continuation card [SNXB](SNXB_sinex_roster_arms_2026-10-19.md) runs its phase 1 on the field golden (6 h; the roster at its SINEX values, six stations or 54): the satellite clocks move by 5 to 7 mm RMS, the consumers by at most 2.4 mm, so the roster is a null for the consumer, and the SINEX truth moves the census more than the roster does.

## 1. The question and the file

The roster, the held-out consumer and the 16 census sites hold ITRF2020 coordinates from the IGS SSC (`scripts/resolve_itrf_ecef.py`), extrapolated linearly from the last solution window of each station, which ends at 2021.0 for most. Card SCU found one station, MIZU00JPN, 1.6 m off its data; the SSC ends at 2021.0 and the resolver applies no post-seismic model. The IGS weekly combined solution is an independent, current coordinate for the same stations. The question: how far apart are the two, per station, and is the difference a frame, an epoch, a marker or a station effect?

The download, as approved (one file):

- The directory `https://cddis.nasa.gov/archive/gnss/products/2313/` was listed first. The requested name was present, listed at 9.53 MB, 2024-05-23 07:30 UT; the Content-Length of the transfer is 9,995,953 bytes. No other file was fetched. The same directory lists a coordinate-only file of the same week (`IGS0OPSSNX_20241260000_07D_07D_CRD.SNX.gz`, 90.56 KB); it was not fetched and its content is unread.
- The Earthdata login (the `urs.earthdata.nasa.gov` entry of the existing netrc) was used through the netrc helpers of `scripts/bootstrap_internal_data.py`; the credentials were never printed. One adaptation was needed: the stock opener answers only a 401 Basic challenge, and on 2026-10-19 the Earthdata endpoint answered the first request with its HTML login form (status 200), so the directory URL returned the login page. The scratch module `cddis_auth.py` offers the same Basic credentials preemptively and only to `urs.earthdata.nasa.gov` (never to CDDIS or any other host). The repository helper is unchanged (section 9).
- The file decompresses completely (25,683,008 bytes) and was copied, byte-identical (equal sha256), to `data/2024-129/products/`. That directory is git-ignored (`.gitignore` line 37). A search of `src/` and `include/` for a `.snx` or `.SNX` extension finds nothing, and the file is not in `data/static/sinex/`, where the Python truth factory loads every `*.snx`, so no default of any run or grader changes.
- The file was treated as data: parsed by `snx_parse.py` (standard library only), kept outside the download directory, run with `python -I`.

## 2. What the file holds

- SINEX 2.02, created by IGN on 2024-05-23 07:25:32 UT, software CATREF. Description: weekly combination of the IGS daily combined solutions of week 2313 (seven daily inputs, 2024-05-05 to 2024-05-11). Data span `24:126:00000` to `24:133:00000`, that is 2024-05-05 00:00 to 2024-05-12 00:00 UT. Constraint code 2 (unconstrained). 1379 parameters: 447 stations times STAX, STAY, STAZ (1341), the daily polar motion and length of day (35 rows) and three geocentre rows.
- **Epoch.** Every coordinate row has the reference epoch `24:129:43200`: 2024-05-08 12:00:00 UT, decimal year 2024.35109, the middle of the week and of DOY 129. **The file is a weekly mean.** It has no velocity parameters (no VELX, VELY, VELZ), one point (A) and one solution number per station (SOLN 1 to 16 among the stations compared).
- The frame is not named in the file (no comment block). IGS weekly combinations of this period are aligned to IGS20; section 5 measures the difference to ITRF2020 instead of assuming it.
- Formal sigma of the largest axis: median 0.57 mm, largest 2.3 mm (ANMG00MYS). Two stations have a short data span: MAC100AUS (to `24:131:00000`) and LCK400IND (to `24:131:86370`).

## 3. What the runs hold, reproduced

| set | where it is read | stations |
| --- | --- | --- |
| roster | `network.stations[].position_ecef`, `config/golden/golden_field_refound.json` | 58 |
| consumer | `network.closed_loop.station.position_ecef`, the same file (WTZZ00DEU; used only as truth, after the same reduction) | 1 |
| census | `sites[].ecef` of `scripts/clock_level/site_list.json` (identical to `runs/_msc/site_list.json`), installed as the consumer coordinate by `gcensus.py` | 16 |
| suspect | MIZU00JPN, dropped from the roster by card SCU; the resolver's value | 1 |

- Each is a monument (marker) coordinate. The engine reduces the roster, and the truth of the consumer, to the antenna reference point with the RINEX `ANTENNA: DELTA H/E/N` under `network.station_marker_to_arp` (`ApplicationNetworkSetup.cpp`, `buildStation`). The comparison below is marker to marker.
- **Reproduction.** `SinexTruth` (`pyviz/gnssviz/eval/truth.py`) on `data/static/sinex/ITRF2020-IGS-TRF.SSC` at decimal year 2024.349727 (DOY 129, 00:00 UT, the resolver's default) gives all 76 held values within 0.074 mm (the printed precision), none above 0.1 mm. The held value is the SSC's linear extrapolation exactly. MIZU00JPN equals the value in card SCU to 0.04 mm.
- **Extrapolation distance.** The last solution window of the SSC ends at 2021.000 for most stations (earliest 2020.246). The runs sit 3.35 years past it (median; largest 4.11).

## 4. The offsets (SINEX minus held)

Local East, North, Up at the station and the 3D length, in millimetres. 72 stations are in the file out of the 76 held; the four absent are all roster stations: BSHM00ISR, FFMJ00DEU, LEIJ00DEU, WTZS00DEU (each is in the SSC). Per station: the appendix.

| set | compared | median 3D | 90th percentile | largest | above 5 mm | above 1 cm | above 5 cm |
| --- | --- | --- | --- | --- | --- | --- | --- |
| roster | 54 of 58 | 7.1 | 37.2 | 64.0 | 35 | 16 | 4 |
| consumer WTZZ00DEU | 1 | 4.5 | | 4.5 | 0 | 0 | 0 |
| census | 16 of 16 | 5.2 | 14.2 | 18.0 | 9 | 4 | 0 |
| **all** | **71 of 75** | **6.0** | **23.1** | **64.0** | **44** | **20** | **4** |

- Components over the 71: median absolute East 1.6, North 1.7, Up 4.7 mm; mean East -1.4, North -2.6, Up -2.0 mm. The Up component carries most of the median (part of it is the week's seasonal and non-linear departure from the secular model, section 6).
- Above 5 cm: MAC100AUS 64.0 (North -62.2), PARK00AUS 62.2 (East -31.5, North -48.1, Up -23.7), GCGO00USA 56.3 (East +24.5, North -50.7), SOLO00SLB 54.5 (East -27.2, North -43.0, Up -19.4). Between 1 and 5 cm: REYK00ISL 44.6, LCK400IND 42.9, NIUM00NIU 23.8, COCO00AUS 23.1, URUM00CHN 18.9, ASCG00SHN 18.0 (census), PALM00ATA 16.7, MBAR00UGA 16.3, CUSV00THA 15.9, CEDU00AUS 15.1 (census), QUIN00USA 13.3 (census), CPVG00CPV 12.1, UTQI00USA 11.3 (census), BOGT00COL 11.2, SCTB00ATA 10.2, STJO00CAN 10.0.
- The consumer truth moves by (East -0.1, North +1.3, Up -4.3) mm. Its model position (the SINEX a priori) and the NGS SIF both agree with the held value (3D 1.2 and 0.5 mm), so the shift is the week's departure from the model (Up -3.5 mm: seasonal or non-linear motion, or weekly noise), and not an error of the held coordinate in the sense of the larger offsets (section 6).
- The 1 cm threshold separates; the 5 mm one does not (44 of 71 are above it, with the median at 6.0 mm).

## 5. Epoch, frame and marker

- **Epoch.** The weekly mean refers to 12:00 UT of DOY 129; the held values to 00:00 UT (decimal year 2024.349727); the 6 h arms run 00:30 to 06:30 UT. The SSC velocity moves a station by 0.035 mm over that half day (median) and 0.103 mm at most (MAJU00MHL); to either end of the week (3.5 days) it is at most 0.7 mm. **Propagation does not matter at this time span**, and the file carries no velocity to propagate with.
- **Frame.** A seven-parameter fit of the offsets on 65 of the 71 stations (clipped at three robust sigma, floor 3 mm) gives a translation of (-0.91, +0.23, +0.15) mm, a scale of -0.20 ppb (-1.3 mm at the surface) and rotations of 0.036, 0.011 and -0.022 mas. Removing it does not reduce the offsets (median 6.3 mm against 6.0; above 1 cm 24 against 20). The offsets are station effects, not a frame mismatch. The file's own geocentre rows read (-0.81, -0.63, +2.24) mm, sigma 0.3 to 0.4 mm.
- **Marker versus ARP.** SINEX coordinates refer to the marker; `SITE/ECCENTRICITY` gives the ARP offset. The RINEX `ANTENNA: DELTA H/E/N` that the engine reduces by was compared with the SINEX eccentricity in force at `24:129:43200` at all 71 stations: identical (largest difference 0.0 mm at the 0.1 mm print precision); 57 have a nonzero height, up to 3.052 m (HARB00ZAF). The Up offsets follow no antenna height (slope 1.2 mm per metre, correlation 0.08; the seven stations with a height of 0.5 m or more read -11.9 to +7.4 mm). The held coordinates and the SINEX are at the same reference point and the same reduction applies to both.

## 6. Where the offsets come from

- **A priori and week.** The file carries a model position at the same epoch for every station (`SOLUTION/APRIORI`; its source is not stated in the file). Over the 71: a priori minus held has a 3D median of 0.9 mm but a 90th percentile of 21.7 mm and a largest of 64.7 mm; weekly estimate minus a priori has a median of 3.3 mm (Up 2.6 mm), a 90th percentile of 8.5 mm and a largest of 18.8 mm. The large offsets are the model's difference from the SSC extrapolation; the week's own departure from the model (seasonal and non-linear motion, noise) is a few millimetres. In central Europe that part is regionally coherent: the nine stations read -7.7 to +0.1 mm in Up against the held value (median -4.7 mm), the week's departure from the model is -3.5 mm (median) and the model equals the held value there (median 0.0 mm). A truth built on the weekly mean contains it; the secular value does not.
- **Solution numbers.** 14 of the 71 stations have a SINEX solution number above the last solution of the SSC, that is, a break after the SSC ends (an earthquake, an equipment change, a new segment); their 3D median is 20.6 mm (largest 64.0), 13 of them above 1 cm: SOLO00SLB (6 to 8), PALM00ATA (4 to 5), STJO00CAN (7 to 8), MAC100AUS (9 to 10), NIUM00NIU (5 to 7), COCO00AUS (7 to 8), PARK00AUS (4 to 5), LCK400IND (1 to 3), CPVG00CPV (4 to 6), REYK00ISL (10 to 12), QUIN00USA (8 to 9), ASCG00SHN (1 to 3), CEDU00AUS (7 to 8). MIZU00JPN also (12 to 16). The other 57 have the same number: 3D median 5.3 mm, largest 56.3; seven above 1 cm: GCGO00USA 56.3, URUM00CHN 18.9, MBAR00UGA 16.3, CUSV00THA 15.9, UTQI00USA 11.3, BOGT00COL 11.2, SCTB00ATA 10.2.
- **A short last window.** The extrapolation uses the velocity of the last window. For PARK00AUS that window is 0.75 years long (2020.251 to 2021.0) with an X velocity of -55.1 mm/yr against -30.8 mm/yr in the window before; over 3.35 years the difference is about 80 mm in X. It is one contributor there; the break (4 to 5) is the other, and the two are not separated here.
- **A third source.** The NGS SIF (`data/static/sif/igs20_SIF.json`: linear, periodic and post-seismic terms) at the SINEX epoch, for the 63 of 71 stations it holds: 3D difference to the SINEX, median 4.1 mm, 90th percentile 11.6 mm, largest 55.3 mm; to the held value, median 4.0 mm, 90th percentile 44.9 mm, largest 113.3 mm. It agrees with the SINEX within 13 mm at PARK00AUS (10.3), SOLO00SLB (10.6), REYK00ISL (12.7), LCK400IND (2.1), ASCG00SHN (2.2) and URUM00CHN (11.7), where the SSC extrapolation is the outlier. It does not at MAC100AUS (51.1), GCGO00USA (55.3), NIUM00NIU (31.9) and COCO00AUS (24.9).
- **GCGO00USA is not explained.** Its solution number is unchanged (2), the SINEX estimate agrees with the SINEX a priori to 2.6 mm (so the a priori carries the offset: it is 54 mm from the held value), and the three sources are pairwise 55 to 111 mm apart (SINEX against held 56, SINEX against SIF 55, SIF against held 111). Unread (section 9).
- The extrapolation error grows with the distance from 2021.0 and with every break since; the SINEX is the position on the day.

## 7. The suspects

| station | SOLN (SSC last) | East | North | Up | 3D | what card SCU read |
| --- | --- | --- | --- | --- | --- | --- |
| MIZU00JPN | 16 (12) | +1527.0 | -698.7 | -22.4 | 1679.5 | leave-out replay estimate (+1471.5, -636.6, -120.4) mm; the SINEX differs from it by (+55.5, -62.1, +98.0) mm |
| MAJU00MHL | 5 (5) | +0.2 | +0.2 | -0.1 | 0.3 | replay found no coordinate error at 0.04 m (Up -0.040 m), section 4.1 |
| POHN00FSM | 7 (7) | +4.0 | -3.2 | +0.4 | 5.1 | census East mean +0.136 m after the drop: not the truth coordinate |
| SOLO00SLB | 8 (6) | -27.2 | -43.0 | -19.4 | 54.5 | replay (-78, -46, +84) mm: North agrees, East by a third, Up not |
| LCK400IND | 3 (1) | -7.4 | -12.3 | -40.4 | 42.9 | replay (+31, -34, -169) mm: the Up sign agrees, a quarter of the size |
| ULAB00MNG | 5 (5) | +0.9 | +0.7 | -2.1 | 2.4 | replay (+51, +50, -146) mm: not a coordinate |
| WUH200CHN | 6 (6) | +0.9 | -1.5 | -1.2 | 2.1 | census Up -0.181 m: not a coordinate |
| BADG00RUS | 8 (8) | -1.1 | +2.7 | -4.7 | 5.5 | census Up -0.167 m: not a coordinate |
| URUM00CHN | 11 (11) | +0.4 | +7.7 | -17.2 | 18.9 | roster replay Up -0.125 m: a seventh |

- **MIZU00JPN.** The weekly SINEX puts the station 1.68 m from the held coordinate, towards the east-southeast (East +1.527, North -0.699 m), and its solution number is four above the SSC's. The data-derived leave-out estimate of card SCU (E +1.4715, N -0.6366, U -0.1204 m) lands within 0.1 m of it in each component. That confirms the size and direction of the error from a source that does not depend on this network; the cause (post-seismic motion of the 2011 Tohoku earthquake, any later break) is not separated by the SINEX, which gives the position and not a model. The SINEX coordinate is `-3857171.2296, 3108692.7812, 4004040.1136` against the held `-3857169.9416, 3108693.7043, 4004040.6697`.
- **The Asian Up bias (-0.13 to -0.18 m, card SCU section 4.1).** Not explained. At the stations that show it the SINEX offsets in Up are -2.1 (ULAB00MNG), -1.2 (WUH200CHN), -4.7 (BADG00RUS), -17.2 (URUM00CHN) and -40.4 mm (LCK400IND); the median Up of the ten Asian stations (latitude 5 to 60, longitude 60 to 145) is -1.6 mm. Four of the five read below 20 mm and three below 5 mm; even at LCK400IND the SINEX accounts for a quarter of the replayed -0.169 m. The bias is not carried by the coordinates; card SCU's reading, a regional bias of the wire, stands.
- **MAJU00MHL.** The coordinate agrees to 0.3 mm. The second-largest congruence statistic of the roster is not a coordinate (as the leave-out replay of card SCU found at 0.04 m).
- **POHN00FSM.** The truth coordinate is within 5.1 mm of the SINEX; the worsening after the MIZU00JPN drop is not a truth error.
- **SOLO00SLB, LCK400IND.** Both are in the roster and both are held with errors of the size read (54.5 and 42.9 mm), with new solution numbers behind them. SOLO00SLB's open item (the replay's East -0.078 m, card SCU sections 4.1 and 7) is partly a held coordinate (East -27 mm, North -43 mm).

## 8. Plan: a coordinate source, default off (not built)

### 8.1 Is it needed

Yes: the offsets reach the centimetre level (20 of 71 above 1 cm, 4 above 5 cm, one station 1.68 m), the roster has 16 stations of 54 above 1 cm, four census truths move by more than 1 cm (ASCG00SHN 18.0, CEDU00AUS 15.1, QUIN00USA 13.3, UTQI00USA 11.3 mm) and the consumer truth by 4.5 mm (the week's departure from the model) against an hourly p50 of 0.010 to 0.018 m in hours 2 to 6. The effect on the producer is expected to be small (section 8.4); the case is the correctness of the truth, the stations that break (MIZU00JPN), and the growth of the SSC extrapolation error with time.

### 8.2 Design

**Phase 1: no engine change, default off by construction.**

1. `scripts/roster_from_sinex.py` (new, owed). Inputs: a weekly SINEX (`.snx` or `.snx.gz`), the config whose roster is replaced (default: the field golden), the SSC for the fallback. The parser is `SinexTruth` of `pyviz/gnssviz/eval/truth.py`, which already reads the weekly file unchanged: `scripts/resolve_itrf_ecef.py --ssc <decompressed weekly>` reproduced this card's parse to the digit at GCGO00USA, MAC100AUS, MIZU00JPN, WTZZ00DEU and JPLM00USA; its only gap is `.gz` input (and the label "ITRF2020" in its header line).
2. Rules of the generator: the station is matched on its four-character code, and refused when the SINEX position is more than 1 km from the config's current coordinate (a reused code); a station absent from the file keeps its current coordinate and is named and counted in the report (`source: config`); marker coordinates only (the engine reduces to the ARP); no velocity propagation (the weekly has none) and the epoch recorded; a station with a data span below six days is flagged (MAC100AUS, LCK400IND).
3. Output: a JSON roster `config/refound/roster_sinex_<yyyyddd>.json` (name, `position_ecef` to 0.1 mm, source, SOLN, sigma, offset from the config, the file name and sha256, the epoch) and a text table in the form of section 4. The existing declarations `network.frame_id` and `network.coord_epoch` (S-contract fields, `GnssConfigLoadNetwork.cpp`) are written into the arm with the epoch 2024.3511.
4. Arm tools: a `rapid_arm.py` op that replaces `position_ecef` of `network.stations[]` from a roster file (as op `ecc` edits the coordinate today) and one that replaces `network.closed_loop.station.position_ecef`; an arm is the golden with only the roster, or only the truth, changed. The goldens and the manifest are not edited. A SINEX-roster golden is a new manifest entry, added only by the author's decision, whose base differs in the roster alone.

**Phase 2: a loader key, only if phase 1 shows an effect or the source is to be declared in the run.**

- Where. Roster coordinates enter in `GnssConfig::loadNetworkRosterAndDatum_` (`src/core/config/GnssConfigLoadNetwork.cpp`: the `addStation` lambda reads `position_ecef`; the loop below resolves a missing one from the SIF under `network.use_sif_coordinates`, with explicit JSON winning). The consumer's enters in the closed-loop section of the same file; at run time `buildStation` copies `cfg.position_ecef` and applies `markerToArp`.
- What. `network.coordinate_source` (`"config"` default; `"roster_file"` with `network.coordinate_roster_path`), the armed file overriding the coordinate of every roster station it names and marking `StationConfig::position_source` (generalizing `position_from_sif`). The precedence differs from `use_sif_coordinates` (which only fills missing coordinates) and the key's documentation states it. Stations the file lacks keep the config coordinate.
- Declared and counted. A run-start table (source, SOLN, file hash, shift against the config, per station) and run-end counters (replaced, kept, largest shift; invariant 4). The snapshot fingerprint takes a token `roster:<hash>` appended only when armed, as `roster:arp` is: the fingerprint hashes station names but not coordinates, so a resume under another roster would not cold-start today. A ledger row (UNVALIDATED) and a test file in `tests/network/`; default off is byte-identical (no token, no line), proved by the same-build golden comparison.
- What it is not. The engine does not read a SINEX. `truth.py` states the reason: the grader is independent because the engine reads no SINEX or SSC; the roster is JSON written offline, and the SINEX stays a Python-side input.

### 8.3 The grading truth, switched by the same source

- The consumer's truth is `network.closed_loop.station.position_ecef` (used only as the consumer's truth: `cl_truth_arp` in the network setup and `truth` in the replay driver, each reduced marker to ARP by the consumer's RINEX eccentricity). The census truths are `sites[].ecef`. The generator writes both: the consumer entry into the arm, and `site_list_sinex.json` beside `site_list.json`. `msc_sites.load()` takes an environment override for the file name (default unchanged), and `gcensus.py make` already sets each site's coordinate from it.
- The weekly is not placed in `data/static/sinex/`: `make_default_truth` loads every `*.snx` there and would switch the truth of the real-data harness for every run.
- One truth per grade. Every number quoted carries its truth key (`ssc` or `sinex`); no table mixes the two.

### 8.4 Arms and bars

Field only (the sim bed has known positions, card SCU section 8). Release, one exe renamed per CLAUDE.md, cold start (a snapshot written under another roster is not resumed: the fingerprint cannot see the change before phase 2), runs under `RAPID_RUNS_ROOT=D:/GnssProcessorSupport/runs_nom`, 2 h development check first, 6 h for the grade.

| arm | change from the golden | purpose |
| --- | --- | --- |
| C | none (the same-exe control) | the baseline of the pair |
| S1 | the 54 present roster stations from the SINEX, the four absent kept; consumer truth unchanged | the roster effect on the wire, the consumer and the census |
| S2 | no run: C and S1 re-graded against the SINEX truth (consumer and 16 census replays on each wire) | the truth effect; the standing comparison, both arms on one truth |
| S3 | S1 plus MIZU00JPN restored at its SINEX coordinate (59 stations) | the post-seismic station on the roster again with no network-derived number; against arm `m2` of card SCU (0.0044 / 0.0028 m) |

Instruments: the held-out consumer WTZZ00DEU by hour (hours 2 to 6 settled, hour 1 ignition); the 16-site census by `scripts/clock_level/gcensus.py` (the wire is GPS + Galileo: the GPS-only graders are not used); the clock wire against the reference clocks (all and consumer-tracked); run-end counters (refloats, anchor events, trips).

Bars, proposed for the author to set (none is a result):

- G0. The goldens and the manifest unchanged, `python scripts/make_golden_config.py --verify` passes; C is byte-identical on `products.txt`, `closed_loop_trace.csv` and `network_epoch_status.csv` to `runs/_rf0hv15_field6` when the exe is the same, else it is the baseline of the pair.
- B1. WTZZ00DEU: S1 within 0.003 m of C at the hourly 3D p50 in hours 2 to 6, both against the SINEX truth.
- B2. Clock wire: S1 not worse than C by more than 0.0002 m (all) and 0.0001 m (consumer-tracked).
- B3. Census, paired per site against C on one truth: the median change of the hourly p50 (S1 minus C) not positive; no site worse than 0.010 m in the mean over hours 2 to 6.
- B4 (S3). Clock wire at most 0.0046 / 0.0029 m (card SCU's figure); congruence statistic at MIZU00JPN below 4.

Expected result, stated in advance. Scaling card SCU section 3.1 linearly (1.68 m, 0.0051 m in quadrature on the all-satellite wire), the four stations above 5 cm contribute a few 0.0001 m. The consumer truth moves 4.5 mm (Up -4.3) against hourly p50 of 0.010 to 0.018 m; the four census truths above 1 cm move against a census median p50 of 0.067 to 0.103 m in hours 2 to 6 (card SCU section 8, GPS-only base). A null S1 is a pass for the roster and the truth (no harm), and then the case rests on the truth and on the stations that break, not on a gain.

### 8.5 Real-time caveat and the self-contained goal

- **Weekly finals are late.** This file was created 11 days after its week ended (2024-05-23 against 2024-05-12) and listed that morning. It cannot seed a roster for the week it covers or for a live service. It also has no velocity: a roster built from it is exact at 2024.351 and ages at the station velocity (the loader's own advisory assumes 2 to 7 cm per year; about 5 mm per month at 6 cm per year). The existing `network.coord_epoch` and `network.coord_age_warn_years` advisory reads the declared epoch for this purpose, and the generator sets it.
- **The real-time source.** The IGS20 cumulative solution (positions, velocities, discontinuities) with its post-seismic models, fixed once per deployment and propagated to the service epoch, refreshed from a new weekly when a station breaks. The weekly's own a priori block is evidence for it: it agrees with the week to 3.3 mm (3D median; Up 2.6 mm; 90th percentile 8.5 mm; largest 18.8 mm) while differing from the SSC extrapolation by 21.7 mm at the 90th percentile. That the block is the IGS20 model is presumed (the file does not say). The cumulative file and the post-seismic models were not fetched or read.
- **The self-contained goal.** Coordinates stay a static frame input, like the frame itself: a roster generated offline, stamped with the file name, hash and epoch, read by the engine as JSON; no download at run time, the same file for the real-time engine. A coordinate source that is a CDDIS product at run time is not proposed. The end state with no external coordinate file is card SCU section 6 (constrained states with a release); this card supplies that build's external truth. Its gate G3 reads MIZU00JPN's correction against (+1.47, -0.64, -0.12) m, the network-derived estimate; the SINEX value is (+1.527, -0.699, -0.022) m, 0.06, 0.06 and 0.10 m from it, so the 0.05 m bar needs a reference restated by the author before that build.

### 8.6 Invariants, pre-flight

- Invariant 1: no time-linked zero-q quantity is added; a roster change is a different run, cold start only.
- Invariant 3: the four absent stations keep their SSC value; "field-inert" is not shown for them (no data), and the report names them.
- Invariant 4: in phase 1 the generator's report is the telemetry (replaced, kept, absent); phase 2 adds the run-start table and run-end counters.
- Invariant 6: phase 1 changes no file the engine reads except arm configs; phase 2 is default off and byte-identical.
- Invariant 7: not applicable; an input value, not a mechanism that changes what information reaches which states.

## 9. What stays open

- GCGO00USA: three sources, 55 to 111 mm apart, one solution number (section 6). Needs the IGS20 cumulative record of the station.
- The IGS20 cumulative SINEX and the post-seismic models: not fetched; a second download needs the author's approval.
- A daily SINEX for DOY 129: the weekly mean against the day itself is not measured here (the week's departure from its model, 3.3 mm median, is the nearest figure).
- The four roster stations absent from the file (BSHM00ISR, FFMJ00DEU, LEIJ00DEU, WTZS00DEU): their SSC values are unverified.
- `scripts/bootstrap_internal_data.py`: its opener returned the Earthdata login page (status 200) for the directory URL on 2026-10-19, and `download_file` writes whatever a 200 returns to the destination without a content check. A file URL is expected to take the same redirect (not tried: no other download was authorized), so a bootstrap run may store a login page as a product. Not changed here.
- The Asian Up bias of card SCU section 4.1 and the open items of card SCU section 7 other than the coordinates: unchanged.

## Appendix: per-station offsets, SINEX minus held (mm)

SOLN is the SINEX solution number; sigma is the largest formal axis sigma of the SINEX. "absent" means the station has no coordinate in the file.

| station | set | SOLN | East | North | Up | 3D | sigma |
| --- | --- | --- | --- | --- | --- | --- | --- |
| BOGT00COL | roster | 12 | +5.1 | -7.9 | +6.0 | 11.2 | 0.8 |
| BOR100POL | roster | 3 | +0.2 | +0.1 | -4.1 | 4.1 | 0.5 |
| BSHM00ISR | roster | absent | | | | | |
| CAS100ATA | roster | 6 | -0.5 | +1.6 | -2.3 | 2.8 | 0.5 |
| CEBR00ESP | roster | 3 | -1.5 | +0.9 | -5.5 | 5.8 | 1.0 |
| CHUR00CAN | roster | 7 | -1.9 | +0.3 | +2.5 | 3.1 | 0.4 |
| COCO00AUS | roster | 8 | +13.2 | +18.3 | -5.0 | 23.1 | 0.6 |
| CPVG00CPV | roster | 6 | +5.6 | +9.5 | -5.0 | 12.1 | 0.6 |
| CUSV00THA | roster | 4 | +3.3 | +12.2 | +9.6 | 15.9 | 0.6 |
| DARW00AUS | roster | 6 | -0.0 | -1.7 | -9.0 | 9.2 | 0.5 |
| DJIG00DJI | roster | 3 | -0.6 | -1.9 | -0.7 | 2.1 | 0.4 |
| FFMJ00DEU | roster | absent | | | | | |
| GAMB00PYF | roster | 5 | +4.9 | -5.0 | -4.1 | 8.1 | 0.5 |
| GCGO00USA | roster | 2 | +24.5 | -50.7 | -1.9 | 56.3 | 1.5 |
| GOP600CZE | roster | 2 | +0.5 | +1.3 | -4.7 | 4.9 | 0.8 |
| GOPE00CZE | roster | 8 | +0.3 | +1.1 | -5.5 | 5.6 | 0.9 |
| GRAZ00AUT | roster | 9 | +0.6 | +1.0 | +0.1 | 1.2 | 0.6 |
| GUAM00GUM | roster | 5 | +6.1 | +2.2 | +7.4 | 9.9 | 0.5 |
| HKSL00HKG | roster | 2 | -1.0 | +2.5 | -4.0 | 4.8 | 0.6 |
| HOLB00CAN | roster | 12 | -2.8 | +6.2 | -6.8 | 9.6 | 0.4 |
| JPLM00USA | roster | 7 | +0.2 | +5.5 | -4.3 | 7.0 | 1.2 |
| KIT300UZB | roster | 3 | -7.0 | +0.2 | -0.3 | 7.0 | 0.6 |
| KOKV00USA | roster | 1 | -2.3 | +0.6 | -2.5 | 3.4 | 0.7 |
| KRGG00ATF | roster | 1 | +2.5 | -0.3 | +1.0 | 2.7 | 0.4 |
| LCK400IND | roster | 3 | -7.4 | -12.3 | -40.4 | 42.9 | 1.2 |
| LEIJ00DEU | roster | absent | | | | | |
| LMMF00MTQ | roster | 3 | +1.2 | -3.0 | +8.3 | 8.9 | 0.5 |
| MAC100AUS | roster | 10 | +13.9 | -62.2 | +4.6 | 64.0 | 1.0 |
| MAJU00MHL | roster | 5 | +0.2 | +0.2 | -0.1 | 0.3 | 1.0 |
| MBAR00UGA | roster | 2 | +1.8 | -1.7 | -16.1 | 16.3 | 0.5 |
| MRC100USA | roster | 1 | +1.5 | +0.3 | +3.9 | 4.2 | 1.4 |
| MRO100AUS | roster | 2 | -0.5 | -0.6 | -2.7 | 2.8 | 0.6 |
| NIUM00NIU | roster | 7 | -22.7 | -1.4 | +7.2 | 23.8 | 0.7 |
| NYA200NOR | roster | 4 | +1.2 | -1.4 | +9.0 | 9.2 | 0.9 |
| PALM00ATA | roster | 5 | +2.0 | -3.3 | -16.3 | 16.7 | 0.8 |
| PARK00AUS | roster | 5 | -31.5 | -48.1 | -23.7 | 62.2 | 0.9 |
| POTS00DEU | roster | 6 | +0.5 | +0.4 | -6.8 | 6.8 | 0.5 |
| POVE00BRA | roster | 4 | +1.3 | -1.3 | +1.7 | 2.5 | 0.5 |
| PTBB00DEU | roster | 3 | +1.6 | +1.9 | -7.7 | 8.1 | 0.5 |
| REYK00ISL | roster | 12 | -37.1 | -3.4 | +24.4 | 44.6 | 0.6 |
| SAVO00BRA | roster | 2 | -2.0 | -2.2 | -9.5 | 10.0 | 0.5 |
| SCTB00ATA | roster | 4 | -0.4 | -0.0 | +10.2 | 10.2 | 1.7 |
| SEYG00SYC | roster | 1 | +1.3 | -1.0 | +6.9 | 7.1 | 0.5 |
| SGOC00LKA | roster | 3 | -3.1 | -1.4 | +3.6 | 4.9 | 0.6 |
| SOLO00SLB | roster | 8 | -27.2 | -43.0 | -19.4 | 54.5 | 0.9 |
| STHL00GBR | roster | 1 | +0.4 | -0.4 | +2.7 | 2.7 | 0.6 |
| STJO00CAN | roster | 8 | -4.5 | +3.7 | +8.2 | 10.0 | 0.5 |
| SVTL00RUS | roster | 3 | +1.3 | +0.8 | -5.8 | 6.0 | 0.6 |
| SYOG00ATA | roster | 7 | +1.2 | +0.2 | -1.8 | 2.2 | 0.5 |
| ULAB00MNG | roster | 5 | +0.9 | +0.7 | -2.1 | 2.4 | 0.4 |
| URUM00CHN | roster | 11 | +0.4 | +7.7 | -17.2 | 18.9 | 0.4 |
| VOIM00MDG | roster | 1 | +1.0 | +0.1 | -5.3 | 5.4 | 0.5 |
| WARK00NZL | roster | 3 | -2.6 | -1.3 | -1.0 | 3.0 | 0.6 |
| WIND00NAM | roster | 6 | -3.5 | -2.5 | +6.1 | 7.4 | 0.5 |
| WROC00POL | roster | 5 | -0.2 | +0.8 | -5.5 | 5.6 | 0.7 |
| WTZR00DEU | roster | 3 | -0.6 | +1.8 | -1.7 | 2.6 | 0.4 |
| WTZS00DEU | roster | absent | | | | | |
| YKRO00CIV | roster | 2 | -1.2 | -2.4 | +8.6 | 9.1 | 0.7 |
| WTZZ00DEU | consumer | 3 | -0.1 | +1.3 | -4.3 | 4.5 | 0.5 |
| ANMG00MYS | census | 1 | -2.0 | -2.4 | +0.0 | 3.1 | 2.3 |
| ARHT00ATA | census | 1 | -2.4 | +1.9 | +0.4 | 3.1 | 0.9 |
| ASCG00SHN | census | 3 | -16.1 | -7.9 | -1.3 | 18.0 | 0.6 |
| BADG00RUS | census | 8 | -1.1 | +2.7 | -4.7 | 5.5 | 0.4 |
| BAKE00CAN | census | 3 | -0.7 | +3.1 | -2.6 | 4.1 | 0.5 |
| CEDU00AUS | census | 8 | +0.9 | +3.8 | -14.6 | 15.1 | 0.4 |
| CHTI00NZL | census | 3 | -5.0 | +1.2 | -2.2 | 5.6 | 0.5 |
| DAV100ATA | census | 7 | +0.9 | +0.1 | -1.4 | 1.7 | 0.5 |
| HARB00ZAF | census | 3 | +2.9 | -0.9 | -1.8 | 3.5 | 0.5 |
| KOKB00USA | census | 5 | -1.5 | -0.2 | -0.5 | 1.6 | 0.6 |
| POHN00FSM | census | 7 | +4.0 | -3.2 | +0.4 | 5.1 | 0.7 |
| QUIN00USA | census | 9 | -6.0 | -1.0 | -11.9 | 13.3 | 0.7 |
| SCRZ00BOL | census | 5 | -2.3 | -2.8 | +6.8 | 7.8 | 0.6 |
| STPM00SPM | census | 1 | -2.8 | +0.7 | +4.4 | 5.3 | 0.6 |
| UTQI00USA | census | 2 | -6.8 | -3.8 | +8.2 | 11.3 | 0.8 |
| WUH200CHN | census | 6 | +0.9 | -1.5 | -1.2 | 2.1 | 0.8 |
| MIZU00JPN | dropped from the roster | 16 | +1527.0 | -698.7 | -22.4 | 1679.5 | 0.4 |

## Check of 2026-10-19: independent recomputation

A second parser, written without reference to `snx_parse.py` (fixed-column reads of SOLUTION/ESTIMATE, SOLUTION/APRIORI, SITE/ECCENTRICITY and SOLUTION/EPOCHS, each asserted equal to a token read; `python -I`), was run on the file in `D:/GnssProcessorSupport/downloads/sinex_2313/` (sha256 equal to the copy in `data/2024-129/products/`; decompressed 25,683,008 bytes, sha256 `b05b5dbe80ea8958...`, as recorded above). Held values: `config/golden/golden_field_refound.json`, `scripts/clock_level/site_list.json` (identical to `runs/_msc/site_list.json`) and, for MIZU00JPN, an own parse of the SSC extrapolated to decimal year 2024.349727 (it reproduces all 76 held values within 0.074 mm). Nothing was downloaded. Scripts and logs: `D:/GnssProcessorSupport/gates/sinex/check/`.

**Result: every figure of sections 2 to 7 and every row of the appendix reproduces. Two statements of section 8 and one of section 2 are corrected below.**

Stations recomputed (SINEX minus held, mm; the card's value, then the recomputed one to 0.01 mm; 3D in brackets):

| station | why chosen | card E, N, U (3D) | recomputed E, N, U (3D) |
| --- | --- | --- | --- |
| MAC100AUS | largest | +13.9 -62.2 +4.6 (64.0) | +13.92 -62.25 +4.63 (63.95) |
| PARK00AUS | second largest | -31.5 -48.1 -23.7 (62.2) | -31.53 -48.10 -23.67 (62.19) |
| GCGO00USA | third largest | +24.5 -50.7 -1.9 (56.3) | +24.46 -50.68 -1.85 (56.30) |
| SOLO00SLB | fourth largest | -27.2 -43.0 -19.4 (54.5) | -27.22 -43.01 -19.40 (54.47) |
| SVTL00RUS | the median (36th of 71) | +1.3 +0.8 -5.8 (6.0) | +1.27 +0.84 -5.84 (6.04) |
| CEBR00ESP | near the median | -1.5 +0.9 -5.5 (5.8) | -1.53 +0.94 -5.53 (5.81) |
| POTS00DEU | near the median | +0.5 +0.4 -6.8 (6.8) | +0.52 +0.40 -6.81 (6.84) |
| WTZZ00DEU | consumer | -0.1 +1.3 -4.3 (4.5) | -0.10 +1.30 -4.26 (4.46) |
| MIZU00JPN | suspect | +1527.0 -698.7 -22.4 (1679.5) | +1527.04 -698.72 -22.44 (1679.45) |

Reproduced without a difference beyond the print precision:

- All 72 appendix rows (SOLN, E, N, U, 3D, sigma; largest difference below 0.05 mm) and the four roster stations absent from the file.
- The counts of section 4: roster 54 (median 7.1, 90th percentile 37.2, largest 64.0; above 5 mm 35, above 1 cm 16, above 5 cm 4), consumer 4.5, census 16 (5.2, 14.2, 18.0; 9, 4, 0), all 71 (6.0, 23.1, 64.0; 44, 20, 4); the component medians and means; the sixteen stations between 1 and 5 cm.
- Solution numbers: 14 stations above the SSC's last solution (3D median 20.6 mm, 13 above 1 cm, the 13 named pairs identical), 57 equal (median 5.3 mm, the seven named above 1 cm).
- Reference point: the RINEX `ANTENNA: DELTA H/E/N` equals the SINEX eccentricity in force at `24:129:43200` at all 72 stations (largest difference 0.0000 m); 57 of the 71 have a nonzero height, the largest 3.052 m (HARB00ZAF); Up offset against height: slope 1.22 mm per metre, correlation 0.076, the seven stations with 0.5 m or more at -11.9 to +7.4 mm. Both the held values (the SSC) and the SINEX are marker coordinates; the engine reduces the roster and the consumer truth to the ARP only under `network.station_marker_to_arp` (true in the golden; `ApplicationNetworkSetup.cpp`).
- The file: 447 stations, 1379 parameters (1341 coordinates, 35 EOP, 3 geocentre), every coordinate row at `24:129:43200`, no velocity parameter, created `24:144:26732`, 7 daily inputs `24:126` to `24:132`; formal sigma of the 71: median 0.57 mm, largest 2.31 mm (ANMG00MYS); geocentre (-0.81, -0.63, +2.24) mm.
- Epoch: SSC velocity over half a day median 0.035 mm, largest 0.103 mm (MAJU00MHL), at most 0.72 mm over 3.5 days; the SSC's last window ends at 2021.000 for 60 of the 71, earliest 2020.246 (POHN00FSM); PARK00AUS last window 0.749 years, X velocity -55.1 against -30.8 mm/yr.
- Frame, with the card's rule: 65 of 71 stations, T (-0.91, +0.23, +0.15) mm, scale -0.20 ppb (-1.285 mm), R (0.036, 0.011, -0.022) mas; after removal the median is 6.3 mm and 24 stations are above 1 cm. The clip is 3 x 1.4826 x the median of the 3D residual norms (floor 3 mm), which is what "three robust sigma" in section 5 means. With a median absolute deviation about the median instead, 57 stations are kept and the fit reads T (-0.13, +0.14, -0.61) mm, scale -0.07 ppb, R at most 0.013 mas, median after removal 5.9 mm, 22 above 1 cm; the conclusion of section 5 holds under both rules.
- A priori and the week: a priori minus held median 0.9 mm, 90th percentile 21.7 mm; week minus a priori median 3.3 mm, 90th percentile 8.5 mm, largest 18.8 mm; GCGO00USA a priori 54.1 mm from the held value and 2.6 mm from its own estimate; the nine central European stations Up -7.7 to +0.1 mm (median -4.7), week minus model -3.5 mm, model minus held 0.0 mm (Up medians); WTZZ00DEU model 1.2 mm from the held value.
- NGS SIF cross-check (the repository's `SifTruth` at the SINEX epoch 2024.3511): 63 of 71 stations, SINEX minus SIF median 4.1, 90th percentile 11.6, largest 55.3 mm; SIF minus held 4.0, 44.9, 113.3 mm; WTZZ00DEU 0.5 mm; the per-station values of section 6 (PARK00AUS 10.3, SOLO00SLB 10.6, REYK00ISL 12.7, LCK400IND 2.1, ASCG00SHN 2.2, URUM00CHN 11.7, MAC100AUS 51.1, GCGO00USA 55.3, NIUM00NIU 31.9, COCO00AUS 24.9 mm).
- Section 7: the Up median of the ten Asian stations (-1.6 mm), the SOLN pairs, the MIZU00JPN SINEX coordinate, its differences from the leave-out estimate of card SCU (+55.5, -62.1, +98.0 mm), and the card SCU figures quoted (leave-out estimate, the SOLO00SLB, LCK400IND and ULAB00MNG replays, G3).

Corrections:

1. Section 8.4, "Expected result": the census median p50 "of 0.067 to 0.103 m in hours 2 to 6" is wrong. Card SCU section 8 (the 58-station column, key `rf8`) reads 0.673 0.169 0.103 0.082 0.095 0.067 m by hour, so hours 2 to 6 span 0.067 to 0.169 m; 0.067 to 0.103 is hours 3 to 6. The four census truths that move by 11 to 18 mm stay an order of magnitude below the median; the conclusion stands.
2. Section 8.3: "`msc_sites.load()` takes an environment override for the file name" states the planned change, not the present code: `msc_sites.PATH` is fixed to `site_list.json` and `load()` reads no environment variable. The override is part of phase 1 and is owed with the generator.
3. Section 2: "one point (A)" holds for every station compared; the file has one point B (IISC), which is in no set here.

Rounding only, no change to any conclusion: "largest 4.11" years in the extrapolation distance is measured to the SINEX epoch (2024.3511), to the held epoch 2024.349727 it is 4.10; "largest 64.7" mm for the a priori minus held is against the unrounded SSC extrapolation, against the printed configuration value it is 64.65 mm.

## Addendum 2026-10-19: the SIF post-seismic term double counted the displacement accumulated by the reference epoch (fixed, branch fix/sif-psd)

Section 6 read the NGS SIF as a third source and found it 51 to 55 mm from the SINEX at MAC100AUS and GCGO00USA, 32 mm at NIUM00NIU, 25 mm at COCO00AUS, and left GCGO00USA unexplained. The SIF data are not the cause; the evaluator was: its post-seismic term moved all eight roster stations with an active term away from the SINEX, and MIZU00JPN by 1.48 m (the SINEX minus SIF value of the three-way table `D:/GnssProcessorSupport/gates/sinex3/three_way.txt`). Scripts and outputs of the diagnosis: `D:/GnssProcessorSupport/gates/psd/`.

### A1. What was read

The coordinate function exists in three copies with one rule: `resolveStationCoordinate` in `src/core/products/StationInfoFile.cpp`, its twin `tools/sif_propagate/sif_propagate.py` (`resolve_station_coordinate`), and `SifTruth.resolve` in `pyviz/gnssviz/eval/truth.py`. Each formed `x(t) = x_ref + v (t - t_ref) + periodic(t) + psd(t)` with `psd(t)` the sum over earthquakes of `A_log ln(1 + dT/tau_log) + A_exp (1 - exp(-dT/tau_exp))`, `dT = t - t_event` in years, zero before the event, per local E, N, U component (SIF `POST_SEISMIC`: `EVENT_EPOCH`, `EAST`/`NORTH`/`UP` with `AEXP`, `TEXP`, `ALOG`, `TLOG`; the file metadata states `PSD_COEFF_UNITS` m and `PSD_TIME_UNITS` y), rotated to ECEF at the station. The ITRF2020 post-seismic formulas, units, the ENU frame and the sign are what the code implements; none of them is the error. GCGO00USA has one event (2002-11-03, Denali) with two exponentials per component: `psd(2024.351)` is (+30.7, -47.2, 0.0) mm, and the SIF minus SINEX of the old rule is (+33.3, -44.2, -0.1) mm, that is the term itself plus 2.6 and 3.0 mm.

### A2. The cause

The reference coordinate of an occupation (ITRF2020 at 2020.0 in the shipped files) is the model position at that epoch with the post-seismic displacement accumulated by then already in it. Evidence, two sources independent of each other:

1. **The SSC.** At 2020.0 the SIF occupation coordinate minus the ITRF2020 IGS SSC secular position (`data/static/sinex/ITRF2020-IGS-TRF.SSC`, which has no post-seismic term) equals `psd(2020.0)`. Over the 46 stations with a horizontal `psd(2020.0)` of at least 20 mm and an SSC entry the horizontal `|SIF - SSC - psd(2020.0)|` has a median of 3.4 mm (27 below 5 mm, 37 below 20 mm; the largest, STK200JPN 183 mm, SANT00CHL 113, MGUE00ARG 70, MIZU00JPN 40; the cause of those four residuals is not examined), against a median `|SIF - SSC|` of 59.5 mm and a median `|psd|` of 56.1 mm. Examples (East, North, mm; SIF minus SSC against `psd(2020.0)`): GCGO00USA (28.4, -46.8) against (30.2, -46.2); CONZ00CHL (-353.5, -12.7) against (-353.0, -10.9); ANTC00CHL (-614.4, 157.2) against (-620.0, 140.9). The three stations of the NCN file with a block agree the same way (HIKB00NZL 14.2 against 14.1 mm East; KEN100USA (16.3, -17.4) against (16.8, -16.9); AC2500USA 0.0 against 0.0, its event of 2020.555 follows the reference epoch). It holds for occupations that precede the event as well: over 171 pre-event occupations (one epoch each, 0.25 yr before the occupation ends) the median `|SIF - SSC|` is 24.8 mm, the median `|psd(2020.0)|` 24.8 mm, and MIZU00JPN's three pre-event occupations read 1479.3 to 1479.4 mm against 1479.4 mm.
2. **The SINEX.** At 2024.351 the SINEX minus the SIF linear and periodic terms is the change since the reference epoch, `psd(t) - psd(2020.0)`, and not `psd(t)`:

| station | SINEX - (linear + periodic), E / N (mm) | `psd(t)`, E / N | `psd(t) - psd(2020.0)`, E / N |
| --- | --- | --- | --- |
| GCGO00USA | -2.6 / -3.1 | +30.7 / -47.2 | +0.5 / -1.0 |
| MAC100AUS | -1.9 / -4.4 | -5.0 / -54.8 | -0.0 / -3.6 |
| NIUM00NIU | +2.7 / -8.8 | -23.5 / +8.6 | -1.9 / 0.0 |
| COCO00AUS | -0.6 / -0.0 | +14.8 / +19.6 | 0.0 / +1.2 |
| MIZU00JPN | +145.1 / -65.9 | +1518.2 / -622.3 | +147.0 / -67.1 |

The term for an event before the reference epoch had counted `psd(t_ref)` twice: 1.37 m East and 0.56 m South at MIZU00JPN (three events, 2005.6, 2011.2 including the Tohoku earthquake, and 2015.4), 30 mm East and 46 mm South at GCGO00USA.

### A3. The rule, in all three copies

`psd(t) - psd(t_ref)` per component, with `t_ref` the selected occupation's own reference epoch. It is zero at `t_ref`, negative back in time, minus the displacement the reference coordinate carries before the event, and the full relaxation for an event after `t_ref` (AC2500USA, DRAG00ISR). The helper is `accumulatedPostSeismicEnu` (C++), `_psd_enu` (the Python tool) and `SifTruth._psd_enu`. Nothing else in the function changes: the linear, periodic, covariance and occupation selection code, the loader, and every caller are as before.

### A4. Result

Against the weekly SINEX (SIF with the periodic term, local ENU in mm, SIF minus SINEX; `compare_old_new.py` runs the pre-fix tool and the fixed tool side by side):

| station | old E / N / U | 3D | new E / N / U | 3D |
| --- | --- | --- | --- | --- |
| GCGO00USA | +33.3 / -44.2 / -0.1 | 55.3 | +3.1 / +2.1 / -0.1 | 3.7 |
| SOLO00SLB | +2.8 / +10.2 / +0.5 | 10.6 | 0.0 / +1.8 / +0.5 | 1.9 |
| CUSV00THA | -3.1 / +8.9 / -2.9 | 9.9 | -3.1 / -0.7 / -2.9 | 4.3 |
| MAC100AUS | -3.1 / -50.5 / -7.7 | 51.1 | +1.9 / +0.8 / -7.7 | 8.0 |
| NIUM00NIU | -26.1 / +17.4 / -5.6 | 31.9 | -4.5 / +8.8 / -5.6 | 11.4 |
| COCO00AUS | +15.4 / +19.6 / -0.8 | 24.9 | +0.6 / +1.2 / -0.8 | 1.5 |
| REYK00ISL | -9.4 / +3.7 / -7.7 | 12.7 | -0.3 / +0.6 / -7.7 | 7.8 |
| MIZU00JPN | +1373.2 / -556.4 / +3.3 | 1481.6 | +2.0 / -1.2 / +3.3 | 4.0 |

- **Tolerance.** Seven of the eight are within 3.1 mm per horizontal component; NIUM00NIU (extrapolated 0.33 yr past its occupation across an antenna change) reads 8.8 mm North. The unit tests use 8 mm per horizontal component (12 mm for NIUM00NIU): twice the 95th percentile (4.1 mm) of the same horizontal difference over 264 SIF stations without a post-seismic block that the SINEX holds, with a covering occupation and periodic block, so it stands for weekly noise and unmodelled motion. The old rule fails the 8 mm bar at all eight (SOLO00SLB 10.2, CUSV00THA 8.9 and REYK00ISL 9.4 mm are the closest). The Up components (to 7.7 mm) are not post-seismic: none of the eight has an Up term.
- **All 43 stations with a block that the SINEX holds.** Horizontal difference: median 32.5 mm before, 2.3 mm after; 90th percentile 340.4 before, 5.1 after; largest 1481.6 (MIZU00JPN) before, 16.9 (GMSD00JPN, extrapolated 2.8 yr) after. 39 stations are closer to the SINEX in 3D, 3 farther (SAMO00WSM, WGTT00NZL, GMSD00JPN), one is equal. 3D median 39.9 before, 4.2 after.
- **The three-way table, rerun** with the fixed tool (`threeway_fixed/three_way.txt`): SIF minus SINEX over the 63 stations median 4.0 mm (4.1 before), 90th percentile 8.3 (11.7), largest 16.6 (55.3), above 5 cm none (two), above 1 cm 5 (10); the roster alone 4.2, 9.2, 16.6 (5.1, 12.7, 55.3); the SIF is closer to the SINEX than the held value at 49 of 63 stations (47). Two statements of section 6 are superseded: the SIF is far from the SINEX at MAC100AUS (51.1 mm), GCGO00USA (55.3), NIUM00NIU (31.9) and COCO00AUS (24.9), which now read 8.0, 3.7, 11.4 and 1.5 mm, and the bullet "GCGO00USA is not explained" (next bullet).
- **GCGO00USA, explained.** The held value is the SSC secular position, which has no post-seismic term, so its offset from the SINEX is the term itself: SINEX minus held (+24.5, -50.7) mm against `psd(t)` (+30.7, -47.2). The same holds at COCO00AUS (+13.2, +18.3 against +14.8, +19.6), NIUM00NIU East (-22.7 against -23.5), CUSV00THA North (+12.2 against +9.6), MIZU00JPN East (+1527.0 against +1518.2), and in part at MAC100AUS North (-62.2 against -54.8). The held offsets at SOLO00SLB and REYK00ISL, and the North of MIZU00JPN (-698.7 against -622.3), are larger than the term and keep their other causes of section 6 (a break after the SSC ends).
- **A vertical exception.** The rule holds in East and North at all 43 stations (largest horizontal difference 16.9 mm, GMSD00JPN, extrapolated). In Up it holds for the 8 stations with an Up term whose occupation derives from the IGb20 epoch-2020 solution (ANTC00CHL 178.5 mm before, 5.6 after; ASPA00ASM -101.2 to 9.5; USUD00JPN 58.6 to 0.2; TSKB00JPN 55.5 to 2.6; AREQ00PER 85.0 to -3.0; CKSV00TWN, SANT00CHL, WGTN00NZL 23 to 25 mm to under 5) and not for five stations whose occupation derives from `ITRF2020-u2023-wo-IGb20` (IQQE00CHL, MTKA00JPN, SAMO00WSM, TSK200JPN, WGTT00NZL): their reference Up coordinate does not carry the Up term (SIF minus SSC at 2020.0 against `psd_U(2020.0)`: MTKA00JPN -58.1 against +73.8 mm, SAMO00WSM +34.1 against -97.2, TSK200JPN -17.8 against +53.0), and their Up against the SINEX is now 35 to 96 mm (it was below 10 mm with the full term). TSKB00JPN and TSK200JPN carry the same post-seismic term (Up 53.0 mm at 2020.0) and differ by 53 mm in the reference. The rule is not keyed on the source string; none of the five is in the golden roster or the census. Open: whether to key the vertical on the occupation source, or to ask NGS whether the vertical term was left out of the epoch-2020 coordinates of that solution.
- **Read, not run.** NGS M-PAGES v1.2 (`StationInfo::getStationAtEpoch`, `Lib/Station/StationInfoFile.cpp`) adds `computePsdXyz` at the epoch with no subtraction at the reference epoch, the form this code had; the same SIF files and the same stations apply. A candidate for the defect list for NGS.

### A5. Reach and tests

- **Who is affected.** Only code that reads SIF coordinates at a station with a block: the engine through `network.use_sif_coordinates` and `processing.enable_station_info` (both off in the goldens; `config/golden/golden_field_refound.json` and `golden_sim_refound.json` read neither), and the Python graders `SifTruth` (`scripts/run_test_suite.py`, `scripts/windup_ab_linearity.py`, `scripts/resolve_itrf_ecef.py` reads the SSC and is unaffected) and `tools/sif_propagate`. An earlier SIF truth at a station with a post-seismic block carries the error (MIZU00JPN, ANTC00CHL, and the others above); the held roster coordinates are SSC values and are not affected.
- **Unit tests.** `tests/test_station_info_psd.cpp` (suite `SifPostSeismicTermTest`, 6 cases, and `SifPostSeismicRealTest`, 3 cases that skip without the fixtures): zero at the reference epoch, the change after and before it, minus the carried displacement before the event, an event after the reference epoch in full, the occupation's own reference epoch, the term off, the eight stations against the SINEX, and ANTC00CHL against the Python twin to 5e-9 m. `tools/sif_propagate/test_sif_propagate.py` sections 11 and 12 (690 checks; the ANTC00CHL rows of the oracle regenerated, 0.66 m) and 17 cases of `pyviz/tests/test_eval_truth.py` (99 pass). The new Python tests fail on the pre-fix tools (31 checks, 17 cases), as the 8 mm bar fails at all eight stations.
- **Goldens unaffected.** Neither golden reads a SIF key (`config/golden/golden_field_refound.json` and `golden_sim_refound.json` hold neither `network.use_sif_coordinates` nor `processing.enable_station_info`), and the field run's stdout has no SIF line. Gate: the field golden for 2 h, one arm on two Release executables built in this worktree with `scripts/wt_build.ps1`: the control from the tree at f7cddb2 before the first edit (`D:/GnssProcessorSupport/gates/exe/x64-Release/gnss_processor_psd_ctl.exe`, md5 41bc67d6) and the fixed tree (`gnss_processor_psd.exe`, md5 ac8fb91f). Arms `_rap_psd_field_ctl` and `_rap_psd_field_fix` under `D:/GnssProcessorSupport/runs_nom/` (58 stations, WTZZ00DEU the held-out consumer), exit 0 in 2,432 and 2,412 s. `python scripts/nom_td_gate.py psd_field_ctl psd_field_fix --whole` (full output `D:/GnssProcessorSupport/gates/psd/td_gate_whole.txt`): **RESULT IDENTICAL**, exit status 0: the arm configs (929 lines, run directory masked), `products.txt` (113,888 lines, equal md5 `fedc3eec`), 34 `logs/*.csv` in row order, the run manifest (12 records), `stdout.txt` (2,956 lines), the `main_log` set (2,425 lines), `rt_engine.log` (382 lines) and the exit code. WTZZ00DEU 3D median by hour 0.06 and 0.01 m in both arms (`rapid_arm.py score`). The sim golden was not run. Debug: `build/x64-Debug/gnss_tests.exe --gtest_brief=1` runs 3,467 cases in 471 suites: 3,457 pass, 10 skip (fixtures the worktree's `data/` does not hold), none fail, exit 0; the nine new cases pass (three of them read the SIF files, copied into the worktree's git-ignored `data/static/sif/`).
- **Commit and owed.** Code, tests, tool and report text: `4800a92` on branch fix/sif-psd (not merged). The text of `docs/report/reference_frames.tex` (equation `trf-propagate`) states the new rule; the tracked `reference_frames.pdf` and `main.pdf` are not rebuilt. Owed: the vertical question above, and the same check for any other consumer of a SIF coordinate at a station with a block.

## 10. Phase 1 arms, recorded in card SNXB (2026-10-19; append only)

This file is at the 60 KB bar, so the phase 1 arms of section 8.2 (the roster at its SINEX coordinates, field golden, 6 h, built on branch `card/snxroster`) are recorded in [SNXB](SNXB_sinex_roster_arms_2026-10-19.md), which holds the section unchanged as its section 10. Result: the roster at its SINEX values moves the emitted satellite clocks by 5 to 7 mm RMS (a per-satellite level) and no consumer by more than 2.4 mm; scoring on the SINEX instead of the held coordinates moves the census median by -1.35 mm (CEDU00AUS -11.5 mm). Nothing is armed; phase 2 (a loader key) is not built.
