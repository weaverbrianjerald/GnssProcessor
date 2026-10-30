# BDS305: BeiDou posture C at 305 producer stations, a 6 h pair

**STATUS**

- **id:** BDS305
- **topic:** network-vehicle
- **status:** closed
- **summary:** Closed 2026-10-30; bars pre-registered 2026-10-29, before any run. The open flag on BeiDou posture C (card MDAY section 10: at 305 producer stations one 2 h pair read WTZZ00DEU 19.7 mm better in hour 1 and 7.9 mm worse in hour 2), checked on the 6 h pair that card BOF section 5 names, on the current field golden moved to 305 stations. Both arms emit 720 of 720 epochs and withhold none. WTZZ00DEU hour 2, C minus GE, +6.05 mm (G2 reproduces); hours 2 to 6 -2.61 mm (G3 not a cost); three off-roster consumers -5.5 mm at the median (G4 better). Verdict: G2 reproduces alone, a transient of 01:00 to 01:30 UT that recurs (also at DAEJ00KOR) and enters through the consumer's BeiDou satellites; for a GPS + Galileo user the C products equal the GE products. Cost 1.2 to 1.9 times GE per epoch on a loaded machine, about 21 GB. The 16-site census has no site at 305 stations (section 3.2). Nothing armed.
- **source:** [OPEN_ITEMS_BUILT_OFF](../status/OPEN_ITEMS_BUILT_OFF.md) (the BeiDou item); [MDAY](MDAY_multiday_candidate_2026-10-19.md) section 10 (the 2 h pair and its base); [BOF](BOF_built_off_follow_through_2026-10-28.md) section 5 (the 6 h pair as the check)
- **knobs / posture:** none armed. Arm C adds to the base `processing.active_systems` `["GPS","Galileo","BeiDou"]`, `processing.signal_priorities.BeiDou` `["C2I","L2I","C6I","L6I"]`, `network.second_band_admission` true and `network.n3c_wire_per_signal` true (the four keys of MDAY section 10; the fifth key of MDAY section 2, `network.sparse_station_normals`, is in the golden since `3e4a53a`)
- **dates in the text:** 2026-10-29, 2026-10-30
- **hashes / run dirs:** exe `D:/GnssProcessorSupport/worktrees/bds305/build/x64-Release/gnss_processor_bds305.exe` (a copy of the Release build of main `1527cf9` in a detached worktree, sha256 `9ada4a9f59e54b321894bb697463a1dec1a0c8d9c0f8736e7cbc0bfbecc571be`); base `D:/GnssProcessorSupport/runs_nom/bds305_bases/golden_field_305.json` (sha256 `72dc7c05d867c69c4d5e5e116f8e5a27a587458d7b678f0e97a6ce8a312e4376`); run root `D:/GnssProcessorSupport/runs_nom` (`RAPID_RUNS_ROOT`): smokes `_rap_bds305_{ge,c}_sm`, arms `_rap_bds305_{ge,c}_f6`, consumer replays `_rap_b305{ge,c}_<site4>`, diagnostic replays `_rap_b305cg_<site4>` (section 5.1, not a bar); tools, site list, logs and tables under `D:/GnssProcessorSupport/gates/bds305/`
- **gates (frozen at pre-registration):** section 3, written 2026-10-29 before the smokes and the arms

**Outcome.** At 305 producer stations, is posture C's hour-2 excess at the held-out consumer a real cost or the noise of one realization? On a 6 h pair both arms emit 720 of 720 epochs and withhold none; WTZZ00DEU reads hour 2 +6.05 mm under C (G2 reproduces, bar +5.0 mm), hours 2 to 6 -2.61 mm (G3, not a cost), and three off-roster consumers read -5.5 mm at the median (G4, better). Verdict: neither, the case "G2 reproduces alone" of section 3.4: a transient of 01:00 to 01:30 UT that recurs, also at DAEJ00KOR, and enters through the consumer's BeiDou satellites. C costs 1.2 to 1.9 times GE per epoch. The arming decision stays open ([OPEN_ITEMS_BUILT_OFF](../status/OPEN_ITEMS_BUILT_OFF.md)).

## 1. Question

At 305 producer stations, is the held-out consumer's hour-2 excess under posture C a real cost or the noise of one realization? On the one 2 h pair (`_rap_bof_ge305_f2`, `_rap_bof_c305_f2`, exe `gnss_processor_bof.exe`, the golden of main `ef9f75d`) WTZZ00DEU read 55.8 and 11.1 mm in hours 1 and 2 under GE and 36.1 and 19.0 mm under C, the hour-2 excess in the half hour 01:00 to 01:30 (26.3 against 9.0 mm). At 58 stations posture C beat the golden in the census on five 24 h days (paired median -0.8 to -3.8 mm), while the same consumer read C a few millimetres worse in hours 2 to 6 of DOY 129 (section 4). A 6 h pair gives a second draw of hour 2, the settled hours 3 to 6, and the wires for consumer replays.

## 2. Base and arms

**Base.** `config/golden/golden_field_refound.json` of main `1527cf9` (sha256 `5fd6d85dab77c42e`, the golden since `2b9af4a`, which arms ocean tide loading from the SIF registry, the noon-turn attitude, the estimable block-Baarda test and the readmit-seed mirrors) with only `network.stations` replaced, by the rule of MDAY section 10: the golden's 58 entries first, unchanged and in golden order, then the stations of `config/test_suite/_net305_3sys_smoke.json` not among them, in that file's order, without WTZZ00DEU (the held-out consumer). 58 + 247 = 305 stations; KOKV00USA is the one golden station absent from the 305 list. Tool `D:/GnssProcessorSupport/gates/bds305/tools/make_base305.py`.

**Check against the BOF base** (`D:/GnssProcessorSupport/runs_nom/bof_bases/golden_field_305.json`, cut from the golden of `ef9f75d`, sha256 `3946ebff485cf3e5`): the station list is identical (305 entries, same order, same contents). A flat leaf diff gives eight keys, each with the value of the current golden: `_comment`, `model_corrections.enable_ocean_tide_loading` (false to true), `model_corrections.otl_from_sif`, `model_corrections.otl_nodal_corrections`, `model_corrections.otl_remove_diurnal_translation`, `network.block_baarda_estimable`, `network.readmit_seed_mirrors` and `processing.enable_noon_turn_attitude` (absent to true). Against the current golden the base differs in `network.stations` alone.

**Arms.** Made with `scripts/rapid_arm.py make` from the base, 6 h (`--hours 6`: 720 epochs of 30 s from 00:00 UT, DOY 129 of 2024), and run at once on the one exe with `rapid_arm.py run --jobs 2`:

| arm | what it is |
|---|---|
| `bds305_ge_f6` | the base (GPS + Galileo) |
| `bds305_c_f6` | the base plus the four posture-C keys |

Smokes `bds305_ge_sm` and `bds305_c_sm` (10 epochs, `--hours 0.0833`) run first. The 6 h pair is a new realization of both postures, not a continuation of the 2 h pair: the golden now carries ocean tide loading (solve-affecting), and the code-bias datum's broadcast group-delay latch takes its window from the run length (MDAY section 4). Beside the BOF keys, the exe's source adds two cards to `ef9f75d`: CVF (products byte-identical in its gates) and SSR-SIG-ID (the real-time SSR decoder only).

## 3. Read-outs and bars (frozen 2026-10-29, before any run of this card)

### 3.1 Read-outs

- **R1 health**, per arm: exit code; epochs emitted, skipped and withheld (`logs/network_epoch_status.csv`); stations in the solve; the ocean tide loading source per station (run start).
- **R2 WTZZ00DEU**, the in-run consumer on its arm's own systems: 3D p50 by hour from 00:00 UT (hours 1 to 6) and by half hour, the mean of the hourly p50 over hours 2 to 6, epochs below 0.1 m (`logs/closed_loop_trace.csv`).
- **R3 cost:** the run-end epoch-loop wall per epoch (the `Network[timing]` line); the per-epoch totals (the `Network (D0)` lines) over the epochs both arms ran at once; process wall; peak working set and peak commit, sampled once a minute from the process (the exe does not report memory), with the other `gnss_processor` processes on the box at each sample; the C arm's per-epoch total against the 30 s epoch interval (mean and p95).
- **R4 off-roster consumers:** consumer-only 6 h replays of each wire (`scripts/clock_level/gcensus.py make`, each arm's own `arm.json` as the consumer base, so a GE replay is a GPS + Galileo user and a C replay uses BeiDou where the site tracks B1I and B3I; the GPS-only bases of `census6h.py` are not used), graded with `gcensus.py grade`: each site's mean hourly p50 over hours 2 to 6, paired by site, C minus GE, hour 1 separately. A replay at WTZZ00DEU runs as a check on the in-run consumer.

### 3.2 The census at 305 stations

Each of the 16 census sites of `scripts/clock_level/site_list.json` is a producer station of the 305 roster: CHTI00NZL, SCRZ00BOL, ANMG00MYS, QUIN00USA, HARB00ZAF, POHN00FSM, DAV100ATA, BADG00RUS, STPM00SPM, ASCG00SHN, CEDU00AUS, KOKB00USA, UTQI00USA, ARHT00ATA, WUH200CHN and BAKE00CAN. All 16 are excluded, since a consumer at a producer station is not held out, and the 16-site census has no site at 305 stations.

Substitute, fixed here: the stations with a DOY 129 RINEX file under `C:/GnssProcessor/data/2024-129/obs` (311 station directories) that are not in the roster are five; less WTZZ00DEU (R2) and P38900USA (no ITRF2020 coordinate, rejected by the inventory of card MSC for that reason) they are DAEJ00KOR, NRMG00NCL and ZIM200CHE, taken with their records from the MSC inventory (ITRF2020 at epoch 2024.3497) into `D:/GnssProcessorSupport/gates/bds305/site_list_offroster305.json` (read by `gcensus.py --sites`). Each has a co-located producer station (DAE200KOR; NRMD00NCL; ZIMM00CHE and ZIM300CHE), as WTZZ00DEU has (WTZR00DEU, WTZS00DEU). NRMG00NCL and ZIM200CHE track C2I and C6I; DAEJ00KOR tracks C2I and C7I only, so its C replay has no B3I. Three sites are read as three consumers, not as a census.

### 3.3 Bars

| bar | read-out | passes when | reference (section 4) |
|---|---|---|---|
| G1 health | R1, both arms | exit 0; 720 of 720 epochs emitted; 0 withheld | 2 h pair: 240 of 240 emitted, 0 withheld in both |
| G2 the flag | WTZZ00DEU hour-2 p50, C minus GE | reproduces when at least +5.0 mm | 2 h pair +7.9 mm; 58 stations +3.2 mm |
| G3 settled cost | WTZZ00DEU mean of the hourly p50 over hours 2 to 6, C minus GE | a cost when at least +5.0 mm | 58 stations +3.4 mm |
| G4 off-roster consumers | R4 at DAEJ00KOR, NRMG00NCL and ZIM200CHE, change over hours 2 to 6, C minus GE | worse when the median of the three is at least +1.0 mm or one site is worse by more than 1 cm; better when the median is at most -1.0 mm and no site is worse by 1 cm; equal otherwise | the census wording of MDAY section 2 |
| G5 cost | R3 | reported, no bar | 2 h pair: 10.37 and 25.88 s per epoch (2.5 times), 11.2 and 12.1 GB peak working set |

The 5 mm thresholds of G2 and G3 lie above the 58-station readings of the same consumer (hour 2 +3.2 mm, hours 2 to 6 +3.4 mm), so they ask whether 305 stations add a cost to what 58 stations already show at WTZZ00DEU; G2's lies below the 2 h reading (+7.9 mm).

### 3.4 Verdict

- **Real cost** when G2 reproduces and G3 is a cost or G4 reads worse: the excess recurs on a new draw and is seen in the settled hours or by other held-out consumers.
- **Noise of one realization** when G2 does not reproduce, G3 is not a cost and G4 does not read worse.
- Otherwise the verdict names the bars that moved: G2 reproduces alone (an hour-2 excess at WTZZ00DEU that recurs and does not persist), or G2 fails while G3 or G4 shows a cost (the flagged hour is not the cost; the cost read is stated).
- A C arm that fails G1 is a cost on its own, whatever G2 to G4 read.

The verdict concerns the hour-2 flag at 305 stations. Nothing is armed here: arming is the author's decision, through the ledger and `scripts/make_golden_config.py`.

## 4. Reference numbers (read from existing run dirs before the bars were set)

Recomputed with `D:/GnssProcessorSupport/gates/bds305/tools/consumer_hours.py` from `logs/closed_loop_trace.csv` (hours from 00:00 UT, DOY 129):

| run | stations | hourly p50, hours 1 to 6 (mm) | half hours 1 to 4 (mm) | mean, hours 2 to 6 (mm) |
|---|---:|---|---|---:|
| `_rap_bof_ge305_f2` (GE, 2 h) | 305 | 55.8 11.1 | 92.5 41.0 9.0 12.8 | n/a |
| `_rap_bof_c305_f2` (C, 2 h) | 305 | 36.1 19.0 | 95.8 34.5 26.3 15.1 | n/a |
| `_rap_mday_ge_129` (GE, 24 h, first 6 h) | 58 | 58.1 11.5 18.3 10.3 15.5 10.6 | 108.1 40.2 8.9 12.7 | 13.23 |
| `_rap_mday_c_129` (C, 24 h, first 6 h) | 58 | 29.9 14.7 19.0 17.1 17.6 14.7 | 116.0 25.8 16.2 13.4 | 16.59 |

C minus GE at 58 stations: hour 1 -28.2 mm, hour 2 +3.2 mm, hours 2 to 6 +3.4 mm, the half hour 01:00 to 01:30 +7.3 mm. At 305 stations (2 h): hour 1 -19.7 mm, hour 2 +7.9 mm, the half hour +17.3 mm. The 58-station runs are an older exe and golden (main `5e95c84`, card MDAY: no MW re-flag skip, GE without sparse normals), so they set a scale, not a paired reference. On the same day the census read C better (DOY 129, hours 2 to 24: -3.2 mm, MDAY Table D; hours 2 to 6 at 6 h: -3.5 mm, card BDS section 9) while WTZZ00DEU read C 3.4 mm worse in hours 2 to 6: WTZZ00DEU alone is one instrument.

## 5. Results

Appended below after the runs.

### 5.1 The 6 h pair (2026-10-30)

**Runs.** The smokes (10 epochs) exited 0 with 10 of 10 epochs emitted and none withheld (GE 200 s, C 293 s). The 6 h arms started together at 2026-10-29 23:11:01 and ended at 02:37 (GE) and 04:06 (C). The machine was shared until 01:45: another session's runs held up to 27 other `gnss_processor` processes (hourly medians of the CPU load 76 to 100 %). After that only the two arms ran, apart from the four GE replays (32 s at 02:39). Tables in `D:/GnssProcessorSupport/gates/bds305/`: `grade_f6.txt`, `readout_f6.txt`, `cost_f6.txt`, `gcensus_grade_f6.txt`, `gcensus_grade_cg.txt`, `consumer_hours_cg.txt`; sampler log `mem_f6.log`.

**Health (R1, bar G1).**

| read-out | GE | C |
|---|---|---|
| exit code; process wall | 0; 12,456 s | 0; 17,704 s |
| epochs emitted, skipped, withheld | 720, 0, 0 | 720, 0, 0 |
| clock satellites per epoch (median) | GPS 31, Galileo 24 | GPS 31, Galileo 24, BeiDou 38 |
| stations in the solve (min, median, max) | 299, 301, 304 | 299, 302, 304 |
| addStation PD rejections | 890 (1.2 per epoch) | 600 (0.8 per epoch) |
| re-admissions after an absence | 52 | 56 |
| ocean tide loading source (306 markers) | SIF 252, none 54 | SIF 252, none 54 |
| second-band admission | not armed | BeiDou withheld at 20 stations (160,269 records), Galileo at 7 (15,762); 84 withhold and 64 readmit edges |

The SIF count holds the 251 producer stations with a SIF block and WTZZ00DEU; the other 54 producer stations run at zero displacement (no BLQ or EOT20 fallback is configured). G1 passes on both arms.

**WTZZ00DEU (R2, bars G2 and G3).** 3D p50 (mm), hours from 00:00 UT:

| arm | 1 | 2 | 3 | 4 | 5 | 6 | mean 2 to 6 | epochs below 0.1 m |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| GE | 56.7 | 12.3 | 18.8 | 14.9 | 20.0 | 18.9 | 16.97 | 692 |
| C | 36.6 | 18.4 | 13.4 | 11.7 | 16.1 | 12.2 | 14.36 | 691 |
| C minus GE | -20.1 | +6.0 | -5.3 | -3.2 | -3.9 | -6.7 | -2.61 | |

Half hours 1 to 12 (mm): GE 93.6 43.0 10.2 14.1 21.3 16.8 14.5 15.1 19.0 20.4 18.6 19.4; C 96.8 34.5 25.6 12.7 13.8 12.7 10.1 14.7 16.1 15.2 13.4 10.3.

- G2: hour 2 +6.05 mm against a bar of +5.0 mm: **reproduces** (2 h pair +7.9 mm). The excess sits again in 01:00 to 01:30 (25.6 against 10.2 mm, +15.4; 2 h pair +17.3, 58 stations +7.3); 01:30 to 02:00 reads C 1.4 mm better.
- G3: hours 2 to 6 -2.61 mm: **not a cost**. C is better in each of hours 3 to 6, by 3.2 to 6.7 mm.

The second draw repeats the first within 2 mm in hours 1 and 2 (GE 55.8 and 11.1 mm on the 2 h pair, 56.7 and 12.3 here; C 36.1 and 19.0, then 36.6 and 18.4), although ocean tide loading now acts and the latch window differs (section 2). The excess belongs to that half hour of DOY 129, on both rosters, rather than to the noise of one realization.

East and Up medians at WTZZ00DEU in 10-minute bins (mm; satellites tracked, median over the run: GE 21, C 34):

| bin start (UT) | 00:50 | 01:00 | 01:10 | 01:20 | 01:30 | 01:40 | 01:50 |
|---|---:|---:|---:|---:|---:|---:|---:|
| East, GE | +11 | 0 | -1 | -3 | +9 | +4 | -2 |
| East, C | +34 | +29 | +24 | +14 | +17 | +7 | -3 |
| Up, GE | +14 | +12 | +8 | +4 | -1 | +2 | +6 |
| Up, C | -3 | -4 | -7 | -6 | -4 | -6 | -2 |

Under C the consumer carries an East offset of +34 mm at 00:50 that decays to +7 mm by 01:40 and is gone by 01:50 (|E| 24.2 mm in 01:00 to 01:30, 23.4 mm on the 2 h pair); the GE consumer's East stays within 11 mm.

**Off-roster consumers (R4, bar G4).** Consumer-only 6 h replays, each on its arm's own `arm.json`. The replays at WTZZ00DEU reproduce the in-run consumers exactly (720 of 720 epochs, largest ENU difference 0.0 m, both arms). Mean of the hourly p50 over hours 2 to 6, and hours 1 and 2 (mm):

| site | GE, 2 to 6 | C, 2 to 6 | C minus GE, 2 to 6 | hour 1, GE / C | hour 2, GE / C |
|---|---:|---:|---:|---|---|
| DAEJ00KOR | 31.82 | 33.41 | +1.59 | 52.0 / 50.5 | 28.4 / 35.5 |
| NRMG00NCL | 62.74 | 42.59 | -20.15 | 460.3 / 97.7 | 141.4 / 68.7 |
| ZIM200CHE | 26.45 | 20.96 | -5.49 | 84.8 / 52.9 | 19.9 / 20.4 |

- G4: median -5.5 mm, at most -1.0 mm, and no site worse by 1 cm (DAEJ00KOR +1.6 mm): **better**. Hour 1: paired median -31.9 mm (two sites better by more than 1 cm, none worse).

Hour 2 is worse at a second consumer: DAEJ00KOR +7.2 mm, again in 01:00 to 01:30 (49.6 against 37.4 mm, mostly Up: +46.3 against +36.8 mm), with hours 3 to 6 equal within 0.6 mm (the site has no B3I; 21 to 26 satellites tracked under C against 12 to 16). ZIM200CHE reads +0.5 mm in hour 2, C worse in 01:30 to 02:00 (28.8 against 19.8 mm, Up). NRMG00NCL, still converging under GE, reads C better in every hour.

**Diagnostic (not a bar): a GPS + Galileo consumer of the C products.** Replays `_rap_b305cg_<site4>` (`gcensus.py make b305cg _rap_bds305_ge_f6 _rap_bds305_c_f6`): the GE arm's consumer (GPS + Galileo; 21 satellites at WTZZ00DEU) on the C arm's `products.txt`. `network.n3c_wire_per_signal` is read only on the producer's emit side (`src/core/network/emit/SolPrep.cpp`), so the consumer needs no key of its own to read the C products.

| site | GE products, 2 to 6 | C products, 2 to 6 | difference | hour 2, GE / C products |
|---|---:|---:|---:|---|
| WTZZ00DEU | 16.97 | 17.35 | +0.38 | 12.3 / 13.1 |
| DAEJ00KOR | 31.82 | 31.79 | -0.03 | 28.4 / 28.9 |
| NRMG00NCL | 62.74 | 63.15 | +0.41 | 141.4 / 142.8 |
| ZIM200CHE | 26.45 | 26.61 | +0.16 | 19.9 / 18.7 |

For a GPS + Galileo user the C products equal the GE products: within 1.4 mm in each of hours 2 to 6 at the four sites, and within 0.4 mm over hours 2 to 6. The hour-2 excess and the gains of C both arrive with the consumer's own use of BeiDou (the BeiDou products and the consumer's BeiDou model), not through the GPS and Galileo products.

**Cost (R3, G5, no bar).**

| read-out | GE | C | C / GE |
|---|---:|---:|---:|
| epoch-loop wall per epoch | 16.84 s | 24.17 s | 1.44 |
| process wall | 12,456 s | 17,704 s | 1.42 |
| per-epoch total over the 417 epochs both arms ran at once (TOW 259200 to 271680): mean, median, p95 | 17.9, 15.8, 39.4 s | 28.8, 23.2, 74.2 s | 1.61 (mean), 1.47 (median) |
| per-epoch total, whole run: mean, p95, max | 16.6, 34.5, 49.7 s | 23.9, 68.0, 91.8 s | |
| epochs over the 30 s interval | 62 | 91 | |
| peak working set; peak commit | 20.02; 22.32 GB | 21.03; 23.48 GB | |

Per-epoch total by hour (mean, s), with the other `gnss_processor` processes on the machine while each arm processed that hour (median of the minute samples):

| hour | 1 | 2 | 3 | 4 | 5 | 6 |
|---|---:|---:|---:|---:|---:|---:|
| GE | 20.3 | 16.0 | 17.4 | 16.8 | 14.4 | 14.8 |
| C | 32.0 | 29.8 | 26.0 | 21.5 | 17.1 | 17.3 |
| C / GE | 1.58 | 1.86 | 1.49 | 1.28 | 1.18 | 1.17 |
| other processes, GE's hour | 18 | 6 | 8 | 9 | 0 | 0 |
| other processes, C's hour | 15 | 8 | 0 | 0 | 0 | 0 |

The ratio is confounded by load: the arms reached each hour at different wall times and under different load (the two lower rows), and they shared the machine with each other until 02:37; C alone (hours 5 and 6) took 17.1 and 17.3 s. The 2 h pair's 2.50 was also measured on a loaded machine (seven other runs when its smoke started; `D:/GnssProcessorSupport/runs_nom/bof_reports/beidou_305.md`). On this machine C costs 1.2 to 1.9 times GE per epoch at 305 stations. After hour 1 every epoch over the 30 s interval falls on a TOW multiple of 300 s (GE 48 of the 60 such epochs, C all 60), a five-minute step not identified here. In hours 5 and 6 an ordinary epoch costs GE 12.8 s and C 14.4 s at the median, a five-minute epoch 31.0 and 42.3 s. Memory grows with the run length: from the end of hour 1 the working set holds 19.8 to 19.9 GB (GE) and 20.9 to 21.0 GB (C), against peaks of 11.2 and 12.1 GB on the 2 h pair; C adds about 1 GB (0.9 GB at 2 h).

**Section 4, unrounded.** Its 58-station differences come from rounded hours; unrounded, hour 1 is -28.10 mm, hour 2 +3.15 mm and hours 2 to 6 +3.36 mm. No bar depends on them.

### 5.2 Verdict (section 3.4)

G1 passes on both arms, G2 reproduces (+6.05 mm, 1.05 mm above its bar), G3 is not a cost (-2.61 mm) and G4 reads better (median -5.5 mm, no site worse by 1 cm). By section 3.4 this is the third case, **G2 reproduces alone**: an hour-2 excess at WTZZ00DEU that recurs and does not persist. It is neither a settled cost nor the noise of one realization. It repeats within 2 mm on a second draw, it appears at a second consumer (DAEJ00KOR, hour 2 +7.2 mm), it lies in one half hour of DOY 129 (01:00 to 01:30) on both rosters, and it comes with the consumer's use of BeiDou. From hour 3 on C is better at WTZZ00DEU in every hour, and over hours 2 to 6 the three off-roster consumers read C 20.2 and 5.5 mm better and 1.6 mm worse.

**Open after this card.** Which BeiDou satellites or model terms at the consumer carry the offset of 01:00 to 01:30 (replays of the C consumer at WTZZ00DEU and DAEJ00KOR with one BeiDou satellite excluded at a time would name them); whether the transient appears at a held-out consumer on other days (the five-day census of card MDAY grades hours 2 to 24, not a half hour); a timing pair on an idle machine for the cost ratio. Nothing is armed: arming posture C remains the author's decision, through the ledger and `scripts/make_golden_config.py`.
