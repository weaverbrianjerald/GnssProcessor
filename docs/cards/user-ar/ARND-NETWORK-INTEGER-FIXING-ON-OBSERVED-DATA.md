# ARND: network integer fixing, the observed-data side

**STATUS**

- **id:** ARND
- **topic:** user-ar
- **status:** open
- **summary:** Opened on 2026-11-14 when the author lifted the do-not-build on integer fixing in the network and ordered the ring-by-ring fixing built, simulator first, then observed data, then more systems and frequencies. The build and its simulator stages are recorded on the builder's branch (`irdu/user-integers`); this card holds the observed-data side: the starting point, then the stage's pre-registration and the re-grades made here. Starting point, read on 2026-11-14 from runs of 2026-11-13 (DOY 144, the chain as built then, emission holds off, 17 users, hours 2 to 6): between satellites that the chain's reference receiver tracks itself, users' ambiguities lean toward integers on Galileo L1X/L5X only (wide lane 0.103, N_1 0.163 cycle against a null of 0.25; two receivers at 0.10 to 0.12 on N_1) and sit at the null on GPS, whose wide lane is not an integer at any distance. The simulator's 0.02 cycle at one link is not reproduced on observed data. Read again with the users averaged out over 73 receivers (sections 3 and 4): the stream's wide lane is integer one link from the reference receiver on Galileo L1X/L5X (0.048) and degrades with every further link (0.088, 0.125), the chain's signature; N_1 is not integer even one link away (0.17 to 0.21, and 0.15 to 0.18 at the reference receivers replayed as users); GPS L1W/L2W hangs on two reference receivers and has no common frame; users that start two hours in read the same.
- **dates:** 2026-11-14 (opened; section 2 read from existing runs; section 3 with 56 more observers), 2026-11-15 (section 4, users from 02:00; sections 5 to 11.1, the re-grades of the builder's stages, the network's own links, the pre-registration of the observed-data stage)
- **knobs / posture:** none new.
- **code:** none. Analysis tool `nw_depth.py` beside the tools of cards ARN and ARNB. Sections 3 and 4: `nw_depth_offsets.py`.
- **run dirs:** `D:\GnssProcessorSupport\runs_nom\_rap_nwa1nh_n144` and `_rap_nwa1_n144` (chain network runs of 2026-11-13), users `_rap_nw_obsa1nh_144_<site>` and `_rap_nw_obsa1_144_<site>`; tools `D:\GnssProcessorSupport\presentation\agu26\fixing_control\tools\`. Sections 3 and 4: `_rap_nw_obsna1nh_144_<site>`, `_rap_nw_obsna1nhs2_144_<site>`, `_rap_nw_obsa1nhs2_144_<site>`.
- **source:** the author's decisions of 2026-11-14 (section 1); cards [ARN](ARN-USER-FIXING-ON-NETWORK-PRODUCTS.md), [ARNB](ARNB-USER-FIXING-ON-NETWORK-PRODUCTS-CONTINUED.md) and [ARNC](ARNC-USERS-OF-SATELLITES-WITHOUT-BIAS-RECORDS.md)

**Outcome.** Open; continued in card [ARNE](ARNE-NETWORK-INTEGER-FIXING-CONTINUED.md). Simulator: the builder's
rule (free order, an odds gate, held-first re-attachment) holds no wrong integer on any bed, GPS and Galileo,
and brings the stream between satellites to 0.01 cycle from the second hour (sections 5, 6, 9, 9.1). Observed
data, nothing held yet: the Galileo L1X/L5X network is integer-like without fixing and the GPS one is not; the
GPS offsets are one term per receiver plus one per satellite, the mark of each vertex's one tree link (sections
7, 11, 11.1). The observed-data stage is pre-registered in section 10 and starts with a log-only pass.

> Results are appended TO THIS FILE, below the text.

---

## 1. Scope and the author's decisions of 2026-11-14

The author, in chat, late on 2026-11-14:

- "Build the safe ring-by-ring fixing on the simulator. Integer fixing in the network is no longer "do not build",
  in fact, it seems like the current bottleneck we need immediately!"
- "Continue to manage Opus to work on the integer network and our ring method, sim, then observed data, expand to
  multi-system, multi-frequency if everything works."

These lift the verdict of 2026-10-07 against integer resolution in the network (card ARL, section of 2026-10-24)
for this work. The reasons measured the same day on the simulator are on card IRDU (sections 12.x): on the chain,
links held at their true integers put every distance from the reference receiver at truth level, a ring reaches
the reference level once its own ring is held, re-seats onto integer links are free, and two decision rules made
no wrong fix when only logged. The rulings of 2026-11-09 otherwise stand.

The order is the author's: simulator, observed data, more systems, more frequencies, each only if the one before
works. The builder's branch carries the build, its own card and the simulator stages. This card carries what is
read and re-graded on observed data. No reference configuration, no ledger verdict and no card status changes
here; nothing is merged.

## 2. 2026-11-14: the starting point on observed data, read by distance from the reference receiver

Read from runs already written (2026-11-13, card ARN's chain arm: six event rows, receiver floor off, receiver
phase bias and wet delay carried, pair family, GPS and Galileo, 56 receivers, 6 h from 00:00 of DOY 144; users are
the 17 test receivers with no fix accepted). No run. Tool `nw_depth.py`: the network's `network_chain_edges.csv`
gives the tree per signal and epoch; every satellite pair of a user's epoch is classed by the largest depth of its
two satellites on the user's two signals (1 = tracked by the reference receiver itself). Mean |fraction| in
cycles, wide lane then N_1, hours 2 to 6; uniform null 0.25.

**The trees.** One root per signal for the whole run: COCO00AUS for Galileo L1C and L5Q, BOGT00COL for Galileo L1X
and L5X, BOR100POL for GPS L1C and L2W, LEIJ00DEU for GPS L1W. Satellites at depth 1, 3, 5, 7: for example 7, 10,
11, 4 on GPS L1C and 10, 15, 3, 1 on Galileo L1X at the first hour.

**Emission holds off (`_rap_nwa1nh_n144`):**

| signals | depth 1 | depth 3 | depth 5 | depth 7 and more |
| --- | --- | --- | --- | --- |
| Galileo L1X/L5X | 0.103, 0.163 (11 411 pair-epochs) | 0.148, 0.230 | 0.170, 0.254 | 0.152, 0.225 |
| Galileo L1C/L5Q | 0.248, 0.191 (2 486) | 0.196, 0.246 | 0.241, 0.243 | 0.205, 0.241 |
| GPS L1C/L2W | 0.191, 0.229 (2 653) | 0.234, 0.252 | 0.242, 0.248 | 0.241, 0.250 |
| GPS L1W/L2W | 0.266, 0.239 (2 418) | 0.244, 0.249 | 0.242, 0.251 | 0.242, 0.254 |

**By receiver at depth 1** (200 pair-epochs or more): Galileo L1X/L5X at STPM 0.097, 0.102 and at ASCG 0.080,
0.119; at QUIN 0.126, 0.194; at WTZZ 0.084, 0.260; at UTQI 0.229, 0.121. GPS L1C/L2W at ASCG 0.086, 0.216; at
STPM 0.176, 0.224; GPS L1W/L2W at WTZZ 0.282, 0.245.

**Emission holds on (`_rap_nwa1_n144`):** at the null at every depth (depth 1: 0.20 to 0.24 on the wide lane, 0.21
to 0.28 on N_1), as the hold's arbitrary levels require.

**Reading.**

1. One link from the reference receiver the chain shows a partial integer signal on Galileo L1X/L5X and it fades
   with distance, as on the simulator; its level (0.10 on the wide lane, 0.16 on N_1; 0.10 to 0.12 on N_1 at two
   receivers) is far from the simulator's 0.02.
2. GPS shows none. Its wide lane is not an integer even at depth 1, and a wrong GPS wide lane moves the N_1
   reading by 0.47 cycle, so GPS N_1 cannot be read here. The stream carries code-bias records for C1C, C1W and C2W
   on every GPS satellite; why the users' GPS wide lanes are off is not traced (card RSCB's receiver-dependent
   satellite code bias is one candidate).
3. The reading covers the network's first six hours and users that started with it, on the arm as it stood on
   2026-11-13.

**What it means for the observed-data stage.** Grade by distance from the reference receiver and by receiver, the
wide lane before N_1, GPS and Galileo apart; start users after the stream has settled; the GPS wide lane at the
user is the first obstacle and is not a matter of the network's N_1.

## 3. 2026-11-14 (night): the starting point again, with the users averaged out and the reference receivers as users

Section 2 reads single users, whose own float limits the reading. Two sharper readings on the same stream
(`_rap_nwa1nh_n144`, emission holds off), still without network fixing.

**Runs.** The network's 56 receivers replayed as users of the stream, no fix accepted, from 00:00
(`_rap_nw_obsna1nh_144_<site>`, all exit 0, `gnss_processor_rscb3.exe`). With the 17 test receivers of section 2
that makes 73 observers.

**Reading A: the stream itself** (`nw_depth_offsets.py`). Users that track the same signals share one fraction per
satellite (card ARN). Per signal pair and 30 min bin that fraction is fitted over every observer of the group, so
the users' own noise averages out; between two satellites of an integer stream the two fitted offsets would be
equal. Mean |difference of the two offsets| in cycles, wide lane then N_1, hours 2 to 6, classed by the pair's
largest depth; two unrelated offsets give 0.25:

| signals (receivers) | depth 1 | depth 3 | depth 5 | depth 7 and more |
| --- | --- | --- | --- | --- |
| Galileo L1X/L5X (39) | 0.048, 0.170 | 0.088, 0.231 | 0.125, 0.256 | 0.074, 0.251 |
| Galileo L1C/L5Q (34) | 0.165, 0.178 | 0.190, 0.235 | 0.242, 0.237 | 0.173, 0.227 |
| GPS L1C/L2W (54) | 0.132, 0.206 | 0.234, 0.246 | 0.241, 0.247 | 0.239, 0.238 |
| GPS L1W/L2W (19) | 0.256, 0.262 | 0.247, 0.249 | 0.254, 0.244 | 0.259, 0.254 |

(154 to 322 pair-bins at depth 1, 55 on GPS L1W/L2W; 1 060 to 2 428 at depth 3.)

**Reading B: a reference receiver as a user of its own stream** (`nw_depth.py --by-site`, depth 1, hours 2 to 6):

| reference receiver | signals | wide lane | N_1 |
| --- | --- | --- | --- |
| BOGT00COL | Galileo L1X/L5X | 0.068 | 0.184 |
| COCO00AUS | Galileo L1C/L5Q | 0.136 | 0.148 |
| BOR100POL | GPS L1C/L2W | 0.134 | 0.180 |
| LEIJ00DEU | GPS L1W/L2W | 0.272 | 0.264 |

**What the two readings say.**

1. *The chain's signature is there on observed data, on the wide lane.* On Galileo L1X/L5X the stream's wide lane
   is integer one link from the reference receiver (0.048, 89 % within 0.10) and degrades with every further link
   (0.088, 0.125), as the narrow lane does on the simulator. GPS L1C/L2W and Galileo L1C/L5Q lean the same way at
   depth 1 (0.132, 0.165) and are at the null beyond.
2. *N_1 is not integer even one link away:* 0.17 to 0.21 cycle between satellites that the reference receiver
   pins itself, with the users averaged out, and 0.15 to 0.18 at the reference receivers replayed as users. The
   simulator reads 0.02 there. A per-satellite term at the reference receiver that every user then sees is what
   this reading measures; its wet delay through each satellite's elevation is the first candidate (the hop term of
   card IRDU), not traced here.
3. *GPS L1W/L2W has no common frame.* Its two signals hang on two reference receivers (L1W on LEIJ00DEU, L2W on
   BOR100POL with the L1C/L2W pair), so no pair of its satellites is one link away on both signals; it reads at the
   null everywhere, the reference receiver included.
4. *The users of this run started with the network.* Whether the first two hours' settling limits the reading is
   tested next (section 4).

**For the stage.** Readings A and B are the observed-data graders of the stream, beside the users' held integers:
truth-free, by distance, the wide lane before N_1. The expectation they set for network fixing: depth 3 and beyond
come to the depth-1 level, and the depth-1 level itself must fall from 0.17 to 0.21 on N_1 before a user can fix.

## 4. 2026-11-15: does the network's settling limit the reading? Users that start two hours in

On the simulator a user that starts while the stream still settles keeps that error (card ARNB section 15). The
same question on observed data, on the stream of sections 2 and 3.

**Runs.** The 73 observers again, started at 02:00 and run for four hours
(`_rap_nw_obsna1nhs2_144_<site>`, `_rap_nw_obsa1nhs2_144_<site>`, all exit 0). Reading A of section 3 on 03:00 to
06:00, against the observers of section 3 read on the same window. Wide lane then N_1, cycles:

| signals | depth 1, start 02:00 | depth 1, start 00:00 | depth 3, start 02:00 | depth 3, start 00:00 |
| --- | --- | --- | --- | --- |
| Galileo L1X/L5X | 0.051, 0.187 | 0.051, 0.143 | 0.100, 0.241 | 0.101, 0.234 |
| Galileo L1C/L5Q | 0.210, 0.177 | 0.207, 0.206 | 0.194, 0.231 | 0.194, 0.237 |
| GPS L1C/L2W | 0.155, 0.240 | 0.161, 0.201 | 0.236, 0.244 | 0.239, 0.245 |
| GPS L1W/L2W | 0.283, 0.243 | 0.275, 0.262 | 0.252, 0.243 | 0.253, 0.244 |

**Reading.** The start makes no difference on the wide lane (0.051 both ways on Galileo L1X/L5X) and none in
favour of the late start on N_1 (0.143 to 0.206 for the early users, 0.177 to 0.240 for the late ones, who have had
less time). The settling of the network's first two hours is not what keeps N_1 from integers one link from the
reference receiver on observed data: the term is in the stream throughout.

**Not verified.** One day and one stream; what the per-satellite term at the reference receiver is (section 3,
item 2).

## 5. 2026-11-15: the builder's first simulator stage, re-graded here, and the decisions taken in the author's absence

The build is card NRF on the builder's branch (`network.ring_fix.*`, default off; commits f2a5e00 and 335235e, not
merged). Its stage N1 ran on the simulated bed without re-seats (2 h). This section is the re-grade made here and
the manager's decisions, for the author's review; card NRF holds the build's own record.

**Stage N1 as reported.** The frozen rule (wide lane rounded first; formal sigma of N_1 at most 0.10 cycle and
fraction at most 0.20; a 600 s window without an event; the same integers at 10 epochs; the lattice as a veto;
strict ring order) held 11 of 186 links in 2 h, the first at 40.5 min, none wrong; the audit catches the wrong
holds of a plain-rounding control (40 138 of 41 903 held link-epochs); off is identical. The stream and the users
read the plain chain run's level.

**Re-graded here** (`nrf_regrade.py`, from the run's `network_ring_fix.csv` and the file of true integers, without
the trace's own audit column): 777 held link-epochs, 0 wrong. The frozen gate alone, log-only, passes 0, 4, 7 and
19 links at 30, 60, 90 and 119.5 min, none wrong. The median formal sigma of N_1 by ring at 119.5 min is 0.113,
0.152, 0.198 and 0.191 cycle (rings 2 to 5), against the bound of 0.10. Plain rounding of the floats is right on
173, 183 and 186 of the 186 links at 60, 90 and 119.5 min: the floats are right, the rule waits.

**A reading added here.** The same trace, log-only, with the acceptance set on the odds: the likelihood ratio
between the nearest and the next integer of N_1, (1 - 2 d) / (2 sigma^2) >= ln(odds) with d the fraction, and d at
most 0.20.

| odds | links passing at 30, 60, 90, 119.5 min | passing row-epochs | wrong |
| --- | --- | --- | --- |
| 1e9 | 2, 8, 28, 51 | 3 785 | 0 |
| 1e6 | 4, 17, 50, 92 | 6 827 | 0 |
| 1e4 | 9, 27, 89, 126 | 10 661 | 0 |
| the frozen gate | 0, 4, 7, 19 | 1 172 | 0 |

**Decisions taken in the author's absence** (2026-11-14 night to 2026-11-15), each open to review:

1. The fixing build was put ahead of the unfinished user-side rule of card ARNC's item 1 (committed default off on
   the builder's branch, its tests not run) and of the queued all-integers run, which became the build's ceiling
   control. The builder's session was stopped twice so that the new priority reached it at once.
2. Stage N1 is taken as passed on safety and as no gain: 11 links in 2 h move nothing.
3. Strict ring order is relaxed: a link is decided when it passes, the most certain first and a few per epoch; the
   rings are how progress is reported. One slow inner link held back 27 050 link-epochs.
4. A second acceptance, on the odds at one in a million, is pre-registered as an arm of its own beside the frozen
   gate. An arm with any wrong hold is rejected, not adjusted.
5. The build for re-seats (stage N2) starts without waiting for the re-grade of the stage before it, and gains a
   steady-state case: the truth holds every link for 30 min, then the fixing alone must hold every new link.
6. Observed-data replays on existing streams were run here as preparation (sections 3 and 4): users only, no
   network run.

**Why hours.** On a sparse global network a link's float separates from the wet delays only over hours of changing
geometry, so a cold start to an integer network takes hours whatever the rule. The case that decides the method is
the steady state, which stage N2 now measures.

## 6. 2026-11-15: the builder's stages N1b and N2, re-graded here, and the next decisions

The builder's record is card NRF sections 10 to 13 on its branch (commits efcf888, 40e6f85, 65a7d42 and c18dc22, not
merged). The simulated beds are GPS only, two frequencies, 60 receivers, 2 h each.

**As reported.**
- Stage N1b (the bed without re-seats). Order free with the frozen gate: 19 links held, none wrong. Order free with
  the odds gate at one in a million: 170 of 186 links held by 2 h, 12 194 held link-epochs, none wrong; the stream's
  narrow lane over hour 2 reads 0.036 cycle against 0.081 on the plain run. Users on that stream held integers 8 % of
  the time against 25 %, one of them with 6 wrong pair-epochs.
- Stage N2 (re-seats and events). Held integers are carried through a re-host when the frame step is known and
  released when it is not. From a cold start: none wrong on the bed with re-seats (31 200 held link-epochs) and on
  seven event beds (27 000 to 31 000 each). A run with every link held at truth: none wrong. A truth warm start
  (truth for 30 min, then the rule alone): 68 wrong decisions.

**Re-graded here.**
- N1b with `nrf_regrade.py` against the file of true integers, without the trace's audit column: 1 036 and 12 194
  held link-epochs on the two arms, 0 wrong on both.
- N2: the reported counts reproduce from each run's trace (31 200 held link-epochs marked right on the bed with
  re-seats; 92 110 on the truth-held run; 60 739 right and 2 728 wrong on the warm start). The truth in each epoch's
  frame was not derived a third time here: the builder has two implementations that agree, and 3 329 decisions
  taken without truth on nine beds agree with it. The seven event beds share one fixture and the same two hours, so
  the independent decisions are fewer than that count.
- The stream against truth on the bed with re-seats, which the builder's record does not report (the builder's
  `irdu_stream_depth.py`, run here; narrow lane, offset p50 between satellites, cycles, by quarter hour):

| run | 0-15 | 15-30 | 30-45 | 45-60 | 60-75 | 75-90 | 90-105 | 105-120 min |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| fixing from a cold start, all depths | 0.247 | 0.166 | 0.106 | 0.060 | 0.047 | 0.023 | 0.017 | 0.022 |
| no fixing, all depths | 0.247 | 0.191 | 0.132 | 0.138 | 0.149 | 0.133 | 0.105 | 0.117 |
| every link at truth, all depths | 0.023 | 0.248 | 0.260 | 0.155 | 0.040 | 0.020 | 0.014 | 0.014 |
| fixing from a cold start, depth 5 | 0.289 | 0.238 | 0.174 | 0.112 | 0.120 | 0.030 | 0.023 | 0.085 |
| no fixing, depth 5 | 0.289 | 0.265 | 0.202 | 0.187 | 0.195 | 0.156 | 0.120 | 0.111 |

  From 75 min the fixing puts the stream at the level of the truth-held run on the bed with re-seats too. The wide
  lane moves the same way (0.035 to 0.045 against 0.064 to 0.077 from 75 to 105 min).
- Share of link-epochs held, by quarter hour: 0, 1, 3, 6, 14, 36, 70, 89 % on the bed without re-seats; 0, 4, 12,
  25, 43, 59, 71, 51 % on the bed with re-seats; 0, 4, 12, 25, 43, 24, 25, 34 % on the root handover bed.

**Findings made here.**
1. **Re-seats discard held integers that could be kept.** On the bed with re-seats one re-host at 104.8 min
   released 74 held links and one at 109.2 min released 47, each with the cause "step unknown". The held share fell
   from 71 % to 51 % and the depth-5 narrow lane from 0.023 to 0.085. Held links crossed each of those cuts, and any
   one of them, taken as the new tree link at its held integer, keeps the frame and every other held integer. Over
   the run 112 of 148 cuts had an unknown step and released 167 held links.
2. **The truth warm start is not valid as run.** The alarm file `network_raim.csv` has no row on the cold-start
   and plain runs. On the two truth-held runs it has 8 255 and 8 080 rows (7 755 and 7 598 arcs refloated) over 44
   receivers, from the epoch of the fixture's one true slip (15 min into the run) for half an hour; the warm
   start's cut-over (33 min) falls inside it. Each of the 383 refloat releases on the truth-held run is followed
   by a truth hold of the same link in the same epoch, and the slipped link is released and held again at every
   epoch: the test harness re-imposes a hold in the epoch in which the arc was just refloated, the solve of that
   epoch conflicts (a 75 m height offset in the congruence check at the slipped receiver), and the alarm response
   refloats again. The stream of the truth-held run sits at the null (0.25 cycle) from 15 to 60 min. The 68 wrong
   decisions were taken in a network torn by the harness; one of the links concerned already had a 4.9 cm residual
   while held at truth before the cut-over. The arm supports neither "an integer network does not stay integer" nor
   the reverse.
3. **A held link under a true slip has not been exercised from a cold start.** The fixture's slip comes at 15 min,
   before the first hold. By standing invariant 1 this blocks an observed-data run with the holds on.
4. **Users have not been read on a settled fixing stream.** The wrong pair integers of the one user on the N1b
   stream fall in its first half hour (30 to 60 min of the run), when 3 to 16 links were held: before the stream
   moved to the integer level, on a stream on which the plain arm's users leave the same pairs unresolved.

**Decisions taken in the author's absence** (2026-11-15, about 02:30), each open to review:

1. Stages N1b and N2 are taken as passed on safety from a cold start (no wrong held integer in about 238 000 held
   link-epochs on nine beds). The stream's gain is taken as shown on two beds. Nothing is shown yet for users.
2. The failure of the truth warm start is not taken as a verdict on the steady state. The builder repairs the
   harness (no truth hold in the epoch of a refloat, a slip or a cut) and re-runs the truth-held run and the warm
   start; a wrong decision that remains is traced before anything else.
3. A new build, default off: when the tree must change and a held link can be the new tree link, it is taken at
   its held integer, so the frame and the other held integers are kept ("held-first re-attachment").
4. New event beds: a true slip on a link the rule holds, from a cold start, in three forms (25 cycles on one band;
   one cycle on both bands, which the slip detectors may miss; one cycle on both bands on a tree link above held
   links). Bars: release at the slip's epoch or within the residual monitor's four epochs, no wrong held integer
   afterwards, no alarm storm.
5. Stage N3 (the 6 h bed from a cold start) runs with three arms (no fixing; fixing as built; fixing with
   held-first re-attachment) and with users started at 30 min, 2 h, 3 h and 4 h. Bars: no wrong held integer in
   the network over 6 h; no wrong pair integer at a user started at 2 h or later. Predictions stated before the
   runs: held share at least 90 % in hours 3 to 6, narrow lane at most 0.03 cycle at every depth, users started at
   3 h hold integers at least half of the time.
6. A log-only form of the rule (trace, no decision, no hold) is ordered for the preparation of the observed-data
   stage, which is run here on existing streams.
7. Not traced now: the one user's wrong pair integers on the unsettled stream (traced only if a user started at
   2 h or later repeats them).

## 7. 2026-11-15: the network's own links on observed data, read without fixing and without users

Sections 2 to 4 read the stream through users. This section reads the network itself: the links that the tree
does not pin (the chords), whose float ambiguities are integers in the chain's frame when the network is integer.
It is the quantity the fixing rule decides on, so it says what the rule will meet on observed data.

**Tool and data.** `chord_frac.py` on the chain run of 13 November (`_rap_nwa1nh_n144`, 6 h of DOY 144, 56
receivers, holds off, no fixing): from `network_amb_state.csv` (the carried float of every station, satellite and
signal) and `network_chain_edges.csv` (the tree per signal). Per link with one signal on each band, f = value /
wavelength, the wide lane w = f1 - f2 and N_1 = f1 + F2/(F1-F2) (w - round(w)); the fixing build's trace writes the
same numbers (checked on one row of the simulator). Arcs older than 30 min since their last re-level, from hour 2.
Cells: mean |fraction| in cycles, share within 0.10 (uniform null 0.250, 20 %).

| signals | ring 2: wide lane, N_1 | ring 3 | ring 4 | ring 5 |
| --- | --- | --- | --- | --- |
| Galileo L1X/L5X | 0.090 (62 %), 0.057 (85 %) | 0.103 (58 %), 0.083 (69 %) | 0.101 (58 %), 0.083 (68 %) | 0.101 (61 %), 0.115 (54 %) |
| GPS L1C/L2W | 0.134 (53 %), 0.163 (42 %) | 0.201 (24 %), 0.237 (19 %) | 0.234 (28 %), 0.244 (19 %) | 0.233 (17 %), 0.270 (14 %) |
| GPS L1W/L2W | 0.294 (12 %), 0.230 (27 %) | 0.247 (18 %), 0.257 (22 %) | 0.242 (21 %), 0.248 (19 %) | 0.215 (27 %), 0.244 (22 %) |

Galileo L1C/L5Q has 9 arcs with enough epochs; its receivers are few and its trees fall into components of
different roots (27 433 link-epochs between components).

**Galileo L1X/L5X converges to integers as the simulator does.** N_1 by hour of the run (rings pooled, from
hour 0): 0.179, 0.121, 0.094, 0.077, 0.070, 0.064, with 37, 56, 64, 75, 76 and 78 % within 0.10. By the arc's age:
0.096 (30 to 60 min), 0.069 (1 to 2 h), 0.055 (2 to 4 h), 0.027 (over 4 h, every link-epoch within 0.10). The
simulator's float chords under the same reader, 2 h from a cold start without re-seats: 0.192, 0.150 and 0.111 by
half hour (34, 44, 56 % within 0.10).

**Each arc's wide lane sits at a stable offset of its own.** Per arc with at least 60 mature epochs, the circular
mean of the wide-lane fraction and the spread about it:

| signals | arcs | wide-lane offset p50, p90, p99 | above 0.25 | above 0.40 | spread about it, p50 | N_1 offset p50, p90 | N_1 above 0.25 |
| --- | --- | --- | --- | --- | --- | --- | --- |
| Galileo L1X/L5X | 179 | 0.075, 0.169, 0.284 | 2 % | 0 % | 0.022 | 0.049, 0.160 | 4 % |
| GPS L1C/L2W | 172 | 0.119, 0.339, 0.481 | 27 % | 7 % | 0.046 | 0.195, 0.314 | 39 % |
| GPS L1W/L2W | 38 | 0.245, 0.426, 0.459 | 50 % | 24 % | 0.120 | 0.204, 0.467 | 39 % |

The offset does not shrink with the arc's age (Galileo L1X/L5X 0.102, 0.091, 0.097 and 0.112 by the age classes
above), and it differs by receiver: on GPS L1C/L2W the receivers' means run from 0.08 to 0.39 (SGOC 0.39, COCO
0.27, KIT3 0.25, LCK4 0.23), on Galileo L1X/L5X from below 0.05 to 0.19 (LMMF 0.19). The simulator has no such offset
(p50 0.040 with a spread of 0.029, noise).

**Reading.**
1. On observed data the Galileo L1X/L5X network is integer-like without any fixing: its N_1 floats move to integers
   with the arcs' age at the simulator's pace, and its wide lanes round right on every arc (no offset above 0.40),
   though at a stable offset of about 0.08 cycle that a formal sigma does not describe.
2. The GPS network is not, and the wide lane fails first: a quarter of the GPS L1C/L2W arcs carry a wide-lane
   offset above a quarter cycle, by receiver, so the wide-lane integer is wrong on some arcs and N_1 follows it to
   the null from ring 3 outward. This is the receiver-dependent code bias of card RSCB seen from the network side
   (the same quantity this branch estimates at the user), and it is the same failure section 2 found through
   users ("the wide lane fails first even one link from the root").
3. A stable offset is a bias, not noise: the odds test on the wide lane reads a formal sigma that shrinks with
   time while the offset stays. On observed data the wide-lane decision needs a bound on the fraction and a screen
   by receiver, not the formal odds alone.

**Consequence for the observed-data stage** (the manager's decision, open to review). The stage starts on Galileo
L1X/L5X alone. GPS enters when its wide lane is handled (a receiver-dependent code-bias state in the network with
its persistence, the network's counterpart of card RSCB), which is the multi-system step of the author's order.

**Not verified.** One day and one run; the formal sigmas of these links on observed data (the log-only rule ordered
from the builder writes them); whether the wide-lane offsets repeat on another day (persistence).

## 8. 2026-11-15: an audit of held integers that needs no truth: two runs must agree

Section 6 took the builder's truth model for the beds with re-seats on trust (two implementations by one author).
This section adds a check that uses no truth and no frame model, and that carries over to observed data.

**The check** (`nrf_dd_agree.py <run A> <run B>`). A run names an integer for an arc in two ways: the pin of a tree
link and the integer of a held link. Two runs with different trees have different frames, so their integers differ;
when both are right the difference on the arcs both runs name is a receiver term minus a satellite term, per
signal. Per epoch the common arcs form a graph between receivers and satellites; a spanning forest fixes those
terms and every other common arc is one check (a double difference of the integers that must close to zero). A
tree alone has no cycle, so each check involves at least one held link.

**Results on the simulator** (every fourth epoch; the reference run is N1b's arm on the bed without re-seats, whose
12 194 held link-epochs section 6 checked against the file of true integers):

| second run | checks per signal | failing |
| --- | --- | --- |
| fixing from a cold start, the bed with re-seats | 1 693 | 0 |
| the seven event beds | 722 to 1 623 each, 9 310 in all | 0 |
| every link held at truth (the builder's truth model in each epoch's frame) | 2 971 | 0 |
| plain rounding in closed loop (known wrong, the control) | 2 894 | 2 723 |
| the truth warm start (known wrong after its cut-over) | 1 652 | 530, the first at 76 min |

**Reading.** The cold-start holds of stage N2 agree with a run that was checked against the true integers, on
11 003 double differences per signal, and so does the builder's truth model (2 971). The check catches both runs
that are known to be wrong. The storm of section 6 is therefore not a matter of wrong truth integers: the integers
of the truth-held run are right, and what conflicts is the hold's entry into the epoch of the refloat.

**Use on observed data.** Two network runs of the same hours with different trees (two start times) give the same
audit without any truth: a failing check is a wrong held integer in one of the two.

## 9. 2026-11-15: the builder's stage N2b and the network side of stage N3, re-graded here

**As reported** (card NRF sections 14 to 16 on the builder's branch, commits bbeaa15 and 95ec239, exe
`gnss_processor_irdu_nrf6.exe`). Three builds and 30 runs, no wrong held integer:
- *Held-first re-attachment.* When the tree must change, the moved side re-attaches through a link that is already
  held, at its held integer. The step is then zero and the held links across the cut keep their integers.
- *The alarm loop of section 6, traced and repaired.* A link released under a hold kept the hold's own variance as
  its re-opening variance, so its stale level met the next truth hold halfway. The repair is a floor of 1 m^2 at
  every release and no truth hold in the epoch of a release.
- *A log-only rule* and *three beds with a true slip on a held link*.

**Re-grade** (tools `nrf_reads_mine.py`, new, and `nrf_dd_agree.py`, extended with constellation and signal keys, a
known slip of one bed and the log rule's rows; outputs in `grades_arnd/n2b/` and `grades_arnd/n3/`).

| what | result here |
| --- | --- |
| counts on the 16 bed runs, the two truth runs and the slip beds | decided, held link-epochs, releases by cause, re-attachments and kept links reproduce (the bed with re-seats: 423 decided, 38 241 held link-epochs, 31 re-attachments keeping 414 held links, every step zero); no row with a wrong audit, no hold event marked wrong, no alarm row on any run that holds |
| the audit of section 8, held first on the eight beds | 16 908 checks per signal, 0 failing |
| the audit, every link at truth and the truth warm start | 2 979 and 2 081 checks, 0 failing (the warm start: 530 of 1 652 failing before the repair) |
| a slip on a held link (25 cycles on L1; one cycle on both bands; S05000SIM-G26, held for 62 min) | released at the slip's epoch by the slip flag; held again 15.5 and 14.5 min later at the old integers plus the slip; the audit with the slip added to the reference: 0 failing of 1 690 (control) and 2 423 (held first); without it 34 and 16 fail, all through that link; with the sign reversed 16 fail |
| a slip on a tree link with 15 vertices below it (one cycle on both bands; S03000SIM-G25) | against the same bed without the slip: 18 326 checks (held first) and 13 972 (control), 0 failing with the slip added, 8 and 11 failing on the slipped arc without it; held first keeps the 42 held links across the cut, the control releases 38 |
| held share of the last 30 min, the bed with re-seats | 83.7 % held first, 61.0 % control |
| the stream on that bed, narrow-lane offset between satellites by 15 min (the builder's grader, run here) | held first 0.247 0.166 0.060 0.046 0.035 0.016 0.013 0.012, every pair within 0.10 from 75 min; every link at truth 0.024 0.018 0.016 0.016 0.016 0.014 0.014 0.014; the truth warm start 0.024 0.018 0.016 0.025 0.028 0.017 0.015 0.024, with 65 to 80 % of the pairs within 0.10 where re-seats release held links (that arm has no held first) |

The audit sees one cycle on one link (the slip beds without the slip added), so its zero counts carry weight.

**The six-hour bed (stage N3), network side.** 60 receivers, GPS L1C/L2W and Galileo L1C/L5Q, from a cold start;
the fixing runs on both constellations. The builder's user runs are not in yet.

| arm | held link-epochs, wrong | held links released at re-seats | held share by half hour (%) | held share from 3 h, rings 2 to 6 (%) | narrow-lane offset, all depths, by half hour (cycles) |
| --- | --- | --- | --- | --- | --- |
| plain chain | no fixing | - | - | - | 0.158 0.115 0.079 0.100 0.119 0.078 0.065 0.064 0.067 0.092 0.066 0.062 |
| fixing, as built | 138 951, 0 | 1 632 | 2 16 29 39 23 28 25 23 32 36 40 35 | 67 43 25 21 10 | 0.167 0.080 0.035 0.042 0.073 0.036 0.042 0.024 0.047 0.043 0.049 0.039 |
| fixing, held first | 332 377, 0 | 63 | 2 19 38 57 67 78 86 89 87 88 87 81 | 89 85 86 87 86 | 0.167 0.078 0.029 0.020 0.019 0.013 0.013 0.011 0.013 0.012 0.014 0.012 |

- No alarm row and 720 of 720 epochs on every arm. Held first made 293 re-attachments, every step zero, keeping
  11 089 held links.
- With held first the stream sits at 0.011 to 0.019 cycle from 2 h at every depth, with 96 to 100 % of the
  satellite pairs within 0.10; the plain chain stays at 0.06 to 0.12 with 43 to 69 % (GPS pairs: the stream grader
  reads GPS only). From 3 h the fixing holds 87.0 % of the Galileo link-epochs and 85.5 % of the GPS ones, evenly
  over the rings (83 to 89 % on rings 2 to 9).
- The audit between the two fixing arms: 17 113 checks per Galileo signal and 17 071 per GPS signal, 0 failing.
  The two arms share their first decisions, so this pair is a weaker check than two start times; a late start of
  the bed is ordered for that (below).
- Predictions of section 6: narrow lane at most 0.03 cycle at every depth, met; held share at least 90 % in hours
  3 to 6, missed (81 to 89 %: the remainder is the wait of new arcs for their first decision); users, pending.

**A receiver's step after the slip on the tree link** (the builder's open question). The receiver is the parent end
of the slipped tree link; its own parent link did not slip. Its phase-bias pair drifts by about -0.07 m (L1) and
-0.10 m (L2) over the four epochs after the slip on every arm, the plain chain included
(`network_rpb_states.csv`). On the held-first arm every held or pinned ambiguity of that receiver is unchanged to
0.000 cycle across the slip (`network_amb_state.csv`), both audits pass and no alarm fires. The move therefore sits
in parameters that are free per epoch (the receiver's phase bias with its clock and the per-arc ionosphere), a
direction the phase data do not see while the per-arc ionosphere is free. It is not an integer matter. Not traced
to a line of code.

**Decisions taken in the author's absence** (open to review).
1. Stage N2b passes, and held first is used in the stages that follow.
2. The builder's bar on releases at re-seats (0 held links released by a cut that a held link crossed) counts the
   links present at the cut. The 26 releases on the two split beds were links absent at their cut, which cannot
   carry the moved side; releasing them across an unknown step is the safe response.
3. The rule that releases the links inside a side that re-joins the tree stays as built (6 links on one bed).
4. Ordered from the builder: a slip on a held link that the detectors do not flag (stage N2c: the slip flags
   masked on one arc at one epoch, so that only the residual monitor and the alarms can release the link), which
   standing invariant 1 asks for before any holding run on observed data; a second start of the six-hour bed one
   hour later (stage N3b), to check the two-run audit itself where truth is known; and the log-only pass on
   observed data of section 10.
5. A holding run on observed data waits for the users of stage N3, for stage N2c and for section 10's pass.

**Not verified.** Users on the fixing stream (the builder's runs); a slip the detectors miss; the six-hour bed
against an independent truth (the code's truth audit and the two-arm audit agree, a truth file of this session
exists only for the bed without re-seats).

## 10. 2026-11-15: the observed-data stage, pre-registered: a log-only pass first (O1), then a holding run (O2)

**Question.** Does the fixing rule of the simulator decide right integers on observed data, on the one signal pair
whose network links are integer-like without fixing (Galileo L1X/L5X, section 7)?

**Step of card IRD section 11 this belongs to.** The network's own integers on the chain (the links the tree does
not pin); the bars reused are the simulator's: no wrong held integer, every release named, every epoch emitted.
On observed data "wrong" has no truth file; the audit of section 8 between two starts stands in for it.

**Bed.** The chain run of 13 November (`_rap_nwa1nh_n144`, its `arm.json`): DOY 144 of 2026, 6 h, 56 receivers,
GPS and Galileo, holds off. GPS stays float in this stage (section 7: its wide lane fails by receiver).

**Build ordered from the builder** (default off, byte-identical off, exe nrf7): a list of the pair families the
fixing considers (`network.ring_fix.pairs`); a bound on the wide lane's fraction in the decision
(`network.ring_fix.wl_frac_max`); the log rule without a truth root.

**O1, log-only (nothing held, the stream unchanged).**

| run | what |
| --- | --- |
| `nrf7off_n144` | the key off, the same-exe control |
| `nrf7_log_n144` | fixing on, rule log, free order, odds 1e6 on N_1 with fraction at most 0.20, wide-lane fraction at most 0.25, pairs Galileo L1X/L5X only |
| `nrf7_log_n144b` | the same, started one hour later (and with another root receiver for that pair if a key allows it) |

Why 0.25 on the wide lane: on Galileo E1/E5a a wide lane wrong by one cycle moves N_1 by 2.949 cycles, so the N_1
fraction cannot catch it (5.6 mm on the ionosphere-free phase), and the formal odds on the wide lane read a sigma
that shrinks while a stable per-arc offset stays (section 7). The per-arc offsets of this pair are close to a
Gaussian of sigma 0.11 cycle (p50 0.075, p90 0.169, p99 0.284 measured; 0.075, 0.183, 0.286 for that Gaussian);
under it an arc whose true offset exceeds 0.75 cycle, the only way to pass a bound of 0.25 at the wrong integer,
has a probability below 1e-10, and 2 % of the arcs are refused.

**Bars of O1 (frozen).**
- O1-a. The log run's products are byte-identical to the control's.
- O1-b. The audit between the two starts on the integers they would hold and their tree pins, per epoch: 0 failing
  checks. A failure stops the stage; the failing links are read, not explained away.
- O1-c. Every epoch emitted on the three runs.

**Reads of O1.** Links and link-epochs that would be held, by hour and by ring; minutes from an arc's birth to the
first decision; fraction and formal sigma of N_1 and the wide-lane fraction at the first decision; refusals by
reason, and the links only the wide-lane bound refuses; the number of checks of the audit and how many common arcs
are a held link in one run and a tree link in the other.

**Known limit of the audit.** An error both starts share on an arc with the same role in both (one biased wide
lane rounded the same way) passes it. The wide-lane bound is the guard there; a second day is the test of it.

**Predictions.** At least half of the Galileo L1X/L5X link-epochs of hours 3 to 6 would be held (section 7: 75 to
78 % of them within 0.10 of an integer there). The audit passes. The formal sigma of N_1 on observed data is not
known, so the pace is the uncertain part.

**O2, the holding run (not started; conditions below).** The same bed with the rule holding on Galileo L1X/L5X
(held first), two starts and the control. Bars: the two-start audit on held integers, 0 failing checks; every
epoch emitted; every release named; the alarm rows on Galileo arcs not above the control's by more than a tenth;
every release by the residual monitor read one by one. Reads: held share by hour and ring; the stream's offsets
between satellites by depth over the 73 observers of section 3 (`nw_depth_offsets.py`) against the control
(prediction: N_1 offsets at depth 2 and beyond fall from 0.17 to 0.21 to at most 0.10 in hours 4 to 6); users'
held shares on Galileo against the control and against card ARN's benchmark.

**Conditions for O2.** The users of stage N3 are not harmed by the fixing stream; a slip the detectors miss is
released on the simulator (stage N2c); O1's bars pass.

**Not in this stage.** GPS (the multi-system step: it needs the receiver-dependent code bias in the network);
Galileo L1C/L5Q (9 arcs, fragmented trees); a third frequency.

## 11. 2026-11-15: the GPS wide-lane offsets of section 7 are mostly one term per receiver plus one per satellite

**Question.** Section 7 found the GPS wide lane of the network's float links failing "by receiver". An offset that
is free arc by arc would need a code-bias state per arc in the network. Does it have a simpler structure?

**Tools and data.** `chord_frac.py --dump-arcs` (per arc with at least 60 mature epochs, the signed circular mean
of its wide-lane fraction) on the same run as section 7 (`_rap_nwa1nh_n144`, from hour 2), then
`chord_wl_by_block.py`: terms fitted on the circle by alternating circular means, the satellite's block from
`igs20.atx`, and the same fit on shuffled offsets (same receivers and satellites, values permuted, 20 draws) as
the measure of what fitting that many terms removes from unstructured values. Outputs in `grades_arnd/n3/`.

| what is removed from the per-arc offset | GPS L1C/L2W, 172 arcs: p50, p90, above 0.25 | Galileo L1X/L5X, 179 arcs |
| --- | --- | --- |
| nothing (what the rule meets today) | 0.119, 0.339, 27 % | 0.075, 0.169, 2 % |
| a term per receiver | 0.080, 0.257, 14 % | 0.052, 0.128, 1 % |
| a term per receiver and block (GPS III, older) | 0.069, 0.257, 12 % | not applicable |
| a term per receiver plus a term per satellite | 0.038, 0.140, 2 % (50 terms) | 0.040, 0.118, 0 % (57 terms) |
| the same fit on shuffled offsets | 0.092, 0.277 | 0.055, 0.139 |
| receiver, satellite and receiver-by-block terms | 0.036, 0.137, 0 % (68 terms) | not applicable |

Largest satellite terms on GPS: G05 +0.35, G21 +0.32, G06 +0.26, G25 -0.21. Largest receiver terms: SYOG -0.48,
COCO -0.37, SGOC -0.37, LMMF -0.34, KRGG +0.32, HOLB -0.31.

**Reading.**
1. On GPS L1C/L2W the offsets are additive to a large part: one term per receiver plus one per satellite takes the
   median from 0.119 to 0.038 and the share above a quarter cycle from 27 % to 2 %, far beyond what the fit takes
   from shuffled values (0.092). The satellite's block adds little once both terms are in.
2. Why additive. The wide-lane fraction of a link that the tree does not pin is a double difference against the
   tree: the link's own code-bias term, less the term of the tree link of its satellite, less that of the tree link
   of its receiver. Each vertex takes its frame from one tree link, so that link's arc-specific code bias moves
   every other link of the vertex by the same amount. The fitted terms are the tree links' own terms.
3. Galileo L1X/L5X has the same structure at a smaller size (0.040 against a null of 0.055).
4. GPS L1W/L2W cannot be read this way here: 38 arcs and 29 terms, the fit equals its null (0.039 against 0.040).

**Consequence for the multi-system step (a design candidate, not built).**
- A wide lane referred to the consensus of the network, a term per receiver and per satellite estimated from the
  mature float links, instead of to one tree link per vertex. With it the GPS wide lane reads like Galileo's.
- On GPS only, the phase can choose between the two integers next to the wide-lane float: adjacent wide-lane
  integers move the N_1 fraction by 0.47 (0.051 on Galileo E1/E5a, where this lever does not exist), so a
  converged N_1 separates them. The code then has to place the wide lane within one cycle, not half a cycle.
- A code-bias state per arc in the network (the counterpart of card RSCB) is then not the first build to try.

**Ordered for the test.** Log-only GPS L1C/L2W traces on the observed-data bed, two starts, beside the Galileo ones
of section 10 (nothing held). Both levers are tested offline on those traces before any build is asked for.

**Not verified.** The fit is in-sample, on per-arc means over about four hours; the terms move when a vertex
changes its tree link, which is not examined here; one day and one run; whether users need a matching term in the
stream (the stream's satellite code bias is the broadcast value).

### 11.1 2026-11-15: N_1 is additive too, and a satellite's links step together when its tree link changes

**N_1** (same arcs, tool `n1_additive.py`; outputs in `grades_arnd/n3/`).

| per-arc N_1 offset | GPS L1C/L2W, 172 arcs: p50, p90 | Galileo L1X/L5X, 179 arcs |
| --- | --- | --- |
| as read | 0.195, 0.314 | 0.049, 0.160 |
| less a term per receiver and a term per satellite | 0.029, 0.122 (82 % within 0.10) | 0.026, 0.086 (93 % within 0.10) |
| the same fit on shuffled offsets | 0.098, 0.274 | 0.043, 0.126 |

On GPS the float links are integer-like in N_1 once a term per receiver and a term per satellite are removed, that
is, in double differences. Section 7's reading "N_1 at the null from ring 3" described the link's own fraction,
which carries the two vertex terms.

**The terms belong to the tree link** (tool `chord_wl_steps.py`). At an epoch where a satellite takes a new tree
link, its other float links are compared before and after (10 min before; 10 min from 31 min after, when the
re-levelled links are mature again; links whose receiver kept its own tree link).

| | events | common step of the satellite's links, p50 and largest | spread of the links about it, p50 |
| --- | --- | --- | --- |
| GPS L1C/L2W, wide lane, at a change | 6 (9 links each at the median) | 0.160; +0.41 on G11 (9 links) | 0.016 |
| GPS L1C/L2W, wide lane, tree link kept | 72 | 0.035 | 0.019 |
| GPS L1C/L2W, N_1, at a change | 6 | 0.218; +0.48 on G32 (11 links), -0.47 on G29 (9 links) | 0.028 |
| GPS L1C/L2W, N_1, tree link kept | 72 | 0.010 | 0.014 |

Five of the six changes move every other link of the satellite by one common amount, to within 0.03 cycle. GPS
satellites changed their tree link 134 times from hour 1 (0.71 per satellite and hour in the tree), Galileo
L1X/L5X satellites 48 times (0.27); Galileo has one clean event (0.05).

**Reading (the manager's; the last point is not proven).**
1. A vertex takes its frame from one tree link, and every other link of the vertex is read against it. A new tree
   link brings its own errors into that frame, and all the other links step together.
2. Two of the six N_1 steps are 0.47 and 0.48 cycle with the wide lane almost unmoved (-0.01 and -0.16). One
   wide-lane cycle moves N_1 by 0.471 on GPS L1/L2 and by 0.051 on Galileo E1/E5a. A new tree link is pinned by
   rounding its two floats, band by band; if its wide-lane float is off by more than half a cycle at that moment
   (code, arc-specific code bias), the frame takes a wide-lane mismatch of one cycle, which costs 0.47 cycle of
   N_1 on GPS and 0.05 on Galileo. This would explain why the same receivers give an integer-like Galileo network
   and a GPS network at the null. The other steps (0.21 to 0.23) and the per-arc test (choosing the best of three
   wide-lane integers per arc leaves p50 0.128, what uniform offsets give) say that it is not the whole of it.

**Consequences.**
- GPS: a rule that decides a link on its own fraction cannot work, and where the two vertex terms add beyond half
  a cycle it would hold a wrong integer behind a small wrapped fraction. The quantity to decide on is the link
  against the consensus of its receiver and its satellite (double differences), which is integer-like (0.029).
  The frame's side of the remedy is the pinning: a new tree link taken from the held links (held first, built) or
  pinned with a wide lane the network agrees on.
- Galileo L1X/L5X: the vertex terms are small (largest 0.28, most below 0.10); the bound of 0.20 on the fraction
  refuses the links whose two terms add up, and a wrong integer would need a sum beyond 0.8, which is not there.
  Section 10's pass stands as pre-registered. One risk to read in it: a link whose wide lane is taken m cycles
  off the frame's lands on integers off by (3m, 4m) on E1 and E5a behind an N_1 fraction of 0.05 m. The two-start
  audit sees it when the two trees differ; `nrf_dd_agree.py` now prints the size of each failing closure.

**Not verified.** Six clean events; the terms are fitted in-sample on means over about four hours; the simulator
has not been read this way.

### 9.1 2026-11-15: the six-hour bed re-run on the builder's corrected build (this replaces section 9's N3 table)

**What changed** (card NRF section 16.1 on the builder's branch, commit f6a2837, exe nrf7). The cut algebra matched
a cut to a held link by the signal name alone, and GPS L1 and Galileo E1 are both `L1C`: a GPS cut also crossed
the receiver's Galileo links, and the other way round. Every known step of the first runs was zero, so no integer
was moved; the effect was spurious releases (the 63 of section 9's table). The builder found it while grading. The
users' start times of the first runs were also 30 min late; those user runs were discarded.

**The re-run, read here** (same tools; outputs `grades_arnd/n3/*nrf7*`).

| arm | held link-epochs, wrong | held links released at re-seats | held share by half hour (%) | held share from 3 h, rings 2 to 6 (%) | narrow-lane offset, all depths, by half hour (cycles) |
| --- | --- | --- | --- | --- | --- |
| fixing, as built | 233 400, 0 | 1 179 | 2 20 38 51 48 54 51 47 58 56 64 57 | 79 66 55 50 31 | 0.168 0.076 0.032 0.041 0.054 0.028 0.029 0.016 0.016 0.028 0.028 0.020 |
| fixing, held first | 342 217, 0 | 0 | 2 21 44 64 72 81 87 89 87 88 86 81 | 89 85 86 87 85 | 0.168 0.073 0.030 0.019 0.020 0.013 0.013 0.010 0.013 0.012 0.015 0.012 |

No alarm row and 720 of 720 epochs on both arms; 294 re-attachments through a held link, every step zero. The audit
between the two arms: 27 613 checks per Galileo signal and 29 579 per GPS signal, 0 failing. With held first every
satellite pair is within 0.10 cycle from 2 h (96 % in one half hour), and the fixing holds 87.1 % of the Galileo
link-epochs and 85.6 % of the GPS ones from 3 h. One release by the residual monitor on the arm without held first
(S32000SIM-G12, held at its true integers for 26 min): a release of a right integer, not a wrong hold.
