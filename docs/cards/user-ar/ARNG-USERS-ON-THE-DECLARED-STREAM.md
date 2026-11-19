# ARNG: users on the stream that declares its steps and flags settled signals

**STATUS**

- **id:** ARNG
- **topic:** user-ar
- **status:** open
- **summary:** Continuation of card ARNF (that file neared the 60 KB bar). Card ARNF holds the stream read without a user, the user bench against the network's whole numbers, the sweep of the user's fixing settings, the users started after the stream has settled, and the re-grades of the builder's remedy at re-hosts. This card takes the user replays on the builder's stream that declares its steps and flags settled signals: a user that starts with the network and waits for the flag, against the user started late.
- **dates:** 2026-11-15 (opened as the continuation, with the pre-registration of section 2); 2026-11-16 (sections 3 to 8: the results, the stream read, the settled-part test and its results); 2026-11-16 (sections 9 to 12: the replays on the builder's own stream on two days, the flag rules at the user, the untied GPS signal and its two remedies)
- **knobs / posture:** none new here. The builder's keys `network.disc_step_class_pairs` and `network.ring_fix.settled_flags` (both UNVALIDATED on its branch); the user's `network.require_signal_integer_for_ar` (default off) and `estimation.ambiguity_resolution.enable_partial_ar` false.
- **code:** none. Tools of card ARNF (`user_vs_net_ints2.py`, `user_fix_vs_float.py`, `users_gal_only.py`) and `stream_seg_drift.py`, `stream_level_series.py`, `stream_filter_flagged.py`, `users_hourly2.py`, `user_resets.py`; run scripts `users_chain_f11.sh`, `users_chain_f12.sh`, `users_chain_f12c.sh` (copies under `D:\GnssProcessorSupport\presentation\agu26\fixing_control\tools\`). Sections 9 to 12: `stream_cmp_counters.py`, `stream_flag_share.py`, `user_amb_drift.py`, `wl_off_probe.py`, `stream_step_unheld.py`, `derive_u12.py`, `derive_rules.py`, `derive_ext.py`, `derive_ext_ind.py`, `derive_prio.py`; run scripts `users_chain_u12.sh`, `users_chain_rules.sh`, `users_chain_ext.sh`, `chain_ext_ind.sh`, `users_chain_prio.sh`.
- **run dirs:** `D:\GnssProcessorSupport\runs_nom\_rap_f11_n144`, `_rap_o10_n144` (the builder's streams) and `_rap_nw_obs{nA11,nB11,nC10,nD11,nE11}_144_*`, `_rap_nw_{obs,fxb}f11_144_*` (the user replays); outputs under `D:\GnssProcessorSupport\presentation\agu26\fixing_control\grades_arnd\f11\`. Section 8: the streams `_rap_f12_n144`, `_rap_f12c_n144` (the builder) and their settled-part copies `_rap_f12s_n144`, `_rap_f12cs_n144`; user replays `_rap_nw_obs{nA12,nD12,nSDf,nSDx,nSBf,nSBx,nG1f,nG1x,nG2f,nG2x,nB2f,nB2x,nCDf,nCDx,nCBf,nCBx}_144_*`, `_rap_nw_{obs,fxb}f12s_144_*`; outputs under `grades_arnd\f12\`. Sections 9 to 12: the builder's streams `_rap_u12_n144`, `_rap_u12_n145`; copies `_rap_f12{cs,Ls,LCs}_n14x`, `_rap_u12x_n14x`, `_rap_f12Lx_n14x`; user replays `_rap_nw_obs{nU,nC,nL,nM}{Df,Dx,Bf,Bx}_{144,145}_*`, `_rap_nw_obs{nV,nW,nP}{Bf,Bx}_{144,145}_*`, `_rap_nw_{obs,fxb}{u12,u12x,u12p}_{144,145}_*`; outputs under `grades_arnd\{u12,rules,ext,prio}\`.
- **source:** card [ARNF](ARNF-USERS-ON-THE-HELD-STREAM.md), sections 17 and 20; card NRF part 6 sections 47 and 49 on the builder's branch `irdu/user-integers`

**Outcome.** Open; the results of sections 10 and 12 are in card [ARNH](ARNH-USERS-UNDER-THE-FLAG-RULES.md). On the builder's own stream the users equal the emulation on DOY 144 (0.0 mm): fixing on both
systems gives 23, 15, 11 and 10 mm in hours 3 to 6. DOY 145 fails the adoption bars for two traced reasons. A GPS
signal of no held family (L1W, 11 of the 28 receivers, all of which also track L1C) is carried on a level that
drifts; and under rule B every Galileo ambiguity restarts at hour 2.5, after which one receiver holds wrongly.
Where the receiver is processed on the tied GPS pair, fixing on both systems gives 12, 9 and 8 mm in hours 4 to 6
of DOY 145. Two remedies for the untied signal and the flag rules are under test (sections 10 and 12).

> Results are appended TO THIS FILE, below the text.

---

## 1. Scope

Sections of card ARNF are cited by their numbers there; the bench (a user at a network receiver against the
network's whole numbers and against its float sibling) is that of ARNF sections 9 and 12.

## 2. 2026-11-15: users started with the network on the declared stream (pre-registered)

**Why.** ARNF section 17: a user started with the network keeps what the stream's unsettled first hour put into it,
and a user started two hours later does not. A user cannot choose when the network started. The builder's stream now
raises a discontinuity counter at every whole or fraction step of a satellite signal's level and flags the signals
that are tied to the reference receiver through held links only; the user engine resets an ambiguity at a counter
increment and can refuse to fix on an unflagged signal (`network.require_signal_integer_for_ar`).

**Streams** (DOY 144, 55 receivers, 6 h, the adopted set, Galileo L1X/L5X and GPS L1C/L2W held):
`_rap_f11_n144` (the declaration and the flags) and `_rap_o10_n144` (neither). Known limit of `_rap_f11_n144`: a
release does not yet raise a counter (ARNF section 20), so a released level can creep unsignalled.

**User runs**, all started with the network; fixing users with partial fixing off and the flag required:

| label | users | stream | receivers |
| --- | --- | --- | --- |
| `nA11` | float, both systems | declared | 28 network receivers |
| `nB11` | fixing, both systems | declared | 28 network receivers |
| `nC10` | float, both systems | neither | 28 network receivers |
| `nD11` | float, Galileo only | declared | 28 network receivers |
| `nE11` | fixing, Galileo only | declared | 28 network receivers |
| `f11` | float and fixing, both systems | declared | 11 independent receivers of the held Galileo family |

**Bars** (median over the receivers of the hourly 3D median, hours 3 to 6 of the day):

1. The float user of both systems on the declared stream is better than the float user of both systems on the stream
   without declaration (`nA11` against `nC10`) and not worse than the Galileo-only float user (`nA11` against `nD11`).
2. The fixing users (`nB11`, `nE11`): at least 99.5 % of the verified holds right against the float sibling, the wide
   lane never off, and a position in hours 3 to 6 of the day not worse than the fixing user started two hours late
   (ARNF section 17: 97, 28, 14 and 13 mm in its hours 1 to 4).
3. At the independent receivers the fixing user is not worse than the float user in hours 3 to 6.

**Reads.** The float's narrow-lane distance from the network's whole numbers by hour; the share of pair-epochs held;
the resets a user makes per hour at the counter's increments.

## 3. 2026-11-16: results of section 2

162 user replays, every one exit 0 (log `users_f11_run.log` in the output folder of section 2; tools
`user_vs_net_ints2.py`, `user_fix_vs_float.py`, `users_hourly2.py`, `user_resets.py`).

### 3.1 Float users against the network's whole numbers

28 network receivers, Galileo L1X/L5X pairs. Per hour of the day: the share of pair-epochs whose narrow lane equals
the network's, and the mean distance of the narrow lane from it in cycles. The wide lane equals the network's in 99.3
to 100 % of the pair-epochs of every hour of every run.

| run | hour 1 | 2 | 3 | 4 | 5 | 6 |
| --- | --- | --- | --- | --- | --- | --- |
| `nC10` both systems, no declaration | 33.8 %, 0.248 | 63.3, 0.214 | 84.7, 0.171 | 93.4, 0.134 | 94.3, 0.126 | 97.4, 0.102 |
| `nA11` both systems, declared | 45.3 %, 0.240 | 82.9, 0.186 | 93.0, 0.142 | 96.2, 0.099 | 99.0, 0.103 | 99.7, 0.086 |
| `nD11` Galileo only, declared | 39.5 %, 0.234 | 78.5, 0.208 | 95.7, 0.145 | 98.1, 0.108 | 98.7, 0.082 | 99.9, 0.073 |

### 3.2 Positions

Median over the receivers of the hourly 3D median, mm, by hour of the day.

| 28 network receivers | 1 | 2 | 3 | 4 | 5 | 6 |
| --- | --- | --- | --- | --- | --- | --- |
| `nC10` float, both systems, no declaration | 420 | 102 | 60 | 33 | 29 | 26 |
| `nA11` float, both systems, declared | 511 | 91 | 45 | 31 | 25 | 21 |
| `nB11` fixing, both systems, waits for the flag | 511 | 121 | 59 | 35 | 27 | 19 |
| `nD11` float, Galileo only, declared | 671 | 101 | 51 | 27 | 19 | 15 |
| `nE11` fixing, Galileo only, waits for the flag | 671 | 137 | 55 | 36 | 18 | 13 |
| ARNF section 17: float, Galileo only, started 2 h late | | | 97 | 34 | 27 | 16 |
| ARNF section 17: fixing, Galileo only, started 2 h late | | | 97 | 28 | 14 | 13 |

`nA11` against `nC10`, paired per receiver: -3, -29, -17, -9, -5 and -5 mm; 15 receivers better and 7 worse over hours
4 to 6 (a move under 5 mm counts as neither).

| 11 independent receivers, both systems, declared | 1 | 2 | 3 | 4 | 5 | 6 |
| --- | --- | --- | --- | --- | --- | --- |
| `f11` float | 408 | 116 | 68 | 37 | 23 | 14 |
| `f11` fixing, waits for the flag | 408 | 129 | 68 | 37 | 22 | 16 |
| `o2s1je` float, Galileo only, stream without flags (`_rap_o2s1j_n144`) | 493 | 214 | 56 | 36 | 26 | 22 |
| `o2s1jeA` fixing, partial fixing off, Galileo only, stream without flags | 493 | 212 | 84 | 62 | 46 | 24 |

Fixing against float at the independent receivers, paired: 0.0, 0.0, 0.0, -0.2, -0.3 and -0.3 mm; two receivers
worse by more than 5 mm over hours 4 to 6 (CEDU, WUH2), none better.

### 3.3 The fixing users that start with the network and wait for the flag

Galileo L1X/L5X pairs; a held pair is judged against the float sibling where that float lies within 0.20 cycle of a
whole number (ARNF section 12).

| run | held pair-epochs | equal to the network | verified right | wrong | wide lane off |
| --- | --- | --- | --- | --- | --- |
| `nE11` Galileo only | 38 135 | 91.2 % | 32 355 (94.5 %) | 1 874 | 0 |
| `nB11` both systems | 31 118 | 87.8 % | 26 117 (91.8 %) | 2 320 | 0 |

By hour, `nE11`: 92.0, 82.0, 87.7, 95.9, 97.2 and 97.6 % of the verified holds right. The wrong holds sit at 8 of the
28 receivers in `nE11` (MBAR 559, CUSV 429, URUM 423, LCK4 357, SCTB 36, GAMB 35, KRGG 34, YKRO 1) and at 9 in
`nB11` (CUSV 1 291, URUM 448, WARK 199, YKRO 149, LEIJ 79, KRGG 78, LCK4 54, WIND 15, STHL 7). They begin in hour 2
(438 pair-epochs in `nE11`) and last: MBAR E21-E26 is two cycles off from TOW 7530 to 20310.

### 3.4 Resets at the counters' increments

Mean per receiver and hour, 28 network receivers.

| run | system | 1 | 2 | 3 | 4 | 5 | 6 |
| --- | --- | --- | --- | --- | --- | --- | --- |
| `nC10` (the value test) | Galileo | 173 | 27 | 4 | 66 | 4 | 2 |
| | GPS | 214 | 20 | 8 | 9 | 12 | 8 |
| `nA11` (the declaration) | Galileo | 122 | 2 | 3 | 11 | 4 | 2 |
| | GPS | 220 | 23 | 15 | 14 | 6 | 11 |

### 3.5 The bars

1. **First half passes, second half fails by 3 to 6 mm.** The float user of both systems is better on the declared
   stream than on the stream without declaration in every hour from hour 2. Against the Galileo-only float user it is
   better in hour 3 (45 against 51 mm) and worse in hours 4 to 6 (31, 25 and 21 against 27, 19 and 15 mm).
2. **Fails.** 94.5 % (Galileo only) and 91.8 % (both systems) of the verified holds are right, against the bar of
   99.5 %. The wide lane is never off (0 of 38 135 and 0 of 31 118). The Galileo-only position in hours 3 to 6 (55,
   36, 18 and 13 mm) is better than the late-started user's in hour 3 and worse by 8 and 4 mm in hours 4 and 5.
3. **Passes within 1.2 mm.** At the independent receivers the fixing user reads 68, 37, 22 and 16 mm in hours 3 to 6
   against 68, 37, 23 and 14 mm for the float user.

### 3.6 Reading

- **The declaration is a gain for the float user that starts with the network.** Its narrow lanes equal the network's
  in 93.0 % of the pair-epochs of hour 3 against 84.7 % without the declaration, its position gains 17 mm in that
  hour, and it makes fewer resets (the value test declared ionosphere-like steps the user's ionosphere states absorb).
- **The flag does not make the fixing user that starts with the network safe.** The flag speaks for the satellite's
  level in the stream. The user's own position, wet delay, clock and ionosphere states carry the stream's unsettled
  first hour (122 to 220 resets per receiver in hour 1), and the user's formal sigma is too small for its error (ARNF
  section 17), so its acceptance test passes on a biased float from hour 2. The user started two hours late has none
  of this and holds 99.6 % right.
- **At the independent receivers the fixing user no longer loses against float** (on the stream without flags, 62
  and 46 mm against 36 and 26 mm in hours 4 and 5), and it gains nothing either.
- **GPS in the user still costs 3 to 6 mm late in the run** (bar 1), although it helps early. Section 4 reads why.

## 4. 2026-11-16: the stream read without a user: levels that move under constant counters

**Why.** The builder's release increments (card NRF part 6 section 50, stream `_rap_f12_n144`) raise a counter at
every release of the chain. The question for a user is wider: how far does a satellite's level move while its
counters say that nothing happened, whatever the cause.

**Method** (tools `stream_seg_drift.py`, `stream_level_series.py`). The per-signal level of ARNF section 3, less the
constellation's datum (the running sum of the median epoch-to-epoch step over the satellites with both settled flags
up and no increment), in its ionosphere-free part in narrow-lane cycles (10.9 cm for Galileo, 10.7 cm for GPS). A
segment of a satellite is a maximal run of its epochs over which neither signal's counter changes. Hours 1 to 6.
ARNF section 3 read the steps between consecutive epochs and the range over half-hour windows; a slow movement over
hours passes under both.

| stream `_rap_f12_n144` | segments | range p50 | p90 | max | satellite-epochs in a segment over 0.15 cycle |
| --- | --- | --- | --- | --- | --- |
| Galileo L1X/L5X | 86 | 0.069 | 0.178 | 0.416 | 24.8 % |
| Galileo L1X/L5X, the runs with both flags up | 87 | 0.069 | 0.143 | 0.306 | 22.1 % |
| GPS L1C/L2W | 128 | 0.143 | 1.095 | 3.091 | 53.1 % |
| GPS L1C/L2W, the runs with both flags up | 39 | 0.113 | 0.178 | 0.301 | 31.3 % |

The release increments change little in this read: 83 segments become 86 for Galileo and 119 become 128 for GPS
(`_rap_f11_n144` against `_rap_f12_n144`; the products are equal with the counters masked).

1. **An unflagged GPS satellite's level walks.** G31 moves 2.2 and 2.7 narrow-lane cycles within two segments of 2.2
   and 1.4 hours, G18, G07 and G20 1.3 to 1.5 cycles, in steps of a hundredth of a cycle per epoch, far under any
   step tolerance. No counter can say that; only the flag does (with both flags up the p90 is 0.18 cycle). A float
   user that gives such a satellite a constant ambiguity takes the walk into its position: the cost of GPS in bar 1.
2. **A flagged Galileo satellite that hangs on one receiver.** E05 moves 0.42 cycle (4.5 cm) in five hours with no
   increment, with both flags up from 2.5 h. In the run of the other reference receiver (`_rap_o10_n144r`) E05 stays
   within 0.08 cycle; the difference of E05's level between the two runs moves by 0.32 cycle (over the 30
   satellites the difference of the two runs moves by 0.039 cycle at p50 and 0.126 at p90, a declared whole step of
   E18 aside). Traced in `_rap_o10_n144` (the same products as `_rap_f12_n144`): E05's tree edge is KOKV00USA from
   its first sighting to TOW 13320; its other links (GAMB00PYF, WARK00NZL, SCTB00ATA, CAS100ATA, then GCGO00USA) read
   one common ionosphere-free fraction of +0.2 to +0.43 cycle for 2.5 hours and are refused; GCGO00USA and GAMB00PYF
   are decided at fractions of +0.18 and +0.20, and the level then moves on toward them. In the other run the same
   links come down to 0.05 to 0.11 cycle before they are decided, from the second half hour on. A satellite reached
   through one link carries that link's local error, here 0.3 cycle (3 cm) at KOKV00USA, not traced further. Nothing
   in the stream says so: the counters see no step, and the flag goes up when the first other link is held, with the
   level still 0.2 cycle from where it ends.
3. **The typical movement is small.** 0.07 cycle (7 mm) in five hours is the size of the difference between two
   clock solutions and no evidence of a defect.

Sent to the builder with the request for a population read and for a flag rule that asks for agreement between a
satellite's links. The choice between a consensus datum and a consensus re-seat stays with the author; E05 is a
case for it.

## 5. 2026-11-16: users on the settled part of the stream, and late starts on both systems (pre-registered)

**Why.** Section 3: the flag protects a satellite, not the user's own state, and section 4: an unflagged satellite's
level walks. The user that starts two hours late has neither problem, but a user cannot know when the network
started. The flags can tell it. A user that uses a satellite only while both of its family signals are flagged starts
by itself when the stream is ready and never carries a walking level. This is tested with no change to the user
engine: a copy of the stream that leaves a satellite out of every epoch at which its family signals (Galileo L1X and
L5X, GPS L1C and L2W) are not both flagged, and that steps the satellite's phase counters at each return
(`stream_filter_flagged.py`; stream `_rap_f12s_n144` from `_rap_f12_n144`). It keeps no satellite in the first half
hour, 10.7 of 30 Galileo satellites in the second, 26 to 29 from hour 1; GPS 7 of 32 in the third half hour and 20
to 26 from then on.

**User runs** (28 network receivers unless said; fixing users with partial fixing off and the flag required):

| label | users | stream | start |
| --- | --- | --- | --- |
| `nA12`, `nD12` | float; both systems, Galileo only | `_rap_f12_n144` | with the network |
| `nSDf`, `nSDx` | float, fixing; Galileo only | `_rap_f12s_n144` (settled part) | with the network |
| `nSBf`, `nSBx` | float, fixing; both systems | `_rap_f12s_n144` | with the network |
| `nG1f`, `nG1x` | float, fixing; Galileo only | `_rap_f12_n144` | 1 h late |
| `nG2f`, `nG2x` | float, fixing; Galileo only | `_rap_f12_n144` | 2 h late |
| `nB2f`, `nB2x` | float, fixing; both systems | `_rap_f12_n144` | 2 h late |
| `f12s` | float and fixing; both systems; the 11 independent receivers | `_rap_f12s_n144` | with the network |

**Bars** (medians over the receivers of the hourly 3D median; hours of the day).

1. **The release increments at the user:** `nA12` against `nA11` and `nD12` against `nD11`, paired per receiver, not
   worse by more than 2 mm in any of hours 3 to 6.
2. **Every fixing user** (`nSDx`, `nSBx`, `nG1x`, `nG2x`, `nB2x`): at least 99.5 % of the verified holds right
   against its float sibling, and the wide lane never off.
3. **The settled part against the late start:** the user that starts with the network on the settled part is not
   worse than the user of the same systems started 2 h late by more than 3 mm in hours 4 to 6 (`nSDf` against `nG2f`,
   `nSDx` against `nG2x`, `nSBf` against `nB2f`, `nSBx` against `nB2x`).
4. **GPS beside Galileo at the user:** on the settled part and on the late start, the user of both systems is not
   worse than the Galileo-only user in hours 4 to 6 (float and fixing).
5. **The independent receivers on the settled part:** fixing not worse than float by more than 2 mm in hours 4 to 6,
   and float not worse than section 3.2's (37, 23 and 14 mm).

**Reads.** The GPS pairs of the fixing users of both systems against their float siblings (GPS L1C/L2W, never
graded against the network's whole numbers before); the held share by hour.

### 5.1 2026-11-16: correction before any graded run: how the stream copy treats an unsettled satellite

The copy described above (records left out) is not an emulation. The user engine uses a satellite that has no
correction records on the a priori products alone: in the smoke run at GOPE00CZE eight Galileo satellites entered
the solution at TOW 0 with empty correction fields, and the corrections then arrived under a running ambiguity. That
copy and its smoke run are discarded.

The copy used for every graded run of this section keeps every record. A satellite whose two family signals are not
both flagged steps the phase counters of all its signals at every epoch, and once more at the epoch at which both
flags are up (13 824 increments added to the 389 271 lines of `_rap_f12_n144`; `stream_filter_flagged.py`, mode
`steps`). The user engine restarts an ambiguity at every increment, so the phase of an unsettled satellite carries
nothing from one epoch to the next, while its code and its corrections stay in use. This is also a rule the emitter
could apply by itself: the level of an unsettled signal is a float that moves, and a counter that stays constant
tells a user that it is continuous. The counts of flagged satellites in the text above are unchanged.

Smoke runs at GOPE00CZE on this copy (median 3D by hour, mm): the Galileo-only fixing user 909, 15, 6, 8, 6 and 5
(`nE11` of section 3: 887, 139, 50, 25, 11 and 12); the fixing user of both systems 624, 44, 7, 3, 1 and 4 (`nB11`:
768, 86, 24, 15, 17 and 19). Its formal 3D sigma covers its error at the half-hour marks (0.93 m for 1.04 m at TOW
1800, 0.152 m for 0.094 m at TOW 3600).

## 6. 2026-11-16: the builder's sections 50 to 51 re-checked (card NRF part 6)

- **The release increments** (commit 9309bea on the builder's branch): the code read here (the chain's released keys
  of an epoch are marked in the emitter, and the first emission that carries a marked key with a baseline steps its
  counter). `_rap_f11_n144` against `_rap_f12_n144` in the read of section 4: the levels are equal, and 83
  constant-counter segments become 86 for Galileo L1X/L5X and 119 become 128 for GPS L1C/L2W. Accepted.
- **The cap of 12 on DOY 145**, two reference receivers (`_rap_q12_n145` against `_rap_q12_n145r`), read here with
  the walk-independent audit (`o2_audit_mode.py`): Galileo L1X 0 of 113 996 arc-epochs and L5X 0 of 114 189; GPS L1C
  0 of 104 600 and L2W 0 of 115 396. The builder's note of the cap now records DOY 145 as verified.
- **The GPS held share** (section 51) accepted as a read: the wide-lane bound never refuses, GPS is refused by the
  fraction of N1, the window and the odds, and GPS III links are held less at some receivers.
- **Sent to the builder:** the user results of section 3, the finding of section 4 with the request for a population
  read and for a flag rule that asks for agreement between a satellite's links, and the test of section 5. No build
  is ordered before section 5 is graded.

## 7. 2026-11-16: the builder's rule C at the user (pre-registered while the runs of section 5 finish)

**Why.** The builder's read of section 4's finding (card NRF part 7 section 53.3) proposes a stricter flag, rule C:
the stream's flag (rule B), and at least two links of the satellite held or pinned, and no unheld chord with a
converged float more than 0.10 cycle from a whole number. It is stamped into a copy of the stream
(`_rap_f12c_n144`; the flagged share falls by 2 to 12 points). The settled part of that copy, made as in section
5.1, is `_rap_f12cs_n144`.

**User runs** (28 network receivers, started with the network, settled part of the rule C copy): `nCDf`, `nCDx`
(float, fixing; Galileo only) and `nCBf`, `nCBx` (float, fixing; both systems).

**Bars.** The fixing users hold at least 99.5 % of their verified holds right with the wide lane never off; against
the users of the rule B copy (`nSDf`, `nSDx`, `nSBf`, `nSBx`) the position is not worse by more than 2 mm in hours 4
to 6 (median over the receivers). **Read:** the wrong holds of the rule B users that rule C removes or keeps.

## 8. 2026-11-16: results of sections 5 and 7

467 user replays in the two batch logs (355 of section 5, 112 of section 7), every one exit 0, plus the smoke runs
of section 5.1. DOY 144, six hours, network stream `_rap_f12_n144` and its copies. Outputs: `grades_arnd\f12\`.
Positions by `users_hourly2.py` (median over a named receiver set of the hourly 3D median; paired differences per
receiver), resets by `user_resets.py`.

### 8.1 Positions

28 network receivers; median over the receivers of the hourly 3D median (mm); hours of the day. `nA11`, `nD11`,
`nB11` and `nE11` are section 3's users on `_rap_f11_n144`.

| label | users | 1 | 2 | 3 | 4 | 5 | 6 |
| --- | --- | --- | --- | --- | --- | --- | --- |
| `nA11` | float, both systems, f11 | 511 | 91 | 45.3 | 30.5 | 24.8 | 20.6 |
| `nA12` | float, both systems, f12 | 511 | 97 | 44.9 | 29.4 | 22.1 | 17.5 |
| `nD11` | float, Galileo only, f11 | 671 | 101 | 50.5 | 27.2 | 19.2 | 14.7 |
| `nD12` | float, Galileo only, f12 | 671 | 101 | 50.5 | 27.2 | 19.2 | 16.4 |
| `nB11` | fixing on the flag, both systems, f11 | 511 | 121 | 59.2 | 35.3 | 26.9 | 18.5 |
| `nE11` | fixing on the flag, Galileo only, f11 | 671 | 137 | 54.6 | 36.0 | 18.2 | 13.3 |
| `nSDf` | float, Galileo only, settled part | 560 | 70 | 27.4 | 23.6 | 16.9 | 13.8 |
| `nSDx` | fixing, Galileo only, settled part | 560 | 70 | 24.9 | 19.2 | 10.6 | 10.4 |
| `nSBf` | float, both systems, settled part | 622 | 80 | 26.0 | 18.4 | 13.5 | 10.2 |
| `nSBx` | fixing, both systems, settled part | 622 | 80 | 22.4 | 14.8 | 11.6 | 9.8 |
| `nG1f` | float, Galileo only, 1 h late |  | 85 | 24.2 | 22.9 | 16.2 | 13.6 |
| `nG1x` | fixing, Galileo only, 1 h late |  | 80 | 22.2 | 13.4 | 12.2 | 11.5 |
| `nG2f` | float, Galileo only, 2 h late |  |  | 78.4 | 35.1 | 21.1 | 16.6 |
| `nG2x` | fixing, Galileo only, 2 h late |  |  | 78.4 | 22.0 | 12.2 | 11.7 |
| `nB2f` | float, both systems, 2 h late |  |  | 137.6 | 62.2 | 30.2 | 23.9 |
| `nB2x` | fixing, both systems, 2 h late |  |  | 130.4 | 62.2 | 27.0 | 23.9 |
| `nCDf` | float, Galileo only, settled part of rule C | 560 | 75 | 24.1 | 22.2 | 16.2 | 13.2 |
| `nCDx` | fixing, Galileo only, settled part of rule C | 560 | 75 | 19.2 | 19.1 | 11.5 | 11.0 |
| `nCBf` | float, both systems, settled part of rule C | 631 | 91 | 25.6 | 18.6 | 15.8 | 13.6 |
| `nCBx` | fixing, both systems, settled part of rule C | 631 | 91 | 21.7 | 14.1 | 12.6 | 11.1 |

The 11 independent receivers (both systems):

| users | 1 | 2 | 3 | 4 | 5 | 6 |
| --- | --- | --- | --- | --- | --- | --- |
| `f11` float (section 3.2) | 408 | 116 | 67.6 | 36.8 | 23.4 | 14.3 |
| `f11` fixing (section 3.2) | 408 | 129 | 68.3 | 36.8 | 22.4 | 15.5 |
| `f12s` float, settled part | 666 | 139 | 50.9 | 24.9 | 16.4 | 17.4 |
| `f12s` fixing, settled part | 666 | 139 | 40.4 | 19.7 | 18.6 | 16.4 |

Per independent receiver, mean of hours 4 to 6 (mm), float on f11, float on the settled part, fixing on the settled
part: ARHT 46.4, 25.3, 24.9; ASCG 23.2, 12.9, 12.3; BADG 17.8, 11.2, 10.9; CEDU 33.5, 22.0, 20.4; CHTI 20.8, 9.0,
3.8; CIBG 24.5, 28.8, 22.8; QUIN 43.9, 35.6, 26.0; STPM 12.3, 13.7, 17.0; UTQI 46.6, 31.9, 28.8; WTZZ 25.8, 20.7,
9.3; WUH2 15.3, 21.1, 20.5. The float user on the settled part is better at 8 of 11 and worse at CIBG, STPM and
WUH2; in hour 6 alone it is worse at BADG (18.0 against 6.6), STPM (14.2 against 8.6) and WUH2 (29.1 against 18.8).

### 8.2 Float users against the network's whole numbers

Galileo L1X/L5X pairs, narrow lane equal to the network's (%), by hour of the day:

| label | 1 | 2 | 3 | 4 | 5 | 6 |
| --- | --- | --- | --- | --- | --- | --- |
| `nD12` | 39.5 | 78.5 | 95.7 | 97.9 | 98.5 | 99.9 |
| `nA12` | 44.5 | 81.6 | 92.0 | 96.1 | 98.8 | 99.9 |
| `nSDf` | 35.3 | 83.2 | 96.4 | 97.4 | 99.0 | 100.0 |
| `nSBf` | 39.6 | 82.5 | 93.4 | 97.9 | 99.1 | 99.9 |
| `nG1f` |  | 76.3 | 93.7 | 94.5 | 97.2 | 99.8 |
| `nG2f` |  |  | 75.0 | 90.0 | 94.0 | 98.4 |
| `nB2f` |  |  | 62.8 | 82.6 | 89.2 | 91.9 |
| `nCDf` | 33.4 | 79.9 | 95.5 | 97.7 | 99.6 | 100.0 |
| `nCBf` | 36.4 | 76.6 | 91.7 | 96.2 | 98.2 | 99.0 |

GPS L1C/L2W pairs (11 of the 28 receivers have them): `nA12` 51.2, 91.8, 94.4, 91.5, 99.7 and 100.0; `nSBf` 42.3,
85.6, 98.0, 100.0, 100.0 and 100.0; `nB2f` (hours 3 to 6) 81.4, 92.2, 100.0 and 100.0. The float wide lane equals
the network's in 99.3 to 100.0 % of the pair-epochs of every hour and label.

### 8.3 The fixing users

Galileo L1X/L5X pairs over the six hours. A held pair-epoch is verified where the float sibling is within 0.20 cycle
of a whole number. The count "wrong by the float" of section 5 is split in two kinds: (a) the fixing user differs
from the network and the float sibling equals the network, a wrong hold; (b) the fixing user equals the network and
the float sibling sits at another whole number.

| label | held pair-epochs | equal to the network | verified right | kind (a) | kind (b) | wide lane off |
| --- | --- | --- | --- | --- | --- | --- |
| `nSDx` | 46 416 | 99.5 % | 43 762 | 48 (KRGG) | 0 | 0 |
| `nSBx` | 44 601 | 99.5 % | 40 816 | 0 | 51 (SVTL 32, VOIM 14, POTS 5) | 0 |
| `nG1x` | 45 815 | 97.7 % | 42 671 | 52 (KRGG) | 0 | 0 |
| `nG2x` | 33 668 | 97.3 % | 30 164 | 0 | 0 | 0 |
| `nB2x` | 20 056 | 95.2 % | 17 644 | 31 (STHL) | 0 | 0 |
| `nCDx` | 39 627 | 99.5 % | 36 801 | 98 (KRGG) | 0 | 0 |
| `nCBx` | 37 766 | 100.0 % | 34 774 | 0 | 211 (VOIM 106, POTS 83, SVTL 22) | 0 |

Kind (b) is the float sibling's error, not the fixing user's. In the hours of each case the fixing user's position
is the better one: VOIM hour 5, 38.3 against 61.6 mm (`nSBx`, `nSBf`) and 36.2 against 85.0 mm (`nCBx`, `nCBf`);
POTS hour 3, 9.4 against 55.0 mm and 14.2 against 71.8 mm; SVTL hour 2, 23.1 against 40.5 mm and 25.7 against
67.4 mm. The float sibling of both systems is therefore not a sound arbiter everywhere, and the split is kept.

GPS L1C/L2W pairs, graded against the network's whole numbers for the first time: `nSBx` 10 receivers, 9 555 held
pair-epochs, 100.0 % equal to the network, 6 669 verified right, none wrong; `nB2x` 11 receivers, 6 686 held, 100.0 %
equal, 5 730 verified right, none wrong; `nCBx` 11 receivers, 9 789 held, 100.0 % equal, 7 135 verified right, none
wrong. The wide lane is never off.

Held share of the Galileo pair-epochs by hour of the day (held pair-epochs over the float sibling's pair-epochs;
hours 2 to 6, %): `nSDx` 18, 59, 73, 81, 82; `nSBx` 17, 58, 68, 77, 81; `nG2x` (hours 3 to 6) 13, 57, 75, 81; `nB2x`
(hours 3 to 6) 5, 27, 44, 60; `nCDx` 11, 51, 58, 70, 78; `nCBx` 13, 42, 57, 68, 76.

### 8.4 The wrong holds at KRGG00ATF

The 48 pair-epochs of `nSDx` are E27 against E29 (TOW 17760 to 18270) and E12 against E27 (TOW 19080 to 19950):
the user holds E27 one cycle below the network. The trace of the two users at this receiver:

- The float user's position error is 132 mm at TOW 10800, 101 mm at TOW 13200, 70 mm at TOW 14400, 50 mm at TOW
  15600, 27 mm at TOW 16800 and 16 mm at TOW 18000: this receiver's own solution converges late (few of the
  satellites it sees are settled early).
- E27 enters at TOW 14220 at 15 degrees. The fixing user's first holds are at TOW 15000 (two pairs, ratio 118) and
  at TOW 15660 (three pairs, ratio 111), the second with E27 against E29 at 121 cycles of L1X (wide lane -149).
- The float sibling's value of that pair is 121.24 at TOW 15060, 121.43 at TOW 16500, 121.72 at TOW 17220 and 121.87
  at TOW 17940, with L5X moving with it (270.26 to 270.88) and the wide lane constant. Its other pairs move in the
  same hour (E30 against E29 from 75.88 to 76.56, E12 against E29 from 110.91 to 110.56). The network holds 122.
- E27's counter does not change in that time, and its level in the stream is steady within 0.043 cycle.

The hold was decided while the user's own float solution was still 5 to 7 cm from its end value, and the ratio did
not show it. The fixing user's position at this receiver is 50.6 mm in hour 5 against 39.8 mm for float. The cause is
on the user's side: the rule C user has the same receiver wrong on other pairs (98 pair-epochs on pairs with E07,
from TOW 18720 to the end of the run). One more case of kind (a) is at STHL in `nB2x` (E12 against E19, 31
pair-epochs; not traced). A settled flag cannot prevent this. Candidates, not built: a condition on the stability of
the user's own float value of a pair before its first hold, and a release of a hold that its own residuals
contradict.

### 8.5 Resets at the user

Per receiver and hour of the day, the 28 network receivers: `nA12` Galileo 122, 2, 3, 12, 4, 2 and GPS 221, 24, 19,
14, 7, 12; `nSBf` Galileo 1463, 91, 27, 27, 25, 2 and GPS 2110, 1081, 677, 492, 351, 327; `nCBf` Galileo 1539, 251,
185, 154, 164, 92 and GPS 2110, 1115, 799, 546, 514, 422. On the settled part a GPS satellite that never settles is
restarted at every epoch to the end of the run. Rule C keeps restarting Galileo satellites through hour 6.

### 8.6 The bars of section 5

1. **The release increments at the user: PASS.** `nA12` against `nA11`, paired, 0.0, 0.0, -0.6 and -0.4 mm in hours
   3 to 6 (4 receivers better, 3 worse); `nD12` against `nD11` 0.0 mm in every hour (3 worse, none better; the
   median of hour 6 is 16.4 against 14.7 mm).
2. **Every fixing user: PASS** by the count of section 5: verified holds right 99.89 % (`nSDx`), 99.88 % (`nSBx`),
   99.88 % (`nG1x`), 100.0 % (`nG2x`) and 99.82 % (`nB2x`), the wide lane never off. By the split of section 8.3 the
   wrong holds are 48, 0, 52, 0 and 31.
3. **The settled part against the late start: PASS.** Differences of the medians in hours 4 to 6 (mm): `nSDf` less
   `nG2f` -11.5, -4.2, -2.8; `nSDx` less `nG2x` -2.8, -1.6, -1.3; `nSBf` less `nB2f` -43.8, -16.7, -13.7; `nSBx`
   less `nB2x` -47.4, -15.4, -14.1.
4. **GPS beside Galileo at the user.** On the settled part the float user PASSES (18.4, 13.5 and 10.2 mm against
   23.6, 16.9 and 13.8). The fixing user is better in hours 4 and 6 and 1.0 mm above in hour 5 (14.8, 11.6 and 9.8
   against 19.2, 10.6 and 10.4; paired -1.2, -0.6 and -1.0 mm, 7 receivers better and 5 worse): not met by the letter
   in one hour. On the late start the bar **FAILS**: float 62.2, 30.2 and 23.9 mm against 35.1, 21.1 and 16.6;
   fixing 62.2, 27.0 and 23.9 against 22.0, 12.2 and 11.7 (paired +20.9, +10.3, +5.6 and +25.6, +10.8, +11.7 mm; 16
   and 19 of the 28 receivers worse).
5. **The independent receivers on the settled part: not met by the letter in two cells.** Fixing against float:
   19.7, 18.6 and 16.4 mm against 24.9, 16.4 and 17.4, so hour 5 is 2.2 mm above for a bound of 2 mm (paired -0.8,
   -0.9 and -1.5 mm; 4 receivers better, none worse). Float against section 3.2's: 24.9, 16.4 and 17.4 mm against
   36.8, 23.4 and 14.3, so hour 6 is 3.1 mm above (paired -5.4, -4.6 and +0.1 mm; 8 better, 1 worse).

### 8.7 The bars of section 7 (rule C at the user)

- **Holds.** `nCDx` 99.73 % of the verified holds right by the count of section 5 (98 wrong, all at KRGG): passes.
  `nCBx` 99.40 %: below the 99.5 % of the bar by that count, with all 211 of kind (b) and the fixing user equal to
  the network in 100.0 % of its held pair-epochs. The wide lane is never off.
- **Position against the rule B users**, differences of the medians in hours 4 to 6 (mm): `nCDf` -1.4, -0.7, -0.6;
  `nCDx` -0.1, +0.9, +0.6; `nCBx` -0.7, +1.0, +1.3; `nCBf` +0.2, +2.3, +3.4: the float user of both systems
  **FAILS** in hours 5 and 6.
- **Read.** Rule C removes none of the wrong holds (KRGG: 98 pair-epochs against 48) and holds 15 % fewer
  pair-epochs (39 627 against 46 416; 37 766 against 44 601).

### 8.8 Reading and decisions

**The settled-part rule works at the user.** A user that carries no phase of an unsettled satellite starts with the
network and reaches, with fixing on both systems, 22.4, 14.8, 11.6 and 9.8 mm in hours 3 to 6, against 59.2, 35.3,
26.9 and 18.5 mm for the user that only waits for the flag (`nB11`), and it is better than either user started two
hours late in each of hours 3 to 6. The user needs no knowledge of when the network started.

**A late start is no remedy on both systems.** On the stream as it is, the user started two hours late carries the
phase of GPS satellites whose level walks (section 4): 62, 27 to 30 and 24 mm in hours 4 to 6, 6 to 26 mm worse than
the same user on Galileo alone, and its float narrow lanes reach only 92 % equal to the network in hour 6. On the
settled part GPS beside Galileo helps the float user from hour 3 on and the fixing user in hours 3, 4 and 6 (in
hours 1 and 2 the user of both systems is the worse one: 622 and 80 mm against 560 and 70 mm).

**Rule C is not adopted at the user.** It holds less, restarts more and leaves the position equal within 1.3 mm for
the fixing users and 2 to 3 mm worse for the float user of both systems. The flags of the emitter rule stay on rule B.
The drift of a flagged satellite that hangs on one receiver (section 4) is the case for the consensus re-seat, which
is not built and is not decided here.

**Decisions taken on this evidence (for later review).**

1. The step declaration with release increments (`network.disc_step_class_pairs`) is to be adopted on the builder's
   branch in the chain posture with fixing: float users gain against the stream without declaration (section 3.2)
   and the release increments are neutral at the user (bar 1). Note for the ledger row: done 2026-11-16; verified by
   the user replays of DOY 144 (one day, 28 network and 11 independent receivers); to do: DOY 145 and further days,
   receivers that track Galileo L1C/L5Q.
2. The emitter rule (an unsettled signal of a held family steps its counter at every emission) stays ordered of the
   builder, default off, on rule B flags. The flags and that rule are judged for adoption together, after the replays
   of this section are repeated on the builder's own stream on DOY 144 and DOY 145.
3. Rule C stays a stamped copy for comparison; it is not built into the emitter.

**Open.** The wrong holds of a user whose own solution has not converged (section 8.4); the independent receivers
that are worse in hour 6 on the settled part (BADG, STPM, WUH2); the float user of both systems at VOIM, POTS and
SVTL; user replays on DOY 145 (the run tools name the day in the folder names).

## 9. 2026-11-16: the replays repeated on the builder's own stream, both days (pre-registered)

**Why.** Section 8's users ran on a copy of the stream made by a script. The builder's emitter rule
(`network.ring_fix.unsettled_steps`, exe nrf24, card NRF part 7 section 54.1) is now built. Read here with
`stream_cmp_counters.py`: `_rap_u12_n144` against `_rap_f12_n144`, 389 271 lines, none differing with the phase
counters masked; the counter changes on the family signals (Galileo L1X and L5X, GPS L1C and L2W) are 26 785 in the
build and 26 785 in the copy `_rap_f12s_n144`, all in common; `_rap_u12_n145` against `_rap_f12_n145`, 391 680 lines,
none differing, 38 410 family counter changes (1 844 without the key). The build leaves the other signals of an
unsettled satellite alone (Galileo L1C and L5Q, GPS L1W), where the copy stepped them (16 385 increments). 17 of the
28 network receivers have no GPS L1C/L2W pair, so the difference can matter for a user of both systems.

**User runs** (started with the network; the settings of section 5's users, only the stream changed): `nUDf`, `nUDx`
(float, fixing; Galileo only) and `nUBf`, `nUBx` (float, fixing; both systems) at the 28 network receivers, and
`u12` (float and fixing; both systems) at the 11 independent receivers; DOY 144 on `_rap_u12_n144` and DOY 145 on
`_rap_u12_n145`. The DOY 145 configurations are the period's DOY 145 user configurations with the edits of the DOY
144 labels applied (`derive_u12.py`).

**Bars** (medians over the receivers of the hourly 3D median; hours of the day).

1. **DOY 144, the build against the copy:** each of `nUDf`, `nUDx`, `nUBf` and `nUBx` not worse than `nSDf`, `nSDx`,
   `nSBf` and `nSBx` by more than 2 mm in any of hours 3 to 6, and the independent users not worse than `f12s` by
   more than 2 mm in any of hours 4 to 6.
2. **Both days, every fixing user:** the wrong holds of kind (a) of section 8.3 are at most 0.5 % of the verified
   holds, and the wide lane is never off.
3. **DOY 145, the level reached:** the fixing user of both systems at most 15 mm in hours 5 and 6, and the users of
   both systems not worse than the Galileo-only users by more than 2 mm in any of hours 4 to 6 (float and fixing).

**Reads.** The users of both systems split by their GPS signals (receivers with a GPS L1C/L2W pair against the
others); the resets per receiver and hour; hours 1 to 3 of DOY 145.

**What follows.** If the bars pass, `network.ring_fix.settled_flags` and `network.ring_fix.unsettled_steps` are
adopted together on the builder's branch in the chain posture with fixing (the three-part note), with the settings
of the fixing users (partial fixing off, the flag required). If bar 1 fails at the receivers without a GPS L1C/L2W
pair, the rule is extended to every phase signal of a satellite whose held family is unsettled, and run again.

## 10. 2026-11-16: the flag rule at the user on both days: rules B, C, L and L with the agreement test (pre-registered)

**Why.** The first three labels of section 9 on DOY 145 (the float and fixing users of Galileo alone and the float
user of both systems; the others still run) show two things that DOY 144 did not. Rule B flags no Galileo satellite
for the first two hours of that day (the tree's datum receiver ties nothing until hour 2.5; read here with
`stream_flag_share.py`: 0, 0, 0, 0, 97 and 98 % by half hour), so the users carry no Galileo phase before hour 3
(453 mm in hour 2, 60 mm in hour 3). And the float user of both systems is 5.8 mm above the Galileo-only one in hour
5 (medians 24.8 against 19.0 mm). The builder's reads (card NRF part 7 sections 53.3 and 58) give three other flag
rules as stamped copies of the stream: rule C (rule B with the agreement test), rule L (the largest set of satellites
tied to each other by held links, whatever receiver roots it) and rule L with the agreement test. Rule C was graded
at the user on DOY 144 only (section 8.7), and rule L not at all. The builder is ordered to build rule L; these runs
grade the rules at the user before that build is done.

**Streams.** Settled-part copies, made as in section 5.1, of the builder's stamped copies: `_rap_f12cs_n145` (rule
C), `_rap_f12Ls_n144` and `_rap_f12Ls_n145` (rule L), `_rap_f12LCs_n144` and `_rap_f12LCs_n145` (rule L with the
agreement test). Rule B is section 9's `_rap_u12_n144` and `_rap_u12_n145`; rule C on DOY 144 is section 7's.

**User runs** (the 28 network receivers, started with the network, the settings of section 9's users): `nCDf`,
`nCDx`, `nCBf`, `nCBx` (rule C, DOY 145); `nLDf`, `nLDx`, `nLBf`, `nLBx` (rule L, both days); `nMDf`, `nMDx`, `nMBf`,
`nMBx` (rule L with the agreement test, both days).

**Bars** (against the rule B users `nU*` of the same day; medians over the receivers of the hourly 3D median).

1. **The early gain of rule L:** on DOY 145 the rule L fixing users (Galileo only and both systems) are better than
   the rule B users by at least 20 % in hours 2 and 3; on DOY 144 they are not worse by more than 2 mm in hours 2
   and 3.
2. **No loss later:** in hours 4 to 6 no label of rules C, L or L with the agreement test is worse than its rule B
   label by more than 2 mm, on either day.
3. **Holds:** every fixing label has wrong holds of kind (a) on at most 0.5 % of its verified holds, and the wide
   lane never off.
4. **The agreement test at the user:** a gain of at least 2 mm in any of hours 4 to 6 for a user of both systems
   (rule C against rule B on DOY 145, rule L with the test against rule L on both days), with no label worse by more
   than 2 mm, reopens section 8.8's decision on rule C.

**Read.** GPS beside Galileo under each rule (the user of both systems against the Galileo-only user, hours 4 to 6).

## 11. 2026-11-16: results of section 9, and what fails on DOY 145

**Runs.** 266 user replays (133 per day), all exit 0, engine `gnss_processor_rscb3.exe`; grades under
`grades_arnd\u12\`. Positions are medians over the receivers of the hourly 3D median, hours of the day, in mm.

### 11.1 Positions

| users | day | h1 | h2 | h3 | h4 | h5 | h6 |
|---|---|---|---|---|---|---|---|
| `nUDf` float, Galileo only | 144 | 559.7 | 70.4 | 27.4 | 23.6 | 16.9 | 13.8 |
| `nUDx` fixing, Galileo only | 144 | 559.7 | 70.4 | 24.9 | 19.2 | 10.6 | 10.4 |
| `nUBf` float, both systems | 144 | 622.0 | 79.2 | 26.0 | 18.4 | 13.2 | 10.2 |
| `nUBx` fixing, both systems | 144 | 622.0 | 79.2 | 23.1 | 14.8 | 10.8 | 9.7 |
| `u12` float, independent | 144 | 685.2 | 137.4 | 53.2 | 22.5 | 16.4 | 17.4 |
| `u12` fixing, independent | 144 | 685.2 | 137.4 | 40.4 | 19.7 | 18.6 | 16.4 |
| `nUDf` float, Galileo only | 145 | 413.6 | 452.9 | 62.3 | 24.8 | 19.0 | 13.3 |
| `nUDx` fixing, Galileo only | 145 | 413.6 | 452.9 | 60.0 | 19.9 | 14.4 | 12.2 |
| `nUBf` float, both systems | 145 | 465.4 | 410.5 | 57.5 | 25.0 | 24.8 | 15.2 |
| `nUBx` fixing, both systems | 145 | 465.4 | 410.5 | 55.4 | 24.6 | 21.9 | 16.6 |
| `u12` float, independent | 145 | 632.2 | 657.3 | 70.2 | 33.2 | 18.2 | 14.6 |
| `u12` fixing, independent | 145 | 632.2 | 657.3 | 73.2 | 33.7 | 21.1 | 13.3 |

DOY 144 against the copy of section 8: the paired difference is 0.0 mm in every hour for the four network labels and
the two independent ones (medians within 0.8 mm), no receiver moved by 5 mm.

### 11.2 Holds of the fixing users

| label, day | held pair-epochs | equal to the network | verified right | wrong | wide lane off |
|---|---|---|---|---|---|
| `nUDx` 144 | 46 416 | 99.5 % | 43 762 | 48, kind (a), KRGG00ATF as in 8.4 | 0 |
| `nUBx` 144 | 44 596 | 99.5 % | 40 774 | 127, all kind (b): SVTL 112, VOIM 14, POTS 1 | 0 |
| `nUBx` 144, GPS L1C/L2W | 9 555 | 100 % | 6 669 | 0 | 0 |
| `nUDx` 145 | 24 403 | 95.0 % | 21 588 | 893, kind (a), all at URUM00CHN | 185, BOGT00COL, satellite E18 |
| `nUBx` 145 | 23 039 | 90.6 % | 19 337 | 915: LEIJ 447, POTS 280, SVTL 122 (the fixing user 1 to 10 cycles from the network's value while its float sibling moves to it); ULAB 66, kind (b) | 0 |
| `nUBx` 145, GPS L1C/L2W | 4 226 | 100 % | 2 890 | 0 | 0 |

### 11.3 The bars of section 9

1. **PASS.** The build equals the copy on DOY 144.
2. **DOY 144 PASS** (0.11 % of the verified holds of `nUDx`, none of `nUBx`, the wide lane never off). **DOY 145
   FAIL:** 4.0 % of the verified holds of `nUDx` (one receiver), 4.2 % of those of `nUBx` (three receivers), and the
   wide lane off on 185 pair-epochs.
3. **FAIL.** The fixing user of both systems reaches 21.9 and 16.6 mm in hours 5 and 6 (bound 15); the users of
   both systems are above the Galileo-only users by 5.8 mm (float, hour 5) and by 4.7, 7.5 and 4.4 mm (fixing, hours
   4 to 6).

`network.ring_fix.settled_flags` and `network.ring_fix.unsettled_steps` are not adopted on these runs.

### 11.4 Reads

**Resets** per receiver and hour (float users of both systems): DOY 144 Galileo 1463, 91, 27, 27, 25, 2 and GPS 1730,
897, 553, 401, 286, 263; DOY 145 Galileo 1861, 1782, 34, 2, 18, 24 and GPS 1726, 938, 660, 590, 467, 466. On DOY 145
no Galileo satellite is flagged before hour 2.5 (section 10), so every user sits at the code level in hour 2 (453 mm)
and restarts every Galileo ambiguity at once in hour 3.

**The split by the GPS pair of the receiver** decides the reading. Correction to section 9: 11 of the 28 network
receivers are processed on GPS L1W/L2W (BOGT, CUSV, GCGO, LEIJ, POTS, STHL, SVTL, ULAB, URUM, VOIM, WIND) and 17 on
L1C/L2W, read from the users' `closed_loop_sat_trace.csv`; section 9 had the two counts reversed. Cells: the
Galileo-only user / the user of both systems.

| receivers | users | day | h2 | h3 | h4 | h5 | h6 |
|---|---|---|---|---|---|---|---|
| 17 on L1C/L2W | float | 144 | 82.3 / 101.5 | 36.5 / 22.8 | 25.6 / 15.0 | 18.6 / 12.7 | 14.5 / 8.9 |
| 17 on L1C/L2W | fixing | 144 | 82.3 / 101.5 | 31.0 / 16.2 | 21.4 / 13.9 | 14.5 / 11.0 | 9.7 / 8.8 |
| 11 on L1W/L2W | float | 144 | 50.5 / 63.4 | 21.4 / 34.3 | 17.4 / 25.1 | 14.9 / 13.8 | 13.2 / 11.0 |
| 11 on L1W/L2W | fixing | 144 | 48.9 / 63.4 | 14.6 / 25.8 | 10.8 / 22.0 | 9.7 / 9.2 | 11.0 / 14.3 |
| 17 on L1C/L2W | float | 145 | 427.7 / 268.7 | 64.0 / 32.6 | 25.2 / 17.6 | 20.1 / 9.8 | 12.4 / 11.3 |
| 17 on L1C/L2W | fixing | 145 | 427.7 / 268.7 | 64.0 / 29.5 | 23.3 / 11.7 | 10.6 / 9.3 | 13.4 / 8.1 |
| 11 on L1W/L2W | float | 145 | 526.2 / 426.8 | 55.9 / 273.8 | 22.2 / 91.0 | 18.1 / 49.3 | 13.4 / 38.9 |
| 11 on L1W/L2W | fixing | 145 | 526.2 / 426.8 | 55.9 / 330.8 | 16.6 / 91.0 | 14.6 / 52.2 | 12.2 / 39.1 |

GPS is a gain from hour 3 on where the receiver is processed on the tied pair, and a loss where it is processed on
L1W. By the count of receivers whose mean of hours 4 to 6 moves by 5 mm or more (float users): 10 better and 1 worse
of the 17 on DOY 144, 8 and 1 on DOY 145; of the 11 on L1W none better, 5 worse on DOY 144 and 11 worse on DOY 145.
On the 17 receivers bar 3 would pass: fixing on both systems 11.7, 9.3 and 8.1 mm in hours 4 to 6 of DOY 145. With
fixing, the five independent receivers on L1W are the five worst of the eleven on DOY 145 (mean of hours 4 to 6: QUIN
352, WTZZ 108, BADG 58, ARHT 47 and WUH2 45 mm; the six on L1C 8 to 23 mm).

### 11.5 Cause 1: a signal of no held family is presented as continuous

L1W belongs to no held family, so its settled flag is never up, its level is a float of the network, and
`network.ring_fix.unsettled_steps` leaves it alone.

- **On the stream** (`stream_seg_drift.py`, `_rap_f12_n144` and `_rap_f12_n145`, GPS L1W/L2W, segments of constant
  counters after their first 600 s): the ionosphere-free level moves by p50 0.62 and 0.60, p90 1.56 and 1.69, at
  most 3.6 and 5.8 narrow-lane cycles (107 mm each) on the two days. The same read of L1C/L2W with both flags up
  gives p50 0.25 and 0.18, at most 0.42 and 0.32.
- **At the user** (`user_amb_drift.py`: a float ambiguity's own drift against the other satellites over segments of
  20 epochs or more with constant counters and no reset; float users of both systems, DOY 145, hours 3 to 6): GPS at
  the 11 receivers on L1W p50 34 mm and p90 96 mm (medians over the receivers; 200 mm at most), at the 17 on L1C 10
  and 25 mm.
- **Why DOY 144 hides it and DOY 145 does not.** The user resets both ambiguities of a satellite when either signal's
  counter steps (read in the trace of SVTL00RUS: the L1W variance returns to its initial value at the epochs at
  which only the L2W counter stepped), so the L2W steps of an unsettled satellite also cover an L1W user, which is
  why the build equals the copy. Once L1C and L2W settle the steps stop and the L1W ambiguity is carried on a level
  that drifts. On DOY 144 that costs 15, 13, 9 and 6 mm in hours 2 to 5 (paired medians, float). On DOY 145 every
  Galileo ambiguity restarts at hour 2.5 in a solution that leans on those GPS phases: the float Galileo ambiguities
  of LEIJ, POTS and SVTL start 3 to 6 cycles from the network's values and take more than an hour to reach them, and
  the fixing users hold what they find on the way (the wrong holds of 11.2).
- **The receivers.** The 16 receivers on L1W (11 of the network, 5 independent) are all JAVAD receivers whose files
  carry both L1C and L1W. `processing.signal_priorities` lists C1W and L1W before C1C and L1C, in the network run and
  in the users, so the first tracked signal is L1W. The network therefore keeps 11 of its 28 receivers out of the GPS
  L1C/L2W family.

### 11.6 Cause 2: a first hold on a solution that has not converged (URUM00CHN, Galileo only, DOY 145)

The flags come up near TOW 93 600 and every Galileo ambiguity restarts. The position-sigma gate (0.05 m) passes 21
minutes later; the first hold is at TOW 94 890 with ratio 20.0 while the position error is 262 mm (the float sibling:
264 mm). The float sibling reaches 36 mm at TOW 96 000; the fixing user stays between 150 and 240 mm until TOW 99 000
and reaches 38 mm at TOW 103 110. This is the failure of KRGG00ATF on DOY 144 (8.4): a formal sigma five times smaller
than the error, and a decisive ratio on a wrong candidate. On rule L's stream (section 10, the first labels: `nLDx`,
DOY 145) the satellites settle one after another from hour 1 and the receiver has no wrong hold: 35 531 held, 33 471
verified, none wrong.

### 11.7 The wide lane at BOGT00COL

Satellite E18, TOW 97 920 to 99 270 (`wl_off_probe.py`): the fixing user holds (3, 4) cycles on (L1X, L5X) against
the network's arc of the same receiver, so the wide lane is one cycle off and the narrow lane 0.05 cycle. The float
user sits at 1.15 to 1.18 wide-lane cycles from the network at the same epochs: the float solution puts the pair
there and the fixing follows. The combination moves the ionosphere-free range by 6 mm and the ionosphere state by
0.45 m. Which of the user and the network is right is not known; not traced further.

## 12. 2026-11-16: two remedies for the untied signal, at the user (pre-registered)

**Remedy V, in the stream.** A phase signal of no held family is never settled: its counter steps at every emission.
Emulated by `stream_step_unheld.py` on section 9's streams (`_rap_u12x_n144`, `_rap_u12x_n145`: the values equal the
source with the counters masked, the family signals' counter changes are unchanged (38 410 on DOY 145), and GPS L1W
and Galileo L1C and L5Q step at every emission, 65 286 changes against 2 482) and on rule L's copies
(`_rap_f12Lx_n144`, `_rap_f12Lx_n145`). User runs, both days: `nVBf`, `nVBx` (rule B, the 28 network receivers),
`nWBf`, `nWBx` (rule L, the 11 receivers on L1W), `u12x` (float and fixing, the 11 independent receivers).

**Remedy P, at the user.** The user processes the tied pair where its receiver tracks it:
`processing.signal_priorities.GPS` with C1C, L1C, D1C and S1C before C1W, L1W, D1W and S1W, nothing else changed, the
stream of section 9. User runs, both days: `nPBf`, `nPBx` (the 11 network receivers on L1W) and `u12p` (the 5
independent receivers on L1W). The network has not used the L1C data of these receivers, so for GPS they are
independent receivers.

**Bars** (medians over the named receivers of the hourly 3D median; the Galileo-only users are `nUDf` and `nUDx` of
the same receivers and day).

1. **V at the 11 network receivers on L1W:** `nVBf` and `nVBx` not worse than the Galileo-only users by more than
   2 mm in any of hours 4 to 6, on both days. At the 17 other receivers `nVB*` equals `nUB*` (0.0 mm at every
   receiver and hour): a user does not read the counter of a signal it does not process.
2. **P at the same 11:** `nPBf` and `nPBx` not worse than the Galileo-only users by more than 2 mm in any of hours 4
   to 6, and better by at least 3 mm in hour 3 or hour 4 (the gain the 17 receivers show), on both days.
3. **Holds:** every fixing label of this section has wrong holds of kind (a) on at most 0.5 % of its verified
   Galileo holds, and the wide lane never off.
4. **The 5 independent receivers on L1W:** on DOY 145 `u12x` and `u12p` better than `u12` by at least 20 % in each
   of hours 4 to 6; on DOY 144 not worse than `u12` by more than 2 mm in any of hours 4 to 6.

**What follows.** If V passes, the builder is ordered to extend `network.ring_fix.unsettled_steps` to the signals of
no held family. If P passes, the settings of the fixing users include the tied pair first, and the builder is ordered
to run the network itself with L1C before L1W (all 28 receivers in the GPS family: one knob on the chain posture,
both days). The first hold on an unconverged solution (11.6) is read again on the results of section 10.
