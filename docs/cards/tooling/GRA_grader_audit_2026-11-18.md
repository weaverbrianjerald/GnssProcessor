# GRA: an audit of the grading scripts behind card NRF (2026-11-18)

**STATUS**

- **id:** GRA
- **topic:** tooling
- **status:** open
- **summary:** An audit, with no script changed, of the scripts that compute the bars of card NRF since 2026-11-14 (network integer fixing), on the model of card CHA. Section 1 maps every bar to its scripts, their helpers, inputs, recorded command lines, outputs and caches, and the sections that quote them; of 205 files in the grading folder `scripts_s20` 99 are in scope, with 7 modules of an ended session's temporary scratchpad, 7 repository scripts and the engine's truth-audit line. Section 2 classes every assumption (signal, family, constellation, path, layout, epoch time, rate, hash seed, cache). Part 2 holds the bars as one table, the defects in priority order with the reproductions that decide whether a recorded number moves, and a plan of steps that moves one owner per bar into the repository. Plan steps G1, G2, G4 and G5 were carried out on 2026-11-18 at the author's go (part 2, results): `scripts/grading/runs.py`, `network_hourly.py`, `slip_hold.py` and `users.py`, whose legacy views reproduce the recorded outputs byte for byte and whose corrected views equal the second session's independent rebuilds.
- **source:** the managing session's brief of 2026-11-18 (the author asked which other parts deserve card CHA's treatment; the grading scripts first, since every adoption decision since 2026-11-14 rests on their numbers); its steers after checkpoint 0 (the bars of section 118.1 first; the files added on 2026-11-18 by the session that runs the grades are their bars' newest owners); [CHA](../../refound/CHA_chain_audit_2026-11-18.md) for the form; [NRF part 13](../../refound/NRF_network_ring_fixing_2026-11-14_part13.md) to [part 16](../../refound/NRF_network_ring_fixing_2026-11-14_part16.md); the builder's hand-over note `D:\GnssProcessorSupport\gates\irdu_handover\IRDU_handover_2026-11-18.md`, sections 6 and 8 (outside the repository).

**Outcome.** No verdict of card NRF moves. Two recorded numbers read differently than stated. C4's GPS clause in section 118.1 compares rounded percentages: the "worst -3 points" is -2.34 exactly. The users' held pair-epochs of sections 107.3 and 118.1 count only pairs the network also holds, so "three times today's holds" is 1.6 to 1.7 times by the users' own count. Five latent defects are measured inert today: dropped slip flags, a member filter, a colon split, two tag-keyed caches. 33 scripts depend on a temporary folder.

## Scope and method

The audit reads the grading scripts as they stand on 2026-11-18 between 13:30 and 16:30, at branch `audit/graders` cut from `chain/audit` `bcf3bfe`. Nothing in `D:\GnssProcessorSupport\runs_nom` (run folders, `_irdu_grades`, `scripts_s20`), in the scratchpad copy or in any worktree was changed. Scripts were run only to reproduce recorded numbers, with `py -3.14 -B` (no `__pycache__` written beside them) and every output redirected into `D:\GnssProcessorSupport\gates\grader_audit_2026-11-18\` (section 7 of part 2 lists the reproductions). Two read-only worker sessions mapped the simulator graders and the code-signature and user graders; every line carried from their maps into this card was read again in the file. "(inferred)" marks a conclusion that no single line shows.

Abbreviations: **S20** `D:\GnssProcessorSupport\runs_nom\_irdu_grades\scripts_s20\`; **G** `D:\GnssProcessorSupport\runs_nom\_irdu_grades\grades_family_if\` (the outputs since section 107); **R** `D:\GnssProcessorSupport\runs_nom` (run `<tag>` is `R/_rap_<tag>`); **TMP** the ended managing session's scratchpad `C:\Users\weaverb\AppData\Local\Temp\claude\aa493991ddf9023f\5bfe1931-55d7-4f05-b483-81fc821b71e4\scratchpad\`; **TMP2** another ended session's scratchpad `C:\Users\weaverb\AppData\Local\Temp\claude\7104903fb15155a9\89732354-932b-4d61-8c4e-e6ff9af2df43\`; **SPC** the safe copy of TMP, `D:\GnssProcessorSupport\gates\irdu_handover\scratchpad_copy\`; **REPO** `scripts/` of the repository at this branch; **IRDU** the worktree `D:\GnssProcessorSupport\worktrees\irdu` (branch `irdu/user-integers`, owned by another session). A file named without a folder is in S20.

## 1. The map

### 1.1 Where the scripts live

| place | files | in scope | notes |
| --- | --- | --- | --- |
| S20 | 205 (137 `.py`, 66 `.sh`, 2 `.ps1`) and `__pycache__` | 99: 50 graders and helpers, 49 `grade_*.sh` | 15 added on 2026-11-18 after 12:00 by the session that runs the grades (the `grade_arm_*.sh` family, `c4_compare.py`, `user3d_by_hour.py`, `release_followup.py`, `scratch_run.py`, `derive_users_for.py`, `runs_arm_chain.sh`, `grade_sims_nrf45b.sh`, `band_split_census*.py`, `sat_pin_trace.py`, `lost_sat_code_truth.py`, two `.ps1`), live and still changing |
| out of scope in S20 | 106 | none | 36 arm makers, user derivers and launchers (`make_*`, `copy_arm.py`, `derive_*`, `runs_*`, `users_*`, `launch_*`, `belownormal_*`); 70 one-off reads and traces (`cq_trace*`, `hksl_*`, `j_*`, `n2c_*`, `o1_*`, ...) |
| TMP | 491 files, 263 of them `.py` | 7 modules | `run_dirs.py`, `nw_arms.py`, `users_hourly2.py`, `o1_emulate.py`, `o2_audit.py`, `user_fix_vs_float.py`, `ref_changes.py` |
| SPC | 263 | (copy) | every `.py` of TMP; the ten modules the graders import are byte-identical to TMP (md5); the 228 files SPC lacks are logs and text outputs |
| `D:\GnssProcessorSupport\presentation\agu26\fixing_control\tools\` | 279 | (copy) | a third copy of the TMP modules (identical md5 on the ten checked) and `stream_seg_drift.py`, imported by `seg_drift_hourly.py:19` |
| REPO | 410 | 7 | `nrf_two_run_audit.py`, `nrf_audit.py`, `nrf_reads.py`, `nrf_log_reads.py`, `nrf_hold_reads.py`, `irdu_stream_depth.py`, `nrf_settled_flags.py`; in IRDU the same files differ only by line endings (`diff --strip-trailing-cr` empty on six checked) |
| the engine | 1 line | 1 | the truth audit's run-end line, `src/core/network/ar/RingFixSeams.cpp:1832`, written to `logs/main_log.log` and `stdout.txt` |

Card NRF cites 192 script names: 140 exist only in S20, 35 in REPO, 13 only in TMP and its copies, 3 only in TMP2 (`ulab_e14_gap.py`, `lone_causes.py`, a copy of `o2_fail_trace.py`) and 1 (`users_chain_d146.sh`) only in TMP.

### 1.2 Imports and paths outside the folder

- **TMP, named directly** by 17 files: `user_fix_vs_float_full.py:32` (inserts TMP on `sys.path` for `o1_emulate`, `o2_audit`), `run_dirs_fill.py:21-22`, `scratch_run.py:16`, `grade_bar6.sh:8`, `grade_bar6_s2.sh:8`, `grade_bar6_n41.sh:8`, `grade_xusers_n41.sh:9`, `runs_nrf46.sh:9`, `users_b146.sh:6`, `users_chain_d147.sh:7`, `users_chain_d147_fill.sh:9`, `users_chain_m10.sh:6`, `users_chain_n41.sh:6`, `users_chain_s1.sh:6`, `users_chain_s2.sh:6`, `users_guard_d146.sh:8`, `users_guard_d146_x.sh:7`.
- **TMP2, named directly** by 9 files: the stage-O graders `grade_f12.sh:7`, `grade_o2j.sh:9`, `grade_o2r.sh:7`, `grade_o3o4.sh:7`, `grade_o5.sh:7`, `grade_o6.sh:7`, `launch_o3o4.sh:4` (its scratchpad), and `chain_cg_validate.sh:6`, `chain_cg_validate2.sh:3` (its task-output files).
- **Reached transitively** by 7 more: `user_fix_vs_float_free.py:21` imports `user_fix_vs_float_full`, whose import inserts TMP; `grade_users_n46.sh:23,27`, `grade_m10.sh:28,32`, `grade_after_chains.sh` and `grade_d146_full.sh` call those two scripts directly; `grade_arm_users.sh:39,43` and `runs_arm_chain.sh:43` call them through `scratch_run.py`, which puts TMP and SPC on the path (`scratch_run.py:26`). In all, 33 files depend on a temporary folder. Both TMP and TMP2 exist today.
- **IRDU's `scripts/`** is on the path of 8 scripts (`cascade_frac.py:23`, `flag_truth.py:28`, `make_zero_mode_bed.py:15`, `o1_raw_classify.py:7`, `o2_fail_trace.py:17`, `self_audit.py:8`, `t3l_truth.py:23`, `trace_vs_truth.py:11`), and the repository's two-run audit is run from IRDU by `grade_n46.sh:34-36` and `grade_sims_nrf45.sh:54-56`.
- **The presentation tools folder** is on the path of 9 scripts (`cq_trace.py:23`, `cq_trace2.py:21`, `cq_trace4.py:19`, `cq_trace5.py:19`, `cq_trace6.py:24`, `cq_trace7.py:16`, `flag_bar.py:19`, `sat_level_series.py:17`, `seg_drift_hourly.py:19`).
- **The run root.** Hard-coded `D:/GnssProcessorSupport/runs_nom` in `single_hourly.py:19`, `d24_hourly.py:24`, `slip_hold_check.py:24`, `gf_code_signature.py:31`, `arc_signature_spread.py:18`, `frame_free_count.py:26`, `grade_guard_sim.py:21`, `single_net_grade.py:22`, `check_member_records.py:10`, `SPC/o1_emulate.py:31`, `SPC/nw_arms.py:24`; read from `RAPID_RUNS_ROOT` in `compare_runs.py:18`, `paired_users.py:17`, `ref_changes_full.py:17`, `user_fix_vs_float_free.py:23`, `held_by_family.py:15`, `SPC/run_dirs.py:14`, `REPO/nrf_two_run_audit.py:38`.

### 1.3 The bars, one block each

Each block names the computing script, its inputs (file: columns), the command line as an orchestrator records it, what it writes, other implementations of the same bar, and the sections of card NRF that quote its numbers.

**B1. Held share by hour and family.**
- `single_hourly.py` (83 lines): held rows over all rows of `logs/network_ring_fix.csv` per hour from the first `E` record of `products.txt` (lines 22-30, 33-45); rows split into GPS L1C/L2W (line 37) and Galileo by the receiver's mode from a list file (lines 20, 39-40); printed as whole percentages (line 48).
- Inputs: `products.txt` (first `E` record, field 2); `network_ring_fix.csv`: `tow`, `sat`, `sig1`, `sig2`, `station`, `state`; the list `G/combined_mode_28.txt` (28 names; the per-day lists `x_stations_d24_n14{6,7}.txt` hold the same 28 and the held-out user).
- Command: `py -3.14 single_hourly.py $t $X > $G/single_$t.txt` (`grade_n46.sh:20`), also `grade_arm_net.sh:36-37`. Writes stdout only.
- Other implementations: `d24_hourly.py` (held rows over all rows per family of a fixed table, lines 25, 62-70; agrees with `single_hourly.py` hour for hour on `d24_n146`, section 3), `held_by_family.py` (held rows counted, no share, lines 20-31), `gps_share.py` (held and decided over all rows, lines 44, 49), `gf_code_signature.py` (per link-hour, for its own filter), `REPO/nrf_reads.py` (stage N2b, 15-minute bins).
- Quoted in sections 83 to 83.4, 92, 93, 104 (`d24_hourly.py`), 107.2, 110.3, 111.1, 112.2, 118.1 (`single_hourly.py`), 115 and 116.1 (`held_by_family.py`).

**B2. GPS held share.**
- `single_hourly.py` (the GPS row, line 37) and, for section 118.1's C4, `c4_compare.py` (56 lines, added 2026-11-18): it parses two `single_*.txt` files (lines 20-35) and prints, per row, the worst hourly difference and the mean over hours 3 to 24 (lines 41-50) from the printed whole percentages.
- Command: `py -3.14 c4_compare.py <arm single_*.txt> <control single_*.txt>` (its docstring, line 6). Writes stdout only.
- Other implementations: `gps_share.py` (held and decided, from hour 2, by receiver; section 51).
- Quoted in sections 107.2 (bar 3), 110.3, 112.2, 118.1 (C4).

**B3. RAIM rows.**
- `single_hourly.py:79-83`: every row of `logs/network_raim.csv` by hour, all systems. `d24_hourly.py` counts the same file. Section 107.2's split by system ("Galileo 14 against 9, GPS 13 against 12") has no script in S20 or REPO (an inline count, inferred).
- Quoted in sections 83 onwards, 107.2 (bar 4), 110.3, 111.1 (R3), 112.2, 118.1 (C4).

**B4. The two-run audit.**
- `REPO/nrf_two_run_audit.py` (337 lines): at each common epoch and band, the links both runs know (ring-fix rows in the given states, lines 49-58; tree links with an integer pin, lines 59-73) form a graph; the difference of the two runs' integers must be a receiver potential minus a satellite potential; a breadth-first walk counts the independent loops and the failing ones (lines 179-207). Self-test `--selftest` (lines 267-279).
- Inputs: `network_ring_fix.csv`: `tow`, `sat`, `sig1`, `sig2`, `station`, `state`, `n1`, `n2`; `network_chain_edges.csv`: `tow`, `system`, `band`, `edge_sta`, `edge_sat`, `target_cyc`.
- Command: `cd D:/GnssProcessorSupport/worktrees/irdu; py -3.14 scripts/nrf_two_run_audit.py $R/_rap_n46c_n146 $R/_rap_n46cr_n146 --json $G/audit_n46c_n146.json > $G/audit_n46c_n146.txt` (`grade_n46.sh:34-36`). Writes the `--json` file (lines 324-332); exit code 1 on any failure (line 333), not read by the orchestrators.
- Other implementations: `SPC/o2_audit.py` `attribute()` (stage O2, a sampled audit on the pins and holds), `aud_windows.py` (tallies a JSON's failures in DOY 144's windows, section 79.4).
- Quoted in sections 18 to 22.2, 62.1, 79.2, 107 and 107.2 (bar 5), 110.3, 112.2, 118.1 (C4).

**B5. The truth audit (right and wrong held link-epochs, wrong decisions).**
- Computed in the engine: `RingFixSeams.cpp:1832` writes "held link-epochs right N, WRONG N, without a true integer N; decisions WRONG N"; an audit-file line at `:1842` also contains "held link-epochs right" (written only when `network.ring_fix` names an audit file; none of the runs of sections 106 to 118 does, checked on seven logs).
- Parsers: `grade_guard_sim.py:24-33` and `grade_entry_sim.py:24-32` (one regex, last match wins); the shell helper `grep -h 'held link-epochs right' .../main_log.log | tail -1` (`grade_c3.sh:16`, `grade_sims_nrf45.sh:16`); `seed_grade.py` (the `stdout.txt` lines and the `truth=WRONG` tokens of RINGFIX hold events); `single_net_grade.py` (the event tokens). Decisions rebuilt offline on truth: `decision_cycles.py` (signal table line 31).
- Earlier offline implementation: `REPO/nrf_audit.py` (246 lines, sections 9 to 20.1).
- Quoted in sections 103 to 103.3, 106.1, 108.1, 111.1, 112.1 (F2), 116.1 (H3), 118.1 (C3).

**B6. The frame-free count.**
- `frame_free_count.py`: per sampled epoch, each held Galileo chord rebuilt on its fundamental cycle against the simulator's truth, the errors in units of the (3, 4) step covered greedily by the fewest links (lines 155-170). Inputs: the fixture's `S*SIM/truth_ambiguities.csv` and `truth_windup.csv`, `network_ring_fix.csv`, `network_chain_edges.csv`, `network_ionosphere.csv`. Writes `--cover-out`.
- Called as a subprocess with `PYTHONHASHSEED=0` by `grade_guard_sim.py:37-49` and `grade_entry_sim.py:35-47`, cached as `G/cover_seed0_<tag>.jsonl` (21 such files today).
- Quoted in sections 103.3, 108.1 (S3), 108.4, 111.1 (R2).

**B7. Links held through a flagged slip (bar 7) and through an injected slip (F3, C3).**
- `slip_hold_check.py` (167 lines): the flags of a reference run (`network_events.csv`, columns read by position: tier, event, sat, station, action, detail; lines 62-77) against the tested run's ring-fix rows of the link's E1 chord and its RINGFIX `release`, `to_tree`, `reattach` events (lines 80-99); the verdict at lines 102-121.
- Command: `py -3.14 slip_hold_check.py $ref $t --list 10 --out $G/slip_hold_$t.jsonl > $G/slip_hold_${t}_all.txt` (`grade_n46.sh:21`). Writes the `--out` file (lines 160-163). `slip_hold_spans.py` reads that file (section 107.9).
- Injected slips: `injected_slips.py` (the truth, by differencing each rewritten RINEX file against its source; writes a JSON-lines file) and `grade_slip_bar.py` (imports `slip_hold_check` by path, line 13-14, and applies its `verdict`).
- Quoted in sections 107.9, 110, 110.1, 110.3, 112.1 (F3), 112.2, 118.1 (C3, C4).

**B8. Lost satellite-samples.**
- `single_hourly.py:50-74`: every 30 minutes from the end of hour 1, the Galileo satellites of each band-1 tree (`network_chain_edges.csv`, `edge_sat` of rows with `system` E and `band` L1C or L1X) whose `P` record on that band does not carry `signal_integer` 1 in `products.txt`.
- Other implementations: `cq_tree_cert.py` (section 96, the flag of band 1 only, lines 33-35), `settled_graph.py` (section 96.1, both signals flagged, line 37, sampling 1800 s); `c4_compare.py:55-56` sums the families. All three give 437 and 58 on `d24_n146` and 390 and 25 on `d24_n147` (sections 96, 96.1 and `G/single_d24_n14{6,7}.txt`).
- Quoted in sections 96, 96.1, 101, 107 (bar 2), 107.2, 110.3, 112.2, 118.1.

**B9. The users' float check, inside and outside the network.**
- Inside: `user_fix_vs_float_full.py` (187 lines): for each network station with both user runs (`_rap_nw_obs<fix|flt>_<NW_DOY>_<site>`, lines 115-120), every held satellite pair of the fixing user (both single differences within 0.01 cycle of a whole number, line 82) against the network's integers at that receiver (pins and holds from `SPC/o1_emulate.pins`, `SPC/o2_audit.held`, lines 104-105, 127-130) and against the float user's narrow lane once converged within 0.20 cycle (lines 143-147); the wide lane is checked against the network (line 141).
- Outside: `user_fix_vs_float_free.py` (109 lines): the fixing user's held pairs against the float user alone, converged on both lanes (lines 71-85).
- Inputs: the users' `stdout.txt`, else `logs/ambiguity_resolver*.log` (`clfrac_sources`, `user_fix_vs_float_full.py:44-53`), regex at line 41; the network run's `arm.json` (`network.stations`), `network_ring_fix.csv`, `network_chain_edges.csv`.
- Commands: `py -3.14 user_fix_vs_float_full.py "_rap_n46c_n$D" --fix n46x --flt n46f --pair "$p" --hours 24 --per-site --list 40` and `py -3.14 user_fix_vs_float_free.py --fix n46x --flt n46f --doy $D --sites $OUT11 --pair "$p"` (`grade_users_n46.sh:23,27`). Write stdout only.
- Other implementations: `SPC/user_fix_vs_float.py` (the 6 h original, rotated logs, sections 83.3 and 91); `wl_off_list.py` (the wide lane against the network, the near-null column, sections 91.2 and 107.3).
- Quoted in sections 91 to 94, 107.3 (bar 6), 110.3, 112.2, 112.4, 114, 118.1 (C4).

**B10. The paired per-receiver difference.**
- `paired_users.py` (77 lines): per hour, the median over the receivers of each receiver's own difference of hourly 3D medians (`logs/consumer_replay_hourly.csv`: `hour`, `err3d_median_m`, lines 20-28, 50-57); per receiver its median, mean and worst hour; a summary line.
- Command: `py -3.14 paired_users.py n46$m a24$m --doy $D --sites $PILOT --name "pilot-type $m"` (`grade_users_n46.sh:17-18`). Writes stdout only.
- The difference of medians beside it: `SPC/users_hourly2.py --matrix` (through `grade_bar6*.sh:12`) and `bar6_receivers.py`.
- Quoted in sections 112.4, 114, 118.1 (C4); the difference of medians in 107.3 and 110.3.

**B11. Reference changes.**
- `ref_changes_full.py` (58 lines): per user run and system, the epochs whose reference satellite differs from the previous epoch's (lines 33-54), from the CLFRAC lines read as in B9 (lines 21-30, a copy of `user_fix_vs_float_full.py:44-53`).
- Command: `py -3.14 "$S/ref_changes_full.py" _rap_nw_obsa24x_<D>_<site> _rap_nw_obsn46x_<D>_<site> ...` (`grade_users_n46.sh:30`). The sums over receivers in section 118.1 have no script (inferred: added by hand from the per-run lines).
- Earlier: `SPC/ref_changes.py` (rotated logs, section 91).
- Quoted in sections 91, 91.1, 107.3 (bar 6), 110.3, 112.2, 118.1 (C4).

**B12. The off-identity comparisons.**
- `compare_runs.py` (102 lines): `products.txt` compared after removing comment lines and the records `P` on L1X or L5X and `B` on C1X or C5X of Galileo satellites (lines 19-20, 23-40); every `logs/network_*.csv` present in both runs, line by line (lines 88-98).
- Commands: `compare_runs.py u2_z45 u2_z`, `compare_runs.py m1_off45 m1_off37` (`grade_sims_nrf45.sh:33`), `compare_runs.py $t $runa` (`grade_n46.sh:22`), `compare_runs.py m1_c u1_m1` (`grade_c3.sh:24`). Writes stdout only.
- Other implementations: the member-record check in awk (`grade_n46.sh:24-30`), `check_member_records.py` (section 112.1, F2), `grade_l12_parts.py identity` (section 61), and a whole-file `cmp` added on 2026-11-18 in `grade_sims_nrf45b.sh:24-25`.
- Quoted in sections 112.1 (F1), 116.1 (H1), 118.1 (C1 to C3), 119.1.

**B13. The code signature.**
- `gf_code_signature.py`: per link-hour where the link is held at least `--held` of the hour, the mean C1 minus C5 code residual (`network_pf_rows.csv` by position: `$2` tow, `$3` station, `$4` sat, `$5` signal, `$6` kind, `$7` resid), centred on the median of the satellite's held links of the same mode; deviations in the step's band counted. Writes the caches `G/gfsig_<tag>.txt` (lines 44-65) and `G/gfheld2_<tag>.txt` (line 68) and three awk programs of fixed names (`gfsig_pf.awk`, `gfsig_rf.awk`, `gfsig_tree.awk`, lines 49-52, 70-74).
- `arc_signature_spread.py` (the whole-arc spread, section 108.3; writes `G/arcsig_<n>.awk`, line 30) and `mode_code_trace.py` (the combined mode's code columns, section 112.6; writes nothing).
- Quoted in sections 107.2, 107.3, 108.3, 110.3, 112.2, 112.6.

### 1.4 The orchestrators of the recent sections

| orchestrator | bars | writes in G | sections |
| --- | --- | --- | --- |
| `grade_n46.sh` | B1 to B4, B7 (bar 7), B8, B12 (C2 and the member records) | `single_<t>.txt`, `slip_hold_<t>.jsonl`, `slip_hold_<t>_all.txt`, `c2_<t>_vs_<runa>.txt`, `members_<t>.txt`, `audit_n46c_n146.{txt,json}` | 118.1 |
| `grade_users_n46.sh <doy>` | B9, B10, B11 | `n46users_paired_<D>.txt`, `n46users_holds_<pair>_<D>.txt`, `n46users_holds_out_<pair>_<D>.txt`, `n46users_refch_<D>.txt` | 118.1 |
| `grade_c3.sh` | B5, B7 (injected), B12 | `c3_n46.txt`, `decision_cycles_m1_c.jsonl` | 118.1 |
| `grade_sims_nrf45.sh` and `_b` | B1 (S4, H3), B5, B6, B12 | `sims115_release[_b].txt`, `sims116_bylife[_b].txt`, `sims117_ebdq[_b].txt`, `audit_t3t45.*`, `audit_t3q.*` | 116.1, 119.1 |
| `grade_bar6.sh`, `grade_bar6_s2.sh`, `grade_bar6_n41.sh` | B9, B10 (difference of medians), B11 | `bar6*_hourly_<D>.txt`, `bar6*_holds_*`, `bar6*_refch_<D>.txt`, `bar6*_wloff_*` | 107.3, 110.3, 112.2 |
| `grade_m10.sh`, `grade_xusers_n41.sh` | B9 to B11 | `m10_*`, `xusers_*` | 112.4, 114 |
| `grade_d24.sh`, `grade_root_tests.sh` | B1, B3, B4 | `../nrf_d24_hourly_<t>.txt`, `../nrf_d24_audit_<D>.*`, `../nrf_root_audit_<T>.*` | 83.1, 92.1, 93.1, 104 |
| `grade_arm_net.sh`, `grade_arm_users.sh`, `grade_arm_sims.sh` (2026-11-18) | the three above, by arm label | `arm_net_<L>.txt`, `<L>users_*`, `arm_sims_<L>.txt` | none yet |

None of the shell orchestrators sets `-e` or reads an exit code; each prints its "graded" line whatever its children did (inferred from the scripts' text: no `set -e`, no `$?`).

## 2. Assumptions

Class: **safe** (holds for every run in the record and for the next planned runs); **record** (wrong for a run already in card NRF: the run is named; whether a number moves is part 2's question); **planned** (wrong for a run the program plans: a native signal family, BeiDou, a third frequency, a streamed network at 1 s).

| # | file:line | assumes | class |
| --- | --- | --- | --- |
| A1 | `single_hourly.py:37` | GPS is the L1C/L2W pair only | safe; planned: a GPS L5 pair family is left out of the GPS row |
| A2 | `single_hourly.py:38-42` | Galileo is every Galileo row of any family; other constellations are dropped | planned: BeiDou has no row; a third-band Galileo family (E1/E5b) enters "Galileo all" and the mode rows |
| A3 | `single_hourly.py:58` | the band-1 trees are named L1C or L1X, system E | safe for every run in the record; planned: BeiDou and any identity of another name are not counted |
| A4 | `d24_hourly.py:25`, `:66-67` | three families (G L1C/L2W, E L1X/L5X, E L1C/L5Q); others skipped | planned |
| A5 | `slip_hold_check.py:25`, `:68` | a flag names an E1 or E5a signal (`^L[15][A-Z]$`), Galileo only | safe for bar 7 as defined; planned: slips on a third held band are never flags |
| A6 | `slip_hold_check.py:53-59`, `:75` | every slip event names its signal in its leading tokens | record: the engine's `lli_slip` events carry "LLI", "LLI;mw_step" or "LLI;gf_step" (`SlipQc.cpp:910-916`) and are dropped; 273 on `d24_n146`, 330 on `d24_n147` (part 2, D3) |
| A7 | `slip_hold_check.py:76`, `:87` | a link is (station, satellite); the flag keeps no signal; the tested rows are any chord with sig1 on L1 | safe; planned: a receiver that keeps both members of a family (card CHA P6) mixes two chords in one row list |
| A8 | `compare_runs.py:19-20`, `:35-38` | L1X/L5X phase and C1X/C5X code records of Galileo are member records | record: in `u2_z` and `u2_z45` the 35 268 records set aside are the combined mode's own C1X/C5X code biases; in `m1_off45`/`m1_off37` (separate families) they would be a family's own records (part 2, D4) |
| A9 | `compare_runs.py:88` | the traces to compare are those present in both runs | safe today (`u2_z` and `u2_z45` have the same 42); a trace missing from one run is not reported |
| A10 | `user_fix_vs_float_full.py:40` (and `user_fix_vs_float_free.py:50`) | the pair's frequencies: G L1/L2, E E1/E5a | safe for the pairs graded; planned: a GPS pair on L5 takes L2's frequency (a wrong narrow lane, no error); BeiDou raises KeyError |
| A11 | `user_fix_vs_float_full.py:115-120` | the inside users are the network's stations, the site is `name[:4].lower()`; a missing run is skipped | safe; a user outside the network or a missing folder is silently absent from the count |
| A12 | `user_fix_vs_float_full.py:78` | a held pair is counted only when the network has an integer at that receiver for both satellites | record: the held counts of sections 107.3 and 118.1 (part 2, D2) |
| A13 | `user_fix_vs_float_full.py:37`, `SPC/users_hourly2.py:17` | `NW_DOY` set, default 144 | safe (every caller exports it: `grade_users_n46.sh:13`, `grade_m10.sh:14`, `grade_bar6*.sh:10`, `grade_arm_users.sh:29`); a caller without it grades folders of DOY 144 |
| A14 | `user_fix_vs_float_free.py:23` against `SPC/o1_emulate.py:31` | the existence check reads `RAPID_RUNS_ROOT`, the reads use the hard-coded root | safe while both are `runs_nom`; another root passes the check on one tree and reads the other, and an empty read is skipped silently (`:59-61`) |
| A15 | `ref_changes_full.py:42` | one reference satellite per system and epoch (the last CLFRAC line wins) | safe; planned: a user on a third frequency or on two families reports one reference per signal |
| A16 | `frame_free_count.py:27`, `decision_cycles.py:31` | Galileo E1/E5a with the identities L1C and L1X | safe; planned: another held family raises KeyError |
| A17 | `gf_code_signature.py:35-36`, `arc_signature_spread.py:20` | Galileo, codes C1* against C5*; the E1/E5a step 4 lambda_5a - 3 lambda_1 hard-coded | safe; planned: third frequency, BeiDou |
| A18 | `mode_code_trace.py:26`, `:37`, `:47` | E1/E5a frequencies; C1C, C1X, C5Q, C5X; tracking read by default from `_rap_n43_smoke` | safe; the default tracking file belongs to one smoke run |
| A19 | `check_member_records.py:11` | the families (L1C, L1X) and (L5Q, L5X) | safe; planned: native family |
| A20 | `single_net_grade.py:20`, `:46-49` | the user's hours start at TOW 261060 and run 1 to 6 | safe for section 106's beds; any other start loses its hours silently |
| A21 | `aud_windows.py:11-21` | DOY 144's windows as small times of week and four southern stations | record-bound: correct only for the runs of section 79.4 |
| A22 | `bar6_receivers.py:29-31` | the base sets are `a24x` and `a24f`, 24 hours | safe for its sections; any other base needs an edit |
| A23 | the hard-coded run roots of section 1.2 | the run is in `runs_nom` | safe today; a finished run moved to `D:\GnssProcessorSupport\runs\` (the project's rule) fails loudly on a rerun |
| A24 | the temporary paths of section 1.2 | TMP and TMP2 exist | safe today; a cleaned temporary folder breaks 33 files (ImportError or "No such file") |
| A25 | `grade_n46.sh:34`, `grade_sims_nrf45.sh:54` | IRDU exists and its `scripts/` holds the audit | safe today (same content as REPO, line endings aside) |
| A26 | `single_hourly.py:78`, `slip_hold_check.py:68-76`, `gf_code_signature.py:35-41` | trace columns read by position | safe today (headers checked: ring-fix `$9` state, chain edges `$14` target_cyc, pf rows `$7` resid); a column added before them shifts every read silently |
| A27 | `single_hourly.py:25`, `:36`, `:57`, `:68`; `slip_hold_check.py:76`, `:88`, `:98`; `REPO/nrf_two_run_audit.py:55`, `:67`; `user_fix_vs_float_full.py:66` | epoch times rounded before matching | safe under `processing.exact_epoch_time` (the stream's times are not whole seconds; the hand-over's trap) |
| A28 | `held_by_family.py:23-28` | hours from the first row's exact time | safe (a sub-second shift of the hour boundary, inferred) |
| A29 | `slip_hold_check.py:35-36`, `d24_hourly.py:116` | a 30 s epoch (window t to t + 30 s, look 600 s; 120 epochs an hour) | planned: at 1 s the bar's "the flag's epoch or the next" becomes 30 epochs and "within 20 epochs" 600 |
| A30 | `frame_free_count.py:162-167` | the greedy cover's ties broken by set order, which depends on `PYTHONHASHSEED` | record: two identical runs gave 545 and 510 (section 108.4); the graders now set the seed (`grade_guard_sim.py:39-43`) |
| A31 | `arc_signature_spread.py:30` | an awk file named from `hash(prog)` | safe (a name only; a new file per seed) |
| A32 | `gf_code_signature.py:46-47`, `:68-69` | the cache `gfsig_<tag>.txt` is valid whenever it exists; its key is the tag only, though `--doy-tow0` is baked into its hours | safe today (twelve tags cached once each); a redirected report or a rerun with another `--doy-tow0` is read back silently (part 2, D7) |
| A33 | `grade_guard_sim.py:40-41`, `grade_entry_sim.py:38-39` | the cover cache `cover_seed0_<tag>.jsonl` is valid whenever it exists; the key omits the truth bed and the stride | safe today (all 21 caches newer than their runs' products, checked); a rerun of a tag reuses the old cover (part 2, D6) |
| A34 | `grade_arm_net.sh:37` | a non-empty control file is a valid table | live file; a control file holding a traceback is reused as a table |
| A35 | `grade_guard_sim.py:82`, `grade_entry_sim.py:98` | `ctl:arm:fixture` with no colon inside the fixture | record: `grade_sims_nrf45.sh:21-22` passes `D:/...`; the split raises ValueError before any line is graded (part 2, D5) |
| A36 | `REPO/nrf_two_run_audit.py:65-66` | a tree pin whose `target_cyc` is not a whole number is skipped | safe today (0 fractional of 975 686 and 1 138 243 rows on `n46c_n146` and `d24_n146`) |
| A37 | `SPC/o1_emulate.py:41` | a tree pin's `target_cyc` is rounded to a whole number without a check | safe today (as A36); the two implementations of "the network's integer at a tree link" differ on a fractional pin |

Continued in [part 2](GRA_grader_audit_2026-11-18_part2.md): the bars as one table, the defects in priority order, the plan, the reproductions.
