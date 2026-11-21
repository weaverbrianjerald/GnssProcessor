# CSC part 2: real-file replay tests and the stream fixes (package stream-real-file-replay-fixes, 2026-11-19)

Continuation of [card CSC](CSC_correction_stream_contract_2026-11-18.md). Filed on 2026-11-21, after the fact, from the package `stream-real-file-replay-fixes` of the day program of 2026-11-19 (`D:/GnssProcessorSupport/gates/day_program_2026-11-19/packages/stream-real-file-replay-fixes/`: `CHECKPOINT_1.md`, `CHECKPOINT_2.md`, `CHECKPOINT_3.md`, `NIGHT_SET_doy146_users.md`); branch `stream/fix-2026-11-19`, merged into `int/2026-11-19` on 2026-11-20 (7826c1a) and in main. The text below is the package text, unchanged apart from heading levels.

## CHECKPOINT_1.md

### Checkpoint 1: real-file replay tests and the class table (before any fix)

Package `stream-real-file-replay-fixes`, 2026-11-19. Base build only: no fix is in the binaries used here.

#### Result in three lines

1. On all nine real product files the reader and writer round-trip every data line, and the tail reader gives the batch store at every poll.
2. The counter disagreements of a user replay (959 to 1 250 epochs of 2 880 per day, equal to the users' warning counts) are 97 % the network's unsettled steps, 2 to 3 % the step-class declaration, and a few the family member copy and the chain's re-host declarations; none is a reader defect.
3. Store hash lists of every file are written as the identity reference for the fixes.

#### What was run

- Test file: `tests/network/test_stream_real_file_replay.cpp`, suite StreamRealFileReplay (new; gated by `GNSS_STREAM_FILES`, evidence to `GNSS_STREAM_OUT`).
- Binary: Release `gnss_tests.exe` of the base worktree `D:/GnssProcessorSupport/worktrees/stream_base` (detached at `cf983dc` = `int/2026-11-19`, plus the new test file and its CMake line only), sha256 `a3603eccc868783080daac8253d9d3c85523e525eb1c99e0c13f83dad3d60f9e`.
- Base engine frozen for the identity replays: `D:/GnssProcessorSupport/gates/exe/x64-Release/gnss_processor_streamfix_base.exe`, sha256 `a00648c7077041a3fd042f16068d21af69b97804671245e039f4de9ba6270df5`.
- Files (`results/stream_replay/files.txt`): `D:/GnssProcessorSupport/runs_nom/` `_rap_ad_n143` to `_rap_ad_n147`, `_rap_adr_n146`, `_rap_n46c_n146` (the five-day read, counters in the file), and `_rap_int19_gf`, `_rap_int19_gs` (2 h, `pbhold=2` in the S line, no counters in the file).
- Script: `results/stream_replay/run_properties.ps1`; output `results/stream_replay/base/` (`gtest_output.txt`, `<file>_p1_disagreements.csv`, `<file>_p1_summary.txt`, `<file>_p4_store_hashes.txt`, `class_table.md`). Class table script: `results/stream_replay/aggregate_p1.py`. Run time 26 min (P3 18 min).

All folders above are under `D:/GnssProcessorSupport/gates/day_program_2026-11-19/`.

#### The four properties

| property | result |
| --- | --- |
| P1 counter disagreements by class | table below; 7 files with counters, 2 without (nothing to compare) |
| P2 parsed epochs re-written by the writer | 0 epochs differ on all nine files (E, C, B, P, R, U, G, I lines) |
| P3 tail reader against batch replay, chunks of 1 to 1 048 573 bytes cutting lines and epochs | 0 polls with a different store on all nine files; 186 polls per day file; every epoch but the last delivered (the tail flushes an epoch when the next E line arrives) |
| P4 store hash list per epoch | written for all nine files; digests below |

#### P1: how a disagreement is classed

The user's replay emitter is built from the S line alone (eight parameters). The network run also applied, at its setup, rules no S token carries. The test feeds every epoch to the user emitter and to emitters that add, one at a time and in this order, the settings read from the run's `arm.json`: the unsettled steps with their unheld form (`network.ring_fix.unsettled_steps`, `unsettled_steps_unheld`), the step-class declaration (`network.disc_step_class_pairs`), the phase-family member table (`network.phase_families`, `phase_family_codes`), the continuity guard (not armed in these runs). A value takes the class of the first setting whose addition makes it agree. After every epoch each emitter takes the file's counters, so each epoch is judged on its own steps.

Direction: file ahead = the file stepped and the replay did not; replay ahead = the replay stepped and the file did not.

#### P1: the class table

Cells: values, total (file ahead / replay ahead). All disagreements are on the phase channel; code and clock counters agree on every epoch of every file.

| file | epochs with a disagreement | values | unsettled | step class | member | other |
| --- | --- | --- | --- | --- | --- | --- |
| _rap_ad_n143 | 959 | 13 134 | 12 824 (12 824 / 0) | 295 (1 / 294) | 2 (0 / 2) | 13 (13 / 0) |
| _rap_ad_n144 | 1 153 | 13 547 | 13 471 (13 471 / 0) | 72 (0 / 72) | 2 (0 / 2) | 2 (2 / 0) |
| _rap_ad_n145 | 1 088 | 14 515 | 14 171 (14 171 / 0) | 173 (0 / 173) | 163 (0 / 163) | 8 (8 / 0) |
| _rap_ad_n146 | 1 181 | 16 183 | 15 891 (15 891 / 0) | 269 (0 / 269) | 12 (0 / 12) | 11 (11 / 0) |
| _rap_ad_n147 | 1 026 | 14 734 | 14 256 (14 256 / 0) | 474 (0 / 474) | 0 | 4 (4 / 0) |
| _rap_adr_n146 | 1 243 | 13 729 | 13 269 (13 269 / 0) | 444 (0 / 444) | 1 (0 / 1) | 15 (15 / 0) |
| _rap_n46c_n146 | 1 250 | 16 501 | 16 168 (16 168 / 0) | 324 (0 / 324) | 0 | 9 (9 / 0) |
| _rap_int19_gf | no counters | | | | | |
| _rap_int19_gs | no counters | | | | | |

Check against the users: the base engine writes one warning per epoch with a disagreement. Counted in the existing user runs: `_rap_nw_obsadx_146_utqi`, `_ascg`, `_arht` 1 181 each; `_143_ascg` 959, `_144_ascg` 1 153, `_145_ascg` 1 088, `_147_ascg` 1 026. Every count equals the P1 epoch count of the file the user replays, so P1 reproduces the users' own comparison exactly.

#### What each class is

- **Unsettled (about 97 %, all file ahead).** The network steps both signals of a held family at every emission while the two flags are not both up, once more when they settle, and every signal of no held family at every emission. The record's `signal_integer` is 0 on 12 592 to 15 956 of these values per file; the remaining 206 to 238 per day have the flag up: the settling step.
- **Step class (2 to 3 %, replay ahead but one value).** The user's value test (0.25 cycle bar) steps a key where the network's step-class declaration classified the move as quiet or ionosphere-like and declared nothing. A replay without the file's counters would reset these keys more often than the network asks.
- **Member (0 to 163 per day, replay ahead).** The network's member records (Galileo L1X, L5X) repeat their identity's counter; the replay's own value test steps the member.
- **Other (2 to 15 per day, all file ahead).** Steps of both signals of a pair with both flags up that no setting reproduces. In the six `_rap_ad*` / `_rap_adr` files, all of them (53 of 53) fall on an epoch where `logs/network_events.csv` records `CHAIN,declared_keys,<sat>,...,cause=rehost` for that satellite and signal. In `_rap_n46c_n146`, 2 of 9 matched exactly; the other 7 sit at epochs with fractional times (exact epoch time) and at the rounded epoch the log records `CHAIN declared_keys` or `CHAIN rehost` events for the same satellites (G14 and G28 checked). They are the chain's release declarations at a re-host, which travel in no S token and no setting (CSC A5: release marks are not serialized).
- **With every setting** the replay still differs on these "other" values and on a few member edges (1 to 7 values per file, counted under the user layer as member or step class).

Consequence: a file user is correct only because the adopted configuration writes the counters into the file (`network.serialize_disc_counters`), and the base reader then emits each of these epochs a second time. The disagreement is a contract property (CSC A5, ruling D7), not a reader fault. The base warning text calls it a divergence on a full-file replay; that is wrong for every class above.

#### P4 digests (base build)

| file | epochs | counters in file | digest of the hash list |
| --- | --- | --- | --- |
| _rap_ad_n143 | 2 880 | yes | 3884185c826cd79a |
| _rap_ad_n144 | 2 880 | yes | 6c5eb600c46f2eeb |
| _rap_ad_n145 | 2 880 | yes | e9e45128b602a4f8 |
| _rap_ad_n146 | 2 880 | yes | 3fb88c2345871d4f |
| _rap_ad_n147 | 2 880 | yes | 0c29e9432f44dd1d |
| _rap_adr_n146 | 2 880 | yes | 951f4d5e627647d5 |
| _rap_n46c_n146 | 2 880 | yes | a3ec86cac39395d7 |
| _rap_int19_gf | 240 | no | b797191965f2e2f9 |
| _rap_int19_gs | 240 | no | df3e2e64b769bad5 |

#### Test case status

P2, P3 and P4 pass. P1 failed on its first assertion, which required "other" to be zero; the chain's re-host declarations make that false by design. The assertion is rewritten for the fixed build: every "other" value must be a step of the file, never of the replay (true on all seven files), and the summary line reports what the emitter with every setting still gets wrong. No level step (`lvl=`) occurs in any of the nine files.

#### Next

The four fixes of the first package are written in `D:/GnssProcessorSupport/worktrees/stream_fix` (branch `stream/fix-2026-11-19`); the Debug suites of both packages ran (208 passed, 1 failure from a test pinning the old legend text, corrected). Next: the fixed Release build, P1 to P4 on the fixed build with the hash lists compared, six 2 h user replays (UTQI, ASCG, ARHT on `_rap_ad_n146`, base and fixed engine).


## CHECKPOINT_2.md

### Checkpoint 2: fixes of both packages committed, Debug suites green

Branch `stream/fix-2026-11-19` (worktree `D:/GnssProcessorSupport/worktrees/stream_fix`), on `cf983dc`. Nothing pushed, nothing merged. 2026-11-19.

#### Result in three lines

1. Both packages are committed: `0829abd` (stream replay, one emission with the file's counters, legend, unknown keys, header and comments) and `1443d82` (SBF Galileo group delays, least-squares clock shortcut, two real-time event defects).
2. Debug `gnss_tests` on the suites of both packages: 217 passed, 0 failed, 25 skipped (4 real-file cases gated by `GNSS_STREAM_FILES`, 21 cases skipped for other known defects).
3. The identity proof (P1 to P4 on the fixed build, hash lists, six 2 h user replays) is not done; the fixed Release binaries are built and copied, the three base replays are done.

#### What changed

- `0829abd` (10 files): `NetworkProductEmitter::emitToStore` takes the file's counters (`FiledDiscCounters`) and installs them before each push; each value the replay's own rules set differently is listed with its class (phase flag down, rise, up, replay step only, code, clock) and counted for the run (`filedDisagreementTotals`). `NetworkProductReader::replay` emits once (the adopt-and-re-emit is removed); the per-epoch warning becomes an info line. Legend `pbhold=<1|2|3>`; E field 8 and R ignore unknown `key=value`; header grammar lists G, U, R, N and every key; two wrong comments corrected. Four T22 skips enabled, all pass. New suites StreamFiledCounters (3 cases) and StreamRealFileReplay (4 gated cases). `tests/test_pb_emit_hold.cpp` pins the new legend text.
- `1443d82` (10 files): Galileo E1-E5a / E1-E5b group delays in their slots (SBF and subframe), suite SbfGalileoBgd (4 cases); least-squares clock exact-node shortcut removed (1 case); `reinflate_isb` recorded after its test (`reinflate_isb_skipped` otherwise); carry and translate paths go through `noteFreshnessFlip_` (4 cases).

#### Output moves named

- Store: identical to the old path on files without `lvl=` (none of the nine files has one); to be proved by the fixed P4 hash lists and the replays. With `lvl=` the old path counted the level step twice.
- Log: the per-epoch replay warning becomes an info line (the replay comparison script strips these lines).
- New files: the `#` legend line reads `pbhold=<1|2|3>`.
- Real time: `closed_loop_sat_trace.csv` may name `reinflate_isb_skipped`; flip counters, ARC rows and `ev_pb` rows grow on the carry and translate postures only. SBF and clock fixes: no current run moves.

#### Binaries

- Fixed Release engine `D:/GnssProcessorSupport/gates/exe/x64-Release/gnss_processor_streamfix_fix.exe`, sha256 `2383fe7c73d7ad4cb9331472bae5219793a641d55afba249f1e2e5eeefe2d768` (both packages).
- Fixed Release tests `.../gnss_tests_streamfix_fix.exe`, sha256 `69b91c7c5bca51657d94684a9a153b14781cdc60d94d9f64cf5802606ee6cac6`.
- Base engine (checkpoint 1) `.../gnss_processor_streamfix_base.exe`, sha256 `a00648c7...`.

#### Done since checkpoint 1

- Three base 2 h user replays on `_rap_ad_n146` (UTQI, ASCG, ARHT): exit 0, 40 to 44 s each, in `D:/GnssProcessorSupport/runs_nom/_rap_sfx_base_{utqi,ascg,arht}`.

#### Owed (held for release)

1. P1 to P4 on the fixed tests binary (`run_properties.ps1`, about 26 min, one slot); compare the nine P4 digests with checkpoint 1.
2. Three fixed 2 h user replays (`run_users.ps1 -Arm fix`, one slot set of 3), then `compare_user_runs.py` base against fix.
3. The night-set note: 21 users of DOY 146 on two engines (42 runs, 24 h), specified only.
4. Final report (checkpoint 3).
5. Listed, not in this file set: a run-end print of the disagreement classes needs a caller edit in `ApplicationConsumerReplay.cpp`; `RealtimeTailNeverChecksACodeBiasPinOnlyChange` needs `ApplicationRealtime.cpp` (another session); the ruling items D1, D2, D3, D7, D8, D10 of the stream rulings paper are not decided here.


## CHECKPOINT_3.md

### Checkpoint 3: identity of the stream replay fixes, package closed

Branch `stream/fix-2026-11-19`, tip `1443d82` (on `cf983dc`). Nothing pushed, nothing merged. 2026-11-19.

#### Result in three lines

1. The fixed build gives the same replay store as the base build on all nine product files: every per-epoch store hash list is identical byte for byte and every digest equals checkpoint 1.
2. The three 2 h users (UTQI, ASCG, ARHT on `_rap_ad_n146`) give identical output on the base and fixed engines; only the per-epoch replay line moves (warning to information line, 239 on each side), plus three items that differ between any two runs.
3. The 24 h check of all 21 users of DOY 146 is specified for tonight and handed to the run session.

#### Identity per file (P4 on the fixed tests binary, sha256 `69b91c7c...`)

| file | digest (base = fix) | hash list | P1 class counts and CSV |
| --- | --- | --- | --- |
| _rap_ad_n143 | 3884185c826cd79a | identical | identical |
| _rap_ad_n144 | 6c5eb600c46f2eeb | identical | identical |
| _rap_ad_n145 | e9e45128b602a4f8 | identical | identical |
| _rap_ad_n146 | 3fb88c2345871d4f | identical | identical |
| _rap_ad_n147 | 0c29e9432f44dd1d | identical | identical |
| _rap_adr_n146 | 951f4d5e627647d5 | identical | identical |
| _rap_n46c_n146 | a3ec86cac39395d7 | identical | identical |
| _rap_int19_gf | b797191965f2e2f9 | identical | identical (no counters) |
| _rap_int19_gs | df3e2e64b769bad5 | identical | identical (no counters) |

The four StreamRealFileReplay cases pass (P1 with its rewritten assertion: every "other" value is a step of the file). The P1 summary text differs only in its last field, renamed in the fixed copy of the test ("left with every setting": 2 to 16 values per day file, all file ahead). Output: `results/stream_replay/fix/`.

The renamed copy of the tests binary cannot start outside its build folder (missing DLLs, exit 0xC0000135); the run used the build-tree binary of the same sha256.

#### Identity per user (2 h, `compare_user_runs.py`, `results/stream_replay/user_compare_2h.txt`)

| user | base run | fix run | files | replay lines removed (base / fix) | differing |
| --- | --- | --- | --- | --- | --- |
| UTQI | `_rap_sfx_base_utqi` | `_rap_sfx_fix_utqi` | 33 | 239 / 239 | 0 |
| ASCG | `_rap_sfx_base_ascg` | `_rap_sfx_fix_ascg` | 33 | 239 / 239 | 0 |
| ARHT | `_rap_sfx_base_arht` | `_rap_sfx_fix_arht` | 33 | 239 / 239 | 0 |

Engines: base `gnss_processor_streamfix_base.exe` (`a00648c7...`), fix `gnss_processor_streamfix_fix.exe` (`2383fe7c...`). The comparison replaces, on both sides, the run folder name, the wall-clock age in the `obs backlog` line and the RINEX ingestion time; no other line differs. The real-time event changes of `1443d82` left no trace on these three runs (their traces are identical).

#### Night set

`NIGHT_SET_doy146_users.md`: 21 users x 2 engines, 24 h, configs written (`runs_nom/_rap_sfxn_{base,fix}_<user>`), bar: identical apart from the replay lines, equal replay-line counts, any difference named by class. Sent to the run session; rank set by the manager.

#### Open (not released, for the second merge pass or the author)

- Run-end print of the disagreement classes: caller edit in `ApplicationConsumerReplay.cpp` (accessor `filedDisagreementTotals()` is in place).
- `RealtimeTailNeverChecksACodeBiasPinOnlyChange`: needs `ApplicationRealtime.cpp`.
- `TranslateFlipNeedsItsOwnCountAndRecord` stays skipped.
- Stream rulings D1 to D10 wait for the author.
- The night set result.


## NIGHT_SET_doy146_users.md

### Night set: 21 users of DOY 146 on the base and fixed engines (identity of the stream replay fixes)

Package `stream-real-file-replay-fixes`, written 2026-11-19. Specified for a night run; not run by day.

#### Purpose

The fixed engine replays a product file once per epoch with the file's counters installed, where the
base engine emitted each epoch twice. On a file without declared level steps (`lvl=`) the two must
give identical user output; the per-epoch replay line changes from a warning to an information line
only. This set checks that on all 21 users of DOY 146 over 24 h. The 2 h check of three users is in
`CHECKPOINT_3.md`.

#### Inputs

- Product file replayed by every user: `D:/GnssProcessorSupport/runs_nom/_rap_ad_n146/products.txt`,
  sha256 `093540cc6d20b059b81be2cddc433f3a98c97bcf35c80892aaa7d047b3a64f6c` (no `lvl=` in it).
- User configs: copies of `D:/GnssProcessorSupport/runs_nom/_rap_nw_obsadx_146_<user>/arm.json`
  with only `network.product_output_path` moved into the new folder (the sources point it at the
  older runs `_rap_p26_c145_<user>`, which a night run must not overwrite). `max_duration_s` 86400.
  Already written by `make_night_set.py` (42 folders).
- Users (21): arht, ascg, badg, cedu, chti, cibg, gop6, hksl, jplm, mrc1, park, quin, reyk, seyg,
  solo, stpm, utqi, wroc, wtzr, wtzz, wuh2.

#### Engines (Release, renamed copies)

| arm | exe | sha256 | source |
| --- | --- | --- | --- |
| base | `D:/GnssProcessorSupport/gates/exe/x64-Release/gnss_processor_streamfix_base.exe` | `a00648c7077041a3fd042f16068d21af69b97804671245e039f4de9ba6270df5` | `cf983dc` (`int/2026-11-19`) |
| fix | `D:/GnssProcessorSupport/gates/exe/x64-Release/gnss_processor_streamfix_fix.exe` | `2383fe7c73d7ad4cb9331472bae5219793a641d55afba249f1e2e5eeefe2d768` | `1443d82` (`stream/fix-2026-11-19`, both packages) |

#### Run root

`D:/GnssProcessorSupport/runs_nom/_rap_sfxn_<arm>_<user>` (`RAPID_RUNS_ROOT` is not used: the
launcher names the folders itself). Scripts in
`D:/GnssProcessorSupport/gates/day_program_2026-11-19/results/stream_replay/`.

#### Commands

The configs are already written; regenerate only if the folders were removed:

```bash
python D:/GnssProcessorSupport/gates/day_program_2026-11-19/results/stream_replay/make_night_set.py
```

Base engine, 21 runs, three at a time (one slot set of 3):

```bash
python D:/GnssProcessorSupport/gates/run_queue/slots.py run --who stream-night --n 3 --shell "powershell -NoProfile -ExecutionPolicy Bypass -File D:\GnssProcessorSupport\gates\day_program_2026-11-19\results\stream_replay\run_night_set.ps1 -Exe D:/GnssProcessorSupport/gates/exe/x64-Release/gnss_processor_streamfix_base.exe -Arm base -Users arht,ascg,badg,cedu,chti,cibg,gop6,hksl,jplm,mrc1,park,quin,reyk,seyg,solo,stpm,utqi,wroc,wtzr,wtzz,wuh2 -Parallel 3"
```

Fixed engine, same users:

```bash
python D:/GnssProcessorSupport/gates/run_queue/slots.py run --who stream-night --n 3 --shell "powershell -NoProfile -ExecutionPolicy Bypass -File D:\GnssProcessorSupport\gates\day_program_2026-11-19\results\stream_replay\run_night_set.ps1 -Exe D:/GnssProcessorSupport/gates/exe/x64-Release/gnss_processor_streamfix_fix.exe -Arm fix -Users arht,ascg,badg,cedu,chti,cibg,gop6,hksl,jplm,mrc1,park,quin,reyk,seyg,solo,stpm,utqi,wroc,wtzr,wtzz,wuh2 -Parallel 3"
```

Each prints one line per user: `<user> <exit code> <seconds>`. Every exit code must be 0.

Expected time: the existing 24 h runs of these users took 227 to 368 s each, so about 5 to 6 min
per run, 7 waves per engine, about 40 min per engine and 1.5 h for the set. With more slots at
night, `-Parallel` and `--n` may be raised together (same value).

#### Comparison

```bash
cd D:/GnssProcessorSupport/runs_nom && python D:/GnssProcessorSupport/gates/day_program_2026-11-19/results/stream_replay/compare_user_runs.py _rap_sfxn_base_arht _rap_sfxn_fix_arht _rap_sfxn_base_ascg _rap_sfxn_fix_ascg _rap_sfxn_base_badg _rap_sfxn_fix_badg _rap_sfxn_base_cedu _rap_sfxn_fix_cedu _rap_sfxn_base_chti _rap_sfxn_fix_chti _rap_sfxn_base_cibg _rap_sfxn_fix_cibg _rap_sfxn_base_gop6 _rap_sfxn_fix_gop6 _rap_sfxn_base_hksl _rap_sfxn_fix_hksl _rap_sfxn_base_jplm _rap_sfxn_fix_jplm _rap_sfxn_base_mrc1 _rap_sfxn_fix_mrc1 _rap_sfxn_base_park _rap_sfxn_fix_park _rap_sfxn_base_quin _rap_sfxn_fix_quin _rap_sfxn_base_reyk _rap_sfxn_fix_reyk _rap_sfxn_base_seyg _rap_sfxn_fix_seyg _rap_sfxn_base_solo _rap_sfxn_fix_solo _rap_sfxn_base_stpm _rap_sfxn_fix_stpm _rap_sfxn_base_utqi _rap_sfxn_fix_utqi _rap_sfxn_base_wroc _rap_sfxn_fix_wroc _rap_sfxn_base_wtzr _rap_sfxn_fix_wtzr _rap_sfxn_base_wtzz _rap_sfxn_fix_wtzz _rap_sfxn_base_wuh2 _rap_sfxn_fix_wuh2 > D:/GnssProcessorSupport/gates/day_program_2026-11-19/results/stream_replay/night_compare.txt
```

The script compares `stdout.txt`, `stderr.txt` and every file under `logs/` line by line, after
removing from both sides the lines containing `NetworkProductReader::replay` (base: one warning per
epoch whose file counters disagree with the replay's rules; fix: one information line per such
epoch). Three items that differ between any two runs of one engine are replaced by fixed tokens
on both sides: the run folder name (it names the arm), the wall-clock age of the observations in
the `obs backlog` line, and the RINEX ingestion time. The 2 h check needed exactly these three and
nothing else (`results/stream_replay/user_compare_2h.txt`). It prints, per pair, the number of removed lines on each side and the first differing line
of every differing file. Exit code 0 when every pair is identical.

#### Bar

1. Every pair identical apart from the removed lines (`IDENTICAL apart from the marker lines`).
2. Per pair, the removed-line counts of the two sides are equal (one line per disagreeing epoch on
   both engines; the 2 h check and the checkpoint 1 table give 1 181 such epochs per user over the
   full day of `_rap_ad_n146`).
3. Any difference is named by class before anything else is concluded: (a) the replay store (a
   difference in the corrections the user applies: first epoch, key and values), (b) the real-time
   reaction changes of commit `1443d82` (`reinflate_isb_skipped` in `closed_loop_sat_trace.csv`,
   flip counts, ARC rows and `ev_pb` rows on carry or translate postures), (c) log text only. A
   class (a) difference refutes the identity claim; the run session reports it and fixes nothing.

#### Rank tonight

Set by the manager: after n19-bdsref, before n19-quiet; dropped to the next night if the list would
not finish by 07:00.
