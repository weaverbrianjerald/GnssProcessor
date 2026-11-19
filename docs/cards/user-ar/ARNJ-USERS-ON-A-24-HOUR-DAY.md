# ARNJ: users on a 24-hour day

**STATUS**

- **id:** ARNJ
- **topic:** user-ar
- **status:** open
- **summary:** The adopted chain configuration (card ARNH section 11) was graded at the user on the first six hours of two days. This card holds the users of one full day, DOY 146 of 2026, on the builder's 24-hour network run: users that start with the network and run 24 hours, and users that join the running network every two hours. Pre-registered runs and bars; results appended below.
- **dates:** 2026-11-16 (opened; sections 1 to 3); 2026-11-17 (sections 6 and 7)
- **knobs / posture:** the fixing and float users of card ARNH section 9 (GPS band 1 on L1C, partial fixing off, the wide-lane branch check, ratio 5); `estimation.cycle_slip.lli_slip_mask` as decided by card ARNI section 5; `processing.exact_epoch_time` absent (the stream carries the rounded epoch times).
- **code:** engine `build\x64-Release\gnss_processor_rscb6.exe` of the worktree `rx_sat_bias` (uncommitted). Tools `derive_d146.py`, `users_chain_d146.sh`, `day_joins.py`, `ref_changes.py`, `lli_scan.py` and those of cards ARNH and ARNI (copies in `D:\GnssProcessorSupport\presentation\agu26\fixing_control\tools\`).
- **run dirs:** `D:\GnssProcessorSupport\runs_nom\_rap_nw_obsa24{f,x}_146_*`, `_rap_nw_obsj{02..20}{f,x}_146_*`; the builder's streams `_rap_d24_n146` and `_rap_d24r_n146`; outputs under `D:\GnssProcessorSupport\presentation\agu26\fixing_control\grades_arnd\d146\`.
- **source:** card [ARNH](ARNH-USERS-UNDER-THE-FLAG-RULES.md) sections 11 and 16; card [ARNI](ARNI-EXACT-EPOCH-TIME-AT-THE-USER.md); card NRF on the builder's branch `irdu/user-integers`

**Outcome.** Open. On two whole days (DOY 146 and 147) the users of the chain stream keep their accuracy for 24
hours (section 6): the 28 network receivers at 15 and 10 mm in hour 3 and at 4 to 9 mm from hour 6, fixing ahead
of float in every hour, the 11 receivers outside under 16 mm from hour 3, and no hold wrong by the float check
in 1.24 million held pair-epochs. Two bars fail: the Galileo reference satellite changes too often at the
receivers of the L1C/L5Q family, and the joining users miss the count of first fixes at five of ten start hours.
Open: a Galileo wide-lane error the float check cannot see (0.03 to 0.09 % of the holds). Section 7: the two
Galileo families carry one level per satellite in the directions the integers see.

> Results are appended TO THIS FILE, below the text.

## 1. Scope

Cards ARNG and ARNH graded the users on the first six hours of DOY 144 and 145. A day holds what six hours do not:
every satellite sets and most rise again, so each receiver-satellite link ends and a later pass starts a new one;
the datum receiver's links turn over completely; the set of satellites tied by held links (the flag rule L) is
rebuilt many times; a user that joins late meets the network in a state no six-hour run has shown. The builder runs
the adopted configuration for 24 hours on DOY 146 (2026-05-26: `_rap_d24_n146`, the configuration of `f13` with the
day and the length changed, 55 receivers; a second run from the datum receiver WTZR00DEU, `_rap_d24r_n146`, for the
agreement of the two runs' integers). The network's own reads (held share by hour, flags, the agreement of the two
runs) are the builder's, in card NRF. This card asks what a user gets.

## 2. The runs (pre-registered)

Stream `_rap_d24_n146\products.txt`. Engine `gnss_processor_rscb6.exe`; every setting is the one of card ARNH
section 9 (`nFBf`, `nFBx` at the network receivers, `u12f` at the receivers outside), with the day 146. The key
`estimation.cycle_slip.lli_slip_mask` is 3 at every user if the bars of card ARNI section 5 pass, and absent if not.
A scan of the observation files of DOY 146 (`lli_scan.py`, 71 files) finds a loss-of-lock value above 3 at the five
Leica receivers only (every Galileo L1C phase), so of the receivers below the key can change HKSL00HKG alone.

- **Whole day** (`a24f` float, `a24x` fixing; both systems; started with the network, 24 h): the 28 network
  receivers of card ARNH (bogt, bor1, cas1, cpvg, cusv, ffmj, gamb, gcgo, gope, kokv, krgg, lck4, leij, lmmf, mbar,
  mro1, pots, pove, sctb, sthl, svtl, syog, ulab, urum, voim, wark, wind, ykro), the 7 receivers of the second
  Galileo family (solo, park, hksl, jplm, mrc1, seyg, gop6) and the 11 receivers outside the network (arht, ascg,
  badg, cedu, chti, cibg, quin, stpm, utqi, wtzz, wuh2): 92 runs.
- **Joining** (`jHHf`, `jHHx`; both systems; `network.start_offset_hours` HH + 0.004167 for HH = 2, 4, ..., 20;
  2 h long): the 28 network receivers and the 11 outside: 780 runs.

The users start once the network run has ended with exit 0.

## 3. Bars (pre-registered)

Hours are hours of the day (hour 1 is the first); the reference is the first six hours of DOY 144 and 145 on the
adopted configuration (card ARNH section 11: fixing 14.7, 11.4, 7.9, 7.3 and 13.4, 10.4, 8.7, 8.4 mm in hours 3 to 6
at the network receivers; card ARNH section 16 for the joining users: under 10 cm from 12 to 21 min, under 5 cm from
21 to 38 min, first fix at 28 to 52 min).

Whole-day users:

1. **Network receivers.** In each hour from 3 to 24 the median over the 28 receivers of the hourly 3D median is at
   most 20 mm, for the float users and for the fixing users.
2. **Fixing against float.** Paired per receiver, the median difference (fixing minus float) is at most +2 mm in
   each hour from 3 to 24, and negative in the mean over those hours.
3. **Receivers outside the network.** Fixing at most 25 mm in each hour from 3 to 24 (median over the 11).
4. **Holds** (network receivers and the 7 of the second Galileo family; the pairs E L1X/L5X, E L1C/L5Q and
   G L1C/L2W; the float sibling decides): wrong holds at most 0.5 % of the verified holds over the day and at most
   2 % in any hour; every wrong hold is listed with its receiver, satellite and span.
5. **Reference satellite.** Fewer than 80 changes per system in each 24-hour run.

Joining users, at each of the ten start hours, over the 39 receivers:

6. **Convergence.** The 3D error stays under 10 cm from at most 30 min and under 5 cm from at most 50 min after the
   user's start (median over the receivers; a bound counts when held for 10 min).
7. **First fix.** At most 60 min after the start (median), and at least 35 of the 39 receivers have a fix within
   the two hours.
8. **Second hour of the user.** Fixing at most 20 mm (median over the receivers of the hourly 3D median).
9. **Holds.** As bar 4, over the joining users of the network receivers.

**Reading.** A bar that fails is reported by hour and receiver, and the cause is sought in the stream at that hour
(a flag that fell, a datum change, a second pass of a satellite, a receiver that left). The adoption of card ARNH
section 11 stands on its own test; this day adds to its note ("verified by") or names what a longer run breaks.

## 4. 2026-11-16: the Leica receivers through the day, with and without the loss-of-lock mask (pre-registered)

**Why.** Card ARNI section 7: with `estimation.cycle_slip.lli_slip_mask` 3 the Galileo phases of the Leica receivers
are used for the first time. Galileo alone goes from 0.1 to 1.0 m to 8 to 34 mm with no wrong hold, but with both
systems the four receivers end 3.6 and 3.9 mm worse than with the key absent in hours 5 and 6 of DOY 145 (paired
median; REYK also on DOY 144), and three bars of that test fail on DOY 145. Section 2 decided the key by those bars;
they did not pass, and the key is used in this card nevertheless (the decision and its grounds are in card ARNI
section 7). Of the receivers of section 2 only HKSL00HKG carries the bit. This section adds the comparison a day
allows.

**Runs** (added to section 2; both systems, started with the network, 24 h): `a24f`, `a24x` (mask 3) also at
REYK00ISL, WTZR00DEU and WROC00POL; `a7f`, `a7x` (the key absent, the detector as built) at HKSL, REYK, WTZR and
WROC: 14 runs, 106 whole-day runs in all.

**Bars** (the four receivers; fixing against fixing, paired per receiver and hour, median over the four):

10. With the mask not worse than with the key absent by more than 2 mm in at least 20 of the 22 hours from 3 to 24,
    and not worse in the mean over those hours.
11. No wrong hold above 0.5 % of the verified holds of the Galileo L1C/L5Q pair, with the mask.
12. With the mask fewer than 80 changes of the Galileo reference satellite in the day at each of the four.

**Reading.** Bar 10 failing over a day names a cost of the Galileo phases at these receivers that the detector as
built was hiding, to be traced at the receiver (its code on E1, its phase on E5a, its antenna pattern on the Galileo
bands); it does not restore the reading of bit 2 as a loss of lock.

## 5. 2026-11-16: the receiver that sits 35 mm low, and the users' elevation mask (a trace, and a read registered before its runs ended)

**The trace of LCK400IND.** Card ARNH section 17 left it open: the users at LCK400IND sit 35.3 and 35.7 mm low on
DOY 144 and 145, with the IGS coordinate, the eccentricity and the antenna calibration checked. Reads of 2026-11-16
(`standalone_ppp.py`, `user_enu.py`; outputs under `grades_arnd\lli\`):

- Both systems carry it: Galileo alone -30.3 and -31.5 mm (fixing), GPS alone -25.2 and -43.4 mm.
- A stand-alone float solution on external products (COD final orbits, clocks and biases; the configuration of the
  full-day runs of card RSCB; the same reference coordinate) is at -0.6 mm in height over hours 7 to 24 of DOY 145
  and at +1.3 mm on DOY 144; the same solution at STPM, WTZZ, BAKE and BOR1 is at -5.3, -0.2, -0.1 and -0.3 mm. The
  coordinate, the eccentricity and the antenna model are sound.
- The network applies the marker-to-ARP reduction at LCK400IND (0.1230 m), and its congruence check reads
  0.00 +- 0.76 cm in height there.
- `network.closed_loop.consume_network_iono` is inert in a replay (the stream carries no ionosphere record; the
  runs with the key false are byte-identical): the replay users estimate their own ionosphere.
- **The elevation mask decides it.** The stand-alone solution with the mask at 15 degrees (the value of the replay
  users) instead of 10 is at -30.6 mm over hours 7 to 24 (-38.5 mm in hours 4 to 6). The replay user with the mask
  at 10 degrees instead of 15 is at +0.8 mm in hours 4 to 6 of DOY 145 (-32.2 mm at 15 degrees). The observations
  of this receiver between 10 and 15 degrees move its height by 3 cm: an elevation-dependent error at the station
  (antenna pattern, near field or troposphere), on external products as on the network's. It is not the
  corrections.

**The read.** The users of DOY 145 on `f13` with `processing.elevation_mask_deg` 10 (`e10Bf`, `e10Bx`: both
systems, float and fixing, the 28 network receivers and the 11 outside, 78 runs) against the users of card ARNH
section 9 (15 degrees). The resolver's own mask (`estimation.ambiguity_resolution.elevation_mask` 20) is not
changed, and the network runs at 15 degrees. Reported: the hourly medians and their paired differences, the mean
height error per receiver, the holds, the time of the first fix. A bar for a later adoption test, not for this
read: no label above its 15-degree label by more than 1 mm in hours 3 to 6, and no wrong hold.

## 6. 2026-11-17: results of sections 2 to 4 (the whole-day users of DOY 146 and 147, the joining users, the Leica receivers)

**Runs.** Launched by the builder session after the hand-over of 2026-11-16 (`users_chain_d146.sh` and a twin for
DOY 147; engine `gnss_processor_rscb6.exe`, the mask 3 of card ARNI section 7 in the users' settings). DOY 146:
stream `_rap_d24_n146` (the `f13` configuration, without the exact epoch time). DOY 147: stream `_rap_d24_n147`
(with it), whole-day users only. The builder's record of DOY 146 with its reads is card NRF part 11, sections 91 to
91.2, on `refs/heads/irdu/user-integers`. The numbers below were re-read by this session from `grades_arnd\d146\`
and `grades_arnd\d147\`, except the hold totals of DOY 146, which are the builder's (the users' resolver logs keep
the last 15 to 17 hours only; the builder's copies of the grading scripts read the console capture).

Median over the receivers of the hourly 3D median, fixing users (float in parentheses), mm:

| users | hour 3 | hour 4 | hour 5 | hour 6 | hours 7 to 24 |
|---|---|---|---|---|---|
| DOY 146, 28 network receivers | 14.9 (17.5) | 10.7 (12.1) | 10.5 (10.1) | 8.8 (10.3) | 4.8 to 7.8 (5.2 to 8.8) |
| DOY 146, 11 outside | 15.7 (17.9) | 12.8 (13.4) | 9.6 (10.8) | 8.4 (14.2) | 5.2 to 12.2 (5.4 to 10.5) |
| DOY 147, 28 network receivers | 10.3 (13.0) | 9.1 (10.1) | 8.0 (8.2) | 6.2 (7.8) | 4.0 to 6.4 (4.6 to 7.1) |
| DOY 147, 11 outside | 12.5 (16.8) | 11.4 (11.3) | 8.2 (9.5) | 8.7 (11.0) | 6.0 to 10.9 (6.2 to 11.9) |

**The bars of section 3.**

- Accuracy of the 28 (at most 20 mm in every hour from 3, float and fixing): passes on both days.
- Fixing against float at the 28 (paired per receiver, at most +2 mm in any hour, negative in the mean): passes;
  the paired median is -1.7 to -0.0 mm on DOY 146 and -1.9 to -0.1 mm on DOY 147 in every hour from 3.
- The 11 outside (fixing at most 25 mm from hour 3): passes, at most 15.7 and 12.5 mm.
- Holds: no hold wrong by the float sibling on either day. DOY 146 (the builder's totals): Galileo L1X/L5X 182 163,
  Galileo L1C/L5Q 31 323, GPS L1C/L2W 403 009 held pair-epochs. DOY 147 (this session's totals of
  `holds_a24_full_*.txt`): 204 007 (196 396 verified), 32 765 (27 930) and 387 533 (377 729). Passes.
- Reference changes (fewer than 80 per system and run): fails. GPS is under 80 everywhere. Galileo is over at seven
  receivers on DOY 146 (GOP6, HKSL, MRC1, REYK, SEYG, WROC, WTZR: 166 to 309 in the last 15 to 17 hours; the
  builder's whole-day counts 173 to 325) and at one on DOY 147 (SEYG, 174). All of them are receivers of the
  Galileo L1C/L5Q family; section 7 gives the cause.
- Joining users of DOY 146 (780 runs, ten start hours, 39 receivers, fixing): under 10 cm after 13.5 to 25.0 min
  and under 5 cm after 27.2 to 36.5 min (medians; bars 30 and 50 min): passes. The second own hour at 10.5 to
  17.1 mm (bar 20): passes. Holds: none wrong (the builder's totals). First fix: the median is 34.2 to 50.5 min
  (bar 60), and the count of receivers fixed within the two hours is 32, 36, 32, 33, 36, 35, 37, 36, 32 and 34 of
  39 (bar 35): fails at five start hours. The builder's read: BOR100POL misses at 7 of the 10 start hours; its
  joining user passes the ratio test and one GPS pair a fifth of a cycle off trips the per-pair limit, a different
  pair each time. With partial fixing off, one such pair holds back the whole first fix.
- The Leica receivers of DOY 146 (section 4; HKSL, REYK, WTZR, WROC, the mask 3 against the key absent, both
  systems, fixing, paired): -6.3, -7.1 and -2.1 mm in hours 3 to 5, -0.3 to -3.6 mm through hour 18, +0.1 to
  +0.6 mm in hours 19 to 24. Without the mask the four hold Galileo L1C/L5Q on 79 pair-epochs of the day; with it
  on 13 413, none wrong (the builder's totals).

**A wrong hold the float sibling cannot see.** The builder's wide-lane column (the user's wide-lane integer against
the network's at the same receiver) is off on 548 held pair-epochs of DOY 146 (0.09 %) and on 204 of DOY 147
(0.03 %: BOGT00COL 95 in hour 13, JPLM00USA 82 in hour 24, SEYG00SYC 27 in hours 13 and 14), all Galileo; on
DOY 146 the narrow lane of these holds is within 0.05 cycle (the builder's read). The arithmetic (checked here): the pair (3, 4) on (E1, E5a) changes the wide lane by
one cycle and the ionosphere-free phase by 2 c / (10 491 f0) = 5.6 mm, 0.051 narrow-lane cycle; its geometry-free
part (0.448 m) is taken by the user's own ionosphere state (0.565 m on E1). On GPS L1/L2 the nearest pairs with a
wide lane off by one, (4, 5) and (3, 4), cost 50 and 57 mm (0.47 cycle), so the search refuses them. For Galileo
E1/E5a the engine admits the wide lane by the float wide lane and its formal sigma alone
(`|d| + 3 sigma_w <= wl_max_frac`; the phase is never decisive at a gap of 0.051 cycle, as the comment of
`wl_branch_check` states), and nothing downstream can refuse a wide lane that is one cycle off. Cost while it
lasts: 5.6 mm of range on one satellite. Not repaired. Candidates: for a pair with a small gap, the float wide lane
within the limit of one integer for a set time before admission (the persistence that `wl_branch_persist_s`
already keeps for the phase), a floor under its formal sigma, or a third frequency once the network holds one.

## 7. 2026-11-17: the offset between the two Galileo families by direction (a read of the streams)

**Why.** Bar 5 fails at the receivers of the Galileo L1C/L5Q family: their satellites are unsettled on the stream
for hours (card NRF part 11 section 92.2), and a user whose only high satellite is unsettled changes its reference
at every epoch (card ARNI section 7). Card NRF part 10 section 88 compared the two families' phase biases per band
and found the satellites' offsets b(L1X) - b(L1C) to agree to 0.10 to 0.15 cycle only (0.14 to 0.18 on E5a).

**How** (`family_offset_if.py` in the tools folder; streams `f13_n144`, `f13_n145`, `d24_n146`, `d24_n147`; every
15 or 30 min from hour 2; satellites flagged on all four signals, 14 to 25 per epoch). With d1 and d5 the
per-band offsets in cycles, each known up to an integer: the wide-lane offset w = d1 - d5, and the ionosphere-free
offset in narrow-lane cycles once the wide lane is an integer, n = d1 + (f5 / (f1 - f5)) w. Circular spread over
the satellites about the epoch's common value (the two families' datum difference), median over the epochs:

| stream | E1 | E5a | wide lane | ionosphere-free | uniform draw (5th percentile) |
|---|---|---|---|---|---|
| `f13_n144` | 0.136 | 0.168 | 0.048 | 0.029 | 0.223 |
| `f13_n145` | 0.117 | 0.158 | 0.039 | 0.021 | 0.231 |
| `d24_n146` | 0.119 | 0.162 | 0.044 | 0.027 | 0.215 |
| `d24_n147` | 0.135 | 0.154 | 0.074 | 0.021 | 0.223 |

**Reads.**

- The per-band disagreement lies in the ionosphere direction. The E5a spread is 1.14 to 1.36 times the E1 spread in
  cycles (f1 / f5 = 1.34 for an offset of the slant ionosphere's level; 0.75 for an ionosphere-free one). That
  level is set by the code of each family's receivers, and a user with its own ionosphere state does not see it.
- In the ionosphere-free direction the two families agree to 0.02 to 0.03 narrow-lane cycle (2 to 3 mm), in the
  wide lane to 0.04 to 0.07 cycle.
- Per satellite the ionosphere-free offset is a constant within about 0.03 cycle of the common value, with a
  scatter over the day of 0.005 to 0.025 cycle at most satellites, and it survives a loss. On DOY 147 E36 is
  flagged in both families until 3.5 h (-0.01), flagged in the L1X/L5X family alone at every sample from 4.0 to
  17.0 h, and returns at -0.00 to -0.03; E04 is lost by the L1C/L5Q family from 4.0 to 13.5 h and returns within
  0.02 cycle of its earlier value. On DOY 146 E36 is flagged in the L1X/L5X family alone from 2.0 to 22.1 h, and
  E27 from 3.0 h on.
- The wide-lane offset of a satellite is constant over hours and differs between days (E12: +0.002 on DOY 145,
  -0.225 on DOY 147, scatter 0.012 and 0.018): the code level of each family at its first hold.
- One satellite moves with both flags up: E30 on DOY 147, from +0.02 at 5.5 h to +0.33 at 8.5 h, before the
  L1C/L5Q family loses it at 9.0 h. One family's level walked a third of a cycle in three hours.

**Reading.** In the two directions that the integers see, the families carry one level per satellite; the premise
of tying them holds there, and the per-band fractions of section 88 measured the third direction. The satellites
the L1C/L5Q family leaves unsettled for hours are held by the L1X/L5X family at the same time: the loss is that
family's network of 27 receivers, not the satellite. The offset series is also a check of one family's holds
against a disjoint set of receivers (E30). Handed to the builder: a rerun of this read, the pieces of the L1C/L5Q
family's held network by hour, and the design of one Galileo network per band, in which the wide-lane offsets
(code level, up to 0.2 cycle per satellite) need a term of their own. Not built; the author decides.
