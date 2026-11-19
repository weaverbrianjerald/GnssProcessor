# NRF part 17: one pair family per configured pair (sections 121 onwards of card NRF)

**STATUS**

- **id:** NRF-part17
- **topic:** network-vehicle
- **status:** open
- **summary:** Sections 121 onwards of card NRF (2026-11-18), continued from [part 16](NRF_network_ring_fixing_2026-11-14_part16.md). Section 121 pre-registers one pair family per configured pair (`network.phase_datum_eq34_family_pairs`, card IRD's step IRD-3 with the two Galileo families kept separate), built on the author's word of 2026-11-18: the arm that tells the two-band tree apart from the single network of section 118 on that section's bars, in two stages (a rapid stage R of 2 h beds and one short observed run, then stage A, the adoption comparison). Section 121.1 is its build and 121.2 fixes stage R's observed run. Also here: the simulator results of sections 115 to 117 graded by the second session (115.1, 116.2, 117.1) and two notes of the grader audit on sections 107.3 and 118.1 (107.12, 118.3). Section 122 pre-registers the entry check on the keep-carry path (card CHA step S8 (b)).
- **source:** the manager's build order of 2026-11-18 12:58 and its amendment of 14:05 (the author's word on run length); [part 16](NRF_network_ring_fixing_2026-11-14_part16.md), sections 118.1, 118.2 and 120; [IRD](IRD_integer_recovering_datum_2026-10-13.md) section 11 (IRD-3); [IRDC](IRDC_integer_recovering_datum_events_2026-11-02.md) section 5; [CHA part 2](CHA_chain_audit_2026-11-18_part2.md) (the note of milestone B on the pair family).

**Outcome.** Pre-registered, not yet run. In the adopted configuration the chain elects Galileo per signal (four phase signals while the two Galileo families stay separate), and section 120 ties the lost satellites to those per-signal trees; section 118's single network removed them but changed the network as well. The key of section 121 forms one pair tree per configured Galileo family with the families kept separate, so that the two effects separate on the same bars. The adoption of either form stays the author's (section 118.2).

### 121. 2026-11-18: one pair family per configured pair (card IRD's step IRD-3 with the two Galileo families kept separate; pre-registered)

**Why.** Section 118's runs change two things against today's runs at once: one tree over both bands (with the table, Galileo tracks two phase signals, so `formChainFamilies` forms its pair family) and one network for both signal types. Section 120 ties the lost satellites to the per-signal trees that the separate families impose on Galileo, which tracks four phase signals there (card CHA, part 2, the note of milestone B). This arm keeps the two families separate and gives each its pair tree, so the two effects are told apart on the same bars before long runs are made on either form. The author's word (2026-11-18, about 12:50): build the two-band tree per Galileo signal type as a switch; the adoption of the single network is held until this form is graded (section 118.2).

**The key.** `network.phase_datum_eq34_family_pairs`: an array of `"<constellation>:<signal>/<signal>"` entries (the form of `network.ring_fix.pairs`), default empty, which is off and byte-identical. With `network.phase_datum_eq34_family` "pair", a constellation that tracks more than two phase signals in an epoch forms one pair family per listed entry whose two signals it tracks, its bands in signal-string order, in place of the per-signal fallback; a signal in no listed entry is elected per signal, and its constellation-epoch counts as a fallback (`eq34_pair_fallbacks`). A constellation that tracks exactly two signals keeps today's rule. The loader refuses an entry that is malformed, that names two signals of one band, or that names a signal another entry names; it warns that the list is INERT without the pair family (no chain, family "signal", or the pin bridge, which gives the pair family up). A run-start line names the list as armed; a line names each constellation's families whenever they change, with its per-signal fallback and the signals that caused it (today only a run-end counter says it); the run-end block counts the epochs of each family. The ledger row is UNVALIDATED. The key is not fingerprinted, like the family key and the other election knobs: it chooses which tree the next election grows, and a frame step that a new tree puts on a carry goes through the same re-reference and shift responses as any other re-host.

**Where the pairs come from.** The chain's own list, not `network.ring_fix.pairs`: the chain runs without the ring fix (a configuration with the ring fix off keeps its tree shape, and step S2 of card CHA names every ring-fix key set while the ring fix is off as INERT), and the two lists answer different questions (which signals share one tree; which chords the ring fix decides). A run-start line says when the two lists differ while the ring fix is armed. The arms list the same Galileo pairs in both: `"network.phase_datum_eq34_family_pairs": ["E:L1C/L5Q", "E:L1X/L5X"]`.

**Checked in the build** (its section reports what was found): every place that might assume at most one pair family per constellation: the family's remembered datum receiver, the root votes, the chord table and `form_configured_pairs`, `pairConsidered`, the emitter's families, the settled sets and the run-end block.

**Exe and controls.** The key's exe is frozen under a name given in the hand-off. The controls are today's runs (`_rap_d24_n146`, `_rap_d24r_n146`, `_rap_d24_n147`, exe nrf32) and the 2 h references of the base engine (`_rap_chaR_*`, engine cha2 at `72b7b3b`), tied to the key's exe by milestone B's whole-day identity run (card CHA, part 2: the branch reproduces `_rap_d24_n147` over 24 h) and by T1.

**Bars** (as the manager gave them, staged as amended at 14:05).
- T1. Off identical: the gate of card CHA section 5.1 for an identical-output step, as amended: the 2 h controls n147, f40sc and h40 with `nom_td_gate.py --whole --mask-time` (products, every trace and the logs identical), the chain unit cases and the two lints.
- T2. Structure, IRD-3's gate per Galileo pair family: identical band-1 and band-2 tree edge sets every epoch, pins per band equal to the gauge count, pin audit COMPLETE, mismatch 0, every edge-set difference attributed to a band-only arc; no satellite with its two band edges at different receivers inside a family other than through such an arc. Its unit part (oracle cases with two configured pair families on one constellation, and the formation rule) passes before the hand-off; its data part is read on the runs of stage R.

**Stage R, rapid.**
- T3 at 2 h: the arms of m1, h40 and d1 with only the key, against the base engine's 2 h references of those beds (`_rap_chaR_m1`, `_rap_chaR_h40`, `_rap_chaR_d1`), graded from 00:30:15. The clean two-mode bed (m1): 0 wrong held link-epochs by the truth audit, held link-epochs not below 99 % of the control's, every epoch emitted, the held-out user by hour within max(+10 %, +5 mm) of the control. The code-error beds (h40, d1): wrong held link-epochs and the frame-free count (fixed hash seed) not above the control's.
- One short observed run of DOY 147 from 00:00, control and arm on the key's exe, its length set by the second session from today's day run as the shortest that shows the lost class after hour 2 (expected 3 to 6 h; the length is reported before the run), with six users (three of each signal type) evaluated from hour 2. Bars: T2 on the data, every epoch emitted, no user hold wrong by the float check, and the lost-class samples (unflagged satellites with at least 3 unheld converged links, every 5 min from hour 2) at most one half of the control's.

**Stage A, the adoption comparison**, once and only if stage R passes.
- T3's three beds at 6 h, on the same bars, against `m1_off37`, `h40_off` and `d1_off`.
- T4. Observed: the configurations of `d24_n146`, `d24r_n146` and `d24_n147` with only the key, 24 h; `entry_revalidate` off as in section 118. Section 118's bars C4 as written there, against today's runs: GPS held share, RAIM rows, the two-run audit of DOY 146 with 0 failing Galileo checks, bar 7, and the users of section 118 (the same 21 receivers, fixing and float, both days): no hold wrong by the float check, the paired per-receiver difference not worse by more than 1 mm in any hour from 3.
- T5. The question: lost satellite-samples (band-1 tree satellites not flagged, every 30 min) at most one half of today's in each run (today 495, 431 and 415; section 118.1's 8, 4 and 7 beside them). Reported beside it, no bar: the Galileo held share by family and hour, the users' Galileo held pair-epochs and reference changes, against today's and section 118.1's.

**Who runs what.** The build, T1, T2's unit part and a 10-epoch smoke of the on arm: the chain audit's session, before the hand-off of the frozen exe. The arms of stages R and A, their runs and their grading: the second session ("Runs and reads") on that exe; its result file is appended to this card by the chain audit's session.

**Reading prepared.** Whatever the result, no adoption by any session: the form goes to the author on one table beside today's runs and section 118.1's single network, and the author chooses between the forms (section 118.2).

### 121.1 2026-11-18: the build (commit `91f36cf`, engine `gnss_processor_cha_fp1.exe`)

**Built.** Commit `91f36cf` on `chain/audit` (after step S6 of card CHA, `0783ad1`, and the test fix `bcf3bfe`), the key as pre-registered.
- Loader: the four refusals (a malformed entry, an unknown constellation, two signals of one band, a signal that two entries name) and the INERT warnings (family "signal" or no chain; the pin bridge); the run-start line "eq34 FAMILY PAIRS armed [...]"; a line naming the entries that `network.ring_fix.pairs` lacks while the ring fix is armed.
- Election: `chainFamiliesOf` gives one constellation's families (today's rule for two signals, the configured pairs for more than two, per signal otherwise) through `chainConfiguredFamilies`; with the list empty, the families, their order and the counters are today's. With the list armed, the families are counted one by one (`eq34_family_epochs`), each constellation's families are named in the log when they change (at most 40 lines), and a run-end line counts them (a warning when no configured pair formed).
- The knob `eq34_family_pairs`, by constellation, is empty unless the pair family is on (so under the pin bridge too); not fingerprinted (the exemption comment names it). Ledger row UNVALIDATED.

**Checked for an assumption of one pair family per constellation; none found.**
- The family's remembered datum receiver is kept per band (`ConstellationState::channels[signal].datum_rx`), read band by band and written for each band of the family.
- The root votes run over the family's own bands' satellites.
- The chord table and `form_configured_pairs` form families per arc (`chainArcFamilies`: each arc's own signals, then the configured families).
- `pairConsidered` and the settled sets are keyed by the family's text (`"E:L1C/L5Q"`); today's configurations already list both Galileo families.
- The emitter's families (unsettled steps, step classes) are configured lists.
- The run-end block counted the pair families only in total; the per-family counts are new.
- One effect beside them: with the key, the observatory records Galileo's groups in family order (L1C, L5Q, L1X, L5X) instead of signal order; each group's own rows keep their order.

**Unit cases** (Release; 313 chain and loader cases pass).
- The formation rule on a hand-built Galileo epoch: four signals and two pairs, a pair without its partner, one pair listed, the list empty, two signals; each against the per-signal election, with one datum receiver and a spanning tree per band, and a pair's two bands pinned on the same edges.
- The oracle on a two-mode Galileo world whose arcs carry both bands of their mode (640 epochs, 610 with both modes): the configured pairs equal the per-signal election's outputs and its record group by group, every group at full rank, no fallback; 1 865 pair families against 610 per-signal fallbacks without the list; 11 278 Galileo tree edges pinned on both bands of a mode.
- The oracle with band-only arcs (640 epochs, 1 879 pair family-epochs): every group at full rank, an arc that both bands of a configured pair carry pinned on both or on neither, and the band extension's work equal to the arcs' own count over every pair family formed; 2 024 extension edges, 29 band-only components.
- Four loader refusals and the armed and INERT lines.
- Found while writing the oracle: with the shift off, a step's zero or not decides a re-reference, so the test world's perturbation must give a Galileo mode's two bands one carry value and one previous target; drawn apart, the per-signal election itself diverges between a mode's two bands.

**T1** (key off, engine `gnss_processor_fp1.exe` of the build, against the base engine's references `_rap_chaR_h40`, `_rap_chaR_f40sc` and `_rap_cha_ctl2_n147`): pass. Products and every trace are identical to the references on the three controls (exit 0; the logs differ only in the two lines that steps S1 and S2 of card CHA announced), and the key's off runs are identical in full to step S6's runs of the same controls (`_rap_cs6_*`, logs included).

**The on arm's smoke** (`_rap_fp_on_smoke`: the 2 h observed control's configuration of DOY 147 with only the key, 9 epochs): exit 0. Galileo formed both configured pair families at every epoch (`E:L1C/L5Q` 9, `E:L1X/L5X` 9, GPS's `G:L1C/L2W` 9: 27 pair families, 0 fallbacks, 9 extension edges); the run-start, change and run-end lines read as specified.

**Frozen exe for stages R and A:** `D:/GnssProcessorSupport/gates/exe/x64-Release/gnss_processor_cha_fp1.exe`, sha256[:16] `da240107c091aa8c` (the build of `91f36cf`; the folder's DLLs are identical to the build's).

### 121.2 2026-11-18: stage R's observed run fixed, and what the beds can show (the manager, before any graded run)

**The observed run of stage R** replaces the short DOY 147 run of section 121: DOY 146 from 00:00 for 5 h, the key off and on, both on the key's exe. Today's run reaches 53 lost-class samples after hour 2 by 5 h on DOY 146, against 23 on DOY 147 (the second session's `lost_class_by_hour.txt`). Validity rule: if the key-off control shows fewer than 30 lost-class samples after hour 2, both runs are extended to 7 h before any bar is read. Six users of both signal types on that day, evaluated over hours 2 to 5, their holds counted by the users' own rule (every hold, both lanes required; section 118.3). Held shares are compared unrounded, to two decimals. The bars of stage R stand as written in section 121.

**What the beds can show.** The simulator beds' per-signal trees already coincide edge for edge (section 120, addendum), so T3 shows the key's mechanics and no harm there; the benefit bar rests on the observed run.

### 115.1 2026-11-18: the release form graded: S1, S2 and S4 pass; S3 fails on d31 (143 more wrong held link-epochs, the frame-free count unchanged)

**Runs.** Exe nrf45 (`3c61a5747aab11ce`, checked), 6 h from 00:30:15 on `mode1_r1`'s configuration, each arm its guard arm of section 108.1 with `network.ring_fix.nn_guard_release` true (`config_diff_vs_base.txt`). Restarted after the reboot with `bash scripts_s20/runs_nrf45_sim.sh 3` at 12:27, three engines at a time, each set to BelowNormal priority right after its launch (`scripts_s20/belownormal_nrf45.ps1`, log `_irdu_grades/belownormal_nrf45.log`): `h40_off45`, `m1_nr`, `d1_nr` ended at 13:22, `d31_nr`, `h1_nr`, `h40_nr` at 13:46; exit 0 and 720 epochs emitted in each. `h40_off45`, `m1_nr` and `d1_nr` held partial output from before the reboot and no exit code; they ran again from the start: `rapid_arm.py run` runs every tag it is given (no test of an existing folder), `ev_launch.ps1` removes the exit code before the launch, the engine opens its traces and its stream truncated, and after the restart each trace held one header line and one row at the first epoch, with `products.txt` at 37 KB against 3.8 MB before.

**Grading.** `bash scripts_s20/grade_sims_nrf45b.sh 115`, output `_irdu_grades/grades_family_if/sims115_release_b.txt`. The copy differs from the prepared `grade_sims_nrf45.sh` in three points: the truth beds are passed to `grade_guard_sim.py` by name (the prepared script passes absolute paths, which the grader splits at the drive's colon, stopping it at S2); S1 is also compared whole-file with `cmp` (`compare_runs.py` sets the L1X/L5X and C1X/C5X records aside, which in these runs are the X/X family's own); every release is followed to the run's end (`release_followup.py`, new). Frame-free counts with `PYTHONHASHSEED=0` over the 72 sampled epochs; the guard-off controls' counts come from the cached covers of section 108.4.

**Bars.**
- **S1 PASS.** `h40_off45` against `h40_off` (exe nrf37): `products.txt`, `network_ring_fix.csv`, `network_chain_edges.csv` and `network_events.csv` identical byte for byte; of the other traces only wall-time columns differ (`network_datum_graph.csv` last column, `network_run_summary.csv`, `network_service_health.csv`).
- **S2 PASS.** `m1_nr`: 112 135 held chord-epochs checked, none flagged (212 refused for the ionosphere-free deviation), no release; 390 546 held link-epochs right and 0 wrong, as `m1_off37`.
- **S3 FAIL on d31; met on d1, h1 and h40.**

| bed | releases (truth at the release: right, wrong) | wrong held link-epochs, release / off | frame-free count C/Q and X/X, release / off | wrong decisions, release / off | epochs emitted |
|---|---|---|---|---|---|
| d1 | 4 (3, 1) | 10 298 / 10 358 | 0 and 584 / 0 and 590 | 36 / 35 | 720 |
| d31 | 6 (6, 0) | **3 714 / 3 571** | 275 and 0 / 275 and 0 | 13 / 13 | 720 |
| h1 | 0 | 0 / 0 | 0 and 0 / 0 and 0 | 0 / 0 | 720 |
| h40 | 1 (1, 0) | 22 100 / 22 100 | 52 and 23 / 52 and 23 | 131 / 131 | 720 |

d31's wrong held link-epochs are 143 above the guard-off run's (+4.0 %). The frame-free count is not above the guard-off run's on any bed, and every epoch is emitted.
- **S4 PASS.** Galileo held link-epochs against the guard-off run, C/Q and X/X: m1 1.0000 and 1.0000; d1 0.9991 and 0.9980; d31 0.9949 and 0.9997; h1 1.0000 and 1.0000; h40 1.0000 and 0.9996 (bar 0.98). GPS 1.0000 on every bed.

**Every released chord against the truth** (`release_followup.py`; hours from the run's first epoch; the deviations are the guard's centred code signature at the flag; "after" counts the chord's held link-epochs from the release to the run's end, the guard-off run's over the same span beside them):

| bed | h | chord | family | truth and pair at the release | k, GF, IF (m) | next held | held after, right and wrong (guard off) |
|---|---|---|---|---|---|---|---|
| d1 | 1.675 | S11000SIM–E11 | X/X | right (30, −76) | −1, −0.336, −0.249 | 30.5 min later, same pair, right | 57, 0 (117, 0) |
| d1 | 1.783 | S55000SIM–E14 | X/X | wrong (−94, 8) | −1, −0.334, −0.289 | 30.5 min later, same pair, wrong | 0, 157 (0, 217) |
| d1 | 4.625 | S36000SIM–E19 | C/Q | right (32, −27) | +1, +0.330, +0.282 | 16.0 min later, same pair, right | 26, 0 (57, 0) |
| d1 | 5.250 | S18000SIM–E27 | C/Q | right (−48, −100) | −1, −0.336, −0.204 | 15.5 min later, same pair, right | 59, 0 (89, 0) |
| d31 | 3.867 | S07000SIM–E12 | X/X | right (−59, 45) | +1, +0.340, +0.296 | not before the end | 0, 0 (22, 0) |
| d31 | 4.108 | S54000SIM–E12 | C/Q | right (−49, 72) | −1, −0.368, −0.299 | not before the end | 0, 0 (200, 0) |
| d31 | 4.117 | S52000SIM–E13 | C/Q | right (−118, −19) | +1, +0.344, +0.184 | 30.5 min later, same pair, right | 81, 0 (141, 0) |
| d31 | 4.350 | S52000SIM–E07 | C/Q | right (−62, −82) | +1, +0.374, +0.294 | 15.5 min later, same pair, right | 167, 0 (197, 0) |
| d31 | 5.275 | S52000SIM–E33 | C/Q | right (−3, −94) | −1, −0.334, −0.290 | 15.5 min later, same pair, right | 56, 0 (86, 0) |
| d31 | 5.475 | S26000SIM–E07 | C/Q | right (9, −101) | +1, +0.348, +0.293 | 19.0 min later, same pair, right | 11, 0 (12, 0) |
| h40 | 3.958 | S13000SIM–E34 | X/X | right (−6, −29) | +1, +0.334, +0.294 | 16.5 min later, same pair, right | 68, 0 (100, 0) |

**The d31 excess** (the wrong held link-epochs per link of both runs from `network_ring_fix.csv`, `_irdu_grades/reads_2026-11-18/wrong_by_link_d31_off.txt` and `wrong_by_link_d31_nr.txt`). The same 27 links carry every wrong held link-epoch in both runs. The 143 more are three chords at S02000SIM, E14 (+60), E34 (+80) and E09 (+3), wrong by the truth audit in both runs and held from 5.175, 5.117 and 5.933 h in the release run against 5.675, 5.783 and 5.958 h without the guard, each to the run's end. With the frame-free count equal (275 and 0), the excess is the same wrong frame held longer on three chords, not a link moved off its integer. Why those three were taken up earlier is not traced (the six releases from 3.87 to 5.48 h change the held set and the order of the later decisions).

**Reading.** The release form moves no integer: its 11 releases on the four code-error beds took 10 chords the truth held right and 1 (d1, S55000SIM–E14) held one near-null step off; every chord held again came back on its pair 15 to 31 min later, the wrong one wrong again, and two d31 chords were not held again before the run's end. On these beds it therefore removes no wrong integer, costs held link-epochs of right chords (S4's largest cost 0.51 %, d31 C/Q), and shifts the timing of later decisions, which on d31 lengthens three wrong-framed holds by 143 link-epochs. The flags sat at the lower edge of the step band (|GF| 0.330 to 0.374 m, |IF| 0.184 to 0.299 m), the signature of section 108.1's false moves: constant code offsets of right links. By the bars as registered, S3 fails on d31, so the observed arms of section 115 are not run. Not run or not checked: the observed arms; whether the release shortens a real step's hold where the flagged chord is wrong for longer than one re-decision (d1's one wrong chord was decided again onto the same wrong pair).

### 116.2 2026-11-18: held first by life on the two other beds: H1 passes; H3 passes on the clean bed and is met on the 40 % bed except its X/X held link-epochs, 4.8 % above the control

**Runs.** Exe nrf45 (`3c61a5747aab11ce`), 6 h from 00:30:15, in the restart of `bash scripts_s20/runs_nrf45_sim.sh 3` (BelowNormal priority, `belownormal_nrf45.ps1`): `m1_off45`, `m1_l` and `h40_l` ended at 14:07, exit 0, 720 epochs each; the 40 % bed's control `h40_off45` ended at 13:22 (section 115.1). `m1_l` and `h40_l` are `m1_off45` and `h40_off45` with `network.ring_fix.held_first_by_life` true.

**Grading.** `bash scripts_s20/grade_sims_nrf45b.sh 116`, output `_irdu_grades/grades_family_if/sims116_bylife_b.txt` (the copy of `grade_sims_nrf45.sh` described in section 115.1; for section 116 it adds the whole-file `cmp` of the off check). Beside the bars: the truth audit's wrong held link-epochs by family (`network_ring_fix.csv`, held rows by their audit column) and the frame-free count (`PYTHONHASHSEED=0 py -3.14 grade_guard_sim.py h40_off:h40_l:simb_mode1h40 m1_off37:m1_l:simb_mode1`, output `_irdu_grades/reads_2026-11-18/s116_frame_free.txt`; `h40_off` stands for `h40_off45`, identical to it by S1 of section 115.1). H2 passed in section 116.1 and is not graded again; the copy's H2 lines reproduce 116.1's counts.

**Bars.**
- **H1 PASS** (the part left from section 116.1). `m1_off45` against `m1_off37` (exe nrf37): `products.txt`, `network_ring_fix.csv`, `network_chain_edges.csv` and `network_events.csv` identical byte for byte; the other traces differ only in wall-time columns (`network_datum_graph.csv` last column, `network_run_summary.csv`, `network_service_health.csv`). The 40 % bed's control is tied to its nrf37 run by section 115.1's S1.
- **H3.**

| bed | wrong held link-epochs, by life / control (C/Q, X/X) | wrong decisions | Galileo held link-epochs, C/Q and X/X, ratio to the control | GPS held link-epochs, ratio | epochs emitted | frame-free count C/Q and X/X |
|---|---|---|---|---|---|---|
| clean (`m1_l`) | 0 / 0 | 0 / 0 | 1.0000 and 1.0011 | 0.9963 | 720 | 0 and 0 / 0 and 0 |
| 40 % (`h40_l`) | 18 689 / 22 100 (17 168 and 1 521 / 21 871 and 229) | 112 / 131 | 0.9997 and **1.0481** | 0.9939 | 720 | 52 and 23 / 52 and 23 |

On the clean bed every part of H3 is met: **PASS**. On the 40 % bed the wrong held link-epochs are below the control's (−3 411), GPS and the C/Q family are within 1 %, and every epoch is emitted; the X/X family holds 82 401 link-epochs against 78 622, 4.81 % above the control, outside "within 1 %" as written. **Not met as written** on that one number. The bar's heading is "no harm", and a held share above the control's is not a harm in the bar's other terms; whether a gain beyond 1 % fails the bar is left to the managing session, and it is reported here as written.

**Beside the bars.** On the 40 % bed the change moves wrong held link-epochs between the families: C/Q 21 871 to 17 168, X/X 229 to 1 521 (6.6 times), with the frame-free count equal in both families (52 and 23), so the same number of links stand a cycle off and the audit's counts move with which chords are held through them; that the extra X/X ones pass through seed 40's X/X outlier (S45000SIM–E02, section 103.3) is consistent with the equal count and is not checked. GPS held link-epochs fall by 0.4 % (clean) and 0.6 % (40 %), as on section 116.1's bed (0.53 %): the rule changes GPS's held-first choices too.

**Reading.** On the clean bed held first by life costs nothing and holds no wrong integer; on the 40 % bed it holds fewer wrong link-epochs in all, more held X/X link-epochs and more of them wrong, with the frame-free count unchanged. With H2 passed on its bed (116.1) and H1 passing, H3 is met everywhere except the X/X gain beyond 1 % on the 40 % bed. Section 116 prepared one-change observed arms only if H1 to H3 pass; none is made or run here (the managing session decides on the X/X reading first). Not checked: which chords carry the extra X/X wrong link-epochs on the 40 % bed.

### 117.1 2026-11-18: the extra-band delay noise on the simulator: Q1 and Q2 pass (no integer changes; the DOY 144 question, Q3, not run)

**Runs.** Exe nrf45 (`3c61a5747aab11ce`), 3 h from 00:30:15, `t3t_sim`'s configuration from its two datum receivers with the cascades held and truth: the controls `t3t45_sim` and `t3t45_sim_r` and the arms `t3q_sim` and `t3q_sim_r` (`network.rx_extra_band_delay_q` 1e-5 m²/s, `config_diff_vs_base.txt`), the last four of `bash scripts_s20/runs_nrf45_sim.sh 3` (BelowNormal priority, `belownormal_nrf45.ps1`), ended between 14:23 and 14:35; exit 0 and 360 epochs emitted in each.

**Grading.** `bash scripts_s20/grade_sims_nrf45b.sh 117`, outputs `_irdu_grades/grades_family_if/sims117_ebdq_b.txt`, `audit_t3t45_b.txt` (`.json`) and `audit_t3q_b.txt` (`.json`). The copy of `grade_sims_nrf45.sh` (section 115.1) adds for section 117 the whole-file `cmp` of the off check and every family's truth counts, pairs and cascades (the prepared script printed the run-end total only).

**Bars.**
- **Q1 PASS** (the simulator half). `t3t45_sim` against `t3t_sim` (exe nrf31): `products.txt`, `network_ring_fix.csv`, `network_chain_edges.csv` and `network_events.csv` identical byte for byte, so every integer is reproduced; the other traces differ only in wall-time columns (`network_datum_graph.csv` last column, `network_run_summary.csv`, `network_service_health.csv`). The observed half (`k1t45_n144` reproducing `k1t_n144`'s two-run disagreement) belongs to the DOY 144 arms and is not run.
- **Q2 PASS.** The truth audit holds 0 wrong held link-epochs and 0 wrong decisions in every run, in the pairs (run-end totals: `t3t45_sim` 177 145 right, `t3q_sim` 177 169, `t3t45_sim_r` 173 199, `t3q_sim_r` 173 151) and in each cascade (G:L1C/L2W>L5Q 79 344, 79 364, 83 417 and 80 184 right; E:L1C/L5Q>L7Q 67 299, 67 304, 62 958 and 62 964 right), as the controls. The pairs' held link-epochs against the controls: E:L1C/L5Q 1.0001 and 1.0001, G:L1C/L2W 1.0002 and 0.9994 (bar: within 1 %).

**Beside the bars.**
- The cascades' held link-epochs: E:L5Q/L7Q 1.0001 and 1.0001; G:L2W/L5Q 1.0003 from the first datum receiver and **0.9612** from the second (80 184 against 83 417; hours 2 and 3: 35 810 and 34 063 against 38 083 and 34 933), none wrong. Why the GPS cascade from the second receiver holds 3.9 % less is not traced.
- The two-run audit of each pair of roots (states held, decided and would-hold): controls 0 failing of 450 247 checks, arms 0 of 448 114, on every signal (E L1C, L5Q, L7Q; G L1C, L2W, L5Q).

**Reading.** On the simulator, freeing the receivers' extra-band delays to wander at 1e-5 m²/s changes no integer: the controls reproduce the earlier run byte for byte, the arms hold the same integers right with the pairs' held link-epochs within 0.06 %, and the two roots agree on every check with and without the noise. The simulator carries no drifting extra-band delay, so it shows only that the noise does no harm there; whether it removes the two clusters of disagreement on DOY 144 (section 79.4) is Q3, which needs the four DOY 144 arms and is not part of this run. Not run: `k1q_n144`, `k1qr_n144`, `k1t45_n144`, `k1r45_n144`.

### 107.12 2026-11-18: section 107.3's users' held pair-epochs by the users' own count (the grader audit)

Section 107.3's held pair-epochs of the pilot-type users (Galileo L1C/L5Q 91 296 and 90 711, today's 31 323 and 42 631) come from `user_fix_vs_float_full.py`, which counts a user's held pair only where the network holds an integer for both satellites at that receiver and epoch, so the count grows with the network's coverage. By the users' own count (the rule of `user_fix_vs_float_free.py`: every user hold, both lanes required), section 107.3's run holds 95 858 against today's 59 908 Galileo pair-epochs on DOY 146 (1.60 times) and 93 910 against 73 002 on DOY 147 (1.29 times). No hold is wrong by either rule. Source: the grader audit's `free_rule_on_pilot_s1_EL1CL5Q_146.txt`, `_147.txt` and `free_rule_on_pilot_a24_EL1CL5Q_146.txt`, `_147.txt` in `D:\GnssProcessorSupport\gates\grader_audit_2026-11-18\`; the manager read the two scripts at the cited lines.

### 118.3 2026-11-18: two notes on section 118.1 from the grader audit

**C4's GPS clause on unrounded shares.** `n46c_n146` against `d24_n146` is worst −2.34 points at hour 4 (86.19 against 88.54 %), mean −0.13 (86.66 against 86.79 %); the −3 and −0.23 recorded in section 118.1 came from whole-percent printouts. `n46cr_n146` against `d24r_n146`: worst −0.51 (hour 19), mean −0.04; `n46c_n147` against `d24_n147`: worst −0.44 (hour 12), mean −0.03. The verdict is unchanged. Source: `held_share_exact_c4.txt` in the grader audit's folder.

**The pilot-type users' held pair-epochs.** Section 118.1's Galileo counts (100 625 on DOY 146 and 102 139 on DOY 147, beside today's 31 323 and 42 631) come from `user_fix_vs_float_full.py`, which counts a hold only where the network holds an integer for both satellites at that receiver and epoch. By the users' own count (the rule of `user_fix_vs_float_free.py`: every user hold, both lanes required) the codes-shared form holds 101 673 against today's 59 908 on DOY 146 (1.70 times) and 103 787 against 73 002 on DOY 147 (1.42 times). No hold is wrong by either rule. Source: `free_rule_on_pilot_EL1CL5Q_146.txt`, `free_rule_on_pilot_n46_EL1CL5Q_147.txt` and the two `free_rule_on_pilot_a24_EL1CL5Q_*.txt` in the same folder.

### 122. 2026-11-18: the entry check on the keep-carry path (card CHA step S8 (b); pre-registered)

**Why.** A chain re-host kept on a held integer (`network.ring_fix.rehost_keep_carry`) enters the tree as a known step without the entry check (card CHA, part 2, P1): the path of section 103, by which a held link one near-null step off enters the tree at its held integers and the satellite's level follows it. The cut's entry check (`network.ring_fix.entry_revalidate`, section 111) did not reach it. Step S8 (a) of card CHA gave a held entry's step one owner (`chainEntryStep`, commit `8d71321`, byte-identical on the three 2 h controls and on the control with the entry check on); step S8 (b) makes keep-carry ask the cut's rule when `entry_revalidate` is on.

**The build** (its commit is named in section 122.1). With `entry_revalidate`, keep-carry asks the verdict on evidence gathered before the election: for each held link, this epoch's gap resumptions of the slip layer (final after PASS A: whether its arc came back across a gap, and whether the gap re-entry test measured and found shifted its own held pair) and its near-null signature from the ring fix's state at the epoch's start (the last epoch's code residuals; the cut reads this epoch's, after the solve). A refused keep is released as without keep-carry. Counters `keep_checked`, `keep_known` and `keep_refused_*`, a run-end line, and RINGFIX `keep_refused` events. With `entry_revalidate` off the build is byte-identical. No new key: `entry_revalidate` (RECOMMEND_SCOPED) widens its reach, and its ledger row says so.

**The control with the entry check on** joins the control set of every later step: n147e, the 2 h observed control of DOY 147 with `entry_revalidate` on (reference `_rap_chaR_n147e` on the base engine cha2; S8 (a) reproduced it, products and every trace).

**Bars.**
- B1. Off identical: the three 2 h controls (n147, f40sc, h40) identical in full to step S8 (a)'s runs (`nom_td_gate.py --whole --mask-time`); the chain unit cases and the two lints.
- B2, stage R: the truth audit and the frame-free count (section 108.4, `PYTHONHASHSEED=0`) on the beds m1 (the clean two-mode bed), d1 and d31 (the code-error beds), h1 and h40 (the bracket beds), 2 h from 00:30:15, each the bed's configuration with only `entry_revalidate` on, on the S8 (b) engine against the same arm on the S8 (a) engine. On every bed: wrong held link-epochs not above the control's (0 on m1), the frame-free count not above the control's, every epoch emitted, and Galileo and GPS held link-epochs not below 98 % of the control's (the bar S4 of section 115). Reported beside it: the keeps checked and refused, by cause, and every refused keep against the truth.
- B2, stage A, once and only if stage R passes: the same beds and bars at 6 h.
- B3. Observed: n147e on the S8 (b) engine against the S8 (a) engine: every epoch emitted; the keeps checked and refused reported, each refused keep with its link and satellite; the products identical where no keep is refused. No bar beyond these (section 111.1 found no cut entry refused on observed data; a refused keep there is a reading).

**Who runs what.** The build, B1, the unit cases and the smoke: the chain audit's session. The arms of B2 and B3, their runs and their grading: the second session, on the frozen exes named in the report "S8 (b) built" (the S8 (b) engine and the S8 (a) engine as its control).

**Reading prepared.** B1 to B3 met: the extension stays under `entry_revalidate`'s scoped recommendation, and the ledger row records the result. A bar of B2 missed: the keep-carry path goes back to taking a usable link as known (the extension reverted), and the author decides.
