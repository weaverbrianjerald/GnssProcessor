# BRD3: the broadcast-referenced level at the clock anchor's statistic decouplings: the bridge, the clock-space share, and the step test

**STATUS**

- **id:** BRD3
- **topic:** network-vehicle
- **status:** open
- **summary:** Pre-registered 2026-11-06, from card BRD2's remainder. Measured offline against CODE first: the smoothed hold's bridge follows the a priori while the clock is decoupled, so it drops the clock correction's motion D over the bridge; D is the LOOSE clock's own departure from truth plus the a priori's drift, and on the CODE a priori the bridge is the right member (its lasting phase-visible error 6 mm against 13 mm for a clock-following bridge), on the tracked a priori a coin flip (15 against 13.5 mm, better on 12 of 26). The bridge member is refuted as the lever; the decoupling is the carrier. Every statistic trip on DOY 132 is carried by the phase-bias keys (clock-space |w| at most 0.57 over 59 trips on both postures; a true 0.5 m step at the same q reads 7.9). The next build, pre-registered here with frozen bars: a clock-share requirement on the phase statistic's trip (`network.clock_anchor_clock_k`), valid at the broadcast regime's q. Run here: the injected clock step on the tracked sim posture passes every matrix channel with the test on and off (the column takes 0.494 m, no refloat, no trip, the wire -0.499 m); the instrumented DOY-132 rerun is byte-identical to card BRD's and reads 24 of its 26 trips as at least 90 % key-carried; cold short arms do not reproduce card ICL5's lone trips, so an instrumented 24 h pair runs in their place. Measure-only: the guard trace's statistic parts and the tracked sim postures. Nothing armed.
- **source:** card [BRD2](BRD2_broadcast_remainder_hold_days_2026-11-03.md) sections 2.4, 5.3, 6 and 7 (the remainder as the statistic decouplings through the bridge; the step test owed); card [BRD](BRD_broadcast_referenced_level_2026-11-02.md) (the posture); card [SMH](SMH_smoothed_hold_clone_2026-09-29.md) sections 9 and 12 (the bridge); card [LAH](LAH_level_aware_hold_2026-09-29.md) (the transfer); cards [SCK](SCK_sat_clock_step_2026-10-25.md), [SCR](SCR_sat_clock_step_response_2026-10-26.md) and [ARS](ARS_anchor_resolve_coupling_2026-10-26.md) (the step on the three postures, the guard's re-solve); cards ICL to ICL5 (branch `icl5/reseed-key-day`: the re-seed key, the frame's quorum, the lone trips of ICL5 section 4.3); standing invariants 1 and 7 of `CLAUDE.md`
- **knobs / posture:** no knob built or changed in this card. Measure-only: nine trailing columns of `logs/network_anchor_guard.csv` (section 3.1); the matrix postures `t` and `tk` of `scripts/scenario_matrix.py` (section 3.2). Pre-registered for the next build, default off: `network.clock_anchor_clock_k` (section 4). The graded posture is card BRD's T2 posture (the GPS broadcast-referenced posture of card NBP with `network.cycle_slip.if_phase_td_sat_centre` and `network.apriori_clock_track`); its sim twin is posture `t`.
- **dates in the text:** 2026-11-06
- **hashes / run dirs:** branch `level/anchor-decouple` from `brdc/brd2` (`4a0f554`, unmerged; `icl5/reseed-key-day` not merged, its runs read in place), worktree `D:/GnssProcessorSupport/worktrees/level_anchor`; exe `gnss_processor_lva1.exe` (sha256[:16] `e0a2aa899d2fa05b`, Release from the worktree: the trace columns and nothing else in the solve); the seeds' exe is card BRD2's `gnss_processor_b2k.exe` (`e44e043908d07826`, the branch's base source). Runs read: card BRD's `_rap_brd_{nt,r}{129,132}_6`, card BRD2's `_rap_brd2_{t,r,tk,rk}{130,131,132}_6`, card ICL2's `_rap_icl2_{p,c}{129,132}_24`, card ICL5's `_rap_icl5_{b,g}{129,132}_24`, card SMX's legs `evleg_smx_sck0b_storm_{n,c,g}`, `evleg_smx_ars0_storm_c`, `evleg_smx_smx1_sat_clk_{g,c,n}` (all under `D:/GnssProcessorSupport/runs_nom`). This card's runs: seeds `evseed_smx_t`, `evseed_smx_tk`; legs `evleg_smx_lvat_*`, `evleg_smx_lvatk_*` (labels `lvat`, `lvatk` under `D:/GnssProcessorSupport/gates/smx`); arms `_rap_lva_nt132_6`, `_rap_lva_g132s_{c,k}`, `_rap_lva_g132_24{c,k}` (the DOY-129 short arms `_rap_lva_g129s_{c,k}` were made and not run, section 5.3). Tools in `D:/GnssProcessorSupport/worktrees/lva_tools/` (outside the repository): `bridge_table.py`, `trip_share.py`, `patch_guard_trace.py`. Truth for every "against CODE" number: `COD0MGXFIN_2024{129,132}0000_01D_30S_CLK.CLK` under `C:/GnssProcessor/data`.
- **gates (frozen at pre-registration):** section 4.2, for the next build; this card's own predictions, written before their runs were read, in section 5

**Outcome.** Question: why does the smoothed hold's bridge lose centimetres of clock motion at a statistic decoupling, is there a hold or a test that keeps satellite clock step detection without that loss, and how do the re-seed key and the frame's quorum bear on it? Against CODE the bridge drops the LOOSE clock's own excursion as much as the a priori's drift, and on the CODE regime it is the right member: no bridge member recovers the remainder. The decoupling is the carrier, and every one of DOY 132's trips is a phase-bias-key event with a clock-space statistic below 0.6, where a true step reads 8. The next build is a clock-share requirement on the trip, pre-registered with its bars. The injected step on the tracked posture passes every channel with the test on and off, carried by the column and the track, and the instrumented rerun reads the trips as key events. Nothing armed.

> Results are appended TO THIS FILE, below the text.

---

## 1. The questions

Card BRD2 closed with the GPS broadcast-referenced posture of card BRD passing the user bars on all four days only
when the clock anchor's statistic test is disarmed (`network.clock_anchor_k` 1e6, its gate B2: census pooled +0.0021 m,
17 % of site-days worse), and read the DOY-132 remainder as the statistic decouplings passed to users through the
smoothed hold's bridge, where the bridge drops centimetres of the clock correction's motion (its section 2.4: before a
decoupling the clone carries a passed motion of decimetres on the tracked a priori, and the bridge passes the a priori's
motion alone). It left the posture measured and not field-runnable: a disarmed test needs the injected satellite clock
step before any field run (standing invariant 1). Three questions follow, in the order the author set them:

1. **The bridge.** Why does it lose that motion, and does a test or a hold exist that keeps satellite clock step
   detection without the loss?
2. **The lone trips.** Card ICL5 (section 4.3) found three Galileo trips of the golden's anchor that appear with the
   re-seed key and not without it (E26 at 550410 and E07 at 554610 on DOY 132, E08 at 311100 on DOY 129), with the same
   IF-TD flags in both runs and one satellite's phase statistic larger on the guard's first pass; its trace lacked the
   signal choice per band and the first pass's solved values.
3. **The key and the quorum.** How `network.cycle_slip.if_phase_td_reseed_on_flag` (cards ICL4, ICL5) and the frame's
   quorum (card ICL3) interact with the first question.

The method the brief fixed: derivation and offline reads of the existing runs first, then the fewest one-knob arms
that separate the hypotheses, every mechanism stated with a falsifying test, no bar relaxed, nothing armed.

## 2. Measured before the gates (2026-11-06, no new producer run)

### 2.1 Why the bridge loses the motion (the code, and one bridge read epoch by epoch)

The smoothed hold (`network/emit/PbHold.cpp`, `PbHoldClone.hpp`) writes a held phase bias P from card LAH's transfer
and the clone's passed motion. With c~ the satellite's emitted clock less the constellation's accumulated median step,
Phi = c(t|t) - c(t0|t) the clone's frame-free passed motion since its birth, C the emitted clock, M the common term and
K the generation's constant:

- a live clone: P(t) = P(t0) + [c~(t) - c~(t0)] - Phi(t), so the phase-visible clock pv = C - P = M + K + Phi(t)
  follows the solve's own clock;
- the bridge (the clone killed `Withdrawn` at the decoupling, the key on LAH's transfer): the passed motion the clone had
  reached is folded into LAH's reference (card SMH section 12), so Phi stays at its last value and pv = M + K + Phi(t_b-)
  follows the a priori alone, while P moves with the de-commoned clock step epoch by epoch;
- the rebirth, four epochs after re-engagement, starts a new passed motion Phi' = c(t|t) - c(t1|t) from zero, so from
  then on pv = M + K + Phi(t_b-) + Phi'(t).

The clock's motion over the bridge, D = c~(t_e) - c~(t_b-) = P(t_e) - P(t_b-) (t_e the last bridged epoch), is therefore
never passed: against the member that would have followed the solve throughout, the phase-visible clock is lower by D
for the rest of the generation. G10 on card BRD's `brd_nt132_6`, decoupled at 520710 (0.642 h): the clone's last row at
520680 (age 2160 s, Phi +0.0679 m); over 520680 to 520830 P rises by +23.56 mm while c_est rises by +23.32 mm, epoch for
epoch (+13.79 / +14.46, +1.40 / +1.26, -3.91 / -4.06, +1.76 / +1.57, +10.53 / +10.08 mm); at 520860 the clone writes again
with Phi +0.0035 m. D = +23.6 mm is the "G10 -24 mm at 0.642 h" of card BRD2's step table.

### 2.2 What D is made of, against CODE (`bridge_table.py`)

Per bridged satellite-event (the keys of a satellite share the transfer): D from the P records; the full clock
full = a + O + U + C_est (brdc + c_wire on the broadcast a priori, CODE + C on the CODE a priori) against the CODE 30 s
clock over the same window, de-commoned over the constellation's satellites not decoupled in the window. D = dfull +
drift, where dfull is the full clock's departure from truth over the bridge (the LOOSE clock's excursion, which the
a priori-following bridge rightly ignores) and drift is the a priori's own motion against truth over the bridge (which
the bridge writes into the phase side). After the bridge the clone follows the solve again, so the lasting phase-visible
error against truth, 20 epochs after the bridge's end, reads pv_net = dfull_after - drift for the hold as run and
pv_net + D = dfull + dfull_after for a bridge that froze P (the clock-following member). Bridges of 5 epochs
throughout (the anchor's 4 re-coupling epochs and the clone's birth at the next stash).

| run | bridges | |D| median / rms (mm) | LOOSE excursion dfull rms | a priori drift rms | pv error over the bridge: as run / frozen P | lasting error 20 epochs after: as run / frozen P | frozen better |
| --- | --- | --- | --- | --- | --- | --- | --- |
| `brd_nt132_6`, tracked, DOY 132 | 26 | 8.2 / 13.6 | 11.7 | 10.7 | 10.7 / 11.7 | **15.1 / 13.5** | 12 of 26 |
| `brd_r132_6`, regime on CODE, DOY 132 | 33 | 10.2 / 13.2 | 13.7 | 2.0 | 2.1 / 13.7 | **6.0 / 13.4** | 5 of 33 |
| `brd_nt129_6`, tracked, DOY 129 | 1 | 1.6 | 2.9 | 1.3 | 4.6 / 2.9 | 10.0 / 8.4 | 1 of 1 |
| `brd_r129_6`, regime, DOY 129 | 3 | 6.7 / 17.3 | 21.1 | 4.4 | 4.4 / 21.1 | 1.4 / 16.9 | 1 of 3 |
| `icl2_p132_24`, GPS + Galileo broadcast with the frame | 74 | 14.0 / 86.2 | 80.7 | 15.5 | 15.5 / 80.7 | 18.0 / 81.1 | 42 of 73 |
| `icl5_b132_24`, the same with the re-seed key | 45 | 10.7 / 24.8 | 21.8 | 12.9 | 12.9 / 21.8 | 16.4 / 23.2 | 30 of 44 |
| sim storm `sck0b_storm_n` (broadcast a priori, slips refloated) | 71 | 147 / 339 | 342 | 65 | 65 / 342 | 86 / 206 (40 epochs) | 17 of 54 |
| sim storm `ars0_storm_c` (CODE a priori) | 70 | 96 / 219 | 219 | 9.6 | 9.6 / 219 | 61 / 212 (40 epochs) | 21 of 69 |

On the regime on CODE the a priori is the truth to 2 mm, so D is the LOOSE clock's own excursion (median ratio 1.04) and
the bridge that ignores it leaves the phase side 2 mm off over the window and 6 mm 10 minutes later, against 13 mm for
the member that follows the clock. On the sim storm legs, where the trips come with refloated arcs and the re-solved
clock moves decimetres (card ARS), the same holds by a factor of 3 to 20. On the tracked a priori the a priori drifts
10.7 mm rms over a 5-epoch bridge (G21 +36, G10 +22, G30 -16, G14 -13, G16 +11 mm: the drift is largest at the
epochs of the mass trips, where the IF-TD column of a tripped satellite absorbs part of the station event that tripped
it), the same size as the excursion (11.7 mm), and the two members tie (15.1 against 13.5 mm lasting, better on 12 of
26). The lasting error after a decoupling on the tracked posture is 15 mm per decoupled satellite whatever the bridge
does, against 6 mm on the CODE regime: the decoupling itself, a LOOSE excursion that does not recover (the full clock
moves against truth by a median -0.33 D in the 10 minutes after, +0.27 D on the regime) plus the a priori's drift
written in, is the carrier.

**Refuted:** the hypothesis that a bridge member exists which keeps the phase-visible clock on the solve's path
through a decoupling at a gain. Freezing P (passing the clock's filtered motion, the only member available without
truth) is a coin flip on the tracked posture and a loss everywhere else. The planned census replay of a rewritten
wire (`bridge_table.py rewrite`, card ICL3's method) was not spent: with CODE as truth the per-event comparison already
decides it. The answer to the first half of question 1 is therefore that the bridge is doing what a hold without a
truth clock can do, and that the loss specific to the tracked a priori is the a priori's own drift over the bridged
epochs, 10.7 mm rms, matched by the excursion the other member would pass.

### 2.3 What tripped: the clock-space share of every statistic trip (`trip_share.py`)

The guard in phase mode trips on |w_phi| > k (k 6) alone; the clock-space statistic w_clk = (e - med) / sqrt(S) is
computed and traced beside it and reads nothing into the decision. From the decoupling events' detail:

| run | statistic trips | max |w_clk| | trips with |w_clk| >= 2 | max |e_clk| (mm) | sqrt(S_phi) at the 0.02 m floor |
| --- | --- | --- | --- | --- | --- |
| tracked DOY 129 / 130 / 131 / 132 | 1 / 7 / 2 / 26 | 0.01 / 1.12 / 2.09 / 0.57 | 0 / 0 / 1 / 0 | 0.4 / 32.7 / 55.7 / 18.4 | 1 of 1 / 7 of 7 / 2 of 2 / 15 of 26 |
| regime on CODE DOY 129 / 130 / 131 / 132 | 3 / 11 / 5 / 33 | 0.12 / 1.01 / 4.08 / 0.50 | 0 / 0 / 2 / 0 | 16.5 / 38.3 / 111.0 / 25.8 | 1 of 3 / 11 of 11 / 3 of 5 / 23 of 33 |
| GPS + Galileo broadcast DOY 132, 24 h, no key / re-seed key | 68 / 44 | 82.3 / 1.75 | 25 / 0 | 1169 / 56 | 44 of 68 / 19 of 44 |
| GPS + Galileo golden DOY 132 / 129, 24 h | 56 / 9 | 21.4 / 66.7 | 1 / 4 | 80 / 287 | 26 of 56 / 4 of 9 |
| sim storm `n` / `c` / `g` | 75 / 75 / 30 | 61.9 / 60.8 / 21.4 | 75 / 67 / 19 | 1281 / 1417 / 229 | 71 / 72 / 28 |
| sim step, row `sat_clk`, the G25 trip at +0 on `c` (q 5e-6) / `g` (q 2.9e-8) | 1 / 1 | **7.94** / 0.15 | 1 / 0 | 149 / 1.3 | 0 / 0 |

On DOY 132 all 59 trips of the two GPS-only postures are carried by the phase-bias keys: the anchored clock moved at
most 18 mm (tracked) and 26 mm (regime) against a clock-space sqrt(S) of about 0.05 m at q 5e-6, |w_clk| at most 0.57,
while the phase statistic read 0.12 to 29.9 m (the 29.9 m of G10 and G16 at 0.642 h on a sqrt(S_phi) of 3.73 m, a key
with a 3.7 m formal sd; 1.63 m on four satellites at once at 0.800 h; 0.20 to 0.77 m at the floor at 1.750 h). A true
clock step of 0.5 m at the same q moves the anchored clock 0.149 m in the epoch and reads w_clk -7.94 (card SCK's `c`
leg); the slips of the sim storm, which pull the clock through their refloated arcs, read 60. Over the four field days
35 of the 36 tracked trips and 50 of the 52 regime trips have |w_clk| below 2. On the GPS + Galileo broadcast posture the
25 trips with a clock share are the post-flag scatters of card ICL4 (e_clk up to 1.17 m), which the re-seed key removes
at the source; with the key 0 of 44 remain. At the golden's q (2.9e-8) the anchored clock cannot move within an epoch,
a 0.5 m step reads w_clk 0.15, and a clock-share rule would mask every step: the rule belongs to the broadcast regime's
q and is refused below it.

### 2.4 The second half of question 1: a test that keeps step detection

On the tracked posture a satellite-common clock step enters the IF-TD two-way centring as the satellite's column, the
track U takes it before the station rows are built and the wire adds it back (card BRD section 5b), so the step reaches
neither the slip test nor the anchor's statistic: the column and the track are the step response there, with the
`sat_centre` event as its edge, and the statistic test sees only what is not satellite-common. That is card BRD2's
sentence "under the tracked a priori a real satellite clock step is followed through the IF-TD column"; it is a claim,
and this card tests it with the injected step on the tracked posture (section 5.1, prediction P-step). What the test
must keep is the detection of a clock event the column cannot take (the regime on CODE without the track, a satellite
below the column's four stations, a step smaller than the thresholds at some stations), and section 2.3 says how: a
clock event moves the clock, and at q 5e-6 the clock-space statistic reads it (7.9 for 0.5 m), while the station and
key events of DOY 132 leave the clock still (at most 0.57). The next build is that requirement (section 4).

### 2.5 The lone trips of card ICL5 (question 2)

From the guard traces of `icl5_g132_24` (the re-seed key) and `icl2_c132_24` (the golden control) at 550410: every
Galileo row agrees between the runs within 0.2 mm in e_clk and 0.1 mm in e_phi, except E26, whose clock innovation is the
same (-2.50 against -2.66 mm, w_clk -0.37 against -0.40) and whose phase statistic reads 0.1249 m (w_phi 6.24, trip)
against 0.0516 m (2.58, ok) on the floored sqrt(S_phi) of 0.020 m. At 554610 E07: e_clk +1.19 against +1.19 mm, e_phi
0.1657 (8.29) against 0.0666 (3.33). The 7 to 10 cm that trip are in the phase-bias keys' innovations, sum_j w_j dpb_j
(the GPS L1 / L5 weights are +2.26 and -1.26; a 3 cm move of the L1 key or 5.5 cm of the L5 key suffices), which no trace
records: the guard trace held e_phi, sqrt(S_phi), w_phi and n_pb and not the pair the functional took (one key per band,
the best determined, GPS L1C against L1W and Galileo L1C against L1X, L5Q against L5X) nor the three innovations. The
events around each trip are the same in both runs (the CLK_BASIS reselect of E26 at 550020, a READMIT park, prior
residual reports of the same rows). The re-seed key changes the day's flag set by 22 to 32 % and halves the arcs T4 drops,
so the dense block's keys and their folded variances differ between the runs at every epoch; which key, and whether its
signal choice flipped, is what the measure-only columns of section 3.1 record. The short arms of section 5.3 start 1.4
and 1.9 h before the epochs with the key on and off; whether a lone trip reproduces from a cold start decides whether
the arm is a bed for it.

### 2.6 The key and the quorum against the bridge (question 3)

- **The re-seed key** (`network.cycle_slip.if_phase_td_reseed_on_flag`) answers the slip in the epoch after an IF-TD
  flag (card ICL4). On the GPS + Galileo broadcast posture of DOY 132 it removes 5 of the control's 16 decoupling epochs
  (529770, 546600, 581790, 582240, 592890: 25 satellite-events, the post-flag scatters, and the 25 trips with a clock
  share of section 2.3), adds one lone trip (E26 at 550410), and leaves card BRD2's station-event trips in place
  (520710, 521280, 521910, 524700, 524970, 527730: identical satellite sets with and without the key). Bridged
  satellite-events fall from 74 to 45 and their lasting error from 18.0 to 16.4 mm rms (section 2.2). The key and the
  bridge therefore do not interact: the key removes one class of decouplings at the source, the station-event class
  that carries card BRD2's remainder is untouched by it, and the bridge is not the lever for either (section 2.2).
- **The frame's quorum** (`network.phase_frame_quorum`, card ICL3) guards the phase-visible frame's vote when the clock
  anchor marks most of a constellation in one epoch; its one natural instance was a post-flag scatter, removed by the
  key (card ICL4 section 4.4). The clock-share requirement of section 4 withholds the statistic trips the keys alone
  carry, so a mass decoupling from the statistic no longer forces the keys out of the vote; the quorum keeps its role for
  the gap, datum and backstop decouplings and for any trip that passes the clock share. The bridge's member changes
  nothing in the vote (the frame votes on the emitted phase-visible correction, after the hold). The hold's level M at
  a mass decoupling (card ICL3 section 7) is likewise a decoupling question, not a bridge question.
- **Together on the GPS + Galileo posture:** the key removes the post-flag class, the clock share the station and key
  class; what both leave is the gap, datum and backstop set and the trips with a genuine clock share (DOY 131's one on
  the tracked GPS posture, the regime's two). That posture is on the ICL branches and is not run here.

## 3. What is built in this card (measure-only)

### 3.1 The guard trace's statistic parts

`logs/network_anchor_guard.csv` gains nine trailing columns, written on the guard's first pass for every row that
has a phase statistic and empty otherwise: `sigs` (the pair the functional took, one signal per band), `dclk` (the
clock's own innovation x_c - prior_c before the constellation median) with `clk_sd` (the clock key's folded standard
deviation), then for each chosen phase-bias key `w`, `dpb` (x_p - prior_p) and `pb_sd`. `clockAnchorPhiRow` carries
them (`include/network/solve/ClockAnchor.hpp`), `clockAnchorPhaseStatistic` fills them where it forms the functional,
and `clockAnchorGuardPartsCsv` writes them (`src/core/network/solve/ClockAnchor.cpp`). e_phi = dclk - sum_j w_j dpb_j
less the constellation median, so a trip is read apart into the clock's share and each key's. Nothing in the solve,
the decision or any product changes; the only reader of the trace (`scripts/clock_level/guardladder.py`) reads it by
column name.
Checks: the instrumented exe reproduces card BRD's `brd_nt132_6` byte for byte (section 5.2) and the lvat legs'
10-epoch prefix; Debug `gnss_tests` on the clock anchor and guard suites (`*ClockAnchor*:*Anchor*:*Guard*`): 142 tests
in 48 suites, 138 passed, 4 fixture-gated skips, 0 failed; the full Debug suite 3 863 tests in 528 suites, 3 830 passed, 33 fixture-gated skips, 0 failed (card BRD2's counts);
`make_golden_config.py --verify` PASS, `nom_epoch_lint.py` PASS, `config_posture_check.py --verify-loader` 0 gaps,
`docs_index_check.py` passes.

### 3.2 The tracked sim postures

`scripts/scenario_matrix.py` gains the postures `t` (card BRD's tracked posture on the sim bed: `n` plus
`network.cycle_slip.if_phase_td_sat_centre` and `network.apriori_clock_track`, both fingerprinted, so a seed of its
own) and `tk` (`t` with `network.clock_anchor_k` 1e6, card BRD2's candidate; `clock_anchor_k` is in the snapshot
fingerprint too), and `make --postures` accepts a posture the matrix does not list, graded on the matrix's rows and
bars. The matrix file is unchanged.

## 4. The next build, pre-registered

### 4.1 The mechanism

`network.clock_anchor_clock_k` (double, default 0 = off, byte-identical off). In phase mode
(`network.clock_anchor_guard_space` phase) a statistic trip requires, besides |w_phi| > `clock_anchor_k`, that the
clock-space statistic the guard already computes carries a share of the event: |w_clk| >= `clock_anchor_clock_k`
(2.0 in the arms). The absolute backstop, the satellite clock event's forced trip (`slip_clk_late`) and every other
decoupling cause are unchanged, and so is the clock-space mode. A phase trip the clock share withholds is an event at
the epoch (`CLK_ANCHOR phi_withheld`, with w_phi, w_clk, the pair and the keys' shares from section 3.1) and a counter,
with a run-end clause (trips withheld, the largest |w_phi| and |w_clk| among them; "0 withheld" is the normal reading on
a quiet day, not inert wiring); the guard trace's decision column reads `withheld`. The loader refuses the key when
`network.clock_anchor_q` is below 1e-6 m²/s (at the goldens' 2.9e-8 the anchored clock cannot move within an epoch and
the clock-space statistic cannot carry a step: section 2.3's 0.15 for 0.5 m), when the guard is not in phase mode, and
when the clock anchor is off. Ledger row UNVALIDATED; the knob is not in the snapshot fingerprint (it withholds a trip;
it writes no state), so a leg may resume a seed written without it. Unit tests: the withhold on a key-carried trip,
the trip kept on a clock-carried one, the backstop and the forced trip unchanged, the three refusals and their
accepting controls.

### 4.2 Gates (frozen 2026-11-06, before the build)

Grading as card BRD2's D: 6 h from 00:00, GPS, hour 1 ignition, hours 2 to 6 the settled read; census
`scripts/clock_level/gcensus.py` through `brd_census.py`, paired per site, the mean of the site's hourly p50 over hours
2 to 6, "worse" is worse by more than 0.02 m. The sim legs are card SMX's matrix (`scripts/scenario_matrix.py`,
`matrix.json` version 1, its bars unchanged), event minus null on one exe and one seed.

| gate | bar |
| --- | --- |
| X-U0 | Debug `gnss_tests`: no new failure, the new tests pass; `gnss_simulator_selftest` all pass; `make_golden_config.py --verify` PASS; `docs_index_check.py`, `nom_epoch_lint.py`, `config_posture_check.py --verify-loader` pass. |
| X0, off | The key off on the final exe: the 2 h field and sim goldens byte-identical to card BRD2's base-exe runs (`brd2_c129_2`, `brd2_s0_2`: `products.txt` without comment lines, `closed_loop_trace.csv`, `network_epoch_status.csv`), and the tracked posture, 2 h, DOY 132, byte-identical to `brd2_t132_2`. |
| X1, the refusals | The key with `clock_anchor_q` 2.9e-8 refused at load; accepted at 5e-6; refused in clock mode and with the anchor off (unit tests). |
| X2, the four days | The tracked posture with the key at 2.0, 6 h, DOY 129 to 132. Predicted from section 2.3: 0 trips taken on DOY 129, 130 and 132 and 1 on DOY 131. Bar: on every day on which no trip is taken, `products.txt` without comment lines byte-identical to card BRD2's test-off arm of that day (`brd2_tk129_6`, `brd2_tk130_6`, `brd2_tk132_6`: the solve has no other reader of the test), so B2's census stands for that day; on a day on which a trip is taken, the day's census against the GPS golden meets the bars of D (WTZZ00DEU at most +0.005 m, census paired median at most +0.005 m) and against the test-off arm reads within 0.003 m, with each taken trip named with its clock share. Pooled over the four days: the share of site-days worse at most 20 %. |
| X3, detection kept | Row `sat_clk` on the sim posture `c` (q 5e-6, the clone regime): the G25 trip at +0 is taken (its |w_clk| 7.94 against 2.0) and the leg's `products.txt` is byte-identical to the knob-off leg's on the same exe and seed; the row on `n` (no G25 trip either way) and on `t` (section 5.1) byte-identical likewise. |
| X4, the storm | Rows `storm` on `c` and `n` with the key: the trips with |w_clk| below 2 are withheld (predicted 8 of 75 on `c`, 0 of 75 on `n`) and every other trip taken; no cell of the matrix changes verdict from pass to fail against the knob-off label of the same exe; the clk and consumer channels of the two storm cells reported. |
| X5, the withheld set | On DOY 132 tracked: every withheld trip is a key-carried one by section 3.1's columns (the keys' share at least 0.9 of e_phi), none has |dclk| above 20 mm; the run-end clause counts 26 withheld. |

Reported without a bar: the regime on CODE with the key on DOY 131 and 132 (its two clock-carried trips of DOY 131
taken); the GPS + Galileo broadcast posture with the re-seed key and the clock share, if the branches are merged
(predicted 0 of 44 trips taken on DOY 132); hour 1 per arm; the decoupling, bridge and generation counts per arm. Owed
after the gates and before any arming: 24 h on DOY 129 and 132 against a 24 h GPS golden, DOY 128, and a real-time run.

### 4.3 What is not built, and why

- A bridge member (freezing P, or any hold that passes the clock's bridge motion): refuted in section 2.2.
- A clone kept alive across a LOOSE fold: equivalent to the frozen member once the fold decorrelates the clone's
  reference (card SMH section 9's reason for killing it stands), so refuted with it.
- A bound on the LOOSE excursion (a decoupling at a multiple of the anchored q instead of the legacy rate when the
  satellite's arcs are kept): it would shrink the 12 to 14 mm excursion of every false decoupling on both a prioris,
  and is the lever if a day shows clock-carried false trips that the clock share must keep; a solve change, left for
  its own card with the storm as its bed.
- The two-way centring's column at a flagged epoch (the a priori drift of 22 to 36 mm at the mass trips of DOY 132): a
  refit without the flagged samples is the natural instrument; its own card, after the clock share removes the trips
  those epochs cause.

## 5. This card's measurements (predictions written before the runs were read)

### 5.1 The injected clock step on the tracked posture (P-step)

Posture `t` and `tk` seeds (6 h sim from 00:30:15, `gnss_processor_b2k.exe`), row `sat_clk` (G25 +0.5 m at the
seed's ninth snapshot, 60 epochs, the event at epoch 10) and its null, both postures, on `gnss_processor_lva1.exe`.
Predicted before any leg ran: on both postures 0 IF-TD flags of G25 at +0 (the column takes the step), a `sat_centre`
event naming G25 at +0 with a column within 50 mm of 0.5 m, 0 `cta_decouple` of G25 at +0 and +1, no cold
re-realization of a G25 family at +0 or +1, the wire's clk channel within its bar, and the two postures' event legs
byte-identical in `products.txt` (no trip on either). A trip of G25 on `t` falsifies "the column pre-empts the
statistic test on the tracked posture".

**Result, posture `t` (test on), 2026-11-06.** Seed `evseed_smx_t` (720 of 720 emitted, 3 767 s under a shared
machine); legs `evleg_smx_lvat_{sat_clk,null60}_t` (label `lvat`, exe `gnss_processor_lva1.exe`, snapshot 485 at tow
275580, the step on G25 at 275910, S40000SIM the reference station of the IF-TD chain, S14000SIM its L2W anchor). At
the step epoch: one `T1_if_slip sat_centre` row names G25 with a column of 0.4942 m over 15 stations (5.8 mm short of
the step: the median polish's overall and row terms take the rest); the reference gate's reset at S40000SIM is re-tested
with the column and kept (`refsat_centre_keep`, step 0.0051 m against a gate of -0.489 m); 0 `if_slip` refloats of G25
in the whole leg; the guard reads G25 at w_phi -0.27 and w_clk -0.43 (ok), 0 decouplings of G25 (the leg's 4
decouplings are the null's 4, at other epochs); no handover or cold re-realization of a G25 family at +0 or +1. The
track takes the column (O + U -0.1771 to -0.6712 m), the correction moves -9.0 mm, the wire moves -0.5034 m at +0 and
-0.4996 m from +1. Grader: pre-event determinism 0.0 mm; clk 0.1 mm against a bar of 39.4 (the null's own spread S
13.1 mm, the track's per-epoch noise), pb 0.1 / 320.7, cons_peak 0.3 / 37.4, cons_plat 0.2 / 37.4, zwd 0.5 / 19.1, iono
0.6 / 224.5, emit 0 / 0: **PASS on every channel**, event minus null at the level of rounding. The consumer's own error
(0.70 m, card SCK section 9's leg consumer) is the same in both legs to 0.3 mm. P-step holds on `t`: the column and the
track are the step response on the tracked posture, and the statistic test does not see the step.

**Result, posture `tk` (test off), 2026-11-06.** Seed `evseed_smx_tk` (720 of 720, 3 044 s); legs
`evleg_smx_lvatk_{sat_clk,null60}_tk` (label `lvatk`; the first pair was stopped by this session's own mistake at 216 s
and re-run from the pristine snapshots). The same response: one `sat_centre` row for G25 (column 0.4942 m, 15
stations), the reference reset kept, 0 `if_slip` refloats of G25, 0 decouplings in either leg, the guard at w_phi
-0.24 and w_clk -0.43; the wire's G25 step, event minus null, -0.4990 m at +0 and -0.4995 m from +1 against -0.4991
and -0.4996 m on `t`. Grader: clk 0.1 / 41.0 mm, pb 0.1 / 318.3, cons_peak 0.3 / 30.3, cons_plat 0.1 / 30.3, zwd
0.5 / 19.6, iono 0.6 / 224.1, emit 0 / 0: **PASS on every channel**. The clause "the two postures' event legs
byte-identical" fails in its letter and not in its substance: the two legs resume two seeds (the anchor's k is in the
fingerprint), and at the resume epoch 275610 the test-on posture re-earns the anchor with four statistic trips (G24,
G10, G03, G32; the warm resume is a jump event for the guard, card SMH section 9) where the test-off posture trips
none, so the products differ from the first epoch by those four clocks; at the step epoch neither posture trips, and
the step responses agree to 0.1 mm. Card BRD2's field-blocking deferral is therefore answered on the sim bed: on the
tracked posture a satellite-common clock step is carried by the column and the track with the statistic test on or
off, and disarming the test loses no detection of such a step. What the test still guards on this posture is the
clock event the column cannot take, and section 2.3 gives the share that distinguishes it.

### 5.2 The instrumented rerun of the tracked DOY 132 (P-trace)

`lva_nt132_6` (the config of `brd_nt132_6`, `gnss_processor_lva1.exe`). Predicted: `products.txt` byte-identical to
`brd_nt132_6`; at each of the 26 trips the keys' share |sum_j w_j dpb_j| at least 0.9 |e_phi| and |dclk| at most 20
mm; at 0.642 h one chosen key of G10 and of G16 has a folded sd of at least 1 m.

**Result (2026-11-06).** `lva_nt132_6` (exit 0, 5 739 s at Normal priority on the shared machine): `products.txt`
without comment lines (171 378 lines), `closed_loop_trace.csv` and `network_epoch_status.csv` byte-identical to
`brd_nt132_6`, and the 26 decouplings identical in epoch, satellite and statistic: the trace columns change nothing.
The recomputed statistic dclk - sum_j w_j dpb_j less the constellation median equals the traced e_phi to 0.56 mm on
every trip. The composition (`trip_parts.py`, `lva_tools/parts_nt132.csv`), the GPS weights +2.546 (L1) and -1.546
(L2):

| epoch (h) | satellites | pair | dclk (mm) | the keys' innovations (m) and sd | keys' share of e_phi |
| --- | --- | --- | --- | --- | --- |
| 0.642 | G10, G16 | L1C + L2W | +15, +10 | L2W +19.21 and +19.21 (sd 2.91 m); L1C -0.07 (sd 0.90) | 1.00, 1.00 |
| 0.800 | G05, G14, G22, G30 | L1C + L2W | -6 to 0 | L1C -0.65 on all four (sd 0.54); L2W within 0.01 (sd 1.19) | 1.00 to 1.01 |
| 0.975 | G05, G14, G22, G30, G21 | L1C + L2W | 0 to +6 | L1C -2.23 to -2.25 and L2W -3.81 to -3.88 on all five (sd 0.34, 0.56), nearly cancelling to -0.20 | 1.00 to 1.03 |
| 1.750 | G06, G30, G08, G14, G25 | L1C or L1W + L2W | -9 to +20 | one key 0.06 to 0.13 (sd 0.22 to 0.38) | 0.95 to 1.06 |
| 1.825 | G06, G30, G08, G14, G25 | the same | -10 to +1 | one key 0.06 to 0.10 (sd 0.22 to 0.37) | 0.97 to 1.07 |
| 2.592 | G13, G15, G23, G30 | L1W or L1C + L2W | +9 to +22 | L2W +0.11 to +0.17 (sd 0.28); G30 L1C +0.17 | **0.81, 0.85**, 0.91, 1.06 |
| 3.917 | G12 | L1C + L2W | +3 | L2W -0.19 (sd 0.21) | 1.00 |

Against the prediction: 24 of the 26 trips have a keys' share of at least 0.9; G15 and G13 at 2.592 h read 0.81 and
0.85 with the clock's own innovation at +21.7 and +18.9 mm, so the clause "at least 0.9 on every trip and |dclk| at
most 20 mm" fails on those two by 1.7 mm and 0.05 to 0.09 of share; the third clause holds (G10's and G16's L2W key at
sd 2.91 m, and the 0.800 h group's L2W key at 1.19 m besides). Three classes: a key born or re-realized with a
metre-level sd whose first innovation is metres (0.642 h: +19.2 m on an L2W key of sd 2.9 m); one station's arcs
moving a key of every satellite it tracks by the same amount (0.800 h: -0.65 m on four L1C keys; 0.975 h: -2.2 and
-3.8 m on both keys of five satellites, nearly cancelling in the ionosphere-free functional); and decimetre moves of
one key at the BSHM00ISR re-entries and after (1.750 to 2.592 h). In none does the clock's own innovation exceed 22 mm
(w_clk at most 0.57 against a clock-space sd of 36 to 95 mm): every trip is withheld by a clock share of 2.0, and the
clock-carried event the rule must keep (a 0.5 m step at this q: dclk -149 mm, w_clk -7.94) is four times the largest
of these.

### 5.3 The short arms around card ICL5's lone trips (P-lone)

The GPS + Galileo golden of DOY 132 from 07:30:15 for 3 h and of DOY 129 from 12:30:15 for 2.5 h, control and
re-seed key, `gnss_processor_lva1.exe`. Predicted: if a trip of E26 at 550410, E07 at 554610 or E08 at 311100
reproduces in the key arm and not in the control, its statistic is carried by one phase-bias key (|w_j dpb_j| above
0.9 |e_phi|) with dclk within 5 mm of the control's row; if none reproduces, the cold short arm is no bed for a trip
that depends on the day's state, and the 24 h instrumented rerun is the owed step.

**Result, DOY 132 (2026-11-06).** `lva_g132s_c` and `lva_g132s_k` (07:30:15, 3 h, 360 epochs each, exit 0). Neither
reproduces a lone trip: E26 at 550410 reads e_phi +0.0002 m in both arms (the 24 h control 0.0516, the 24 h key arm
0.1249) with the pair L1X + L5X, dclk -9.8 mm, dpb -14.8 and -17.7 mm on keys of sd 0.52 and 0.92 m; E07 at 554610
reads +0.0027 m in both (0.0666 and 0.1657 in the day runs). The second branch of P-lone holds: a cold start 1.4 h
before the epoch does not carry the day's key state, and the DOY-129 pair (E08 at 311100, 1.9 h after a cold start) was
stopped unrun for the same reason. The instrumented 24 h reruns of the DOY-132 golden pair, `lva_g132_24c` (the
control's config) and `lva_g132_24k` (the re-seed key), were started in their place; their rows at 550410 and 554610
are appended below when they land. The columns themselves check: the recomputed statistic equals e_phi to 0.16 mm on
every row. What the short arms do show, as a side reading: in their first 1.5 h (ignition) the golden's anchor trips 7
(control) and 5 (key) times, every trip carried 99 to 100 % by one phase-bias key (dpb 0.65 to 4.9 m on keys of sd 0.23
to 0.95 m, the L5X or L5Q key in six of seven) with the clock still (|dclk| at most 13 mm): on the golden's q as on the
regime's, the statistic test's trips are key events.

**Result, the 24 h pair, E26 at 550410 (read mid-run, 2026-11-06 21:50).** `lva_g132_24c` and `lva_g132_24k`
reproduce the day runs' statistics exactly (e_phi +0.0516 and +0.1249 m) and read them apart:

| run | pair | e_phi (m) | dclk (mm) | L1C key: dpb (mm), sd (mm) | L5X key: dpb (mm), sd (mm) | keys' share |
| --- | --- | --- | --- | --- | --- | --- |
| control | L1C + L5X | +0.0516 | -2.9 | **-26.8**, 45.0 | -5.3, 156 | 1.05 |
| re-seed key | L1C + L5X | +0.1249 | -2.8 | **-59.3**, 60.5 | -5.3, 157 | 1.02 |

The pair is the same, the clock's own innovation is the same (-2.9 against -2.8 mm, clock sd 19 mm) and the L5X key
moves alike; the trip is E26's L1C key moving -59 mm where the control's moves -27 mm, on a key whose folded sd is 60
against 45 mm, and the next epoch's statistic decays in both runs (+0.025 and +0.011 m). E26's own record is identical
between the runs up to that epoch (the same six stations in the solve, the same anchor handovers, the same flags and
re-entries, one IF-TD flag in the day). What differs is the station the L1C key is anchored at since 544800,
NIUM00NIU: the key arm refloated 41 of its arcs in the four hours before against the control's 26 (16 against 10 in
the last half hour), the concentration card ICL5 section 4.5 reported for NIUM00NIU, SOLO00SLB and DARW00AUS. A
reading consistent with the numbers: the extra refloats leave the station's states less determined, the key anchored
there inherits the larger variance, and the same epoch's data move it 2.2 times as far, over the 0.12 m bar on the
0.02 m floor. The clock did not move (w_clk -0.15 on the golden's 19 mm clock sd), the trip costs no receiver (card
ICL5), and the clock-share rule of section 4 does not apply at the golden's q. The reading's falsifier, not run
tonight: the key arm with the extra refloats at NIUM00NIU alone taken back (or NIUM00NIU dropped) should leave the key
at the control's sd and the statistic under the bar. E07 at 554610 is appended when the runs pass it.

**Result, the 24 h pair, E07 at 554610 (read mid-run, 22:10), and the reading corrected.** The same pattern to the
digit: pair L1C + L5X in both runs, the clock's own innovation +1.3 mm in both (clock sd 17 mm), the L5X key +1.2 mm
in both, and the L1C key -28.3 mm in the control (sd 42.3 mm, e_phi +0.0666) against -72.2 mm in the key arm (sd 57.2
mm, e_phi +0.1657, trip); one epoch later both read within 4 mm of zero. E07's eight stations and its anchors (L1C at
MRC100USA, L1X and L5X at BOGT00COL) are the same in both runs, and MRC100USA has no IF-TD refloat in either run in
the four hours before, so the station-specific reading given for E26 above (its anchor NIUM00NIU's extra refloats)
does not carry to E07 and is withdrawn as the mechanism. What the two trips share: the key arm refloats more arcs
across the network (228 against 181 in the four hours before 554610, +26 %; card ICL5: +22 to +32 % over the day),
its phase-bias keys carry larger folded variances (E26's L1C sd 60 against 45 mm, E07's 57 against 42 mm, variance
ratios 1.8), and the same one-epoch L1C transient that moves the control's key by -27 and -28 mm moves the looser key
2.2 and 2.6 times as far, over the 0.12 m bar on the 0.02 m floor. The clock did not move in either trip. The
falsifier of this reading, not run tonight: the control's L1C keys re-inflated to the key arm's variances at those
epochs should reproduce the trips; a key arm whose extra refloats are confined to stations that do not track E26 or
E07 should not.

## 6. Verdict (2026-11-06)

**The bridge is not the lever; the decoupling is. Every statistic trip of the day that fails is a phase-bias-key event
with the clock still, the tracked posture carries an injected clock step through its column with the test on or off,
and the next build is a clock-share requirement on the trip, pre-registered with frozen bars. Nothing is armed.**

1. **Why the bridge loses the motion (question 1).** While a clock is decoupled the smoothed hold's key moves its held
   phase bias by the de-commoned clock step and the clone, reborn after re-engagement, starts a new passed motion, so
   the phase-visible clock follows the a priori through the bridge and the correction's motion D over it is never
   passed (section 2.1; G10's +23.6 mm is card BRD2's step). Against CODE, D is the LOOSE clock's own departure from
   truth plus the a priori's drift (section 2.2). On the CODE regime the drift is 2 mm and the a priori-following
   bridge is the right member: 6 mm of lasting phase-visible error against 13 mm for a bridge that follows the clock,
   and on the sim storms 61 to 86 against 206 to 212 mm. On the tracked a priori the drift (10.7 mm rms per 5-epoch
   bridge, up to 36 mm at the mass trips, where the IF-TD column absorbs part of the station event) equals the
   excursion (11.7 mm) and the two members tie, 15.1 against 13.5 mm, better on 12 of 26. **Refuted:** a hold that
   keeps the phase side on the solve's path through a decoupling at a gain; no bridge member recovers the remainder.
   What a decoupling costs the tracked posture, 15 mm lasting per decoupled satellite against 6 mm on CODE, is the
   decoupling itself: a LOOSE excursion that does not recover plus the a priori's drift written in.
2. **A test that keeps step detection.** All 59 statistic trips of DOY 132 on the two postures are carried by the
   phase-bias keys: the clock's own innovation at most 22 mm, |w_clk| at most 0.57, while a true 0.5 m step at the same
   q moves the anchored clock 0.149 m and reads 7.94 (section 2.3; the composition of the 26 tracked trips in section
   5.2: 24 of 26 at least 90 % key-carried, the other two at 81 and 85 %; three classes, a key born with a metre sd,
   one station moving a key of every satellite it tracks, decimetre single-key moves at the BSHM00ISR re-entries). Over
   the four field days 35 of 36 tracked trips and 50 of 52 regime trips sit below a clock share of 2; the post-flag
   scatters of the GPS + Galileo day, which do move the clock, are the re-seed key's class. The requirement is
   pre-registered as `network.clock_anchor_clock_k` with the gates of section 4.2, valid at the broadcast regime's q
   and refused at the goldens'.
3. **The step on the tracked posture (P-step, section 5.1).** On the sim bed the injected +0.5 m step is taken by the
   two-way centring's column (0.494 m over 15 stations), the reference gate keeps its arc, nothing refloats, the
   anchor does not trip, the track carries it and the wire moves -0.499 m against the null from the step epoch:
   every matrix channel passes with the test on and with it off, event minus null at the level of rounding, the two
   responses equal to 0.1 mm. Card BRD2's deferral (standing invariant 1 on the test-off posture) is answered for the
   satellite-common step; the statistic test is not the step detector on this posture.
4. **The lone trips (question 2, section 5.3).** The measure-only columns read the statistic apart to 0.16 mm and show
   every ignition trip of the golden's short arms as a key event; a cold start 1.4 h before the epoch does not carry the
   day's key state, so the lone trips are read from the instrumented 24 h pair, which is running at close and is
   appended below when it lands.
5. **The key and the quorum (question 3, section 2.6).** Independent of the bridge: the key removes the post-flag class
   (5 of 16 decoupling epochs on the GPS + Galileo day, the 25 clock-carried trips), none of the station-event trips
   that carry card BRD2's remainder; the clock share withholds those; the quorum keeps its role for the gap, datum and
   backstop decouplings.

Nothing is armed; arming, any golden change and the merge are the author's decisions.

## 7. What remains

- **The next build** (section 4): `network.clock_anchor_clock_k` with gates X-U0 and X0 to X5; then 24 h on DOY 129 and
  132 against a 24 h GPS golden, DOY 128 and a real-time run before any arming (card BRD2 section 7).
- **The lone trips.** The DOY-129 trip (E08 at 311100) needs its own 24 h instrumented pair; the falsifier of section
  5.3's reading (the key arm's looser keys carry the same one-epoch L1C transients over the bar) is named there.
- **The day's composition on the golden.** `lva_g132_24c` and `lva_g132_24k` were at epoch 1 341 of 2 880 at this
  card's commit (running at Normal priority on a machine shared with five other sessions' runs); their whole-day trip
  composition (`trip_parts.py --all-trips`; over the first 9.4 h 52 and 53 trips, 49 of each at least 90 % key-carried,
  21 rows on a key of sd at least 1 m) is read when they finish.
- **The clock-carried trips** of DOY 131 (one tracked at 2.09, two on the regime at 4.08, e_clk 56 and 111 mm): real
  clock events or not, read with the new columns when X2 runs that day.
- **The column at a flagged epoch.** The tracked a priori drifts 22 to 36 mm over the bridges of the mass trips: a
  refit of the two-way centring without the flagged samples is the instrument, after the clock share removes the
  trips those epochs cause.
- **The LOOSE excursion itself** (12 to 14 mm per decoupling on both a prioris, no recovery): a decoupling at a
  multiple of the anchored q when the satellite's arcs are kept, with the storm as its bed, if a day shows
  clock-carried false trips the clock share must keep.
- **The warm-resume seam on the tracked posture:** four statistic trips at the first resumed epoch of every `t` leg
  (the guard re-earning the anchor), none on `tk`; a seam question for the resume cards, not a field one.
- **GPS + Galileo** with the re-seed key and the clock share, once the ICL branches and this one meet.
