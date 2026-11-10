# SNXB: the SNX card, continued (phase 1 arms: the roster at its SINEX coordinates)

**STATUS**

- **id:** SNXB
- **topic:** network-vehicle
- **status:** closed
- **summary:** CLOSED 2026-10-31. Card SNX's continuation (that file is at the 60 KB bar). Phase 1 of the plan in SNX section 8.2 is built and run on the field golden, 6 h. 2026-10-19 (section 10): the 54 roster stations the SINEX holds (arm S2) and the six largest offsets (arm S1) at their weekly SINEX coordinates move the emitted satellite clocks by 5 to 7 mm RMS, a per-satellite level, and WTZZ00DEU and the 16 census sites by 2.4 mm at most; scoring on the SINEX instead of the held coordinates moves the census median by -1.35 mm (CEDU00AUS -11.5 mm). 2026-10-31 (sections 11 and 12, pre-registered, current golden, exe of the baseline pair `_rf0hv17`): S2c repeats the wire effect (6.9 and 5.1 mm RMS) and moves ARHT00ATA by 7.4 mm and the next site by 1.6 mm; MIZU00JPN restored at the held coordinate (H) reads a congruence statistic of 74.2 and moves the wire by 84 mm, at its SINEX coordinate (S3) 0.707, clock rows within 0.0005 m of S2c and WTZZ00DEU within 0.0025 m. B1 and B4 (b) met; B3 and B4 (a) not met. The roster-file route does what a loader key would: phase 2 not built, nothing armed.
- **source:** [SNX](SNX_sinex_station_coordinates_2026-10-19.md) sections 4, 8.2, 8.3 and 8.4 (the offsets, the plan, the grading truth, the arms and bars); the author's request of 2026-10-19 to build and run phase 1
- **knobs / posture:** none in the engine. Tooling: `scripts/roster_from_sinex.py`, `rapid_arm.py make --roster`, `gcensus.py --sites`, `scripts/clock_level/snx_roster_grade.py`, `scripts/clock_level/site_list_sinex.json` (commit `69e3dfd` on branch `card/snxroster`). Added 2026-10-31 (branch `close/snx`): `rapid_arm.py make --add-station` (commit `18069d1`) and `snx_roster_grade.py consumer --ref`, `congruence`, `r3rows` (commit `56a6cea`). Posture: the field golden `config/golden/golden_field_refound.json`; the arms differ from it in `network.product_output_path` and, for S1 and S2, in the `position_ecef` of the replaced stations
- **dates in the text:** 2026-10-19, 2026-10-31
- **hashes / run dirs:** exe `D:/GnssProcessorSupport/gates/exe/x64-Release/gnss_processor_snxroster.exe` (sha256 `3d6c679e8068a39879c582b4495e069c569a3706049826f96f6123e2267a288e`); instruments and roster files `D:/GnssProcessorSupport/gates/snxroster/` (`grades.txt` holds every table of section 10 as printed); arms and census replays `D:/GnssProcessorSupport/runs_nom/_rap_snxr*`; roster-file and site-list hashes in section 10.2. 2026-10-31 (sections 11 and 12): exe `D:/GnssProcessorSupport/gates/exe/x64-Release/gnss_processor_snxclose.exe` (sha256 `91143c81de9a9547...`, a copy of `gnss_processor_bofarm.exe`); rosters `D:/GnssProcessorSupport/gates/snxclose/ros/` (S2 `0d8fbc025d0bbfae...`, S3 `25df2a43166cdf08...`); arms `_rap_snxc_{C,C2c,S2c,H,S3,dev_S3,smoke_S3}`, census replays `_rap_snxc{C,S2c,H,S3}[x]_<site>` and MIZU00JPN replays `_rap_snxcmz_{S2c,S3,H}` under `D:/GnssProcessorSupport/runs_nom/`; instruments and outputs `D:/GnssProcessorSupport/gates/snxclose/`.
- **gates (frozen at pre-registration):** [SNX](SNX_sinex_station_coordinates_2026-10-19.md) section 8.4 (arms and bars), read in section 10.7; section 11.2 (arm S3 on the current golden, 2026-10-31), read in section 12.8.

**Outcome.** Question: does replacing the roster's held coordinates by the weekly SINEX values change the emitted wire or the consumers, and does MIZU00JPN restored at its SINEX coordinate repair what its held coordinate broke? Result: the satellite clocks change by 5 to 7 mm RMS, a per-satellite level, and the consumers follow by 2.4 mm at most on the 2026-10-19 wire (section 10.4); on the current golden 7.4 mm at ARHT00ATA, 1.6 mm at the next site. MIZU00JPN at the held coordinate reads a congruence statistic of 74.2 and moves the wire by 84 mm; at its SINEX coordinate 0.707, with a leave-out replay within 5 mm of it. B3 and B4 (a) are missed. Standing: the roster-file route suffices; phase 2 is not built; nothing is armed.

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

## 11. 2026-10-31: arm S3 on the current golden (pre-registered)

Section 10.7 left B4 (arm S3, MIZU00JPN restored) not run; card SNX section 8.4 froze it. This section fixes the arms, the readings and the verdict rule before any run of it is read. The engine is unchanged and nothing is armed.

### 11.1 What is run

- Exe `D:/GnssProcessorSupport/gates/exe/x64-Release/gnss_processor_snxclose.exe`, a copy of `C:/GnssProcessor/build/x64-Release/gnss_processor_bofarm.exe` (sha256 `91143c81de9a95479aa08131c08945676d5e542dbd607e9a8ff6d1852d2d73d6` for both), the exe of the baseline pair `runs/_rf0hv17_{field,sim}6`. The eight DLLs beside it equal those of the main build (sha256 compared).
- Base `config/golden/golden_field_refound.json` at main `e639032` (last changed by `2b9af4a`, card BOF): GPS + Galileo, 58 stations, WTZZ00DEU the held-out consumer, DOY 129 from 00:00, 6 h, cold start.
- S2 roster regenerated for this golden by `scripts/roster_from_sinex.py` from the weekly SINEX of section 10 (sha256 `74a412dc1d09db48...`): 54 positions, byte-identical to the S2 roster file of section 10.2 (sha256 `0d8fbc025d0bbfae...`) and equal to the coordinates of the S2 arm of 2026-10-19 at all 54 stations. The same generator, run on the roster with MIZU00JPN added at its held coordinate, gives MIZU00JPN as solution 16, formal sigma 0.7 mm, data span 7.0 d, at (-3857171.2296, 3108692.7812, 4004040.1136) m, which is 1679.4 mm from the held (-3857169.9416, 3108693.7043, 4004040.6697) m (E +1527.0, N -698.7, U -22.4 mm). That 55-position file (the 54 and MIZU00JPN) is the roster of S3 (sha256 `25df2a43166cdf08...`).
- Tooling, committed before the arms: `rapid_arm.py make --add-station NAME RINEX X Y Z` (selftest 105 of 105, commit `18069d1`); `snx_roster_grade.py consumer --ref`, `congruence` and `r3rows` (commit `56a6cea`; they reproduce the tables of sections 10.3 and 10.6, and the run-end congruence table of `runs/_rf0hv17_field6` at 58 of 58 stations).
- MIZU00JPN observations `C:/GnssProcessor/data/2024-129/obs/MIZU00JPN/MIZU00JPN_R_20241290000_01D_30S_MO.rnx` (Septentrio PolaRx5, SEPCHOKE_B3E6 NONE, antenna height 0.1206 m; GPS L1, L2W, L5Q and Galileo E1, E5a present). The station was in the roster until card SCU dropped it.

| tag | run directory | change from the golden |
| --- | --- | --- |
| C | `C:/GnssProcessor/runs/_rf0hv17_field6` | none (the existing run, same exe) |
| C2c | `_rap_snxc_C2c` | none; a fresh run of the renamed copy beside the others |
| S2c | `_rap_snxc_S2c` | the S2 roster: 54 stations at their SINEX coordinates |
| H | `_rap_snxc_H` | S2c plus MIZU00JPN at the held SSC coordinate (59 stations) |
| S3 | `_rap_snxc_S3` | S2c plus MIZU00JPN at its SINEX coordinate (59 stations) |

S3 differs from H in one coordinate and from S2c in one station. Order: a 10-epoch smoke of S3; a 2 h arm of S3 (development: it exits 0 and the network uses MIZU00JPN; not graded); then the 6 h arms C2c, S2c, H and S3 together under `RAPID_RUNS_ROOT=D:/GnssProcessorSupport/runs_nom`; then the census replays of the four wires with `gcensus.py` (GPS + Galileo; WTZZ00DEU and the 16 sites; keys on the held truth and, with `--sites site_list_sinex.json`, on the SINEX truth). Instruments and outputs: `D:/GnssProcessorSupport/gates/snxclose/`.

### 11.2 Bars and readings (frozen)

- **G0.** C2c reproduces C: the records of `products.txt` (comment lines and the `S` line excluded), `closed_loop_trace.csv` and `network_epoch_status.csv` are equal. If not, C2c is the control of every delta below (same box state) and the difference between C and C2c is reported.
- **B1** (consumer, the form of section 10.7). WTZZ00DEU, hourly 3D p50 in hours 2 to 6: S3, S2c and H each within 0.003 m of C at every hour. Read on the SINEX truth and on the held truth.
- **B3** (census, the form of section 10.7). Consumer replays at WTZZ00DEU and the 16 sites, paired per site against C on one truth: the median over the 16 sites of the change in the mean hourly p50 over hours 2 to 6 not positive (a positive value at five decimals is a miss; section 10.7 records three such misses of 0.03 to 0.05 mm, below the resolution of the trace), and no site worse than 0.010 m. Read on both truths.
- **B4** (S3, as frozen in SNX section 8.4). (a) Clock wire at most 0.0046 / 0.0029 m: the reading of `scripts/ftc_step_attrib.py --r3 --systems G,E` for system G, hours 2 to 6, `e_clk` p50 of the within-arc RMS against the CODE clock, rows "ALL sats, DIFFMEDIAN" and "consumer-TRACKED, DIFFMEDIAN" (`snx_roster_grade.py r3rows`). Caveat: the figure is card SCU's (section 3.1, arm m2: 0.0044 / 0.0028 m), measured on the GPS-only base of 2026-09-22, before the level-aware hold, the exact update and the Galileo package, and that card does not name its rows. The control of section 10.6 reads 0.0230 / 0.0195 m on these rows (0.0814 / 0.0658 m on the raw rows), so the clause is expected to be missed by every arm, C included; it is applied literally and a miss is recorded as a miss. (b) Congruence statistic: geo(t) at MIZU00JPN below 4 in S3 (the run-end station congruence table of `stdout.txt`, recomputed from `logs/network_congruence.csv` by `snx_roster_grade.py congruence`; card SCU: 29.1 at the held coordinate, 1.43 at its leave-out estimate).
- **Read beside the bars** (no bar). S3 against H (the coordinate alone) and S3 against S2c (adding the station), and each against C: all eight R3 rows (G and E; all, tracked, each also de-commoned); the per-epoch RMS clock difference between wires (`snx_roster_grade.py clock`); geo(t) at MIZU00JPN and the roster median; the run-end counters of `rapid_arm.py score`; B1 and B3 in their paired form for the pairs S3-H and S3-S2c.
- **Verdict rule.** S3 repairs the station when (i) geo(t) at MIZU00JPN in S3 is below 4 and (ii) the de-commoned R3 rows of S3 lie within 0.0010 m (system G) and 0.0016 m (system E) of those of S2c, the largest changes that replacing the whole roster by SINEX coordinates made on those rows in section 10.6; this tolerance is set here and is not a bar of SNX section 8.4. The test discriminates only if H shows the defect: geo(t) at MIZU00JPN in H not below 4, or a de-commoned G row of H more than 0.0010 m from S2c; otherwise the clock wire is recorded as unable to discriminate in this posture and the verdict rests on (i). A failure of (ii) in which S3 and H move from S2c alike reads as an effect of the station's data and not of its coordinate. If S3 repairs the station, the roster-file route already does what a loader key would and phase 2 (`network.coordinate_source`) is not justified by it. If not, the card states which of (i) and (ii) fails and what remains of the coordinate error, read from MIZU00JPN replayed as a consumer on the S3 and H wires (diagnostic, no bar).

### 11.3 Expected, stated in advance

H reproduces the defect of card SCU: a large geo(t) at MIZU00JPN and a clock wire off S2c. The SINEX coordinate lies (0.06, 0.06, 0.10) m from card SCU's leave-out estimate (+1.4715, -0.6366, -0.1204 m, SNX section 7); the screen's horizontal MDB is 0.02 to 0.03 m and its Up MDB 0.19 to 0.24 m (the run-end table of `runs/_rf0hv17_field6`), so S3 can read above the 1.43 of the leave-out arm and may or may not clear 4. The consumer and the census are expected to follow section 10.4 (a null). Not covered: other days or networks, the sim bed (known positions), the four roster stations absent from the file.

## 12. 2026-10-31: arm S3 on the current golden, results

The arms, bars and verdict rule are those of section 11 (commit `7fd2fec`), read after the runs; nothing was changed. A 10-epoch smoke of S3 (exit 0, 11 epochs) and a 2 h arm of S3 (`_rap_snxc_dev_S3`, exit 0, 675.6 s, 240 epochs emitted; geo(t) at MIZU00JPN 0.896, roster median 0.787; development reading, not graded) came first. The four 6 h arms then ran together under `RAPID_RUNS_ROOT=D:/GnssProcessorSupport/runs_nom` with the exe of section 11.1: exit 0 for C2c, S2c, H and S3 after 1528.4, 1515.5, 1557.1 and 1556.4 s, 720 of 720 epochs emitted and none skipped in each. Census replays (`_rap_snxc{C,S2c,H,S3}[x]_<site>`, 17 each, all exit 0; the `x` keys on the SINEX truth) and the MIZU00JPN replays (`_rap_snxcmz_{S2c,S3,H}`) followed. Every table below is printed in `D:/GnssProcessorSupport/gates/snxclose/out/` (`g0.txt`, `congruence.txt`, `r3rows_*.txt`, `clock.txt`, `consumer.txt`, `census.txt`, `census_sites.txt`, `score.txt`); the R3 outputs are `ftc_<arm>.txt` one level up.

### 12.1 G0 and what differs between the arms

- **G0 met.** C2c equals C: `products.txt` is byte-identical (sha256 `bf16e9474305e2de...`, 343,048 records), as are `logs/closed_loop_trace.csv` (`eb2eca92f04e9906...`) and `logs/network_epoch_status.csv` (`cc63d5f1d10d62be...`). C2c is therefore C, and the census replays of C stand for both (C2c's were not run). `C` is the run `runs/_rf0hv17_field6` copied to `_rap_snxc_C` (its `products.txt` and trace hashes equal).
- Configs (key by key, `arm_diff.py`): C2c against the control's `arm.json` differs in `network.product_output_path` and one comment line (the golden's `_comment[4]` was extended after the control was made); S2c against C2c in 54 station coordinates (largest 63.9 mm); H against S2c in one added station (MIZU00JPN at the held coordinate); S3 against H in the coordinate of MIZU00JPN only, by (-1.2880, -0.9231, -0.5561) m.
- Run-end counters (`rapid_arm.py score`; epochs under 0.10 m, T4 attributions, IF slips, band-5 IF slips, band-scope events, clock anchors, S-transforms, MW detections): C and C2c 688, 55, 111, 41, 3131, 229, 274, 405; S2c 687, 55, 111, 41, 3131, 229, 274, 405; H 688, 74, 111, 41, 3131, 229, 270, 405; S3 688, 60, 111, 41, 3131, 229, 270, 405. The added T4 attributions are 14 at MIZU00JPN (H, none in S3) and 5 at HKSL00HKG (H and S3); the S-transform count falls by 4 in both as 12 of them move to MIZU00JPN (HKSL00HKG -8, GUAM00GUM -4, CHUR00CAN -2, JPLM00USA -2).

### 12.2 B4 (b): the congruence statistic at MIZU00JPN

geo(t) of the run-end station congruence table (`stdout.txt`; `snx_roster_grade.py congruence` recomputes it from `logs/network_congruence.csv` and agrees at every station printed):

| wire | MIZU00JPN geo(t) | station-epochs above t 4 (of 72) | roster median | stations above 4 |
| --- | --- | --- | --- | --- |
| C | absent | | 0.933 | MAJU00MHL 6.29 |
| S2c | absent | | 0.948 | MAJU00MHL 6.40 |
| H | 74.155 | 67 | 0.885 | MIZU00JPN 74.15, MAJU00MHL 7.39, GCGO00USA 6.68, DARW00AUS 5.18, GUAM00GUM 4.28 |
| S3 | 0.707 | 1 | 0.952 | MAJU00MHL 6.52 |

B4 (b) is met: 0.707 is below 4 and below the 1.43 that card SCU read at its leave-out estimate. H reproduces the defect (card SCU: 29.1) and also lifts GCGO00USA, DARW00AUS and GUAM00GUM above 4; S3 does not.

### 12.3 B4 (a): the clock wire

R3 (`ftc_step_attrib.py --r3 --systems G,E`), hours 2 to 6, `e_clk` p50 of the within-arc RMS against the CODE clock (m); "dm" is the de-commoned (difference-median) row:

| system, set | C | S2c | H | S3 | S3 - S2c | H - S2c |
| --- | --- | --- | --- | --- | --- | --- |
| G, all | 0.0811 | 0.0818 | 0.1102 | 0.0859 | +0.0041 | +0.0284 |
| G, tracked | 0.0659 | 0.0657 | 0.0892 | 0.0664 | +0.0007 | +0.0235 |
| G, all, dm | 0.0234 | 0.0224 | 0.0341 | 0.0219 | -0.0005 | +0.0117 |
| G, tracked, dm | 0.0189 | 0.0180 | 0.0221 | 0.0180 | +0.0000 | +0.0041 |
| E, all | 0.0226 | 0.0232 | 0.0442 | 0.0218 | -0.0014 | +0.0210 |
| E, tracked | 0.0144 | 0.0146 | 0.0111 | 0.0145 | -0.0001 | -0.0035 |
| E, all, dm | 0.0211 | 0.0242 | 0.0385 | 0.0237 | -0.0005 | +0.0143 |
| E, tracked, dm | 0.0125 | 0.0128 | 0.0113 | 0.0126 | -0.0002 | -0.0015 |

- **B4 (a) as frozen is not met.** S3 reads 0.0219 / 0.0180 m on the G dm rows against 0.0046 / 0.0029 m. As section 11.2 expected, the control reads 0.0234 / 0.0189 m, so the clause was out of reach for every arm; the figure is card SCU's, from another posture.
- **Verdict rule (ii) is met.** The dm rows of S3 lie within 0.0005 (G, all), 0.0000 (G, tracked), 0.0005 (E, all) and 0.0002 m (E, tracked) of S2c, inside the 0.0010 (G) and 0.0016 m (E) tolerances. H shows the defect: G dm all +0.0117 and tracked +0.0041 m from S2c, E dm all +0.0143 m, and geo(t) 74.2 at MIZU00JPN; the test discriminates.

### 12.4 The wire between arms

Hours 2 to 6 mean of the per-epoch RMS over satellites of the clock difference (m), after removing the per-epoch common mean per constellation (`snx_roster_grade.py clock`); "pooled" removes one mean over GPS and Galileo; the last two columns are over all sat-epochs of the per-constellation set (mm):

| pair | GPS | Galileo | pooled | RMS | constant per-satellite part |
| --- | --- | --- | --- | --- | --- |
| S2c - C | 0.00686 | 0.00509 | 0.00864 | 6.34 | 5.08 |
| S3 - C | 0.01611 | 0.00862 | 0.04895 | 13.65 | 11.33 |
| H - C | 0.08644 | 0.07393 | 0.12034 | 82.16 | 68.70 |
| S3 - S2c | 0.01564 | 0.00776 | 0.04434 | 13.18 | 11.81 |
| H - S2c | 0.08917 | 0.07422 | 0.11817 | 83.96 | 70.86 |
| S3 - H | 0.07559 | 0.07137 | 0.08503 | 74.92 | 61.88 |

- S2c against C repeats the roster effect of section 10.6 on the current golden (GPS 0.00686 against 0.00685, Galileo 0.00509 against 0.00508, constant part 5.08 mm in both).
- The held coordinate of MIZU00JPN moves the wire by 84 mm RMS, mainly a per-satellite level (G08 +213.8, G27 +202.6, G16 +150.4, E14 +135.4, G14 -119.9 mm against S2c). At the SINEX coordinate 15 to 23 % of that level remains on the same satellites with the same sign (S3 against S2c: G27 +46.9, G08 +31.9, G16 +22.7 mm; GPS RMS by hour 0.0226 0.0209 0.0194 0.0149 0.0125 0.0106 m), 13.18 mm in all, twice the effect of the 54-station roster.
- Whether that remainder is the station's data or a coordinate residual is not separated by an arm. The residual the data allow is below 5 mm (section 12.7), and H against S2c gives 50 mm of wire per metre of coordinate error (83.96 mm over 1.679 m), so a 5 mm residual accounts for 0.25 mm of the 13.18 mm. This is a scaling argument, not a measurement; the within-arc rows of 12.3 cannot see a constant per-satellite level.

### 12.5 B1: the held-out consumer WTZZ00DEU

p50 of the 3D error by hour (m), SINEX truth; the change from C in hours 2 to 6 follows (`snx_roster_grade.py consumer --ref snxc_C`):

| wire | h1 | h2 | h3 | h4 | h5 | h6 | mean h2 to h6 | change from C, h2 to h6 | largest, SINEX / held truth |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| C | 0.0605 | 0.0142 | 0.0253 | 0.0135 | 0.0168 | 0.0104 | 0.0160 | | |
| S2c | 0.0606 | 0.0142 | 0.0263 | 0.0138 | 0.0170 | 0.0103 | 0.0163 | -0.0000 +0.0010 +0.0004 +0.0001 -0.0002 | 0.0010 / 0.0010 |
| H | 0.0596 | 0.0139 | 0.0228 | 0.0132 | 0.0167 | 0.0106 | 0.0154 | -0.0004 -0.0025 -0.0003 -0.0001 +0.0002 | 0.0025 / 0.0024 |
| S3 | 0.0600 | 0.0136 | 0.0235 | 0.0126 | 0.0165 | 0.0104 | 0.0153 | -0.0006 -0.0018 -0.0008 -0.0003 +0.0000 | 0.0018 / 0.0019 |

B1 is met by S3, S2c and H on both truths (largest 0.0025 m against 0.003 m). S3 against H: largest 0.0007 (SINEX) and 0.0005 m (held); S3 against S2c: 0.0028 and 0.0029 m (hour 3). The truth switch on C moves the mean over hours 2 to 6 from 0.0152 to 0.0160 m.

### 12.6 B3: the 16-site census

Consumer replays at the 16 sites on each wire, paired per site against C on the same truth (mean hourly p50 over hours 2 to 6; `snx_roster_grade.py census`, `census_sites.py`):

| arm | truth | median change (m) | mean (m) | largest increase | sites above +0.010 m | median clause | site clause |
| --- | --- | --- | --- | --- | --- | --- | --- |
| S2c | held | +0.00003 | +0.00057 | ARHT00ATA +0.0074 | 0 | missed | met |
| S2c | SINEX | +0.00003 | +0.00061 | ARHT00ATA +0.0075 | 0 | missed | met |
| H | held | +0.00020 | +0.00093 | DAV100ATA +0.0251 | 1 (DAV100ATA) | missed | missed |
| H | SINEX | +0.00054 | +0.00113 | DAV100ATA +0.0253 | 1 (DAV100ATA) | missed | missed |
| S3 | held | +0.00025 | +0.00143 | ARHT00ATA +0.0112 | 1 (ARHT00ATA) | missed | missed |
| S3 | SINEX | +0.00037 | +0.00146 | ARHT00ATA +0.0112 | 1 (ARHT00ATA) | missed | missed |

- **B3 is not met by S3** on either truth: the median clause by 0.25 mm (held) and 0.37 mm (SINEX), the site clause by ARHT00ATA at +11.2 mm (1.2 mm over the bar). S2c misses the median clause by 0.03 mm, as the arms of section 10.7 did, and meets the site clause with ARHT00ATA at +7.4 mm (+2.4 mm in section 10.4: the site moves more on this golden).
- The two Antarctic sites swing with the wire and not with the correctness of the coordinate: H moves ARHT00ATA by -22.3 mm (better) and DAV100ATA by +25.1 mm (worse), S3 by +11.2 and +2.3 mm, S2c by +7.4 and +1.6 mm, at errors of 0.23 and 0.13 m. Card NSP bounds a neutral change at 0.19 mm for any site, so these are responses of the wire, not noise. Without those two sites the changes are at most +6.7 mm (HARB00ZAF in H) and +4.8 mm (HARB00ZAF in S3).
- S3 against S2c (adding the station): median +0.00030 m (held), +0.00044 m (SINEX), largest HARB00ZAF +0.0043 and +0.0044 m, ARHT00ATA +0.0038 m, no site above +0.010 m. S3 against H: median +0.00001 and -0.00009 m, ARHT00ATA +0.0335 m, DAV100ATA -0.0228 m.
- Hour 1 (ignition, outside the bars): the median site p50 is 0.142 m for C, 0.143 for S2c, 0.168 for S3 and 0.170 for H; QUIN00USA rises by 0.190 m (S3) and 0.248 m (H), KOKB00USA by 0.076 and 0.075 m, HARB00ZAF by 0.036 and 0.042 m. Both arms with the station show it, with the right coordinate and the wrong one.
- Truth switch on C (SINEX against held, replays do not read their truth: `truth-check` agrees to 1e-4 m for C, S2c, H and S3): median -0.00129 m, mean -0.00126 m, 10 sites better and 6 worse, CEDU00AUS -0.0103, ASCG00SHN -0.0060, UTQI00USA -0.0054, SCRZ00BOL +0.0048, STPM00SPM +0.0031 m; WTZZ00DEU replay +0.0008 m. Section 10.4, on the earlier wire: -0.00135 m, 12 better and 4 worse, CEDU00AUS -0.0115 m.

### 12.7 MIZU00JPN as a consumer (diagnostic)

MIZU00JPN replayed as the closed-loop consumer (its own RINEX, the SINEX coordinate as truth) on the wire of S2c, which does not contain the station, and on those of S3 and H. Mean error over hours 4 to 6 (m), then the 3D p50 by hour:

| wire | E | N | U | 3D p50 by hour |
| --- | --- | --- | --- | --- |
| S2c (leave-out) | +0.0004 | +0.0003 | -0.0046 | 0.0365 0.0243 0.0089 0.0092 0.0115 0.0171 |
| S3 | +0.0003 | +0.0004 | -0.0048 | 0.0352 0.0235 0.0089 0.0092 0.0114 0.0172 |
| H | -0.0004 | +0.0006 | -0.0044 | 0.0370 0.0241 0.0089 0.0088 0.0112 0.0169 |

On a wire formed without the station the data put the antenna within 0.5 mm of the SINEX coordinate in East and North and within 4.6 mm in Up. The SINEX coordinate therefore lacks nothing the data show at that resolution. The SINEX coordinate lay (+55.5, -62.1, +98.0) mm from card SCU's leave-out estimate on its wire (card SNX section 7); the wire has improved since. The replay on the H wire lands (E -0.0004, N +0.0006, U -0.0044) m from the SINEX coordinate, that is 1.68 m from the held coordinate that wire was formed with (the held-to-SINEX distance is 1.679 m, 12.1 and 12.4), so a leave-in replay does expose the error; card SCU section 2 found it so at this station (E +1.363, N -0.588, U -0.121 m from the held coordinate, on its wire) and notes that a leave-in replay under-reports a coordinate error by the station's own share of the wire (7 % there). What the replay cannot show is that the H wire was formed with a wrong coordinate: its 3D p50 by hour is 0.0370 0.0241 0.0089 0.0088 0.0112 0.0169 m on H against 0.0365 0.0243 0.0089 0.0092 0.0115 0.0171 m on S2c, while the clocks of the two wires differ by 84 mm RMS (12.4); the congruence statistic (74.2, 12.2) and the clock wire show that.

### 12.8 Against the bars; verdict

| bar | reading |
| --- | --- |
| G0 | met (12.1) |
| B1, S3 | met: 0.0018 m on the SINEX truth, 0.0019 m on the held (S2c 0.0010, H 0.0025 and 0.0024) |
| B3, S3 | not met on both truths: median +0.00025 / +0.00037 m, ARHT00ATA +0.0112 m (12.6) |
| B4 (a), S3 | not met as written: 0.0219 / 0.0180 m against 0.0046 / 0.0029 m; the control reads 0.0234 / 0.0189 m (12.3) |
| B4 (b), S3 | met: geo(t) 0.707 (H 74.2) |
| verdict rule (i), (ii), H discriminates | met, met, yes |

- **Verdict: S3 repairs the station.** The congruence statistic returns from 74.2 to 0.707, the de-commoned clock rows return to S2c's within 0.0005 m, the held-out consumer follows within 0.0025 m in every arm, and the leave-out replay lands within 5 mm of the SINEX coordinate. The roster-file route (a roster JSON from `roster_from_sinex.py` spliced into the arm) already does for MIZU00JPN what a loader key would; the coordinate in the engine is the same number. Phase 2 (`network.coordinate_source`) is not justified by this evidence and is not built.
- **Stands from section 10.7:** B2 is not met as written (S2c repeats it on these rows: G all +0.0007, E all +0.0006, E tracked +0.0002 m against 0.0002 / 0.0001 m), and the median clause of B3 is missed by S2c, S3 and H, by 0.03 to 0.54 mm.
- **Not met, with the station on the roster:** B3 by S3 (ARHT00ATA +11.2 mm, median +0.25 mm), and the ignition hour (12.6). The station is not restored to the golden roster by this card; that is the author's decision.
- **Not covered:** the 13 mm RMS between S3 and S2c is not separated (12.4); one day, one network, the GPS + Galileo posture; the four roster stations absent from the SINEX (card SNX, note of 2026-10-31).

### 12.9 Reproduction

```
python -I scripts/roster_from_sinex.py --sinex <weekly.SNX.gz> --out ros/roster_s2.json                       # 54 positions
python -I scripts/roster_from_sinex.py --sinex <weekly.SNX.gz> --config <59-station arm.json> --out ros/roster_s3.json   # 55 positions
python -I scripts/rapid_arm.py make --base config/golden/golden_field_refound.json --tag snxc_S3 --hours 6 --roster ros/roster_s3.json --add-station MIZU00JPN <rinex> -3857169.9416 3108693.7043 4004040.6697   # H: roster_s2.json
python -I scripts/rapid_arm.py run --tags snxc_C2c snxc_S2c snxc_H snxc_S3 --jobs 4 --exe <renamed exe>        # RAPID_RUNS_ROOT=D:/GnssProcessorSupport/runs_nom
python scripts/clock_level/gcensus.py [--sites scripts/clock_level/site_list_sinex.json] make snxcS3[x] _rap_snxc_S3 _rap_snxc_S3 <exe>
python -I scripts/clock_level/snx_roster_grade.py consumer --ref snxc_C ... | census | clock | congruence | r3rows
```

`ftc_step_attrib.py` runs under `C:/Program Files/Python314/python.exe`; `gcensus.py` without `-I`. Instruments and outputs: `D:/GnssProcessorSupport/gates/snxclose/` (`tools/`, `ros/`, `out/`, `grade_all.sh`, `census_launch.sh`).

### 12.10 Reopen criterion for phase 2; the reference of gate G3 of card SCU

- Phase 2 reopens with a warm resume or a real-time deployment whose roster coordinates differ between legs (the snapshot fingerprint hashes station names and not coordinates, card SNX section 8.2, so such a resume would not cold-start), or with a rule that every run declares its coordinate source in the run-start table. Every arm here is a cold start, and neither case exists today.
- Gate G3 of card SCU (a correction within 0.05 m of (+1.47, -0.64, -0.12) m) has a data-based reference now: the leave-out replay of 12.7 puts the data (+1.5274, -0.6984, -0.0270) m from the held coordinate, against the SINEX difference (+1.5270, -0.6987, -0.0224) m. Restating the reference is the author's decision (card SNX section 8.5).
