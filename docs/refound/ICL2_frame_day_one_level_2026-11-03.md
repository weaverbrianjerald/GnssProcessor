# ICL2: the phase-visible frame over a day, and one level for code and phase

**STATUS**

- **id:** ICL2
- **topic:** network-vehicle
- **status:** closed
- **summary:** Pre-registered and closed 2026-11-03. (1) Over 24 h (DOY 129 and 132) the phase-visible frame keeps receivers whose GPS-to-Galileo offset is frozen equal to free-offset receivers (paired +0.0009 and +0.0019 m); one marked leash re-anchor (GPS, after 23.4 h, harmless); one unmarked 0.403 m step when the clock anchor decoupled 18 of 22 Galileo satellites (D2 fails on DOY 129; D1, D3, D4 pass). (2) The frame voting in the phase-bias hold's level (`network.phase_frame_hold_level_vote`, default off, chosen offline over the clock frame hold voting on C + U) brings each constellation's code and phase to within 6 to 9 mm per 120 epochs of one level (from 20 to 25), an exact no-op on the golden, receivers unchanged; the GPS-to-Galileo level then follows the code side (19.0 mm on DOY 129, from 9.4). K2 and K3 fail on their bars. Nothing armed.
- **source:** card [ICL](ICL_inter_constellation_level_2026-11-03.md) section 7 (the two items); card [BRD](BRD_broadcast_referenced_level_2026-11-02.md) (the posture and its knobs); card [SMH](SMH_smoothed_hold_clone_2026-09-29.md) (the smoothed hold); card [LAH](LAH_level_aware_hold_2026-09-29.md) (the phase-clock hold); `docs/SPEC_clock_emit_rebase.md` (the clock frame hold)
- **knobs / posture:** new, default off: `network.phase_frame_hold_level_vote` (UNVALIDATED, parent `network.emit_phase_frame_hold`; section 5.2); existing, read: `network.emit_phase_frame_hold`, `network.phase_frame_leash_m` (card ICL), `network.emit_clock_frame_hold`, `network.apriori_clock_track`, `network.cycle_slip.if_phase_td_sat_centre`; graded on card BRD's GPS + Galileo broadcast posture with both of its knobs and the frame (card ICL's `icl_p{129,132}_6`), receivers as the golden configures them
- **dates in the text:** 2026-11-03
- **hashes / run dirs:** branch `icl2/frame-day-one-level` from `icl/inter-constellation-level` (`7026d4b`, unmerged, not pushed), worktree `D:/GnssProcessorSupport/worktrees/icl2`; item 1 exe `gnss_processor_icl2d.exe` (census `gnss_processor_icl2c.exe`), copies of card ICL's `gnss_processor_icl2.exe` (sha256[:16] `6eaa68964d3ff620`) in `D:/GnssProcessorSupport/worktrees/icl/build/x64-Release`; item 2 exe `gnss_processor_icl2k1.exe` (`3d0b60a606c380c7`), Release from this worktree; arms `D:/GnssProcessorSupport/runs_nom/_rap_icl2_*` (`icl2_{p,c}{129,132}_24` item 1, `icl2_k{129,132}_{sm,2,6}`, `icl2_{c129,s,p129,cv129}_2` item 2); census replays `_rap_<key><doy>_<site>`, keys `i2p`, `i2c`, `i2pI` (item 1, 24 h), `i2k` (item 2, 6 h); scripts in `D:/GnssProcessorSupport/worktrees/icl2_tools/` (outside the repository)
- **gates (frozen at pre-registration):** section 4

**Outcome.** Over 24 h the phase-visible frame keeps receivers whose GPS-to-Galileo offset is frozen where free-offset receivers are (paired +0.001 and +0.002 m on DOY 129 and 132). It crossed the 1 m leash once (marked, harmless) and stepped 0.40 m unmarked when the clock anchor decoupled 18 of 22 Galileo satellites (D2 fails on DOY 129). Each constellation's code and phase levels drifted apart by 20 to 25 mm per hour; the frame voting in the phase-bias hold's level (default off) cuts that to 6 to 9 mm, an exact no-op on the golden, at a cost on DOY 129's GPS-to-Galileo level (9 to 19 mm). Nothing armed.

> Results are appended TO THIS FILE, below the text.

---

## 1. The questions

Card ICL (section 7) closed with the phase-visible frame (`network.emit_phase_frame_hold`) cutting the GPS-to-Galileo
phase-visible level of card BRD's GPS + Galileo broadcast posture from 188 and 193 to 9 and 32 mm per 120 epochs (DOY 129 and
132, 6 h), and two items owed:

1. **The frame over a day.** The frame's |G| reached 0.49 m in 6 h and moved 0.1 to 0.2 m per hour while the tracked pivots
   walked; the leash (`network.phase_frame_leash_m`, 1.0 m) re-anchors a constellation to the solve frame with a datum mark,
   which a receiver answers by re-inflating its GPS-to-Galileo offset. Does the frame hold the level over 24 h, how often does
   it re-anchor, and what does a re-anchor cost a receiver whose offset is frozen?
2. **One level for code and phase.** A receiver's clock and inter-system offset serve code and phase alike. The clock frame
   hold (`network.emit_clock_frame_hold`) votes on the emitted clock C, the correction against the tracked a priori a + U, so
   the code-visible level follows the median of the tracked a priori increments; the phase frame votes on C - P_IF + U, the
   phase-visible correction against the raw broadcast a priori a, so the phase-visible level follows the median of the raw
   increments. Two candidates make the two sides follow one level: (a) the clock frame hold voting on C + U; (b) the phase
   frame voting on the phase biases' motion that does not come with the satellite's clock. Which predicts better, and does
   the built one deliver its prediction?

## 2. Measured before the gates (2026-11-03)

On card ICL's and card BRD's 6 h runs, hours 2 to 6, against the CODE final clock, with `icl2_tools/icl2_status.py` (built on
`icl_tools/icl_level.py`; it reproduces card ICL's code-visible and phase-visible D to 0.1 mm). Besides the GPS-to-Galileo D,
each constellation's code-to-phase level K_c: the cumulated difference of its code-visible and phase-visible level increments
(the median one-epoch increments of e and of pv, card ICL section 2). K_c is what neither the receiver clock nor the
inter-system offset can absorb, since both serve code and phase. Lags in epochs of 30 s, root mean square, mm; pivot handover
epochs out:

| day | arm | code D, lags 1 / 10 / 40 / 120 / 240 | phase D | K_G | K_E |
| --- | --- | --- | --- | --- | --- |
| 129 | GPS + Galileo golden (`brd_c129_6`) | 0.1 / 0.4 / 1.3 / 2.5 / 3.1 | 0.1 / 0.4 / 1.3 / 2.5 / 3.1 | 0.0 at every lag | 0.0 at every lag |
| 129 | both knobs (`brd_ngt129_6`) | 1.7 / 4.9 / 10.3 / 22.6 / 33.4 | 3.3 / 20.7 / 71.4 / 188.5 / 292.5 | 2.1 / 12.8 / 40.6 / 99.6 / 120.7 | 1.6 / 11.0 / 41.0 / 117.0 / 204.3 |
| 129 | both knobs and the frame (`icl_p129_6`) | 1.7 / 4.9 / 10.3 / 22.6 / 33.4 | 1.1 / 3.1 / 5.7 / 9.4 / 14.0 | 1.1 / 3.8 / 10.0 / 23.1 / 39.7 | 0.7 / 3.1 / 9.8 / 23.7 / 36.3 |
| 129 | the regime on CODE and the frame (`icl_rp129_6`) | 0.1 / 0.6 / 1.9 / 4.2 / 5.4 | 0.0 / 0.1 / 0.1 / 0.2 / 0.2 | 0.1 / 0.5 / 1.7 / 4.5 / 8.7 | 0.1 / 0.7 / 2.4 / 6.0 / 10.5 |
| 132 | GPS + Galileo golden (`brd_c132_6`) | 0.1 / 0.4 / 1.1 / 2.4 / 2.8 | 0.1 / 0.4 / 1.1 / 2.4 / 2.8 | 0.0 at every lag | 0.0 at every lag |
| 132 | both knobs (`brd_ngt132_6`) | 1.9 / 6.0 / 12.2 / 28.3 / 54.2 | 3.6 / 23.1 / 77.1 / 193.4 / 318.7 | 2.4 / 16.0 / 52.6 / 133.2 / 202.3 | 1.5 / 8.0 / 23.8 / 44.4 / 70.4 |
| 132 | both knobs and the frame (`icl_p132_6`) | 1.9 / 6.0 / 12.2 / 28.3 / 54.2 | 1.4 / 5.0 / 12.6 / 32.4 / 64.1 | 1.2 / 4.6 / 10.4 / 24.9 / 48.9 | 0.7 / 2.9 / 8.5 / 19.5 / 37.5 |

Card LAH's phase-clock hold keeps K at exactly zero (the held phase bias moves with the satellite's de-commoned clock step,
`network/emit/PbHold.cpp`). With the frame, the two sides of each constellation drift apart by 20 to 25 mm per 120 epochs on
the broadcast a priori. On the CODE a priori, where the clock frame hold and the phase frame vote against one a priori, the
same frame leaves 4.5 and 6.0 mm: the floor of two medians over different satellite sets.

## 3. Item 2: the candidates and the decision rule

### 3.1 The quantities

Emission order in `networkEmitStore`: the clock frame hold (C_held = C - A, A the accumulated median innovation of the
estimate C against the tracked a priori a + U), the phase-bias hold (LAH's transfer, minus the clone's passed motion Phi under
the smoothed hold), the phase frame (P + G), the broadcast wire (C_wire = C_held + O + U). A receiver reconstructs the full clock
a + U + C_held (code-visible) and a + U + C_held - P_IF - G (phase-visible). M_c is the phase-bias hold's accumulated median
emitted-clock step of constellation c (`pb_hold_frz_M`), the level LAH keeps on code and phase alike.

### 3.2 The two candidates

- **(a) The clock frame hold voting on C + U.** Under `network.apriori_clock_track`, each satellite enters the clock frame
  hold with C + U (the correction against the raw broadcast a priori) and leaves it with U taken off again, so the code-visible
  level follows the median of the raw broadcast increments, as the phase frame's does. With the phase frame armed the
  phase-visible clock is unchanged (the frame absorbs the clock side's common shift exactly where both move). Without the track
  U is absent and the hold is unchanged: a no-op on the goldens by construction.
- **(b) The phase frame voting on the phase biases' motion beyond the hold's clock transfer.** The phase frame votes on
  w = C_held - P_IF - M (the phase-visible correction in the hold's level) instead of v = C_held - P_IF + U. Under LAH every
  satellite's w is constant within a generation, so the frame does not move: an exact no-op on the goldens (the property card
  ICL's gate G1 found missing), up to the rounding of the hold's own arithmetic, which a floor of 1e-9 m on the step removes.
  Under the smoothed hold w moves with the clone's passed motion Phi, so the frame removes Phi's common mode and the
  phase-visible level follows M, the code side's.

### 3.3 The offline prediction and the decision rule (frozen 2026-11-03, before either prediction is computed)

`icl2_tools/icl2_predict.py` predicts each candidate from the existing 6 h runs, hours 2 to 6, pivot handover epochs out, the
engines' voter rules reproduced (the previous emitted value within 600 s, a datum-changed constellation and a retro-bumped
satellite out of the vote, at least 4 voters; the phase vote within one generation of both phase biases of the modelled pair):

- (a): the run with the frame (`icl_p{129,132}_6`); the code-visible clock re-held on d(C_held + U) (U = the tracked run's
  a priori minus the untracked run's, `brd_ngb{129,132}_6`); the phase-visible clock re-derived through a re-run of the phase
  frame on the re-held clock.
- (b): the run without the frame (`brd_ngt{129,132}_6`); the phase frame run on w with M recomputed from the emitted clocks
  (the hold's population: every satellite with a clock at both emissions).

For each candidate and day: code-visible D, phase-visible D, K_G and K_E at lags 1 / 10 / 40 / 120 / 240. **Rule:** S = the root
mean square of the eight lag-120 values (the four quantities on the two days); the candidate with the smaller S is built. Within
10 % of each other, (b) is built (one phase frame then serves both holds). The decision and the numbers are recorded in a dated
amendment before any build, with the new key's name.

### 3.4 The built mechanism

Default off and byte-identical off; one new key with a ledger row (UNVALIDATED); unit tests; outputs only (no estimate moves).
Events at both edges with counters: the decoupling edge (the first epoch a constellation votes on the new quantity) and the
re-coupling edges (the leash re-anchor and the warm-resume seam, each with its datum mark, as in the existing frames), every
vote counted, and a run-end line in which "0 voted" is inert wiring.

## 4. Gates (frozen 2026-11-03, before any run of the card)

### 4.1 Item 1, the frame over a day

Arms: the graded posture `_rap_icl2_p{129,132}_24` (card ICL's `icl_p{doy}_6` with 24 h) and the control `_rap_icl2_c{129,132}_24`
(the GPS + Galileo golden `brd_c{doy}_6` with 24 h), the same exe for both; census keys `i2p` and `i2c` (WTZZ00DEU and the 16
sites, 24 h consumer replays, the site's hourly p50). DOY 129 is graded; DOY 132 is run and graded on the same bars if it
completes. A 10-epoch smoke of each arm precedes the long runs.

| gate | bar |
| --- | --- |
| D1, live | Both arms exit 0 and reach 23:59:30; each constellation's frame moves on at least 99 % of the emitted epochs after the first (datum-change epochs aside); the run-end line present. |
| D2, level | Over hours 2 to 24: the phase-visible D against CODE at lag 120 at most 35 mm (card ICL's G3 bar); the closed-loop receiver's ISB one-epoch rms at most 2.0 mm. |
| D3, re-anchors | Every leash re-anchor is a `PHASE_FRAME` `reanchor` event at an epoch whose E record names the constellation's datum change; around each re-anchor, the median 3D error over the 30 minutes after minus that over the 30 minutes before at most +0.02 m at WTZZ00DEU and at most +0.01 m for the median over the 16 sites. No re-anchor in 24 h: not exercised, the headroom stated. |
| D4, no decay | Census paired median against the control over hours 7 to 24 at most its value over hours 2 to 6 of the same arms plus 0.005 m; WTZZ00DEU's change over hours 7 to 24 at most its change over hours 2 to 6 plus 0.010 m. |

Reported without a bar: |G| by hour (the end-of-hour value and the hourly largest), each re-anchor (time, constellation, |G|), the
drained sum; the code-visible D and K_G, K_E over 24 h; WTZZ00DEU and the census median by hour; card ICL's G5 user bars over
hours 2 to 24.

### 4.2 Item 2, one level for code and phase

Arms: the graded posture with the new key, `_rap_icl2_k{129,132}_{2,6}`; references card ICL's frame arms `icl_p{129,132}_6`
(census key `iclp`), the golden `brd_c{129,132}_6` (`brdc`), the free-offset arm (`brdngtI`).

| gate | bar |
| --- | --- |
| K-U0 | Debug `gnss_tests`: no new failure, the new tests pass; `gnss_simulator_selftest` 99 of 99; `make_golden_config.py --verify` PASS; `docs_index_check.py`, `nom_epoch_lint.py` pass. |
| K0, off | The key off: the 2 h field and sim goldens and the 2 h graded posture (DOY 129) byte-identical to the base exe's (`products.txt` without comment lines, `closed_loop_trace.csv`, `network_epoch_status.csv`). |
| K1, no-op by construction | The key on the GPS + Galileo golden, 2 h, DOY 129 (for (b) with `network.emit_phase_frame_hold`): byte-identical to the golden in the same files. |
| K2, live | The graded posture with the key, 2 h, DOY 129 and 132: exit 0; the run-end line present with nonzero votes; each constellation voted on at least 99 % of the emitted epochs after the first (datum-change epochs aside); hour 2: K_G and K_E at lag 40 at most 4 mm, the phase-visible D at lag 1 at most 2.5 mm and at lag 40 at most 20 mm. |
| K3, one level | The same, 6 h, each day: K_G and K_E at lag 120 at most 8 mm (from 23.1 / 23.7 and 24.9 / 19.5; the CODE regime with the frame reads 4.5 / 6.0); the code-visible and the phase-visible D at lag 120 each at most 35 mm; the receiver's ISB one-epoch rms at most 2.0 mm. |
| K4, the user | Against card ICL's frame arm (`iclp`): census paired median over hours 2 to 6 at most +0.003 m and WTZZ00DEU's change over hours 2 to 6 at most +0.005 m, each day. |

Reported without a bar: the comparison with the golden (`brdc`) and the free-offset arm (`brdngtI`); card ICL's G5 user bars;
hour 1; the prediction's error (built against predicted, each quantity).

---

## 5. Results

### 5.1 Item 2: the offline prediction and the decision (2026-11-03, before any build)

`icl2_tools/icl2_predict.py`, the rule of section 3.3. The phase-frame simulator, run on the frame arm's own vote, reproduces the
run's G (`network_phase_frame.csv`) to 0.018 mm (DOY 129) and 0.007 mm (DOY 132) at most over 1 440 values each; with the clock
frame re-held on C + U it reproduces the frame arm's phase-visible D exactly (9.4 and 32.4 mm), as section 3.2 states.
Hours 2 to 6, lags 1 / 10 / 40 / 120 / 240 epochs, mm:

| day | candidate | code D | phase D | K_G | K_E |
| --- | --- | --- | --- | --- | --- |
| 129 | as run (the frame) | 1.7 / 4.9 / 10.3 / 22.6 / 33.4 | 1.1 / 3.1 / 5.7 / 9.4 / 14.0 | 1.1 / 3.8 / 10.0 / 23.1 / 39.7 | 0.7 / 3.1 / 9.8 / 23.7 / 36.3 |
| 129 | (a) clock frame on C + U | 1.2 / 3.6 / 8.0 / 14.4 / 14.5 | 1.1 / 3.1 / 5.7 / 9.4 / 14.0 | 0.5 / 2.2 / 7.4 / 18.4 / 27.0 | 0.4 / 1.6 / 4.1 / 9.3 / 11.9 |
| 129 | (b) phase frame on C - P_IF - M | 1.7 / 4.9 / 10.3 / 22.6 / 33.4 | 1.6 / 4.7 / 9.9 / 19.0 / 28.1 | 0.4 / 1.4 / 3.1 / 5.9 / 10.4 | 0.4 / 1.4 / 3.0 / 7.0 / 11.8 |
| 132 | as run (the frame) | 1.9 / 6.0 / 12.2 / 28.3 / 54.2 | 1.4 / 5.0 / 12.6 / 32.4 / 64.1 | 1.2 / 4.6 / 10.4 / 24.9 / 48.9 | 0.7 / 2.9 / 8.5 / 19.5 / 37.5 |
| 132 | (a) clock frame on C + U | 1.4 / 5.2 / 14.4 / 37.7 / 66.7 | 1.4 / 5.0 / 12.6 / 32.4 / 64.1 | 0.6 / 2.7 / 8.0 / 20.5 / 30.1 | 0.6 / 2.0 / 5.1 / 12.8 / 25.5 |
| 132 | (b) phase frame on C - P_IF - M | 1.9 / 6.0 / 12.2 / 28.3 / 54.2 | 2.0 / 6.1 / 13.7 / 34.8 / 65.4 | 0.5 / 1.6 / 3.6 / 9.0 / 15.3 | 0.4 / 1.4 / 3.4 / 7.3 / 13.9 |

S (the root mean square of the eight lag-120 values): as run 23.84 mm, (a) 21.73 mm, (b) 19.69 mm. **(b) is built** (smaller S;
the two are 9.4 % apart counted on (a), 10.4 % on (b), and the tie rule names (b) as well).

Read beside the rule. (a) takes the code side to the raw broadcast level (code D 14.4 and 37.7 mm), but its code-to-phase level
stays near today's for GPS (18.4 and 20.5 mm): the two frames remove the medians of C + U and of C + U - P_IF, which differ by
the clones' passed motion. (b) closes each constellation's K to 6 to 9 mm (the CODE regime with the frame reads 4.5 and 6.0) at the
price of the phase-visible D on DOY 129 (9.4 to 19.0 mm): code and phase then follow one level, the tracked a priori's common mode
(the code-visible D unchanged, 22.6 and 28.3 mm). Against the frozen bars the prediction reads: K3's K_G on DOY 132 at 9.0 mm
against 8, its phase-visible D at 34.8 mm against 35; the user gate K4 is not predicted.

On the GPS + Galileo golden (card LAH's hold, `brd_c{129,132}_6`) the vote on w = C - P_IF - M, rebuilt from the printed
products, moves by at most 7.1e-15 and 5.6e-15 m per epoch (median 0) over 1 436 voted epochs each: the rounding of the hold's
arithmetic. With the 1e-9 m floor of section 3.2 G stays exactly 0.

The key: `network.phase_frame_hold_level_vote` (bool, default false): the phase frame votes on the phase-visible correction in
the phase-bias hold's level, C - P_IF - M, in place of C - P_IF + U. It requires `network.emit_phase_frame_hold` and a hold that
keeps M (`network.emit_pb_hold_phase_clock` or `network.emit_pb_hold_smoothed`), refused at load otherwise.

### 5.2 Item 2: the build and the 2 h gates (2026-11-03)

`network.phase_frame_hold_level_vote` (section 5.1): `network/emit/PhaseFrameHold` takes -M of the satellite's constellation
(`pb_hold_frz_M`, written by the phase-bias hold earlier in the same epoch) as the offset of its sample in place of U, and a
median step of at most `kPhaseFrameLevelVoteFloorM` (1e-9 m) is not applied (`PhaseFrameConfig::step_floor_m`,
`PhaseFrameStep::floored`); the leash, the datum marks and the trace are the frame's. Events: the `PHASE_FRAME` `frame_start`
event names the vote (`vote=hold_level`); the re-anchor and the resume seam unchanged. Counters: per constellation the voted
epochs floored (`phf_floored`) and the satellite-epochs without a hold level (`phf_no_level`), on a second run-end line, "every
voted epoch floored is the phase-clock hold's exact no-op". Loader: refused without `network.emit_phase_frame_hold` and without
`network.emit_pb_hold_phase_clock` or `network.emit_pb_hold_smoothed`. Ledger row UNVALIDATED (parent
`network.emit_phase_frame_hold`); `make_golden_config.py --verify` PASS, `config_posture_check.py --verify-loader` 0 gaps,
`docs_index_check.py`, `nom_epoch_lint.py` (V12: 96 writers, 96 rows) and `nom_n4_verify.py --whole` pass. Tests: the new suite
`PhaseFrameLevelVote` (10 tests: the sample with -M, the floor, the phase-clock hold's stream left at exactly 0 over 240 epochs, the
smoothed hold's common passed motion absorbed to 1e-9 m, the loader). Exe `gnss_processor_icl2k1.exe` (sha256[:16]
`3d0b60a606c380c7`), Release from the worktree.

**K-U0.** Debug `gnss_tests`: 3 883 tests in 529 suites, 3 850 passed, 33 skipped (fixture-gated, as on the base), 0 failed
(card ICL's base: 3 873 in 528); `gnss_simulator_selftest` 99 of 99; the verifiers above pass. **K-U0 PASS.**

The 10-epoch smokes (`icl2_k{129,132}_sm`): exit 0; every record but the phase biases identical to card ICL's frame arms over the
same epochs; 4 of the 10 voted epochs floored per constellation (the smoothed hold's clones are not yet born, so the keys bridge on
the phase-clock hold's transfer: the no-op case).

**K0, off.** The 2 h field golden (`icl2_c129_2` against card ICL's `icl_c129_2`), the sim golden (`icl2_s_2` against `icl_s_2`)
and the graded posture with the key off (`icl2_p129_2` against the first 240 epochs of `icl_p129_6`): `products.txt` without
comment lines, `closed_loop_trace.csv` and `network_epoch_status.csv` byte-identical. **K0 PASS.**

**K1, a no-op by construction.** The key and the frame on the GPS + Galileo golden (`icl2_cv129_2`, card LAH's phase-clock hold,
2 h, DOY 129) against `icl_c129_2`: the three files byte-identical; run-end: 239 of 239 voted epochs floored for each constellation,
0 phase-bias values shifted, largest |G| 0. Card ICL's frame on the same golden moved |G| to 35 and 51 mm (its G1). **K1 PASS.**

**K2, live.** `icl2_k{129,132}_2`: exit 0; GPS and Galileo voted on 238 of 238 emitted epochs after the first on DOY 129 (238 of
238 and 237 of 237 on DOY 132, one Galileo datum-change epoch aside), 30.6 and 20.9 voters per epoch (DOY 129); 7 and 16 voted
epochs floored (DOY 129), 19 and 11 (DOY 132); largest |G| 0.176 and 0.129 m (DOY 129), 0.304 and 0.201 m (DOY 132); no
re-anchor. Hour 2, lags 1 / 10 / 40 (mm):

| day | arm | code D | phase D | K_G | K_E |
| --- | --- | --- | --- | --- | --- |
| 129 | card ICL's frame (`icl_p129_6`, hour 2) | 1.8 / 4.0 / 7.7 | 1.3 / 3.1 / 5.8 | 1.1 / 3.3 / 8.9 | 0.8 / 3.5 / 11.4 |
| 129 | the key (`icl2_k129_2`) | 1.8 / 4.0 / 7.7 | 1.6 / 4.0 / 8.8 | 0.5 / 2.0 / **4.8** | 0.5 / 1.6 / **4.2** |
| 132 | card ICL's frame (`icl_p132_6`, hour 2) | 1.8 / 7.0 / 8.1 | 1.4 / 4.3 / 5.5 | 1.1 / 3.5 / 6.9 | 0.8 / 2.5 / 5.4 |
| 132 | the key (`icl2_k132_2`) | 1.8 / 7.0 / 8.1 | 2.0 / 6.6 / 10.6 | 0.7 / 2.3 / **4.3** | 0.6 / 1.8 / 3.6 |

The phase-visible D passes its bars (lag 1 1.6 and 2.0 mm against 2.5; lag 40 8.8 and 10.6 mm against 20); the code-to-phase level
at lag 40 halves against the frame's but stays above the 4 mm bar for both constellations on DOY 129 and for GPS on DOY 132. **K2
FAIL** (the K bar; the mechanics pass). The 6 h arms follow as the task orders; K3 and K4 decide.

### 5.3 Item 2: the 6 h gates (2026-11-03)

`icl2_k{129,132}_6` (exit 0, 2 605 s each). Run-end, DOY 129: GPS voted 719, paused 1, 30.4 voters per epoch, drained 1.155 m, largest
|G| 0.349 m; Galileo voted 718, paused 1, one datum-change epoch, 21.9 voters, drained 0.797 m, largest |G| 0.367 m; no re-anchor;
120 110 phase-bias values shifted. DOY 132: GPS drained 1.202 m, largest |G| 0.591 m; Galileo 0.715 m and 0.254 m; no re-anchor.
Hours 2 to 6, lags 1 / 10 / 40 / 120 / 240 epochs (mm):

| day | arm | code D | phase D | K_G | K_E | receiver ISB rms |
| --- | --- | --- | --- | --- | --- | --- |
| 129 | card ICL's frame (`icl_p129_6`) | 1.7 / 4.9 / 10.3 / 22.6 / 33.4 | 1.1 / 3.1 / 5.7 / 9.4 / 14.0 | 1.1 / 3.8 / 10.0 / 23.1 / 39.7 | 0.7 / 3.1 / 9.8 / 23.7 / 36.3 | 1.03 |
| 129 | the key (`icl2_k129_6`) | 1.7 / 4.9 / 10.3 / 22.6 / 33.4 | 1.6 / 4.7 / 9.9 / **19.0** / 28.1 | 0.4 / 1.4 / 3.1 / **5.9** / 10.4 | 0.4 / 1.4 / 3.0 / **7.0** / 11.8 | **1.33** |
| 132 | card ICL's frame (`icl_p132_6`) | 1.9 / 6.0 / 12.2 / 28.3 / 54.2 | 1.4 / 5.0 / 12.6 / 32.4 / 64.1 | 1.2 / 4.6 / 10.4 / 24.9 / 48.9 | 0.7 / 2.9 / 8.5 / 19.5 / 37.5 | 1.26 |
| 132 | the key (`icl2_k132_6`) | 1.9 / 6.0 / 12.2 / 28.3 / 54.2 | 2.0 / 6.1 / 13.7 / **34.8** / 65.4 | 0.5 / 1.6 / 3.6 / **9.0** / 15.3 | 0.4 / 1.4 / 3.4 / **7.3** / 13.9 | **1.52** |

The built mechanism reads its offline prediction (section 5.1) to the printed 0.1 mm in every column. **K3:** K_G and K_E at lag
120 5.9 and 7.0 mm (DOY 129) and 9.0 and 7.3 mm (DOY 132) against 8 mm; the code-visible D 22.6 and 28.3 mm and the phase-visible D
19.0 and 34.8 mm against 35; the receiver's offset 1.33 and 1.52 mm against 2.0. **K3 FAIL** (DOY 132's K_G, 9.0 mm, as predicted
in section 5.1); every other clause passes.

The user, census keys `i2k129`, `i2k132` (17 replays each, exit 0), hours 2 to 6:

| day | against | census paired median | worse by > 2 cm | better / worse at 1 cm | WTZZ00DEU change |
| --- | --- | --- | --- | --- | --- |
| 129 | card ICL's frame (`iclp129`) | **+0.0004** | 0 (0 %) | 2 / 0 | **+0.0018** (0.0178 against 0.0160) |
| 132 | card ICL's frame (`iclp132`) | **+0.0015** | 1 (6 %) | 1 / 4 | **-0.0048** (0.0315 against 0.0363) |
| 129 | the golden (`brdc129`) | +0.0080 (the frame: +0.0114) | 5 (31 %) | 3 / 8 | +0.0025 |
| 132 | the golden (`brdc132`) | +0.0205 (the frame: +0.0173) | 8 (50 %) | 2 / 11 | +0.0159 |
| 129 | the free-offset arm (`brdngtI129`) | -0.0005 | 1 (6 %) | 1 / 3 | +0.0049 |
| 132 | the free-offset arm (`brdngtI132`) | +0.0034 | 3 (19 %) | 4 / 6 | -0.0091 |

Census median by hour: 0.164 0.049 0.047 0.035 0.042 0.038 (DOY 129), 0.216 0.094 0.063 0.061 0.056 0.045 (DOY 132); WTZZ00DEU
0.067 0.032 0.022 0.013 0.012 0.010 and 0.052 0.040 0.044 0.039 0.019 0.016. **K4 PASS** (census +0.0004 and +0.0015 m against
+0.003; WTZZ00DEU +0.0018 and -0.0048 m against +0.005). Pooled against the golden +0.0142 m, 13 of 32 site-days (41 %) worse by
2 cm (the frame: +0.0144 m, 38 %): card ICL's G5 user bars still fail (WTZZ00DEU +0.0159 m and the census on DOY 132, the census
on DOY 129, the 20 % share).

Read beside the gates: the vote in the hold's level brings each constellation's code and phase to within 6 to 9 mm per 120
epochs of one level (from 20 to 25), is an exact no-op on the golden's hold, and leaves the receivers where card ICL's frame left
them (paired +0.0005 m pooled); the price is the GPS-to-Galileo phase-visible level on DOY 129, which follows the tracked a
priori's common mode with the code side (19.0 mm against 9.4), and the receiver's offset (1.33 and 1.52 mm per epoch against
1.03 and 1.26).

### 5.4 Item 1: the frame over a day (2026-11-03)

`icl2_p{129,132}_24` (exe `gnss_processor_icl2d.exe`, card ICL's icl2) and the controls `icl2_c{129,132}_24` (the same exe, the
GPS + Galileo golden): all four exit 0 at 23:59:30 (12 403 and 12 247 s; 7 424 s the DOY 132 control). The four 10-epoch smokes
reproduce the 6 h arms' first epochs in `products.txt`, `closed_loop_trace.csv` and `network_epoch_status.csv`. Census keys
`i2p129`, `i2c129`, `i2p132`, `i2c132` (24 h consumer replays, 17 of 17 exit 0 each); `i2pI129`, `i2pI132` the same wires with
receivers that re-estimate the offset every epoch (`filter.process_noise.system_time_offset_q` 1e-3, card BRD's free-offset
setting), a reading beside the gates. Tools: `icl2_day.py`, `icl2_reanchor.py`, `icl2_pool.py`, `icl2_ctrl_levels.py`.

**The frame by hour.** G at the end of hours 1 to 24 (m):

- DOY 129, GPS: -0.017 -0.128 -0.207 -0.202 -0.204 -0.034 +0.037 +0.011 -0.100 -0.056 +0.113 +0.302 +0.367 +0.499 +0.590 +0.719
  +0.847 +0.878 +0.610 +0.605 +0.700 +0.749 +0.911 +0.065 (largest |G| 1.003 m, at the re-anchor);
- DOY 129, Galileo: -0.047 +0.152 +0.336 +0.399 +0.424 +0.465 +0.418 +0.399 +0.337 +0.276 +0.284 +0.304 +0.400 +0.477 +0.557
  +0.602 +0.648 +0.615 +0.588 +0.668 +0.680 +0.700 +0.627 +0.952 (largest 0.976 m);
- DOY 132, GPS: -0.050 -0.265 -0.447 -0.434 -0.426 -0.313 -0.244 -0.302 -0.495 -0.465 -0.355 -0.221 -0.094 +0.081 +0.200 +0.320
  +0.456 +0.440 +0.243 +0.181 +0.210 +0.281 +0.525 +0.664 (largest 0.705 m);
- DOY 132, Galileo: +0.093 +0.237 +0.326 +0.346 +0.359 +0.321 +0.281 +0.203 +0.189 +0.248 +0.249 +0.282 +0.305 +0.371 +0.332
  +0.290 +0.205 +0.255 +0.329 +0.347 +0.284 +0.222 +0.235 +0.298 (largest 0.379 m).

Drained over the day: GPS 5.63 and 6.18 m, Galileo 3.74 and 3.64 m (DOY 129 and 132), 0.09 to 0.55 m per hour. The frame does
not walk to the leash at a steady rate: it wanders and comes back (DOY 132 GPS from -0.50 m in hour 9 to +0.66 m in hour 24);
one leash crossing in 48 constellation-hours.

**The re-anchor.** DOY 129, GPS, 23:25:00 (hour 24, after 23.4 h): |G| 1.003 m, reset to 0; the E record carries the datum change
of GPS; the closed-loop receiver re-inflates its offset and takes the 1.003 m step in the epoch (its Galileo offset moves -1.003
m). Around it, the median 3D error over the 30 minutes after minus that over the 30 minutes before: WTZZ00DEU +0.0008 m, the site
median -0.0043 m (worst site +0.059 m); over 10 minutes +0.0059 and -0.0028 m. DOY 132: no re-anchor.

**An unmarked step, DOY 129, 23:40:30.** The clock anchor decoupled 18 of the 22 Galileo satellites in one epoch (`CLK_ANCHOR`
`cta_decouple`, statistics 6.4 to 223 against 6.0, and G06), each with a retro bump; their clock estimates stepped by -5.2 to +1.0
m. The golden logs the same phase-anchor handover and S-transform at that epoch and no decoupling: an event of the broadcast
posture (card BRD2's anchor decouplings, on GPS + Galileo). The frame voted on the 4 satellites left, its minimum (E02, the
pivot; E03; E07; E36), and moved +0.403 m without a datum mark. On E02 and E36 the phase biases had moved by about 0.4 m while
their code-visible clocks stayed (+0.4 and +1.2 mm): the frame kept their phase-visible clocks to +1.7 and +2.8 mm (E07 +100 mm,
E03 -445 mm). The 18 bumped satellites carry the frame's shift (their median phase-visible step -445 mm, which is the level
statistic's jump); the receiver re-solves them (its Galileo offset moves -0.487 m in the epoch) and its position does not notice:
over the 10 minutes after against the 10 before, WTZZ00DEU -0.0037 m, the site median -0.0007 m, the worst site (SCRZ) +0.027 m
(the control: -0.0023, -0.0025, +0.014 m).

**The levels** (lag 120 epochs, mm; pivot handovers and every E-record datum change out, which moves card ICL's 6 h numbers by at
most 0.2 mm):

| day | window | code D | phase D | K_G | K_E | receiver ISB rms (p99) |
| --- | --- | --- | --- | --- | --- | --- |
| 129 | the control, hours 2 to 24 | 1.4 | 1.4 | 0.0 | 0.0 | 0.75 (2.8) |
| 129 | the frame, hours 2 to 24 | 34.9 | **58.3** | 33.4 | 53.9 | **9.49** (4.8) |
| 129 | the frame, hours 2 to 24, 23:40:30 out | 33.5 | 15.8 | 33.4 | 24.5 | 1.95 (4.5) |
| 129 | the frame, hours 2 to 6 | 22.5 | 9.4 | 23.1 | 23.7 | 1.04 (3.9) |
| 129 | the frame, hours 7 to 24 | 38.0 | 66.1 | 34.7 | 59.7 | 10.71 (4.9) |
| 132 | the control, hours 2 to 24 | 1.6 | 1.6 | 0.0 | 0.0 | 0.49 (2.0) |
| 132 | the frame, hours 2 to 24 | 27.5 | **18.9** | 31.4 | 25.7 | **0.87** (3.5) |
| 132 | the frame, hours 2 to 6 | 28.5 | 32.4 | 24.9 | 19.7 | 1.26 (3.7) |
| 132 | the frame, hours 7 to 24 | 27.9 | 14.0 | 32.4 | 27.3 | 0.72 (3.0) |

The phase-visible GPS-to-Galileo D at the end of each hour from hour 2 (mm): DOY 129 -1 -13 -20 -7 -23 -21 -34 -59 -82 -75 -66 -90
-102 -134 -147 -149 -156 -159 -158 -182 -192 -217 -673 (the last hour: the 23:40:30 step); DOY 132 +23 +53 +98 +115 +139 +141 +153
+142 +151 +179 +190 +193 +191 +198 +215 +222 +241 +238 +256 +230 +232 +257 +252.

**The user** (census against the control, the site's mean hourly p50 over the window, paired):

| day | window | census paired median | worse by > 2 cm | better / worse at 1 cm | WTZZ00DEU change |
| --- | --- | --- | --- | --- | --- |
| 129 | hours 2 to 6 | +0.0113 | 5 (31 %) | 3 / 8 | +0.0008 |
| 129 | hours 7 to 24 | **+0.0160** | 6 (38 %) | 0 / 12 | **-0.0004** |
| 129 | hours 2 to 24 | +0.0164 | 6 (38 %) | 0 / 11 | -0.0002 |
| 132 | hours 2 to 6 | +0.0175 | 7 (44 %) | 2 / 11 | +0.0208 |
| 132 | hours 7 to 24 | **+0.0100** | 2 (12 %) | 1 / 8 | **+0.0066** |
| 132 | hours 2 to 24 | +0.0151 | 5 (31 %) | 0 / 9 | +0.0097 |
| pooled | hours 2 to 24 | +0.0159 | 11 of 32 (34 %) | | |

By hour (median over the 16 sites, then WTZZ00DEU), m:

- DOY 129, control: 0.143 0.050 0.036 0.029 0.032 0.030 0.023 0.027 0.026 0.023 0.027 0.024 0.030 0.028 0.038 0.032 0.026 0.033
  0.031 0.027 0.027 0.024 0.029 0.028; WTZZ00DEU 0.059 0.012 0.023 0.012 0.017 0.012 0.016 0.009 0.008 0.011 0.010 0.008 0.012
  0.012 0.008 0.011 0.010 0.017 0.012 0.013 0.016 0.029 0.035 0.030;
- DOY 129, the frame: 0.192 0.051 0.042 0.038 0.051 0.039 0.035 0.040 0.043 0.033 0.034 0.034 0.039 0.040 0.043 0.046 0.061 0.056
  0.042 0.052 0.054 0.048 0.045 0.051; WTZZ00DEU 0.059 0.026 0.016 0.013 0.015 0.010 0.006 0.008 0.008 0.007 0.010 0.013 0.023
  0.015 0.014 0.044 0.029 0.011 0.011 0.009 0.012 0.012 0.012 0.011;
- DOY 132, control: 0.117 0.068 0.051 0.036 0.034 0.027 0.023 0.024 0.023 0.022 0.029 0.024 0.028 0.028 0.028 0.026 0.025 0.025
  0.032 0.023 0.026 0.030 0.028 0.026; WTZZ00DEU 0.070 0.018 0.015 0.017 0.019 0.009 0.011 0.019 0.026 0.012 0.015 0.017 0.015
  0.029 0.024 0.016 0.012 0.017 0.013 0.019 0.024 0.016 0.019 0.013;
- DOY 132, the frame: 0.237 0.092 0.071 0.057 0.049 0.043 0.044 0.048 0.042 0.046 0.040 0.036 0.038 0.042 0.043 0.039 0.040 0.035
  0.033 0.037 0.046 0.048 0.034 0.032; WTZZ00DEU 0.062 0.046 0.045 0.040 0.022 0.029 0.044 0.047 0.046 0.026 0.023 0.024 0.015
  0.015 0.019 0.016 0.021 0.010 0.028 0.016 0.031 0.024 0.014 0.017.

Receivers that re-estimate the offset every epoch, on the same wires: hours 2 to 24 against the control +0.0161 and +0.0112 m
(WTZZ00DEU -0.0006 and +0.0101 m); paired with them, the receivers as configured read **+0.0009 and +0.0019 m** over hours 2 to 24
(0 of 32 site-days worse by 2 cm), +0.0003 and -0.0009 m over hours 17 to 24. Over the whole day, the re-anchor and the 23:40:30
step included, the frame keeps the receivers as configured where free-offset receivers are; DOY 129's later hours sit about 0.02 m
above the control for both (0.042 to 0.074 m by hour from hour 17 with the free offset, against 0.024 to 0.033 m): the broadcast
posture's, not the level's.

**Gates.** D1: exit 0 and 23:59:30 on all four arms; each constellation's frame moved on 2 878 of 2 878 (GPS) and 2 877 of 2 877
(Galileo, one datum-change epoch aside) emitted epochs after the first, each day; the run-end line present. **D1 PASS.** D2: DOY 132
phase-visible D 18.9 mm against 35, the receiver's offset 0.87 mm against 2.0 (pass); DOY 129 58.3 mm and 9.49 mm (fail), both from
the 23:40:30 step (without that epoch 15.8 mm and 1.95 mm). **D2 FAIL** (DOY 129). D3: the one re-anchor is marked, WTZZ00DEU
+0.0008 m against +0.02, the site median -0.0043 m against +0.01; DOY 132 not exercised (largest |G| 0.705 m). **D3 PASS.** D4: DOY
129 census +0.0160 m over hours 7 to 24 against its bar of +0.0163 (hours 2 to 6 +0.0113 plus 0.005), WTZZ00DEU -0.0004 m against
+0.0108; DOY 132 +0.0100 against +0.0225, WTZZ00DEU +0.0066 against +0.0308. **D4 PASS.** Card ICL's G5 user bars over hours 2 to
24: WTZZ00DEU -0.0002 m (pass) and +0.0097 m (fail), the census +0.0164 and +0.0151 m (fail), 34 % of the site-days worse by 2 cm
(fail).

**The vote in the hold's level over the same day, predicted.** `icl2_predict_day.py` (the simulator that reproduced the built key to
0.1 mm, run on the 24 h frame arms with the frame's G taken off): no re-anchor (largest |G| 0.50 and 0.75 m); hours 2 to 24
(23:40:30 out), lag 120: K_G and K_E 4.1 and 5.4 mm (DOY 129), 5.8 and 3.9 mm (DOY 132), against 33.4 and 24.5, 31.4 and 25.7 with
the frame's vote; the phase-visible D follows the code side, 32.8 and 29.5 mm (code-visible 33.5 and 27.5).

## 6. Verdict

**Item 1: the frame holds a day.** Over 24 h on DOY 129 and 132 the phase-visible frame keeps the receivers as configured (the
offset frozen) where receivers that re-estimate their offset every epoch are: paired +0.0009 and +0.0019 m over hours 2 to 24, no
site-day worse by 2 cm, the last hours included. Its |G| wanders and comes back (largest 1.003, 0.976, 0.705 and 0.379 m over the
four constellation-days) and crossed the 1 m leash once, GPS on DOY 129 after 23.4 h; that re-anchor is marked and the receivers do
not notice it (WTZZ00DEU +0.0008 m, the site median -0.0043 m over 30 minutes). D1, D3 and D4 pass. D2 fails on DOY 129 through one
unmarked step: at 23:40:30 the clock anchor decoupled 18 of the 22 Galileo satellites in one epoch, each with its retro bump, and
the frame moved 0.403 m on the 4 left, its minimum. The level statistic, which keeps the bumped satellites, jumps 0.45 m and the
receiver's offset 0.49 m; the receivers' positions do not move (WTZZ00DEU -0.0037 m, the site median -0.0007 m over 10 minutes).
Without that epoch the level reads 15.8 mm per 120 epochs and the receiver's offset 1.95 mm per epoch on DOY 129. What the receivers
lose against the golden over the day (census +0.0164 and +0.0151 m; DOY 129's later hours about 0.02 m) free-offset receivers lose
too: the broadcast posture's remainder, not the level.

**Item 2: one level for code and phase, at a price on the GPS-to-Galileo level.** Measured first: with card ICL's frame each
constellation's code and phase levels drift apart by 20 to 25 mm per 120 epochs (the golden: 0). Offline, the phase frame voting
on C - P_IF - M (the phase-bias hold's level) beat the clock frame hold voting on C + U (S 19.69 against 21.73 mm; the two together
21.55). Built as `network.phase_frame_hold_level_vote` (default off, outputs only), it reads its prediction to 0.1 mm: K 5.9 and 7.0
mm (DOY 129), 9.0 and 7.3 mm (DOY 132) per 120 epochs, from 23.1 and 23.7, 24.9 and 19.5, and 4 to 6 mm over a day predicted. It is
an exact no-op on the golden's phase-clock hold (byte-identical, every step floored), the property card ICL's G1 found missing, and
the receivers are where card ICL's frame left them (paired +0.0004 and +0.0015 m). The price: the GPS-to-Galileo phase-visible level
then follows the tracked a priori's common mode with the code side, 19.0 mm per 120 epochs on DOY 129 (from 9.4) and 34.8 on DOY 132
(from 32.4), and the receiver's offset moves 1.33 and 1.52 mm per epoch (from 1.03 and 1.26). K-U0, K0, K1 and K4 pass; K2 (hour 2,
lag 40: 4.2 to 4.8 mm against 4) and K3 (DOY 132's K_G, 9.0 mm against 8, as predicted) fail on their code-to-phase bars.

Nothing is armed; arming, any golden change and the merge are the author's decisions.

## 7. What remains

- **The vote at a mass decoupling.** When the clock anchor decouples most of a constellation in one epoch, the frame votes on the
  few satellites left (4 of 22 at 23:40:30 on DOY 129) and moves unmarked. Untested: a quorum relative to the constellation's
  continuing satellites, below which the frame pauses, or a datum mark of the constellation when the anchor decouples most of it.
  Card BRD2's tracked posture with the anchor's statistic test disarmed (`network.clock_anchor_k` 1e6, branch `brdc/brd2`, GPS only)
  removes such decouplings at the source; it has no GPS + Galileo run.
- **The key over a day.** Predicted (section 5.4): no re-anchor, K of 4 to 6 mm per 120 epochs. A 24 h run of
  `network.phase_frame_hold_level_vote` with its census is owed.
- **The level both sides follow.** With the vote in the hold's level, code and phase follow the tracked a priori's common mode (the
  code-visible D 33.5 and 27.5 mm per 120 epochs over a day). The raw broadcast level is quieter on DOY 129 and noisier on DOY 132
  (section 5.1, candidate (a) and the two together). A level that follows neither (the S-basis pinning an untracked or a quiet
  pivot, card ICL section 2.5 rows (a) and (a')) is the remaining lever on the GPS-to-Galileo D.
- **The broadcast posture's remainder.** Against the golden the receivers lose +0.016 m pooled over the day whatever their offset
  setting, DOY 129's later hours about 0.02 m (card BRD2: the anchor's decouplings and the smoothed hold's bridge). DOY 130 and 131
  and a real-time run remain owed (card ICL section 7).

## 8. Correction (2026-11-05)

Section 5.4 states that at 23:40:30 on DOY 129 the golden "logs the same phase-anchor handover and S-transform at that epoch and no
decoupling". Both clauses are wrong, and the text above stands as written. The graded arm's own handover is E26's, one epoch earlier
(23:40:00, YKRO00CIV to WIND00NAM, after the IF-TD test refloated E26's L1X + L5X at the anchor station); the golden's handover at
23:40:30 is E18's (HOLB00CAN to JPLM00USA). The golden does decouple at 23:40:30, 2 satellites (E26 and E33, innovations relative to
their median -0.287 and +0.188 m), after the same slip. So the event is not absent from the golden: the broadcast posture amplifies it
(-4.46 and +0.98 m, 18 satellites). Section 5.4's reading of the frame's step and of the receivers is unchanged. The analysis continues in
card [ICL3](ICL3_frame_quorum_2026-11-05.md), section 2.2, which also finds that the 18 bumped satellites did not move by the common
step: the frame's minority vote made it.

Section 5.4 also says that leaving out the datum-change epochs moves card ICL's 6 h numbers by at most 0.2 mm. The larger part of those differences is the arms themselves: an arm of 24 h is not a superset of an arm of 6 h, because the
broadcast group delays are latched over the processing window (over 24 h, 15 Galileo satellites latch another value; the clocks move by about 1 mm, the phase biases of those satellites by up to 0.5 m on a band). The 10-epoch smokes
reproduce the 6 h arms, not the 24 h arms' first epochs. No grade of this card mixes the two lengths except that attribution ([ICL3](ICL3_frame_quorum_2026-11-05.md), section 5.3).
