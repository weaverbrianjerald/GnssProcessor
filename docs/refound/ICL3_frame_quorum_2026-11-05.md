# ICL3: the phase-visible frame's quorum at a mass clock-anchor decoupling

**STATUS**

- **id:** ICL3
- **topic:** network-vehicle
- **status:** closed
- **summary:** Pre-registered and closed 2026-11-05: the failure of card ICL2's gate D2. At 23:40:30 on DOY 129 the clock anchor decoupled 18 of 22 Galileo satellites, and the phase-visible frame, voting on the 4 left, turned a decaying excursion of 3 of them into a permanent, unmarked 0.40 m level step. `network.phase_frame_quorum` (default off): below half of the constellation the satellites marked by their own bumps vote too. The frame moves -0.044 m where it moved +0.403 m; phase-visible D over hours 2 to 24 15.8 mm per 120 epochs (from 58.3); the receiver's offset -0.040 m at the event (from -0.487 m); positions unchanged. Q-U0, Q0, Q1, Q3 pass; Q2 fails on one clause (offset rms 2.10 against 2.0 mm). One natural event; injections did not reproduce it. Nothing armed.
- **source:** card [ICL2](ICL2_frame_day_one_level_2026-11-03.md) sections 5.4 and 7 (the event and the owed item); card [ICL](ICL_inter_constellation_level_2026-11-03.md) (the frame); card [SMH](SMH_smoothed_hold_clone_2026-09-29.md) (the hold whose bridge and clones write the phase biases); card BRD2 (`docs/refound/BRD2_broadcast_remainder_hold_days_2026-11-03.md` on branch `brdc/brd2`: the anchor's statistic decouplings)
- **knobs / posture:** new, default off: `network.phase_frame_quorum` (UNVALIDATED; sections 3.4 and 5.2); read: `network.emit_phase_frame_hold`, `network.phase_frame_leash_m`, `network.phase_frame_hold_level_vote`; graded on card BRD's GPS + Galileo broadcast posture with both of its knobs and the frame (`icl2_p{129,132}_24`), receivers as the golden configures them
- **dates in the text:** 2026-11-05
- **hashes / run dirs:** branch `icl3/frame-quorum` from `icl2/frame-day-one-level` (`bee32a8`, unmerged, not pushed), worktree `D:/GnssProcessorSupport/worktrees/icl2` (the same build tree); base exe `gnss_processor_icl2k1.exe` (sha256[:16] `3d0b60a606c380c7`), the guard's `gnss_processor_icl3q1.exe` (`92743f1fb3c747fb`), census `gnss_processor_icl2c.exe`; arms `D:/GnssProcessorSupport/runs_nom/_rap_icl3_*` (`icl3_q129_24`, `icl3_q132_6`, `icl3_{c129,s,p129}_2`, the injected `icl3_i{1..5}_129_3`, the rewritten products `icl3_x{P,W,M}129`); census replays `_rap_<key><doy>_<site>`, keys `i3P`, `i3W`, `i3M`, `i3q`, `i3qI`; scripts in `D:/GnssProcessorSupport/worktrees/icl3_tools/` (outside the repository)
- **gates (frozen at pre-registration):** section 4

**Outcome.** At a clock-anchor decoupling of 18 of 22 Galileo satellites the phase-visible frame voted on the 4 left and turned a decaying excursion of 3 of them into a permanent, unmarked 0.40 m level step. A quorum (default off) lets the satellites marked by their own bumps vote too: the frame moves -0.044 m, the GPS-to-Galileo level over the day reads 15.8 mm per hour (from 58.3), the receiver's offset -0.040 m at the event (from -0.487 m). Positions do not change, since receivers absorbed the step through their offsets. Q2 fails on one clause. Nothing armed.

> Results are appended TO THIS FILE, below the text.

---

## 1. The question

Card ICL2 graded card ICL's phase-visible frame over 24 h on DOY 129 and 132 and found one failure (gate D2, DOY 129): at 23:40:30
the clock anchor decoupled 18 of the 22 Galileo satellites in one epoch, each with a retro bump, and the frame moved 0.403 m on the 4
left, with no datum mark. The question: what should the frame do when most of a constellation is marked in one epoch, so that the
phase-visible level stays what it was, and what does each answer cost a receiver?

## 2. Measured before the gates (2026-11-05)

On card ICL2's 24 h arms `icl2_p{129,132}_24` (scripts `icl3_tools/icl3_survey.py`, `icl3_event.py`, `icl3_vote.py`).

### 2.1 How often the vote is a minority

Per epoch after the first and per constellation, the frame's voters as a share of the constellation's satellites with a sample (an
emitted clock and a complete modelled pair), 11 516 constellation-epochs:

| arm, constellation | share below 0.5 | 0.5 to 0.7 | 0.7 to 0.9 | at least 0.9 |
| --- | --- | --- | --- | --- |
| DOY 129, GPS | 0 | 0 | 0 | 2 879 |
| DOY 129, Galileo | 2 | 0 | 6 | 2 871 |
| DOY 132, GPS | 0 | 1 | 10 | 2 868 |
| DOY 132, Galileo | 1 | 1 | 0 | 2 877 |

The epochs with a share below 0.7: two declared datum changes of Galileo, in which every satellite is forced and the frame stays by
design (DOY 129 03:20:30, DOY 132 01:45:00); DOY 132 00:48:00, GPS, 16 voters of 30 (14 retro bumps, step +0.07 mm); DOY 132
03:09:30, Galileo, 16 of 23 (7 retro bumps, step -7.6 mm); and DOY 129 23:40:30, Galileo, 4 of 22 (18 retro bumps, step +403.5 mm).
The one voted epoch below one half is the failure. The anchor's decouplings per epoch and constellation reach 18 on DOY 129 (that
epoch) and 14, 7, 7, 6, 6, 5 on DOY 132.

### 2.2 The event

At 23:40:00 (`T1_if_slip`) the IF-TD test refloats E26's L1X + L5X at YKRO00CIV, E26's phase anchor for that pair, and the anchor
hands over to WIND00NAM (`anchor_slip_handover`, a continuous S-transform of -36.6 m on the phase bias). At 23:40:30 the solve's
Galileo clock innovations scatter: relative to their median, E26 -4.46 m, E33 +0.98 m, E13 -0.73 m, E19 +0.43 m, and the anchor
decouples 18 satellites (statistics 6.4 to 223 against 6.0). The same slip reaches the golden, where the anchor decouples 2
satellites at the same epoch, E26 and E33, with innovations relative to their median of -0.287 and +0.188 m: the broadcast posture
amplifies the same event 5 to 15 times. The tracked a priori does not move (its second differences are at most 7 mm).

The delivered clocks against CODE, relative to 23:39:30, Galileo (mm):

| group | side | 23:40:00 | 23:40:30 | 23:41:00 | 23:41:30 | 23:42:00 | 23:42:30 |
| --- | --- | --- | --- | --- | --- | --- | --- |
| E02, the pivot | code-visible | -2 | -1 | -57 | -83 | -89 | -90 |
| E02 | phase-visible before the frame | -2 | +403 | +43 | -43 | -64 | -67 |
| E36 | phase-visible before the frame | -6 | +401 | +53 | -36 | -58 | -59 |
| E07 | phase-visible before the frame | -4 | +500 | +48 | -36 | -54 | -59 |
| E03 | phase-visible before the frame | -3 | -44 | -65 | -73 | -75 | -73 |
| the 18 bumped, median | phase-visible before the frame | -3 | -45 | -69 | -76 | -77 | -76 |
| E02 | phase-visible as emitted | -1 | +1 | -336 | -416 | -437 | -440 |
| E03 | phase-visible as emitted | -1 | -446 | -444 | -446 | -447 | -446 |
| the 18 bumped, median | phase-visible as emitted | -1 | -447 | -448 | -449 | -450 | -448 |

The code-visible clocks of the 18 scatter by -5.2 to +1.0 m at 23:40:30 and return within one epoch to about -85 mm. The frame's vote
at 23:40:30, dv per satellite (mm): voters E02 +402.0, E03 -41.8, E07 +503.6, E36 +405.0, median +403.5 (the run's step); the 18
forced between -51.4 and -37.4, most at -43.7, the median step M of the hold's clock level (their keys bridge on the phase-clock
transfer, so C - P_IF moves by M alone); the median over all 22 is -43.7. After 23:40:30 the vote is normal and the level does not
come back. So the excursion of E02, E36 and E07 relative to the group (+0.45 m, +0.11 m, +0.03 m in three epochs) is a transient,
and the frame, taking it for the constellation's common move, moved the other 19 by -0.40 m for good: phase-visible D steps by
-0.45 m, unmarked.

## 3. The candidates and the decision rule

### 3.1 The quorum

A constellation's plain vote stands when its voters reach the quorum: at least `max(min_voters, ceil(share x n))`, n its satellites
with a sample this epoch, share `network.phase_frame_quorum`. By section 2.1 a share of 0.5 acts on the failure alone. A declared
datum change of the constellation is not a quorum event: every satellite is forced and the frame stays, as today.

### 3.2 Below the quorum, three candidates

- **(P), pause.** The frame does not move this epoch. The step of this epoch (-44 mm at the event) stays on the phase side.
- **(W), widen.** The satellites forced by their own bump alone vote with the others, if they continue in their generation; the frame
  moves by the median of that wider set when it reaches the quorum, else it pauses. A retro bump keeps one satellite's own event out
  of the vote; when the marked satellites are the majority, they are the constellation. At the event: -43.7 mm.
- **(M), pause with a datum mark.** As (P), and the constellation is declared changed (`markDatumChange`, as the leash's re-anchor), so
  a receiver re-inflates its offset.

### 3.3 The decision rule (frozen 2026-11-05, before any replay)

The frame is outputs only and a receiver's replay re-derives the emitter's counters from the file, so rewritten products are what a
live run of the candidate would deliver. `icl3_tools/icl3_rewrite.py` writes, from `icl2_p129_24/products.txt`, one product file per
candidate: every Galileo phase bias from 23:40:30 on shifted by the difference between the candidate's frame and the run's ((P) -403.480
mm, (W) -447.174 mm), (M) as (P) with the datum change in that epoch's E record. Census replays of 24 h, exe `gnss_processor_icl2c.exe`,
keys `i3P129`, `i3W129`, `i3M129`; reference `i2p129`.

Over the 39 epochs from 23:40:30 to 23:59:30, S is the median over the 16 sites of the site's median 3D error. The candidate with the
smallest S is built; candidates within 0.002 m of the smallest are a tie, resolved in the order (W), (P), (M): (W) holds the level with
no mark and leaves no exposure to a real common move, (P) leaves the epoch's step unmarked, (M) spends a receiver re-inflation on
every such event. Reported for each: WTZZ00DEU's median over the window, the worst site, the closed-loop receiver's largest one-epoch
offset change in the window, the phase-visible D's step at the event. The decision and its numbers are recorded in a dated amendment
before any build.

### 3.4 The built mechanism

`network.phase_frame_quorum` (double, default 0 = off, byte-identical; 0.5 in the arms; refused at load outside [0, 1] or armed without
`network.emit_phase_frame_hold`). Outputs only. Events at both edges: `PHASE_FRAME` when a constellation falls below the quorum (with
its voters, its satellites, the plain vote's step not taken and the step taken) and when its vote is back at the quorum. Counters per
constellation and a run-end clause, in which "0 below the quorum" is the normal reading, not inert wiring. A new unit-test suite, a
ledger row (UNVALIDATED, parent `network.emit_phase_frame_hold`).

## 4. Gates (frozen 2026-11-05, before any run of the card)

Arms: the graded posture with `network.phase_frame_quorum` 0.5, `_rap_icl3_q129_24` (24 h, the event) and `_rap_icl3_q132_6` (6 h, the
epochs of share 0.53 and 0.70); census key `i3q129`.

| gate | bar |
| --- | --- |
| Q-U0 | Debug `gnss_tests`: no new failure, the new suite passes; `gnss_simulator_selftest` 99 of 99; `make_golden_config.py --verify` PASS; `docs_index_check.py`, `nom_epoch_lint.py`, `nom_n4_verify.py --whole` pass. |
| Q0, off | The key off: the 2 h field and sim goldens and the 2 h graded posture (DOY 129) byte-identical to the base exe's (`icl2_c129_2`, `icl2_s_2`, `icl2_p129_2`: `products.txt` without comment lines, `closed_loop_trace.csv`, `network_epoch_status.csv`). |
| Q1, inert at the quorum | `icl3_q132_6`: the three files byte-identical to `icl_p132_6`, no quorum event, the run-end clause present with 0 below the quorum. |
| Q2, the event | `icl3_q129_24`: the three files byte-identical to `icl2_p129_24` up to 23:40:00; the event named at 23:40:30 with both edges; |frame step| at that epoch at most 0.05 m; the phase-visible D against CODE over hours 2 to 24 at lag 120 at most 35 mm and the closed-loop receiver's offset rms at most 2.0 mm (card ICL2's D2 bars). |
| Q3, the receivers | Census `i3q129` against `i2p129`, over 23:40:30 to 23:59:30: paired median of the site's median 3D error at most +0.002 m; WTZZ00DEU's change at most +0.005 m; at most 1 of the 16 sites worse by more than 0.02 m. Over hours 2 to 24: census paired median within 0.001 m of zero. |

Reported without a bar: the built arm's products against the rewritten candidate's; the frozen-offset receivers paired with free-offset
receivers around the event (key `i3qI129`); the vote in the hold's level with the quorum, predicted offline; an injected second event,
if one reproduces a minority vote in a 2.5 h arm (a slip at a Galileo satellite's phase anchor station, the trigger of section 2.2): the
same statistics over the 30 minutes after it, or "not reproduced".

---

## 5. Results

### 5.1 The replays and the decision (2026-11-05, before any build)

`icl3_tools/icl3_rewrite.py` on `icl2_p129_24/products.txt`: 3 494 Galileo phase-bias values shifted from 23:40:30 on; a zero shift
rewrites the file byte for byte. Census replays of 24 h, 17 of 17 exit 0 per key. Over the 39 epochs from 23:40:30 to 23:59:30 (m):

| candidate | key | S (median over the 16 sites of the site's median 3D error) | WTZZ00DEU | worst site | paired against as run: median, most worse, most better |
| --- | --- | --- | --- | --- | --- |
| as run (the frame) | `i2p129` | 0.0507 | 0.0101 | ARHT00ATA 0.1903 | |
| (P) pause | `i3P129` | 0.0505 | 0.0101 | ARHT00ATA 0.1905 | -0.0000, +0.0010 (DAV100ATA), -0.0044 (HARB00ZAF) |
| (W) widen | `i3W129` | 0.0505 | 0.0101 | ARHT00ATA 0.1905 | -0.0000, +0.0011 (DAV100ATA), -0.0058 (HARB00ZAF) |
| (M) pause with a datum mark | `i3M129` | 0.0505 | 0.0101 | ARHT00ATA 0.1905 | -0.0000, +0.0010 (DAV100ATA), -0.0044 (HARB00ZAF) |

All three candidates tie within 0.002 m (0.2 mm against as run, no site moving by 2 cm), so **the rule of section 3.3 builds (W)**, the
vote widened to the satellites marked by their own bumps.

Why the positions do not choose: a clock-channel retro bump makes a receiver re-inflate its GPS-to-Galileo offset and keep its
ambiguities (`closed_loop_sat_trace.csv`, WTZZ00DEU, `ev_clk` = `reinflate_isb`, ambiguity variance unchanged), so on the epoch of the
18 bumps the receiver absorbs a common phase step through its offset. As run, the bumped satellites' phase innovations at 23:40:30 read
-0.422 to -0.455 m (the frame's step, absorbed: the offset moves -0.487 m); with (W) they read -0.008 to +0.025 m, as on the epoch
before; with (P) -0.019 to -0.052 m. E07's ambiguity restarts at WTZZ00DEU in all arms. The guard's gain is therefore the level and the
receiver's offset, not the position at this event: a receiver gets away with the frame's step because the same epoch happens to carry
the marks of the satellites that voted against it.

The key: `network.phase_frame_quorum` (section 3.4), with `PHASE_FRAME` events `quorum_low` and `quorum_restored`.

### 5.2 The build and the off path: Q-U0, Q0, Q1 (2026-11-05)

`network.phase_frame_quorum` (section 3.4): in `network/emit/PhaseFrameHold`, `PhaseFrameEntry::own_mark_only` (forced by a retro bump
of its own alone), `PhaseFrameConfig::quorum`, and in `phaseFrameHoldEpoch` the need `max(min_voters, ceil(quorum x n))`; below it, on a
started frame with no datum change of the constellation pending, the entries with `own_mark_only` that pass the same continuity tests
join the vote (`PhaseFrameStep::widened`), else the frame stays (`below_quorum` alone). Events `PHASE_FRAME` `quorum_low` (statistic the
voters' share, threshold the quorum, action `widened` or `paused`, detail the voters, the satellites, the marked, the plain step not taken
and the step taken) and `quorum_restored`; trace events `quorum_widened`, `quorum_paused`; counters `phf_quorum_widened`, `phf_quorum_paused`,
`phf_quorum_declined_m` and the latch `phf_quorum_low`; a run-end clause. Ledger row UNVALIDATED. Exe `gnss_processor_icl3q1.exe` (sha256[:16]
`92743f1fb3c747fb`), Release from the worktree; 10-epoch smokes with the key on and off identical to the first 11 epochs of `icl_p129_6` in
the three files.

**Q-U0.** Debug `gnss_tests`: 3 896 tests in 530 suites, 3 863 passed, 33 skipped (fixture-gated, as on the base), 0 failed (card ICL2's
base: 3 883 in 529; the new suite `PhaseFrameQuorum` holds 13, the vote of 23:40:30 among them); `gnss_simulator_selftest` 99 of 99;
`make_golden_config.py --verify` PASS; `config_posture_check.py --verify-loader` 0 gaps; `docs_index_check.py`, `nom_epoch_lint.py`,
`nom_n4_verify.py --whole` pass. **Q-U0 PASS.**

**Q0, off.** `icl3_c129_2` against `icl2_c129_2`, `icl3_s_2` against `icl2_s_2`, `icl3_p129_2` against `icl2_p129_2`: `products.txt` without
comment lines, `closed_loop_trace.csv` and `network_epoch_status.csv` byte-identical. **Q0 PASS.**

**Q1, inert at the quorum.** `icl3_q132_6` (share 0.53 on GPS at 00:48:00, 0.70 on Galileo at 03:09:30) against `icl_p132_6`: the three files
byte-identical; the run-end clause reads widened 0, paused 0 for both constellations; the only `PHASE_FRAME` events are the two
`frame_start`. **Q1 PASS.**

**The injected second event: not reproduced.** Four 3 h arms of the graded posture with the key, a one-cycle slip on a satellite's L5X at its
phase anchor station at 02:00 on DOY 129: E26 and E33 at CUSV00THA, E05 at GCGO00USA, and E10 at WIND00NAM, the one satellite at that epoch
with split X-channel anchors, as E26 had at the natural event (its L1X hands over alone, as E26's did). On the last three the IF-TD test and the
anchor handover fire as at the natural event (statistics 0.318 to 0.325 m against its 0.346 m), on E26 the anchor hands over cold; the clock
anchor decouples nothing in the 2 minutes after, and the frame's vote stays at 21 or 22 of 22. Among the S-transforms of the two days, size does not mark the event either: |S| reaches 279 m
with no decoupling at the next epoch; the event's is 36.6 m. Why one slip handover in 267 scatters a constellation is left open (section 7).

An arm added after the slips failed, not pre-registered: the same posture with persistent clock steps injected at 02:00 on the 18 satellites of
the natural event, at its sizes (`icl3_i5_129_3`, -5.158 to +1.001 m). The clock anchor decouples 4 satellites; 11 of the 22 are forced, so the
vote stands at exactly the quorum (plain step +13.1 mm) and has 19 voters at the next epoch. The widened path is not reached, and the natural
event stays its only instance in the runs of this card.

### 5.3 The event with the guard: Q2, Q3 (2026-11-05)

`icl3_q129_24` (exit 0, 23:59:30 reached, 9 714 s), the graded posture with `network.phase_frame_quorum` 0.5, against card ICL2's `icl2_p129_24`:

- `products.txt` without comment lines: header and epochs up to 23:40:00 identical; from 23:40:30 on, 3 494 Galileo phase-bias values differ, each by
  exactly -447.174243 mm, and no other line differs. Against the rewritten candidate of section 5.1 the largest phase-bias difference is 0.000243 mm
  (the trace's rounding of the step) and no other line differs. `network_epoch_status.csv` identical over the whole day; `closed_loop_trace.csv` identical up to
  23:40:00 (2 841 rows), 14 of the last 39 rows differ.
- `PHASE_FRAME` `quorum_low` at 23:40:30, Galileo: statistic 0.1818 against 0.5, action `widened`, "voters=4; present=22; own_marked=18; plain_step=0.4035 m;
  step=-0.0437 m". `quorum_restored` at 23:41:00 (21 of 22). Run-end: Galileo widened 1, paused 0, largest minority move kept off the level 0.4472 m; GPS widened 0,
  paused 0. Every other `PHASE_FRAME` event as in `icl2_p129_24`, the GPS leash re-anchor at 23:25:00 among them.
- The phase-visible clocks relative to 23:39:30 (mm), against section 2.2's as-emitted rows, in the same epochs: E03 -1, +1, +3, +1, 0, +2; E26 0, +2, +4, +1, -1,
  +3; E02 -1, +448, +111, +31, +11, +7; E36 -4, +445, +121, +38, +16, +16; E07 -3, +545, +116, +38, +20, +15. The majority holds; the three excursions now show as what they
  are, and decay.
- The closed-loop receiver's Galileo offset at the event moves -0.0404 m (from -0.4874 m), its stated standard deviation 0.0146 m in both arms.

| window | arm | code D, lag 120 | phase D, lag 120 | K_G | K_E | receiver ISB rms (p99) |
| --- | --- | --- | --- | --- | --- | --- |
| hours 2 to 24 | the frame (`icl2_p129_24`) | 34.9 | 58.3 | 33.4 | 53.9 | 9.49 (4.8) |
| hours 2 to 24 | the frame and the quorum (`icl3_q129_24`) | 34.9 | **15.8** | 33.4 | 25.3 | **2.10** (4.8) |
| hours 7 to 24 | the frame and the quorum | 38.0 | 17.3 | 34.7 | 25.4 | 2.31 (4.9) |

The phase-visible D at the end of each hour from hour 2 (mm): -1 -13 -20 -7 -23 -21 -34 -59 -82 -75 -66 -90 -102 -134 -147 -149 -156 -159 -158 -182 -192 -217 -226 (card
ICL2's last value: -673).

**Q2:** identity up to the event, the event named with both edges, |frame step| 0.0437 m against 0.05, phase-visible D 15.8 mm against 35: pass. The closed-loop receiver's
offset rms 2.10 mm against 2.0: **fail, by 0.10 mm.** The statistic's sum over the day: 60 % is one excursion at 12:28:30 to 12:29:30 (+63.7, -18.6, -57.4 mm, at epochs with 1 to 2
Galileo and 2 to 3 GPS satellite bumps, the frame's vote 20 to 21 of 22 and 28 to 29 of 31, far above the quorum, the same in both arms), 13 % the event's remaining -40.4 mm, and without the 8 largest epochs the rms is 0.89
mm in both arms. The event's own share was 95.8 % (-487.4 mm) without the guard. The -40 mm is the receiver's re-inflated offset following that epoch's code: the code-visible
level moves -41.1 mm at 23:40:30, then -21.9 and -9.5 mm, and the frame does not touch the code side. **Q2 FAIL** on that clause; the frame's clauses pass.

**Q3**, census `i3q129` (17 replays, exit 0) against `i2p129`, 23:40:30 to 23:59:30: paired median -0.0000 m against +0.002; WTZZ00DEU +0.0000 m against +0.005; 0 of 16 sites worse by
more than 0.02 m (most worse DAV100ATA +0.0011 m, most better HARB00ZAF -0.0058 m); hours 2 to 24 paired +0.0000 m. S 0.0505 m, equal to the rewritten candidate's. **Q3 PASS.**

Reported without a bar. Against the golden, unchanged from card ICL2: census +0.0113, +0.0160 and +0.0164 m over hours 2 to 6, 7 to 24 and 2 to 24, WTZZ00DEU +0.0008, -0.0004 and
-0.0002 m. Paired with free-offset receivers on the same wire (`i3qI129`): hours 2 to 24 +0.0009 m (WTZZ00DEU +0.0005 m, 0 of 16 worse by 2 cm); hour 24 +0.0026 m; the event's
window +0.0040 m, as without the guard (+0.0041 m, the worst site ARHT00ATA +0.046 m both ways).

The vote in the hold's level with the quorum, predicted (`icl3_predict.py`, the simulator of card ICL2): hours 2 to 24 on DOY 129, phase-visible D at lag 120 34.4 mm (74.7 without the quorum),
K_G 4.1 mm, K_E 5.4 mm (55.4 without), no re-anchor; at 23:40:30 the plain vote would take +448.6 mm and the widened vote takes 0. On DOY 132 no epoch reaches the quorum: identical
with and without it.

**A note on run length.** The arms of 24 h are not supersets of the arms of 6 h: the 6 h and 24 h arms of one configuration differ from the first epoch. The broadcast group delays are latched once per run over the
processing window (`latched broadcast group delays` at setup), and over 24 h 15 Galileo satellites latch another value than over 6 h (E15 and E26 +0.279 m, E04 and E31 +0.209 m). The phase biases of those satellites shift on both
bands (E26 L1C -0.279 m, L5Q -0.500 m), the clocks by about 1 mm (at 02:00 the Galileo median -0.8 mm, GPS +0.02 mm). The grades here compare arms of one length, so none of them is touched; card ICL2's section 5.4 attributes its
differences of at most 0.2 mm from card ICL's 6 h numbers to the epochs left out, and this is the larger part (its section 8 is corrected).

## 6. Verdict

**The guard does what it was built for: the phase-visible level no longer takes a minority's move. It does not change any receiver's position at this event, and its evidence is one natural
event.** Measured first: when the clock anchor decoupled 18 of 22 Galileo satellites at 23:40:30 on DOY 129, the 18 agreed on -44 mm, and 3 of the 4 voters left carried an excursion of +0.45 m that decayed
within 4 epochs. The frame took the excursion for the common move and left the constellation's phase-visible level 0.40 m off for good, unmarked. With `network.phase_frame_quorum` 0.5, below half
of the constellation the satellites marked by their own bumps vote too: the frame moves -0.0437 m where it moved +0.4035 m. The phase-visible GPS-to-Galileo D over hours 2 to 24 reads 15.8 mm per 120 epochs
(from 58.3), passing the level clause of card ICL2's D2, and the closed-loop receiver's offset moves -0.040 m at the event (from -0.487 m). Over 11 516 constellation-epochs the key acts on this epoch alone: the off path and the 6 h arm at
the quorum are byte-identical, and in the event's arm the products differ only by one constant on the Galileo phase biases from that epoch on. Q-U0, Q0, Q1 and Q3 pass. Q2 fails on one clause: the receiver's offset rms
is 2.10 mm against 2.0, of which 60 % is an unrelated excursion at 12:28 and 13 % the code-visible step of the event, which the frame does not touch.

The positions were already safe: a clock-channel retro bump makes a receiver re-inflate its offset and keep its ambiguities, so on the epoch of the 18 bumps the receivers absorbed the frame's 0.44 m through their offsets
(census -0.0000 m, WTZZ00DEU +0.0000 m with the guard against without). That safety was incidental to the marks of that epoch, so the guard's value is the integrity of the level and of the offset, not a measured position gain.
Four injected slips at phase anchor stations, one with split anchors like the natural event's, did not reproduce a mass decoupling, and a mass of injected clock steps stopped at exactly the quorum: the widened path has run live once.

The choice among the three candidates came from a tie, by the frozen rule's order: the receivers do not separate them at this event. Nothing is armed; arming, any golden change and the merge are the author's decisions.

## 7. What remains

- **Why this slip scatters the constellation.** The same slip at YKRO00CIV moves E26 and E33 by -0.287 and +0.188 m on the golden and by -4.46 and +0.98 m on the broadcast posture, with 18 satellites decoupled against 2. One slip handover in 267 over the two days
  does it; neither the size of the S-transform (|S| up to 279 m without a decoupling) nor split anchors reproduce it. The guard removes the frame's part, not the event: the code-visible clocks of the 18 still scatter by -5.2 to +1.0 m for one epoch.
- **The clock frame hold's vote.** It excludes the same forced keys and voted on the same 4 satellites at the event. Its step did no harm there, but the code-visible level moves -72.5 mm over 3 epochs, unmarked, and the rule has the same minority exposure. It is armed in the goldens, so a
  change there is a card of its own.
- **The hold's level M.** Its median runs over every satellite with a clock, the 18 included, so a mass decoupling writes -44 mm into every bridged key's phase-visible clock and into the level the code side shares; a quorum on M is untested.
- **The receiver's offset at bumps.** A re-inflated offset follows that epoch's code (12:28 on DOY 129, +63.7 mm); whether one bump should re-inflate it, and for how long, is a receiver question.
- **The key over a day, and DOY 130 and 131.** The vote in the hold's level with the quorum has a prediction (section 5.3) and no 24 h run; card ICL's other owed days and the real-time run remain.

## 8. Correction (2026-11-06)

Section 7's first item is answered by card [ICL4](ICL4_post_flag_slip_scatter_2026-11-06.md), and its framing is wrong. The slip handover is not the cause: a short arm with E26's anchors at another station and no handover decouples the same 18 satellites. The cause is a second, real slip (L5X of E26, about -294 cycles, no LLI) in the epoch after the IF-TD flag of section 2.2, which no pre-solve test answers. That is also why the four injected slips of section 5 did not reproduce the event: each was a first slip, which the IF-TD test catches. With `network.cycle_slip.if_phase_td_reseed_on_flag` the event does not occur and the frame votes on 22 satellites, so this card's one natural event no longer reaches the quorum on that key.
