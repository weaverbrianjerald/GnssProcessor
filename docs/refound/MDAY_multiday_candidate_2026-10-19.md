# MDAY: the field golden and the candidate posture on five days (DOY 128 to 132, 2024, 24 h)

**STATUS**

- **id:** MDAY
- **topic:** network-vehicle
- **status:** closed
- **summary:** Read 2026-10-20. Both postures, GE (the field golden, GPS + Galileo) and C (GE plus BeiDou B1I + B3I, `network.second_band_admission`, `network.n3c_wire_per_signal`, `network.sparse_station_normals`), ran 24 h on each of DOY 128 to 132 of 2024 on one Release exe (DOY 132 for 86,100 s, 5.5 minutes short of the day: the DOY 133 products are not on disk). All ten arms exit 0 and emit every epoch (2,880 of 2,880; 2,870 of 2,872). Accurate every day: WTZZ00DEU hourly p50 at most 0.060 m (GE) and 0.047 m (C) in hours 2 to 24, census median site 0.02 to 0.07 m, no consumer above 1 m after hour 1 except ARHT00ATA under GE for 26 min on DOY 130 (2.2 m). C against GE: census paired median -0.8 to -3.8 mm, no site worse than +2.9 mm, ARHT00ATA -0.08 to -0.13 m on four days, hour 1 better in the median on every day. Nothing is armed.
- **source:** the request of 2026-10-19 (does the network deliver accurate positions on every day, and does the candidate posture beat the golden there); [MDR](MDR_multiday_replay_2026-10-05.md) (the earlier multi-day replay, GPS-only golden, 6 h); [BDS](BDS_beidou_third_constellation_2026-10-18.md) and [BDSB](BDSB_beidou_continued_2026-10-19.md) (the candidate, graded on DOY 129 in 6 h)
- **knobs / posture:** none armed. Arm C adds `processing.active_systems` `["GPS","Galileo","BeiDou"]`, `processing.signal_priorities.BeiDou` `["C2I","L2I","C6I","L6I"]`, `network.second_band_admission` true, `network.n3c_wire_per_signal` true, `network.sparse_station_normals` true to the field golden moved to the day
- **dates in the text:** 2026-10-19, 2026-10-20
- **hashes / run dirs:** exe `gnss_processor_mday.exe` (copy of the Release build of main `5e95c84`, sha256 `3d6c679e8068a39879c582b4495e069c569a3706049826f96f6123e2267a288e`), run root `D:/GnssProcessorSupport/runs_nom` (`RAPID_RUNS_ROOT`): wires `_rap_mday_ge_<doy>` and `_rap_mday_c_<doy>` (doy 128 to 132), smokes `_rap_mday_sm_<doy>`, census keys `mdyge<doy>` and `mdyc<doy>` (replays `_rap_<key>_<site4>`); scratch, day goldens, tools and tables `D:/GnssProcessorSupport/gates/mday/` (`cfg/`, `tools/`, `out/`, `B0_armdiff.txt`, `queue.log`, `census_chain.log`); branch `card/mday`, commit `9373715` (the census tool's day options) and this card
- **gates (frozen at pre-registration):** none numeric; the read-outs and the verdict wording are in section 2

**Outcome.** Question: does the network deliver accurate positions on every day, and does the candidate posture (GPS + Galileo + BeiDou with the second-band admission) beat the field golden (GPS + Galileo) there? On five 24 h days both postures run healthy and are accurate: WTZZ00DEU stays under 0.06 m per hour after ignition, the 16-site census median under 0.07 m, and no consumer passes 1 m after hour 1 except the golden's ARHT00ATA for 26 min on DOY 130. The candidate is better on four days and equal on one (census paired median -0.8 to -3.8 mm, never a site worse than +2.9 mm), far better at ARHT00ATA (-0.08 to -0.13 m on four days) and in hour 1, at about twice the golden's cost. Not armed.

## 1. Question and posture

**Question.** Does the network deliver accurate positions on every day, and does the candidate posture beat the golden there? The record before this card is thin on both counts. Card [MDR](MDR_multiday_replay_2026-10-05.md) graded the field golden of 2026-10-05 (GPS only, a different posture) in the first 6 h of four more days. Cards [BDS](BDS_beidou_third_constellation_2026-10-18.md) and [BDSB](BDSB_beidou_continued_2026-10-19.md) graded GPS + Galileo (GE) against GPS + Galileo + BeiDou with the second-band admission (C) on DOY 129 alone, in 6 h. This card runs both postures for 24 h on five consecutive days of 2024 (DOY 128 to 132, 2024-05-07 to 2024-05-11) on one exe. DOY 131 from about 18 UT and DOY 132 lie in the geomagnetic storm of 10 and 11 May (card MDR, section 2).

## 2. Arms, instruments and read-outs

| arm | what it is |
| --- | --- |
| GE | The field golden `config/golden/golden_field_refound.json` moved to the day by `scripts/mday_golden_config.py` (every DOY 129 string, the station and consumer RINEX paths, `data_download.doy`, the stations whose RINEX is missing that day dropped; for DOY 129 the golden itself), made with `scripts/rapid_arm.py make --hours 24`. GPS + Galileo, nothing else changed. |
| C | GE plus `processing.active_systems` `["GPS","Galileo","BeiDou"]`, `processing.signal_priorities.BeiDou` `["C2I","L2I","C6I","L6I"]` (as arm `_rap_bds_gec6` of card BDS), `network.second_band_admission` true, `network.n3c_wire_per_signal` true, `network.sparse_station_normals` true (the exact sparse station normals, output-identical to the dense products in the checks of card NETCOST; not separately controlled here). |

The golden, the ledger and the manifest are not edited. The census replays are consumer-only runs of each wire (`scripts/clock_level/gcensus.py`, extended on this card with `--day`, `--obs-root`, `--start-hour`, `--hours`, `--duration-s` and `--jobs`, default behaviour unchanged: `grade` output on the keys `bdsge`, `bdsgecs`, `bdsgec` is byte-identical to the previous script and `make` issues the same 17 `rapid_arm.py make` commands and the same `run` command, checked against a recorded-subprocess comparison; commit `9373715` of branch `card/mday`). Each replay uses the arm's own `arm.json` as its base, so the GE census consumers are GPS + Galileo and the C census consumers use BeiDou where the site tracks B1I and B3I (ten of the 16 sites, card BDS section 2). The same exe runs every wire and every replay.

**Read-outs** (the brief of 2026-10-19; no gate was numeric before the runs):

- Per arm and day: exit code, epochs emitted and withheld, wall time (Table A).
- Held-out consumer WTZZ00DEU, the in-run consumer of each arm: 3-D p50 by hour from 00:00 UT, the pooled p50 and the mean of the hourly p50 over hours 2 to 24 (hour 1 is ignition), epochs below 0.1 m (Table B).
- 16-site census: median over the sites of the site hourly p50 (Table C); C against GE paired by site: the change of each site's mean hourly p50 over hours 2 to 24, its median, better and worse at 1 cm, and hour 1 separately (Table D); ARHT00ATA, DAV100ATA, HARB00ZAF and KOKB00USA named (Table E); every site (Table F).
- Divergence: any consumer, wire or replay, with an epoch above 1 m after hour 1: its hour and the first epoch above (Table G and section 5.4).
- Wire health: the BeiDou clock class, the satellites per epoch, and the change of the GPS and Galileo clocks between the arms (Table H, after gates H2 to H4 of card BDS).

**Verdict wording**, written down on 2026-10-20 after four of the five days had been read (DOY 132 unread): *accurate* on a day when the WTZZ00DEU and the census-median hourly p50 stay at or under 0.10 m in hours 2 to 24 and no consumer shows an hourly p50 above 1 m; C *better* than GE on a day when the paired median change over hours 2 to 24 is at most -1 mm and no site is worse by 1 cm, *equal* when it lies within 1 mm of zero, *worse* when it is at least +1 mm or a site is worse by 1 cm. A site-level reading is reported next to the day-level verdict.

## 3. Data and products on disk

| day | date | roster file | stations in the arms | products in the interpolation window | run length |
| --- | --- | --- | --- | --- | --- |
| 128 | 2024-05-07 | 57 of 58 (COCO00AUS is absent from the CDDIS daily archive) | 57 (the MAC100AUS receiver logged no satellites before 10:30 UT: the station joins the solve at 10:30:30, 1,617 of 2,880 epochs) | DOY 127 to 129 (104,544 orbits) | 24 h, 2,880 epochs |
| 129 | 2024-05-08 | 58 of 58 | 58 | DOY 128 to 130 (104,544 orbits) | 24 h, 2,880 epochs |
| 130 | 2024-05-09 | 58 of 58 | 58 | DOY 129 to 131 (104,544 orbits) | 24 h, 2,880 epochs |
| 131 | 2024-05-10 | 58 of 58 (the MAC100AUS file holds 242 epochs, to 02:01:30) | 58 | DOY 130 to 132 (104,544 orbits) | 24 h, 2,880 epochs |
| 132 | 2024-05-11 | 56 of 58 (MAC100AUS and LCK400IND are absent from the archive) | 56 | DOY 131 and 132 only (69,696 orbits): DOY 133 is not on disk and was not fetched | 86,100 s, 2,870 epochs (10 epochs short of the day) |

The DOY 132 arms stop at 23:54:30 (86,100 s), a precaution taken because the DOY 133 products are absent: the orbit window of the last 25 minutes is then one-sided (the code shifts the 11-node window inward at the data boundary, ModelGeometry.cpp), and the DOY 132 clock file ends at 23:59:30. The files in fact reach the end of the day (the SP3 holds nodes to 2024-05-12 00:00:00), so a 24 h run was possible: the last 10 epochs (23:55:00 to 23:59:30) were not run, and their effect was not read. The WTZZ00DEU consumer and the 16 census sites have a full-day RINEX file on every day. Observations for DOY 128 and 130 to 132 are under `D:/GnssProcessorSupport/data_mday`, those of DOY 129 under `C:/GnssProcessor/data`; the broadcast navigation, the CODE MGEX orbit, clock and bias files and the ERP of each day are under `C:/GnssProcessor/data/<YYYY-DOY>/products` (DOY 128 and 130 use the DOY 129 ERP file, card MDR section 1; the DOY 127 products added by card MDR section 4 are present).

## 4. Checks before the grade

- **Arm records (B0).** `D:/GnssProcessorSupport/gates/mday/B0_armdiff.txt`: the GE arm of each day differs from its day golden in `network.max_duration_s`, `processing.max_duration_s` and `network.product_output_path` only (the day goldens differ from the DOY 129 golden in the date-bound paths, the roster and `data_download.doy`); C differs from GE in exactly `network.n3c_wire_per_signal`, `network.second_band_admission`, `network.sparse_station_normals`, `processing.active_systems`, `processing.signal_priorities.BeiDou` and the output path.
- **Exe.** `D:/GnssProcessorSupport/gates/exe/x64-Release/gnss_processor_mday.exe`, a copy of `C:/GnssProcessor/build/x64-Release/gnss_processor.exe` (built 2026-10-19 05:45, no source file under `src`, `include` or the CMake list newer than it; main `5e95c84`), sha256 `3d6c679e8068a39879c582b4495e069c569a3706049826f96f6123e2267a288e`. Nothing was built. `make_golden_config.py --verify` passed before the arms were made (76 recommended knobs armed in regime, 2 unarmed with documented exceptions, 0 failures, both goldens).
- **Smoke.** Five 11-epoch smokes of the C arm, one per day (`_rap_mday_sm_<doy>`): exit 0, 11 of 11 epochs emitted, 56 to 58 stations in the solve (on DOY 128 the 360 s parse window of a smoke holds no MAC100AUS data and the file is reported as "Failed to parse RINEX"; the 24 h arms parse it), the product windows of the table above, the second-band admission withholding BeiDou on the first epochs at MAJU00MHL, SCTB00ATA and WARK00NZL (and at MAC100AUS on DOY 129).
- **DOY 129, the first 6 h against cards BDS and BDSB.** The consumer reproduces to the printed precision: WTZZ00DEU hours 1 to 6 GE 0.058 0.011 0.018 0.010 0.015 0.011 m (688 of 720 epochs below 0.1 m), C 0.030 0.015 0.019 0.017 0.018 0.015 m (687 of 720); the 6 h arms read the same (`_rap_sbv_ge6`, `_rap_sbv_on6`). The products do not reproduce bit for bit, and the reason is a design property: the broadcast group-delay latch of the code-bias datum takes, per satellite, the low median of the healthy navigation records whose clock epoch lies in the processing window (`ApplicationNetworkSetup.cpp`, `NetworkSbasis::latchBroadcastGroupDelays`), 86,400 s here and 21,600 s there. Its run-start line reads "97 on the window rule" and three satellites latched at exactly 0 (C29, E03, E21) for the 6 h arm of card BDSB, "88 on the window rule" with the same three for the 300 s smoke, and "97 on the window rule" with two (C29, E03) for the 24 h arm: E21 is the one satellite whose latched value differs. The first 720 epochs of the 24 h arms against the 6 h arms: GPS clock datum-free rms at most 0.6 mm (median 0.0 mm); Galileo at most 16.5 mm (median 1.1 mm); BeiDou (C) at most 0.3 mm; constellation-level shift at most 2.1 mm. A 24 h run is therefore a different realization of the same posture at the millimetre level from epoch 1, and no 6 h number of another card is a bit-exact reference for it.

## 5. Results

### 5.1 Run health

**Table A. Run health.**

| day | arm | exit | wall (s) | emitted of synced | withheld (gate, sigma0) | skipped | roster, in the solve (min / median) | addStation PD rejections | orbits, clocks | ms per epoch |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| 128 | GE | 0 | 17878 | 2880 of 2880 | 0, 0 | 0 | 57, 55 / 57 | 2 | 104544, 1045332 | 6151 |
| 128 | C | 0 | 25893 | 2880 of 2880 | 0, 0 | 0 | 57, 55 / 57 | 1 | 104544, 1045332 | 8911 |
| 129 | GE | 0 | 8970 | 2880 of 2880 | 0, 0 | 0 | 58, 57 / 58 | 81 | 104544, 1045243 | 3075 |
| 129 | C | 0 | 18258 | 2880 of 2880 | 0, 0 | 0 | 58, 57 / 58 | 82 | 104544, 1045243 | 6294 |
| 130 | GE | 0 | 10743 | 2880 of 2880 | 0, 0 | 0 | 58, 56 / 58 | 567 | 104544, 1045341 | 3685 |
| 130 | C | 0 | 19986 | 2880 of 2880 | 0, 0 | 0 | 58, 56 / 58 | 446 | 104544, 1045341 | 6897 |
| 131 | GE | 0 | 10830 | 2880 of 2880 | 0, 0 | 0 | 58, 56 / 57 | 2 | 104544, 1045342 | 3719 |
| 131 | C | 0 | 19920 | 2880 of 2880 | 0, 0 | 0 | 58, 56 / 57 | 4 | 104544, 1045342 | 6852 |
| 132 | GE | 0 | 7714 | 2870 of 2872 | 0, 0 | 0 | 56, 55 / 56 | 126 | 69696, 696951 | 2652 |
| 132 | C | 0 | 15005 | 2870 of 2872 | 0, 0 | 0 | 56, 55 / 56 | 140 | 69696, 696951 | 5195 |

All ten arms exit 0 and emit every synchronised epoch inside the run length: 2,880 of 2,880 on four days, 2,870 of 2,872 on DOY 132 (the other two lie past the 86,100 s cutoff, section 3). None is skipped or withheld by the observability gate, the sigma0 ceiling or the rank gate. `n_threads` is 20 in every arm, and the warning that `parallel_carry` and `parallel_reduce` together switch the carry parallelism off does not occur in any log. The addStation positive-definiteness guard rejects BSHM00ISR on three days (81 epochs under GE and 82 under C on DOY 129, 557 and 436 on DOY 130, 124 and 138 on DOY 132), MAC100AUS on three (1 to 10 epochs), MAJU00MHL on DOY 132 (2) and MRC100USA on DOY 131 under C (2); the single-band BeiDou stations of card BDS (MAJU00MHL, SCTB00ATA, WARK00NZL) are never rejected for their BeiDou in C.

**Cost.** The wall time of C is 1.45 (DOY 128), 2.04, 1.86, 1.84 and 1.95 times that of GE. The load on the shared box was not controlled (another workflow kept a varying number of processes running, up to ten at once, heaviest while the DOY 128 arms and the DOY 129 census ran), so the ratio is read as about 1.8 to 2.0 on the four days of comparable load, and 1.45 on DOY 128 where both arms ran under heavy outside load. C includes the exact sparse station normals, which cards NETCOST and NETCOSTB measured as an 11 to 13 % shorter epoch loop.

### 5.2 The held-out consumer WTZZ00DEU

**Table B. WTZZ00DEU, 3-D p50 by hour (m), the in-run consumer on the arm's own systems** (GE: GPS + Galileo; C: GPS + Galileo + BeiDou).

| hour | 128 GE | 128 C | 129 GE | 129 C | 130 GE | 130 C | 131 GE | 131 C | 132 GE | 132 C |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| 1 | 0.062 | 0.041 | 0.058 | 0.030 | 0.089 | 0.059 | 0.082 | 0.083 | 0.072 | 0.056 |
| 2 | 0.025 | 0.015 | 0.011 | 0.015 | 0.017 | 0.012 | 0.027 | 0.018 | 0.018 | 0.017 |
| 3 | 0.022 | 0.013 | 0.018 | 0.019 | 0.027 | 0.012 | 0.014 | 0.014 | 0.016 | 0.014 |
| 4 | 0.009 | 0.010 | 0.010 | 0.017 | 0.039 | 0.028 | 0.019 | 0.013 | 0.019 | 0.016 |
| 5 | 0.012 | 0.009 | 0.015 | 0.018 | 0.060 | 0.047 | 0.036 | 0.024 | 0.022 | 0.020 |
| 6 | 0.006 | 0.011 | 0.011 | 0.015 | 0.053 | 0.030 | 0.035 | 0.015 | 0.010 | 0.014 |
| 7 | 0.014 | 0.017 | 0.015 | 0.013 | 0.040 | 0.023 | 0.023 | 0.007 | 0.011 | 0.013 |
| 8 | 0.036 | 0.037 | 0.013 | 0.015 | 0.011 | 0.012 | 0.014 | 0.013 | 0.021 | 0.020 |
| 9 | 0.038 | 0.039 | 0.015 | 0.013 | 0.025 | 0.024 | 0.025 | 0.027 | 0.033 | 0.038 |
| 10 | 0.023 | 0.026 | 0.023 | 0.020 | 0.026 | 0.033 | 0.026 | 0.027 | 0.017 | 0.025 |
| 11 | 0.008 | 0.009 | 0.016 | 0.016 | 0.018 | 0.017 | 0.024 | 0.024 | 0.022 | 0.024 |
| 12 | 0.010 | 0.009 | 0.009 | 0.008 | 0.010 | 0.009 | 0.024 | 0.024 | 0.023 | 0.025 |
| 13 | 0.011 | 0.009 | 0.012 | 0.009 | 0.013 | 0.012 | 0.017 | 0.020 | 0.021 | 0.027 |
| 14 | 0.007 | 0.006 | 0.011 | 0.008 | 0.016 | 0.012 | 0.011 | 0.014 | 0.033 | 0.026 |
| 15 | 0.013 | 0.012 | 0.013 | 0.012 | 0.010 | 0.008 | 0.017 | 0.020 | 0.025 | 0.019 |
| 16 | 0.016 | 0.009 | 0.020 | 0.013 | 0.025 | 0.014 | 0.030 | 0.021 | 0.014 | 0.016 |
| 17 | 0.015 | 0.013 | 0.015 | 0.016 | 0.027 | 0.019 | 0.039 | 0.023 | 0.014 | 0.025 |
| 18 | 0.014 | 0.012 | 0.023 | 0.019 | 0.040 | 0.023 | 0.017 | 0.012 | 0.023 | 0.030 |
| 19 | 0.018 | 0.016 | 0.012 | 0.011 | 0.030 | 0.021 | 0.018 | 0.014 | 0.015 | 0.025 |
| 20 | 0.020 | 0.023 | 0.013 | 0.011 | 0.014 | 0.013 | 0.016 | 0.017 | 0.024 | 0.028 |
| 21 | 0.025 | 0.018 | 0.014 | 0.011 | 0.011 | 0.011 | 0.024 | 0.025 | 0.028 | 0.023 |
| 22 | 0.032 | 0.024 | 0.023 | 0.016 | 0.014 | 0.014 | 0.028 | 0.027 | 0.017 | 0.014 |
| 23 | 0.023 | 0.014 | 0.029 | 0.016 | 0.011 | 0.012 | 0.041 | 0.033 | 0.019 | 0.007 |
| 24 | 0.011 | 0.009 | 0.025 | 0.011 | 0.009 | 0.010 | 0.023 | 0.014 | 0.011 | 0.007 |

| day | arm | pooled p50, hours 2 to 24 (m) | p95 | mean of the hourly p50 | epochs below 0.1 m | epochs | maximum after hour 1 (m) at (UT) | epochs above 1 m after hour 1 |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| 128 | GE | 0.017 | 0.039 | 0.018 | 2862 | 2880 | 0.058 at 07:36:30 | 0 |
| 128 | C | 0.014 | 0.036 | 0.016 | 2855 | 2880 | 0.070 at 07:47:00 | 0 |
| 129 | GE | 0.015 | 0.031 | 0.016 | 2848 | 2880 | 0.056 at 08:21:00 | 0 |
| 129 | C | 0.014 | 0.026 | 0.014 | 2847 | 2880 | 0.054 at 08:21:00 | 0 |
| 130 | GE | 0.021 | 0.055 | 0.024 | 2832 | 2880 | 0.080 at 05:08:00 | 0 |
| 130 | C | 0.016 | 0.044 | 0.018 | 2854 | 2880 | 0.072 at 09:28:00 | 0 |
| 131 | GE | 0.023 | 0.042 | 0.024 | 2845 | 2880 | 0.059 at 22:36:00 | 0 |
| 131 | C | 0.019 | 0.035 | 0.019 | 2842 | 2880 | 0.055 at 10:36:00 | 0 |
| 132 | GE | 0.019 | 0.038 | 0.020 | 2838 | 2870 | 0.054 at 13:36:00 | 0 |
| 132 | C | 0.021 | 0.037 | 0.021 | 2832 | 2870 | 0.057 at 08:39:30 | 0 |

Hour 1 is ignition (GE 0.062 0.058 0.089 0.082 0.072 m, C 0.041 0.030 0.059 0.083 0.056 m on DOY 128 to 132). In hours 2 to 24 the hourly p50 stays at or under 0.060 m under GE (DOY 130, hour 5) and 0.047 m under C (the same hour); the pooled p50 lies at 0.015 to 0.023 m (GE) and 0.014 to 0.021 m (C), and over the whole day 2,832 to 2,862 epochs of 2,870 to 2,880 are below 0.1 m. The elevation of DOY 130 in hours 4 to 7 (GE 0.039 0.060 0.053 0.040, C 0.028 0.047 0.030 0.023 m) is the WTZZ00DEU event card MDR recorded on that day under the earlier golden (0.054 to 0.079 m in hours 4 to 6); the census median does not move in those hours (Table C), so it belongs to the site and the day. C against GE at WTZZ00DEU: the pooled p50 is lower by 3.0, 0.8, 4.8 and 4.2 mm on DOY 128 to 131 and higher by 1.8 mm on DOY 132. WTZZ00DEU is one instrument (card MSC) and is read, not adopted on.

### 5.3 The 16-site census

**Table C. 16-site census, median over the sites of the site 3-D p50 by hour (m).**

| hour | 128 GE | 128 C | 129 GE | 129 C | 130 GE | 130 C | 131 GE | 131 C | 132 GE | 132 C |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| 1 | 0.149 | 0.086 | 0.137 | 0.097 | 0.142 | 0.077 | 0.119 | 0.088 | 0.117 | 0.077 |
| 2 | 0.050 | 0.029 | 0.054 | 0.033 | 0.036 | 0.025 | 0.043 | 0.038 | 0.067 | 0.044 |
| 3 | 0.032 | 0.027 | 0.041 | 0.031 | 0.039 | 0.030 | 0.039 | 0.033 | 0.065 | 0.044 |
| 4 | 0.034 | 0.027 | 0.036 | 0.029 | 0.035 | 0.025 | 0.037 | 0.025 | 0.035 | 0.030 |
| 5 | 0.035 | 0.022 | 0.040 | 0.027 | 0.031 | 0.028 | 0.045 | 0.032 | 0.039 | 0.030 |
| 6 | 0.032 | 0.028 | 0.037 | 0.029 | 0.038 | 0.032 | 0.044 | 0.032 | 0.026 | 0.024 |
| 7 | 0.035 | 0.030 | 0.032 | 0.028 | 0.035 | 0.031 | 0.038 | 0.032 | 0.027 | 0.027 |
| 8 | 0.032 | 0.025 | 0.033 | 0.028 | 0.040 | 0.032 | 0.036 | 0.030 | 0.024 | 0.024 |
| 9 | 0.031 | 0.031 | 0.027 | 0.026 | 0.029 | 0.026 | 0.032 | 0.030 | 0.032 | 0.027 |
| 10 | 0.030 | 0.024 | 0.030 | 0.027 | 0.031 | 0.028 | 0.031 | 0.029 | 0.033 | 0.026 |
| 11 | 0.037 | 0.034 | 0.029 | 0.027 | 0.025 | 0.025 | 0.027 | 0.027 | 0.029 | 0.023 |
| 12 | 0.038 | 0.035 | 0.028 | 0.026 | 0.027 | 0.024 | 0.024 | 0.025 | 0.023 | 0.021 |
| 13 | 0.033 | 0.030 | 0.030 | 0.029 | 0.029 | 0.026 | 0.025 | 0.025 | 0.031 | 0.028 |
| 14 | 0.034 | 0.027 | 0.034 | 0.032 | 0.033 | 0.032 | 0.031 | 0.034 | 0.029 | 0.028 |
| 15 | 0.025 | 0.023 | 0.042 | 0.032 | 0.025 | 0.028 | 0.031 | 0.026 | 0.028 | 0.027 |
| 16 | 0.031 | 0.023 | 0.033 | 0.030 | 0.037 | 0.033 | 0.029 | 0.030 | 0.030 | 0.025 |
| 17 | 0.032 | 0.026 | 0.032 | 0.027 | 0.036 | 0.030 | 0.029 | 0.029 | 0.035 | 0.029 |
| 18 | 0.031 | 0.032 | 0.035 | 0.026 | 0.032 | 0.028 | 0.027 | 0.026 | 0.029 | 0.027 |
| 19 | 0.030 | 0.025 | 0.028 | 0.027 | 0.027 | 0.027 | 0.025 | 0.025 | 0.037 | 0.029 |
| 20 | 0.026 | 0.030 | 0.030 | 0.025 | 0.030 | 0.031 | 0.027 | 0.026 | 0.025 | 0.022 |
| 21 | 0.034 | 0.044 | 0.028 | 0.027 | 0.037 | 0.030 | 0.028 | 0.027 | 0.033 | 0.033 |
| 22 | 0.034 | 0.038 | 0.026 | 0.025 | 0.032 | 0.029 | 0.029 | 0.029 | 0.030 | 0.023 |
| 23 | 0.040 | 0.043 | 0.026 | 0.030 | 0.030 | 0.030 | 0.027 | 0.029 | 0.028 | 0.026 |
| 24 | 0.038 | 0.035 | 0.031 | 0.025 | 0.032 | 0.028 | 0.035 | 0.034 | 0.033 | 0.024 |

**Table D. Census, C against GE, paired by site.** Change of each site's mean hourly p50 over hours 2 to 24 (hour 1 separately); better and worse at 1 cm.

| day | sites | median site, GE (m) | median site, C (m) | paired median change, hours 2 to 24 (m) | better / worse | hour 1 median change (m) | better / worse | largest worsening (site, m) | largest gain (site, m) |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| 128 | 16 | 0.031 | 0.027 | -0.0015 | 2 / 0 | -0.0281 | 9 / 2 | UTQI00USA +0.0003 | ARHT00ATA -0.0948 |
| 129 | 16 | 0.032 | 0.029 | -0.0032 | 4 / 0 | -0.0414 | 11 / 1 | ASCG00SHN +0.0028 | ARHT00ATA -0.1254 |
| 130 | 16 | 0.036 | 0.030 | -0.0024 | 1 / 0 | -0.0036 | 7 / 1 | ASCG00SHN +0.0029 | ARHT00ATA -0.1186 |
| 131 | 16 | 0.034 | 0.033 | -0.0008 | 1 / 0 | -0.0037 | 5 / 5 | ASCG00SHN +0.0024 | POHN00FSM -0.0110 |
| 132 | 16 | 0.039 | 0.032 | -0.0038 | 2 / 0 | -0.0197 | 10 / 2 | SCRZ00BOL +0.0004 | ARHT00ATA -0.0766 |

**Table E. The named sites, mean hourly p50 over hours 2 to 24 (m), GE / C (change); hour 1 in brackets as GE / C.**

| site | 128 | 129 | 130 | 131 | 132 |
| --- | --- | --- | --- | --- | --- |
| ARHT00ATA | 0.114 / 0.019 (-0.0948) [0.392 / 0.121] | 0.147 / 0.022 (-0.1254) [0.304 / 0.093] | 0.142 / 0.024 (-0.1186) [0.372 / 0.054] | 0.023 / 0.021 (-0.0014) [0.047 / 0.033] | 0.103 / 0.026 (-0.0766) [0.611 / 0.035] |
| DAV100ATA | 0.031 / 0.025 (-0.0056) [0.142 / 0.091] | 0.051 / 0.033 (-0.0180) [0.303 / 0.148] | 0.031 / 0.029 (-0.0026) [0.393 / 0.142] | 0.032 / 0.025 (-0.0072) [0.190 / 0.244] | 0.036 / 0.028 (-0.0077) [0.121 / 0.062] |
| HARB00ZAF | 0.027 / 0.025 (-0.0014) [0.185 / 0.072] | 0.029 / 0.026 (-0.0035) [0.139 / 0.054] | 0.040 / 0.030 (-0.0097) [0.202 / 0.083] | 0.026 / 0.022 (-0.0048) [0.141 / 0.085] | 0.027 / 0.023 (-0.0043) [0.086 / 0.038] |
| KOKB00USA | 0.027 / 0.027 (-0.0004) [0.085 / 0.063] | 0.032 / 0.028 (-0.0035) [3.293 / 0.060] | 0.036 / 0.032 (-0.0034) [0.074 / 0.072] | 0.048 / 0.041 (-0.0075) [0.060 / 0.049] | 0.040 / 0.035 (-0.0050) [0.093 / 0.077] |

**Table F. All sites, change C minus GE of the mean hourly p50 over hours 2 to 24 (mm).**

| site | 128 | 129 | 130 | 131 | 132 |
| --- | --- | --- | --- | --- | --- |
| CHTI00NZL | -0.2 | -0.1 | -0.7 | +0.4 | +0.4 |
| SCRZ00BOL | -1.8 | +0.8 | -2.3 | +1.4 | +0.4 |
| ANMG00MYS | -0.3 | -0.2 | -0.6 | +0.5 | -0.1 |
| QUIN00USA | -2.4 | -3.4 | -6.7 | +0.7 | -6.8 |
| HARB00ZAF | -1.4 | -3.5 | -9.7 | -4.8 | -4.3 |
| POHN00FSM | -6.1 | -27.5 | +0.9 | -11.0 | -8.1 |
| DAV100ATA | -5.6 | -18.0 | -2.6 | -7.2 | -7.7 |
| BADG00RUS | -0.1 | -0.5 | -0.8 | -0.3 | -0.1 |
| STPM00SPM | -1.6 | -3.0 | -3.1 | -4.2 | -3.3 |
| ASCG00SHN | -1.5 | +2.8 | +2.9 | +2.4 | -0.9 |
| CEDU00AUS | -7.4 | -5.7 | -6.9 | -5.9 | -6.6 |
| KOKB00USA | -0.4 | -3.5 | -3.4 | -7.5 | -5.0 |
| UTQI00USA | +0.3 | +0.1 | +0.3 | +0.0 | -0.1 |
| ARHT00ATA | -94.8 | -125.4 | -118.6 | -1.4 | -76.6 |
| WUH200CHN | -15.4 | -15.1 | -9.6 | -7.0 | -43.9 |
| BAKE00CAN | -0.0 | -0.3 | +0.1 | +0.1 | +0.1 |

The census median site (Table C) sits at 0.02 to 0.07 m in every hour 2 to 24 of both arms (GE at most 0.067 m, C at most 0.044 m). Hour 1: GE 0.149 0.137 0.142 0.119 0.117 m, C 0.086 0.097 0.077 0.088 0.077 m. DOY 132 settles slowest, GE 0.067 and 0.065 m in hours 2 and 3 (C 0.044 and 0.044 m), against 0.036 to 0.054 m in hour 2 on the other days. The median over sites of each site's mean hourly p50 over hours 2 to 24 is 0.031 to 0.039 m under GE and 0.027 to 0.033 m under C.

C against GE (Table D): the paired median change is -1.5, -3.2, -2.4, -0.8 and -3.8 mm on DOY 128 to 132 with 2, 4, 1, 1 and 2 sites better and none worse at 1 cm. No site is worse by more than 2.9 mm on any day (ASCG00SHN on DOY 129, 130 and 131); the gains sit at a few sites (Table F): ARHT00ATA on four days, WUH200CHN (-7 to -44 mm), POHN00FSM (-27.5 mm on DOY 129), DAV100ATA (-18 mm on DOY 129), CEDU00AUS (-6 to -7 mm on every day). The six sites without the B1I + B3I pair (CHTI00NZL, SCRZ00BOL, ANMG00MYS, BADG00RUS, UTQI00USA, BAKE00CAN; card BDS section 2) change by at most 2.3 mm. Hour 1 (Table D): -28.1, -41.4, -3.6, -3.7 and -19.7 mm; 9, 11, 7, 5 and 10 sites better against 2, 1, 1, 5 and 2 worse.

The named sites (Table E): ARHT00ATA, the worst site of the earlier record, averages 0.114, 0.147, 0.142 and 0.103 m in hours 2 to 24 under GE on DOY 128, 129, 130 and 132 and 0.023 m on DOY 131; under C it averages 0.019 to 0.026 m on all five days (ARHT00ATA is one of the ten sites with the B1I + B3I pair, card BDS section 2). DAV100ATA, HARB00ZAF and KOKB00USA are better under C on every day by 0.4 to 18.0 mm, none worse.

**Site-hour cells.** Of the 391 consumer-hours per arm and day (17 consumers, hours 2 to 24), the cells above 0.10 m number 5, 7, 6, 2 and 3 under C (largest 0.206 m, UTQI00USA hour 20 of DOY 129) and 22, 32, 24, 3 and 25 under GE (largest 0.371 m, ARHT00ATA hour 2 of DOY 129). Over the five days that is 106 cells under GE (ARHT00ATA 55, POHN00FSM 18, WUH200CHN 11, UTQI00USA 8, the rest 14) and 23 under C (UTQI00USA 8, POHN00FSM 7, SCRZ00BOL 3, the rest 5) of 1,955.

### 5.4 Ignition and divergence

Epochs above 1 m inside hour 1, summed over the 17 consumers (2,040 epochs per arm and day), the consumers affected, and the latest epoch above 1 m:

| day | GE: epochs, consumers, latest epoch above 1 m | C: epochs, consumers, latest epoch above 1 m |
| --- | --- | --- |
| 128 | 93, 11, 00:42:30 (SCRZ00BOL) | 60, 10, 00:13:30 (SCRZ00BOL) |
| 129 | 245, 13, 00:42:30 (KOKB00USA, 12.6 m at its worst) | 126, 15, 00:10:30 (SCRZ00BOL) |
| 130 | 192, 14, 00:29:00 (SCRZ00BOL) | 105, 15, 00:29:00 (SCRZ00BOL) |
| 131 | 86, 13, 00:36:30 (SCRZ00BOL) | 69, 15, 00:38:00 (SCRZ00BOL) |
| 132 | 170, 17, 00:37:00 (SCRZ00BOL) | 109, 15, 00:36:30 (SCRZ00BOL) |

Every consumer of both arms is under 1 m from 00:43 UT on every day. After hour 1:

**Table G. Divergence screen.** The largest hourly p50 in hours 2 to 24 (m) over WTZZ00DEU and the 16 sites, and the epochs above 1 m after hour 1.

| day | arm | largest hourly p50 (consumer, hour, m) | consumers with an epoch above 1 m after hour 1 |
| --- | --- | --- | --- |
| 128 | GE | ARHT00ATA, hour 3, 0.246 | none |
| 128 | C | POHN00FSM, hour 24, 0.122 | none |
| 129 | GE | ARHT00ATA, hour 2, 0.371 | none |
| 129 | C | UTQI00USA, hour 20, 0.206 | none |
| 130 | GE | ARHT00ATA, hour 2, 0.278 | ARHT00ATA (53) |
| 130 | C | SCRZ00BOL, hour 3, 0.201 | none |
| 131 | GE | POHN00FSM, hour 3, 0.196 | none |
| 131 | C | POHN00FSM, hour 3, 0.126 | none |
| 132 | GE | WUH200CHN, hour 6, 0.295 | none |
| 132 | C | ANMG00MYS, hour 10, 0.118 | none |

The one consumer above 1 m after hour 1 is ARHT00ATA under GE on DOY 130: 53 consecutive epochs from 09:42:30 (1.725 m) to 10:08:30 (1.021 m), the maximum 2.198 m at 09:43:30, falling to 0.72 m by 10:12:30 (the hourly p50 of hour 10 stays below 1 m, at 0.145 m). Under C the same site reads 0.020 m at 09:42:30 and 0.032 m at 09:43:00. The excursion starts within a minute of the G03 clock event of section 6.1. The two divergences card MDR recorded at ARHT00ATA under the earlier GPS-only golden (166 m on DOY 131, 99.7 m on DOY 128) do not recur under either posture: the largest ARHT00ATA hourly p50 in hours 2 to 24 is 0.371 m (GE, DOY 129), and 0.047 m on DOY 131. That record was GPS only and 6 h, a different posture, so the comparison is of the outcome, not of a mechanism.

### 5.5 The wires

**Table H. Wire clock health.** BeiDou c0 of C (median, 99th percentile, maximum |c0| with satellite and epoch), C## per epoch (median / minimum), and the change of the GPS and Galileo clocks C against GE (per-epoch rms after removing the per-epoch median: maximum / median, m).

| day | BeiDou absolute c0, median / p99 (m) | maximum (m, satellite, UT) | C## per epoch | GPS change rms max / median (m) | Galileo change rms max / median (m) | GPS level shift, max (m) | Galileo level shift, max (m) |
| --- | --- | --- | --- | --- | --- | --- | --- |
| 128 | 2.92 / 4.78 | 6.21, C29, 19:29:30 | 38 / 37 | 0.238 / 0.020 | 0.105 / 0.016 | 0.104 | 0.120 |
| 129 | 3.91 / 5.63 | 13.88, C20, 01:19:30 | 38 / 37 | 0.070 / 0.051 | 0.083 / 0.057 | 0.088 | 0.052 |
| 130 | 3.12 / 8.45 | 14.69, C46, 09:44:00 | 38 / 37 | 2.873 / 2.055 | 0.476 / 0.167 | 0.056 | 0.148 |
| 131 | 3.63 / 5.24 | 5.58, C25, 00:11:30 | 38 / 37 | 0.202 / 0.018 | 0.093 / 0.028 | 0.060 | 0.066 |
| 132 | 3.67 / 5.26 | 5.47, C11, 00:00:00 | 38 / 37 | 0.053 / 0.030 | 0.059 / 0.025 | 0.127 | 0.043 |

BeiDou is in the metre class on every day (largest |c0| 14.7 m, C46 on DOY 130 at 09:44:00, during the G03 event), 38 satellites per epoch with a minimum of 37 (the COD MGX SP3 holds 38): failure mode A of card BDS does not occur on any day. The change of the GPS and Galileo clocks between the arms is 0.02 to 0.06 m rms (median over epochs) on DOY 128, 129, 131 and 132; DOY 130 differs, GPS 2.05 m median and 2.87 m maximum, through G03 and, less, G04 (section 6.1).

The second-band admission (arm C), run-end counters:

| day | records screened | withheld (unit-epochs) | edges, withhold / readmit | BeiDou withheld at | Galileo withheld at |
| --- | --- | --- | --- | --- | --- |
| 128 | 3,794,237 | 115,192 (8,685) | 47 / 44 | BSHM00ISR, MAC100AUS, MAJU00MHL, SCTB00ATA, WARK00NZL (115,182 records) | MAC100AUS (10 records) |
| 129 | 3,969,457 | 118,159 (8,962) | 32 / 29 | the same five (115,366) | BSHM00ISR, MAC100AUS, PALM00ATA, WROC00POL (2,793) |
| 130 | 3,970,931 | 125,423 (9,572) | 93 / 90 | the same five (121,045) | BSHM00ISR, MAC100AUS, MAJU00MHL (4,378) |
| 131 | 3,892,280 | 113,711 (8,648) | 10 / 5 | the same five (113,695) | MAC100AUS, MRC100USA (16) |
| 132 | 3,820,450 | 119,836 (9,212) | 20 / 17 | BSHM00ISR, MAJU00MHL, SCTB00ATA, WARK00NZL (115,801) | BSHM00ISR, PALM00ATA (4,035) |

The rule acts as designed on every day: the three single-band BeiDou stations are held out for the whole day and nothing else is withheld at scale; the Galileo records it withholds are a few thousand at two to four stations on three days.

## 6. Findings (reads)

### 6.1 DOY 130: the G03 clock event at 09:42 UT, handled differently by the two arms

At 09:42 UT (34,920 s into the day) both arms decouple the clock anchor and declare gross lines of sight on G03. The emitted G03 clock steps by about -9 m in both wires (2.14 m at 09:41:30 to -7.16 m at 09:42:30 under GE, 2.11 m to -6.35 m under C), and the satellite is then withheld from the wire: for 46 epochs under GE (back at 10:06:00) and for 126 under C (back at 10:46:00). On its return GE carries G03 at 4.17 m (10:46:00; 2.29 m at 16:40), 2.0 m above its level before the event and back under 8 m from the a priori product from 10:05:30. C carries G03 at -11.46 m (10:46:00; -9.97 m at 16:40), 13.6 m under its level before the event (2.11 m), about 11 m from the product for the rest of the day (1,714 epochs above 8 m, against 45 under GE), and 11.3 m under the GE wire at 23:59:30 (datum-free). G04 differs as well, by -3.7 m at 09:43:00 easing to -0.9 m at the end of the day (1,714 epochs beyond 0.8 m), and G08 by -1 m for 20 minutes. Before 09:42 the datum-free difference of G03 between the wires is at most 0.03 m.

Consequences read. (1) ARHT00ATA under GE leaves 1 m for 26 minutes (section 5.4); under C it does not move. (2) Under C no consumer shows a step at or after 10:46: the census median stays at 0.024 to 0.033 m in hours 11 to 24, and WTZZ00DEU at 0.008 to 0.023 m in hours 15 to 19, when it uses G03 on 393 epochs per signal between 14:00 and 18:00 with the wire clock at -10.1 m (median) against 2.3 m under GE. (3) The GPS clock change of Table H on this day (2.05 m median rms, 2.87 m maximum) is these satellites: left out G03, the median falls to 0.31 m. Which G03 level is the better is not decided here, since a satellite clock has no truth in this setup; GE returns to the product's level and C does not, which reads as an offset of the C wire after a long withhold, of unread origin. An 11 to 15 m level on one satellite is not the metre-class departure of failure mode A of card BDS, and no consumer of this census reacts to it.

### 6.2 DOY 128 and 131: G01 about 100 m from the product in both arms

From 03:36:00 on DOY 128 and from 03:22:30 on DOY 131 to the end of the day, the emitted G01 clock sits 85 to 112 m from the a priori product (2,232 and 2,253 epochs; maximum 110.7 m under GE and 111.6 m under C on DOY 128, 103.1 and 103.9 m on DOY 131), identical in both arms, so a property of the product day and not of the posture. Card MDR met the same departure on DOY 128 (the wire clock rms of 12 to 13 m at the sampled epochs) and ARHT00ATA diverged on that day at 03:36. Under the current golden no consumer diverges on either day and ARHT00ATA reads 0.114 m (DOY 128) and 0.023 m (DOY 131) under GE. The origin of the G01 clock departure (the product, or an unmodelled event) was not read further.

### 6.3 A 24 h run is a different realization of a 6 h run from epoch 1

The latch of the broadcast group delay (section 4) reads the processing window, so changing the run length moves the datum of the satellites whose records change inside the window: E21 here (latched at exactly 0 in the 6 h window, at a non-zero value in the 24 h window), mm-level in the clocks (Galileo up to 16.5 mm rms in the first 720 epochs) and invisible at the consumer to the printed precision. The latch is documented as one value per run so that the datum cannot churn; the corollary is that a multi-day replay compares realizations, not a continuation of one, and that a bit-exact comparison against a 6 h arm is not available for a 24 h arm.

## 7. Answer

**Does the network deliver accurate positions on every day?** Yes, on the five days DOY 128 to 132 of 2024 (the last two inside the storm of 10 and 11 May), for both postures, at the reading of section 2. Every arm runs to the end without a withheld epoch. After the ignition hour the held-out consumer WTZZ00DEU has an hourly p50 of at most 0.060 m (GE) and 0.047 m (C) and the census median site stays at 0.02 to 0.07 m; the median over sites of the mean hourly p50 is 0.031 to 0.039 m under GE and 0.027 to 0.033 m under C. Where it is not accurate: in hour 1 (WTZZ00DEU 0.03 to 0.09 m, census median 0.08 to 0.15 m, single consumers above 1 m until 00:43 UT, the longest SCRZ00BOL and KOKB00USA); at ARHT00ATA under GE, which averages 0.10 to 0.15 m in hours 2 to 24 on four days and leaves 1 m for 26 minutes on DOY 130 (maximum 2.2 m), the only consumer above 1 m after hour 1 in either arm; and in a few site-hours between 0.10 and 0.37 m (106 of 1,955 under GE, 23 under C). The ARHT00ATA divergences of card MDR (166 m, 99.7 m) do not recur. **Is the candidate better than the golden?** Better on DOY 128, 129, 130 and 132 (census paired median -1.5, -3.2, -2.4 and -3.8 mm; 2, 4, 1 and 2 sites better at 1 cm, none worse), equal on DOY 131 (-0.8 mm, 1 better, none worse); no site is worse than +2.9 mm on any day; hour 1 is better in the median on every day (-3.6 to -41.4 mm; more sites better than worse on four days, 5 against 5 on DOY 131); ARHT00ATA is 0.08 to 0.13 m better on four days, WUH200CHN 7 to 44 mm; WTZZ00DEU is 1 to 5 mm better in the pooled p50 on four days and 2 mm worse on DOY 132. The price is about twice the epoch-loop cost, partly offset by the sparse station normals it carries. The one wire difference of consequence is G03 on DOY 130 (section 6.1), which leaves the candidate's consumers untouched at this census. Nothing is armed by this card: the second-band admission, `network.n3c_wire_per_signal` and BeiDou stay with their ledger verdicts (UNVALIDATED) and the decision to arm is the author's, through a ledger row and the generator.

## 8. Not run, and what stays open

- **Users of the C wire without BeiDou.** GPS + Galileo and GPS-only consumers of the C wire (the S5 clause of card BDS) were not run on the five days, so the better-than-golden reading is for consumers that use BeiDou where they track it. Whether a GPS + Galileo user of the C wire matches the GE wire on DOY 130 after 10:46 (G03 at -11 m) is the open question of section 6.1.
- **A same-exe C without the sparse normals.** The output identity of `network.sparse_station_normals` rests on cards NETCOST and NETCOSTB; no control arm was run here.
- **The end of DOY 132.** The DOY 133 products are not on disk and were not fetched; the DOY 132 arms were stopped at 23:54:30 as a precaution against the one-sided orbit window, which the files did not require (section 3). The last 10 epochs of that day were not run.
- **Other regimes.** The 305-station regime, a warm resume across the day boundary, a start inside the day and the satellite-scope hysteresis of the admission rule were not run (the same list as card BDSB).
- **The earlier ARHT00ATA item.** The ARHT00ATA divergence item of `docs/status/OPEN_ITEMS.md` (166 m on DOY 131, 99.7 m on DOY 128 under the earlier golden) is not reproduced by either current posture; the item text is left as written.
