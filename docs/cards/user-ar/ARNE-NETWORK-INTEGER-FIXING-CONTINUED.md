# ARNE: network integer fixing, the observed-data side, continued

**STATUS**

- **id:** ARNE
- **topic:** user-ar
- **status:** open
- **summary:** Continuation of card ARND (that file neared the 60 KB bar). Card ARND holds the starting point on observed data, the re-grades of the builder's simulator stages up to the six-hour bed, the reading of the network's own float links on observed data and the pre-registration of the observed-data stage (its section 10). This card takes the results from 2026-11-15 on: the simulated users on the fixing stream, the log-only pass on observed data (stage O1), the holding run (stage O2) and the steps toward more systems.
- **dates:** 2026-11-15 (opened as the continuation)
- **knobs / posture:** none new here. The builder's keys `network.ring_fix.*` (default off, ledger rows UNVALIDATED) live on its branch.
- **code:** none. Analysis tools of card ARND (`nrf_reads_mine.py`, `nrf_dd_agree.py`, `chord_frac.py`, `n3_users_mine.py`).
- **run dirs:** `D:\GnssProcessorSupport\runs_nom\_rap_nrf*` (the builder's network runs) and `_rap_irdu_u_*` (its simulated users); tools and grade outputs under `D:\GnssProcessorSupport\presentation\agu26\fixing_control\`.
- **source:** card [ARND](ARND-NETWORK-INTEGER-FIXING-ON-OBSERVED-DATA.md), in particular its sections 9 to 11; card NRF on the builder's branch `irdu/user-integers`

**Outcome.** Open; continued in card [ARNF](ARNF-USERS-ON-THE-HELD-STREAM.md). On observed data (DOY 144)
Galileo L1X/L5X links are held with no disagreement between two reference receivers and two starts, at twice
the formal sigma and, once the bad-epoch response is on, at the formal sigma (78 to 88 % of the links held from
the first hour). The response (the builder's stage E1) is vetted here: a reset that keeps the pre-slip value
spreads a slip; a variance covering the measured step cures it. Users gain the wide lane; N_1 is taken up in
card ARNF.

> Results are appended TO THIS FILE, below the text.

---

## 1. Scope

Sections of card ARND are cited by their numbers there. The bars of the observed-data stage are those of card ARND
section 10 and are not restated here.

## 2. 2026-11-15: the simulated users on the six-hour bed (the builder's stage N3), with this session's read

**As reported** (card NRF part 3 section 18 on the builder's branch, commit 77d57ef; exe nrf7; five users, started
30 min, 2 h, 3 h and 4 h after the stream's first epoch, float and fixing, on three streams; 120 runs).

| stream | integers held by the users, by start (share of pair-epochs) | wrong pair integers |
| --- | --- | --- |
| the chain without network fixing | 15.9, 14.7, 17.1, 7.5 % | 0 |
| network fixing as built | 21.3, 31.5, 39.0, 35.8 % | 12 pair-epochs at one user of the 30 min start, 0 at the later starts |
| network fixing, held first | 50.2, 56.9, 48.1, 50.9 % | 0 |

**This session's read of the same runs** (`n3_users_mine.py`: 3D error from each user's `closed_loop_trace.csv`,
median over the five users of each user's bin median, by 15 min from the user's own start; mm).

| users started at | stream | float | fixing |
| --- | --- | --- | --- |
| 2 h | no network fixing | 390 147 138 108 92 77 71 61 | 390 147 138 109 80 62 49 48 |
| 2 h | held first | 351 201 74 41 55 50 38 28 | 351 201 74 33 55 42 21 16 |
| 3 h | no network fixing | 266 129 81 105 91 91 70 48 | 266 129 81 105 86 77 78 39 |
| 3 h | held first | 197 110 80 77 81 71 61 44 | 197 110 70 46 40 43 36 29 |
| 4 h | no network fixing | 278 79 104 87 137 82 82 90 | 278 79 104 87 134 91 82 91 |
| 4 h | held first | 371 146 65 95 62 60 68 65 | 371 146 53 63 59 41 30 27 |

**Reading.**
1. On the stream of the fixed network the simulated users hold integers about half the time and none is wrong;
   on the chain without network fixing they hold 8 to 17 %. This is the first stream of this project on which
   users of the network hold right integers at that rate, on the simulator.
2. In position the gain comes after one to two hours (16 to 29 mm against 39 to 91 mm at 105 to 120 min); the
   builder's longer series for the 2 h start reads 4 to 14 mm fixing against 20 to 30 mm float from the third
   hour. The first half hour is the same on both streams: the user's own fixing engages late (card IRD-8's bar K-c
   fails).
3. Card IRD-8's bar P-a fails on every chain arm, with or without network fixing: the held-out receiver's float
   position on the chain's stream trails the reference configuration's by a factor of two to six in most hours
   (the builder's numbers by hour, mm: reference configuration 99.5 32.6 39.8 16.0 18.0 15.2; held first 266.1
   115.7 35.9 60.9 118.5 31.9). That is a property of the chain's stream, not of the fixing.
4. One user of the 30 min start (S27000SIM) is thrown about 150 m off at its third epoch on both fixing streams
   and is fine on the plain one (seen here first, in the 3D read). The builder traced it by replays on edited
   streams to the user's own handover of its reference satellite at its third epoch: a user-side defect that the
   fixing stream's values happen to trigger (moving one record by 1 cm avoids it). Not changed.
5. Predictions of card ARND section 6: the users started at 3 h hold at least half the time, missed narrowly
   (48.1 %).

**Not verified here.** The users' held and wrong counts are the builder's grader's; this session read positions
only.

## 3. 2026-11-15: a log-only pilot on observed data: the rule as built fails the two-start audit; one ring out is clean

**What ran, and how it departs from card ARND section 10.** A pilot of stage O1 run by this session, to gain an
hour: the builder's exe nrf7 (sha256[:16] e9ddf9ea14925df0, a copy in `fixing_control\bin\x64-Release\`), arms
made with the builder's `rapid_arm.py` from `_rap_nwa1nh_n144/arm.json`. It differs from the pre-registered pass in
three ways: every signal pair is traced (the build has no pair filter yet), so the three decisions per epoch are
shared between the pairs; the wide-lane bound of 0.25 is applied offline, at the first row of each would-hold
spell; and the starts are four, not two. Nothing is held, and the stream is unchanged.

| run | start | epochs | what |
| --- | --- | --- | --- |
| `_rap_mgr_off_n144` | 00:00 | 720 | the key off |
| `_rap_mgr_log_n144` (A) | 00:00 | 720 | rule log, free order, odds 1e6 |
| `_rap_mgr_log_n144d` (D) | +0.5 h | 660 | the same |
| `_rap_mgr_log_n144b` (B) | +1 h | 600 | the same |
| `_rap_mgr_log_n144c` (C) | +2 h | 480 | the same |

Every run exits 0 with every epoch emitted. The log run's products are byte-identical to the key-off run's (bar
O1-a). The key-off run's products differ from those of 13 November (another build; a read, not a bar).

**What the rule would hold** (`log_reads.py`, run A).

| signal pair | candidate links, links ever accepted | accepted share of the candidate link-epochs by hour (%) | at the first acceptance: N_1 fraction p50, formal sigma p50, wide-lane fraction p50 and max |
| --- | --- | --- | --- |
| Galileo L1X/L5X | 387, 291 | 16 51 51 36 62 39 | 0.075, 0.139, 0.077 and 0.355 (8 links above 0.25) |
| Galileo L1C/L5Q | 380, 236 | 13 35 36 33 40 34 | 0.060, 0.134, 0.103 and 0.380 (16 above 0.25) |
| GPS L1C/L2W | 723, 181 | 5 16 17 14 8 6 | 0.096, 0.138, 0.113 and 0.400 (24 above 0.25) |
| GPS L1W/L2W | 293, 14 | 2 2 0 0 0 0 | 0.087, 0.132, 0.148 and 0.369 |

**The two-start audit, rule as built** (runs A and B; the audit of card ARND section 8 on the integers the rule
would hold and the tree pins, here through `o1_emulate.py`; `nrf_dd_agree.py --states would_hold --wl-max 0.25`
finds the same failing checks).

| signal pair | checks per signal | failing | sizes of the failing closures (band 1, band 2) | failing of checks by hour of the common span |
| --- | --- | --- | --- | --- |
| Galileo L1X/L5X | 13 743 | 462 | (-1,-1) 255, (+1,+1) 171, (-3,-4) 36 | 190 of 763, 85 of 3 020, 6 of 2 726, 132 of 4 753, 49 of 2 481 |
| Galileo L1C/L5Q | 4 694 | 754 | (+3,+4) 231, (-2,-3) 175, (+1,+1) 146, (-1,-1) 130 | 142 of 558, 347 of 1 103, 260 of 1 270, 0 of 1 553, 5 of 210 |
| GPS L1C/L2W | 1 162 | 0 | none | none |

**Bar O1-b (0 failing checks): FAILED for the rule as built.** The closures of size (3, 4) are the E1/E5a
signature of a wide lane one cycle off, which card ARND section 11.1 expected the audit to show.

**Stricter rules, tried offline on the same traces** (`o1_emulate.py`, `o1_pairs.py`: inside a would-hold spell the
stricter rule decides at the first row that meets it; summed over the six pairs of the four runs).

| rule | Galileo L1X/L5X: failing of checks | Galileo L1C/L5Q | GPS L1C/L2W |
| --- | --- | --- | --- |
| as built | 1 888 of 61 890 | 2 173 of 50 653 | 0 of 3 422 |
| odds with 2 times the formal sigma | 451 of 28 662 | 149 of 26 225 | 0 of 1 699 |
| 3 times | 110 of 8 103 | 0 of 8 509 | 0 of 1 099 |
| 4 times | 4 of 2 216 | not run | not run |
| 3 times and 60 min of persistence, or a network age of 120 or 180 min | 110 each | not run | not run |
| ring 2 only, as built | 39 of 21 646 (36 of the wide-lane kind) | 0 of 12 053 | 0 of 3 193 |
| **ring 2 only, 2 times the formal sigma** | **0 of 15 554** | **0 of 8 905** | **0 of 1 699** |
| ring 2 only, 3 times | 0 of 6 389 | 0 of 4 920 | 0 of 1 099 |
| rings 2 and 3, 2 times | 242 of 21 677 | 0 of 12 567 | 0 of 1 699 |

**Reading.**
1. On observed data the rule's formal sigma is too small: most failures come from a young network (26 to 31 % of
   the checks fail in the late start's first hour), and doubling the sigma removes three quarters of them.
2. No scale removes all. The survivors are links decided at an N_1 fraction below 0.01 and a formal sigma of 0.05
   to 0.09 in every run, one cycle apart between runs and stable for hours. No test on the link's own fraction and
   sigma can see them.
3. One ring out from the reference receiver, with the sigma doubled, the four starts agree on every check of all
   three pairs (26 158 checks). The failures live in ring 3 and beyond, where a link is read through more tree
   links. This is the order the author proposed: one link from the reference receiver first, then the next.

**Two survivors, traced with three or four runs voting** (`o1_failing_links.py`).
- KOKV00USA-E34 (ring 4, 4.2 to 5.4 h). The cycle with KOKV-E03, GCGO-E34 and GCGO-E03 has the same tree in every
  run and closes at 3 in runs A and C and at 4 in run B. It begins at the re-seat of 3.70 h, where the chain takes
  GCGO00USA-E03 as E03's tree link two epochs after that arc's birth (carried variance 0.16 m^2) and KOKV takes
  KOKV-E03; the link is re-born there and settles one cycle apart in the two runs.
- YKRO00CIV-E33 (ring 3). The cycle with YKRO-E04, STHL-E33 and STHL-E04 closes at 115 in runs A and B and at 114
  in run C, where it is already 114 at a network age of 1.25 h and stays so through a re-host of E33 at 3.80 h.
Why a run settles a cycle on another integer is not traced to the code; both cases are with the builder.

**A fact about the chain that goes with it** (`tree_link_age.py`). When a vertex changes its tree link, the new
link's carried float is younger than 2 epochs in 34 to 37 % of the changes of a satellite and younger than 10
epochs in about half (median age 6 epochs on Galileo L1X, 13 on GPS L1C). The simulator's plain chain reads the
same (36 to 39 %, median 6 to 9). The chain does not prefer the candidate with the most mature float.

**Decisions taken in the author's absence** (open to review).
1. No holding run on observed data with the rule as built.
2. The observed-data stage goes ring by ring. Stage O2 holds ring 2 only: odds with twice the formal sigma, the
   wide-lane bound 0.25, held first; two arms (Galileo L1X/L5X alone, as pre-registered; and the three pairs, GPS
   L1C/L2W at ring 2 included), each from two starts, with the key-off control. Bars as in card ARND section 10.
   Two keys are asked of the builder for it: a scale on the formal sigma and a ring cap whose refusal is tested
   last, so that the rows refused for the cap alone are the next ring's candidates.
3. A ring is opened only after the two-start audit has passed on the ring before it, held, and on the next ring's
   candidates read from the holding runs.
4. A change of the chain's choice of a new tree link (the most mature candidate first) is the likely root of the
   survivors. Not ordered; the builder's view is asked.

**Not verified.** The offline rule is not the built rule in closed loop (holding changes the floats of the links
that are not held). One day. The 26 158 clean checks at ring 2 are repeated looks at perhaps 100 to 150 links, so
they bound a link's error rate at a few per cent, not at the simulator's level. Users see ring 2 on the satellites
the reference receiver tracks only.

### 3.1 2026-11-15: CORRECTION to section 3: most of its failing checks were an artefact of comparing two start times

**What was wrong.** Section 3 ran the two-start audit on every arc both runs know. The builder's rehearsal of that
audit on the simulator (stage N3b: two starts of the six-hour bed, both right by truth) fails 125 386 of 512 298
checks as run, 0 with each run's wind-up branch removed, and 0 on the arcs born after the later start. Under the
accumulated wind-up convention each run gives an arc the wind-up branch of its own start of that arc. An arc that
is alive when the later run starts therefore carries another whole number of cycles in each run, the same on both
bands, which the audit reads as an N_1 one cycle off behind a right wide lane.

**What this withdraws from section 3.**
- Bar O1-b is not "FAILED for the rule as built" on the evidence of section 3: its closures of size (1, 1) are
  that artefact.
- Reading 2 (survivors that no scale removes) and the two traced survivors: both are cycles through arcs that the
  runs start at different epochs. They are not wrong integers.
- Reading 3 and decisions 2 to 4 (ring 2 only; ring by ring; the chain's choice of a young arc as the root cause):
  withdrawn. The measurement that a third of the tree-link changes land on a float 0 to 1 epochs old stands; its
  link to wrong integers does not.

**The audit redone on the arcs both runs start together** (`o1_pairs.py --born`: an arc's start from
`network_slip_raw.csv` with the producer's rule, a gap above 300 s; an arc enters when it starts after the later
run's first epoch in both runs; tree pins of other arcs are dropped too; six pairs of the four starts; the rule as
built with the wide-lane bound 0.25 applied offline).

| signal pair | rule as built: failing of checks | with twice the formal sigma |
| --- | --- | --- |
| Galileo L1X/L5X | 0 of 12 756 (0 of 13 084 without the wide-lane bound) | 0 of 4 457 |
| Galileo L1C/L5Q | 6 of 8 770, all of the wide-lane kind (closures +3/+4 and -3/-4, one pair of runs) | 0 of 3 819 |
| GPS L1C/L2W | 0 of 44 | no checks |
| GPS L1W/L2W | 0 of 132 | 0 of 89 |

**What stays real from section 3.** The closures of size (3, 4): a wind-up branch is the same on both bands and
cannot make them. On Galileo L1X/L5X there are 41 over the six pairs with the rule as built, all in a young
network's first hour and on arcs alive at the later start (the redone audit cannot see those), and none once the
odds use twice the formal sigma. On Galileo L1C/L5Q six remain among the arcs born together. The wide lane of a
young network is the weak point on E1/E5a, as card ARND section 11.1 expected.

**Bar O1-b, restated and read.** On the arcs both runs start together: Galileo L1X/L5X passes (0 of 12 756);
Galileo L1C/L5Q does not at the rule as built (6 wide-lane failures) and does with twice the formal sigma; GPS has
too few checks to judge.

**Decisions, replacing those of section 3** (taken in the author's absence, open to review).
1. Stage O2 runs on Galileo L1X/L5X over all rings, as pre-registered, in two arms: the rule as built, and the
   same with the odds on twice the formal sigma (against a young network's wide lane); each from two starts, with
   the key-off control; wide-lane bound 0.25; held first.
2. The two-start audit of held integers removes the wind-up branches (a trace of each arc's branch is asked of the
   builder) or, until then, uses the arcs both runs start together.
3. A sixth pilot run from the same start with another reference receiver is added (`_rap_mgr_log_n144r`,
   `network.phase_datum_eq34_receiver` GOPE00CZE): two runs of one start share every arc's branch, so their audit
   covers every arc, the young network's first hour included.

**How the error was caught.** By the builder's rehearsal on the simulator, where truth is known, which this
session had ordered for that purpose. The audit of card ARND section 8 was validated on runs of one start only;
its use across starts was not.

### 3.2 2026-11-15: the audit on every arc: two runs from one start with different reference receivers

**Why.** Two runs that start together give every arc the same wind-up branch, so their audit needs no restriction
and covers the first hour of two young networks. The frames still differ when the reference receiver differs.

**Run.** `_rap_mgr_log_n144r`: the pilot's log-only arm from start 0 with `network.phase_datum_eq34_receiver`
GOPE00CZE (exe nrf7; exit 0, 720 epochs). Its reference receivers: Galileo L1X/L5X and GPS L1C/L2W at GOPE00CZE
(BOGT00COL and BOR100POL in run A); Galileo L1C/L5Q at COCO00AUS and GPS L1W at LEIJ00DEU in both runs.

**Audit of run A against it, every common arc, every epoch** (`o1_pairs.py`, wide-lane bound 0.25 offline).

| signal pair | rule as built: failing of checks | with twice the formal sigma |
| --- | --- | --- |
| Galileo L1X/L5X | 0 of 25 771 (by hour 0 of 857, 4 859, 6 530, 4 025, 6 606, 2 894; 0 of 26 823 without the wide-lane bound) | 0 of 10 283 |
| GPS L1C/L2W | 45 of 12 428, one link | 0 of 8 577 |
| Galileo L1C/L5Q (same reference receiver in both runs) | 225 of 28 790, the links of one satellite | 225 of 14 809 |
| GPS L1W/L2W | 0 of 380 | 0 of 196 |

- **Galileo L1X/L5X passes on every arc**, the first hour included. With section 3.1's six pairs of starts (0 of
  12 756 on the arcs born together) and the builder's own pre-registered pair on its new build (its audit 0 of
  6 642; this session's filter on its runs 0 of 1 681 per band), bar O1-b is met for the pair the stage holds.
- **GPS L1C/L2W, one link** (SGOC00LKA-G13, 3.1 to 3.7 h, ring 2): the two runs take wide lanes one cycle apart at
  N_1 fractions of 0.08 to 0.14 and a formal sigma of 0.10 to 0.14. A real wrong decision in one of the runs; gone
  with twice the formal sigma. Apart from it the GPS decisions of the two runs agree on 12 383 checks, most of
  them one ring from the reference receiver.
- **Galileo L1C/L5Q, the links of E03** (DARW00AUS, SOLO00SLB, PARK00AUS, 2.3 to 2.65 h): one cycle on both bands.
  In the GOPE run the link DARW00AUS-E03 steps from (-160, -156) to (-161, -157) at 2.12 h with its formal sigma
  unchanged (a shifted carry, not a re-birth); run A does not step. Whether this is a wind-up branch restarted in
  one run only or a real one-cycle step is not settled without a trace of the branches. Not on the pair the stage
  holds.

**The builder's stages reported meanwhile** (card NRF part 3 section 20, commit cfbae53; read here, not re-graded
beyond the audit above).
- *A slip the detectors do not flag* (stage N2c): the residual monitor releases the held link on its fourth epoch;
  the link is held wrong for those four epochs and never after; no other hold is touched; it is held again at the
  right integers 16.5 min later, and the stream is back within 0.004 cycle by 18 min. Without any hold the float
  needs 38 min. Standing invariant 1 is met for a holding run on observed data.
- *The two-start audit rehearsed where truth is known* (stage N3b): 0 wrong held integers in the late start by
  truth; the audit fails as run (125 386 of 512 298) and passes with the wind-up branches removed or on the arcs
  born after the later start. This is what exposed the error of section 3.
- *The pre-registered log-only pass on its new build* (stage O1, pairs filter and wide-lane bound in the build):
  products byte-identical to the key-off run's; the key-off run equals the run of 13 November in every data line;
  288 of 387 links would be held in run A.

**Decision** (replacing decision 1 of section 3.1 in one point). Stage O2's audit pair is two runs from one start
with different reference receivers; a pair of different starts is audited on the arcs born together until the
builder's trace of the wind-up branches exists.

## 4. 2026-11-15: stage O2, integers held on observed data: the builder's runs re-graded

**What ran** (the builder; card NRF part 4 sections 21 to 23 on `irdu/user-integers`, commits `5747848`, `cc51628`,
`f938457`, `7059c3c`; exe `gnss_processor_irdu_nrf9.exe`, sha256[:16] `a18324f2ed30a1c5`). Bed: DOY 144 of 2026, 6 h,
56 receivers, the posture of `_rap_nwa1nh_n144`. New in the build: `network.ring_fix.sigma_scale` (the odds of N_1
and of the wide lane computed with that factor times the formal sigma) and the trace `network.trace_windup_arcs`
(each station's accumulated wind-up per arc at the arc's start and every 300 s).

| arm | what | runs |
| --- | --- | --- |
| `o2off` | the key off | `_n144` (start 0), `_n144b` (+1 h), `_n144r` (start 0, reference receiver GOPE00CZE) |
| `o2s2` | holding, Galileo L1X/L5X, odds on twice the formal sigma, wide-lane bound 0.25, held first | the same three |
| `o2s1` | the same on the formal sigma (the rule as built on the simulator) | the same three |
| `o2g` | as `o2s2` with GPS L1C/L2W held beside Galileo L1X/L5X | `_n144`, `_n144r` |

**Re-grade here.** Tools: `o2_audit.py` (the audit of card ARND section 8 on tree pins and the rows in state held
or decided, as they stand, every epoch, per band; with `--links` an attribution on the tree pins common to both
runs); `o2_audit_wu.py` (two starts on every arc: per arc segment the whole number of cycles between the two runs'
wind-up, from the trace, is removed from run B's integers); `nrf_reads_mine.py --sys`. Outputs
`D:\GnssProcessorSupport\presentation\agu26\fixing_control\grades_arnd\o2\`.

| pairing | failing of checks, this session (band 1, band 2) | the builder |
| --- | --- | --- |
| twice the sigma, one start, two reference receivers, every arc | 0 of 41 691, 0 of 41 663 | 0 of 82 972 |
| twice the sigma, two starts, every arc, wind-up differences removed | 0 of 39 421, 0 of 39 555 | 0 of 78 588 |
| twice the sigma, two starts, arcs born together | 0 of 10 600, 0 of 10 631 | 0 of 21 684 |
| twice the sigma with GPS, one start, two receivers: Galileo L1X/L5X | 0 of 43 117, 0 of 43 159 | 0 of 85 878 |
| the same: GPS L1C/L2W | 0 of 20 294, 0 of 20 294 | 0 of 40 360 |
| formal sigma, one start, two receivers | 2 881 of 75 952, 6 137 of 76 398 | 13 008 of 151 698 |
| formal sigma, two starts, arcs born together | 116 of 12 787, 149 of 13 099 | 1 015 of 26 048 |

- A failing count depends on the spanning tree the audit walks; a count of 0 does not. The verdicts agree in every
  row.
- The trace removes little: 3 of the 336 arc segments compared differ by a whole cycle between the two starts, and
  no anchor disagrees. With the sign of the removal reversed, 1 294 and 1 031 checks fail by two cycles, so the
  removal is what makes the pair agree.
- **Which run departs under the formal sigma** (the formal-sigma run against the twice-sigma run of the same start
  and receiver): start 0 with the default receiver 0 of 52 607; the +1 h run 3 556 of 31 360, 11 links
  (CPVG00CPV-E31, -E28, -E19, -E23; GAMB00PYF-E12 by three cycles on L1X, a wide-lane cycle; LMMF00MTQ-E10, -E28;
  POVE00BRA-E19; YKRO00CIV-E29; STHL00GBR-E29; WIND00NAM-E28); the GOPE00CZE run 2 713 of 49 913, 7 links
  (CPVG00CPV-E04, -E31, -E06, -E28, -E23, -E09; MRO100AUS-E27). The builder's reading stands: the departures are
  early decisions of a young network (22 to 76 min after its start, one of them a wrong wide lane behind an N_1
  fraction of 0.005), and one station's first wrong hold spreads to its other links.

**The other bars** (read here from the runs). Every epoch emitted (720, 600 for the +1 h runs). Alarm rows equal the
key-off run's: 15 and 15, 8 and 8, 17 and 17; with GPS held 13 and 16 against 15 and 17. Releases all named: at
twice the sigma gap 48, slip 6 (start 0); gap 44, slip 2, monitor 1 (+1 h); gap 51, slip 3 (GOPE00CZE). One release
by the residual monitor (LMMF00MTQ-E28 at TOW 21150 in the +1 h run, read by the builder: a station disturbance
that block-Baarda and the congruence test flag in the key-off run too). Re-attachments through a held link: 93
keeping 731 held links (start 0), every step 0.

**How much is held, twice the sigma** (held rows over the chord rows of the pair, %; hours of the run).

| run | h1 | h2 | h3 | h4 | h5 | h6 |
| --- | --- | --- | --- | --- | --- | --- |
| Galileo L1X/L5X, start 0 | 1 | 36 | 61 | 59 | 65 | 61 |
| Galileo L1X/L5X, GOPE00CZE | 4 | 29 | 49 | 57 | 58 | 63 |
| Galileo L1X/L5X, +1 h | 1 | 36 | 56 | 64 | 61 | |
| GPS L1C/L2W beside Galileo, start 0 | 1 | 9 | 12 | 17 | 25 | 21 |
| GPS L1C/L2W beside Galileo, GOPE00CZE | 1 | 14 | 16 | 19 | 25 | 16 |
| for comparison, formal sigma, Galileo, start 0 | 24 | 82 | 85 | 82 | 87 | 85 |

By ring, start 0: Galileo 64, 50, 49, 36, 33, 26 % at rings 2 to 7; GPS 31, 25, 10, 4, 3, 2 %. Holding GPS beside
Galileo leaves the Galileo shares within one point.

**Why GPS stops at ring 4** (`o2g_n144` from hour 2; share of the chord rows by ring and by the reason refused).

| | held | event window | odds | N_1 fraction |
| --- | --- | --- | --- | --- |
| GPS ring 2 | 52 | 11 | 13 | 21 |
| GPS ring 3 | 32 | 28 | 20 | 17 |
| GPS ring 4 | 15 | 39 | 27 | 18 |
| GPS ring 5 | 3 | 50 | 21 | 25 |
| GPS ring 6 and deeper | 3 | 61 | 16 | 20 |
| Galileo ring 2 | 83 | 5 | 10 | 1 |
| Galileo ring 5 | 42 | 27 | 25 | 3 |
| Galileo ring 6 and deeper | 45 | 31 | 14 | 8 |

A deep GPS link is refused first because an event fell inside its 600 s window, then for its N_1 fraction. The
second agrees with card ARND sections 11 and 11.1 (a frame that hangs on one arc's own code error, which on GPS is
a bias by receiver and satellite); the first says the GPS frame changes too often below ring 3 for a link to
qualify.

**Readings.**
1. Stage O2's bars (card ARND section 10) pass with the odds on twice the formal sigma, in three pairings, one of
   them with GPS L1C/L2W held: no failing check in 290 000. With the formal sigma they fail. The rule that passed
   every simulator bed is therefore not safe on observed data as built; its formal sigma is too small while a
   network is young.
2. Twice the sigma costs time and coverage: the first hold comes 48 min after the start instead of 17, and the held
   share of hours 3 to 6 is about 60 % instead of 85 %.
3. GPS L1C/L2W can be held beside Galileo without a failing check, but only near the reference receiver.
4. One day, one bed. The audit cannot see an error both runs share.

**Decisions** (taken in the author's absence, open to review).
1. `sigma_scale` 2 is the setting from here on; the formal-sigma rule is not run again on observed data.
2. Ordered from the builder, in this order: (G0) reads only, why GPS stops at ring 4: which events reset the window
   of a deep GPS link, and whether the refused fractions are one term per receiver plus one per satellite; a remedy
   proposed, not built; (O3) a second day at the setting that passed, Galileo L1X/L5X and GPS L1C/L2W, one start with
   two reference receivers and the key-off controls; (O4) every pair family held on DOY 144 (Galileo L1C/L5Q and GPS
   L1W/L2W added), with roots changed and a +1 h start: half the Galileo receivers track L1C/L5Q, and the one-cycle
   case of section 3.2 is tested there under holding; an assessment, not a build, of what a third frequency needs.
3. The user side of O2 is run here (section 4.1).

### 3.3 2026-11-15: the one-cycle case of section 3.2 traced: a receiver's wet delay kicked by another receiver's bad epoch

**Question.** Section 3.2 left one case open on Galileo L1C/L5Q: two log-only runs from one start (`_rap_mgr_log_n144`
= A, `_rap_mgr_log_n144r` = R, reference receiver GOPE00CZE for the X pair and GPS L1C/L2W) disagree by one cycle on
both bands on "the links of E03" from 2.3 h. The builder's trace of the wind-up accumulators rules a wind-up branch
out (card NRF part 4 section 21.1).

**What it is not.** The tree of the C/Q family is identical in both runs from TOW 6900 to 8400 (COCO00AUS, E02, then
MAC100AUS, DARW00AUS, PARK00AUS and SOLO00SLB at depth 2, E03 under MAC100AUS; no change of a tree link, no pin
move). No slip is declared on the links concerned.

**Which links.** The attribution on the tree pins common to both runs (`o2_audit.py --log --links`, the rows in
state would_hold) names two links, both of one receiver: DARW00AUS-E03 (45 epochs, TOW 8460 to 9780) and
DARW00AUS-E08 (3 epochs). SOLO00SLB and PARK00AUS in section 3.2 were the audit walk's choice of check arcs.

**What happened** (TOW 7800, 02:10:00).
- MRO100AUS has one grossly wrong epoch. The congruence test reads a coordinate offset there with t 154 721 and an
  apparent height offset of 34.5 m in the GOPE00CZE runs (t 1 257 and 3.2 m in the default runs, from the same
  observations); the gross-LOS screen declares G11 at MRO100AUS two epochs later (15.4 m on C1C and C2W); RAIM spikes
  on eight of its satellites in run R, on one in run A. The test reports and does not act; the epoch is solved with
  those rows.
- The carried wet delay of several receivers steps in that one epoch, with the formal sigma (7 mm) unchanged
  (`network_zwd.csv`, steps of 2 cm or more; the key-off runs `_rap_o2off_n144` and `_rap_o2off_n144r` read the same
  as A and R):

| receiver | step, default reference receivers | step, GOPE00CZE runs |
| --- | --- | --- |
| MRO100AUS | +10.8 cm | +103.1 cm |
| DARW00AUS | +2.8 cm | +33.5 cm |
| MBAR00UGA | below 2 cm | -9.0 cm |
| COCO00AUS | below 2 cm | -5.9 cm |
| CUSV00THA | below 2 cm | +4.6 cm |
| SEYG00SYC | below 2 cm | +4.2 cm |
| SOLO00SLB | below 2 cm | +2.3 cm |

- The steps last: DARW00AUS in the GOPE00CZE run reads 0.274 m before, 0.609 at the step, 0.557, 0.500, 0.531 and
  0.377 m at 10, 20, 50 and 110 min after it, and is back (0.278) after 170 min; MRO100AUS 0.056, 1.087, 0.704,
  0.568, 0.327, 0.123, 0.070 m at the same times.
- The float ambiguities of the kicked receiver move with it at no cost. In run R every Galileo float of DARW00AUS
  moves at TOW 7800 by an elevation-like amount, nearly equal in metres on the two bands: E03 by -0.745 and -0.623
  cycle (-142 and -159 mm), E08 by -0.994 and -0.913 (-189 and -233 mm), E32 by -0.434 and -0.369 (-82 and -94 mm);
  their formal sigmas do not change. In run A the same links move by 0.01 to 0.11 cycle.
- The log rule accepts the moved floats ten epochs later (DARW00AUS-E03: refused for persistence at 7800, would hold
  at 8070 at -161 and -157 with an N_1 fraction of 0.13), because no event of the link itself was declared and the
  600 s window never opened. The odds on twice the formal sigma do not refuse it (section 3.2: 225 of 14 809).

**Do held integers protect a receiver?** Not by themselves. In the holding runs of section 4 the same steps occur
with nearly the same sizes (MRO100AUS +102.7 against +103.1 cm, DARW00AUS +32.2 against +33.5 cm in the GOPE00CZE
runs with and without holds; MBAR00UGA -5.4 against -9.0 cm with two held links). A receiver with one or two held
links still takes the step in its clock, its wet delay and its other floats; the held links' residuals stay below
8 mm and no hold is released. No wrong integer followed on the pair held there (the audit of section 4 passes over
that hour), but the receiver whose floats were decided wrongly in log mode (DARW00AUS) tracks the family that was
not held.

**Readings.**
1. The open case of section 3.2 is not a property of Galileo L1C/L5Q. It is standing invariant 1 on the carried
   wet delay: a stiff time-linked state without a jump response, kicked by one epoch of gross rows that the tests
   see and do not stop, and at another receiver than the faulty one.
2. For the fixing rule: its event window knows only the link's own events. A step of a station state at either end
   of a link is an event of that link. A window alone is no cure while the wet delay stays wrong for three hours.
3. For the network without any fixing: one receiver's bad epoch degrades up to seven other receivers' wet delays for
   hours; how far it reaches depends on the reference receiver by a factor of ten. This is a defect of its own, on
   the float stream too.

**Decisions** (taken in the author's absence, open to review). The facts were sent to the builder for stage O4 (the
same bed and epoch, with the C/Q family held) and for its answer on the window. Proposed, not ordered: an
injected-event test on the simulator (one epoch of gross code errors at one receiver), the reach of the kick
measured, and the response designed there before any change on observed data.

### 3.4 2026-11-15: correction to section 3.3 (the trigger is one flagged slip) and the author's order: the response

**Correction.** Section 3.3 says that MRO100AUS "has one grossly wrong epoch". The raw combinations of
`network_slip_raw.csv` say less: at TOW 7800 the only fault in the observations, network-wide, is one flagged loss
of lock, MRO100AUS-G11 at 15.9 degrees elevation. Its L1 phase jumps by about 288 cycles (L1-C1 +54.25 m, GF +54.83
m, MW +247.6 m), the loss-of-lock indicator is set on L1, L2 is untouched, and the jump persists. Every other
satellite at MRO100AUS, and every satellite at DARW00AUS, is quiet in MW, GF and L1-C1. The 34.5 m of apparent
height at MRO100AUS and the metre-level code residuals at other receivers are the network's solution after its
own responses, not observations.

**What the network does at that epoch** (`network_events.csv`, the key-off runs with the default reference
receivers and with GOPE00CZE).
- The slip detectors catch the slip: `T0_mw_detect lli_slip` (refloat), `T1_gf_jump` (coincident), `T1_if_slip`
  (refloat). They are the only slip rows of the network at that epoch.
- Responses: the wet delay of MRO100AUS is erased (`ZWD slip_latch`, and `ZWD raim_attribution` on G05); the RAIM
  prior channel refloats 3 arcs of MRO100AUS (default) or 6 and more (GOPE00CZE); the satellite clock anchors let
  go on 13 satellites (default) or 29 (GOPE00CZE), each with a declared discontinuity.
- Tests that see the result and only report: the congruence test (22 and 7 stations flagged), block-Baarda (123
  and 73 rows), the MDB exceedances, RAIM (spikes at one station, below its three-station rule).
- Five or more clock anchors let go at one epoch four times in these six hours (TOW 7800, 10080, 15480, 18450), so
  the family is not a single case.

**The author's order** (2026-11-15, in chat): "Have the builder test a bad-epoch response on the simulator." and
"Regardless of the cause of the bad epoch, our network can detect it, but doesn't know how to respond/adapt. This
is what Opus should work on to save our weekly tokens!" The cause is not traced further here.

**Assigned to the builder as stage E1, ahead of the stages of section 4.** One injected event on the simulator (the
slip above at one receiver, the same bed without it as the control, two reference receivers); the reach measured
against truth and the control on every receiver's wet delay, clock and floats and on the emitted satellite clocks
and phase biases; the step that turns one link's fault into a network-wide step named; a response designed and
built default off. Bars sent with the order: no other receiver's wet delay more than 5 mm from the control, the
faulty receiver's within 2 cm of truth, emitted clocks and phase biases of the other satellites within the noise
of a control pair, every epoch emitted, no wrong held integer and no decision at the faulty receiver inside the
event with fixing on, keys at default byte-identical; then the observed epoch with both reference receivers, the
steps of section 3.3 below 1 cm at every receiver but MRO100AUS.

## 5. 2026-11-15: the builder's methods vetted (stage O2's reading and its proposal for GPS depth)

**Why.** The author's instruction (2026-11-15, in chat): "You can recheck the headline numbers, but more
importantly, think about the Opus solution and investigate yourself to confirm the work is reasonable." This
section judges the methods, with reads of the runs made here.

### 5.1 Why the rule on the formal sigma failed: mostly a wet delay reset after a slip, not the sigma

**Read.** The decisions of the three formal-sigma runs (`RINGFIX hold` events: time, ring, log-odds of N_1 and of the
wide lane), the links that depart (section 4), and `network_zwd.csv` at their receivers.

- **The margin of twice the sigma is thin.** Twice the sigma needs log-odds of 55.3 on the formal sigma. Among the
  first wrong decisions the largest is 44.3 (CPVG00CPV-E28 in the +1 h run); a scale of 1.75 would have taken it.
  Of the decisions that agree, 72 to 87 % also sit below 55.3 when taken. The setting passes by delaying decisions,
  not by telling right from wrong.
- **CPVG00CPV, TOW 3990.** A flagged slip on G20 (GF step -50.8 m, 20.3 degrees elevation). In the same epoch the
  receiver's carried wet delay is erased (`ZWD slip_latch`, `ZWD raim_attribution`), the RAIM prior channel refloats
  the arc, and the chain re-hosts G29 onto CPVG00CPV with a cut release of 26 arcs. The wet delay (0.18 to 0.21 m
  when settled):

| run | TOW 3960 | 3990 | 4200 | 4560 | 5400 | 7200 | 9000 |
| --- | --- | --- | --- | --- | --- | --- | --- |
| key off, default reference receivers | 0.215 | 0.128 | 0.089 | 0.099 | 0.120 | 0.153 | 0.169 |
| key off, GOPE00CZE | 0.197 | 0.058 | -0.014 | 0.008 | 0.068 | 0.144 | 0.175 |
| key off, started at TOW 3600 | 0.403 | -2.646 | -2.531 | -1.440 | -0.469 | 0.088 | 0.171 |

- **The departing decisions fall inside that recovery.** GOPE00CZE run: CPVG00CPV-E04 at TOW 4560 and -E31 at 4860.
  The +1 h run: CPVG00CPV-E28, -E31 and -E19 at TOW 7110 to 7470, the wet delay still near 0.08 m.
  MRO100AUS-E27 (GOPE00CZE run, TOW 12 810) falls in the recovery of the kick of section 3.3 (0.157 m against 0.05
  to 0.07).
- **Wrong holds keep the wet delay wrong.** CPVG00CPV in the GOPE00CZE formal-sigma run reads 0.114, 0.115, 0.149,
  0.169 and 0.145 m at TOW 5400, 7200, 9000, 10 800 and 14 400, where the other runs read 0.18; in the +1 h run
  0.077, 0.123, 0.153 and 0.148.
- **Not this mechanism:** GAMB00PYF-E12 (a wide-lane cycle) and LMMF00MTQ-E10, decided 22 to 23 min after the +1 h
  run's start; their wet delays are within 1 to 3 cm of the other runs.

**The reset is routine.** In the key-off run the wet-delay prior is erased 934 times in six hours by a slip at the
station and 39 times by a RAIM attribution. The step that follows is 0.2 cm at the median and 0.8 cm at the 90th
percentile; 52 steps reach 2 cm and 23 reach 5 cm (largest +28.9 cm). It is harmless while the station keeps its
ambiguity carries, and large when the same epoch also refloats several of its arcs or a cut release erases them.
The same slip of TOW 7800 gives no kick in the runs started at TOW 3600.

**Readings.**
1. Section 4's reading 1 ("its formal sigma is too small while a network is young") is incomplete. Thirteen of the
   eighteen departing links sit at two receivers inside a wet-delay recovery; two are early decisions of a young
   network; three follow from those.
2. Twice the sigma is a guard that works by waiting. It costs a quarter of the held share and three times the time
   to the first hold. The cure is the response to the reset (stage E1 of section 3.4).
3. Wrong integers and a wrong wet delay hold each other in place, so a hold decided inside a recovery does not heal.

**Decision** (open to review). After stage E1's response exists, the formal-sigma pairs of section 4 are run again
with it on. Prediction: the departures at CPVG00CPV and MRO100AUS go; the two early ones may stay. That decides
whether the faster setting returns.

### 5.2 The proposal for GPS depth (card NRF part 4 section 24), judged

- **The reads** agree with card ARND sections 11 and 11.1 on another run: most of a GPS link's fraction is one term
  per receiver plus one per satellite (N_1 p50 0.214 to 0.046, wide lane 0.186 to 0.034), and a deep link is refused
  first for its window. A quarter of the GPS window episodes are unattributed; the wet-delay erases and the RAIM
  refloats at the link's station are to be checked as their cause.
- **(a) Rebase instead of release when the tree re-attaches through a held link: sound.** The held link already
  carries the frame; erasing the moved side's carries throws convergence away for no event of their own. Asked of
  the builder: why carries are released at any cut whose new tree arc has a float, since that float gives the step
  as a real number and the carries could be rebased by it with their variances (an S-transformation).
- **(b) A decision against the consensus of a link's receiver and satellite: the diagnosis is right, the form is
  the author's choice.** The frame hangs on one arc per receiver and satellite, so that arc's own error becomes
  the offset of every link below it; on GPS that error is a bias (code, by receiver and satellite). The builder
  proposes a new phase datum set by the consensus of all arcs. An alternative that stays inside the chain: a
  consensus re-seat as a discrete event (a receiver or satellite re-pinned onto the arc nearest the consensus of
  its links, with the known real-valued step and the carries rebased), after which held-first moves the tree onto
  validated links. Both are to be compared on a simulator bed with code biases by receiver and satellite, judged
  by the stream users see. Neither is built before the author chooses.
- **(c) Fewer tree-link changes, and the simulator bed with code biases by receiver and satellite:** agreed.

**Order of work sent to the builder.** E1; the formal-sigma pairs with the response on; (a); the simulator bed for
(b).

### 4.1 2026-11-15: the user side of stage O2 (the reads pre-registered in card ARND section 10)

**Runs** (user program `gnss_processor_rscb3.exe`, DOY 144, six hours, static). Stream with network fixing:
`_rap_o2g_n144` (Galileo L1X/L5X and GPS L1C/L2W held, odds on twice the formal sigma). Stream without:
`_rap_nwa1nh_n144` (equal to `_rap_o2off_n144` in every data line). Observers, no fix accepted: the 56 network
receivers and the 17 test receivers on each stream (`_rap_nw_obsno2g_144_*`, `_rap_nw_obso2g_144_*` against
`_rap_nw_obsna1nh_144_*`, `_rap_nw_obsa1nh_144_*`). Fixing users (the `fxb` arm): the 17 test receivers on each
stream (`_rap_nw_fxbo2g_144_*` against `_rap_nw_fxba1nh_144_*`). All 180 runs exit 0. The replay on the Galileo-only
stream `_rap_o2s2_n144` was stopped after 15 observers (machine load); the Galileo holds of the two streams agree
within one point (section 4). Outputs `grades_arnd\o2\depth_offsets_*.txt`, `users_fxb_*_grade.txt`.

**The stream's offsets between satellites, by depth in the tree, from hour 3** (`nw_depth_offsets.py`, the users
averaged out; mean |fraction| and the share within 0.10 cycle; two unrelated offsets give 0.250 and 20 %).

| group | depth | wide lane, without | wide lane, with | N_1, without | N_1, with |
| --- | --- | --- | --- | --- | --- |
| Galileo L1X/L5X (39 receivers) | 1 | 0.051, 89 % | 0.022, 100 % | 0.143, 46 % | 0.105, 71 % |
| | 3 | 0.101, 58 % | 0.046, 91 % | 0.234, 25 % | 0.197, 34 % |
| | 5 | 0.141, 46 % | 0.045, 91 % | 0.267, 14 % | 0.224, 22 % |
| | 7 and more | 0.106, 51 % | 0.064, 78 % | 0.243, 14 % | 0.198, 32 % |
| GPS L1C/L2W (54 receivers) | 1 | 0.161, 36 % | 0.035, 98 % | 0.201, 25 % | 0.150, 33 % |
| | 3 | 0.239, 23 % | 0.132, 57 % | 0.245, 21 % | 0.223, 26 % |
| | 5 | 0.247, 22 % | 0.214, 29 % | 0.250, 20 % | 0.261, 18 % |

Galileo L1C/L5Q (not held) is unchanged (N_1 0.24 to 0.25 beyond depth 1).

**Fixing users** (`nw_grade.py`; share of pair-epochs held; median over the receivers of a group).

| group of test receivers | Galileo held, without | with | GPS held, without | with |
| --- | --- | --- | --- | --- |
| tracking Galileo L1X/L5X, the held pair (11) | 9.8 % | 18.3 % | 4.1 % | 3.0 % |
| tracking Galileo L1C/L5Q, not held (6) | 8.3 % | 8.1 % | 3.8 % | 2.5 % |

Ten of the eleven receivers of the held pair gain (CEDU00AUS 10.0 to 36.7 %, CIBG00IDN 2.8 to 29.4 %, CHTI00NZL 5.9
to 23.8 %, UTQI00USA 10.8 to 24.6 %); QUIN00USA loses (14.2 to 5.9 %). Their positions do not improve (3D median of
hours 4 to 6: 139 to 150 mm).

**Float users** (the 17 observers; 3D median by hour, median over receivers): 474, 225, 149, 110, 112, 92 mm
without network fixing; 479, 234, 132, 84, 79, 62 mm with it; better at 10, 11 and 13 of 17 receivers in hours 4,
5 and 6.

**Readings.**
1. The network's held integers reach users. The wide lane of the stream is now integer for users of the held
   Galileo pair at every depth (0.045, 91 % within 0.10), and for GPS one step from the reference receiver. The
   receivers of the pair that is not held are an untouched control.
2. **The prediction of card ARND section 10 fails for N_1.** The N_1 offsets between satellites at depth 2 and
   beyond were to fall to at most 0.10 in hours 4 to 6; they stay at 0.20 to 0.22 (0.23 to 0.27 without fixing).
   On the simulator the same read fell to 0.01. Holding 60 % of the links does not make the narrow-lane part of
   this stream integer for users.
3. Users therefore gain little: twice the held share on the held pair, still below 20 %, against 85 to 95 % on the
   offset benchmark of card ARN; float positions a quarter to a third better late in the run.
4. The absolute level is poor with and without fixing: a static float user of this stream is at 6 to 11 cm after
   four to six hours. The stream of this posture is far from the reference configuration's for a float user.

**What is not known.** Why N_1 between satellites stays near random with the links held. First suspect, untested:
the receivers' wet delays in the network (section 5.1: reset about 930 times in six hours, wrong by centimetres to
decimetres for hours after some resets), which enter each satellite's clock and phase bias through the mapping.
The test: this read again on a stream produced with stage E1's response.

**Decision** (open to review). No user-side claim is made for observed data beyond the wide lane. The N_1 read of
this section is the bar the next stream has to move.

## 6. 2026-11-15: stage E1 vetted (the response to a bad epoch, card BER on the builder's branch)

**What was vetted.** The builder's commit e0258b1 on `refs/heads/irdu/user-integers` (card BER there): the step that
spreads a detected slip is the float vehicle's drop, which keeps the pre-slip value and restores the arc's first
variance; the response `network.amb_float_drop_jump_k` (default 0, run at 1) gives a slip-latched drop a variance of
at least (k times the band's phase-minus-code step across the epoch) squared. Reported: every bar of section 3.4
passes on the simulator and on the observed epoch. Method of this section, on the author's instruction (section 5):
the method judged by reads made here; the headline numbers second.

### 6.1 Reads made here

- **Numbers.** The simulator wet delays recomputed from `network_zwd.csv` without the builder's script: faulty
  receiver 3.56 / 15.98 cm to 0.43 / 0.64 cm (two reference receivers), other receivers 1.89 cm (3 above 5 mm) /
  0.995 cm (1) to 0.002 / 0.010 cm. The builder's `e1_reach.py` re-run: other satellites' phase biases 4.195 m to
  0.0009 m; the same flagged slip at one cycle 0.0352 m.
- **Code.** The comment inside `ambFloatDrop` states the design assumption: "a prior at birth variance is dominated
  by the epoch's data". It fails twice. The kept value keeps a real weight (about 30 % in the simulator event), and
  the restored variance is the arc's own first variance, as small as (0.057 m)^2 on observed data.
- **The observed arc** (MRO100AUS-G11, TOW 7800, with the response): only L1C is reset (the band scope); its float
  is back in the same epoch at sigma 0.046 m (0.045 m before the slip), moved by 54.80 m; L2W is untouched. With
  the other band intact the response costs nothing.
- **CPVG00CPV** (the receiver under most formal-sigma failures of section 5.1). Wet delay (m):

| run | TOW 3960 | 3990 | 4200 | 4560 | 5400 | 7200 | 9000 |
| --- | --- | --- | --- | --- | --- | --- | --- |
| no response, default reference receivers | 0.215 | 0.128 | 0.089 | 0.099 | 0.120 | 0.153 | 0.169 |
| response | 0.213 | 0.212 | 0.214 | 0.208 | 0.196 | 0.186 | 0.181 |
| no response, GOPE00CZE | 0.197 | 0.058 | -0.014 | 0.008 | 0.068 | 0.144 | 0.175 |
| response, GOPE00CZE | 0.197 | 0.196 | 0.200 | 0.195 | 0.192 | 0.186 | 0.185 |

### 6.2 A population read: every reset of two hours (`e1_drop_walk.py`, `e1_cover_gaps.py`)

For every drop in `network_amb_state.csv`: |value after the epoch's solve minus the kept value| over the restored
sigma. A kept value more than three restored sigmas from the epoch's answer is a prior the solve had to fight: the
fault of stage E1 at any size. DOY 144, TOW 1800 to 9000, runs `_rap_o2off_n144{,r}` against `_rap_o2j_n144{,r}`
(default reference receivers / GOPE00CZE); the class is the detector tier at the drop's epoch and line of sight.

| class of reset | resets | kept value beyond 3 sigma, no response | with the response |
| --- | --- | --- | --- |
| slip between consecutive epochs | 365 / 372 | 8 / 8 (6 / 6 beyond 10 sigma) | 0 / 0 |
| return after a gap (tier `T0_gap`) | 92 / 94 | 9 / 11 (4 / 4 beyond 10 sigma) | 9 / 11 |

| after-effect, TOW 1800 to 9000 | no response | response |
| --- | --- | --- |
| T4 refloats | 22 / 30 | 15 / 15 |
| wet-delay erases by RAIM attribution | 14 / 14 | 9 / 9 |
| satellite clock anchors decoupled | 15 / 32 | 0 / 2 |
| one-epoch wet-delay steps of 2 cm or more (5 cm or more) | 19 / 29 (9 / 13) | 14 / 8 (7 / 2) |
| wet-delay erases at a slip latch | 194 / 194 | 195 / 195 |
| chain cut-release events | 313 / 318 | 313 / 318 |

The steps of 2 cm or more that remain: BSHM00ISR at TOW 2820 to 3210 and KIT300UZB at TOW 8280 (+8.4 / +8.3 cm).

**The uncovered class.** A band or an arc that returns after a gap with a shifted level. The gap instrument measures
the shift and declares it (`sig_resume`, `reentry_relevel`, `arc_resume` with the verdict shifted; GF shift 8.6 to
23.5 m), and the response to the declaration is the same drop at the pre-gap value. The cover sees nothing, because
the raw screen forms a step only from the immediately preceding epoch. Cases with the response on: KIT300UZB-E34 L1C
at TOW 8280 (gap of 4 epochs, shift 12.48 m, restored sigma 0.057 m, 60 sigma; a T4 refloat and a wet-delay erase
at TOW 8280, 8310 and 8340); DARW00AUS-E25 L1C at TOW 5700 (9.40 m, 21 sigma); PARK00AUS-E15 L1C at TOW 1800
(15.81 m, 20 sigma); KIT300UZB-G30 both bands at TOW 540; BOGT00COL-G18 both bands at TOW 6300 (a whole-arc return).

### 6.3 Judgement

1. **The diagnosis is right and the response is the right kind.** It is a reset, not a repair: the value is kept and
   made irrelevant, the band is re-determined by its own data. On its own population it is complete (8 to 0).
2. **It covers half of the population.** Returns after a gap produce as many badly wrong kept values as slips on
   this day, and one of them steps a receiver's wet delay by 8 cm with the response on.
3. **Small slips.** The cover cannot size a step below the code noise. The pull of a kept value wrong by J is about
   J Vd / (max(Vfirst, k^2 J^2) + Vd), Vd the variance the epoch's data give the float; at k = 1 it peaks near
   J equal to the first sigma. The one-cycle arm of the builder (35 mm on another satellite's phase bias, 0.2 cycle
   on other receivers' floats) is this leak; 3 to 5 cycles are predicted worse. The arc of the simulator event was
   two epochs old with both bands loose; a settled arc with the other band intact is the easier case.
4. **The project already holds a blanket form.** `network.amb_float_drop_var_m2` (ledger KEEP_OFF) floors every
   drop. Card RF-3: it removes the post-drop walk and missed the hour-1 bars of September. Card ICL6, at 100 m2 over
   24 h on two days: T4 arcs 422 to 80 and 549 to 176, clock-anchor decouplings 5 and 31 to 0, positions neutral.
   Those are the symptoms of stage E1. The builder's key is the selective form (it acts where a step is measured);
   the two are to be compared before k is tuned.
5. **The test bed covered one regime.** No held link and no tree link was involved in the simulator event, so its
   fixing bar passed without the chain reacting.

**Orders sent to the builder** (after its report of the formal-sigma re-run, which is running as seven arms).
(1) Cover the gap-return latch: a band declared shifted is freed like a slipped band, by its phase-minus-code step
across the gap or outright; for a whole-arc return the GF shift is no bound on a band's shift. Bars: on these two
hours and both reference receivers, zero resets with a kept value beyond three restored sigmas in both classes;
KIT300UZB's step at TOW 8280 below 1 cm; refloats, erases and alarms not above the present run's; one injected
return of each kind on the simulator. (2) The slip size on the simulator: a flag with no slip (the yardstick of
what any latch costs), 1, 4 and 10 cycles; the same on a settled arc; the 288-cycle slip on a tree link and on a
held link of a settled receiver. (3) Three arms on the same events and hours: the response with the gap cover; the
existing drop floor at 100 m2 alone; the response with the gap cover and a floor on declared slips and shifts only.
Read: the table of section 6.2, the reach table, and hour 1 of a cold start.

**Corrections to this note's own bars.** The clause "the faulty receiver's wet delay within 2 cm of truth" of
section 3.4 was badly posed (the control fails it); the builder's reading against the control is accepted.

**Decision** (open to review). The response is accepted as the base of the next runs. No claim of a complete
response is made before the gap-return class reads zero in the table of section 6.2. The prediction of section 5.1
(the formal-sigma departures at CPVG00CPV and MRO100AUS go) has its precondition met at CPVG00CPV; the re-run
decides it.

### 6.4 2026-11-15: the formal-sigma pairs with the response on, re-graded (the prediction of section 5.1)

The builder's runs `_rap_o2s1j_n144{,r,b}` (Galileo L1X/L5X held with the odds at the formal sigma,
`network.amb_float_drop_jump_k` 1; card NRF part 5 section 28 on its branch, commit 4700bb8). Audits of this
session (`o2_audit.py`, `o2_audit_wu.py`):

| audit | without the response (section 4) | with the response |
| --- | --- | --- |
| one start, default reference receivers against GOPE00CZE | fails (7 to 11 departing links per run) | L1X 0 of 78 109, L5X 0 of 78 190 |
| two starts, wind-up branches removed | fails | 0 of 65 962, 0 of 66 032 |
| two starts, arcs born together | fails | 0 of 21 198, 0 of 20 934 |

Held share by quarter hour (start-0 run): 0, 9, 26, 58, 78, 85, 88, 81, 84, 85, ...; last half hour 86.8 %
(twice the formal sigma: about 60 %). 356 decisions; releases: gap 92, slip 17; 7 alarms, as the key-off run.

**Reading.** The prediction of section 5.1 holds and is exceeded: every departure is gone, the two early ones
included. On this day the formal-sigma failures were the spread of slips, not the sigma.

**Decision** (open to review). The second day (DOY 145) is run at the formal sigma with the response (and the gap
cover of section 6.3 when it passes): two reference receivers and a second start. If it reads zero, the formal
sigma with the response is the working setting on observed data, and twice the sigma the fallback.

### 6.5 2026-11-15: CORRECTION to section 6.3 point 3: the reach of the one-cycle slip is ionosphere-like

Asked by the author: a satellite phase bias moving 35 mm should look suspicious to the tests. Read here: each
satellite's phase-bias change (event minus control, the constellation's median removed, per epoch) split as
d1 = I + G, d2 = gamma I + G (gamma = f1^2 / f2^2): I is the part a slant ionosphere absorbs, G the ionosphere-free
part. Largest over the run, GPS, in mm, two reference receivers:

| event | L2W as reported | ionosphere-like I | ionosphere-free G | G plus the clock |
| --- | --- | --- | --- | --- |
| one cycle (response on, not acting) | 35.19 / 5.08 | 21.21 / 3.00 | 0.49 / 0.13 | 0.46 / 0.31 |
| 288 cycles, response | 0.92 / 0.30 | 0.55 / 0.18 | 0.04 / 0.08 | 0.12 / 0.30 |
| 288 cycles, no response | 4195 / 2134 | 2534 / 1276 | 197 / 33 | 197 / 43 |

1. The one-cycle slip moves what a user's position and the ionosphere-free integer of a link can feel by 0.3 to
   0.5 mm, the level of the large slip with the response. The 35 mm is a shift of the ionosphere level shared by
   the satellite's phase biases, the slant ionospheres of its links and their floats, which only code determines.
   Point 3 of section 6.3 overstated it; the prediction for 3 to 5 cycles is to be read on G plus the clock.
2. Why no test fires: in this posture the satellite phase bias carries no time link (`phase_datum_time_linked`
   false, no hold at emission); it is solved at each epoch, and the carried quantities are the links' floats and
   the receiver's phase bias. A joint move of phase bias, ionosphere and floats leaves every phase residual
   unchanged; the rows that resist are code (decimetres) and the floats' own priors (centimetres to decimetres).
   The minimal detectable bias of one phase row at that epoch: median 4.2 cm, 95th percentile 8.7 cm, and 4 rows
   of 966 with none (new arcs).
3. Sent to the builder: the reach table reports the ionosphere-free part with the clock, the ionosphere-like part
   and the wide lane separately; a dated clarification is appended to its card; the slip-size sweep moves behind
   the comparison with the existing drop floor.
