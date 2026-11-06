# ICL3: the phase-visible frame's quorum at a mass clock-anchor decoupling

**STATUS**

- **id:** ICL3
- **topic:** network-vehicle
- **status:** open
- **summary:** Pre-registered 2026-11-05: the failure of card ICL2's gate D2. At 23:40:30 on DOY 129 the clock anchor decoupled 18 of 22 Galileo satellites, the phase-visible frame voted on the 4 left, and a decaying excursion of 3 of them became a permanent, unmarked 0.40 m step of the constellation's phase-visible level. A quorum on the frame's vote, default off: pause, widen the vote to the satellites marked by their own bumps, or pause with a datum mark, chosen by replaying the receivers on rewritten products under a frozen rule.
- **source:** card [ICL2](ICL2_frame_day_one_level_2026-11-03.md) sections 5.4 and 7 (the event and the owed item); card [ICL](ICL_inter_constellation_level_2026-11-03.md) (the frame); card [SMH](SMH_smoothed_hold_clone_2026-09-29.md) (the hold whose bridge and clones write the phase biases); card BRD2 (`docs/refound/BRD2_broadcast_remainder_hold_days_2026-11-03.md` on branch `brdc/brd2`: the anchor's statistic decouplings)
- **knobs / posture:** new, default off: `network.phase_frame_quorum` (section 3.3); read: `network.emit_phase_frame_hold`, `network.phase_frame_leash_m`, `network.phase_frame_hold_level_vote`; graded on card BRD's GPS + Galileo broadcast posture with both of its knobs and the frame (`icl2_p{129,132}_24`), receivers as the golden configures them
- **dates in the text:** 2026-11-05
- **hashes / run dirs:** branch `icl3/frame-quorum` from `icl2/frame-day-one-level` (`bee32a8`, unmerged, not pushed), worktree `D:/GnssProcessorSupport/worktrees/icl2` (the same build tree); base exe `gnss_processor_icl2k1.exe` (sha256[:16] `3d0b60a606c380c7`); arms `D:/GnssProcessorSupport/runs_nom/_rap_icl3_*`; census replays `_rap_<key><doy>_<site>`; scripts in `D:/GnssProcessorSupport/worktrees/icl3_tools/` (outside the repository)
- **gates (frozen at pre-registration):** section 4

**Outcome.** Pre-registration. Card ICL2 found the phase-visible frame holding a 24 h day except at one epoch: a clock-anchor decoupling of 18 of 22 Galileo satellites, at which the frame moved 0.403 m on the 4 satellites left. Measured first: the 18 agree on -44 mm, 3 of the 4 voters carry an excursion of +0.45 m that decays within 4 epochs, so the frame turned a transient of 3 satellites into a permanent level step of the constellation. Over 11 516 constellation-epochs of the 24 h arms no other vote is a minority.

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
-0.434 to -0.455 m (the frame's step, absorbed: the offset moves -0.487 m); with (W) they read -0.008 to +0.025 m, as on the epoch
before; with (P) -0.019 to -0.052 m. E07's phase is refused by the receiver in all arms. The guard's gain is therefore the level and the
receiver's offset, not the position at this event: a receiver gets away with the frame's step because the same epoch happens to carry
the marks of the satellites that voted against it.

The key: `network.phase_frame_quorum` (section 3.4), with `PHASE_FRAME` events `quorum_low` and `quorum_restored`.
