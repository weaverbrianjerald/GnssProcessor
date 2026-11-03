# ICL2: the phase-visible frame over a day, and one level for code and phase

**STATUS**

- **id:** ICL2
- **topic:** network-vehicle
- **status:** open
- **summary:** Pre-registered 2026-11-03: card ICL's two owed items on card BRD's GPS + Galileo broadcast posture with both of its knobs and the phase-visible frame. (1) The frame over 24 h on DOY 129 and 132 against the GPS + Galileo golden over the same 24 h (re-anchors, |G| by hour, the levels, the receiver, the census). (2) One level for code and phase: the clock frame hold voting on C + U, or the phase frame voting on the phase biases' motion beyond the hold's clock transfer, chosen offline by a frozen rule, then built default off.
- **source:** card [ICL](ICL_inter_constellation_level_2026-11-03.md) section 7 (the two items); card [BRD](BRD_broadcast_referenced_level_2026-11-02.md) (the posture and its knobs); card [SMH](SMH_smoothed_hold_clone_2026-09-29.md) (the smoothed hold); card [LAH](LAH_level_aware_hold_2026-09-29.md) (the phase-clock hold); `docs/SPEC_clock_emit_rebase.md` (the clock frame hold)
- **knobs / posture:** existing, read: `network.emit_phase_frame_hold`, `network.phase_frame_leash_m` (card ICL), `network.emit_clock_frame_hold`, `network.apriori_clock_track`, `network.cycle_slip.if_phase_td_sat_centre`; new, default off: the key of the candidate built in item 2 (section 3.3), named at the decision amendment
- **dates in the text:** 2026-11-03
- **hashes / run dirs:** branch `icl2/frame-day-one-level` from `icl/inter-constellation-level` (`7026d4b`, unmerged), worktree `D:/GnssProcessorSupport/worktrees/icl2`; item 1 exe `gnss_processor_icl2d.exe`, a copy of card ICL's `gnss_processor_icl2.exe` (sha256[:16] `6eaa68964d3ff620`); arms `D:/GnssProcessorSupport/runs_nom/_rap_icl2_*`; census replays `_rap_<key><doy>_<site>`, keys `i2p`, `i2c` (item 1), `i2k` (item 2); scripts in `D:/GnssProcessorSupport/worktrees/icl2_tools/` (outside the repository)
- **gates (frozen at pre-registration):** section 4

**Outcome.** Pre-registration. Card ICL held the GPS-to-Galileo phase-visible level of the broadcast-referenced network in a per-constellation frame and left two items: whether the frame holds over a day (|G| reached 0.49 m in 6 h against a 1.0 m leash, and each re-anchor is a datum mark), and whether code and phase can follow one level (the frame takes the phase side to the raw broadcast a priori, the clock frame hold the code side to the tracked one). Measured first: with the frame, each constellation's code and phase levels still drift apart by 20 to 25 mm per 120 epochs.

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
