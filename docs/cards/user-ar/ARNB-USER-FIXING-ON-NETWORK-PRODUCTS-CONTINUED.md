# ARNB: integer fixing at a user of the network's own products, continued

**STATUS**

- **id:** ARNB
- **topic:** user-ar
- **status:** open
- **summary:** Card ARN's continuation (that file nears the 60 KB bar). One question, pre-registered in section 2 and measured on 2026-11-14 (DOY 144, three network runs, users from 18:00): a network that has run for 18 hours gives a user no integers without a correction of the stream. The raw fractions stay at the null, the satellites stay on 5 to 12 reference frames per signal, and pairs that share a frame lean toward integers only with the emission hold off, short of the expected 0.10 cycle. The warm-up buys steadier GPS offsets (about half the step between 30 min windows) and 3 to 8 points of GPS held share when the offsets are removed offline; a cold network supports that fixing about an hour and a half after its start, and a float user is served alike by both. Sections 10 to 12 (the same evening, DOY 144, three receivers): the lead of section 9 is not the explanation of HARB00ZAF's gap between the two streams. Satellites that a user takes with the stream's clock and without the bias records of its own signals change the float position on both streams (first hour after a cold start on the stream as built: 248 to 53 mm and 252 to 65 mm at two receivers with those satellite-epochs left out; a few millimetres either way in later hours). The stream's start-up jumps are the solution settling: the IF-TD slip detector raises no flag on the simulated runs and 14 in the first half hour on observed data against 2 243 jumps. The fallback test continues in card [ARNC](ARNC-USERS-OF-SATELLITES-WITHOUT-BIAS-RECORDS.md).
- **dates:** 2026-11-14 (registered as the continuation; section 2 pre-registered before any graded run; sections 3 to 8 the results; section 9 two readings of the streams; section 10 pre-registered before its runs; sections 11 and 12 its results and a reading of the stream's jumps; section 13 the pointer to card ARNC; section 14 the jumps as a user sees them)
- **knobs / posture:** none new. Network runs are copies of the period's golden field run with the run length (`processing.max_duration_s`, `network.max_duration_s`), the start (`network.start_offset_hours`) or the four emission-hold keys changed; user runs are the consumer replay of card ARN with `network.start_offset_hours` set.
- **code:** none. Analysis tools only (`nw_warm.py`, `nw_root.py`, `nw_warm_grade.py`, `nw_warm_fix.py`, `nw_stream_diff.py`, `cmp_first_epoch.py` beside the tools of card ARN). Sections 9 to 12: `stream_common.py`, `uncorr_rows.py`, `rinex_drop.py`, `nw_xu.py`, `stream_crossings.py`, `stream_epoch_table.py`, `stream_jump_lift.py`, `events_by_time.py`.
- **run dirs:** `D:\GnssProcessorSupport\runs_nom\_rap_nw{w24,h24,c18}_n144` (network runs), `_rap_nw_obs<label>_144_<site>` and `_rap_nw_{fxb,obs}x<variant>_144_<site>` (user runs), corrected copies `_rap_nwx<variant>{gc,gw}_n144`; tools `D:\GnssProcessorSupport\presentation\agu26\fixing_control\tools\`. Section 11: `_rap_nw_obs{h18,w18,h00,c00}x_144_{harb,dav1,scrz}`, filtered observation files `_rap_nw_xu_inputs\`.
- **source:** card [ARN](ARN-USER-FIXING-ON-NETWORK-PRODUCTS.md); card [ARR](../../record/refound/ARR_integer_ambiguity_review_2026-11-07.md) sections 2.1, 6 and 7 (the conditions for integers at the user, the measurement of card SZE, the rulings of 2026-11-09)

**Outcome.** Open; measured on one day. Warm-up alone does not remove the missing integers at a user of the
network's products. After 18 hours the raw fractions read at the null as on a cold network, and the satellites
still sit on 5 to 12 reference frames per signal. With the emission hold off, pairs that share a frame lean toward
integers (N_1 0.14 to 0.21 against 0.24 to 0.28 for the others) and miss the expected 0.10. The warm network's GPS
offsets are about twice as steady, and the offline fixing gains 3 to 8 points of GPS held share.
A later check (sections 10 to 12) does not explain HARB00ZAF's worse positions on the holds-off stream by the
satellites without bias records; leaving those out changes float positions on both streams, most in the first hour
after a cold start. The stream's start-up jumps are the solution settling, not slip-detector triggers.

> Results are appended TO THIS FILE, below the text.

---

## 1. Scope

Card ARN nears the 60 KB bar; its results continue here. Nothing of card ARN is re-opened: its sections 5 to 15 stand
as written. Every run here is a measurement or an offline demonstration on copies of a product file. No producer
code, no key, no golden change, no bar frozen. The rulings of 2026-11-09 stand.

## 2. Pre-registered: a network that has run for most of a day (2026-11-14, before any graded run)

**The question.** Asked on 2026-11-14 after the results of card ARN: suppose the network always runs for about a day
from its cold start before any user applies its products, and every later start is warm. Do users of a converged
network get integers without any correction of the stream?

**The reading before the runs.** By the conditions of card ARR section 2.1:

- C3 (a converged value at the start of a generation of the emission hold) is met once the held value is taken after
  the warm-up. As built, a held value is renewed only when a satellite starts a new generation, so a longer run
  replaces the cold values one generation at a time.
- C1 and C2 (one reference receiver per signal family; integer targets at handovers) do not depend on convergence.
  The user's between-satellite ambiguity carries the term [delta_a(p) - delta_a(s)] / lambda, the difference of the
  phase offsets of the two satellites' reference receivers. It holds no estimation error that time could remove.
- Card SZE (card ARR section 6) measured the estimate before the hold in hours 2 to 6: integer between satellites
  only on pairs that share one never-changed reference receiver, 0.5 to 11 % of pair-epochs.

So the expectation is that warm-up alone does not give a user integers, and that it removes the cold-value part of
the emission hold's effect.

**Network runs** (DOY 144, 56 receivers, executable `gnss_processor_rscb3.exe` 5ecf8e1c765fafe3; each a copy of
`_rap_p26_n144/arm.json` with one change, made by `nw_warm.py`):

| run | change | purpose |
| --- | --- | --- |
| `_rap_nww24_n144` | `processing.max_duration_s` and `network.max_duration_s` 86400 | the stream as built, after most of a day |
| `_rap_nwh24_n144` | the same, and `network.emit_pb_hold`, `emit_pb_hold_trip_generation`, `emit_pb_hold_phase_clock`, `closed_loop.pb_hold_presence_flip` false | the converged estimate itself on the stream, no freeze |
| `_rap_nwc18_n144` | `network.start_offset_hours` 18 (6 h) | the control: the network cold at 18:00, in the same hours |

Smoke runs of 10 epochs passed for the three before the launch. The first 10 epochs of the 24 h arm are byte-identical
to `_rap_p26_n144/products.txt`, and the cold control's first epoch is 18:00:00. The identity of the first six hours of
`_rap_nww24_n144` against `_rap_p26_n144` is checked when the run ends.

**User runs.** The observe arm of card ARN (consumer replay, between-satellite differences, static position, no fix
accepted, resolver log at debug), started at 18:00 on each stream (`network.start_offset_hours` 18 in the replay of
the 24 h streams; the replay of the cold control begins with its stream), at the 17 test receivers and at the
network's 56 receivers. On the holds-off stream also from 00:00 at the 17 test receivers. Reference: the runs of
card ARN on `_rap_p26_n144` (network cold at 00:00, users from 00:00).

**Readings, with the expectation written before any of them was graded** (diagnostic; no bar is frozen):

1. *Raw fractions.* Reference-free satellite pairs at the 17 test receivers from the second hour of each user run,
   wide lane and ionosphere-free N_1 (uniform null: mean 0.25, 20 % within 0.10). Expected: at the null over all pairs
   on the three streams, as on the cold stream of card ARN.
2. *Pairs that share a reference frame* (`nw_root.py`). The root of a satellite and signal at an epoch is the receiver
   of its last cold pin (first reference receiver, a cold handover, or a reference that returns after an epoch
   without one), kept across continuous handovers; the network's `network_phase_anchor.csv` and the `PBGAUGE` rows of
   `network_events.csv` give it. A pair is "same" when its two satellites have the same root on both signals of the
   receiver. Expected on the holds-off stream: same-root pairs near integers (mean |N_1 fraction| below 0.10), the
   other pairs at the null. Expected on the stream as built: same-root pairs near integers only where every
   generation of the hold started after the warm-up.
3. *Steadiness.* The step between 30 min windows of the offsets fitted per satellite and signal (`nw_offsets.py`), and
   each test receiver's fractions with the offsets fitted on all the others (`nw_fcb.py`, 90 min bins). Expected: the
   warm stream steadier than the cold streams (card ARN: median step 0.04 to 0.07 cycle) and the fractions with
   offsets at or below the cold streams' (card ARN: N_1 0.056 to 0.065).
4. *Fixing with past-only offsets* (`nw_warm_fix.py`; an addition of the session). Offsets fitted without STPM00SPM,
   WTZZ00DEU and BAKE00CAN from the 90 min before each epoch, removed from a copy of the stream, and fixing at the three
   from 18:00: on the warm stream with the offsets from receivers that have used it since 12:00, against the cold
   control with everything started at 18:00. Expected: more pair-epochs held on the warm stream. Card ARN measured the
   cost of the past-only fit on a cold six-hour run at 4 to 7 points of GPS held share.

**Limits known before the runs.** One day. The users start after 18 hours of the network, not after a full day; a
run across two days would be needed for more. The holds-off stream is a measurement device: card SZE showed that a
float user degrades on it. Reading 2 rests on the root as defined here, which assumes that a continuous handover
keeps the frame and that receiver phase offsets are steady over hours.

---

## 3. 2026-11-14: the runs, the identity check and the cold control

**Runs.** The three network runs ended with exit 0: `_rap_nwc18_n144` (1 673 s), `_rap_nww24_n144` (7 473 s) and
`_rap_nwh24_n144` (7 531 s). The user runs on the cold control (17 test receivers and 56 network receivers from 18:00)
all ended with exit 0. The user runs on the 24 h streams are reported in the sections that follow.

**The identity check of section 2 failed, and the cause is a property of the run length, not a fault.** The first six
hours of `_rap_nww24_n144` are not byte-identical to `_rap_p26_n144`, although the 10-epoch smoke was.
`NetworkSbasis::latchBroadcastGroupDelays` (called from `src/core/network/driver/ApplicationNetworkSetup.cpp`) pins
each satellite's group delay once per run at the low median of its healthy navigation records whose clock epoch lies
in the processing window. The window follows the run length: the 24 h window moves 23 satellites off the navigation
file's first record (22 Galileo satellites by one to four broadcast steps of 0.0698 m on C1, and J08), the 6 h window
moves J08 alone. Over the 720 common epochs (`nw_stream_diff.py`):

| records | largest difference between the two streams |
| --- | --- |
| GPS code biases, clocks, phase biases | 0.11 mm, 0.29 mm, 1.40 mm |
| Galileo code biases | a constant per satellite of up to 0.5007 m; 3.61 mm once that constant is removed |
| Galileo clocks | 12.54 mm |
| Galileo phase biases | the same constants; one departure of 301.49 mm once they are removed (E25 L5Q: -0.3755 m in its first generation of the hold, -0.0742 m in its second; not traced) |

Consequences. Comparisons inside one stream are unaffected. The cold six-hour stream of card ARN and the 24 h streams
differ in Galileo by a constant per satellite, so results are compared between streams as fractions, held shares and
positions, never record by record. A run's Galileo code-bias datum depends on its length: two runs of different
lengths over the same hours are not the same stream, and a service that never stops has no window as defined today.

**Reference receivers over the day** (`nw_root.py --lineage`; the root of a satellite and signal as defined in
section 2):

| run | first reference | cold handover | continuous handover | return after an epoch without a reference | roots per signal | same-root share of satellite pairs |
| --- | --- | --- | --- | --- | --- | --- |
| `_rap_nwc18_n144` (6 h) | 216 | 37 | 304 | 67 | 6 to 13 | 10 to 30 % |
| `_rap_nww24_n144` and `_rap_nwh24_n144` (24 h, identical) | 216 | 58 | 1 311 | 237 | 4 to 12 | 10 to 32 % (Galileo L1C: 49 to 66 % from hour 12, on 4 to 5 roots) |

The number of roots does not fall with the hours: at hour 18 the 24 h runs hold 5 to 12 roots per signal, the cold
control 6 to 8 at its start. A long run does not gather the satellites onto one reference frame.

**Reading 1 on the cold control** (raw fractions, 17 test receivers, 19:00 to 24:00; uniform null 0.25):

| signals | pair-epochs | wide lane mean | N_1 mean |
| --- | --- | --- | --- |
| Galileo L1C/L5Q | 41 066 | 0.227 | 0.253 |
| Galileo L1X/L5X | 110 067 | 0.248 | 0.241 |
| GPS L1C/L2W | 106 873 | 0.252 | 0.252 |
| GPS L1W/L2W | 38 441 | 0.245 | 0.249 |

At the null, as expected. Reading 2 on the cold control: the same-root classes (6 to 23 % of pair-epochs at the test
receivers, every one in a cold generation of the hold) read at the null as well (the two largest: Galileo L1X/L5X
19 607 pair-epochs, wide lane 0.281, N_1 0.236; GPS L1C/L2W 11 193, 0.237, 0.281).

**Reading 3 on the cold control.** Each test receiver's fractions with the offsets fitted on all the others (90 min
bins), and the step of the fitted offsets between 30 min windows:

| signals (receivers) | wide lane, N_1 with offsets | card ARN's cold run from 00:00 | step, centred fit: median, p90 (cycle) | step, past-only fit |
| --- | --- | --- | --- | --- |
| GPS L1C/L2W (53) | 0.104, 0.097 | 0.093, 0.091 | 0.071, 0.195 | 0.075, 0.197 |
| GPS L1W/L2W (19) | 0.106, 0.074 | 0.100, 0.071 | 0.074, 0.228 | 0.081, 0.239 |
| Galileo L1C/L5Q (34) | 0.081, 0.096 | 0.101, 0.120 | 0.039, 0.147 | 0.043, 0.142 |
| Galileo L1X/L5X (39) | 0.088, 0.090 | 0.081, 0.071 | 0.042, 0.120 | 0.042, 0.122 |

(Card ARN's cold run, steps: centred 0.055, 0.183; 0.076, 0.235; 0.042, 0.155; 0.031, 0.111 in the row order above;
past-only 0.062, 0.188; 0.080, 0.235; 0.046, 0.168; 0.035, 0.121.) The cold control behaves as the cold run of card
ARN did: the hours of the day do not change the picture.

**Float positions on the untouched cold control** (3D median per hour, mm): STPM 56.7, 18.2, 12.9, 10.4, 10.2, 11.5;
WTZZ 111.4, 15.8, 8.6, 7.6, 6.9, 5.0; BAKE 23.4, 5.9, 4.4, 2.4, 2.0, 1.4; HARB 122.0, 24.6, 18.7, 14.8, 13.0, 13.9;
CEDU 57.6, 13.4, 15.2, 3.4, 2.9, 3.5; KOKB 47.5, 14.6, 19.0, 22.5, 23.8, 16.0.

## 4. 2026-11-14: fixing on the cold control, and a correction of the tool

Offsets from the 70 other receivers (73 observers less the three held out), removed from a copy of the cold control's
product file; fixing with the phase check at STPM00SPM, WTZZ00DEU and BAKE00CAN (`nw_warm_fix.py`). "First" is the
time of the first held pair-epoch. Positions are the 3D median per hour in mm, fixed and float on the same copy.

| case | receiver | GPS held | Galileo held | first (GPS, Galileo) | fixed | float |
| --- | --- | --- | --- | --- | --- | --- |
| `c18n`: centred fit, user from 18:00 | STPM | 65.1 % of 2 911 | 68.4 % of 2 051 | 18:30, 18:19 | 50.8, 11.1, 25.7, 13.7, 13.8, 13.5 | 56.7, 14.9, 30.7, 17.3, 18.4, 19.7 |
| | WTZZ | 92.5 % of 3 327 | 94.7 % of 2 951 | 18:49, 18:44 | 111.5, 13.1, 7.4, 12.2, 11.5, 10.3 | 111.4, 17.3, 9.1, 12.5, 13.0, 11.4 |
| | BAKE | 91.3 % of 3 109 | 94.2 % of 3 084 | 18:18, 18:18 | 19.6, 7.6, 8.4, 6.1, 6.2, 6.1 | 22.3, 5.0, 9.0, 6.6, 6.4, 6.3 |
| `c19c`: past-only fit, user from 19:00 (5 h) | STPM | 75.1 % of 2 138 | 69.0 % of 1 560 | 19:27, 19:26 | 25.7, 34.3, 24.2, 21.1, 16.3 | 61.2, 45.2, 29.5, 27.0, 25.2 |
| | WTZZ | 85.4 % of 2 826 | 93.2 % of 2 334 | 19:32, 19:28 | 97.1, 18.9, 6.3, 8.0, 9.9 | 117.4, 35.9, 11.8, 8.3, 10.2 |
| | BAKE | 84.2 % of 2 461 | 96.0 % of 2 428 | 19:32, 19:21 | 35.8, 12.6, 9.4, 6.5, 8.6 | 39.5, 19.3, 15.7, 8.5, 9.0 |
| `c18c`: past-only fit, user from 18:00 | STPM | 4.9 % of 2 909 | 18.1 % of 2 051 | 18:38, 18:19 | 56.5, 48.3, 59.7, 54.9, 50.2, 46.5 | 56.7, 80.8, 73.3, 60.9, 55.0, 50.6 |
| | WTZZ | 40.9 % of 3 326 | 30.2 % of 2 946 | 19:08, 18:50 | 111.4, 79.7, 80.4, 36.0, 26.6, 25.4 | 111.4, 94.9, 89.8, 38.0, 30.6, 29.2 |
| | BAKE | 31.5 % of 3 109 | 24.1 % of 3 083 | 18:41, 19:31 | 22.4, 131.5, 183.0, 113.7, 62.2, 40.6 | 23.4, 164.5, 190.5, 109.0, 55.2, 34.2 |

**The case `c18c` is damaged by the tool, not by the stream.** With the past-only fit the copy's records are unchanged
before the first value (19:00); at 19:00 every phase bias steps by its whole offset, with no flag. A user who is
already running takes the step into its float ambiguities (float positions of 5 to 19 cm for hours). The past-only
check of card ARN did not show this because its users started an hour after the stream. Decision of the session:
variants with the user starting at 19:00, after the first values, were added for the cold control and for the 24 h
stream (`c19c`, `w19c`), so that the two are compared on equal terms; `c18c` stays on the record as the cost of a
first application without a flag. A service would declare the first application as a discontinuity.

**Reading.** A cold network supports fixing at a user about an hour and a half after its start: with the past-only
fit a user from 19:00 holds its first pairs between 19:21 and 19:32 and 84 to 96 % of its pair-epochs at WTZZ and
BAKE (69 to 75 % at STPM). Against the centred fit the past-only fit costs 7 points of GPS held share at WTZZ and
BAKE and nothing for Galileo (the two cases differ in the user's start as well). Where integers are held, the fixed
position is better than the float on the same copy in the first two hours of the 19:00 user and within millimetres of
it afterwards.

## 5. 2026-11-14: the author's direction, and two decisions of the session

- On the offsets: "I prefer the chain, but this offset idea might be useful as comparison after." The ruling of
  2026-11-09 stands (the chain first). The offset stage is not built in the network; the offline results of card ARN
  and of this card are the benchmark for a later comparison.
- On where to work: "the sim is so much faster, it seems like we should target our effort in the sim first before the
  "observed" [...] data." The simulator study of the chain at the user is card IRDU (branch `irdu/user-integers`, not
  merged). Its first result, 2026-11-14: simulated users get no integers from the chain's stream on its quiet bed
  (the frame is integer at its centre, the value sent each epoch wanders with the depth of the tree, and a release
  every 100 s restarts the carried values). A follow-up on why the carried receiver phase bias stays loose was passed
  to that card the same day.
- **Deviation from section 2, reading 4.** The case with offsets from receivers that had used the 24 h stream since
  12:00 (`w12c`, `w12d`, `w12n`) was not run: its 56 twelve-hour observer runs were stopped after six had started, to
  free the machine for the simulator work (the 17 test-receiver observers from 12:00 ran and are unused). Reading 4 on
  the 24 h stream is therefore the comparison on equal terms from 19:00 (`w19c` against `c19c`, observers since
  18:00). What is not measured: a user who starts at 18:00 on a stream whose offsets already exist.
- The user runs still queued when the direction came were set to a lower process priority, so their timings are not
  quiet-machine timings (no timing is graded here).

## 6. 2026-11-14: the 24 h stream as built (`_rap_nww24_n144`), users from 18:00

**Runs.** On the two 24 h streams, 17 test receivers and 56 network receivers from 18:00 each, and 17 test receivers
from 00:00 on the holds-off stream: 163 observe runs, all exit 0. Grades: `nw_warm_grade.py warm`, `nohold` and
`nohold00`; the outputs are kept beside the tools (`fixing_control\grades_arnb\`).

**Reading 1, raw fractions** (17 test receivers, 19:00 to 24:00; uniform null 0.25):

| signals | pair-epochs | 24 h stream as built: wide lane, N_1 | holds-off stream | cold control (section 3) |
| --- | --- | --- | --- | --- |
| Galileo L1C/L5Q | 41 066 | 0.243, 0.224 | 0.247, 0.244 | 0.227, 0.253 |
| Galileo L1X/L5X | 110 067 | 0.248, 0.246 | 0.199, 0.235 | 0.248, 0.241 |
| GPS L1C/L2W | 106 873 | 0.253, 0.252 | 0.254, 0.240 | 0.252, 0.252 |
| GPS L1W/L2W | 38 441 | 0.254, 0.240 | 0.234, 0.241 | 0.245, 0.249 |

At the null, as expected: eighteen hours of running leave the raw stream as far from integers as a cold start.

**Reading 2 on the stream as built.** Same-root pairs whose hold generations all began after the warm-up (708 to
2 744 pair-epochs per class, on Galileo L1C/L5Q and GPS L1C/L2W only): wide lane 0.154 to 0.261, N_1 0.208 to 0.301.
Same-root pairs with a cold generation (2 004 to 29 224 pair-epochs per signal pair): wide lane 0.260 to 0.305, N_1
0.189 to 0.242. Expected: near integers where every generation began after the warm-up. Not met: the warm
generations read at the null on N_1.

**Reading 3, steadiness.** Each test receiver's fractions with the offsets fitted on all the others (90 min bins),
and the step of the fitted offsets between 30 min windows (median, p90, cycle); the cold control of section 3 in
brackets:

| signals (receivers) | wide lane, N_1 with offsets | step, centred fit | step, past-only fit |
| --- | --- | --- | --- |
| GPS L1C/L2W (53) | 0.096, 0.093 (0.104, 0.097) | 0.037, 0.110 (0.071, 0.195) | 0.040, 0.115 (0.075, 0.197) |
| GPS L1W/L2W (19) | 0.096, 0.073 (0.106, 0.074) | 0.047, 0.162 (0.074, 0.228) | 0.055, 0.198 (0.081, 0.239) |
| Galileo L1C/L5Q (34) | 0.077, 0.079 (0.081, 0.096) | 0.040, 0.152 (0.039, 0.147) | 0.042, 0.154 (0.043, 0.142) |
| Galileo L1X/L5X (39) | 0.084, 0.090 (0.088, 0.090) | 0.036, 0.104 (0.042, 0.120) | 0.035, 0.108 (0.042, 0.122) |

Expected: the warm stream steadier. Met for GPS: the fitted offsets step about half as much between 30 min windows
(median 0.037 to 0.055 against 0.071 to 0.081 cycle). Galileo is unchanged (0.035 to 0.043 on both streams). The
fractions with offsets are at or slightly below the cold control's.

**Reading 4, fixing with past-only offsets, users from 19:00** (`w19c`; offsets from the 70 other receivers, which
have used the stream since 18:00; `c19c` of section 4 in brackets). "First" is the time of the first held pair-epoch.

| receiver | GPS held | Galileo held | first (GPS, Galileo) | fixed 3D median per hour (mm) | float on the same copy |
| --- | --- | --- | --- | --- | --- |
| STPM | 77.9 % of 2 101 (75.1 %) | 74.9 % of 1 548 (69.0 %) | 19:30, 19:30 | 41.4, 36.3, 23.5, 20.1, 11.5 | 54.8, 46.2, 27.5, 22.7, 16.5 |
| WTZZ | 93.1 % of 2 821 (85.4 %) | 95.1 % of 2 329 (93.2 %) | 19:40, 19:22 | 49.1, 16.7, 10.0, 7.4, 6.3 | 52.4, 34.4, 17.0, 7.6, 6.4 |
| BAKE | 90.5 % of 2 486 (84.2 %) | 95.7 % of 2 453 (96.0 %) | 19:28, 19:20 | 99.2, 14.6, 6.1, 4.9, 11.7 | 101.7, 14.8, 10.2, 5.5, 11.8 |

Expected: more pair-epochs held on the warm stream. Met for GPS at the three receivers, by 2.8, 7.7 and 6.3 points;
Galileo +5.9, +1.9 and -0.3. The first held pair-epoch comes at the same time as on the cold control (19:20 to 19:40
against 19:21 to 19:32). The case with the user from 18:00 on the 24 h stream (`w18c`) carries the damage of `c18c`
(GPS held 69.6 % at STPM, 18.1 % at WTZZ, 28.3 % at BAKE) and stays on the record only.

**Float positions on the untouched 24 h stream, users from 18:00** (3D median per hour, mm), the cold control of
section 3 in brackets: STPM 59.8, 19.2, 12.7, 10.4, 10.2, 11.5 (56.7, 18.2, 12.9, 10.4, 10.2, 11.5); WTZZ 114.0,
15.7, 8.6, 7.6, 6.9, 4.9 (111.4, 15.8, 8.6, 7.6, 6.9, 5.0); BAKE 23.2, 5.8, 4.2, 2.4, 1.9, 1.4 (23.4, 5.9, 4.4, 2.4,
2.0, 1.4); HARB 36.7, 12.7, 30.2, 18.0, 15.8, 16.8 (122.0, 24.6, 18.7, 14.8, 13.0, 13.9); CEDU 58.7, 14.4, 15.8, 3.5,
2.9, 3.6 (57.6, 13.4, 15.2, 3.4, 2.9, 3.5); KOKB 46.0, 14.8, 19.7, 22.5, 23.8, 16.1 (47.5, 14.6, 19.0, 22.5, 23.8,
16.0). With the hold, a float user is served alike by a network that started with it and by one that has run for 18
hours: five of the six receivers agree within 1.5 mm from the second hour (HARB00ZAF differs both ways).

## 7. 2026-11-14: the holds-off stream (`_rap_nwh24_n144`), users from 18:00 and from 00:00

**Reading 2, pairs that share a reference frame.** Mean fractions (wide lane, N_1) by root class; "same" classes
are split by the number of continuous handovers since the root's cold pin.

| signals, users from | largest same-root class | other same-root classes | different roots |
| --- | --- | --- | --- |
| Galileo L1X/L5X, 18:00 | 3 or more handovers, 29 111: 0.114, 0.138 | none | 74 384: 0.233, 0.273 |
| GPS L1C/L2W, 18:00 | 3 or more, 12 882: 0.117, 0.181 | 1 to 2, 1 782: 0.124, 0.340 | 86 309: 0.274, 0.244 |
| Galileo L1C/L5Q, 18:00 | 3 or more, 4 736: 0.257, 0.191 | 1 to 2, 646: 0.083, 0.176 | 12 382: 0.257, 0.283 |
| GPS L1W/L2W, 18:00 | 3 or more, 2 004: 0.187, 0.213 | none | 24 063: 0.232, 0.242 |
| Galileo L1X/L5X, 00:00 | 1 to 2, 10 421: 0.100, 0.193 | none, 8 489: 0.094, 0.168 | 68 597: 0.252, 0.272 |
| GPS L1C/L2W, 00:00 | 1 to 2, 17 289: 0.154, 0.192 | 3 or more, 2 214: 0.160, 0.228; none, 2 195: 0.135, 0.168 | 73 651: 0.256, 0.256 |
| Galileo L1C/L5Q, 00:00 | 1 to 2, 10 387: 0.071, 0.126 | none, 2 917: 0.071, 0.125; 3 or more, 277: 0.054, 0.109 | 22 694: 0.239, 0.252 |
| GPS L1W/L2W, 00:00 | 1 to 2, 5 927: 0.235, 0.227 | 3 or more, 1 549: 0.247, 0.237; none, 481: 0.116, 0.177 | 30 533: 0.264, 0.253 |

Expected: same-root pairs near integers (mean |N_1 fraction| below 0.10), the other pairs at the null. The other
pairs are at the null (N_1 0.24 to 0.28). The same-root pairs lean toward integers and do not reach the bar: in the
largest class of each signal pair N_1 reads 0.138, 0.181, 0.191 and 0.213 from 18:00 and 0.193, 0.192, 0.126 and
0.227 from 00:00, the wide lane 0.07 to 0.16 except on GPS L1W/L2W and on the Galileo L1C/L5Q class with three or
more handovers (0.19 to 0.26). The 18 hours do not sharpen them: no signal pair moves from the cold hours' level to
the bar. No trend with the number of handovers is clear in these classes.

**Reading 3 on the holds-off stream.** With the offsets of the others (90 min bins) the fractions read wide lane
0.085 to 0.114 and N_1 0.127 to 0.144, against 0.073 to 0.093 on the stream as built: an offset held constant over
90 min fits a stream that keeps moving less well. Steps of the fitted offsets (centred, median and p90): GPS L1C/L2W
0.038, 0.103; GPS L1W/L2W 0.053, 0.274; Galileo L1C/L5Q 0.040, 0.122; Galileo L1X/L5X 0.035, 0.101.

**Float positions** (3D median per hour, mm):

| receiver | holds off, from 18:00 | as built, from 18:00 | holds off, from 00:00 | as built, from 00:00 (card ARN) |
| --- | --- | --- | --- | --- |
| STPM | 42.1, 19.2, 24.6, 18.7, 16.0, 15.7 | 59.8, 19.2, 12.7, 10.4, 10.2, 11.5 | 287.7, 64.2, 28.4, 30.2, 33.6, 27.6 | 22.5, 6.1, 6.0, 5.4, 6.4, 6.2 |
| WTZZ | 101.2, 18.2, 12.3, 10.1, 7.0, 7.0 | 114.0, 15.7, 8.6, 7.6, 6.9, 4.9 | 488.2, 356.6, 189.4, 45.9, 58.0, 40.4 | 67.6, 12.5, 10.7, 7.5, 3.6, 2.6 |
| BAKE | 28.4, 34.1, 11.3, 12.5, 8.2, 7.1 | 23.2, 5.8, 4.2, 2.4, 1.9, 1.4 | 336.3, 88.6, 103.9, 42.4, 40.5, 48.4 | 54.7, 12.0, 11.3, 11.4, 7.2, 4.7 |
| HARB | 45.2, 29.8, 47.6, 28.0, 45.8, 55.1 | 36.7, 12.7, 30.2, 18.0, 15.8, 16.8 | 890.1, 111.5, 89.8, 49.9, 37.7, 31.1 | 248.2, 28.9, 7.4, 7.0, 7.3, 6.8 |
| CEDU | 66.7, 17.3, 24.4, 27.2, 18.0, 11.2 | 58.7, 14.4, 15.8, 3.5, 2.9, 3.6 | 334.1, 187.5, 27.1, 20.9, 20.9, 14.7 | 60.4, 10.3, 12.4, 5.5, 4.4, 3.9 |
| KOKB | 51.1, 19.3, 12.9, 17.7, 18.6, 14.0 | 46.0, 14.8, 19.7, 22.5, 23.8, 16.1 | 664.8, 309.4, 155.7, 72.3, 57.3, 30.0 | 70.6, 36.7, 15.3, 8.2, 7.1, 8.3 |

Without the hold a float user pays decimetres in the first hours of a cold network (second hour 64 to 357 mm against
6 to 37 mm) and centimetres after 18 hours of running (hours 4 to 6 from 18:00: 7 to 55 mm against 1.4 to 24 mm; KOKB
is the one receiver that is not worse). This is card SZE's cost of the holds-off stream, here on observed data.

## 8. 2026-11-14: verdict, and what is not verified

**The question of section 2.** A network that has run for 18 hours does not give a user integers without a
correction of the stream.

1. *Raw fractions:* at the null on the three streams. As expected.
2. *Pairs that share a reference frame:* with the hold off they lean toward integers while the other pairs sit at
   the null, in the cold hours and after 18 hours alike, and they miss the expected 0.10 on N_1; on the stream as
   built the warm generations read at the null. The frame is the mechanism, as expected; a warm-up does not make the
   same-root pairs integer, which was not expected.
3. *Steadiness:* the GPS offsets step about half as much on the 24 h stream; Galileo is unchanged. Expected, met for
   GPS.
4. *Fixing with past-only offsets, users from 19:00:* 3 to 8 points more GPS pair-epochs held on the 24 h stream and
   the same waiting time. Expected, met at three receivers on one day.

**What the warm-up buys and what it does not.** It buys steadier GPS offsets and a few points of held share once the
offsets are removed offline. It does not gather the satellites onto one reference frame (5 to 12 roots per signal at
hour 18, section 3), it does not make the raw stream integer, and it does not help a float user, who is served alike
by a cold network (section 6). For the offline comparison an hour of network run is enough to start fixing (section
4).

**Not verified.** One day (DOY 144) and users from 18:00 only; a start after a full day needs a run across two days.
Reading 4 rests on three receivers and on observers since 18:00; the case with observers since 12:00 was not run
(section 5). Why the gain in steadiness is on GPS only is not traced. The identity check failed for a traced cause
and one Galileo departure of 0.30 m is not traced (section 3). Reading 2 rests on the root as defined in section 2.
HARB00ZAF's float positions differ between the cold control and the 24 h stream both ways (first hour 122.0 against
36.7 mm; hours 3 to 6 13 to 19 against 16 to 30 mm), not traced.

**Standing.** Open. No producer code, no key, no golden change; the rulings of 2026-11-09 stand. The integer route
is card IRD's chain; by the author's decision of 2026-11-14 the offsets stay an offline benchmark (section 5).

## 9. 2026-11-14 (later): two readings of the streams that bear on sections 6 and 7

Read from the product files and the user traces already written; no new run. They follow card IRDU section 12
(branch `irdu/user-integers`, not merged), which found on the simulator that a float user of a holds-off stream is
hurt by (a) each satellite's own movement and declared steps and (b) a drift shared by every satellite's phase
correction, the latter only where the user also takes satellites that the stream does not correct.

**The shared drift on the two 24 h streams** (`stream_common.py`: per signal, the median over satellites of the
change between epochs of [code bias minus phase bias], summed over time; a clock change is in neither record):

| stream | window | GPS L1C, L1W, L2W (mm) | Galileo L1C, L1X, L5Q, L5X (mm) |
| --- | --- | --- | --- |
| holds off (`_rap_nwh24_n144`) | hours 18 to 24 | 98, 81, 137 | 301, 129, 533, 225 |
| holds off | hours 1 to 6 | 756, 768, 1 255 | 148, 195, 266, 347 |
| as built (`_rap_nww24_n144`) | hours 18 to 24 | 0.1, 3.3, 0.2 | 2.7, 0.8, 1.7, 0.5 |
| as built | hours 1 to 6 | 0.1, 6.1, 0.1 | 6.0, 4.1, 3.7, 2.9 |

(The range of the shared drift over the window.) With the holds off, every satellite's phase correction moves
together against the code corrections by a decimetre in six hours after 18 hours of running, and by about a metre
in the first six hours. As built, the emission hold removes it. Between-satellite differences cancel a shared term
for a user whose satellites all carry stream records.

**Satellites without a stream record at the test receivers** (rows of `closed_loop_sat_trace.csv` with empty
correction fields; the 17 observe runs from 18:00, the same rows on both 24 h streams): 5 362 of 380 038 rows
(1.4 %), at five receivers and on Galileo only:

| receiver | rows without corrections | satellites |
| --- | --- | --- |
| SCRZ | 332 of 2 384 (13.9 %) | E02, E30 |
| HARB | 2 286 of 23 168 (9.9 %) | E02, E14, E15, E30, E34, E36 |
| DAV1 | 2 348 of 26 602 (8.8 %) | E02, E14, E15, E21, E27, E30, E34, E36 |
| KOKB | 198 of 21 404 (0.9 %) | E18, E27, E29 |
| QUIN | 198 of 22 109 (0.9 %) | E18 |

The other twelve, STPM00SPM and WTZZ00DEU among them, have none. From 00:00 on the holds-off stream the count is
10 527 of 392 320 rows (2.7 %).

**What it means for sections 6 and 7.** At the receivers without such rows the shared drift cancels, so the cost of
the holds-off stream in section 7 is each satellite's own movement and its declared steps. HARB00ZAF, the receiver
whose float positions on the holds-off stream are the worst of the six (28 to 55 mm in hours 4 to 6) and whose
fixing card ARN left untraced, takes a tenth of its rows from Galileo satellites that the stream does not correct
for its signals. That is a lead, not a finding: no run here excludes those satellites.

**Not run, and why.** A replay of HARB, DAV1 and SCRZ with those satellites excluded would test the lead in minutes.
It was left for the author's return: the session's work in the author's absence was limited to the simulator.

## 10. 2026-11-14 (evening): the lead of section 9, pre-registered before any run

The author's go-ahead of 2026-11-14 ("Yes, run it") covers the replay that section 9 left open. This section was
written before any run of it.

**Measured first, from the traces and product files already written** (`uncorr_rows.py`, `nw_xu.py same`,
`stream_common.py`; no run):

1. *The rows of section 9 lack the bias records, not the clock.* Every one of them carries the stream's clock; the
   code-bias and phase-bias records of the receiver's two Galileo signals (L1C, L5Q) are absent. The receiver uses
   such a satellite with the clock alone. When the records arrive in mid-pass the phase-bias event resets the
   ambiguity (HARB00ZAF, E02 at 19:46: `flip_to_fresh;reset_flip`).
2. *Most of these satellites lack the records for part of a pass only.* HARB00ZAF from 18:00: E02 on 30 % of its
   rows (18:00 to 19:45), E14 19 %, E15 100 % (22:37 to 23:59, below 28 degrees), E30 72 %, E34 49 %, E36 4 %.
   DAV100ATA: E15 and E30 100 %, six others 5 to 67 %. SCRZ00BOL has data until 18:41 only on that start (E02 and
   E30, 100 %). An exclusion for the whole run, as section 9 worded the replay, would discard 30 to 96 % of rows
   that carry their records.
3. *The same satellite-epochs lack the records on both streams:* from 18:00, 1 277 of 1 277 at HARB00ZAF, 1 292 of
   1 292 at DAV100ATA and 166 of 166 at SCRZ00BOL (24 h stream as built against the holds-off stream); from 00:00,
   1 298, 1 503 and 2 321 (the 6 h stream as built of card ARN against the holds-off stream).
4. *The shared drift of the holds-off stream is mostly ionosphere-like.* The two bands' shared drifts combined
   (range over the window, mm):

   | signal pair | hours 18 to 24: ionosphere-free, geometry-free | hours 1 to 6: ionosphere-free, geometry-free |
   | --- | --- | --- |
   | GPS L1C/L2W | 38, 40 | 93, 499 |
   | GPS L1W/L2W | 6, 56 | 83, 487 |
   | Galileo L1C/L5Q | 11, 232 | 37, 118 |
   | Galileo L1X/L5X | 12, 97 | 16, 153 |

   A satellite's own ionosphere state takes the geometry-free part. What a user of a satellite without bias records
   cannot absorb is the ionosphere-free part: 11 mm over six hours on these receivers' Galileo signals from 18:00,
   37 mm in the first six hours of the network.
5. *The earlier runs give the lead little room.* From 18:00 DAV100ATA is better on the holds-off stream than on the
   stream as built in hours 1 to 5 (18.8, 9.1, 6.8, 11.0, 10.5 against 25.2, 29.6, 16.9, 15.5, 16.2 mm), with 8.8 %
   of its rows without records. From 00:00 the receivers without any such row pay as much on the holds-off stream
   as these three (section 7).
6. *The users' rule of card IRDU* (branch `irdu/user-integers`, key `network.closed_loop.drop_unrecorded`) asks the
   stream for a clock record. These satellites have one: the rule as built would leave them in.

**The test.** Per receiver and start, a copy of the observation file from which every satellite-epoch without a
phase bias in the earlier run's trace is removed (`rinex_drop.py`), and the earlier run's configuration with that
file and nothing else changed (`nw_xu.py`). Executable: `gnss_processor_rscb3.exe` (built 2026-11-13 22:50, before
every earlier run of this card). The copy stands for a rule that leaves a satellite out while the stream has no
phase-bias record for it. Arms at HARB00ZAF, DAV100ATA and SCRZ00BOL, no fix accepted:

| arm | stream | user start | earlier run (the comparison) |
| --- | --- | --- | --- |
| h18 | holds off, `_rap_nwh24_n144` | 18:00 | `_rap_nw_obsh18_144_<site>` |
| w18 | as built, `_rap_nww24_n144` | 18:00 | `_rap_nw_obsw18_144_<site>` |
| h00 | holds off, `_rap_nwh24_n144` | 00:00 | `_rap_nw_obsh00_144_<site>` |
| c00 | as built, `_rap_p26_n144` | 00:00 | `_rap_nw_obs_144_<site>` |

The arms on the stream as built are the control: that stream has no shared drift (section 9), so their change is
what the lost data costs. Smoke: 10 epochs of the filtered arm and of the unfiltered configuration at HARB00ZAF from
00:00; the unfiltered smoke must reproduce the first 10 epochs of the earlier run byte for byte.

**Deviation from section 9's wording, decided before the runs.** Removal epoch by epoch in place of
`processing.excluded_satellites` for the whole run, for the reason of item 2.

**Reading.** The 3D median and the mean Up per hour (`consumer_replay_hourly.csv`); change = filtered run less
earlier run, hours 2 to 6.

**Verdict rule, per receiver and start.** *Supported:* on the holds-off stream the hourly median falls by at least
5 mm in at least three of hours 2 to 6, and by at least twice the control's change in those hours. *Refuted:* the
change on the holds-off stream stays within 3 mm in each of hours 2 to 6, or is no better than the control's in
every one of them. *Undecided* otherwise. At HARB00ZAF from 18:00 the lead *explains the gap* when at least half of
the gap to the stream as built closes in hours 4 to 6 (gap 10.0, 30.0 and 38.3 mm).

**Expected.** From item 4 the lead predicts a change of a centimetre at most from 18:00 and of a few centimetres at
most from 00:00. Prediction: refuted or undecided at the three receivers, and HARB00ZAF's gap stays.

**Limits.** One day, three receivers, float positions only. Held integers under the removal are not graded: the
offline fixing of sections 4 and 6 exists for STPM, WTZZ and BAKE, none of which has such rows.

## 11. 2026-11-14 (evening): the lead of section 9, results

**Runs.** Smoke passed: the unfiltered configuration reproduced the first 11 epochs of the earlier run byte for byte
(position trace and satellite trace), and the filtered smoke held no row of the removed satellites. Twelve replays,
all exit 0 (`_rap_nw_obs{h18,w18,h00,c00}x_144_{harb,dav1,scrz}`; filtered files under `_rap_nw_xu_inputs\`). No
replay holds a row without a phase bias; the row counts are the earlier runs' less the removed satellite-epochs on
both signals (HARB00ZAF from 18:00: 23 168 less 2 554 = 20 614).

**Correction of section 10, item 1.** Not every such satellite-epoch lacks both records: on 268 of HARB00ZAF's
1 277 satellite-epochs from 18:00 (236 of 1 292 at DAV100ATA) one signal carries its phase bias and the other does
not (E34 and E36 at 20:30: L1C present, L5Q absent). The removal takes the satellite out in both cases.

**Change of the 3D median per hour** (mm; replay less earlier run, hours 1 to 6):

| receiver, start | holds-off stream | stream as built (the control) |
| --- | --- | --- |
| HARB00ZAF, 18:00 | -18.9, -14.9, -6.7, +1.7, -2.3, -7.7 | -7.5, -5.0, -4.4, -3.4, -3.7, -4.9 |
| DAV100ATA, 18:00 | +7.7, -1.8, +0.9, +0.5, +3.2, +0.2 | +1.7, +2.7, +2.3, +2.3, +2.0, +1.1 |
| SCRZ00BOL, 18:00 (41 min) | +7.8 | +16.0 |
| HARB00ZAF, 00:00 | -323.0, +81.8, +56.5, -1.5, +5.2, +2.6 | -195.4, -13.5, +0.9, +4.1, +2.5, +1.4 |
| DAV100ATA, 00:00 | -504.9, -169.9, -51.5, -4.7, -6.4, -16.3 | -187.0, -38.0, -13.4, -5.5, -4.1, -5.2 |
| SCRZ00BOL, 00:00 | -56.0, -2.4, +0.6, -7.6, -1.7, -6.3 | -18.0, +0.2, +0.1, +1.9, +2.0, +2.8 |

**The replays' medians** (mm, hours 1 to 6; the earlier runs' are in sections 6 and 7 and in card ARN):

| receiver, start | holds-off stream | stream as built |
| --- | --- | --- |
| HARB00ZAF, 18:00 | 26.3, 14.9, 40.9, 29.7, 43.5, 47.4 | 29.2, 7.7, 25.8, 14.6, 12.1, 11.9 |
| DAV100ATA, 18:00 | 26.5, 7.3, 7.7, 11.5, 13.7, 16.1 | 26.9, 32.3, 19.2, 17.8, 18.2, 11.6 |
| HARB00ZAF, 00:00 | 567.1, 193.3, 146.3, 48.4, 42.9, 33.7 | 52.8, 15.4, 8.3, 11.1, 9.8, 8.2 |
| DAV100ATA, 00:00 | 471.0, 108.0, 45.5, 62.5, 43.7, 24.7 | 65.3, 33.5, 10.7, 8.7, 8.1, 8.9 |
| SCRZ00BOL, 00:00 | 674.5, 32.7, 21.8, 26.8, 21.2, 56.4 | 118.6, 65.3, 47.1, 37.4, 32.0, 27.1 |

**Verdicts by the rule of section 10** (hours 2 to 6):

- *HARB00ZAF from 18:00: undecided.* Three hours fall by 5 mm or more (hours 2, 3 and 6); one of them (hour 2) by
  at least twice the control's change. *The gap is not explained:* in hours 4 to 6 it goes from 10.0, 30.0 and
  38.3 mm to 11.7, 27.7 and 30.6 mm against the earlier run on the stream as built (15.1, 31.4 and 35.5 mm against
  that stream's own replay).
- *DAV100ATA from 18:00: undecided by the letter* (the change stays within 3 mm except +3.2 mm in hour 5); no hour
  improves by 5 mm.
- *SCRZ00BOL from 18:00:* not gradable (41 min of data).
- *HARB00ZAF from 00:00: undecided.* Hours 2 and 3 are worse by 81.8 and 56.5 mm on the holds-off stream.
- *DAV100ATA from 00:00: supported by the rule as written* (hours 2, 3 and 6 fall by 169.9, 51.5 and 16.3 mm, 3.1
  to 4.5 times the control's change). The control falls by the same proportion (53, 56 and 37 % against 61, 53 and
  40 %): the earlier errors differ by a factor of four between the two streams, and a rule on absolute changes does
  not separate the streams there. A weakness of the rule as written, recorded and not re-scored.
- *SCRZ00BOL from 00:00: undecided.* Hours 4 and 6 fall by 7.6 and 6.3 mm while the control rises by 1.9 and 2.8.

**Against the expectation.** HARB00ZAF's gap stays, as predicted. The prediction "refuted or undecided at the three
receivers" fails by the letter at DAV100ATA from 00:00. The sizes exceed what the shared drift can give (11 and
37 mm of ionosphere-free drift over six hours, section 10 item 4), and the stream as built, which has no shared
drift, moves as well: the changes are not the shared drift's. The lead of section 9 is not the explanation.

**What the test found instead.** A satellite that a user takes with the stream's clock and without the bias records
of its own signals changes the user's float position on the stream as built too.

- *First hour after a cold start, stream as built:* HARB00ZAF 248.2 to 52.8 mm, DAV100ATA 252.3 to 65.3 mm,
  SCRZ00BOL 136.6 to 118.6 mm with those satellite-epochs left out.
- *Later hours, where it helps:* DAV100ATA from 00:00 71.5, 24.1, 14.2, 12.2, 14.1 to 33.5, 10.7, 8.7, 8.1, 8.9 mm
  (hours 2 to 6); HARB00ZAF from 18:00 12.7, 30.2, 18.0, 15.8, 16.8 to 7.7, 25.8, 14.6, 12.1, 11.9 mm.
- *Where it costs:* DAV100ATA from 18:00 +1.1 to +2.7 mm, HARB00ZAF from 00:00 +0.9 to +4.1 mm in hours 3 to 6,
  SCRZ00BOL from 00:00 +0.1 to +2.8 mm (the data lost), and on the holds-off stream HARB00ZAF's hours 2 and 3 from
  00:00.

**Why these satellites have no records** (product file, HARB00ZAF's six satellites at 18:00, 20:30 and 23:00): the
stream carries them on the other Galileo signal pair. E02, E14 and E30 at 18:00 hold code and phase biases for
C1X, C5X, L1X and L5X and none for C1C, C5Q, L1C and L5Q; the same for E30 at 20:30 and E15 at 23:00. The network
sees the satellite at those epochs through receivers of the other tracking type only.

**Not traced.** By which path a satellite without its bias records reaches the position (candidates: the code of
that satellite without its code bias during the convergence of its ambiguity; the reset when the records arrive in
mid-pass; on the holds-off stream the moving phase level). Why HARB00ZAF's hours 2 and 3 from 00:00 get worse on
the holds-off stream.

**Named, not run.**

1. *The fallback in place of the removal.* A measurement that misses a bias input takes a fallback and is not
   dropped (the project's standing rule for missing antenna and bias entries). Here the fallback is at hand: the
   same band's records on the other signal pair (C1X for C1C, C5X for C5Q; for a float user the phase biases too,
   with a declared discontinuity at both edges). The code biases of the two pairs agree to 8 and 12 mm in the
   median (C1C against C1X, at the first epoch and at 20:00; largest 30 and 46 mm) and to 1 and 4 mm (C5Q against
   C5X; largest 2 and 11 mm). Offline on a copy of the product file; it keeps the geometry that the removal gives
   up.
2. *Card IRDU's rule on these satellites:* it would need the bias records of the receiver's signals as its
   criterion, in place of the clock record (section 10, item 6).

**Limits.** One day, three receivers, float positions. No fixing run, no network code, no key.

## 12. 2026-11-14 (evening): when the holds-off stream jumps, and the slip detector

The author's remark of 2026-11-14 recalled the jumps of a network's first half hour as triggers of the
ionosphere-free time-differenced slip detector (`network.cycle_slip.if_phase_td`). Read from product and event
files already written (`stream_crossings.py`, `stream_epoch_table.py`, `stream_jump_lift.py`, `events_by_time.py`);
no run. The simulator side belongs to card IRDU and was assigned to that card's session the same evening; the three
simulated rows below are that card's runs, re-read here.

A *crossing* is a phase-bias record that changes by more than 0.25 cycle between consecutive epochs (the emitter's
discontinuity gate).

| stream | crossings | when | slip flags of the run |
| --- | --- | --- | --- |
| simulated, every link at its true integer (`_rap_irdu_q1_nr3`, 2 h) | 1 542 | all before 37.0 min; 5 min bins 483, 405, 287, 99, 152, 89, 23, 4 | IF-TD 0 of 63 574 evaluations; wide-lane detector 23 |
| simulated, the chain arm on the same bed (`_rap_irdu_zw_nr3`) | 1 798 | all before 48.0 min | IF-TD 0 of 63 574; wide-lane detector 23 |
| simulated, the chain arm on r3c (`_rap_irdu_zw_r3c`) | 2 610 | through the run (506 in hour 2) | IF-TD 0 of 114 115 (its reference gate once); wide-lane detector 33 |
| observed data, holds off (`_rap_nwh24_n144`, 24 h) | 4 343 | hour 1: 2 649 (1 853 in the first 10 min); hour 2: 672; then 8 to 269 per hour | IF-TD 616 (14 in the first 30 min, 26 per hour after hour 3); loss-of-lock flags 1 898 |

1. *Simulator.* The detector raised no flag in the three runs: no jump there is its doing. In the first 13 min 50 or
   more of the 58 records cross at nearly every epoch, and the change every satellite shares is 0.1 to 1.3 m per
   epoch: the whole solution moves while it settles. 772 of the 1 542 crossings remain once the shared change is
   removed. 68 crossings (4.4 %) sit beside a wide-lane detection of the same satellite, against 0.6 % at random.
2. *Observed data, first 10 min.* The same picture: 181 of 196 records cross at the first epoch, 130 or more of 204
   in the first 2.5 min, 39 to 155 per epoch until 10 min.
3. *Observed data, after 10 min.* 26 epochs of 2 860 hold 1 745 of the 2 490 crossings (20 or more records jump
   together). An IF-TD flag sits at 11 of the 26 (42 %) against 14 % of the other epochs; a gap re-entry re-level
   at 15 (58 % against 19 %); a prior-channel attribution of RAIM at 8 (31 % against 5 %); a cold datum handover at
   3 (12 % against 1 %). Satellite by satellite, 114 of the 4 343 crossings (2.6 %) sit beside an IF-TD flag of the
   same satellite, against 0.7 % at random.

**Reading.** The recollection is not confirmed for the start-up. On the simulator the detector is silent and the
jumps are the solution settling. On observed data the first ten minutes look the same, and the detector raises 14
flags in the first half hour against 2 243 crossings. After the start-up, on observed data, its flags sit at the
mass jumps three times more often than at other epochs, together with gap re-entries and RAIM attributions at the
same epochs. Whether a flag causes a mass jump or shares its cause (a gap at many receivers at once) is not
separated here: no arm was run.

**Not verified.** Which network state moves the shared part while the solution settles (card IRDU 12.11 left it
open; assigned). The observed-data reading is one day and the holds-off stream only.

## 13. Continued in card ARNC

The fallback in place of the removal (item 1 of section 11's "Named, not run") was pre-registered and measured the same
night. Its pre-registration and its results were written here as sections 13 and 14 and moved, unchanged but for the
section numbers and the references to this card's sections, to card [ARNC](ARNC-USERS-OF-SATELLITES-WITHOUT-BIAS-RECORDS.md)
when this file passed the 60 KB bar.

## 14. 2026-11-14 (night): the jumps as a user sees them (addendum to section 12)

Card IRDU sections 13.2 to 13.5 (branch `irdu/user-integers`, commits 67a3774 and 0e057b9, not merged) traced the
start-up jumps of the simulated streams: they are two quantities that only the code observations pin down,
converging together. One is the level that the phase biases of every satellite share, set at the chain's root
receiver; the other is each satellite's own split between its clock and its phase bias. Re-read here from the same
runs and from the observed-data stream (`stream_user_view.py`, `shared_vs_rx.py`; no run).

**Re-graded, and found as reported.** The 1 542 jumps of `_rap_irdu_q1_nr3` and their 5 min bins; 0 IF-TD flags; 23
wide-lane detections, 21 of them in the first 5 min; with the detector off (`_rap_irdu_q1nt_nr3`,
`_rap_irdu_zwnt_nr3`) the product files are identical byte for byte to their controls; at 15 to 40 min the median
shared change is 18.5 mm and the median own change 12.6 mm against a gate of 47.6 mm; the root clock's formal sigma
follows 0.89 m / sqrt(n) from the tenth epoch on and the receivers' phase biases 22 m / sqrt(n); the arm with the
receiver clock carried (`_rap_irdu_q1rc_nr3`) declares the same 1 542 jumps, the last at 37.0 min; and the users
with the declarations off (`_rap_irdu_q1dt_nr3`; second hour by 15 min, median of the five users' medians): 165,
116, 110, 106 mm with the steps declared against 64, 85, 68, 73 mm without, and 190, 99, 64, 59 against 65, 73, 43,
40 mm with card IRDU's rule for satellites without a record. Not re-graded: the share of the shared series that the
root receiver's own states reproduce (92 to 95 %).

**Two readings added here.**

1. *The shared level is traded between the satellites and the receivers.* At 15 to 40 min the shared change of the
   satellites' phase bias and the median change of the unpinned receivers' phase bias correlate at -0.964 with a
   slope of -0.96 (`_rap_irdu_q1_nr3`), and at -0.960 and -0.96 on the chain arm of the same bed. Each satellite's
   own change of the phase bias follows its own change of the clock: correlation +0.99 on the all-integers run,
   +0.78 on the chain arm, +0.2 on r3c; 0.0 to 0.6 by signal on the observed-data stream.
2. *Most declared jumps are no jumps for a user.* A jump is declared on the phase bias alone. A user's phase takes
   clock less phase bias, and between satellites it loses what every satellite shares. Jumps above 0.25 cycle,
   counted three ways:

   | stream | on the phase bias (as declared) | on clock less phase bias | the same, less the median over satellites |
   | --- | --- | --- | --- |
   | simulated, every link at its true integer | 1 542, last at 37.0 min | 1 277, last at 28.0 min | 536, last at 15.5 min |
   | simulated, the chain arm on the same bed | 1 798, last at 48.0 min | 1 567, last at 39.5 min | 987, last at 35.0 min |
   | simulated, the chain arm on r3c | 2 610 | 2 414 | 1 960 |
   | observed data, holds off, 24 h | 4 343 (hour 1: 2 649; later: 1 694) | 4 104 (2 409; 1 695) | 2 673 (2 033; 640) |

   On the all-integers run two thirds of the declared jumps are none for a user of between-satellite differences,
   and the last real one falls at 15.5 min, not at 37. On observed data without holds the share is 38 % (62 % after
   the first hour). On r3c the re-seats make real steps and most remain.

**Reading.** The recollection of section 12 stands as not confirmed, and the cause is named: the declaration is
made on a record whose movement the clock record largely cancels. Every declared jump makes the user re-float that
satellite's ambiguity, so declaring on the phase bias alone costs the users of the simulated stream a factor of
two to three in their second hour.

**Not verified.** Observed-data users without the declarations (no run). A declaration made on what a user sees,
graded where real steps exist (r3c, and observed data with slips and datum handovers): card IRDU names it and four
other approaches for the author's decision; none is built.

## 15. 2026-11-14 (night): the declaration on what a user sees, built on the simulator and re-graded

By the author's go-ahead of 2026-11-14 ("Yes, assign it") card IRDU's session built the declaration of section 14
as `network.disc_phase_visible` (default off, ledger UNVALIDATED, stream token `discpv=4`; card IRDU section 14,
branch `irdu/user-integers`, commits a024ae3 and 70ec03c, not merged). It is step IRD-7 of card IRD's route. Its
record is card IRDU's; what follows is the re-grade made here and two readings added from the same runs
(`pv_users.py`, `pv_pairs.py`; no run).

**Re-graded, and found as reported.** Key off: no data line of the product file differs from the control's on the
three runs (one legend comment line of the new build does). Key on: the only data line that differs is the header
record (`discpv=4`); every clock, code-bias and phase-bias value is the control's. Declared jumps: 536 at the
producer's run end on the all-integers run, the count of section 14. Between-satellite jumps above twice the gate
with neither satellite declared: 5, 50 and 76 under the old rule (all-integers run, chain arm, r3c), 0 under the
new. The users (float, 15 min bins, median of five): the all-integers run from 0 min with card IRDU's rule 770,
191, 115, 101, 67, 68, 34, 38 mm with the key against 959, 336, 318, 158, 190, 99, 64, 59 with the steps declared
on the phase bias, and equal to the declarations-off arm from 15 and 30 min; the chain arm from 0 min 1 109, 914,
783, 403, 255, 184, 148, 135 against 848, 532, 380, 172, 147, 125, 83, 75; r3c's last two bins 143 and 146 against
118 and 121.

**Two readings added here.**

1. *Between one and two gates the new rule leaves more pair jumps undeclared than the old:* 1 773 against 36 on
   the all-integers run, 3 152 against 971 on the chain arm, 5 280 against 3 899 on r3c (pair-epochs whose
   difference jumps by more than a quarter cycle with neither satellite declared).
2. *Neither rule sees the slow movement.* Inside 15 min windows without a declared jump, a satellite's own
   clock less phase bias (less the median) ranges over 84 mm in the median on the chain arm at 15 to 60 min (p90
   191 mm) and 36 mm in the second hour (p90 67); 55 and 19 mm on the all-integers run; 79 and 39 mm on r3c (p90
   198 and 133).

**Reading of the users' loss** (card IRDU left it untraced; this is a reading, not a test). The loss follows the
user's start, not the hour: on the chain arm, users from 0 and 15 min end worse with the key in every late bin,
and users from 30 min do not (641, 137, 148, 105, 88, 76 against 543, 169, 145, 125, 83, 75 mm). An ambiguity
estimated while the stream still moves by centimetres per quarter hour below the gate keeps that error. The old
rule's flood of declarations forces those ambiguities to be estimated again and so protects such users by
accident; the new rule removes the flood and the protection with it. On the all-integers run the slow movement is
half as large and the users gain.

**Not verified.** That reading (the test: the key's users re-levelled once when the stream has settled); card
IRDU's own candidate (the clock that the user's code takes still moves undeclared); observed data.
