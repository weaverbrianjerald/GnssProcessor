# ARNH: users under the candidate flag rules, and the GPS signal of no held family

**STATUS**

- **id:** ARNH
- **topic:** user-ar
- **status:** open
- **summary:** Continuation of card ARNG (that file neared the 60 KB bar). Card ARNG holds the user replays on the builder's stream that declares its steps and flags settled signals, the pre-registration of the flag rules at the user (its section 10) and of two remedies for a GPS signal of no held family (its section 12). This card takes their results, a wait on the flag's raise, and the replays on the builder's streams with GPS processed on L1C.
- **dates:** 2026-11-16 (opened as the continuation; sections 2 to 11: the flag rules, the wait, the two remedies, the replays on `p12` and the adoption test on `f13`)
- **knobs / posture:** none new here. The builder's keys `network.ring_fix.settled_rule` (rule L) and `network.ring_fix.unsettled_steps_unheld` (both UNVALIDATED on its branch); `processing.signal_priorities` with GPS L1C before L1W (at the user in section 12 of card ARNG, in the network in the builder's arms `p12`).
- **code:** none. Tools of card ARNG and `stream_delay_flags.py`; run scripts `users_chain_rules.sh`, `users_chain_ext2.sh`, `users_chain_prio.sh`, `users_chain_wait.sh`, `users_chain_wait10.sh`, `users_chain_p12.sh`, `users_chain_f13.sh`; folder makers `derive_wait.py`, `derive_p12.py`, `derive_f13.py` (copies under `D:\GnssProcessorSupport\presentation\agu26\fixing_control\tools\`).
- **run dirs:** `D:\GnssProcessorSupport\runs_nom\_rap_nw_obs{nL,nM}{Df,Dx}_{144,145}_*`, `_rap_nw_obsnC{Df,Dx}_145_*`, `_rap_nw_obsnK{Df,Dx}_{144,145}_*`; streams `_rap_f12{Ls,LCs,cs}_n14x`, `_rap_f12Ld20_n14x`, `_rap_f12Ld20s_n14x`, the builder's `_rap_l12_n144`, `_rap_l12_n145`; `_rap_nw_obsnJ{Df,Dx}_{144,145}_*`, `_rap_nw_obsnQ{Df,Dx,Bf,Bx}_145_*`, `_rap_nw_{obs,fxb}u12q_145_*`, `_rap_nw_obsnF{Df,Dx,Bf,Bx}_{144,145}_*`, `_rap_nw_{obs,fxb}u12f_{144,145}_*`; the builder's `_rap_p12_n145`, `_rap_f13_n144`, `_rap_f13_n145`; outputs under `D:\GnssProcessorSupport\presentation\agu26\fixing_control\grades_arnd\{rules,wait,prio,ext,p12,f13}\`.
- **source:** card [ARNG](ARNG-USERS-ON-THE-DECLARED-STREAM.md), sections 10 to 12; card NRF part 8 sections 61 to 66 on the builder's branch `irdu/user-integers`

**Outcome.** Open; continued in card [ARNI](ARNI-EXACT-EPOCH-TIME-AT-THE-USER.md) (the exact epoch time). On the builder's final candidate arm (the flags by the largest tied set with a wait of 10
emissions, the unsettled steps, GPS band 1 on L1C, the decision cap of 12, three held families) a fixing user of
both systems reaches 15, 11, 8 and 7 mm (DOY 144) and 13, 10, 9 and 8 mm (DOY 145) in hours 3 to 6 at 28 network
receivers, 16, 10, 11 and 9 mm and 13, 12, 9 and 8 mm at 11 independent ones, with no wrong hold in 255 781 held
pair-epochs. Every pre-registered bar passes; adoption on the builder's branch is ordered. Before the change
of GPS band 1 the second day gave 55, 25, 22 and 17 mm.

> Results are appended TO THIS FILE, below the text.

---

## 1. Scope

Sections of card ARNG are cited by their numbers there. The rules: B (a satellite is flagged when the tree's datum
receiver reaches it through held links and certified tree edges), C (rule B with the agreement test), L (the
largest set of a family's satellites tied to each other by held links, whatever receiver roots it), M (rule L with
the agreement test). Positions are medians over the 28 network receivers of the hourly 3D median, hours of the day,
in mm.

## 2. 2026-11-16: results of ARNG section 10, the Galileo-only labels

**Runs.** 280 replays (`nLDf`, `nLDx`, `nMDf`, `nMDx` on both days; `nCDf`, `nCDx` on DOY 145), all exit 0, on the
settled-part copies of the builder's stamps. The labels of both systems (`nLB*`, `nCB*`, `nMB*`) were stopped after
7 runs: 11 of the 28 receivers are processed on a GPS signal of no held family (ARNG section 11.5), which makes a
comparison of rules for GPS inconclusive there, and four run sets at once had saturated the disk. They are run on
the builder's streams with GPS on L1C instead (the arms `p12`).

**The build against the emulation.** The builder's rule L streams `_rap_l12_n144` and `_rap_l12_n145` (exe nrf26)
differ from `_rap_u12_n144` and `_rap_u12_n145` in the settled flags and the phase counters only (4 156 and 11 038
flags; no other field of any line). Their family counter changes (22 677 and 27 402) contain all of the emulated
copies' (22 593 and 27 279); the flagged share by half hour is the stamp's from the second half hour on (the first:
2 and 1 % against 3 and 2 % on Galileo, the minimum of four satellites). The results below therefore stand for the
build after the first half hour.

### 2.1 Positions

| rule, users | day | h1 | h2 | h3 | h4 | h5 | h6 |
|---|---|---|---|---|---|---|---|
| B float | 145 | 413.6 | 452.9 | 62.3 | 24.8 | 19.0 | 13.3 |
| L float | 145 | 401.8 | 49.6 | 24.8 | 17.9 | 13.2 | 12.9 |
| C float | 145 | 413.6 | 452.9 | 64.1 | 22.2 | 16.6 | 11.6 |
| M float | 145 | 402.1 | 53.9 | 24.9 | 15.8 | 13.5 | 13.3 |
| B fixing | 145 | 413.6 | 452.9 | 60.0 | 19.9 | 14.4 | 12.2 |
| L fixing | 145 | 401.8 | 46.8 | 15.7 | 13.1 | 11.1 | 8.6 |
| C fixing | 145 | 413.6 | 452.9 | 64.1 | 16.2 | 15.2 | 12.1 |
| M fixing | 145 | 402.1 | 49.0 | 16.1 | 10.4 | 12.3 | 9.7 |
| B float | 144 | 559.7 | 70.4 | 27.4 | 23.6 | 16.9 | 13.8 |
| L float | 144 | 501.7 | 82.3 | 28.1 | 22.7 | 16.4 | 13.7 |
| M float | 144 | 548.5 | 70.5 | 24.1 | 21.9 | 15.1 | 13.2 |
| B fixing | 144 | 559.7 | 70.4 | 24.9 | 19.2 | 10.6 | 10.4 |
| L fixing | 144 | 501.7 | 82.3 | 23.2 | 18.9 | 9.7 | 10.4 |
| M fixing | 144 | 548.5 | 73.5 | 18.5 | 17.4 | 11.5 | 10.8 |

Paired, rule L against rule B on DOY 145 (fixing, hours 1 to 6): -32.7, -398.1, -38.9, -6.5, -4.7 and -2.2 mm; over
hours 4 to 6, 13 receivers better and 3 worse by 5 mm or more (BOGT +18 and +17 mm in hours 4 and 5, LMMF +28 and
+41, WARK +14 and +12). Paired, rule M against rule L: within 3 mm in hours 3 to 6 on both days and 2 to 5 mm worse
in hour 2.

### 2.2 Holds of the fixing users

| label, day | held pair-epochs | equal to the network | verified right | wrong | wide lane off |
|---|---|---|---|---|---|
| B `nUDx` 145 | 24 403 | 95.0 % | 21 588 | 893, URUM00CHN | 185, BOGT00COL E18 |
| L `nLDx` 145 | 35 531 | 99.5 % | 33 471 | 0 | 163, BOGT00COL E18 |
| C `nCDx` 145 | 21 550 | 96.5 % | 18 718 | 489, URUM00CHN | 0 |
| M `nMDx` 145 | 32 387 | 99.4 % | 29 207 | 0 | 177, BOGT00COL E18 |
| B `nUDx` 144 | 46 416 | 99.5 % | 43 762 | 48, KRGG00ATF | 0 |
| L `nLDx` 144 | 47 649 | 99.5 % | 44 843 | 48, KRGG00ATF | 0 |
| M `nMDx` 144 | 40 230 | 99.5 % | 37 268 | 98, KRGG00ATF | 0 |

Every wrong hold is of kind (a) of ARNG section 8.3.

### 2.3 The bars of ARNG section 10

1. **The early gain of rule L.** DOY 145, Galileo only: PASS (fixing 46.8 and 15.7 mm in hours 2 and 3 against 452.9
   and 60.0, 90 and 74 % better). DOY 144: FAIL by the letter in hour 2 (medians 82.3 against 70.4 mm, float and
   fixing; the paired median is 0.0). With fixing, seven receivers are worse than under rule B by 5 mm or more in
   hour 2 (CPVG +6, LMMF +14, GCGO +16, POVE +24, GAMB +26, BOGT +30, KOKV +43 mm) and two better (WIND -11, SCTB
   -8); five of the seven gain 53 to 339 mm in hour 1 under the same rule. In hour 3 BOGT (+49 mm) and KOKV (+21)
   are still worse.
2. **No loss later:** PASS for the Galileo-only labels of rules C, L and M on both days (hours 4 to 6).
3. **Holds.** Rule L: no wrong hold on DOY 145 (URUM00CHN clean), 0.11 % on DOY 144. Rule M: none and 0.26 %. Rule
   C: 2.5 % on DOY 145, FAIL. The wide lane off at BOGT00COL is not a wrong hold (section 3); by the letter the bar
   fails on it under rules B, L and M.
4. **The agreement test for users of both systems:** not graded (the labels were stopped).

**Reading.** Rule L is the candidate: it removes the two hours without Galileo phase of DOY 145 and the mass
restart that produced the wrong holds, and costs nothing from hour 3. Its cost is hour 2 of DOY 144 at seven
receivers: a user carries phase from the first hour on satellites whose levels are flagged early. Rule C and the
agreement test bring no gain for Galileo (fewer holds, the same or more wrong ones): not adopted, not built.

## 3. 2026-11-16: the wide lane at a receiver that hosts the satellite's tree edge

ARNG section 11.7 left open which of the user and the network is right at BOGT00COL on E18. Read in
`network_chain_edges.csv` and `network_ring_fix.csv` of `_rap_u12_n145`: from TOW 87 330 (a re-host that carries the
link's value, 15 minutes after the start of the day) until TOW 102 180 BOGT00COL is the host of E18 in the L1X and
L5X trees, the tree edge pinned at -6 and 17 cycles, not certified. Those are the whole numbers the link's floats
round to in its first minutes (-5.88 and 17.12 at TOW 86 700), when its wide lane read between -24 and -23 with
fractions up to 0.50. A tree-edge pin is a choice of level, so any whole
numbers are legitimate for the phase; but the satellite's code biases do not move with it, and the code of the same
receiver, averaged over the arc, puts the wide lane 1.15 to 1.18 cycles from the pin. The user at that receiver
(float and fixing alike) settles on the code's wide lane and holds (3, 4) cycles against the pin, which is 6 mm in
ionosphere-free range. E18 is flagged through held chords of other receivers. After TOW 102 180 the host is
STHL00GBR and the link is a chord held at (-5, -7). The bar "the wide lane never off" should not count a satellite
at the receiver that hosts its tree edge.

## 4. 2026-11-16: rule L with a wait on the flag's raise (pre-registered)

**Why.** Section 2.3: under rule L a user carries phase from the first hour on satellites flagged early, and seven
receivers are worse in hour 2 of DOY 144. The builder's truth audit on the simulator (card NRF part 8 section 61.1)
finds the same period: of the pairs rule L adds in the first 90 minutes, 2.7 % are more than 0.10 cycle from truth
(0.4 % of rule B's own pairs in the third half hour), and one satellite's phase biases move by 1 to 10 cm per epoch
for some minutes after the epoch at which both rules flag it.

**Stream.** `stream_delay_flags.py --n 20` on the builder's stamps `_rap_f12L_n144` and `_rap_f12L_n145`: a satellite
is flagged only from the 21st consecutive emission (10 minutes) at which rule L flags both signals of its family;
then the settled-part copy as in ARNG section 5.1 (`_rap_f12Ld20s_n144`, `_rap_f12Ld20s_n145`). Flagged share of
Galileo by half hour 1 to 6: DOY 144 rule L 3, 44, 89, 91, 97, 99 %, with the wait 0, 24, 81, 89, 95, 98 % (rule B
0, 36, 89, 91, 97, 99 %); DOY 145 rule L 2, 39, 84, 96, 97, 98 %, with the wait 0, 20, 75, 93, 97, 97 % (rule B 0,
0, 0, 0, 97, 98 %).

**User runs.** `nKDf`, `nKDx` (float, fixing; Galileo only; the settings of ARNG section 9), the 28 network
receivers, both days.

**Bars** (medians over the receivers of the hourly 3D median).

1. **DOY 144, hour 2:** not worse than rule B's users by more than 2 mm (float and fixing), and at most three
   receivers worse than under rule B by 5 mm or more with fixing (rule L: seven).
2. **DOY 145, the gain kept:** the fixing user better than rule B's by at least 20 % in hours 2 and 3.
3. **No loss later:** in hours 3 to 6 of both days not worse than rule L's users by more than 2 mm.
4. **Holds:** wrong holds of kind (a) on at most 0.5 % of the verified holds on each day.

**What follows.** If the bars pass, the wait is proposed to the builder as a parameter of the flag (emissions in the
set before the raise), to be read on its truth audit of the simulator first. If bar 1 fails, the hour-2 cost is not
a matter of minutes after the raise and stays as measured.

## 5. 2026-11-16: the replays on the builder's streams with GPS on L1C, both days (pre-registered)

**Why.** ARNG section 11.5: the network and the users process the receivers that report both L1C and L1W on L1W,
a signal of no held family. The builder's arms `p12_n144` and `p12_n145` (card NRF part 8 section 65, exe nrf26) are
`l12_n144` and `l12_n145` (the step declaration, the release increments, the flags by rule L, the unsettled steps)
with C1C, L1C, D1C and S1C ahead of their W entries in `processing.signal_priorities.GPS`: every receiver of the
network then contributes GPS on L1C/L2W, and the L1W phase leaves the stream. These are the first streams on which
the flags, the steps and GPS can be judged together at every receiver.

**User runs** (started with the network; the settings of ARNG section 9; the users of both systems with the same
reordering of `processing.signal_priorities.GPS`): `nQDf`, `nQDx` (float, fixing; Galileo only) and `nQBf`, `nQBx`
(float, fixing; both systems) at the 28 network receivers, and `u12q` (float and fixing; both systems) at the 11
independent receivers; DOY 144 on `_rap_p12_n144`, DOY 145 on `_rap_p12_n145`. The network's whole numbers are read
from the same runs.

**Bars** (medians over the receivers of the hourly 3D median; hours of the day).

1. **Galileo not harmed by the GPS change:** `nQDf` and `nQDx` not worse than rule L's users of section 2 (`nLDf`,
   `nLDx`) by more than 2 mm in any of hours 3 to 6, on both days.
2. **GPS a gain at every receiver:** `nQBf` and `nQBx` not worse than `nQDf` and `nQDx` by more than 2 mm in any of
   hours 3 to 6 on both days, for the 17 receivers already on L1C and for the 11 that move from L1W, each set taken
   apart.
3. **The level reached:** the fixing user of both systems at most 15 mm in hours 5 and 6 on both days (28
   receivers).
4. **Holds:** every fixing label has wrong holds of kind (a) on at most 0.5 % of its verified holds on each day,
   Galileo pairs and GPS pairs apart; the wide lane off only on a satellite at the receiver that hosts its tree edge
   (section 3).
5. **The independent receivers:** `u12q` not worse than `u12` (ARNG section 11.1) by more than 2 mm in any of hours
   4 to 6 on either day (medians over the 11), and the five that move from L1W better than under `u12` by at least
   20 % in each of hours 4 to 6 of DOY 145.

**Reads.** GPS holds at the users, the 11 receivers new to the family apart; the resets per receiver and hour; hours
1 to 3; the builder's own bars of its section 65.

**What follows.** If the bars pass and the builder's pass, the candidate for adoption on the builder's branch in
the chain posture with fixing is: the flags by rule L, the unsettled steps with the signals of no held family, GPS
band 1 on L1C, with the settings of the fixing users (partial fixing off, the flag required, the tied pair first).

## 6. 2026-11-16: a wait of 10 emissions beside the wait of 20 (pre-registered while section 4's runs are in flight)

**Why.** The builder's read of the wait on its simulator truth audit (card NRF part 8 section 67): with a wait of 10
emissions rule L's flagged pairs more than 0.10 cycle from truth fall from 894 to 203 on GPS (rule B: 198) and from
256 to 35 on Galileo (rule B: 180) for 4 % of the flagged pair-epochs; a wait of 20 brings them to 138 and 28 for
9 %; a wait of 40 costs 17 % for little more. Above 0.25 cycle every wait keeps only one declared step. The shorter
wait is the better trade there, so it is graded at the user beside section 4's.

**Stream and runs.** `stream_delay_flags.py --n 10` on the same stamps, then the settled-part copy
(`_rap_f12Ld10s_n144`, `_rap_f12Ld10s_n145`); flagged share of Galileo by half hour 1 to 6: DOY 144 0, 34, 86, 90,
97, 98 %; DOY 145 0, 30, 80, 95, 97, 97 %. Users `nJDf`, `nJDx` (float, fixing; Galileo only), the 28 network
receivers, both days.

**Bars.** Those of section 4. The wait proposed to the builder is the shorter one that passes them; if both pass,
10 emissions.

## 7. 2026-11-16: results of ARNG section 12, the two remedies for the GPS signal of no held family

**Runs.** Remedy P (the user processes L1C/L2W): `nPBf` and `nPBx` at the 11 network receivers on L1W and `u12p` at
the 5 independent ones, DOY 145, 32 runs, all exit 0. Remedy V (the untied signals step at every emission): `nVBf`
at 6 of the 11 receivers on DOY 145 and at 3 on DOY 144, and at 8 and 7 receivers on L1C (the identity check). The
rest was stopped (remedy P on DOY 144; remedy V's fixing users, its independent receivers and the label W): a run of
both systems took about 15 minutes on a saturated disk, and the builder's streams with GPS on L1C (section 5) put
both days to the stronger test.

**Remedy P, DOY 145** (medians over the receivers, mm).

| users | receivers | h1 | h2 | h3 | h4 | h5 | h6 |
|---|---|---|---|---|---|---|---|
| float, Galileo only | 11 network | 556.1 | 526.2 | 55.9 | 22.2 | 18.1 | 13.4 |
| float, both systems as built (L1W) | 11 network | 568.3 | 426.8 | 273.8 | 91.0 | 49.3 | 38.9 |
| float, both systems on L1C | 11 network | 693.4 | 278.2 | 44.0 | 18.1 | 16.5 | 13.4 |
| fixing, Galileo only | 11 network | 556.1 | 526.2 | 55.9 | 16.6 | 14.6 | 12.2 |
| fixing, both systems as built (L1W) | 11 network | 568.3 | 426.8 | 330.8 | 91.0 | 52.2 | 39.1 |
| fixing, both systems on L1C | 11 network | 693.4 | 278.2 | 44.0 | 12.9 | 10.9 | 10.0 |
| float, as built (L1W) | 5 independent | 571.0 | 996.2 | 317.3 | 108.9 | 63.2 | 38.0 |
| float, on L1C | 5 independent | 638.7 | 274.8 | 25.9 | 18.5 | 10.9 | 11.0 |
| fixing, as built (L1W) | 5 independent | 571.0 | 996.2 | 317.3 | 97.8 | 56.1 | 36.5 |
| fixing, on L1C | 5 independent | 638.7 | 274.8 | 23.9 | 16.6 | 10.0 | 10.2 |

Holds of `nPBx`: 10 876 held pair-epochs, 98.3 % equal to the network, 10 430 verified right, none wrong; the wide
lane off on 185, BOGT00COL E18 (section 3). The independent receivers with fixing, mean of hours 4 to 6: ARHT from
46.9 to 5.0 mm, BADG 58.3 to 6.9, QUIN 351.8 to 22.5, WTZZ 108.2 to 12.7, WUH2 44.8 to 28.2. The network had not
used the L1C data of any of these 16 receivers.

**Remedy V, float users.** DOY 145, six receivers (BOGT, CUSV, GCGO, LEIJ, POTS, STHL), hours 3 to 6: 52.2, 19.3,
18.8 and 14.9 mm against the Galileo-only users' 46.4, 20.1, 19.0 and 14.8 (as built 481.0, 94.1, 59.4 and 40.2);
per receiver within 1.4 mm of the Galileo-only user in hours 4 to 6, and 13 to 14 mm worse in hour 3 at LEIJ and
STHL. DOY 144, three receivers: within 0.9 mm in hours 3 to 6. At the receivers on L1C the runs equal those on the
unstepped stream at every hour (0.00 mm): a user reads only the counters of the signals it processes.

**The bars of ARNG section 12.** 1 (remedy V): met on what ran (float, six and three receivers); the fixing users
not run. 2 (remedy P): PASS on DOY 145; DOY 144 not run. 3 (holds): PASS for `nPBx`, the wide lane being the pin of
section 3. 4 (independent receivers): PASS for `u12p` on DOY 145; `u12x` and DOY 144 not run.

**What followed.** The builder built remedy V as `network.ring_fix.unsettled_steps_unheld` (card NRF part 8 section
66, exe nrf27) and ran the network with GPS band 1 on L1C (its section 65.1, arms `p12_n144` and `p12_n145`): the
GPS L1C/L2W family spans 55 receivers (38 before), holds 86 to 89 % of its links from hour 3 on both days (50 to 65
and 35 to 45 % before), and L1W leaves the stream. The split of GPS band 1 between L1C and L1W was the larger part of
GPS's shortfall against Galileo in the network.

## 8. 2026-11-16: results of sections 4 and 6, the wait on the flag's raise

**Runs.** 224 replays (`nKDf`, `nKDx` with a wait of 20 emissions; `nJDf`, `nJDx` with a wait of 10; both days), all
exit 0; grades under `grades_arnd\wait\`. Galileo only; medians over the 28 receivers, mm.

| rule, users | day | h1 | h2 | h3 | h4 | h5 | h6 |
|---|---|---|---|---|---|---|---|
| B fixing | 144 | 559.7 | 70.4 | 24.9 | 19.2 | 10.6 | 10.4 |
| L fixing | 144 | 501.7 | 82.3 | 23.2 | 18.9 | 9.7 | 10.4 |
| L, wait 10, fixing | 144 | 534.5 | 81.0 | 23.1 | 15.1 | 9.9 | 9.4 |
| L, wait 20, fixing | 144 | 532.1 | 78.3 | 21.5 | 15.6 | 10.5 | 9.2 |
| L, wait 10, float | 144 | 534.5 | 81.0 | 29.1 | 23.6 | 17.0 | 12.3 |
| L, wait 20, float | 144 | 532.1 | 78.7 | 29.9 | 24.1 | 17.5 | 12.1 |
| B fixing | 145 | 413.6 | 452.9 | 60.0 | 19.9 | 14.4 | 12.2 |
| L fixing | 145 | 401.8 | 46.8 | 15.7 | 13.1 | 11.1 | 8.6 |
| L, wait 10, fixing | 145 | 422.0 | 59.6 | 13.3 | 11.0 | 11.5 | 8.3 |
| L, wait 20, fixing | 145 | 443.6 | 62.1 | 13.0 | 12.1 | 11.5 | 8.6 |
| L, wait 10, float | 145 | 422.0 | 59.6 | 21.5 | 17.3 | 12.8 | 13.2 |
| L, wait 20, float | 145 | 443.6 | 64.5 | 24.6 | 17.9 | 12.6 | 13.5 |

The float users of rules B and L are in section 2.1. Holds with the wait of 10: 48 084 held pair-epochs on DOY 144
(44 812 verified, 56 wrong at KRGG00ATF, 0.12 %) and 35 510 on DOY 145 (32 992 verified, none wrong); with the wait
of 20: 48 882 (45 174 verified, 63 wrong at KRGG00ATF) and 33 809 (31 581 verified, none wrong). The wide lane is
off only at BOGT00COL on E18 (section 3).

**Bars of section 4.**

1. **DOY 144, hour 2: FAIL for both waits** (81.0 and 78.3 mm against rule B's 70.4). With fixing, 15 and 14
   receivers are worse than under rule B by 5 mm or more and 9 and 10 better (rule L: 7 and 2).
2. **DOY 145, the gain kept: PASS** (59.6 and 13.3 mm, 62.1 and 13.0 mm, against rule B's 452.9 and 60.0).
3. **No loss later: PASS.** Against rule L in hours 3 to 6 the waits are within 2 mm above and up to 4 mm below
   (fixing, hour 4 of DOY 144: 15.1 and 15.6 against 18.9 mm).
4. **Holds: PASS.**

**Hour 2 of DOY 144 is not a matter of minutes after the raise.** In hour 2 the two rules flag the same
satellite-epochs (`stream_seg_drift.py` on `_rap_f12_n144` and `_rap_f12L_n144`, TOW 3 600 to 7 170: the same 35
flagged segments and 3 106 satellite-epochs), and the users' resets per receiver and hour are equal from hour 2 on
(at the seven receivers of section 2.3: 179, 58, 69, 55 and 5 under both rules); they differ in hour 1 only (1 648
under rule B, 1 419 under rule L). What differs in hour 2 is the state a user carries over from the first hour of
the network, when its own levels are still moving. A wait changes which ambiguities are carried, not that.

**Decision.** At the user the wait costs 13 to 15 mm in hour 2 of DOY 145 and gains 2 to 4 mm in hours 3 and 4; on
the builder's truth audit of the simulator (card NRF part 8 section 67) a wait of 10 emissions removes most of rule
L's early inconsistency for 4 % of the flagged pairs. The value 10 is passed to the builder for the arm on which
adoption is judged (`f13_n144`, `f13_n145`: rule L with the wait, the unsettled steps with the signals of no held
family, GPS on L1C, the decision cap of 12, Galileo L1C/L5Q held beside L1X/L5X if its own bars pass).

## 9. 2026-11-16: the replays on the final candidate arm, on which adoption is judged (pre-registered)

**Streams.** The builder's `f13_n144` and `f13_n145` (card NRF part 8 section 70, exe nrf28): the flags by rule L
with a wait of 10 emissions (`network.ring_fix.settled_flags`, `settled_rule` "largest",
`settled_wait_emissions` 10), the unsettled steps of the held families and of the signals of no held family
(`unsettled_steps`, `unsettled_steps_unheld`), GPS band 1 on L1C, the decision cap of 12, and Galileo L1C/L5Q held
beside L1X/L5X (`network.ring_fix.pairs` E:L1X/L5X, E:L1C/L5Q, G:L1C/L2W). The first replays on the intermediate
arm `p12` (section 5; DOY 145 only, the day that failed in ARNG section 11) are reported with the results.

**User runs** (started with the network; the settings of section 5): `nFDf`, `nFDx` (float, fixing; Galileo only)
and `nFBf`, `nFBx` (float, fixing; both systems) at the 28 network receivers of the earlier sections (Galileo
L1X/L5X) and at 7 network receivers of the second Galileo family (L1C/L5Q: SOLO00SLB, PARK00AUS, HKSL00HKG,
JPLM00USA, MRC100USA, SEYG00SYC, GOP600CZE, the builder's proposal); `u12f` (float and fixing; both systems) at the
11 independent receivers; both days.

**Bars** (medians over the named receivers of the hourly 3D median; hours of the day).

A. The 28 receivers on Galileo L1X/L5X.

1. **The level reached:** fixing on both systems at most 15 mm in each of hours 4 to 6, on both days.
2. **GPS a gain:** the users of both systems not worse than the Galileo-only users of the same stream by more than
   2 mm in any of hours 3 to 6, float and fixing, on both days (the 17 receivers on L1C before and the 11 that move
   from L1W are read apart).
3. **Fixing against float:** `nFBx` not worse than `nFBf` by more than 2 mm in any of hours 3 to 6, on both days.
4. **Holds:** every fixing label has wrong holds of kind (a) on at most 0.5 % of its verified holds on each day,
   Galileo pairs and GPS pairs apart; the wide lane off only on a satellite at the receiver that hosts its tree edge
   (section 3).
5. **Against the stream of ARNG section 9:** fixing on both systems not worse than `nUBx` (ARNG section 11.1) by
   more than 2 mm in any of hours 3 to 6 of DOY 144, and better by at least 30 % in each of hours 3 to 6 of DOY 145.

B. The 11 independent receivers: `u12f` with fixing at most 20 mm in hours 5 and 6 on both days, and not worse than
`u12` (ARNG section 11.1) by more than 2 mm in any of hours 4 to 6 on either day.

C. The 7 receivers of the second Galileo family, judged apart (the builder's section 68.1 reads the flagged levels
of that family as moving on DOY 144): fixing on both systems at most 20 mm in hours 5 and 6, and wrong holds of kind
(a) on at most 0.5 % of the verified holds of the pair L1C/L5Q, each day.

**What follows.** If A and B pass, the package is the candidate adopted on the builder's branch in the chain posture
with fixing, with the three-part note (date; verified by these replays on two days; to do: more days and
receivers): the flags by rule L with the wait of 10, the unsettled steps with the signals of no held family, GPS
band 1 on L1C, with the settings of the fixing users (partial fixing off, the flag required, the tied pair first).
C decides whether Galileo L1C/L5Q is held in that posture, or left a signal of no held family (stepping) until its
levels on DOY 144 are traced. The goldens of the main posture are not changed.

## 10. 2026-11-16: results of section 5, the replays on `p12`, DOY 145

**Runs.** 134 replays on `_rap_p12_n145` (GPS on L1C in the network, rule L, the unsettled steps, the decision cap
of 3), all exit 0; grades under `grades_arnd\p12\`. DOY 144 was not run on `p12`: the final candidate arm (section 9)
takes both days. Medians over the receivers, mm; "old stream" is `_rap_u12_n145` (ARNG section 11).

| users | receivers | h1 | h2 | h3 | h4 | h5 | h6 |
|---|---|---|---|---|---|---|---|
| fixing, both systems, old stream | 28 network | 465.4 | 410.5 | 55.4 | 24.6 | 21.9 | 16.6 |
| fixing, both systems, `p12` | 28 network | 363.3 | 38.6 | 14.4 | 9.8 | 9.7 | 8.6 |
| float, both systems, `p12` | 28 network | 363.3 | 41.8 | 17.0 | 12.1 | 10.2 | 9.1 |
| fixing, Galileo only, `p12` | 28 network | 432.9 | 78.0 | 15.6 | 14.3 | 12.0 | 11.1 |
| float, Galileo only, `p12` | 28 network | 432.9 | 78.0 | 24.5 | 18.6 | 11.7 | 12.6 |
| fixing, both systems, `p12` | 17 on L1C before | 358.7 | 38.2 | 14.6 | 9.4 | 9.3 | 8.8 |
| fixing, Galileo only, `p12` | 17 on L1C before | 435.9 | 92.5 | 23.8 | 14.8 | 12.8 | 11.3 |
| fixing, both systems, `p12` | 11 moved from L1W | 391.5 | 39.0 | 14.3 | 10.1 | 9.8 | 8.3 |
| fixing, Galileo only, `p12` | 11 moved from L1W | 429.8 | 57.8 | 12.0 | 13.8 | 9.2 | 8.1 |
| float, both systems, `p12` | 11 moved from L1W | 391.5 | 44.7 | 17.7 | 12.4 | 10.7 | 8.6 |
| float, Galileo only, `p12` | 11 moved from L1W | 429.8 | 62.6 | 14.5 | 14.6 | 10.6 | 7.9 |
| fixing, both systems, old stream | 11 independent | 632.2 | 657.3 | 73.2 | 33.7 | 21.1 | 13.3 |
| fixing, both systems, `p12` | 11 independent | 493.9 | 54.0 | 15.0 | 13.4 | 11.1 | 8.5 |
| float, both systems, `p12` | 11 independent | 493.9 | 54.0 | 20.7 | 15.8 | 11.9 | 9.6 |

**Holds of the fixing user of both systems.** Galileo L1X/L5X: 38 165 held pair-epochs, 99.5 % equal to the network,
37 397 verified right, none wrong; the wide lane off on 185 (BOGT00COL E18, section 3). GPS L1C/L2W: 54 320 held
(4 226 on the old stream), all equal to the network, 53 368 verified right, none wrong, the wide lane never off; all
28 receivers hold GPS pairs. The Galileo-only fixing user: 38 230 held, 35 768 verified, 7 wrong (LMMF, 0.02 %).

**The float user against the network's whole numbers** (both systems): GPS 75 847 pair-epochs at 28 users (9 469 at
8 users on the old stream), the narrow lane on the network's whole number at 99.7 % in hour 3 and 100 % in hours 4
to 6, mean distance 0.05 cycle in hours 5 and 6; Galileo 99.1 % in hour 3 and 100 % afterwards. Resets per receiver
and hour: Galileo 1543, 312, 19, 2, 13, 8; GPS 1259, 128, 27, 12, 0, 13 (on the old stream GPS 1726, 938, 660, 590,
467, 466).

**The independent receivers** with fixing, mean of hours 4 to 6: QUIN from 351.8 to 19.1 mm, WTZZ 108.2 to 4.4, BADG
58.3 to 8.0, ARHT 46.9 to 5.9, WUH2 44.8 to 12.7 (the five that moved from L1W); UTQI is worse (21.1 to 34.3 mm), not
traced; CHTI 8.0 to 12.2.

**The bars of section 5, DOY 145.**

1. **Galileo not harmed:** float PASS; fixing 2.5 mm above rule L's emulated user in hour 6 (11.1 against 8.6), 0.5 mm
   over the bound, on a lineage at the decision cap of 3 that ties Galileo more slowly in the first 90 minutes (the
   builder's section 65.1; its cap of 12 removes that, section 68.1).
2. **GPS a gain:** PASS at the 17 receivers. At the 11 that moved from L1W the users of both systems are 3.2 mm
   (float) and 2.3 mm (fixing) above the Galileo-only users in hour 3 and equal or better afterwards: over the bound
   in hour 3 by 1.2 and 0.3 mm.
3. **The level reached: PASS** (9.7 and 8.6 mm in hours 5 and 6).
4. **Holds: PASS.**
5. **The independent receivers: PASS** on DOY 145.

**Reading.** GPS on L1C is confirmed at the user: every receiver holds GPS, none wrongly, and a user of both
systems is at 1 cm from hour 4 with or without fixing. The two small misses are in hour 3 and in hour 6 of the
Galileo-only user; both are read again on the final candidate arm.

## 11. 2026-11-16: results of section 9, the adoption test on the final candidate arm

**Runs.** 324 replays on `_rap_f13_n144` and `_rap_f13_n145` (exe nrf28), all exit 0; grades under
`grades_arnd\f13\`. Re-checked here: `f13` differs from the builder's `g13` in the settled flags and the phase counters
only (1 970 and 2 026 flags; no other field of any line). Flagged share by half hour 1 to 6 (`stream_flag_share.py`):
DOY 144 Galileo L1X/L5X 0, 42, 91, 97, 100, 98 %, Galileo L1C/L5Q 0, 53, 73, 77, 75, 77 %, GPS L1C/L2W 0, 83, 100, 98,
100, 98 %; DOY 145 0, 40, 82, 95, 97, 97 %; 1, 56, 83, 83, 83, 83 %; 1, 76, 100, 98, 100, 97 %. Medians over the
receivers, mm.

### 11.1 Positions

| users | receivers | day | h1 | h2 | h3 | h4 | h5 | h6 |
|---|---|---|---|---|---|---|---|---|
| fixing, both systems | 28 network | 144 | 598.1 | 25.7 | 14.7 | 11.4 | 7.9 | 7.3 |
| float, both systems | 28 network | 144 | 598.1 | 25.7 | 17.1 | 12.5 | 9.7 | 8.9 |
| fixing, Galileo only | 28 network | 144 | 506.8 | 68.5 | 22.2 | 16.9 | 10.7 | 10.9 |
| float, Galileo only | 28 network | 144 | 506.8 | 68.5 | 27.4 | 21.0 | 14.8 | 12.4 |
| fixing, both systems, old stream (ARNG 11.1) | 28 network | 144 | 622.0 | 79.2 | 23.1 | 14.8 | 10.8 | 9.7 |
| fixing, both systems | 28 network | 145 | 383.3 | 33.0 | 13.4 | 10.4 | 8.7 | 8.4 |
| float, both systems | 28 network | 145 | 383.3 | 37.4 | 15.2 | 11.2 | 9.3 | 8.7 |
| fixing, Galileo only | 28 network | 145 | 437.3 | 40.8 | 13.6 | 11.3 | 10.0 | 9.9 |
| float, Galileo only | 28 network | 145 | 437.3 | 42.8 | 22.9 | 15.7 | 12.1 | 11.7 |
| fixing, both systems, old stream (ARNG 11.1) | 28 network | 145 | 465.4 | 410.5 | 55.4 | 24.6 | 21.9 | 16.6 |
| fixing, both systems | 17 on L1C before | 144 | 573.8 | 21.6 | 17.6 | 12.1 | 7.5 | 7.3 |
| fixing, both systems | 11 moved from L1W | 144 | 632.1 | 28.0 | 13.9 | 8.7 | 8.2 | 7.3 |
| fixing, Galileo only | 11 moved from L1W | 144 | 507.4 | 48.0 | 17.7 | 8.9 | 9.2 | 12.4 |
| fixing, both systems | 17 on L1C before | 145 | 368.2 | 33.7 | 14.1 | 7.4 | 8.7 | 8.7 |
| fixing, both systems | 11 moved from L1W | 145 | 485.1 | 29.3 | 13.3 | 11.4 | 9.0 | 8.0 |
| fixing, Galileo only | 11 moved from L1W | 145 | 407.8 | 43.9 | 11.2 | 10.3 | 9.3 | 9.1 |
| fixing, both systems | 11 independent | 144 | 451.7 | 52.4 | 16.1 | 10.4 | 11.2 | 9.4 |
| float, both systems | 11 independent | 144 | 451.7 | 52.7 | 21.3 | 14.4 | 11.5 | 9.8 |
| fixing, old stream (ARNG 11.1) | 11 independent | 144 | 685.2 | 137.4 | 40.4 | 19.7 | 18.6 | 16.4 |
| fixing, both systems | 11 independent | 145 | 485.6 | 37.1 | 13.0 | 12.3 | 9.4 | 8.3 |
| float, both systems | 11 independent | 145 | 485.6 | 37.1 | 16.7 | 14.7 | 10.5 | 9.3 |
| fixing, old stream (ARNG 11.1) | 11 independent | 145 | 632.2 | 657.3 | 73.2 | 33.7 | 21.1 | 13.3 |
| fixing, both systems | 7 on Galileo L1C/L5Q | 144 | 385.9 | 21.4 | 15.2 | 11.6 | 9.6 | 7.1 |
| fixing, Galileo only | 7 on Galileo L1C/L5Q | 144 | 538.9 | 41.1 | 21.3 | 18.1 | 18.2 | 13.0 |
| fixing, both systems | 7 on Galileo L1C/L5Q | 145 | 547.6 | 35.1 | 13.0 | 6.7 | 6.1 | 6.8 |
| fixing, Galileo only | 7 on Galileo L1C/L5Q | 145 | 451.7 | 48.6 | 32.8 | 20.3 | 16.4 | 15.0 |

### 11.2 Holds of the fixing users

| label, day, pair | receivers | held pair-epochs | equal to the network | verified right | wrong | wide lane off |
|---|---|---|---|---|---|---|
| both systems, 144, Galileo L1X/L5X | 28 | 48 199 | 100.0 % | 47 255 | 0 | 0 |
| both systems, 144, GPS L1C/L2W | 35 | 73 425 | 100.0 % | 71 949 | 0 | 0 |
| both systems, 144, Galileo L1C/L5Q | 7 | 12 178 | 100.0 % | 10 553 | 0 | 0 |
| both systems, 145, Galileo L1X/L5X | 28 | 35 993 | 99.5 % | 35 076 | 0 | 183, BOGT00COL E18 |
| both systems, 145, GPS L1C/L2W | 35 | 75 700 | 100.0 % | 74 308 | 0 | 0 |
| both systems, 145, Galileo L1C/L5Q | 7 | 10 286 | 100.0 % | 8 465 | 0 | 0 |
| Galileo only, 144, L1X/L5X | 28 | 47 744 | 99.6 % | 45 300 | 0 | 0 |
| Galileo only, 144, L1C/L5Q | 6 | 10 817 | 100.0 % | 9 638 | 0 | 0 |
| Galileo only, 145, L1X/L5X | 28 | 37 840 | 99.5 % | 35 538 | 0 | 183, BOGT00COL E18 |
| Galileo only, 145, L1C/L5Q | 7 | 10 347 | 100.0 % | 7 643 | 0 | 0 |

The fixing user of both systems holds 255 781 pair-epochs over the two days, 247 606 of them verified by the float
sibling, none wrong. The wrong holds of the earlier streams (KRGG00ATF on DOY 144, URUM00CHN on DOY 145) do not
occur.

### 11.3 The bars of section 9

A. The 28 receivers on Galileo L1X/L5X.

1. **The level reached: PASS** (11.4, 7.9 and 7.3 mm; 10.4, 8.7 and 8.4 mm in hours 4 to 6).
2. **GPS a gain: PASS.** The users of both systems are below the Galileo-only users in every hour from 3 to 6 on
   both days, float and fixing. Read apart, the 11 receivers that moved from L1W are above the Galileo-only users on
   DOY 145 by 2.1 mm in hour 3 and 1.1 mm in hour 4 with fixing and by 1.4 mm in hour 4 without, and below
   otherwise.
3. **Fixing against float: PASS** (fixing below float in every hour from 3 to 6).
4. **Holds: PASS.** None wrong; the wide lane off only at the receiver that hosts the satellite's tree edge.
5. **Against the stream of ARNG section 9: PASS** (DOY 144 better in every hour; DOY 145 better by 76, 58, 60 and
   49 % in hours 3 to 6).

B. **The 11 independent receivers: PASS** (11.2 and 9.4 mm, 9.4 and 8.3 mm in hours 5 and 6; better than `u12` in
every hour from 4 to 6 on both days).

C. **The 7 receivers of the second Galileo family: PASS** (9.6 and 7.1 mm, 6.1 and 6.8 mm in hours 5 and 6 with both
systems; none wrong of 10 553 and 8 465 verified holds of the pair L1C/L5Q).

### 11.4 Reads

- **Hour 2** with fixing on both systems: 25.7 and 33.0 mm. The hour-2 cost of rule L on DOY 144 (section 8) is not
  seen on this arm: the Galileo-only users are at 68.5 mm, where rule B's had 70.4.
- **Receivers above 15 mm** (fixing on both systems, mean of hours 4 to 6). Network: LCK4 36 and 37 mm on the two
  days, LMMF 20 and 23, and on DOY 144 URUM 28, CPVG 26, STHL 23; 16 of the 28 are under 10 mm over both days and 25
  under 20 mm. Independent: UTQI 35 and 31 mm, QUIN 24 and 16; 10 of the 11 are under 20 mm over both days.
- **The second Galileo family without GPS** is weaker than the first: Galileo-only fixing users at its receivers
  have 18.2 and 13.0 mm (DOY 144) and 16.4 and 15.0 mm (DOY 145) in hours 5 and 6, against 10.7 and 10.9, 10.0 and
  9.9 mm at the receivers on L1X/L5X. The builder reads that family's flagged levels as moving on DOY 144 (card NRF
  part 8 section 68.1), not yet traced.
- **HKSL00HKG without GPS** is 0.1 to 1.2 m off on both days: its Galileo ambiguities alternate between two values
  from one epoch to the next under constant counters (E02 on L1C: 8.15 and 9.53 m), their variance never falling. It
  is the one Leica receiver of the seven; a matter of the user engine at that receiver, not traced. With GPS it is at
  8 and 20 mm.

### 11.5 Decision

Every bar of section 9 passes. The package is ordered adopted on the builder's branch in the chain posture with
fixing, with the three-part note (done 2026-11-16; verified by these replays: two days, 46 receivers, 255 781 held
pair-epochs with none wrong; to do: more days and receivers, a network that is already warm, the second Galileo
family's levels on DOY 144, HKSL00HKG without GPS): `network.ring_fix.settled_flags` with `settled_rule` "largest"
and `settled_wait_emissions` 10, `network.ring_fix.unsettled_steps` and `unsettled_steps_unheld`, Galileo L1C/L5Q
held beside L1X/L5X, and GPS band 1 on L1C in the network and at the users, with the settings of the fixing users
(partial fixing off, the flag required, the tied pair first). The goldens of the main posture are not changed.

## 12. 2026-11-16: users that join a network that is already running (pre-registered)

**Why.** Every user of sections 9 to 11 starts with the network, so its first two hours are also the network's. In
operation the network runs continuously and a user joins it later; that case is the first item of section 11.5's
list. On the earlier stream a late start with both systems failed (ARNG section 8: 62, 30 and 24 mm in hours 4 to 6
against 22, 12 and 12 mm for Galileo alone), because unflagged GPS levels walked.

**User runs.** The streams of section 9 (`_rap_f13_n144`, `_rap_f13_n145`) and the settings of its users of both
systems, with `network.start_offset_hours` 2.004167 (labels `nH2f`, `nH2x`: float, fixing; 4 h) and 3.004167 (`nH3f`,
`nH3x`; 3 h) at the 28 network receivers on Galileo L1X/L5X, and `u12h2`, `u12h3` (float and fixing) at the 11
independent receivers; both days.

**Bars** (medians over the receivers; the user's own hours, counted from its start).

1. **The level by hour, fixing on both systems, 28 receivers:** at most 60 mm in the user's first hour, 15 mm in
   its second, 12 mm in its third and later hours, for both starts on both days (hourly 3D medians).
2. **Against the users started with the network** (`nFBx`, section 11.1): from the late user's second hour on, not
   worse by more than 3 mm in the same hours of the day.
3. **Holds:** wrong holds of kind (a) on at most 0.5 % of the verified holds of each fixing label and day (Galileo
   pairs and GPS pairs apart); the wide lane off only at a receiver that hosts the satellite's tree edge.
4. **The independent receivers:** fixing at most 80 mm in the first hour and 20 mm in the second and later hours.

**Reads** (from the per-epoch traces, `user_convergence.py`): the median 3D error 5, 10, 15, 20, 30 and 45 minutes
after the start; the time after which the error stays under 10, 5 and 3 cm for the following 10 minutes (median and
90th percentile over the receivers); the time of the first fix.

**What follows.** The numbers are the convergence of a user on a running network, the figure a description of the
service would quote. A failed bar 1 or 3 reopens the "to do" of the adoption note before any wider use.

## 13. 2026-11-16: GPS-only users on the final candidate arm (pre-registered)

**Why.** Sections 9 to 11 grade users of Galileo alone and of both systems; no user of GPS alone has been run. With
GPS on L1C the network ties 86 to 89 % of the GPS links, so GPS alone should carry a user.

**User runs.** `nFGf`, `nFGx` (float, fixing): the folders of section 9's users of both systems with
`processing.active_systems` ["GPS"], at the 35 network receivers (28 + 7), both days, on `_rap_f13_n144` and
`_rap_f13_n145`.

**Bars** (medians over the 35 receivers of the hourly 3D median). Fixing at most 25 mm in hour 3 and 20 mm in each
of hours 4 to 6, on both days; fixing not worse than float by more than 2 mm in any of hours 3 to 6; wrong holds of
kind (a) on at most 0.5 % of the verified holds, the wide lane never off. Read: GPS alone against Galileo alone and
against both systems.

## 14. 2026-11-16: results of section 13, GPS-only users on the final candidate arm

**Runs.** 140 replays (`nFGf`, `nFGx`; 35 network receivers; both days), all exit 0; grades under `grades_arnd\gps\`.
Medians over the receivers, mm.

| users | receivers | day | h1 | h2 | h3 | h4 | h5 | h6 |
|---|---|---|---|---|---|---|---|---|
| fixing, GPS only | 35 network | 144 | 774.7 | 36.6 | 17.6 | 13.8 | 11.2 | 8.7 |
| float, GPS only | 35 network | 144 | 774.7 | 37.8 | 21.4 | 15.8 | 11.4 | 10.6 |
| fixing, both systems | 35 network | 144 | 533.0 | 23.4 | 15.2 | 11.6 | 8.2 | 7.3 |
| fixing, GPS only | 35 network | 145 | 567.2 | 48.3 | 19.5 | 17.4 | 13.1 | 12.8 |
| float, GPS only | 35 network | 145 | 567.2 | 51.9 | 21.5 | 18.8 | 13.4 | 13.7 |
| fixing, both systems | 35 network | 145 | 398.4 | 33.7 | 13.3 | 10.1 | 8.7 | 8.2 |
| fixing, GPS only | 28 on Galileo L1X/L5X | 144 | 793.5 | 36.1 | 18.0 | 13.8 | 12.8 | 11.9 |
| fixing, Galileo only | 28 on Galileo L1X/L5X | 144 | 506.8 | 68.5 | 22.2 | 16.9 | 10.7 | 10.9 |
| fixing, GPS only | 28 on Galileo L1X/L5X | 145 | 557.9 | 53.4 | 22.0 | 17.9 | 13.2 | 13.0 |
| fixing, Galileo only | 28 on Galileo L1X/L5X | 145 | 437.3 | 40.8 | 13.6 | 11.3 | 10.0 | 9.9 |

**Holds of the GPS-only fixing user** (pair L1C/L2W): DOY 144 71 493 held pair-epochs, all equal to the network,
69 704 verified right, none wrong; DOY 145 68 948 held, 66 924 verified right, none wrong; the wide lane never off.

**Bars of section 13: PASS.** Fixing 17.6, 13.8, 11.2 and 8.7 mm and 19.5, 17.4, 13.1 and 12.8 mm in hours 3 to 6
(bounds 25 and 20); fixing below float in every hour from 3 to 6; no wrong hold.

**Read.** GPS alone carries a user: between 9 and 20 mm in hours 3 to 6, with fixing. It is ahead of Galileo alone
in hours 2 to 4 of DOY 144 and behind it on DOY 145 by 3 to 8 mm; both systems together are better than either on
both days from hour 2.

## 15. 2026-11-16: users at every receiver of the second Galileo family, a read of their positions (pre-registered)

**Why.** The builder has ruled out the wet delay, the borrowed E5a antenna pattern and weak ties as the cause of the
L1C/L5Q family's moving flagged levels on DOY 144 (card NRF part 9 sections 75 to 75.2): the movement is per
satellite, of mixed sign, follows the elevation at the receivers that tie the satellite, and is largest for the
satellites tied by the most held links. A receiver whose a priori coordinate (or antenna height) is off by some
centimetres puts exactly such a pattern into every satellite it ties. A user at that receiver shows it directly: its
converged position sits off the a priori coordinate by the same vector on every day.

**Runs.** Users of both systems (`nFBf`, `nFBx`, the settings of section 9) at the 20 receivers of the family not run
there, both days, on `_rap_f13_n144` and `_rap_f13_n145`; with section 9's seven, all 27 receivers of the family.

**The read** (`user_enu.py`: the mean east, north and up error of the fixing user over hours 4 to 6, per receiver
and day). A receiver is a suspect when its mean 3D offset exceeds 30 mm on both days and the two days' vectors agree
within 15 mm. For comparison the same read at the 28 receivers on L1X/L5X (section 9's runs). If suspects are found
they are named to the builder for a network run without them; if none, a coordinate error is ruled out too.

## 16. 2026-11-16: results of section 12, users that join a network that is already running

**Runs.** 312 replays on `_rap_f13_n144` and `_rap_f13_n145` (starts 2 h and 3 h after the network's, both days), all
exit 0; grades under `grades_arnd\warm\`. Fixing on both systems; medians over the receivers.

| start, day | receivers | error at 5 / 15 / 30 / 60 min (mm) | under 10 cm from (median; 90th pct, min) | under 5 cm from | under 3 cm from | first fix (min) | hourly, own hours 1 to 4 (mm) |
|---|---|---|---|---|---|---|---|
| 2 h, 144 | 28 network | 184 / 58 / 52 / 14 | 11.8; 27.0 | 36.5; 55.5 | 42.2; 85.5 | 43.0 | 50.0, 11.7, 8.9, 7.7 |
| 2 h, 145 | 28 network | 176 / 56 / 41 / 19 | 12.2; 28.5 | 29.5; 42.0 | 39.2; 76.5 | 33.0 | 42.2, 13.8, 12.2, 10.3 |
| 3 h, 144 | 28 network | 174 / 115 / 37 / 15 | 16.5; 37.5 | 30.0; 65.0 | 44.5; 108.5 | 46.0 | 47.2, 12.4, 10.4 |
| 3 h, 145 | 28 network | 164 / 75 / 26 / 16 | 13.2; 22.5 | 21.2; 53.5 | 40.8; 79.0 | 37.0 | 35.8, 13.3, 9.4 |
| 2 h, 144 | 11 independent | 168 / 96 / 53 / 22 | 20.5; 26.5 | 37.5; 42.5 | 41.0; 70.0 | 52.0 | 52.9, 12.1, 10.0, 10.7 |
| 2 h, 145 | 11 independent | 183 / 61 / 28 / 14 | 13.0; 23.5 | 25.0; 39.5 | 35.0; 52.0 | 33.5 | 30.9, 11.6, 12.6, 8.5 |
| 3 h, 144 | 11 independent | 235 / 95 / 37 / 16 | 16.5; 25.5 | 28.5; 61.0 | 37.5; 131.0 | 27.8 | 37.3, 17.3, 19.5 |
| 3 h, 145 | 11 independent | 160 / 131 / 40 / 23 | 16.5; 23.5 | 22.5; 52.5 | 54.0; 82.5 | 51.5 | 58.8, 19.9, 10.4 |

"Under X from" is the time after which the 3D error stays under X for the following 10 minutes. Every receiver of
every row gets under 10 cm and under 5 cm. The float siblings reach 10 cm at the same times and 5 cm within 7 minutes
of them (the first fix mostly comes later) and are 3 to 11 mm above the fixing users at 45 and 60 minutes.

**Holds** of the four fixing labels at the network receivers: 241 089 held pair-epochs (Galileo L1X/L5X 100 235, GPS
L1C/L2W 140 854), 230 837 verified by the float sibling, none wrong. The wide lane is off twice, each time on a
satellite at the receiver that hosts its tree edge (section 3): BOGT00COL E18 on DOY 145 (183 pair-epochs) and
KOKV00USA E14 on DOY 144 (59; host from TOW 12 930 by a re-host that carries the link's value, not certified), the
second with one narrow-lane cycle beside it, the whole-cycle wind-up difference of an arc started later than the
network's (card ARNF). E14 and E18 are the two satellites on eccentric orbits.

**Bars of section 12.**

1. **The level by hour: FAIL by 0.2 mm in one cell of twelve** (the third hour of the 2 h start on DOY 145: 12.2 mm
   against 12); the first hour is 36 to 50 mm (bound 60) and the second 12 to 14 mm (bound 15).
2. **Against the users started with the network: FAIL.** From its second hour a late user is within 5 mm of the
   early one in the same hour of the day (above it by 0.3 to 4.6 mm; the bound of 3 mm is exceeded in five cells of
   ten by 0.1 to 1.6 mm). The bound asked a user one to two hours old to equal one four to five hours old.
3. **Holds: PASS.**
4. **The independent receivers: PASS** (31 to 59 mm in the first hour, 10 to 20 mm afterwards).

**Reading.** A user that joins the running network stays under 10 cm after 12 to 21 minutes and under 5 cm after
21 to 38 minutes (medians), reaches 12 to 14 mm in its second hour and about 1 cm afterwards, and holds no wrong
integer. The first fix comes after 28 to 52 minutes, later than the float solution's own convergence, so the fixing
adds a few millimetres at this stage and not speed: the user estimates its own slant ionosphere with no help from
the network, and that sets the time. The two failed bars are of the level, by under 2 mm; they do not reopen the
adoption note.

## 17. 2026-11-16: results of section 15, users at every receiver of the second Galileo family

**Runs.** 80 replays (the 20 further receivers, float and fixing, both days), all exit 0; with section 9's, all 27
receivers of the L1C/L5Q family; grades under `grades_arnd\cq\`.

**Positions** (fixing on both systems, medians over the 27 receivers, hours 1 to 6, mm): DOY 144 375.2, 35.5, 17.1,
10.6, 7.6, 6.5; DOY 145 381.0, 27.5, 10.5, 9.0, 8.1, 8.5. **Holds** of the pair L1C/L5Q: 36 629 and 32 300 held
pair-epochs, all equal to the network, 32 808 and 25 134 verified by the float sibling, none wrong.

**The read of section 15** (`user_enu.py`, mean east, north and up error over hours 4 to 6): no receiver of the
family is a suspect. The largest mean offsets are GUAM 20.3 and 25.4 mm (the two days 44.6 mm apart, the height of
opposite sign: not a coordinate), HKSL 7.0 and 20.2, KIT3 10.0 and 18.9, STJO 18.0 and 10.7; the receivers whose two
days agree within 5 mm (SGOC, DJIG, PARK, WTZS, WTZR, GOP6) sit 3 to 12 mm from their coordinate. The same read at
the 28 receivers on L1X/L5X names one: LCK400IND, 35.6 and 36.0 mm, the two days 0.7 mm apart, all of it in height
(-35.3 and -35.7 mm).

**Reading.** A coordinate or antenna-height error at a receiver of the family is ruled out as the cause of its
moving flagged levels, with the wet delay, the borrowed E5a pattern and weak ties (the builder's sections 75 to
75.2); the cause stays open. At the user the family is sound with both systems: every one of its 27 receivers is
within 26 mm of its coordinate over hours 4 to 6 on both days, and the pair holds no wrong integer.

**LCK400IND.** Its reference is not at fault: the coordinate is the mean of the IGS daily solutions of DOY 143 to 147
as for every receiver of these runs, the IGS solution of DOY 144 carries the same antenna (JAVGRANT_G5TLBI NONE) and
the same eccentricity (0.1230 m up) as the observation file, and `igs20.atx` holds that antenna with every band. The
network's own station congruence puts the receiver within 3 mm of its coordinate on both days, as it does every
receiver: with the coordinate held, a height error goes into the wet delay and the clock. The user's solution there
is 35 mm low against the IGS coordinate on both days; not traced.
