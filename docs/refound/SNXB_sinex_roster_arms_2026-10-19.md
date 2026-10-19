# SNXB: the SNX card, continued (phase 1 arms: the roster at its SINEX coordinates)

**STATUS**

- **id:** SNXB
- **topic:** network-vehicle
- **status:** open
- **summary:** Card SNX's continuation (that file is at the 60 KB bar), from 2026-10-19. Phase 1 of the plan in SNX section 8.2 is built and run on the field golden, 6 h: the 54 roster stations the SINEX holds (arm S2) and the six largest offsets (arm S1) at their weekly SINEX coordinates, against a control (C) and its same-exe repeat (C2, byte-identical). The emitted satellite clocks move by 5 to 7 mm RMS, a per-satellite level; WTZZ00DEU and the 16 census sites move by at most 2.4 mm; scoring on the SINEX instead of the held coordinates moves the census median by -1.35 mm (CEDU00AUS -11.5 mm). No engine change, nothing armed, phase 2 (a loader key) not built.
- **source:** [SNX](SNX_sinex_station_coordinates_2026-10-19.md) sections 4, 8.2, 8.3 and 8.4 (the offsets, the plan, the grading truth, the arms and bars); the author's request of 2026-10-19 to build and run phase 1
- **knobs / posture:** none in the engine. Tooling: `scripts/roster_from_sinex.py`, `rapid_arm.py make --roster`, `gcensus.py --sites`, `scripts/clock_level/snx_roster_grade.py`, `scripts/clock_level/site_list_sinex.json` (commit `69e3dfd` on branch `card/snxroster`). Posture: the field golden `config/golden/golden_field_refound.json`; the arms differ from it in `network.product_output_path` and, for S1 and S2, in the `position_ecef` of the replaced stations
- **dates in the text:** 2026-10-19
- **hashes / run dirs:** exe `D:/GnssProcessorSupport/gates/exe/x64-Release/gnss_processor_snxroster.exe` (sha256 `3d6c679e8068a39879c582b4495e069c569a3706049826f96f6123e2267a288e`); instruments and roster files `D:/GnssProcessorSupport/gates/snxroster/` (`grades.txt` holds every table of section 10 as printed); arms and census replays `D:/GnssProcessorSupport/runs_nom/_rap_snxr*`; roster-file and site-list hashes in section 10.2
- **gates (frozen at pre-registration):** [SNX](SNX_sinex_station_coordinates_2026-10-19.md) section 8.4 (arms and bars), read in section 10.7

**Outcome.** Question: does replacing the roster's held coordinates by the weekly SINEX values change the emitted wire or the consumers? Result: the satellite clocks change by 5 to 7 mm RMS, mainly a constant per-satellite level (up to 13 mm), more than ten times the figure SNX section 8.4 expected; no consumer follows (WTZZ00DEU at most 0.9 mm, the 16-site census paired change median 0.00001 to 0.00005 m, largest site 2.4 mm). The SINEX as grading truth moves the census median by -1.35 mm and the worst site (CEDU00AUS) by 11.5 mm, so the case for a coordinate source rests on the truth and on the stations that break, not on the roster. Bars G0 and B1 are met; B2 is not met as written; B3 is met except that the median clause is missed by 0.03 to 0.05 mm in three arms; B4 (S3, MIZU00JPN) was not run. Standing: nothing armed; phase 2 is for the author to decide.

Section 10 below is moved from card [SNX](SNX_sinex_station_coordinates_2026-10-19.md) unchanged (branch `card/snxroster` appended it there; it was moved here at the merge of that branch, after the addendum of branch `fix/sif-psd` had brought SNX to the 60 KB bar). Sections 4, 6, 8 and 9 cited in it are those of card SNX.

## 10. Phase 1 arms of 2026-10-19: the roster at its SINEX coordinates

Section 8.2 phase 1 is built and run on the field golden. No engine change, no golden or manifest edit, nothing armed. Branch `card/snxroster` (worktree `D:/GnssProcessorSupport/worktrees/snxroster`, base `f7cddb2`, the head of main), tools in commit `69e3dfd`. `grades.txt` in `D:/GnssProcessorSupport/gates/snxroster/` holds every table below as printed.

### 10.1 What was built

- `scripts/roster_from_sinex.py`. Reads a weekly SINEX (plain or gzip), own parser (`python -I`, standard library), the solution covering the reference epoch (point A preferred; the solution number, the formal sigma and the data span are recorded). Writes `positions` (name to marker ECEF, 0.1 mm) for the stations to replace, a per-station record (source, solution, sigma, ENU and 3D offset from the config, flags), a text report in the form of section 4, the consumer entry and, with `--site-list`, the census site list at SINEX coordinates. A station absent from the file keeps the config coordinate and is named; a SINEX position more than 1 km from the config's is refused as a reused code; a data span below six days is flagged (MAC100AUS 5.0 d, LCK400IND just under 6.0 d); a file with velocity rows is refused unless `--epoch` is given. `--only NAME,...` and `--min-offset-mm X` pick the stations to replace. `--selftest` passes.
- `scripts/rapid_arm.py make --roster <roster.json>`. Replaces `network.stations[].position_ecef` from `positions`; every name must be in the arm, a shift above 1 km is refused, the consumer and every other key are untouched; `rapid.json` records the file, its sha256, the stations replaced and the largest shift. The selftest grew from 94 to 98 checks (98/98 pass).
- `scripts/clock_level/site_list_sinex.json`, written by the generator beside `site_list.json`: the 16 census sites and a `consumer` entry (WTZZ00DEU) at the weekly SINEX. `msc_sites.py` reads another list through `MSC_SITE_LIST` (unset: unchanged); `gcensus.py --sites FILE` selects it for one invocation, and a `consumer` entry of the list sets the coordinate of the WTZZ00DEU replay of that key. `gcensus.py` now takes its repository root from its own location (it was the literal `C:/GnssProcessor`), so a worktree copy calls its own `rapid_arm.py`; for the main tree the value is the same.
- `scripts/clock_level/snx_roster_grade.py`: `consumer`, `census`, `sites`, `truth-check` and `clock` (usage in its docstring).
- Checks before the commit: Debug build of the worktree (`scripts/wt_build.ps1`) and `build/x64-Debug/gnss_tests.exe --gtest_brief=1`: 3,458 cases, 3,428 passed, 30 skipped (all "fixture not present": the worktree's `data/` is empty), 0 failed. No C++ file changed. `python scripts/make_golden_config.py --verify` passes.

### 10.2 Arms

Exe: a copy of `C:/GnssProcessor/build/x64-Release/gnss_processor.exe` (21,538,304 bytes, written 2026-10-19 05:45, sha256 `3d6c679e8068a39879c582b4495e069c569a3706049826f96f6123e2267a288e`) as `D:/GnssProcessorSupport/gates/exe/x64-Release/gnss_processor_snxroster.exe`. It was built after `5e95c84` (05:17) and before `3e4a53a` (10:55); the only change under `src/` and `include/` between `5e95c84` and the head `f7cddb2` is `3e4a53a` (the default of `network.sparse_station_normals` to true, bit-identical by card NETCOST), and both goldens pin that key. Uncommitted edits in the main tree at 05:45 cannot be excluded; the control arm C is the check (below).

Base `config/golden/golden_field_refound.json` (58 stations, GPS + Galileo, WTZZ00DEU held out, 6 h from DOY 129 00:00). Each `arm.json` differs from the golden in `network.product_output_path` and, for S1 and S2, in the `position_ecef` of the replaced stations (checked key by key: C 1 difference, S1 7, S2 55). Cold start, Release, one `--hours 6` arm per tag, four arms at once on a box shared with other sessions (wall times are not a performance figure).

| tag | change from the golden | stations replaced | roster file sha256 recorded in `rapid.json` | wall (s) |
| --- | --- | --- | --- | --- |
| C | none (control) | 0 | | 7171 |
| C2 | none (same-exe repeat) | 0 | | 7155 |
| S1 | the six stations 43 to 64 mm off (against the printed config coordinates: MAC100AUS 64.0, PARK00AUS 62.2, GCGO00USA 56.3, SOLO00SLB 54.5, REYK00ISL 44.5, LCK400IND 42.9 mm; section 4 has REYK00ISL at 44.6, against the unrounded SSC extrapolation), at their SINEX values | 6 | `3266746d8e882d20...` | 7156 |
| S2 | every roster station the file holds | 54 (BSHM00ISR, WTZS00DEU, LEIJ00DEU, FFMJ00DEU keep the held value) | `5cab8f0278113398...` | 7174 |

The arm names differ from section 8.4: S1 here is the six largest offsets, S2 here is the 54-station arm of section 8.4 (its S1), and the section 8.4 S2 (re-grade on the SINEX truth) is the `x` column of the tables below. S3 (MIZU00JPN restored) was not run. The roster files were regenerated after the arms ran (a repo-relative `source.config`); their `positions` equal the arms' coordinates (all 6 and 54 compared) and they now hash to `362e77abf5aebb54...` (S1) and `0d8fbc025d0bbfae...` (S2). `site_list_sinex.json` hashes to `f593d370de94147f...`; the 17 replay arms of each `x` key carry exactly its coordinates.

**Controls.** C2 is byte-identical to C: `products.txt` (sha256 `c6de838b83ed7eb5...`), `closed_loop_trace.csv`, and all 34 census replay traces. The engine is deterministic at its thread count on a loaded box, so every difference below between S1, S2 and C is caused by the coordinates. C equals the baseline field wire `runs/_rf0hv15_field6` of card GAD at record level (every `C`, `B`, `P` and `E` record; the files differ in the `S` line and two comment lines only, 343,063 against 343,065 lines) and reproduces its consumer by hour (0.058 0.011 0.018 0.010 0.015 0.011 m). The per-epoch dispositions (all 720 emitted), `n_adapt` and the run-end counters read by `rapid_arm.py score` (688 epochs under 0.10 m, 55 T4 attributions, 111 IF slips, 41 on band 5, 3131 band-scope events, 226 clock anchors, 274 S-transforms, 401 MW detections) are the same in C, S1 and S2.

### 10.3 The held-out consumer WTZZ00DEU

Closed-loop consumer of each wire, p50 of the 3D error by hour from 00:00 (m); the SINEX rows are the same trace shifted by the coordinate difference (SINEX minus held at the consumer: E -0.1, N +1.3, U -4.3 mm), which section 10.5 shows exact. Hours 2 to 6 are the settled ones.

| wire | truth | h1 | h2 | h3 | h4 | h5 | h6 | mean h2 to h6 |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| C | held | 0.0581 | 0.0113 | 0.0183 | 0.0103 | 0.0155 | 0.0105 | 0.0132 |
| S1 | held | 0.0581 | 0.0109 | 0.0190 | 0.0101 | 0.0153 | 0.0103 | 0.0131 |
| S2 | held | 0.0581 | 0.0109 | 0.0190 | 0.0101 | 0.0153 | 0.0103 | 0.0131 |
| C | SINEX | 0.0611 | 0.0116 | 0.0202 | 0.0098 | 0.0145 | 0.0085 | 0.0129 |
| S1 | SINEX | 0.0614 | 0.0120 | 0.0211 | 0.0099 | 0.0144 | 0.0085 | 0.0132 |
| S2 | SINEX | 0.0614 | 0.0119 | 0.0211 | 0.0099 | 0.0144 | 0.0085 | 0.0132 |

- Change from C, hours 2 to 6 (m). Held truth: S1 -0.0005 +0.0006 -0.0001 -0.0001 -0.0003, S2 -0.0005 +0.0006 -0.0001 -0.0001 -0.0002. SINEX truth: S1 +0.0004 +0.0009 +0.0002 -0.0001 -0.0000, S2 +0.0003 +0.0009 +0.0001 -0.0001 -0.0000. Largest 0.9 mm.
- Epoch by epoch, the consumer position of S1 differs from C by a median 0.5 mm, 95th percentile 1.2 mm, largest 14.5 mm; for S2 0.4, 1.1 and 6.4 mm. S1 and S2 agree to 0.1 mm per hour: the six stations carry the consumer effect.
- The truth switch alone (C, SINEX against held): +0.0030 in hour 1, then +0.0003 +0.0019 -0.0005 -0.0009 -0.0020 m; mean over hours 2 to 6 -0.0002 m. These are differences of the unrounded hourly p50s (+0.00304, +0.00034, +0.00188, -0.00049, -0.00093, -0.00196; mean over hours 2 to 6 -0.00023); the difference of the rounded cells of the table (0.0129 against 0.0132) reads -0.0003 m and is not the figure.

### 10.4 The 16-site census

Consumer-only replays of each wire at WTZZ00DEU and 16 sites (`gcensus.py make`, base the wire's own `arm.json`, 6 h), keys `snxr<arm>` (held coordinates) and `snxr<arm>x` (`--sites site_list_sinex.json`). Median over the 16 sites of the hourly p50 (m):

| wire | truth | h1 | h2 | h3 | h4 | h5 | h6 | mean h2 to h6 |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| C | held | 0.1365 | 0.0545 | 0.0410 | 0.0359 | 0.0402 | 0.0370 | 0.0417 |
| S1 | held | 0.1371 | 0.0545 | 0.0407 | 0.0358 | 0.0403 | 0.0367 | 0.0416 |
| S2 | held | 0.1368 | 0.0547 | 0.0412 | 0.0360 | 0.0403 | 0.0368 | 0.0418 |
| C | SINEX | 0.1362 | 0.0552 | 0.0350 | 0.0311 | 0.0394 | 0.0338 | 0.0389 |
| S1 | SINEX | 0.1368 | 0.0549 | 0.0350 | 0.0312 | 0.0395 | 0.0335 | 0.0388 |
| S2 | SINEX | 0.1366 | 0.0550 | 0.0350 | 0.0312 | 0.0395 | 0.0336 | 0.0389 |

Paired change of each site's mean hourly p50 over hours 2 to 6 (m), 16 sites, against C on the same truth, and the truth effect (C on SINEX against C on held):

| comparison | median | mean | smallest | largest | better / worse at 1 cm | hour 1 median |
| --- | --- | --- | --- | --- | --- | --- |
| S1 against C, held | -0.00001 | +0.00010 | -0.0004 | +0.0008 | 0 / 0 | +0.00017 |
| S2 against C, held | +0.00004 | +0.00027 | -0.0003 | +0.0024 | 0 / 0 | +0.00007 |
| S1 against C, SINEX | +0.00003 | +0.00013 | -0.0002 | +0.0009 | 0 / 0 | +0.00022 |
| S2 against C, SINEX | +0.00005 | +0.00029 | -0.0001 | +0.0023 | 0 / 0 | +0.00007 |
| truth: SINEX against held, on C | -0.00135 | -0.00174 | -0.0115 | +0.0035 | 1 / 0 | +0.0006 |

- Hour 1 (ignition), largest single-site change: QUIN00USA +0.0212 m and ARHT00ATA -0.0147 m (S1), ARHT00ATA -0.0127 m (S2), against hour-1 p50 of 0.096 and 0.304 m.
- Per site, hours 2 to 6 mean p50 (m): the coordinate difference (SINEX minus held), C on the held and on the SINEX truth, and the change of S1 and S2 from C on the held truth.

| site | SINEX - held (mm) | C, held | C, SINEX | truth effect | S1 - C | S2 - C |
| --- | --- | --- | --- | --- | --- | --- |
| WTZZ00DEU (replay) | 4.5 | 0.0132 | 0.0129 | -0.0002 | -0.0001 | -0.0000 |
| CHTI00NZL | 5.6 | 0.0427 | 0.0413 | -0.0014 | +0.0001 | +0.0001 |
| SCRZ00BOL | 7.8 | 0.0450 | 0.0486 | +0.0035 | +0.0001 | +0.0001 |
| ANMG00MYS | 3.1 | 0.0545 | 0.0568 | +0.0023 | -0.0001 | -0.0001 |
| QUIN00USA | 13.3 | 0.0264 | 0.0216 | -0.0047 | -0.0002 | -0.0003 |
| HARB00ZAF | 3.5 | 0.0391 | 0.0374 | -0.0017 | -0.0001 | +0.0005 |
| POHN00FSM | 5.1 | 0.0678 | 0.0656 | -0.0021 | +0.0001 | +0.0000 |
| DAV100ATA | 1.7 | 0.1293 | 0.1290 | -0.0003 | +0.0007 | +0.0013 |
| BADG00RUS | 5.6 | 0.0249 | 0.0235 | -0.0014 | -0.0000 | +0.0000 |
| STPM00SPM | 5.3 | 0.0239 | 0.0246 | +0.0006 | -0.0001 | -0.0001 |
| ASCG00SHN | 18.0 | 0.0351 | 0.0316 | -0.0035 | -0.0000 | +0.0000 |
| CEDU00AUS | 15.1 | 0.0427 | 0.0313 | -0.0115 | +0.0004 | +0.0003 |
| KOKB00USA | 1.7 | 0.0495 | 0.0497 | +0.0001 | +0.0002 | +0.0002 |
| UTQI00USA | 11.3 | 0.0491 | 0.0438 | -0.0054 | +0.0000 | +0.0000 |
| ARHT00ATA | 3.1 | 0.2253 | 0.2240 | -0.0013 | +0.0008 | +0.0024 |
| WUH200CHN | 2.1 | 0.0456 | 0.0451 | -0.0005 | -0.0001 | +0.0000 |
| BAKE00CAN | 4.1 | 0.0165 | 0.0158 | -0.0006 | -0.0004 | -0.0003 |

The four sites whose coordinate difference exceeds 11 mm (ASCG00SHN 18.0, CEDU00AUS 15.1, QUIN00USA 13.3, UTQI00USA 11.3) all read better on the SINEX truth, by 3.5 to 11.5 mm; SCRZ00BOL (+3.5) and ANMG00MYS (+2.3) read worse. In all, 12 sites read better and 4 worse.

### 10.5 The replays do not read their truth

`snx_roster_grade.py truth-check`: for each of C, S1, S2 and C2, the replays on the held truth and on the SINEX truth (17 consumers, 12,239 epoch rows) differ in their east, north and up error by the coordinate difference alone, to within 0.0001 m (the print precision of the trace, both columns rounded). A consumer's solution is independent of the coordinate it is scored against, and the SINEX rows of section 10.3, obtained by shifting, are exact.

### 10.6 The clock products, S1 and S2 against C

Per epoch, the RMS over satellites of the difference of the emitted `C` record (metres), after the per-epoch common mean is removed (per constellation; "pooled" removes one mean over GPS and Galileo together), averaged over the epochs of each hour. 720 of 720 epochs are in both wires. C2 against C is zero in every cell.

| wire | set | h1 | h2 | h3 | h4 | h5 | h6 | mean h2 to h6 | largest epoch |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| S1 | GPS | 0.0046 | 0.0048 | 0.0054 | 0.0070 | 0.0091 | 0.0098 | 0.0072 | 0.0106 |
| S1 | Galileo | 0.0055 | 0.0048 | 0.0054 | 0.0058 | 0.0055 | 0.0061 | 0.0055 | 0.0068 |
| S1 | pooled | 0.0100 | 0.0098 | 0.0100 | 0.0098 | 0.0097 | 0.0089 | 0.0096 | 0.0108 |
| S2 | GPS | 0.0043 | 0.0046 | 0.0050 | 0.0067 | 0.0085 | 0.0094 | 0.0069 | 0.0098 |
| S2 | Galileo | 0.0045 | 0.0040 | 0.0044 | 0.0053 | 0.0053 | 0.0063 | 0.0051 | 0.0072 |
| S2 | pooled | 0.0086 | 0.0086 | 0.0088 | 0.0090 | 0.0088 | 0.0083 | 0.0087 | 0.0098 |
| S2 against S1 | GPS | 0.0015 | 0.0020 | 0.0023 | 0.0029 | 0.0030 | 0.0029 | 0.0026 | 0.0034 |
| S2 against S1 | Galileo | 0.0026 | 0.0038 | 0.0034 | 0.0026 | 0.0021 | 0.0020 | 0.0028 | 0.0040 |

- Without the common-mean removal the difference to C is 0.011 m (GPS) and 0.008 m (Galileo) in S1 and 0.011 and 0.006 m in S2, hours 2 to 6.
- Structure, hours 2 to 6, per-constellation set, all sat-epochs: S1 against C 6.70 mm RMS, of which a constant per-satellite part (RMS of the 57 satellite means) 5.37 mm; S2 against C 6.33 mm and 5.08 mm; S2 against S1 2.74 mm and 2.29 mm. The largest satellite means are G16 -13.5, G27 -12.7, G26 -11.7, G03 +11.4 and E07 +11.2 mm (S1). The difference is mainly a per-satellite level, not epoch noise.
- Adding the 48 smaller offsets (S2) changes the S1 result by 2.7 mm, less than half of S1's own distance from C.

The same wires against the CODE clock (`scripts/ftc_step_attrib.py --r3 --systems G,E`, field wire, p50 over arcs of the within-arc RMS of the wire's clock record, a delta on the CODE clock, hours 2 to 6, m; "trk" is the consumer-tracked subset, "dm" the difference-median de-commoned variant):

| system, set | C | S1 | S2 | S1 - C | S2 - C |
| --- | --- | --- | --- | --- | --- |
| G, all | 0.0814 | 0.0826 | 0.0820 | +0.0012 | +0.0006 |
| G, trk | 0.0658 | 0.0660 | 0.0650 | +0.0002 | -0.0008 |
| G, all, dm | 0.0230 | 0.0220 | 0.0220 | -0.0010 | -0.0010 |
| G, trk, dm | 0.0195 | 0.0189 | 0.0189 | -0.0006 | -0.0006 |
| E, all | 0.0222 | 0.0235 | 0.0249 | +0.0013 | +0.0027 |
| E, trk | 0.0139 | 0.0135 | 0.0138 | -0.0004 | -0.0001 |
| E, all, dm | 0.0220 | 0.0235 | 0.0236 | +0.0015 | +0.0016 |
| E, trk, dm | 0.0127 | 0.0133 | 0.0131 | +0.0006 | +0.0004 |

Signs are mixed and the changes are at most 2.7 mm. The GPS-only wire-against-CODE read of `scripts/refound_grade.py` (within-arc `clk` rows, tracking and declared segmentations, hours 2 to 6) moves in both directions: the p50 and the rms by at most 0.004 m (largest p50 change 0.003 m, largest rms change 0.004 m), the 95th percentile by at most 0.007 m (S1, tracking, hour 3: 0.118 to 0.125 m; S2 at most 0.004 m) (`rfgrade_*.txt`).

### 10.7 Against the bars of section 8.4

None of the bars is a result; they were proposed for the author to set. Read as written:

| bar | reading |
| --- | --- |
| G0, goldens unchanged, `--verify` | met; C reproduces the baseline wire at record level (10.2) |
| B1, WTZZ00DEU S1 within 0.003 m of C at the hourly p50, hours 2 to 6, both on the SINEX truth | met; largest 0.0009 m (S2 the same) |
| B2, clock wire not worse than C by more than 0.0002 m (all) and 0.0001 m (consumer-tracked) | not met as written (the instrument of the bar was not named in section 8.4; the rows are those of `ftc_step_attrib.py --r3`): the raw rows exceed it for GPS all satellites (+0.0012, +0.0006 m), GPS consumer-tracked S1 (+0.0002) and Galileo all satellites (+0.0013, +0.0027); the de-commoned GPS rows are better (-0.0010, -0.0006) and the de-commoned Galileo rows worse (+0.0015, +0.0016 all; +0.0006, +0.0004 tracked) |
| B3, census paired against C on one truth: median change not positive; no site worse than 0.010 m | the largest site change is +0.0024 m, so the 0.010 m clause is met; the median clause is met by S1 on the held truth (-0.00001 m) and missed by 0.03 to 0.05 mm by the other three (all four are zero to 0.1 mm, below the resolution of the trace) |
| B4, S3 | not run |

**Expected result of section 8.4, against what was found.** Section 8.4 expected the stations above 5 cm to contribute "a few 0.0001 m" on the all-satellite wire, by linear scaling of card SCU's quadrature figure. The measured wire difference is 5 to 7 mm RMS, more than ten times that figure, so the scaling underestimates the clock effect of replacing roster coordinates. The other expectation, a null at the consumer and in the census, held (section 10.3 and 10.4).

### 10.8 Reading

- **What changes.** The emitted satellite clocks, by a per-satellite level of up to 13 mm (5 mm RMS) that the six stations of S1 carry almost entirely. Whether S1 or S2 sits closer to the true clocks is not resolved by the wire-against-CODE rows: they move by up to 2.7 mm with mixed signs, and the de-commoned GPS rows improve.
- **What does not.** The consumers: WTZZ00DEU and the 16 census sites follow none of the clock change (paired census change, median 0.00001 to 0.00005 m, largest site 2.4 mm). Why the consumer is insensitive to a per-satellite clock level of this size was not examined. This is a statement about 17 consumers and 6 h of one day.
- **What the coordinates are worth to the grade.** The truth switch changes the census median by -1.35 mm and the worst site by 11.5 mm, against 0.05 mm and 2.4 mm for the roster. The held-out consumer's own figure moves by 0.2 mm in the mean of hours 2 to 6. The case for phase 2 (section 8.2) therefore rests on the truth and on the stations that break, as section 8.4 anticipated, and a coordinate source for the roster alone changes the wire by millimetres without changing any consumer.
- **Not covered.** S3 (MIZU00JPN restored, the post-seismic case); the four roster stations absent from the file; a second day or network; the sim bed (known positions).
- **Decision for the author.** Phase 2 (a loader key) is not built. Nothing here arms a source: the generator, the arm option and the SINEX site list are tooling, default unchanged.

### 10.9 Reproduction

```
python -I scripts/roster_from_sinex.py --sinex <weekly.SNX.gz> [--only MAC100AUS,PARK00AUS,GCGO00USA,SOLO00SLB,REYK00ISL,LCK400IND] --out roster.json
python -I scripts/rapid_arm.py make --base config/golden/golden_field_refound.json --tag snxr_S1 --hours 6 --roster roster.json   # RAPID_RUNS_ROOT=D:/GnssProcessorSupport/runs_nom
python -I scripts/rapid_arm.py run --tags snxr_C snxr_S1 snxr_S2 --jobs 3 --exe <Release exe copy>
python scripts/clock_level/gcensus.py [--sites scripts/clock_level/site_list_sinex.json] make snxrS1[x] _rap_snxr_S1 _rap_snxr_S1 <exe>
python -I scripts/clock_level/snx_roster_grade.py consumer|census|sites|truth-check|clock ...
```

`gcensus.py` runs without `-I` (it imports `msc_sites`); `ftc_step_attrib.py` and `refound_grade.py` need numpy and pandas (the Python 3.14 install has both) and run without `-I`.
