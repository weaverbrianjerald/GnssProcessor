# ARNF: users on the stream with network integer fixing, and the stream read without a user

**STATUS**

- **id:** ARNF
- **topic:** user-ar
- **status:** open
- **summary:** Continuation of card ARNE (that file neared the 60 KB bar). Card ARNE holds the holding runs on observed data (stage O2), the vetting of the bad-epoch response (stage E1) and the re-grade of the formal-sigma pairs. This card takes the results from the afternoon of 2026-11-15 on: the user replays on the streams made with the response, a read of the stream itself that needs no user, the replays of users restricted to the held family, the re-grade of the second day (DOY 145) and the vetting of the builder's orders 1 and 2.
- **dates:** 2026-11-15 (opened as the continuation)
- **knobs / posture:** none new here. The builder's keys `network.ring_fix.*`, `network.amb_float_drop_jump_k`, `network.amb_float_drop_gap_cover` and `network.zwd_event_inflate_m` live on its branch (the last two RECOMMEND_SCOPED there since commit fb4139c).
- **code:** none. New analysis tools: `stream_stab.py`, `stream_jumps.py`, `o2_audit_mode.py`, `nw_offsets_time.py`, `users_hourly.py`, `users_gal_only.py`.
- **run dirs:** `D:\GnssProcessorSupport\runs_nom\_rap_o2*_n144*`, `_rap_o5*_n145*` (the builder's network runs) and `_rap_nw_{obs,fxb}*_144_*` (the user replays of this session); tools and grade outputs under `D:\GnssProcessorSupport\presentation\agu26\fixing_control\` (`tools\`, `grades_arnd\o2\`, `grades_arnd\o5\`).
- **source:** card [ARNE](../../record/cards/user-ar/ARNE-NETWORK-INTEGER-FIXING-CONTINUED.md), in particular its sections 4.1 and 6; cards NRF (part 5) and BER on the builder's branch `irdu/user-integers`

**Outcome.** Open; continued in card [ARNG](ARNG-USERS-ON-THE-DECLARED-STREAM.md) (the user replays on the stream that declares its steps and flags settled signals). With network integer fixing at the formal sigma and the bad-epoch response, the stream of the
held family (Galileo L1X/L5X) takes no fractional and no whole-cycle step from hour 1 on, at any depth, and users
restricted to that family read the between-satellite N_1 flat with depth (0.11) and stand at 3 to 4 cm. The families
not held step by about one narrow-lane cycle up to 18 times per satellite-hour with no discontinuity signal: that,
not the wet delays, kept users of both systems from integers. DOY 145's one departure followed a release at a
re-host (sections 8 and 11); with the builder's remedy, which keeps the carries at a re-host onto a held link, the
day reads zero (section 15); as first built it held wrong whole numbers on DOY 144 (section 18), and its narrower
form reads zero on both days, both families and two starts (section 19). A user at a network receiver matches
the network's whole numbers (section 9), and
its own fixing is right 99.9 % of the time with partial fixing off (section 13); independent receivers are open.

> Results are appended TO THIS FILE, below the text.

---

## 1. Scope

Sections of card ARNE are cited by their numbers there. "Depth" is the larger depth of two satellites in the chain's
tree (1 = tracked by the reference receiver). "N_1" is the between-satellite narrow-lane offset fitted over every
observer of a signal group (`nw_depth_offsets.py`, card ARND section 4): two unrelated offsets give a mean of 0.250
and 20 % within 0.10.

## 2. 2026-11-15: users on the two streams made with the bad-epoch response (the test of ARNE section 4.1)

Streams: `_rap_o2gj_n144` (Galileo L1X/L5X and GPS L1C/L2W held at twice the formal sigma, response on) and
`_rap_o2s1j_n144` (Galileo L1X/L5X held at the formal sigma, response on). Per stream 73 observers (the network's 56
receivers and the 17 test receivers) and 17 fixing users; 180 replays, every exit code 0. Comparison: the stream
without fixing (`_rap_nwa1nh_n144`) and the stream held at twice the sigma without the response (`_rap_o2g_n144`),
both from ARNE section 4.1.

**N_1 between satellites, Galileo L1X/L5X, from hour 3** (mean distance from a whole number, share within 0.10):

| stream | depth 1 | depth 3 | depth 5 | depth 7 and more |
| --- | --- | --- | --- | --- |
| no fixing | 0.143, 45.9 % | 0.234, 24.5 % | 0.267, 13.8 % | 0.243, 14.0 % |
| twice the sigma, no response | 0.105, 71.0 % | 0.197, 33.9 % | 0.224, 22.1 % | 0.198, 31.5 % |
| twice the sigma, response | 0.121, 61.1 % | 0.195, 32.5 % | 0.228, 25.4 % | 0.234, 22.7 % |
| formal sigma, response | 0.074, 70.2 % | 0.159, 46.4 % | 0.215, 29.2 % | 0.263, 21.8 % |

The wide lane of the same pairs: 0.051, 0.101, 0.141, 0.106 without fixing; 0.020, 0.040, 0.042, 0.043 at the formal
sigma with the response. GPS L1C/L2W held at twice the sigma keeps its wide lane only near the root (0.049, 0.135,
0.224, 0.220) and its N_1 at the null.

**Positions, median over the 17 test receivers of the hourly 3D median (mm, hours 1 to 6):**

| stream | float users | fixing users |
| --- | --- | --- |
| no fixing | 474 225 149 110 113 92 | 480 287 226 178 148 102 |
| twice the sigma, no response | 479 234 133 84 79 62 | 494 248 196 173 159 136 |
| twice the sigma, response | 360 190 125 97 83 67 | 380 298 239 178 140 121 |
| formal sigma, response | 398 198 178 96 86 59 | 395 285 210 171 122 109 |

Wide-lane pair-epochs held by the fixing users, median over the 11 receivers of the held pair: 9.8, 18.3, 16.1 and
25.7 % in the order of the table.

**Reading.** The response shortens the first two hours of a float user (474 and 225 mm become 360 to 398 and 190 to
198 mm). It does not move N_1: the two streams held at twice the sigma read alike (0.197 and 0.195 at depth 3). The
formal sigma, with more links held, gains depth 1 and depth 3 only. The first suspect of ARNE section 4.1 (the
network's wet delays, reset by slips) is therefore not what carries the N_1 error at depth; the prediction made
there failed.

## 3. 2026-11-15: the stream read without a user (tools `stream_stab.py`, `stream_jumps.py`)

The clock record of the stream is a correction to the a priori 30 s clock product (the `S` record names it). The
ionosphere-free phase level of a satellite, in narrow-lane cycles, `nl = (f1 E_1 - f2 E_2) / (f1 - f2)` with
`E_b = (C - P_b + mu_b x) / lambda_b` (card IRD section 8 on the emitted records; the code term `x` cancels in
`nl`), is then constant for every satellite pair of a stream whose phase level is steady. A float user holds one
constant per arc, so the movement of `nl(a) - nl(b)` in time is what it feels. No user and no truth enter.

**Range of the pair difference inside a half hour** (p50 / p90, cycles; share of pair-windows at most 0.10), Galileo
L1X/L5X, hours 3 to 6:

| stream | depth 1 | depth 3 | depth 5 | depth 7 and more |
| --- | --- | --- | --- | --- |
| no fixing, response (`o2offj`) | 0.080 / 0.177, 70.8 % | 0.132 / 2.111, 41.0 % | 0.531 / 3.061, 17.1 % | 1.036 / 2.804, 0 % |
| twice the sigma (`o2g`) | 0.072 / 0.126, 82.1 % | 0.075 / 0.137, 71.6 % | 0.085 / 0.179, 62.1 % | 0.123 / 4.453, 40.6 % |
| twice the sigma, response (`o2gj`) | 0.071 / 0.127, 83.3 % | 0.075 / 0.135, 72.7 % | 0.100 / 2.250, 49.9 % | 0.106 / 1.929, 46.5 % |
| formal sigma, response (`o2s1j`) | 0.064 / 0.109, 87.6 % | 0.065 / 0.104, 87.8 % | 0.070 / 0.141, 75.2 % | 0.066 / 0.110, 86.8 % |

GPS L1C/L2W, the same read: not held (`o2offj`, `o2s1j`) 0.106 / 7.142, 3.661 / 9.222, 6.155 / 11.721,
6.332 / 10.914; held at twice the sigma (`o2gj`) 0.079 / 0.176, 0.235 / 6.406, 3.298 / 8.465, 5.750 / 15.146.

**Steps between consecutive epochs, per satellite-hour** (the satellite's own step, the constellation's median
removed; tolerance 0.15 cycle). "Whole": both signals move by whole cycles (a re-pin). "Ionosphere-like": the
ionosphere-free part stays under the tolerance (a move of the level that only code holds). "Fraction": the
ionosphere-free level moves by part of a cycle. Hours 3 to 6, by depth 1 / 3 / 5 / 7 and more:

| stream and family | whole | ionosphere-like | fraction |
| --- | --- | --- | --- |
| Galileo L1X/L5X, no fixing | 0.00 / 0.06 / 0.00 / 0.00 | 0.52 / 4.18 / 8.66 / 22.67 | 0.04 / 0.62 / 1.35 / 1.33 |
| Galileo L1X/L5X, formal sigma with the response | 0 at every depth | 0.37 / 1.47 / 1.64 / 0.98 | 0 at every depth |
| GPS L1C/L2W, not held | 0.00 / 0.16 / 0.33 / 0.41 | 0.91 / 1.26 / 1.89 / 0.41 | 0.32 / 4.92 / 8.13 / 17.67 |
| GPS L1C/L2W, held at twice the sigma | 0.00 / 0.07 / 0.22 / 0.48 | 0.37 / 1.77 / 2.15 / 1.38 | 0.09 / 0.90 / 6.15 / 14.03 |

A fraction step is about one narrow-lane cycle in size (p50 0.9 to 1.2, p90 2.7 to 4.2) and its wrapped part is
uniform (p50 0.21 to 0.26). The held Galileo stream hour by hour: in hour 0 it takes 5.6 to 9.0 fraction steps and
0.3 to 1.1 whole steps per satellite-hour; from hour 1 to hour 6 none of either at any depth (144 satellite-hours),
and its largest ionosphere-free move between two epochs is 0.088 cycle (E03 at TOW 13320).

**The stream carries no discontinuity counter in this posture:** the `P` records have neither the optional
`phase_disc` field nor `gen`. A user cannot tell a step from data.

**Reading.**

1. Where the network holds, the stream is steady: no step of any kind from hour 1, at every depth. The fixing with
   held-first re-attachment removes the re-pin steps of the chain, which is what the ring-by-ring proposal was for.
2. A family that is not held is re-pinned by rounding the new tree link's float at every re-host, and the
   satellite's level moves by the float's fraction. GPS takes 5 to 18 such steps per satellite-hour at depth 3 and
   beyond, each about 10 cm, unsignalled.
3. Standing invariant 7 applies: a re-pin is an event at both edges, and its user-facing edge is missing.

## 4. 2026-11-15: users of the held family alone (Galileo-only replays)

The same replays with `processing.active_systems` set to Galileo (`users_gal_only.py`; a 900 s smoke passed): on
`_rap_o2s1j_n144` 73 observers and 17 fixing users, on the stream without fixing (`_rap_o2offj_n144`, response on) 73
observers; 163 replays, every exit code 0.

**N_1 between satellites, Galileo L1X/L5X, from hour 3:**

| users | stream | depth 1 | depth 3 | depth 5 | depth 7 and more |
| --- | --- | --- | --- | --- | --- |
| GPS and Galileo | formal sigma, response | 0.074, 70.2 % | 0.159, 46.4 % | 0.215, 29.2 % | 0.263, 21.8 % |
| Galileo only | formal sigma, response | 0.106, 73.0 % | 0.113, 58.0 % | 0.117, 55.8 % | 0.193, 30.9 % (110 pair-bins) |
| Galileo only | no fixing, response | 0.102, 58.4 % | 0.184, 33.0 % | 0.209, 29.0 % | 0.279, 8.8 % |

Over the six half-hour bins from hour 3 (`nw_offsets_time.py`, 435 pairs seen in at least 4 bins) the mean N_1
offset of a pair is p50 0.062 and p90 0.164 cycle for the Galileo-only users on the held stream (71.5 % of the pairs
within 0.10; spread between bins p50 0.131), against 0.129 and 0.370 (40.7 %) on the stream without fixing and 0.117
and 0.340 (44.1 %) with GPS in the users. The six largest are all pairs of E14 (0.36 to 0.49).

**Float users, median over the 17 test receivers of the hourly 3D median (mm):**

| users | stream | hours 1 to 6 | last hour under 5 cm |
| --- | --- | --- | --- |
| GPS and Galileo | formal sigma, response | 398 198 178 96 86 59 | 7 of 17 |
| Galileo only | formal sigma, response | 386 137 79 42 35 34 | 11 of 17 |
| Galileo only | no fixing, response | 379 137 100 53 49 40 | 10 of 17 |

Per receiver, mean of hours 4 to 6, Galileo only against both systems on the held stream: WTZZ 16.7 against 116.3
mm, BADG 15.4 against 121.7, CHTI 22.2 against 79.1, ARHT 19.5 against 32.5, STPM 19.6 against 26.5. SCRZ stands at
1 m with Galileo alone on either stream (a fault of that receiver's Galileo data, not of the stream).

**Fixing users, Galileo only, on the held stream.** Positions 386 131 93 60 50 53 mm (both systems: 395 285 210 171
122 109), 13 of 17 receivers better. Wide-lane pair-epochs held, median over the 11 receivers of the held pair:
51.4 % (25.7 % with both systems, 9.8 % without network fixing). WTZZ reaches 24, 8 and 6 mm in hours 4 to 6 and
holds its between-satellite L1 ambiguities at whole numbers (the resolver's `CLFRAC` lines: 0.0002 to 0.0005 cycle).
Two cautions:

- The user trace's `ar_success` (0 to 6 % of the epochs) is not the share of epochs with integers held. Once
  ambiguities are held, the user's narrow-lane search is re-run on the held values, finds a ratio above its upper
  bound of 1e4 (40 to 86 % of the searches at nine receivers) and is logged as rejected. A held N_1 share needs a
  count of its own.
- The fixing user is not reliably better than the float user: better at ASCG (20 against 40 mm, mean of hours 4 to
  6), CIBG (49 against 105), KOKB (48 against 81) and WTZZ (13 against 17), worse at BADG (89 against 15), QUIN (158
  against 34), STPM (62 against 20) and UTQI (67 against 39). Wrong holds at the user remain; the user engine is now
  the limit on this family, not the stream.

**Reading.** With the user restricted to the family the network holds, the N_1 read no longer grows with depth:
depth 3 and depth 5 stand at the level of the pairs the reference receiver itself tracks. The users' depth problem
of ARNE section 4.1 was the families that are not held, mixed into the user's solution: their unsignalled steps
(section 3) bias the float position, and through it every ambiguity. The remaining 0.11 is common to all depths:
model differences between receivers and the noise of a Galileo-only float user (the simulator's same read is 0.01).

## 5. 2026-11-15: the second day (DOY 145) re-graded

The builder's arms of card NRF part 5 section 30 (exe nrf14): Galileo L1X/L5X held at the formal sigma with the full
response (`network.amb_float_drop_jump_k` 1, gap cover free, wet-delay inflate 0.02): `_rap_o5s1j_n145` (default
reference receiver), `_rap_o5s1j_n145r` (GOPE00CZE), `_rap_o5s1j_n145b` (default, start 1 h later); GPS L1C/L2W held
beside it: `_rap_o5gj_n145[r]`; key-off runs `_rap_o5offj_n145[r,b]`. Audits of this session:

| audit | checks | failing |
| --- | --- | --- |
| two starts, wind-up branches removed, Galileo | L1X 45 430, L5X 44 745 | 0, 0 |
| two starts, arcs born together, Galileo | L1X 9 258, L5X 8 188 | 0, 0 |
| one start, two reference receivers, GPS L1C/L2W | L1C 45 426, L2W 45 367 | 0, 0 |
| one start, two reference receivers, Galileo | L1X 67 667, L5X 67 675 | 428, 476 (fourth hour; closures 3 on L1X, 4 on L5X) |

Held share of the default run by quarter hour: 0 2 19 65 78 80 84 83 87 91 86 86 91 90 84 76 71 73 75 71 68 67 65
66 %; 317 decisions; releases: gap 104, slip 23, monitor 1; 1 alarm.

**Attribution that does not depend on the walk** (`o2_audit_mode.py`: one number per receiver and satellite by
majority, then the arcs that depart): one arc pair, 68 epochs per band, TOW 97260 to 99270: BOGT00COL-E18 against
STHL00GBR-E18, closure (3, 4) on (L1X, L5X).

1. (3, 4) is the Galileo E1/E5a pair that nearly keeps the ionosphere-free phase (3 f1 is close to 4 f5): one
   wide-lane cycle with N_1 moved by 3, the ionosphere-free phase 5.6 mm off, the ionosphere 0.565 m on L1.
2. BOGT00COL-E18 is a tree pin in both runs, set at TOW 87330 (15.5 min after the cold start, row
   `RehostUncertified`) at (-6, 17) and at (23, 42). The key-off runs carry the same pins: they are the chain's own.
3. STHL00GBR-E18 (born at TOW 94890) is held at 96420 in the default run and at 97260 in the GOPE00CZE run. Each
   decision sits inside its lane of that run's floats (wide-lane fraction 0.11 and 0.17; per-signal offsets
   ionosphere-like, 7 to 8 cm against a lane spacing of 56 cm). The departure begins when the second run holds the
   arc and closes the loop.
4. Block-Baarda flags the block (BOGT00COL, E18) in all four runs, holding or not, from TOW 92100.

**Reading.** One of the two start-up pins on BOGT00COL-E18 was rounded onto the neighbouring (3, 4) lane: 15 minutes
after a cold start the ionosphere-like uncertainty of a float is comparable to half the lane spacing. It is not a
decision of the gate, and twice the sigma would hide it (the loop does not close) without removing it. A pin on the
neighbouring lane costs a user of that satellite 5.6 mm of ionosphere-free phase. The pre-registered bar (zero
failing checks) nevertheless fails by the letter.

## 6. 2026-11-15: the builder's orders 1 and 2 vetted

- **Order 1, the gap-return cover** (card BER section 13, commit 65d4086; adopted in fb4139c with the wet-delay
  inflate, both RECOMMEND_SCOPED in the regime of the chain posture with fixing). The population read of this session
  on the builder's runs `o2jgf_n144[r]` and `o2jgfz_n144[r]` (TOW 1800 to 9000): resets whose kept value is more than
  3 restored sigmas off: slips between consecutive epochs 0 / 0, returns after a gap 0 / 0 (92 / 94 returns, 88 / 90
  covered); it was 9 / 11 for the returns (ARNE section 6.2). The free form (a band declared shifted takes 1e4 m^2)
  is a refloat, not a stiff re-seed. Accepted.
- **BSHM00ISR** carries every remaining wet-delay step of 2 cm or more (1 / 13, up to -12.3 cm at TOW 3060 with
  GOPE00CZE): its local block is reported not positive definite at every epoch. Ordered: the cause stated in the
  builder's card, and the receiver left out of the next observed-data arms.
- **Order 2, the slip size** (card BER section 16): with the response no chord slip of any size moves another
  satellite's ionosphere-free level by more than about 1 mm; the 4-cycle slip on a settled arc is the peak (1.03 mm),
  as ARNE section 6.3 predicted. A 288-cycle slip on a tree link or a held link moves Galileo's ionosphere-free level
  by 3.2 to 3.3 mm and other receivers' wet delays by 4.6 to 4.9 mm, through the cut release and the hold release.

## 7. 2026-11-15: decisions and orders (open to review)

1. `network.ring_fix.sigma_scale` is not adopted in the ledger: bar 1 of the second day fails by the letter, and the
   author's condition was that the day passes. The judgment is put to the author with the recommendation to adopt
   and to write the exception into the note. The working setting of every later observed-data run stays the formal
   sigma with the full response (a run setting, not a ledger change).
2. `network.amb_float_drop_jump_k` is to be adopted in the scoped class under the author's rule of 2026-11-15: it
   passed its own bars (the simulator's event beds and the two observed hours) and does not depend on the second day.
3. Ordered from the builder, in this order: (a) the pin lane: confirm the attribution of section 5, count the pins
   set from floats against the lane spacing of each family, and test the narrowest remedy on a simulator bed with one
   pin forced onto the neighbouring lane; (b) the stream signals its steps: a discontinuity counter per satellite
   signal at every re-pin whose step is not zero, checked with `stream_jumps.py`; (c) GPS beside Galileo on DOY 144
   at the formal sigma with the full response.
4. Open on the user side, for this session: a count of the user's held N_1 that does not rest on `ar_success`, and
   the wrong holds of the fixing user at four receivers (a comparison of a network receiver's user integers with the
   network's own held integers on the same links is the available truth).

## 8. 2026-11-15: CORRECTION to sections 5 and 7: the second day's departure follows a move of the satellite's code reference

The reading of section 5 (a start-up pin rounded onto the neighbouring lane) is wrong, and item 3(a) of section 7
(the pin lane) is withdrawn. Read after section 5 was written, on the key-off runs `_rap_o5offj_n145[r]` and the
holding runs alike:

1. **The pins were consistent.** The slant ionosphere of BOGT00COL-E18 (`network_ionosphere.csv`, `iono_total_m`)
   agrees between the two reference receivers to 5 mm from TOW 87600 to 93600 (12.403 / 12.399, 9.162 / 9.175,
   7.302 / 7.302, 6.076 / 6.075, 5.195 / 5.192, 4.646 / 4.641 m).
2. **The default run then departs, with or without fixing.** Difference default minus GOPE00CZE: 0.05 m at TOW 94710,
   0.22 at 94830, 0.32 at 94890, 0.42 at 94950, 0.72 at 96000, 0.58 at 97200, 0.51 at 98400; equal again at 103200
   (4.784 / 4.752). The default run's series jumps (4.687 at 94800, 5.498 at 96000); GOPE00CZE's stays smooth
   (4.586, 4.778, 4.876, 5.011).
3. **At TOW 94830 both runs move E18's code reference:** `CLK_BASIS reselect E18 if_pin_reference_moved
   C1C+C5Q->C1X+C5X`, `CB_DATUM reseed_disc_landed E18` (`code_datum_time_linked_reseed`), `TOPOLOGY col_added` for
   `CB:E18:C1X` and `CB:E18:C5X`. The stream carries code-bias records for E18 C1X and C5X only from about TOW 94830
   to about 99300, the window that contains the departure (97260 to 99270). The emitted values differ by 1.5 to 2.6 cm
   between the runs (1.099 against 1.084 to 1.073 m), so the 0.5 to 0.7 m is not in the emitted code bias itself.
4. **The link was decided inside the window.** STHL00GBR-E18 was born at TOW 94890, one minute after the move, and
   held at 96420 (default) and 97260 (GOPE00CZE). Each decision is clean against its own run's floats (section 5,
   item 3); the level under the default run's decision was displaced by about one (3, 4) lane.

**Reading.** During the hour after E18's code reference moved, the part of E18's level that only code holds stood 0.5
to 0.7 m apart between the two runs, about one lane of Galileo E1/E5a. The gate decided a wide lane on a link born
inside that window; the two runs came out one lane apart, and by the continuity of item 2 the off-lane hold is the
default run's (wide-lane fraction 0.108, a decision that looked clean). It is a decision of the gate at the formal
sigma, made while the formal sigma did not reflect the event. On Galileo E1/E5a it costs a user of that satellite
5.6 mm; on GPS one wide-lane cycle costs half a narrow-lane cycle. Not yet explained: why the default run's level
departs while the emitted code biases agree to 2 cm.

**Decisions (replacing item 1 and item 3(a) of section 7).**

1. The second day fails its first bar for a cause tied to the setting under test. `network.ring_fix.sigma_scale` is
   not adopted, and no judgment is asked of the author: the author's condition (the day passes) is not met.
2. Ordered from the builder in place of the pin lane: (a) the window explained (what moved the reference and why
   the default run's level departs), with a count of such moves per day; (b) the move treated as an event for the
   wide-lane gate (standing invariant 7): no wide-lane decision on a satellite's links for a set time after its
   code reference moves, or the re-seed's uncertainty carried into the wide-lane odds, and the fate of links already
   held stated; (c) a simulator bed with one satellite's code reference forced to move, then the DOY 145 pair re-run:
   if the same-start audit reads zero, the formal sigma is adopted under the author's rule.
3. The formal sigma with the full response stays the setting of development runs (a run setting): the audits catch a
   departure, and the stream it gives is the one sections 3 and 4 describe.

## 9. 2026-11-15: a user at one of the network's own receivers against the network's whole numbers (tool `user_vs_net_ints.py`)

For the same data and the same stream, a user's between-satellite ambiguity on a signal must equal the difference of
the network's whole numbers on the two links of that receiver (a tree pin from `network_chain_edges.csv`, or a held
link from `network_ring_fix.csv`). The user's value is read from the resolver's `CLFRAC` lines; the difference
`d_b = user - (N_net(r, sat_i) - N_net(r, sat_ref))` is formed per signal, then the wide lane `d_1 - d_2` and the
narrow lane `(f1 d_1 - f2 d_2) / (f1 - f2)` (the ionosphere-free part). A user that agrees sits at zero. This is the
truth the observed data lacked for a user: it holds wherever the network has both links.

Read on the Galileo-only observers of section 4 at the 31 network receivers of the L1X/L5X family, stream
`_rap_o2s1j_n144`:

| hour | pair-epochs | wide lane equal to the network's | narrow lane equal | narrow lane, mean distance |
| --- | --- | --- | --- | --- |
| 1 | 4 305 | 100.0 % | 43.6 % | 0.257 |
| 2 | 13 420 | 99.9 % | 58.2 % | 0.247 |
| 3 | 14 571 | 99.9 % | 76.5 % | 0.218 |
| 4 | 14 267 | 99.9 % | 95.5 % | 0.157 |
| 5 | 15 370 | 100.0 % | 98.9 % | 0.121 |
| 6 | 12 576 | 99.8 % | 99.7 % | 0.091 |

"Equal" means the user's float rounds to the network's value; the wide lane's mean distance is 0.07 to 0.08 cycle at
every hour. Hours 4 to 6 together: 42 213 pair-epochs, wide lane 99.9 %, narrow lane 98.0 % (mean distance 0.124, 54.8
% within 0.10); what is not at zero sits one cycle away (503 at +1, 336 at -1), on a few slow pairs (URUM00CHN E15-E21
for 103 epochs, BOR100POL E13-E23 for 60, YKRO00CIV E26-E28 and E33-E28 for 59 each, KRGG00ATF E08-E29 for 57). By
receiver the narrow-lane distance runs from 0.031 (MRO100AUS, 99.7 % within 0.10), 0.055 (STHL00GBR) and 0.061
(CPVG00CPV) to 0.235 (URUM00CHN), 0.222 (SCTB00ATA) and 0.207 (BOGT00COL).

The observers of both systems on the same stream (section 2), same receivers and links: narrow lane equal in 48.4,
59.6, 76.0, 84.4 and 92.1 % of the pair-epochs in hours 2 to 6 (83.9 % over hours 4 to 6, mean distance 0.167).

**Reading.**

1. The convention holds with no offset: the user's value and the network's difference are the same quantity.
2. The held stream is integer for a user at a network receiver: the wide lane at once, the narrow lane as fast as the
   user's own float solution comes within half a cycle (three to four hours for a static Galileo-only float user).
3. GPS in the user slows that by about an hour and a half (section 3: its unheld half steps without a signal).
4. What a user needs from here is its own integer search to shorten those hours; the stream no longer stands in the
   way on this family. Fixing users at the same receivers are run next and graded against the same truth.

## 10. 2026-11-15: GPS L1C/L2W held beside Galileo on DOY 144, re-graded (the builder's card NRF part 5 section 37)

The builder's arms `_rap_o6gj_n144` (default reference receivers) and `_rap_o6gj_n144r` (GOPE00CZE), both families
held at the formal sigma with the full response, 55 receivers (BSHM00ISR left out), exe nrf14; key-off runs
`_rap_o6offj_n144[r]`. Audits of this session, one start, two reference receivers:

| family | held link-epochs | checks | failing | departing arcs (majority attribution) |
| --- | --- | --- | --- | --- |
| GPS L1C/L2W | 75 991 / 62 647 | L1C 53 092, L2W 53 616 | 0, 0 | 0 |
| Galileo L1X/L5X | 84 196 / 85 324 | L1X 79 041, L5X 79 288 | 0, 0 | 0 |

The stream (`stream_jumps.py`, hours 3 to 6, per satellite-hour, depth 1 / 3 / 5 / 7 and more): GPS fraction steps
0.05 / 1.30 / 1.66 / 4.65 held against 0.27 / 4.47 / 9.41 / 14.65 key off; GPS whole steps 0.05 / 0.04 / 0.06 / 0.25;
Galileo none of either. GPS pair ranges inside a half hour: p50 0.091 / 0.103 / 0.183 / 0.405 cycle, p90 0.23 / 2.6 /
6.1 / 7.1. The builder reads a GPS held share of 45 to 55 % from hour 2 (Galileo 81 to 87 %).

**Reading.** GPS held beside Galileo agrees between the two reference receivers on both days (DOY 145: section 5). It
is half way: about half of its links are held, the stream steps a third to a sixth as often as without fixing, and
the unheld half still steps without a signal. User replays on this stream (both systems, and Galileo only) are
running.

## 11. 2026-11-15: the second day's departure, the builder's reading accepted (card NRF part 5 section 35)

The builder traced section 8's window further and its reading replaces this session's:

1. The two reference receivers' runs part at TOW 94560, 270 s before E18's code reference moves, in the default
   receiver's runs only (both starts, with and without fixing). At that epoch the chain re-hosts a satellite out of
   BOGT00COL, and the release frees BOGT00COL's receiver phase bias and E18's satellite phase bias on the X signals.
2. With both carries free, the part of E18's level that only code holds rests on BOGT00COL's code alone and settles
   0.5 to 0.7 m from the level the GOPE00CZE runs still carry. The code-reference move at TOW 94830 shifts a level
   that was already free (0.125 m against 0.018 m where it was carried). The emitted code biases agree because the
   code side is common to the runs.
3. Which level is right is not settled by the traces: BOGT00COL's E18 code is flagged by the block test, and the
   carried level dates from the first hour on another receiver's flagged code.
4. Code-reference moves: 41 on DOY 144 (13 satellites) and 33 on DOY 145 (11), all Galileo.

So section 8's statement that the move caused the departure is too strong: the release at the re-host is the root,
and the move adds to it. The wait after a code event that section 8 ordered was built by the builder
(`network.ring_fix.code_event_window_s`, default 0) with its own prediction that 1800 s does not cure the case and
5400 s only removes the common decision; its runs are recorded as they fall and the key stays off unless they pass.

**Orders revised (sent).** Remedy (a) of card NRF section 24.3 moves first: a cut of known step rebases the moved
side's carried entries and phase biases instead of releasing them (simulator beds, then the DOY 145 pair: the
same-start audit zero and E18's level within 0.1 m between the two reference receivers; the carried entries released
per hour counted with and without it). Then the stream signalling its steps, then what limits the GPS held share
(the wide-lane refusals by receiver).

## 12. 2026-11-15: the fixing user graded against truth (tools `user_vs_net_ints2.py`, `user_fix_vs_float.py`)

Fixing users at the network's own receivers, Galileo only, stream `_rap_o2s1j_n144`: the settings of the fixing users
of section 4 (ratio 5, partial fixing on, per-pair cap 0.15 cycle, narrow-lane sigma floor 0.04 cycle, wide-lane
branch check), folders `_rap_nw_obsnfxo2s1je_144_*`, 56 replays, every exit code 0; 28 receivers of the L1X/L5X
family have pair-epochs. A pair counts as held by the user when both per-signal values are within 0.01 cycle of whole
numbers.

**Against the network's whole numbers (section 9's read):**

| hour | pair-epochs | held by the user | of the held: equal to the network's value | wide lane off |
| --- | --- | --- | --- | --- |
| 1 | 4 305 | 1.9 % | 58.8 % | 0 |
| 2 | 13 420 | 7.0 % | 91.1 % | 0 |
| 3 | 14 571 | 31.7 % | 93.0 % | 0 |
| 4 | 14 267 | 52.5 % | 94.8 % | 0 |
| 5 | 15 370 | 65.4 % | 94.6 % | 0 |
| 6 | 12 576 | 65.3 % | 92.0 % | 0 |
| all | 74 509 | 42.1 % | 93.6 % | 0 of 31 385 |

Every held value that differs from the network's differs in the narrow lane by a whole number (one cycle, a few by
two) with the wide lane equal.

**Equality with the network undercounts.** The user's arc and the network's arc of the same receiver and satellite do
not always start at the same epoch, and the accumulated wind-up of two arcs that start apart differs by whole cycles,
equal on both signals: the narrow lane moves by a whole number and the wide lane does not (the wind-up branch of ARNE
section 3.2, here between a user and the network). The float users of section 9 show it without any fixing: a few
pairs sit at exactly one cycle from the network's value for an hour. A wrong fix is also a whole number away, so the
user's own float sibling (the same engine and arcs, no fixing: label `no2s1je`) is taken as the branch wherever its
float has come within 0.20 cycle of a whole number.

**Against the float sibling:**

| hour | held pair-epochs | right | wrong | unverified (float not converged) |
| --- | --- | --- | --- | --- |
| 1 | 80 | 55 | 0 | 25 |
| 2 | 936 | 751 | 7 | 178 |
| 3 | 4 623 | 3 264 | 61 | 1 298 |
| 4 | 7 486 | 6 097 | 1 | 1 388 |
| 5 | 10 049 | 7 839 | 237 | 1 973 |
| 6 | 8 211 | 7 167 | 313 | 731 |
| all | 31 385 | 25 173 (97.6 % of the verified) | 619 | 5 593 |

23 of the 28 receivers have no wrong hold. The wrong ones: YKRO00CIV 340, MBAR00UGA 85, BOR100POL 83, KRGG00ATF 67,
CUSV00THA 37, SVTL00RUS 6, CPVG00CPV 1; LCK400IND holds mostly unverified values that differ from the network's
(8.7 % equal).

**Positions** (3D against the network's coordinates, median by hour, mm, hours 4 to 6; fixing against float):

- Receivers whose holds are right: FFMJ00DEU 10 7 8 against 19 14 16; MRO100AUS 10 12 12 against 15 14 14; POVE00BRA
  8 7 4 against 21 22 16; WIND00NAM 9 4 4 against 16 16 15; SVTL00RUS 9 7 8 against 16 8 11. The fix arrives in hour 3
  at several: SVTL00RUS 12 against 58, MRO100AUS 15 against 39, WARK00NZL 12 against 42, VOIM00MDG 14 against 41.
- Receivers with wrong holds: YKRO00CIV 160 106 57 against 75 42 34; BOR100POL 91 46 27 against 53 18 10; LCK400IND
  143 114 50 against 47 38 17; GCGO00USA 79 58 43 against 39 25 8.

**How the wrong fixes pass** (the resolver logs of BOR100POL and LCK400IND). Most accepted fixes are partial: one pair
out of three to seven, with the float within 0.15 cycle of a whole number. For one ambiguity the ratio is
((1 - f) / f) squared, 32 at f = 0.15 whatever the float's uncertainty, and the success-rate test passes because the
formal narrow-lane sigma is 0.03 to 0.07 cycle while the float's distance from the truth averages 0.16 to 0.25 cycle
in hours 2 to 4 (section 9). A float that sits near the wrong whole number is therefore fixed there and held; the hold
has a variance of 1e-7 m^2 and no independent estimate left to release it.

**Reading.**

1. Where the user's holds are right, fixing does what it should: 4 to 12 mm from hour 4, and one hour earlier than
   the float user at several receivers.
2. The wide lane is never wrong, at the user as in the network.
3. The user's narrow-lane validation is too permissive for the float it is given: about one held pair-epoch in forty
   is wrong, at five receivers of 28, and each wrong hold costs centimetres to a decimetre for an hour or more.
4. This truth (the network's whole numbers and the float sibling) is the bench the user engine lacked on observed
   data. A sweep of the user's settings is graded on it next: partial fixing off, the per-pair cap at 0.10 cycle, a
   minimum arc of 40 epochs.

## 13. 2026-11-15: a sweep of the user's fixing settings on the bench of section 12

Three settings of `estimation.ambiguity_resolution`, one changed at a time against the fixing user of section 12,
each run at the 28 receivers (Galileo only, stream `_rap_o2s1j_n144`, labels `nfxAe`, `nfxBe`, `nfxCe`; tool
`users_gal_only.py --fxb --ar key=value`; a 600 s smoke first) and graded with `user_fix_vs_float.py`:

| setting | held pair-epochs | equal to the network's value | right of the verified | wrong | 3D median of the 28 receivers, hours 3 to 6 (mm) |
| --- | --- | --- | --- | --- | --- |
| float user (no fixing) | | | | | 79 39 24 15 |
| as it stands (section 12) | 31 385 | 93.6 % | 97.6 % | 619 | 72 34 20 14 |
| A: `enable_partial_ar` false | 27 746 | 99.2 % | 99.9 % | 25 | 76 27 18 13 |
| B: `nl_max_per_pair_frac` 0.10 | 25 543 | 93.7 % | 97.4 % | 565 | 74 35 23 15 |
| C: `min_epochs` 40 | 31 384 | 93.6 % | 97.6 % | 619 | 72 34 20 14 |

Setting A by hour: held 16, 538, 2 889, 5 930, 9 600, 8 773 pair-epochs (0.4, 4.0, 19.8, 41.6, 62.5 and 69.8 % of the
pair-epochs); equal to the network's value 81.2, 90.0, 95.2, 99.4, 100.0 and 100.0 %. Its 25 wrong pair-epochs are one
pair at one receiver (YKRO00CIV E29-E28, TOW 10260 to 10980), and 27 of the 28 receivers have none. The receivers that
the fixing of section 12 made worse return to the float user's level or better (mean of hours 4 to 6, mm, float /
as it stands / A): BOR100POL 27 / 55 / 27, LCK400IND 34 / 102 / 34, GCGO00USA 24 / 60 / 14, CUSV00THA 36 / 32 / 10,
LEIJ00DEU 15 / 32 / 14, KRGG00ATF 39 / 55 / 37. YKRO00CIV stays at 108 against 50: its float does not approach the
network's values at all (narrow-lane distance 0.24 to 0.29 through the six hours), and its "full sets" are one or two
ambiguities, accepted as in section 12.

**Reading.**

1. The wrong holds of section 12 come from partial fixing: with it off, 99.9 % of the verified holds are right and in
   hours 5 and 6 every held value equals the network's (18 373 pair-epochs). The cost is 12 % of the held pair-epochs,
   in hours 2 to 4.
2. The per-pair cap and the minimum arc length are not the lever: B changes nothing of note and C is inert here.
3. A set of one or two ambiguities is still accepted on its fraction alone. A floor on the searched sigma that is
   honest about the float (section 9: 0.16 to 0.25 cycle in hours 2 to 4) is the next setting: `nl_sigma_floor_cyc`
   0.15 and 0.20, with and without partial fixing (labels `nfxDe`, `nfxEe`, `nfxFe`, running).
4. A wrong hold still has no way out: the hold's variance leaves no independent estimate to test it against.

**Decision** (a run setting of this session's user replays, open to review): fixing users on the network's stream
run with partial fixing off from here on. The loader's default is already off; the fixing-control block of card ARU
had turned it on.

### 13.1 2026-11-15: the same setting at the 17 independent test receivers

Fixing users, Galileo only, on the same stream, at the 17 receivers that are not part of the network (labels
`o2s1je` as it stands, `o2s1jeA` with partial fixing off; float users of section 4). Median over the 17 receivers of
the hourly 3D median (mm, hours 1 to 6):

| users | hours 1 to 6 |
| --- | --- |
| float | 386 137 79 42 35 34 |
| fixing as it stands | 386 131 93 60 50 53 |
| fixing, partial fixing off | 386 134 96 62 56 40 |

Per receiver, mean of hours 4 to 6 (mm; float / as it stands / partial fixing off): better with fixing at ASCG (40 /
20 / 20), CIBG (105 / 49 / 74) and KOKB (81 / 48 / 55); level at WTZZ (17 / 13 / 17), CEDU (29 / 31 / 31), CHTI (22 /
21 / 21), POHN (35 / 33 / 33) and WUH2 (38 / 43 / 43); worse at BADG (15 / 89 / 87), ARHT (20 / 33 / 53), QUIN (34 /
158 / 77), STPM (20 / 62 / 46) and UTQI (39 / 67 / 62).

**Reading.** Outside the network, fixing is not yet a gain over the float user under either setting, and there is no
truth there to say which holds are wrong. Two differences from the network's own receivers are known: a network
receiver's own systematic errors are part of the stream it is graded on, an independent receiver's are not (the
antennas of BADG and QUIN have no Galileo calibration in igs20.atx and use substitute values); and six of the 17
track the family the network does not hold (BAKE, DAV1, HARB, KOKB, POHN, SCRZ on L1C/L5Q). The bench of section 12
therefore bounds what the user's rule can do when the receiver's model matches the stream; the independent receivers
measure what is left when it does not. Open: how far the independent receivers' floats sit from whole numbers on the
held family, receiver by receiver.

### 13.2 2026-11-15: a floor on the searched sigma stops the fixing altogether

`nl_sigma_floor_cyc` 0.15 with partial fixing off (label `nfxDe`) and on (`nfxEe`), same bench: both hold 326
pair-epochs in six hours (27 746 with the floor at 0.04 and partial fixing off), all right. The two runs are
identical: with the floor no set passes the success-rate test (`min_success_rate` 0.999), whatever its size, because
the floor is added per satellite and does not shrink when other ambiguities are fixed. The run at 0.20 was stopped
(the answer is the same). The floor is therefore not the tool: an honest per-ambiguity sigma says that rounding one
ambiguity is unsafe, while the full-set search is right 99.9 % of the time (section 13) because the float's errors
are common to the satellites of a receiver and the set's geometry sees them. The setting of section 13 stands
(partial fixing off, floor 0.04); what remains open is the set of one or two ambiguities and the missing way out of
a wrong hold.

## 14. 2026-11-15: the builder's remedy (a) on the simulator, re-graded (card NRF part 5 section 39)

`network.ring_fix.rehost_keep_carry` (default off; commit fc43724, exe nrf17): a chain re-host onto a link the ring
fix holds takes the held whole number, and the moved side's carried arc entries, receiver phase biases and satellite
phase biases are kept instead of released. Not kept when the link's residual monitor has tripped, when either band is
in the epoch's free-epoch latch, when the new host has no carry, or when the step cannot be carried. The design and
the code change (`chainHeldInteger` and the kept branch of `applyRehostRow`) were read here: narrow, and the
exclusions are the right ones. The builder shows that the re-host of TOW 94560 on DOY 145 (section 11) is such a case
(through the held link STHL00GBR-E21, step 0 on both bands).

Simulator arms (the golden fixture, 3 h from 00:30:15, GPS L1C/L2W and Galileo L1C/L5Q held at the formal sigma with
the full response): `_rap_k0_sim[_r]` without the remedy, `_rap_ka_sim[_r]` with it (default reference receivers and
S45000SIM). Read here:

| read | result |
| --- | --- |
| audit, remedy on, two reference receivers | GPS 0 of 47 175 per band, Galileo 0 of 49 211 |
| audit, remedy on against off, one reference receiver | GPS 0 of 55 457, Galileo 0 of 61 018: no whole number changes |
| audit, remedy off, two reference receivers | GPS 0 of 37 581, Galileo 0 of 50 386 |
| wrong against truth (the trace's audit column), every run | 0 held, 0 decided |
| held link-epochs, off to on, default receivers | GPS 59 585 to 62 047, Galileo 61 956 to 65 104 |
| held link-epochs, off to on, S45000SIM | GPS 60 640 to 68 227, Galileo 58 755 to 53 131 |
| held share in the last half hour, off to on | GPS 77.9 to 87.8 % and 80.0 to 85.5 %; Galileo 86.8 to 91.3 % and 89.9 to 91.0 % |
| cut releases; carried arc entries erased, default receivers | 372 to 166; 10 024 to 2 704 |
| the same, S45000SIM | 282 to 130; 7 998 to 2 350 |
| receiver and satellite phase biases released, default receivers | 920 to 188; 428 to 114 |

The run-end line counts 212 and 154 re-hosts kept, every one "with a non-zero step carried by the shift walk" and
none re-targeted.

**Reading.** The builder's numbers reproduce: no wrong whole number, no whole number changed by the remedy, the
releases down by 71 to 73 %, the held link-epochs up 3 % over the four family-runs. Two points were put to the
builder: one family-run holds fewer link-epochs with the remedy (Galileo with S45000SIM, 9.6 % fewer, slower through
the second hour and level at the end), and every kept re-host is counted with a non-zero step although the design
expects a step of 0 on a held link (the size of those steps is asked for). Bed B and the DOY 145 pair are running.

## 15. 2026-11-15: remedy (a) on DOY 145, re-graded: the day reads zero

The builder's pair `_rap_o8a_n145` (default reference receivers) and `_rap_o8a_n145r` (GOPE00CZE): section 5's
holding pair (Galileo L1X/L5X at the formal sigma, full response) with `network.ring_fix.rehost_keep_carry` on, exe
nrf17. Read here against section 5's pair without the remedy:

| read | without the remedy (section 5) | with the remedy |
| --- | --- | --- |
| same-start audit, L1X | 428 failing of 67 667 | 0 of 73 907 |
| same-start audit, L5X | 476 failing of 67 675 | 0 of 74 096 |
| departing arcs (majority attribution) | one pair, 68 epochs | none (113 167 and 113 360 arc-epochs compared) |
| E18's level on BOGT00COL-E18, largest difference between the two runs, TOW 94200 to 99300 | 0.72 m | 0.015 m |
| held link-epochs, the two runs | 69 872, 82 239 | 75 436, 84 864 |
| held share of the default run, last half hour | 65.1 % | 86.9 % |
| fraction steps per satellite-hour, hours 3 to 6, depth 1 / 3 / 5 / 7 and more | 0.00 / 0.09 / 0.46 / 2.26 | 0 at every depth |
| ionosphere-like steps per satellite-hour, the same | 0.21 / 0.89 / 1.42 / 0.75 | 0.15 / 0 / 0 / 0 |
| alarms (RAIM rows) | 1 | 1 |

The default run's held share by quarter hour: 0 2 19 65 79 79 84 87 88 90 86 86 91 91 87 83 83 84 88 88 86 86 88 86 %
(without the remedy it fell to 65 to 75 % over hours 4 to 6).

**Reading.** The cause named in section 11 is confirmed by its cure: with the carries kept at the re-host, the two
reference receivers hold E18 on one level and every held whole number agrees. The remedy also removes the rest of the
stream's movement on this day: no fraction step at any depth (there were up to 2.3 per satellite-hour at depth 7 and
more without it) and no ionosphere-like step beyond depth 1, and the held share stays near 87 % to the end. The
pre-registered bars of the builder's section 39 pass (the same-start audit zero; E18's level within 0.1 m).

**Decision.** The condition of the author's rule of 2026-11-15 is met: the formal sigma (`network.ring_fix.sigma_scale`
1) and the remedy are to be adopted by the builder in the scoped class (regime: the chain posture with network integer
fixing; the goldens unchanged) once its build with a tolerance on the zero step (asked here) reproduces these runs.
The note names what verified each key (the formal sigma: DOY 144 without the remedy on every audit and both families,
DOY 145 with it on the same-start audit; the remedy: the simulator beds and the DOY 145 pair) and what is still to
do (DOY 144 and the two-start audit with the remedy, ordered at once; more stations and days; the main posture
without the chain).

## 16. 2026-11-15: the adoption on the builder's branch, verified (commit 3e93813, card NRF part 6 section 41)

The builder's build with a tolerance on the zero step (exe nrf18: a displacement under 1e-4 cycle is a step of 0)
reproduced every whole number of the runs of sections 14 and 15, and the adoption followed. Checked here on the
commit:

- `network.ring_fix.sigma_scale` (armed at 1, the formal sigma) and `network.ring_fix.rehost_keep_carry` (armed when
  on) are RECOMMEND_SCOPED in the regime `on("network.ring_fix.enable")`, each with its note (the date, what verified
  it, what is still to do: DOY 144 and the two-start audit with the remedy, more stations and days, the main posture
  without the chain).
- `network.ring_fix.enable`, `network.ring_fix.held_first` and `network.ring_fix.code_event_window_s` stay
  UNVALIDATED; the golden configs are unchanged across the commit.
- A wording point was put to the builder: the note of `sigma_scale` reads as if the two-start audit of DOY 144
  covered GPS; it covered Galileo L1X/L5X.

The builder's answers to the two points of section 14:

1. The family-run that holds fewer link-epochs with the remedy (Galileo with S45000SIM) is passed over by the ring
   fix's cap of three decisions per epoch over all families, taken in order of the log-odds: with the remedy more
   links pass every test, GPS takes the slots first, and Galileo's link-epochs refused by the cap rise from 976 to 3
   379 (GPS: 3 543 to 4 416). Its held share is level by the end (91 against 90 %).
2. The steps of the kept re-hosts are zero to rounding (largest 2.6e-5 cycle; no accumulation along a satellite's
   level), but for a link decided one epoch before it becomes a tree edge, whose carry has not yet reached its whole
   number (one case per bed and day, 0.10 to 0.15 cycle, carried exactly, the decision right against truth).

On the receiver phase bias (the request of section 14's review): what resets a kept one is what resets every carried
one (an event on every arc of the receiver, a return after a gap, a gap past the horizon, its own step test); none
of the kept re-hosts on the simulator or on DOY 145 fell at a receiver with a receiver-wide event.

**New item.** The cap of three decisions per epoch now refuses thousands of link-epochs that passed every test. A
higher cap is put to the builder as a test, simulator first.

## 17. 2026-11-15: users that start after the stream has settled

**Why.** A float user that starts with the network states a 3D sigma five to eight times smaller than its error for
three hours (WTZZ at 2 h: 34 mm stated, 224 mm actual; at 3 h: 16 and 72; BOR100POL at 2 h: 35 and 252). In its first
hour the stream is not yet held and steps without a signal (section 3: 5.6 to 9 fraction steps per satellite-hour),
and a static user keeps what that hour put into its solution. The users of sections 4, 9, 12 and 13 all started with
the network.

**Runs.** The same stream (`_rap_o2s1j_n144`), Galileo only, the user started 2 h after the stream
(`network.start_offset_hours` 2.004167 at the user; a 900 s smoke first): float and fixing users (partial fixing off)
at the 28 network receivers (labels `nfl2e`, `nfx2e`) and at the 11 independent receivers of the held family (label
`o2s1je2`); 78 replays, every exit code 0. A user started late runs on past the end of the stream on its last
corrections (here hours 5 and 6 of the user): only the user's first four hours are read.

**Positions, median over the receivers of the hourly 3D median (mm), hours 1 to 4 since the user's start:**

| users | start | 28 network receivers | 11 independent receivers |
| --- | --- | --- | --- |
| float | with the network | 419 193 79 39 | 493 214 56 36 |
| fixing (partial fixing off) | with the network | 419 193 76 27 | 493 212 84 62 |
| float | 2 h later | 97 34 27 16 | 118 45 23 19 |
| fixing (partial fixing off) | 2 h later | 97 28 14 13 | 118 45 17 19 |

Receivers under 2 cm in hours 2, 3 and 4 with the late start: fixing 11, 16 and 20 of 28 (float 4, 13, 18) at the
network's receivers, and 4, 6 and 7 of 11 (float 0, 1, 6) at the independent ones. Independent receivers, late start,
hours 2 to 4 (mm; float, then fixing): WTZZ 24 9 5, then 17 6 4; BADG 25 22 16, then 13 13 14; ARHT 23 22 10, then
15 15 10; ASCG 24 41 46, then 10 12 20; STPM 45 21 16, then 45 16 14; UTQI 55 23 27, then 48 17 27. CIBG (122 192
124) and WUH2 (33 73 64) are poor under either.

**Against the network's whole numbers** (28 network receivers; the hour is the user's):

| read | hour 1 | hour 2 | hour 3 | hour 4 |
| --- | --- | --- | --- | --- |
| float, narrow-lane distance, start with the network (section 9) | 0.257 | 0.247 | 0.218 | 0.157 |
| float, narrow-lane distance, start 2 h later | 0.194 | 0.142 | 0.118 | 0.087 |
| fixing, pair-epochs held, start with the network (section 13) | 0.4 % | 4.0 % | 19.8 % | 41.6 % |
| fixing, pair-epochs held, start 2 h later | 7.5 % | 54.1 % | 69.7 % | 72.5 % |
| fixing 2 h later: right of the verified holds | 100 % | 99.7 % | 99.1 % | 100 % |

Over the four hours 99.6 % of the verified holds are right (99 wrong: KRGG00ATF 57, URUM00CHN 42), the wide lane is
never off (0 of 28 601), and the independent receivers' own floats come to whole numbers sooner (WTZZ 0.194, 0.090,
0.056, 0.027; STPM 0.267, 0.165, 0.062, 0.045; BADG 0.267, 0.114, 0.110, 0.075).

**Reading.**

1. The user's slow start was the stream's first hour, not the user's geometry: on a stream that has settled, a
   Galileo-only static user is at 3 to 5 cm in its second hour instead of about 20 cm, and its fixing holds half of
   its pair-epochs in the second hour instead of 4 %.
2. With the late start, fixing is a gain or level at every independent receiver where the float itself converges:
   the receivers that fixing made worse in section 13.1 (BADG, ARHT, STPM, UTQI) had been fixed wrongly during the
   unsettled hours.
3. So a user should begin on a stream that is already held, or the stream should tell it when it is (the step
   declaration of the builder's order, and a flag of which satellite signals are held). This is the author's
   proposal of 2026-11-14 (users only use a network that has converged), measured.
4. Open: the first hour of the user itself (97 to 118 mm); the receivers whose float never reaches whole numbers
   (CIBG, WUH2, YKRO00CIV); GPS in the user once the builder's stream declares its steps.

## 18. 2026-11-15: CORRECTION to sections 14 to 16: the remedy as first built holds wrong whole numbers on DOY 144

The to-do named in the adoption notes (DOY 144 with the remedy) was run by the builder at once (card NRF part 6
section 42.1, runs `_rap_o9_n144[r]` and `_rap_o9_n145[r]`, exe nrf18, GPS L1C/L2W held beside Galileo L1X/L5X) and
fails on DOY 144. Read here:

| pair of runs | family | checks | failing |
| --- | --- | --- | --- |
| DOY 144, two reference receivers | Galileo L1X | 82 026 | 686 (hours 4 to 6, closures of one cycle) |
| | Galileo L5X | 82 006 | 289 |
| | GPS L1C, L2W | 57 152, 57 452 | 0, 0 |
| DOY 145, two reference receivers | Galileo L1X, L5X | 73 520, 73 713 | 0, 0 |
| | GPS L1C, L2W | 46 863, 46 896 | 0, 0 |

The majority attribution on DOY 144 names SYOG00ATA-E02 on L5X from TOW 12240 (211 epochs) and WARK00NZL-E02, and on
L1X WARK00NZL-E25 (141 epochs), WARK00NZL-E02, MRO100AUS-E08, GAMB00PYF-E02, WARK00NZL-E08 and MRO100AUS-E03.

**The cause** (the builder's trace). SYOG00ATA-E02 was decided at TOW 11940 with its per-signal floats still 0.20 and
0.28 cycle from their whole numbers (an ionosphere-like lag: ionosphere-free 0.04, wide lane 0.08) and became a tree
edge by a kept re-host one epoch later, before the hold had pulled its carry onto the whole numbers. The kept branch
took the carry's displacement, 0.283 cycle, as a known step and carried it into the moved side: the carried floats of
three other held links of E02 moved off their whole numbers, were released, and later decisions on E02 were taken in
a frame 0.283 cycle off the other run's.

**What sections 14 to 16 got wrong.** Section 14 noted that every kept re-host was counted with a non-zero step and
asked for the sizes; section 16 accepted the answer that the few real ones (0.10 to 0.15 cycle) were "carried
exactly, the decision right against truth" without asking what a fractional step does to the held links of the moved
side. A held link cannot be the source of a frame step: its whole numbers are the frame, and a carry that has not
reached them is the link's own lag. The adoption of section 16 was made on code with this path.

**The fix** (the builder's exe nrf20): a re-host is kept only when the new host's carry sits on its held whole number
to within 1e-4 cycle; a held link whose carry is off is released as before, and counted. The simulator pair, both
days with the two-start audit of DOY 145, and the step declaration's twins are re-run on it. Outcome rule: if every
bar passes, the adoption stands on the fixed code with its notes revised; if not, the remedy's adoption is withdrawn.
Until then the statements of sections 15 and 16 about the adopted set hold for DOY 145 and the simulator only.

**The settled flag** (the builder's read on `_rap_o2s1j_n144`, asked for after section 17). A satellite signal counts
as settled when it is reached from the family's reference receiver only through held links and tree edges that
entered the tree as held links: 21 % of the satellite-epochs in hour 1, 92 % in hour 2, 97 to 100 % afterwards, with
0.05 changes per satellite-hour after the first hour. Ordered as a build (default off): the stream's `signal_integer`
by that rule. The user engine already has the matching veto (`network.require_signal_integer_for_ar`). The user test
owed here: a user that starts with the network, on a stream carrying the step declaration and the flag, against the
late-started user of section 17.

## 19. 2026-11-15: the narrowed remedy re-graded: every audit zero on both days

The builder's re-runs on exe nrf20 (commit af34831: a re-host is kept only when the new host's carry sits on its held
whole number to within 1e-4 cycle), the adopted set with GPS L1C/L2W held beside Galileo L1X/L5X, 55 receivers:
`_rap_o10_n144[r]`, `_rap_o10_n145[r]`, `_rap_o10_n145b` (start 1 h later), and the simulator pair
`_rap_kd_sim[_r]`. Read here:

| audit | Galileo | GPS |
| --- | --- | --- |
| DOY 144, two reference receivers | L1X 0 of 83 644, L5X 0 of 83 624 | L1C 0 of 57 402, L2W 0 of 57 694 |
| DOY 145, two reference receivers | 0 of 73 597, 0 of 73 790 | 0 of 47 128, 0 of 47 110 |
| DOY 145, two starts, wind-up branches removed | 0 of 55 728, 0 of 55 723 | 0 of 2 894, 0 of 2 894 |
| simulator, two reference receivers | L1C 0 of 49 116 (L5Q the same) | 0 of 47 142 per band |
| simulator, remedy on against off | 0 of 60 963 | 0 of 55 437 |

The failure of section 18 is gone and nothing else departs. By the outcome rule of section 18 the adoption stands on
the fixed code; the builder revises the two notes (the defect, the fix, DOY 144 and the two-start audit now verified
with the remedy). The to-do that remains in the notes: more stations and days, and the main posture without the
chain.

## 20. 2026-11-15: the decision cap, the step declaration and the settled flags (card NRF part 6 sections 46 to 49)

- **The adoption notes** of `network.ring_fix.sigma_scale` and `network.ring_fix.rehost_keep_carry` on the builder's
  branch now name the defect of section 18, its fix, and what verified each key on the fixed code. Checked here.
- **The decision cap** (`network.ring_fix.max_decisions_per_epoch`, loader default 3). On the simulator the cap of 3
  held the second hour back: at 12 the held share reaches 91 % (Galileo) and 82 % (GPS) in the third half-hour
  against 59 % and 34 %, with no wrong whole number at 3, 6, 12 or no cap. On DOY 144 at 12, read here
  (`_rap_q12_n144[r]`): two reference receivers, Galileo 0 of 84 308 and 84 288, GPS 0 of 59 111 and 59 435; cap 12
  against cap 3 from one reference receiver, 0 of 86 804 and 0 of 78 831 (no whole number changes). On observed data
  the cap binds in the first hour only and 12 changes little. Adopted by the builder at 12 in the scoped class, with
  its note (to do: DOY 145 and a two-start audit at 12).
- **The step declaration** (`network.disc_step_class_pairs`, UNVALIDATED). Every whole and fraction step carries an
  increment, by construction. The cause-side check asked for in section 14's review fails: of 733 to 975 releases
  per run, 117 to 214 have no increment within 10 minutes, and the released level then moves by a median 0.20 cycle
  in steps each under the tolerance, mostly on the families that are not held. Ordered: every released satellite
  signal raises its increment at the release epoch. The user engine does react to an increment (it resets that
  signal's ambiguity).
- **The settled flags** (`network.ring_fix.settled_flags`, UNVALIDATED): built, two defects of the build fixed (a
  copied record carried another signal's flag; a held link in a tracking gap counted). On the stream
  `_rap_f11_n144` the flags equal the rule at every record; flagged share by hour from hour 2: Galileo 90, 98, 95, 95
  and 100 %, GPS 44, 66, 73, 80 and 84 %.

The user replays on `_rap_f11_n144` (users started with the network, waiting for the settled flag) are recorded in
the continuation of this note, card ARNG.
