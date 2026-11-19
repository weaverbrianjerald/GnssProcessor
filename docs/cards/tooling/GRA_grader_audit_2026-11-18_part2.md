# GRA part 2: the bars as one table, the defects, the plan (sections 3 onwards of card GRA)

**STATUS**

- **id:** GRA-part2
- **topic:** tooling
- **status:** open
- **summary:** Sections 3 onwards of card GRA (2026-11-18), continued from [part 1](GRA_grader_audit_2026-11-18.md). Section 3: each bar's definition as pre-registered in card NRF beside what its script computes, with every difference named, and the bars with more than one implementation with their agreement on named runs. Section 4: the defects in priority order (silent changes of a recorded number first, then what breaks a rerun, then duplication and scripts named nowhere), each with its evidence, the reproduction that says whether a number of card NRF moves, and a test that would catch it. Section 5: a plan of ten steps that moves one owner per bar into the repository, each step reproducing named recorded numbers exactly; nothing of it is carried out here. Section 6: what the plan enables. Section 7: the reproductions made for this card. Results: plan steps G1, G2, G4 and G5 carried out on 2026-11-18 (`scripts/grading/`), legacy views byte-identical to the recorded outputs, corrected views equal to the second session's rebuilds. Results of 2026-11-19 (the day program's grader package): step G3 as a second implementation of the two-run audit with a count by distinct link (`two_run.py`), F5 for GPS by the chord's own signals (`slip_hold.py`), the integrity of held links (`held_integrity.py`: long one-signed holds, the windowed monitor replay, resumptions meeting a held link, holds against the simulator's truth) and the users' runs of wrong holds by receiver type (`users.py`), each gated on the recorded numbers; steps G6 to G10 wait for the author's go.
- **source:** [part 1](GRA_grader_audit_2026-11-18.md); the scripts as of 2026-11-18 afternoon; [NRF part 14](../../refound/NRF_network_ring_fixing_2026-11-14_part14.md) to [part 16](../../refound/NRF_network_ring_fixing_2026-11-14_part16.md); the outputs in `D:\GnssProcessorSupport\gates\grader_audit_2026-11-18\`.

**Outcome.** Every verdict of card NRF that was reproduced stands. Two recorded statements need an annotation in card NRF: the C4 GPS figures of section 118.1 are differences of rounded percentages (exact worst -2.34, -0.51, -0.44 points), and the users' held pair-epochs of sections 107.3 and 118.1 count only pairs the network also holds (the users' own gain is 1.3 to 1.7 times, not 2 to 3.2). Bar 7 drops every `lli_slip` flag; counted, they add 159 to 259 flags a day and no hold through one. On 2026-11-19 bar F4 by distinct link names the attribution's two links, F5 for GPS fails only on BOGT00COL G09 (DOY 143), and every recorded held-link count reproduces except one window-replay tie.

Abbreviations as in part 1 (S20, G, R, TMP, TMP2, SPC, REPO, IRDU).

## 3. The bars as one table

| bar | pre-registered (card NRF) | what the script computes | differences |
| --- | --- | --- | --- |
| B1 held share | 107, bar 1: "Galileo held share over the pilot receivers' links at least 75 % in every hour from 3"; over all Galileo links beside it | `single_hourly.py:33-48`: rows with state `held` over all rows of the ring-fix trace, per hour from the first `E` record; a station not in the combined-mode list counts as pilot | (a) a link counts only in epochs it has a ring-fix row; (b) the decision epoch (`decided`) is not held, while `gps_share.py:44,49` counts it held; (c) printed in whole percentages (line 48), the only form later tools read |
| B2 GPS held share | 107, bar 3: "not lowered by more than 2 points in any hour from 3"; 118, C4: "not below today's by more than the neutral two-root spread (3 points) in any hour from 3, its mean over hours 3 to 24 not lower by more than 1 point" | `c4_compare.py:41-50`: worst and mean of the differences of `single_hourly.py`'s rounded percentages | the bar is on shares, the script on rounded shares: worst differences off by up to 0.66 point on section 118.1's runs (D1); the "neutral spread" of 3 points is itself a rounded figure (exact -2.55, `d24r_n146` against `d24_n146`, hour 20) |
| B3 RAIM rows | 107, bar 4: "RAIM rows not above today's on either day"; 118, C4: "within today's pair's own difference" | `single_hourly.py:79-83`: rows of `network_raim.csv` by hour, every system | none for the totals; the split by system quoted in 107.2 has no script; "within the pair's difference" is judged by hand from two printed lists |
| B4 two-run audit | 107, bar 5: "`s1_n146` against `s1r_n146` ... not worse in any family", states held, decided, would hold; 118, C4: "0 failing Galileo checks" | `REPO/nrf_two_run_audit.py`: loops of the graph of common links per band, default states `held,decided,would_hold` (line 288) | (a) a check is counted on each band, so "1 661 228 Galileo checks" is twice the loops of E1/E5a; (b) a non-zero failing count depends on the spanning tree (line 181), zero does not; (c) a tree pin with a fractional target is skipped (lines 65-66; none today) |
| B5 truth audit | 108, S3; 112, F2; 116, H3; 118, C3: wrong held link-epochs and wrong decisions | the engine's line `RingFixSeams.cpp:1832`, in each epoch's frame; parsed by regex (`grade_guard_sim.py:27`) or by `grep ... \| tail -1` (`grade_c3.sh:16`) | the shell form takes the last line containing "held link-epochs right", which would be the audit-file line of `RingFixSeams.cpp:1842` (another count) if a run named an audit file; none of sections 106 to 118 does |
| B6 frame-free count | 108, S3: "links a cycle off (mean per sampled epoch, each family) not above the guard-off run's" | `grade_guard_sim.py:89-90`: the greedy cover's links summed over the sampled epochs (stride 10); `frame_free_count.py:184-195` prints a mean over the epochs with a wrong chord | three normalisations for one name; the run-against-control verdicts agree because both runs have the same sampled epochs (720 epochs each, inferred from the beds' lengths); the recorded figures (545, 510; section 108.1's table) are sums |
| B7 held through a flagged slip | 110, bar 7, as amended in 110.1: the flag list (with T0_mw_detect `lli_slip` refloat); the link's last row before the flag within 20 epochs held, and at the flag's epoch or the next still held or taken into the tree as a known step (RINGFIX `reattach` held_first), no release | `slip_hold_check.py:62-121` | (a) `lli_slip` flags are dropped: their detail names no signal (D3); (b) the windows are 30 s and 600 s (lines 35-36), equal to one and 20 epochs only at 30 s; (c) any `reattach` or `to_tree` event counts as held through (lines 111-112), wider than "reattach held_first"; (d) with no row in the window the next row within 600 s decides (lines 116-121), an addition of the script's docstring; (e) a flag is a (station, satellite, epoch): a flag on each band at one epoch counts once (line 76), so "3 695 flags" counts link-epochs |
| B7b held through an injected slip | 112, F3; 118, C3: "no link held through an injected slip (`grade_slip_bar.py`, the rule of section 110's bar 7)" | `grade_slip_bar.py` with `slip_hold_check.verdict`; truth from `injected_slips.py` | as B7 (b) to (d); the truth needs no event parse, so (a) does not apply |
| B8 lost satellite-samples | 107, bar 2: "section 96.1's read: satellites of the family's tree not flagged, every 30 min from hour 1" | `single_hourly.py:50-74`: band-1 tree satellites whose band-1 `P` record lacks `signal_integer` 1, at 46 epochs from t0 + 1 h to t0 + 23.5 h | section 96.1's tool requires both signals flagged (`settled_graph.py:37`), `single_hourly.py` and section 96's `cq_tree_cert.py:33-35` read band 1 only; the three agree on `d24_n146` (437, 58) and `d24_n147` (390, 25) |
| B9 float check | 107, bar 6: "no hold wrong by the float check"; 118, C4: "(the combined-mode users by the check without the network's integers)" | inside: `user_fix_vs_float_full.py:135-147`, the narrow lane against the float user; outside: `user_fix_vs_float_free.py:71-85`, both lanes | (a) inside, a near-null (3, 4) error passes as right (its narrow-lane offset is 0.05 cycle) and shows only in the "wide lane off" column against the network; (b) inside, a held pair is counted only where the network has both satellites' integers at that receiver (line 78), so its held count is a coverage count (D2) |
| B10 paired difference | 118, C4: "the paired per-receiver difference not worse by more than 1 mm in any hour from 3, the difference of medians beside it" | `paired_users.py:50-57`: per hour the median over the receivers of A minus B | a receiver missing an hour leaves that hour's median (line 52) without a message; receivers missing a whole run are listed (lines 45-48) |
| B11 reference changes | 107, bar 6: "Galileo reference changes not above today's at any receiver" | `ref_changes_full.py:33-54`: changes between consecutive epochs that have a CLFRAC line, per system | a gap is bridged (a change across it counts once); one reference per system and epoch (A15); the sums over receivers in section 118.1 have no script |
| B12 off identity | 112, F1; 116, H1: "reproduces ... in `products.txt` and the ring-fix and chain-edge traces"; 118, C1, C2 | `compare_runs.py`: products without the comment lines and the fixed member records, and the traces present in both runs | (a) "reproduces" becomes "identical once L1X/L5X and C1X/C5X records are set aside", whatever the run declares (D4); (b) member records are counted (line 85), not compared; C2's equality came from `grade_n46.sh:24-30`; (c) a trace absent from one run is not reported (line 88) |
| B13 code signature | 107.2: per link-hour where the link is held "(a held chord, or a tree link with its integer pin) at least 90 % of the hour, with 20 code epochs or more", against the median of the satellite's held links of the same mode | `gf_code_signature.py`; tree links count only with `--with-tree` (its help, line 29; cache `gfheld2_<tag>.txt`) | the default omits tree links; the recorded outputs `gfsig_tree_*` used `--with-tree` (inferred from their names) |

**Bars with more than one implementation, and their agreement on named runs.**
- Held share: `single_hourly.py` and `d24_hourly.py` print the same hourly percentages for `d24_n146` (`G/single_d24_n146.txt` against `_irdu_grades/nrf_d24_hourly_d24_n146.txt`: the GPS row and the two Galileo families' rows equal in all 24 hours; RAIM rows equal, 21). `gps_share.py` counts `decided` as held (0.3 % of the rows of `d24_n146`: 5 239 of 1 797 285); not compared on a named run.
- Lost satellite-samples: `cq_tree_cert.py`, `settled_graph.py` and `single_hourly.py` agree on `d24_n146` and `d24_n147` (B8).
- The truth audit: the engine's line and `REPO/nrf_audit.py` (sections 9 to 20.1) are separate implementations; the offline one has not been run on a run of sections 103 onwards.
- The network's integer at a tree link: `REPO/nrf_two_run_audit.py:65-66` skips a fractional target, `SPC/o1_emulate.py:41` rounds it; identical today (no fractional target in `n46c_n146`, `n46cr_n146`, `d24_n146`).
- The float check: inside and outside rules differ (B9). Applied to the same ten pilot-type users, both give 0 wrong on DOY 146 and 147 for `n46`, `a24` and `s1` (section 7).
- Off identity: `compare_runs.py`, `grade_l12_parts.py identity` (line 28 iterates run A's traces only) and a whole-file `cmp` (`grade_sims_nrf45b.sh:24-25`). On `u2_z45` against `u2_z`, `compare_runs.py` and `cmp` agree (section 7).

## 4. Defects, in priority order

### 4.1 What can make a recorded number wrong without an error

**D1. C4's GPS clause compares rounded percentages.** `single_hourly.py:48` prints `{:.0f}`; `c4_compare.py:26-28` parses those integers and computes the worst and the mean from them (lines 46-50). Section 118.1 quotes "worst -3 points (hour 4: 86 against 89 %), mean -0.23 (86.64 against 86.86 %)" for `n46c_n146`, and calls hour 4 "at the bar's edge". Reproduced unrounded with the same counting rule (section 7): worst -2.34 (86.19 against 88.54 %), mean -0.13; `n46cr_n146` worst -0.51 at hour 19 (recorded -1 at hour 4), mean -0.04; `n46c_n147` worst -0.44 at hour 12 (recorded -1 at hour 24), mean -0.03; the two-root spread -2.55 (rounded -3). **Affected:** the figures of 118.1's C4 GPS clause; the verdict passes with 0.66 point to spare. At a bar of whole points a rounded difference can fail a passing hour (86.45 against 89.55 % prints 86 against 90, "-4" for -3.10) or pass a failing one (85.6 against 89.4 % prints 86 against 89, "-3" for -3.8). Test: a fixture of two hourly tables, 85.6 % against 89.4 %, expected -3.8 and a failure.

**D2. The inside float check counts the network's coverage and reads the narrow lane only.** `user_fix_vs_float_full.py:78` keeps a satellite only if the network has an integer for it and for the reference at that receiver; its "wrong" compares the narrow lane alone with the float user (lines 143-147). Sections 107.3 ("three times today's Galileo holds on DOY 146 and twice on DOY 147") and 118.1 ("100 625 Galileo pair-epochs against today's 31 323") quote that held count. The users' own held pairs (the rule of `user_fix_vs_float_free.py`, every hold of the fixing user, both lanes) on the same ten receivers: `s1` 95 858 against 59 908 (1.60) and 93 910 against 73 002 (1.29); `n46` 101 673 against 59 908 (1.70) and 103 787 against 73 002 (1.42). Wrong by both lanes: 0 in all six user sets. **Affected:** the held-count readings of 107.3 and 118.1, not the bar (no hold wrong stands under the stricter rule). Test: a CLFRAC fixture with one pair held at (3, 4) off the float user's branch, expected wrong; a fixture where the network lacks one satellite, expected the user's hold still counted.

**D3. Bar 7 drops every `lli_slip` flag.** `slip_hold_check.py:53-59` takes the leading tokens of the detail that match `^L[15][A-Z]$` (line 25); the engine writes `lli_slip` details "LLI", "LLI;mw_step", "LLI;gf_step" (`SlipQc.cpp:910-916`), one event per satellite for every band with loss of lock (`SlipQc.cpp:886-908`), so line 75 drops them all. Bar 7's list names `lli_slip` refloat. Reproduced (section 7) with every Galileo `lli_slip` refloat event taken as a flag on the link's E1/E5a chord (an upper bound, since the event may concern another band): the script's own counts reproduce 118.1 exactly (3 695 flags, 281 released, 0 held through; 3 695, 291, 0; 3 709, 381, 0); the dropped events not already a flag are 159, 159 and 259, of which 35, 32 and 41 released and 0 held through. **Affected:** the flag counts of 110.3, 112.2 and 118.1 are low by 4 to 7 %; no verdict moves. Test: an events fixture with an `LLI` refloat on a held link and no other flag, expected one flag.

**D4. `compare_runs.py` sets aside records by fixed names.** Lines 19-20 and 35-38 drop Galileo `P` on L1X/L5X and `B` on C1X/C5X from both files, whatever the run declares; line 85 counts them; line 88 compares only traces present in both runs. In `u2_z` (the phase relabel, codes kept apart) the 35 268 records set aside are B C1X 17 634 and B C5X 17 634, the combined mode's own code biases. In a separate-family run (`m1_off37`) they would be a family's own phase and code records. **Affected:** none. `cmp` gives `products.txt` byte-identical for `u2_z45` against `u2_z` (section 116.1's H1, which the managing session had reported unaffected: confirmed), `m1_off45` against `m1_off37` (H1's second pair), `h40_off45` against `h40_off` and `t3t45_sim` against `t3t_sim`; the traces of `u2_z45` and `u2_z` differ only in the three wall-time files, as 116.1 states. Test: two products files differing in one B C1X record of a run without a family legend, expected DIFFER.

**D5. Lost satellite-samples changed definition between sections.** Section 107 pre-registers "section 96.1's read" (both signals flagged, `settled_graph.py:37`); `single_hourly.py:72` reads band 1 only. Equal on `d24_n146` and `d24_n147`; on a run where a satellite is flagged on band 1 and not on band 2 they differ (inferred). **Affected:** none found. Test: a products fixture with one satellite flagged on L1C and not on L5Q, the two tools expected to differ by one.

**D6. The frame-free cover is cached by tag.** `grade_guard_sim.py:40-41` and `grade_entry_sim.py:38-39` reuse `G/cover_seed0_<tag>.jsonl` whenever it exists; the key holds neither the truth bed, the stride nor the run's time. A tag rerun after a crash or a restart (section 119.1 restarted `m1_nr`, `d1_nr`, `h40_off45` from the start) is graded on the old cover if one was written first. **Affected:** none: all 21 caches are newer than their runs' `products.txt` (checked, section 7). Test: grade a tag, rewrite its `products.txt`, grade again, expected a recomputed cover.

**D7. The code-signature cache is keyed by tag and can be emptied by a redirect.** `gf_code_signature.py:46-47` reads `G/gfsig_<tag>.txt` whenever it exists; `--doy-tow0` is baked into its hours (lines 53-56) but not into its name; a report redirected to that name truncates it first and is then read back as an empty table (lines 96-99, inferred from the code; the hand-over's trap). Reports named `gfsig_n146.txt`, `gfsig_n147.txt`, `gfsig_tree_n146.txt`, `gfsig_tree_n147.txt` sit in the cache's namespace (107.2's outputs; no run has such a tag). **Affected:** none found (twelve run tags cached once each). Test: run with one `--doy-tow0`, then another, expected different hours.

**D8. Silent zeros that read as a pass.** `user_fix_vs_float_full.py:119-120` skips a receiver whose user runs are missing and prints the receiver count only in its header; a caller without `NW_DOY` grades folders of DOY 144 (line 37), finds none, and prints no "wrong"; `user_fix_vs_float_free.py:59-61` skips an empty read with no line; `check_member_records.py:37` prints "0 identity records, 0 without an equal member record" for a stream with none; `paired_users.py:52` drops a receiver's missing hour; `SPC/users_hourly2.py:72` prints no paired column when `--base` is not one of the specs, as `grade_xusers_n41.sh:14-15` passes it (`G/xusers_paired_x_146.txt` holds the arm's medians only; section 112.4's paired figures came from `paired_users.py`, `G/xusers_paired_0542.txt`). `single_net_grade.py:20` places the user's hours from TOW 261060, losing any other start's hours silently (lines 46-49). **Affected:** none found; every recorded call set `NW_DOY` and named existing folders (A13). Test: each tool on an empty or missing input, expected a non-zero exit.

**D9. Two definitions of "held" for one name.** `single_hourly.py:45` and `d24_hourly.py:69-70` count `held`; `gps_share.py:44,49` counts `held` and `decided`; `held_by_family.py:26` counts `held` rows; `REPO/nrf_two_run_audit.py` takes `held,decided,would_hold` (line 288); `SPC/o2_audit.py:25` takes `held,decided` as the network's integers. Each is right for its bar; the name does not say which. **Affected:** none. Test: a ring-fix fixture with one `decided` row, each tool's count stated.

### 4.2 What can break a rerun

**D10. Temporary folders.** 33 files depend on TMP or TMP2 (part 1, section 1.2), including the inside float check of every user bar (`user_fix_vs_float_full.py:32`), every user run launcher (`run_dirs.py` through `runs_nrf46.sh:9`, `users_chain_*.sh`) and the difference of medians (`grade_bar6*.sh:8`). The stage-O graders and `chain_cg_validate*.sh` depend on TMP2, a second ended session's folder, which the hand-over note does not name and SPC does not copy. Test: run each orchestrator with TMP and TMP2 absent from the path, expected success.

**D11. An absolute fixture path split at its colon.** `grade_guard_sim.py:82` and `grade_entry_sim.py:98` unpack `spec.split(":")` into three; `grade_sims_nrf45.sh:21-22` passes `m1_off37:m1_nr:D:/GnssProcessorSupport/runs_nom/simb_mode1`, four parts, so the first spec raises ValueError and nothing is graded (the traceback goes into `G/sims115_release.txt`, and the script carries on, having no `set -e`). `grade_sims_nrf45b.sh:31-32` passes relative names. **Affected:** none: section 115's bars are not yet recorded, and the files of `G` today are the `_b` outputs. Test: the argument with an absolute Windows path, expected the bed resolved.

**D12. Graders run from another session's worktree.** `grade_n46.sh:34`, `grade_sims_nrf45.sh:54` and `grade_d24.sh` change directory into IRDU to run the repository's two-run audit; 8 scripts put IRDU's `scripts/` on their path (part 1, section 1.2). Same content today. Test: run with IRDU absent, expected success.

**D13. Hard-coded run roots and mixed roots.** Eleven scripts hard-code `runs_nom` (part 1, section 1.2) while their callers export `RAPID_RUNS_ROOT`; `user_fix_vs_float_free.py` checks one root (line 23) and reads the other (through `SPC/o1_emulate.py:31`). A finished run moved to `D:\GnssProcessorSupport\runs\` cannot be regraded without a junction. Test: a fixture tree under another root, expected every tool to read it.

**D14. No exit code is read.** No `grade_*.sh` sets `-e` or reads `$?` (0 of 49); `grade_c3.sh:19-27` pipes into `tail -N`, which hides a failure; `REPO/nrf_two_run_audit.py:333` returns 1 on any failure, unread. `grade_arm_net.sh:37` reuses a non-empty control table, a traceback included. Test: an orchestrator whose child fails, expected a failed grade.

**D15. Shared scratch files in the output folder.** `gf_code_signature.py:49-52, 70-74` write `G/gfsig_pf.awk`, `gfsig_rf.awk`, `gfsig_tree.awk` under fixed names, so two concurrent grades overwrite each other's program (identical text today); `arc_signature_spread.py:30` writes a new `G/arcsig_<n>.awk` per hash seed.

### 4.3 Duplication and scripts named nowhere

**D16. Copies.** Measured with a line-set and sequence comparison over S20 and SPC (section 7): `SPC/user_fix_vs_float.py` and `user_fix_vs_float_full.py` (0.95, the full-day copy); `SPC/ref_changes.py` and `ref_changes_full.py` (0.82), whose CLFRAC reader is the same text as `user_fix_vs_float_full.py:44-53` (`ref_changes_full.py:21-30`); `grade_bar6.sh`, `grade_bar6_s2.sh`, `grade_bar6_n41.sh` (one script, three labels; `grade_bar6_n41.sh:2-3` still names `s1x` and line 4 `grade_bar6.sh`); the five `derive_*_users.py` (0.84 to 0.96; `derive_users_for.py` of 2026-11-18 is the parameterised one); `hksl_trace.py` and `hksl_trace_code.py` (0.98); `e03_trace.py` and `e14_trace.py` (0.94; the second's docstring still names E03); `band_split_census.py` and `band_split_census2.py`; `frame_free_count.py` and `decision_cycles.py` (0.77: the truth reader, the tree, the arcs and the cycle walk); `grade_guard_sim.py` and `grade_entry_sim.py` (`cover`, `status`, `runend`); `settled_graph.py` and `cq_tree_cert.py`. The products reader (`E` and `P` records) is written out in at least eight scripts, the held share by hour in five.

**D17. Scripts named nowhere.** 35 files written before 2026-11-18 are named in no section of card NRF, no hand-over note and no other script: `chain_cg_validate.sh`, `chain_cg_validate2.sh`, `chords_modes.py`, `cq_trace5.py`, `derive_n41_xusers.py`, `e14_trace.py`, `e1e5a_check.py`, `grade_after_chains.sh`, `grade_bar6_n41.sh`, `grade_d146_full.sh`, `grade_kc.sh`, `grade_l12.sh`, `grade_o1.sh`, `grade_o2.sh`, `grade_o2r.sh`, `grade_o9.sh`, `launch_o3o4.sh`, `make_mode1_arms.sh`, `make_o2_arms.py`, `make_o2r_arms.py`, `make_o3o4_arms.py`, `make_o5_arms.py`, `n2c_level.py`, `n2c_read.py`, `n2c_stream.py`, `o1_int_changes.py`, `o1_raw_classify.py`, `prep_exact_epoch_time_adoption.py`, `ring_bands.py`, `sat_level_series.py`, `sat_resid_by_sys.py`, `self_audit.py`, `trace_vs_truth.py`, `users_chain_n41.sh`, `users_chain_s2.sh`. Some produced recorded numbers under a name the card does not give (`grade_bar6_n41.sh` for section 112.2's users, `chords_modes.py` for 107.2's chord read through its outputs `chords_modes_s1_n14*.txt`); the stage-O ones depend on TMP2.

## 5. The plan

Each step moves one bar's owner into the repository, under `scripts/grading/` (a home for the bars, beside the existing `scripts/nrf_two_run_audit.py`), with a self-test on a small frozen fixture, and is gated by reproducing numbers already recorded in card NRF for named runs exactly, in a legacy view where the new owner corrects a defect (the correction is a second, announced view). Nothing of the plan is carried out in this task.

**Frozen fixtures.** Two hours from the first epoch of `d24_n146`, `n46c_n146`, `n46cr_n146` (`products.txt`, `network_ring_fix.csv`, `network_chain_edges.csv`, `network_events.csv`, `network_raim.csv`, `network_epoch_status.csv`), two hours of the users `_rap_nw_obs{n46,a24}{x,f}_146_{solo,reyk}` (`stdout.txt` CLFRAC lines, `consumer_replay_hourly.csv`), and the first 120 epochs of `m1_c` with its truth files; kept beside the gate folder with their sha256 recorded in the step's commit, plus a synthetic fixture of a few rows per defect (D1 to D9) in the repository.

| step | owner moved into the repository | retires | reproduces exactly |
| --- | --- | --- | --- |
| G1 | `scripts/grading/runs.py`: one run-root rule (`RAPID_RUNS_ROOT`, a bare tag or a path), readers of the products records, the ring-fix and chain-edge traces and the events by header name, the CLFRAC lines (stdout first), epoch times rounded once | the copies of the products reader, `clfrac_sources` twice, every hard-coded root | `G/single_d24_n146.txt` byte for byte through G2's legacy view |
| G2 | `network_hourly.py` (B1, B2, B3, B8): held share by hour per family and per receiver mode, families read from the trace (no table), RAIM rows by system, lost satellite-samples by both rules, unrounded with a rounded view; an arm-against-control form | `single_hourly.py`, `d24_hourly.py`, `c4_compare.py` (first, as a file of 2026-11-18), `held_by_family.py`, `gps_share.py` | `G/single_{d24,d24r,n46c,n46cr}_n146.txt`, `single_{d24,n46c}_n147.txt` in the rounded view; `_irdu_grades/nrf_d24_hourly_d24_n146.txt`'s held, RAIM and epoch rows; this card's `held_share_exact_c4.txt` in the exact view |
| G3 | the two-run audit stays `scripts/nrf_two_run_audit.py`, called from the repository by every orchestrator; `aud_windows.py` folded in as a window option | the IRDU calls (D12) | `G/audit_n46c_n146.txt` and `G/audit_d24_n146.txt` line for line (the first reproduced here) |
| G4 | `slip_hold.py` (B7, B7b): the flag list as a table, the signal kept in the key, `lli_slip` events counted as flags of the link and reported apart, windows in epochs from the run's rate; the injected-slip truth | `slip_hold_check.py`, `slip_hold_spans.py`, `grade_slip_bar.py`, `injected_slips.py` | `G/slip_hold_n46c_n146_all.txt` and the two others (3 695, 281, 0; 3 695, 291, 0; 3 709, 381, 0) in the legacy view; this card's `slip_hold_lli_c4.txt` in the new; `G/c3_n46.txt`'s slip lines (400, 208, 0) |
| G5 | `users.py` (B9 to B11): one float check (both lanes) with two counts (every hold; holds the network knows), the paired difference, the difference of medians, reference changes with their sums; `o1_emulate.pins` and `o2_audit.held` moved in | `user_fix_vs_float_full.py` and `_free.py` (and the TMP import), `paired_users.py`, `bar6_receivers.py`, `ref_changes_full.py`, `SPC/users_hourly2.py`, `SPC/user_fix_vs_float.py`, `SPC/ref_changes.py`, `wl_off_list.py` | `G/n46users_holds_*_{146,147}.txt`, `n46users_paired_{146,147}.txt` (reproduced here), `n46users_refch_*.txt` in the legacy view; this card's `free_rule_on_pilot_*` in the new |
| G6 | `compare_runs.py` (B12): whole-file identity first; records set aside only as the run's own legend declares them (`# phase families`), member values compared to their identities; traces present in one run reported; wall-time columns named in one table | `compare_runs.py`, the awk of `grade_n46.sh:24-30`, `check_member_records.py`, `grade_l12_parts.py identity` | H1 (`u2_z45`, `u2_z`: identical), C2 (`G/c2_n46c_n146_vs_s2_n146.txt` and its two siblings; `members_n46c_*`: 0 unequal), F1 |
| G7 | `truth.py` (B5, B6): the run-end line parsed by kind (truth audit, audit file); the frame-free cover with the seed-0 order kept as the legacy view and a sorted tie-break as the new; the cache keyed by tag, bed, stride and the products' sha256 | `grade_guard_sim.py`, `grade_entry_sim.py` (their shared parts), `frame_free_count.py`, the shell `truth()` helpers | `G/c3_n46.txt`'s truth lines (404 958 right, 0 wrong); the frame-free sums of section 108.1's table under `PYTHONHASHSEED=0`; the sorted view stated once on the same runs before it is used |
| G8 | `code_signature.py` (B13): the three tools on one reader, tree links counted as 107.2 defines, no cache or a cache keyed by every argument and the inputs' sha256, no awk programs | `gf_code_signature.py`, `arc_signature_spread.py`, `mode_code_trace.py` | `G/gfrep_s2_n146.txt`, `gfrep_n41_n14{6,7}.txt` and section 108.3's figures |
| G9 | one orchestrator per bar group, from the `grade_arm_*.sh` family of 2026-11-18 (already parameterised by arm label): fails on a child's failure, writes a manifest per grade (script commit, arguments, inputs' sizes and times) | the 49 `grade_*.sh` (kept frozen beside the outputs they made), `scratch_run.py`, `run_dirs_fill.py`, `SPC/run_dirs.py` and `nw_arms.py` (moved with the user-run launcher, a tool of their own) | the outputs of G2 to G8, run through it |
| G10 | none | the 35 files named nowhere (D17) and the copies of D16, after a check that no orchestrator kept calls them; TMP and TMP2 imports gone | nothing new: a listing that no kept file names a retired one |

The order puts the bars the author's next decision rests on first (G2, G4, G5, the bars of section 118.1 and of the comparison of the two-tree form) after the reader (G1), and the simulator bars after them. The files added on 2026-11-18 (`c4_compare.py`, `user3d_by_hour.py`, `release_followup.py`, `derive_users_for.py`, the `grade_arm_*.sh` family) are each step's first candidates: they are the newest owners of their bars and already take the arm's label as an argument.

## 6. What the plan enables

- A native signal family: G6 sets records aside by the run's own declaration, so a member that keeps its own name is compared, not dropped; G2 and G4 read families from the trace.
- BeiDou and further constellations: G2 and G5 take constellations and frequencies from one table (the project's `common/GnssIdentity.hpp` traits mirrored in Python), where today GPS and Galileo are hard-coded (A1 to A4, A10, A16 to A19).
- A third frequency: one reference per signal (A15), every held band's slips as flags (A5), held shares per pair family.
- A streamed network at 1 s: windows in epochs from the run's rate (A29), no 30 s constants.
- Reruns after a run moves to `D:\GnssProcessorSupport\runs\` or after the temporary folders are cleaned (D10, D13).

## 7. The reproductions of this card

Outputs in `D:\GnssProcessorSupport\gates\grader_audit_2026-11-18\`, every script run with `py -3.14 -B` (no bytecode written beside it), inputs read from `R`.

| what | command (abridged) | output | result |
| --- | --- | --- | --- |
| H1 by whole file | `cmp` of `products.txt` and of every `logs/*.csv` of `u2_z` against `u2_z45`; the records set aside counted by awk | (console) | products byte-identical; 3 of 42 traces differ (`network_datum_graph.csv`, `network_run_summary.csv`, `network_service_health.csv`); set aside: B C1X 17 634, B C5X 17 634 |
| other off pairs | `cmp` of `products.txt` | (console) | `m1_off45`/`m1_off37`, `h40_off45`/`h40_off`, `t3t45_sim`/`t3t_sim` byte-identical |
| C4 GPS unrounded | `held_share_exact.py <arm> <control> combined_mode_28.txt` | `held_share_exact_c4.txt` | D1's figures |
| bar 7 with `lli_slip` | `slip_hold_lli.py <ref> <tested>` on a byte copy of `slip_hold_check.py` (md5 `40e126eb`) | `slip_hold_lli_c4.txt` | the script's counts equal 118.1's; dropped flags 159, 159, 259; 0 held through |
| the two-run audit | `REPO/nrf_two_run_audit.py n46c_n146 n46cr_n146 --json` (repository at `bcf3bfe`) | `audit_n46c_n146_repro.{txt,json}` | every line after the header equal to `G/audit_n46c_n146.txt` (90 s) |
| the inside float check | `user_fix_vs_float_full.py _rap_n46c_n146 --fix n46x --flt n46f --pair "E L1C/L5Q" --hours 24 --per-site --list 40` (`NW_DOY=146`) | `full_repro_EL1CL5Q_146.txt` | identical to `G/n46users_holds_EL1CL5Q_146.txt` (100 625 held, 0 wrong, REYK wide lane off 222) |
| the both-lanes rule on the pilot-type users | `user_fix_vs_float_free.py --fix <l>x --flt <l>f --doy <D> --sites solo,...,wroc --pair "E L1C/L5Q"` for `n46`, `a24`, `s1` on DOY 146 and 147 | `free_rule_on_pilot_*.txt` | D2's figures; 0 wrong in all six |
| the paired difference | `paired_users.py n46{x,f} a24{x,f} --doy {146,147} --sites <pilot>,<outside 11>` | `paired_repro_{146,147}.txt` | identical to `G/n46users_paired_{146,147}.txt` |
| fractional pins | awk over `target_cyc` of `network_chain_edges.csv` | (console) | 0 fractional of 975 686 (`n46c_n146`, `n46cr_n146`) and 1 138 243 (`d24_n146`) |
| `lli_slip` events | awk over `network_events.csv` of `d24_n146` | (console) | 273 Galileo `lli_slip` refloat events: "LLI" 132, "LLI;mw_step" 122, "LLI;gf_step" 19 |
| cover caches | modification times of `G/cover_seed0_*.jsonl` against the runs' `products.txt` | (console) | all 21 caches newer than their runs |
| copies | `near_dups.py S20 SPC` | `near_dups.txt` | D16's pairs |

## Results

### 2026-11-18: plan steps G1 and G2 (the shared reader; the hourly network bars)

**The go.** The author, 2026-11-18 at about 15:00, to "Start the grading-script cleanup now, as new tested copies in the repository?": "Yes, first four steps" (G1, G2, G4, G5; relayed by the managing session). G3 and G6 to G10 are not started. The older scripts stay in `scripts_s20`, untouched; a step "retires" a script only in the sense that its new owner exists and reproduces it.

**What was built** (branch `audit/graders`): `83ab1f6` (G1) and `7f2a39b` (G2), under `scripts/grading/`.
- `runs.py` (G1): one rule each for the run folder (a bare tag under `RAPID_RUNS_ROOT`, a `_rap_` name or a path), the epoch key (whole seconds, for `processing.exact_epoch_time`), the hour, the first epoch, the stream's epochs, a trace by header or by position, a name list, and a user run's CLFRAC lines (`stdout.txt` first, else the rotated resolver logs). `fixture.py`: the synthetic fixtures and the frozen-fixture check. `README.md`: each bar, its script, how it runs, its two views.
- `network_hourly.py` (G2; bars B1, B2, B3, B8): `table` (views `legacy` = `single_hourly.py`, `d24` = the held, RAIM, epoch and withheld rows of `d24_hourly.py`, `exact`), `compare` (views `legacy` = `c4_compare.py` as of section 118.1, `exact` = the worst hour and the mean from unrounded shares, defect D1), `families` (= `held_by_family.py`). The exact table adds a row per family found in the trace, the RAIM rows by constellation and the lost samples by both rules (band 1 flagged; both signals flagged, section 96.1).
- The self-tests follow the repository's convention: `--selftest`, a `selftest()` printing PASS or FAIL and setting the exit code, as in `scripts/nrf_two_run_audit.py:267-284` (63 scripts of `scripts/` have one). Their synthetic fixtures are JSON files materialised in a temporary folder, because the repository ignores every `logs/` folder and `*.log` file (`.gitignore:80` and the `*.log` rule). The frozen real-run fixtures are `D:\GnssProcessorSupport\gates\grader_fixtures_2026-11-18\` (the first two hours of `d24_n146`, `d24r_n146`, `n46c_n146`, `n46cr_n146` and of the users `_rap_nw_obs{n46,a24}{x,f}_146_{solo,reyk}`; 44 files, 163 MB; `MANIFEST.sha256` sha256 `936ae5de676441875ddefa41040e55d4d0ca556b763cf2dc923381f2494da1fd`), made by `gates/grader_audit_2026-11-18/freeze_fixtures.py`; `frozen.json` holds six commands of `network_hourly.py` and their output sha256. All self-tests pass.

**The gate** (outputs in `D:\GnssProcessorSupport\gates\grader_audit_2026-11-18\g2\`, script `gate_g2.sh`):
- `table`, legacy view: all 16 recorded `G/single_*.txt` byte-identical (`d24`, `d24r`, `e1`, `e24`, `e24r`, `n41`, `n41r`, `n46c`, `n46cr`, `s1`, `s2`, `s2r` on DOY 146 and 147). This is G1's gate too.
- `table`, d24 view: the seven rows of `_irdu_grades/nrf_d24_hourly_d24_n14{3,5,6,7}.txt` byte-identical in a binary comparison. The gate's first text comparison reported a difference that was its own: its pattern for the epochs row also caught the row "epochs at the cap".
- `compare`, legacy view: byte-identical to `c4_compare.py` as of section 118.1 on `n46c_n146`, `n46cr_n146` and `n46c_n147`. That script was replaced in `scripts_s20` at 14:29 by the second session's unrounded version; the original was rebuilt from this audit's full read of it (`c4_compare_v1_reconstructed.py`, 2 814 bytes, the original's size) and reproduces every C4 figure of section 118.1 (GPS -3 at hour 4 with mean -0.23, -1 at hour 4 with +0.05, -1 at hour 24 with -0.05; RAIM rows 16, 20, 19 against 21, 18, 17; lost samples 8, 4, 7 against 495, 431, 415).
- `families`: `held_by_family.py`'s three blocks of `G/sims116_bylife_b.txt` (`u2_z45` and `u2_zl`, `m1_off45` and `m1_l`, `h40_off45` and `h40_l`) byte-identical.
- Not covered: `gps_share.py` (held and decided counted held, section 51) and `gf_code_signature.py`'s per link-hour share stay as they are.

**The cross-check** (the managing session's request: two independent implementations of the corrected rule). The exact `compare` view against the second session's unrounded `c4_compare.py` on section 118.1's three runs: every figure equal, for the GPS row and the three Galileo rows (the worst difference, its hour, both shares, the mean and both mean shares; GPS -2.34 at hour 4, -0.51 at hour 19, -0.44 at hour 12), and the RAIM totals equal (`xcheck/c4_crosscheck.txt`).

### 2026-11-18: plan steps G4 and G5 (links held through a slip; the users' bars)

**What was built** (branch `audit/graders`): `129070f` (G4) and `4956fe9` (G5), under `scripts/grading/`, on `runs.py`.
- `slip_hold.py` (G4; bars B7, B7b): `flagged` (views `legacy` = `slip_hold_check.py`, its printout and its `--out` file; `lli` = the reference run's `lli_slip` refloat events counted as flags of the link's E1/E5a chord when no other flag has that station, satellite and epoch, reported apart, with the windows set from the tested run's epoch interval: defect D3 and assumption A29), `injected` (= `grade_slip_bar.py`), `truth` (= `injected_slips.py`).
- `users.py` (G5; bars B9, B10, B11): `inside` (legacy = `user_fix_vs_float_full.py`; `--sites` = `user_fix_vs_float_full_sites.py`), `outside` (= `user_fix_vs_float_free.py`), `both` (the corrected check of defect D2: every hold judged on both lanes against the float user, at any receiver, with the holds known to the network counted beside), `paired` (= `paired_users.py`), `hourly` (= `users_hourly2.py`), `medians` (legacy = `bar6_receivers.py`, which reads the hourly medians to 0.1 mm from the printed matrix; exact = unrounded), `refch` (= `ref_changes_full.py`, with sums). The network's pins and holds that `user_fix_vs_float_full.py` imported from the temporary scratchpad (`SPC/o1_emulate.py:35-43`, `SPC/o2_audit.py:28-50`) are part of `users.py`; the day is always given (`--doy`), where the older scripts fell back to `NW_DOY` or 144 (defect D8).
- Self-tests as in G1 and G2. G4's three frozen cases use the G1 window. The users hold few integers in the first two hours, so six of G5's seven frozen cases use a second window of the same runs, hours 6 to 8 of DOY 146 (`D:\GnssProcessorSupport\gates\grader_fixtures_2026-11-18_h6\`: `d24_n146`, `n46c_n146` and the eight users; 30 files, 85 MB; `MANIFEST.sha256` sha256 `0d20b2aadc8aaaad01366871fc3c676d9557d691cb4bbe139e0b1a87164e0f9c`); a frozen case names its window (`fixture.py`). All self-tests pass.

**The gates.** G4 (`g4/`, `gate_g4.sh`), all byte-identical: the printouts and `--out` files of `slip_hold_check.py` for `n46c_n146`, `n46cr_n146`, `n46c_n147`, `s2_n146`, `s2r_n146`, `s2_n147`, `s1_n146`, `s1r_n146` and `s1_n147` (18 files), the three `s1` runs restricted to the 28 combined-mode receivers, the C3 slip-bed lines of `G/c3_n46.txt` (binary comparison; the gate's first comparison passed the recorded lines through `sed`, which changed their line endings), and `G/injected_f40_s_on.jsonl` rebuilt from `f40_s_on` against the base `m1_off37` (3 277 steps). G5 (`g5/`, `g5x/`, `gate_g5.sh`, `gate_g5x.sh`), 32 outputs byte-identical: `inside` for `n46`, `s1` and `a24` on both pairs and both days (`n46users_holds_*`, `bar6_holds_*`), `outside` for `n46` on both pairs (`n46users_holds_out_*`), `paired` (`n46users_paired_*`), the bar 6 matrix (`bar6_hourly_*`), the difference of medians against `bar6_receivers.py` run on the recorded matrix, reference changes (`n46users_refch_*`), and `inside --sites` against the eight outputs of `user_fix_vs_float_full_sites.py` (`n46users_full_*`).

**The receiver set of the inside check** (the second session's finding, covered here). `user_fix_vs_float_full.py` counts every network receiver with both user runs of the labels (`user_fix_vs_float_full.py:115-120`): 38 for today's streams and 10 for an arm's users, so `G/bar6_holds_a24_GL1CL2W_146.txt` holds 403 009 GPS pair-epochs over 38 receivers where the ten pilot-type receivers hold 110 567. The legacy view keeps that set (it reproduces the recorded files); `--sites` gives the comparison over one list.

**The cross-checks** (two independent implementations of each corrected rule).
- Bar 7: the `lli` view against the second session's `slip_hold_check2.py` on section 118.1's three runs: equal (the `lli_slip` events not already a flag 159, 159 and 259; released 35, 32 and 41; held through 0; `xcheck/slip_hold_check2_*.txt`).
- The users: `both` against the second session's regraded users of section 118 (`_irdu_grades/reads_2026-11-18/n46users_holds_summary_{146,147}.txt`): 16 of 16 rows equal in held, right, wrong and unverified, by receiver group (pilot-type, combined-mode outside), pair and label, on both days; the holds within the network's coverage of the pilot-type rows equal too (100 625, 31 323, 110 104, 110 567 on DOY 146; 102 139, 42 631, 113 760, 112 257 on DOY 147; `xcheck/users_crosscheck.txt`). The only wrong holds are the three short runs of section 118.1 at the combined-mode receivers outside the network (33 and 14 on DOY 146, 16 on DOY 147) and the 34 of today's stream on DOY 146 (the UTQI run).

**Not covered by steps G1 to G5.** `gps_share.py`, `slip_hold_spans.py` (a read of section 107.9), `wl_off_list.py` (the users' wide lane against the network's, section 107.3's near-null column) and the other reads of `scripts_s20` stay as they are. G3 and G6 to G10 wait for a decision.

### 2026-11-19: bar F4 by link, F5 for GPS, the integrity of held links, the users' runs

**The order.** The managing session, 2026-11-19, package `tooling-graders-remaining` of the day program
(`D:\GnssProcessorSupport\gates\day_program_2026-11-19\program.json`, with the scout package `held-integrity-graders`
merged into it): bar F4 counted by distinct link; the long one-signed holds counted for every day run, with a view by
resumption and a view against the simulator's truth; F5 for GPS; the users' runs of wrong holds split by receiver type;
each new grader gated on numbers already recorded. Plan step G3 is done in the form the package asks for (a second
implementation of the two-run audit, where section 5 had it stay as it is). Steps G6 to G10 still wait for the author's
go (listed at the end of this entry).

**What was built** (branch `audit/graders`, commits `010ae0a` and `fcb2902`, under `scripts/grading/`).
- `slip_hold.py` (F5). Every view of `flagged` takes `--sys E|G` (`slip_hold.py:51`). `--view check3`
  (`slip_hold.py:248`) is `slip_hold_check3.py`, the five-day read's copy, whose GPS rule counts a flag on any L1 or L2
  signal against the L1C/L2W chord (`scripts_s20/slip_hold_check3.py:151`). `--view chord` (`slip_hold.py:285`) is the
  five-day read's ruling: a flag counts against a held link only when it names a signal of that link's own chord, read
  from the tested run's ring-fix row before the flag (`slip_hold.py:270`); flags on other signals only, and the
  `lli_slip` events at an epoch with no flag of the chord, are counted on lines of their own.
- `two_run.py` (F4, new). The legacy view is `scripts/nrf_two_run_audit.py` without its wind-up options, its walk
  written again (`two_run.py:85`) in the original's order (common links sorted, `nrf_two_run_audit.py:181`; roots in
  that order, `:189`). The `bylink` view (`two_run.py:137`, `:171`) takes, at each epoch and band where the walk fails,
  every four-link loop of every link; a link failing more than half of at least three of its loops disagrees (the rule
  of the five-day read's `two_run_loops.py`); each failing check of the walk is attributed to the disagreeing link of
  its loop, as the closing link or on the tree path (`two_run.py:118`), and the closing links of checks with none are
  listed. Where the walk passes, every loop closes, so no other epoch can hold a disagreeing link. The self-test shows
  the walk's dependence on its tree: one disagreeing link fails 9 of the 9 checks of a 4 by 4 grid when it is a tree
  edge, 1 of 9 when it is not; the loop rule names it in both.
- `held_integrity.py` (new, no pandas). A hold is a run of held rows of one link with constant integers, rows without a
  residual dropped, a gap over 120 s ending it (`held_integrity.py:141`), as in the managing session's three scripts
  (`window_monitor_replay.py:30`, `decision_split.py:29`, `onesigned_scan.py:15`). Residuals are kept in units of
  0.01 mm, the trace's resolution (`held_integrity.py:58`), so every threshold test is exact. `onesigned`: legacy =
  `decision_split.py`'s printout; `--view exact` = per constellation the holds of an hour or more and the signature at
  8, 10 and 12 mm, the holds with the signature listed by start (the population of `onesigned_scan.py`). `window`
  (`held_integrity.py:278`, `:315`): `window_monitor_replay.py`'s table, `--view exact` with the counts per run.
  `resume` (`held_integrity.py:377`): every T0_gap arc_resume (rows of one epoch, station and satellite taken together)
  that meets a link held at that epoch, matched by either chord signal as the scout's `resume_held.py:12-14` matches
  it, by class, with the residual at the epoch, the held rows that followed with the same integers and the first
  release or move to the tree. `truth` (`held_integrity.py:445`): holds graded by their rows' audit column, from 00:30:15
  of the day, by `--rule any` (wrong when any row is wrong) or `--rule majority` (the class of more than half the rows,
  the scout's rule), with residual statistics, the signature and the window rule against truth.
- `users.py`. `outside --view free2` is `user_fix_vs_float_free2.py` (every run of wrong holds, a gap over 60 s starting
  a new one; `users.py:235`, `:360`). `both --runs` (`users.py:412`) lists the same runs on the corrected check, each with
  its receiver's type, in or outside the station list of the network run named by `--net` or `--type-from`
  (`users.py:388`, arm.json only), with runs and epochs by type.
- Frozen cases. Ten cases on `D:\GnssProcessorSupport\gates\grader_fixtures_2026-11-19\` (windows of `ad_n143`,
  `ad_n146`, `adr_n146` and the two UTQI users of DOY 146; 16 files, 180 MB, `MANIFEST.sha256` sha256
  `df0f4eeef7110a30ddc07949f441717bc9f0ff645ab4179392a4c96a4091a07e`), made by
  `gates/grader_audit_2026-11-18/freeze_fixtures_1119.py` and `make_frozen_1119.py`. `fixture.py` takes a case's
  folder (`root`), masks the folder's path in a printout that names runs by full path (`mask_root`) and accepts a
  declared exit code (the two-run audit exits 1 on a failing check, as the original does). Every self-test passes with its frozen cases (26: 6 of `network_hourly.py`, 5 of
  `slip_hold.py`, 2 of `two_run.py`, 4 of `held_integrity.py`, 9 of `users.py`; `selftests_1119.txt`,
  `selftest_users_1119.txt`).

**The gates** (outputs and scripts in `D:\GnssProcessorSupport\gates\grader_audit_2026-11-18\`: folders `f5/`, `f4/`,
`held/`, `uruns/`; scripts `gate_f5.sh`, `gate_f4.sh`, `gate_held.sh`, `gate_uruns.sh`).

| bar | recorded number and its source | here | agreement |
| --- | --- | --- | --- |
| F5, the checker copy | `slip_hold_self_<run>_{E,G}.{txt,jsonl}` of `ad_n143` to `ad_n147`, `adr_n146` (GPS counts 7, 0, 2, 2, 3, 3; Galileo 0) | `flagged --view check3` | all 24 files byte-identical |
| F5, GPS by the chord's signals | the five-day read's verdict row: 1, 0, 0, 0, 0, 0, BOGT00COL G09 on DOY 143 | `flagged --view chord --sys G` | equal: 1 (BOGT00COL G09 at 572730, arc_resume shifted_fresh on L1C and L2W), then 0 on every run |
| F5, Galileo by the chord's signals | 0 on every run | `flagged --view chord` | equal: 0 |
| F4, the walk's count | `audit_ad_n146`, `audit_n46c_n146`, `audit_d24_n146` (`.txt`, `.json`): 1 502 failing checks on E1 and on E5a for the adopted pair | `two_run.py` legacy | all 6 files byte-identical |
| F4, by distinct link | the five-day read's `attrib_two_run_loops_base.txt`: GUAM00GUM-E29 458 epochs, LMMF00MTQ-E06 379 epochs; the attribution of 2026-11-19, section 3 | `two_run.py --view bylink` | the same two links, both link lines identical; every one of the 1 502 failing checks per band on a loop through one of them (693 as the closing link, 809 on the tree path, 0 on neither) |
| one-signed holds | `decision_split.py`'s printout (`nrf_reads_2026-11-18/manager_decision_split.txt`): 31 795 holds of an hour or more, 42 with the signature, eight runs | `onesigned` legacy | byte-identical (all lines but the one naming the script's CSV file) |
| one-signed holds at 10 mm | the ten `manager_onesigned__rap_<run>.csv` of `onesigned_scan.py` | `onesigned --view exact --mm 10` | the same 35 holds, means within 2e-18 m |
| windowed monitor replay | `manager_window_monitor_replay.txt`: 15 rows, 40 min and 8 mm catching 42 of 42 for 182 other trips (about 23 a run) | `window` | 14 of 15 rows equal; at 10 min and 4 mm 14 075 other trips against 14 074 (the tie below) |
| resumptions meeting a held link | the scout's census (`scouts/held.json`): 179 over the six adopted runs, 128 clean_fresh, 46 clean_bridged, 4 unmeas_bridged, 1 shifted_fresh; residuals 27.36 m (POTS00DEU G13, DOY 144), 5.04 m (DOY 145), 0.137 and 0.010 m (DOY 147) | `resume` | equal in every count and residual |
| wrong holds on the simulator | the scout's: 86, 40 and 14 wrong held segments on `h40_off`, `d1_off`, `d31_off`, none with the signature; wrong rows' residual median 3.1 to 3.2 mm against 3.3 mm; the 40 min, 8 mm window trips 1 of 140 wrong and 5 of 4 580 right | `truth --rule majority` | equal: 86, 40, 14 wrong, 1 510, 1 515, 1 555 right (one hold of `h40_off` mixed), none with the signature; medians 3.16, 3.09, 3.23 mm against 3.30, 3.34, 3.33 mm; trips 1 of 140 and 5 of 4 580 |
| the users' runs of wrong holds | `five_days_free_<group>_<pair>_<doy>.txt`, 20 files (the seven runs of the five-day read) | `outside --view free2`; `both --runs --type-from ad_n<doy>` | all 20 files byte-identical, run listings included; by type, every one of the seven runs is at a receiver outside the network's station list (UTQI, CHTI, WUH2, ASCG), and the ten pilot-type receivers, all in the list, hold none |

**What the gates add to the record.**
- *F5 for GPS.* The checker copy's GPS rule counts third signals: of its 17 holds through a flag, 16 are gaps of L2S or
  L2L, signals of no chord (the five-day read's finding, reproduced). The chord view also lists the gaps of GPS L5
  (L5Q, at PALM00ATA, KIT300UZB and SOLO00SLB), which the copy's L1/L2 rule never reads: 7, 10, 4, 9, 7 and 11 holds
  through such a flag on DOY 143, 144, 145, 146, 146 (second root) and 147. Galileo's flags off the chord are all E6
  and E5 AltBOC signals (L6B, L6C, L6X, L7Q with L8Q, L8Q, L8X): 133 to 177 holds through one a run, the moves into the
  tree included.
- *F4.* The two DOY 146 pairs before the adopted one disagree on no Galileo link, as recorded; on GPS the by-link view
  names MBAR00UGA G04 (107 epochs) and G07 (3 epochs) in all three pairs, MBAR00UGA G14 (3 epochs) and GUAM00GUM G08
  (16 and 41 epochs) in the two earlier ones, GUAM00GUM G04 (110 epochs) and ULAB00MNG G04 (6 epochs) in `d24`. The
  loop rule names no link when several links of one receiver disagree together: on `d24` 218 failing GPS checks per
  band are listed by closing link as unattributed (216 at GUAM00GUM with G04 and G06 to G22, 2 at MAC100AUS G03), and
  15 on `n46c` (MAC100AUS G03 14, MBAR00UGA G03 1); they are not counted as distinct links.
- *The window replay's one difference.* Five holds reach a 20-row mean of exactly 4.00 mm and never more
  (`held/window_ties.txt`, `window_ties.py`); pandas' rolling mean returns 0.004 for four of them and
  0.003999999999999999 for WTZS00DEU E12 of `n46c_n146`, which the replay therefore does not trip. The exact count is
  14 075. No other row moves; the 42 of 42 and the 182 other trips at 40 min and 8 mm stand.
- *The resumptions.* The five shifted or unmeasured holds sat at 0.010 to 27.4 m at the resumption epoch (KIT300UZB G29
  on DOY 147 at -0.010 m, 0.033 m the next epoch) and were held 1, 1, 2, 2 and 4 rows before a release by slip,
  refloat or the monitor (BOGT00COL G09, after 120 s).
- *The users' receiver set (defect D4).* `both --runs` states the receivers it counts, by type: on every day and pair
  the ten pilot-type receivers are all in the station list of the day's network run and the eleven combined-mode
  receivers all outside it, none missing or empty; the seven runs of wrong holds are all at receivers outside it.
- *The simulator.* The two truth rules differ by half: by any wrong row, 226, 59 and 27 holds are wrong; by the
  majority of rows, 86, 40 and 14. Under both, no wrong hold has the signature, and the 40 min, 8 mm window trips one
  wrong hold (on `d1_off`, after 215.5 min) and five right ones.

**Not covered.**
- `two_run.py` carries none of the audit's wind-up options (`--windup`, `--windup-trace`, `--born-after`), which serve
  the simulator and resumed runs; `scripts/nrf_two_run_audit.py` stays their owner.
- The truth view does not split wrong holds by frame class (frame-type, isolated, other shape): that needs the
  frame-free cover of card NRF section 108.4 (plan step G7), which the package `held-signature-evidence` is extending
  (`packages/held-signature-evidence/scratch/sim-power-by-class/ffc_ext.py`); a planted wrong link's catch and delay
  waits for a planted bed (none exists yet).
- The resume view follows the scout's rule (a held row at the resumption's epoch). The package `held-resume-release`
  writes its own rules (held when entering the epoch, folded epochs from the event state machine; its `rules.md`,
  amendment A1), which count more cases (169 pairs on `ad_n143` alone); its table is not reproduced here.
- The legacy view of `window` is the table with exact trips, not pandas' arithmetic; it equals the recorded table
  except for the one tie above.

**Plan steps G6 to G10, waiting for the go** (estimates; no engine run in any of them):
- G6 `compare_runs.py` (whole-file identity, records set aside only as the run's own legend declares, traces present in
  one run reported): reads four scripts; gate H1, C2 and F1 of section 5; about 0.25 M tokens.
- G7 `truth.py` (the run-end truth line by kind, the frame-free cover with the seed-0 order as the legacy view and a
  sorted tie-break beside it, a cache keyed by tag, bed, stride and the products' sha256): gate `G/c3_n46.txt`'s truth
  lines and section 108.1's sums under `PYTHONHASHSEED=0`; it also gives the truth view its frame classes; about 0.45 M.
- G8 `code_signature.py` (three tools on one reader): gate `gfrep_s2_n146.txt`, `gfrep_n41_n14{6,7}.txt`, section 108.3;
  about 0.3 M.
- G9 one orchestrator per bar group (fails on a child's failure, a manifest per grade): gate the outputs of G2 to G8 run
  through it; about 0.35 M.
- G10 the retire listing (no kept file names a retired one): about 0.1 M.
