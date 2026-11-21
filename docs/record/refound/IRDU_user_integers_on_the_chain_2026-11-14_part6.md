# IRDU part 6: the phase step declaration on what a user sees (section 14 of card IRDU)

**STATUS**

- **id:** IRDU-part6
- **topic:** network-vehicle
- **status:** superseded
- **summary:** Section 14 of card IRDU, continued from [part 5](IRDU_user_integers_on_the_chain_2026-11-14_part5.md) (2026-11-14): approach 1 of section 13.5, approved by the author, built on the simulator and default off as card IRD's step IRD-7 (emission and declarations). The key `network.disc_phase_visible` declares a phase step of a satellite and signal when the change of its clock less phase bias, less the median of that change over the satellites of its constellation and signal, exceeds the discontinuity threshold; the rule travels in the stream header (`discpv=`) and a replay reproduces the counters. Runs A to E are pre-registered below before any graded run. Results are appended below. 14.2: off byte-identical in every data record and for the users (A); with the key the stream's values are unchanged, the declared steps equal the reference counts (536, 987, 1 960) and a replay reproduces them; no user-visible pair jump above twice the bar goes undeclared, where the legacy test left up to half a metre (D); the true slip is declared at its epoch (E). The users gain where every link is held (Q1: from 15 and 30 min the declarations-off arm's result, from the start with the 12.11 rule), but B's bar 1 fails (72 % of bins against 80 %: users from the start with the recipe), and on the plain chain arm and r3c users from 0 and 15 min end worse than with the legacy declarations in several bins of the second hour (C, D): not traced.
- **source:** [part 5](IRDU_user_integers_on_the_chain_2026-11-14_part5.md) sections 13.2 (the declarations-off arm), 13.3 and 13.5 (the cause, approach 1); [IRD](../../refound/IRD_integer_recovering_datum_2026-10-13.md) section 11, step IRD-7 and its frozen bars

**Outcome.** The key declares a phase step only when what a user of between-satellite differences sees moves. On the stream side it meets every bar: off identical, the declared steps equal to the offline tool's counts, a replay reproducing them, no large user-visible jump undeclared, the true slip caught. Users that start while the network converges gain where every link is held, but where links float or re-seat they end worse than with the old declarations in parts of the second hour, cause not traced. It stays an instrument: default off, UNVALIDATED.

Sections 1 to 10 are in [IRDU](IRDU_user_integers_on_the_chain_2026-11-14.md), section 11 in [part 2](IRDU_user_integers_on_the_chain_2026-11-14_part2.md), sections 12 to 12.5 in [part 3](IRDU_user_integers_on_the_chain_2026-11-14_part3.md), sections 12.6 to 12.11 in [part 4](IRDU_user_integers_on_the_chain_2026-11-14_part4.md), section 13 in [part 5](IRDU_user_integers_on_the_chain_2026-11-14_part5.md). Results are appended below.

### 14 2026-11-14: the step declaration on what a user sees, built (card IRD step IRD-7; the graded runs pre-registered before they run)

**Assigned** by the requesting session after the author's approval. The author was asked "Have the Opus session build step declarations on what a user sees (simulator, default off)?" and answered "Yes, assign it". Nothing else is approved: not the emission hold, not the datum change, not the scaled threshold, and not an extension of the users' rule of 12.11 (that question stays with the author). The requesting session re-graded 13.2 to 13.5 and reproduced them, except the 92 to 95 % share of 13.3, which it did not re-grade. Its offline tool (`D:/GnssProcessorSupport/presentation/agu26/fixing_control/tools/stream_user_view.py`: jumps above 0.25 cycle between consecutive epochs, counted on the phase bias, on the clock less the phase bias, and on the clock less the phase bias less the median over the satellites of the signal, with four or more satellites) gives the reference counts:

| run | phase bias alone | clock less phase bias | less the satellites' median |
| --- | --- | --- | --- |
| `_rap_irdu_q1_nr3` | 1 542 (last 37.0 min) | 1 277 (28.0) | 536 (15.5) |
| `_rap_irdu_zw_nr3` | 1 798 (48.0) | 1 567 (39.5) | 987 (35.0) |
| `_rap_irdu_zw_r3c` | 2 610 | 2 414 | 1 960, through the run |

**Where it sits.** Card IRD section 11, step IRD-7 (emission and declarations). Its frozen bars apply as follows:
- **Off byte-identical with no token:** run A.
- **A replay reproduces the discontinuity counters byte for byte:** the unit test, and run B's producer and replay counts.
- **Every step declared:** here every step of what a user sees, by run D's audit and run E.
- **The W bars on the emitted phase biases:** unchanged by construction, since the key changes no emitted value, only the counters and the header.

The other IRD-7 bars (the settle regeneration, `signal_integer`) do not apply.

**What was built** (this branch; committed with this section, before any graded run):

1. **The rule:** a pure function, `products/PhaseVisibleStepTest` (`decidePhaseVisibleSteps`, `phaseVisibleMedian`). For each key it takes d, the change of the clock less the phase bias since the key's last emission with a clock.
   - Groups are (constellation, signal).
   - A key votes when its change is defined and its baseline is the previous emission (consecutive epochs).
   - The median is the middle value, or the mean of the two middle values for an even count.
   - With at least four voters (`kPhaseVisibleMinSats`), a key is declared when |d less the median| exceeds its threshold (discfrac times the wavelength, or the flat gate).
   - Below four voters, each key is tested on its own change d, so the shared part is then declared with the rest (the safe side).
2. **The emitter** (`NetworkProductEmitter`, eighth construction parameter, the minimum count; 0 is the legacy test).
   - A pre-pass over the epoch forms the groups before the per-satellite loop stamps the counters; a tested key takes the rule's verdict in place of the phase bias alone.
   - These keys are not tested:
     - a key without a clock in this or its previous emission keeps the legacy test;
     - a first record is seeded;
     - a return past the staleness horizon is re-seeded, as the legacy test does;
     - a key under a forced mark is declared by the mark and does not vote.
   - Off, the pre-pass does not run and the legacy code path is unchanged.
   - The state (each key's baseline and the previous emission) is in the snapshot as a version 4 header, written only when armed.
3. **The stream header:** the S record carries `discpv=4` when armed (the legend declares the optional token). `checkContract` compares it, and `makeReplayEmitter` builds the replay's emitter with it, so a replay of the file reproduces the counters. The user's reaction to a counter change is unchanged.
4. **The key:** `network.disc_phase_visible` (default false), a getter with Doxygen, and a ledger row (UNVALIDATED, area DatumSignalling).
   - Under `network.emit_pb_hold` the loader disarms it with a warning, since the hold's generations declare the held steps, and the header then carries no token (item 7).
5. **Both edges** (standing invariant 7). Counters over the emitter's life, printed at the run end of the producer and of a replay:
   - first records, returns tested against an older baseline, returns past the horizon;
   - keys under a forced mark, keys without a clock;
   - vote-set entries and exits;
   - groups entering and leaving the below-minimum state;
   - the legacy test's verdicts on the tested keys, and the keys declared here that the legacy test would not have declared.
6. **Unit tests:** 19 cases in `tests/test_phase_visible_step_test.cpp`, all passing on the Debug build with the 173 tests of the neighbouring emitter, stream-file, mark-scope, hold and snapshot suites.
   - The rule on hand-built epochs: a shared move alone, one satellite's own move, fewer voters than the minimum, untested keys, groups, the even median.
   - The emitter: off is the legacy test; a shared move; an own move; a phase-bias move matched by its clock; below the minimum; a satellite appearing, leaving and returning; a key without a clock; a forced mark; a return past the horizon; the snapshot round trip and its refusal by an unarmed emitter.
   - The header token: off byte-identical; the parse; the replay emitter; the contract check.
   - A file replay reproducing the producer's counters epoch by epoch.
   - The config key and its disarming under the hold.
   - The stream file's legend test (`NetworkProductIoTest`) now arms the token too, so it checks that the legend declares it.

**The shared part (item 3): whom the declaration protects.** The header states the assumption, and the stream does not declare the shared move as a step of its own. A user that differences between satellites the stream corrects loses the shared part in its differences, and the closed-loop consumer differences in that way. A user that does not difference absorbs a move common to every satellite of a signal into its receiver terms. A user that pairs a corrected satellite with one the stream does not correct (12.9) reads the shared part as a between-satellite move and carries it undeclared.

The 12.11 rule (`network.closed_loop.drop_unrecorded`) removes that case at the user. Without the rule, 13.2 measured that such users still end better with no declarations at all after their 45th minute, and are mixed before it. The run-end line counts the epochs whose shared move exceeded the bar: the steps a non-differencing user would have needed.

**The clock's declaration (item 5).** The key does not change the clock's discontinuity counter or the code-bias jump test. When a clock step is declared, the user's engine (`RealTimeEngine::reinflateOnClockDisc`) re-inflates the variances of its inter-system biases (all of them on a step of the reference constellation's clock) and resets no phase ambiguity; the receiver clock, free every epoch, absorbs the common part. A clock step with the clock less phase bias continuous therefore does not re-float the user's phase ambiguities. Finding: none; nothing changed.

**A pair property, stated before the audit.** Each satellite is tested against the median, so two satellites moving apart by just under the threshold each make their pair's difference move by up to twice the threshold with neither declared. The legacy test on the phase bias alone has the same property. Run D's audit therefore counts undeclared pair jumps above twice the threshold, which must be 0 by construction (a check of the implementation), and above the threshold, reported for the key and for the control.

#### 14.1 The runs, pre-registered

**Executables.** Network: the build of this section, copied to `build/x64-Release/gnss_processor_irdu_pv1.exe` (sha256[:16] `cee9504ce1d1424e`); the controls of 13.2 (`gnss_processor_irdu_fp2.exe`) and an r3c control on the same fp2 executable (`irdu_zwc_r3c`). Users: the same build, copied to `D:/GnssProcessorSupport/gates/exe/irdu/gnss_processor_irdu_pv1.exe`, folders with the suffix `_bpv1`. Each network arm is preceded by a 10-epoch smoke, cold start, 2 h, Release. Arms (`scripts/ird_make_arms.py`; knob group `fpin_nr3_all_pv` = Q1's keys plus the key, `--knob` elsewhere):

| tag | bed | base | key |
| --- | --- | --- | --- |
| `irdu_q1k0_nr3`, `irdu_zwk0_nr3`, `irdu_r3k0_r3c` | irdu_nr, irdu_nr, r3c | Q1, the plain chain arm, the plain chain arm | off (the new executable) |
| `irdu_q1pv_nr3`, `irdu_zwpv_nr3`, `irdu_r3pv_r3c` | the same | the same | `network.disc_phase_visible` true |

**A. Key off.**
- The new executable's `products.txt` is byte-identical to the fp2 control's on Q1, the plain chain arm and r3c in every data record. The one `#` legend comment line differs: it declares the new optional token, the codebase's rule for a wire extension. The reader skips it, and byte comparisons of stream files strip the `#` lines.
- The users are byte-identical between the previous user build (`ur1`) and the new one on the control streams, by `scripts/irdu_build_identity.py`:
  - Q1: 30 replays (starts 0, 15 and 30 min, with the recipe and with the rule);
  - the plain chain arm and r3c: 5 each (from 30 min, the recipe).

**B. Q1, key on.**
- **The stream:** every C, B and P value equal to the control's (`scripts/irdu_stream_identity.py`); only the header and the counters differ.
- **The declared value steps:** 536 with the last at 15.5 min (the reference count). A difference is explained key by key from `scripts/irdu_pv_audit.py replay`, the independent replay that already gives 536 on the control stream.
- **The replay:** the producer's run-end counts of the test equal a replay's from the file's first epoch (users starting at 0 min).
- **The users** from 0, 15 and 30 min, with the recipe and with the rule, against the control (13.2) and the declarations-off arm (13.2, arm iii). Bar, PASS when all hold:
  1. For starts 0 and 15 min, in every 15-minute bin from each user's 45th minute, the key's median is at most the declarations-off median plus a quarter of the control's excess over it, or below the declarations-off median, in at least 80 % of those bins (with the recipe and with the rule pooled).
  2. Users from 30 min within 8 mm of the control in every bin from their 45th minute (13.2's spread).
  3. Resets: 0 for users from 30 min (no declared step after 15.5 min).

**C. The plain chain arm, key on.** The same reads, against its own control users and its declarations-off users (`irdu_zwc_nr3`, `irdu_zwdt_nr3`, users on the new executable).
- The declared value steps: 987, the last at 35.0 min.
- The users' bar as in B (1 and 2); its resets after 35 min 0.

**D. r3c, key on: the safety side.**
- **The audit** (`scripts/irdu_pv_audit.py audit`): for every epoch, signal and pair of satellites emitted at that epoch and the previous one, the jump of the pair's clock less phase bias difference, with neither satellite declared.
  - Above twice the threshold: 0, a check of the implementation.
  - Above the threshold: reported for the key and for the control, with the largest.
- **The declared value steps:** 1 960 plus the forced bumps, the reference count.
- **The users** from 0, 15 and 30 min, with the recipe and with the rule, against the r3c control's users: no 15-minute bin from each user's 30th minute worse than the control by more than 10 mm or 10 % of the control's median, whichever is larger.

**E. The negative control.** The fixture's true slip (S43000SIM/G22, L1C, +25 cycles at 15.0 min) is declared on G22 at 15.0 min with the key on: its clock less phase bias moves −90.0 mm on L1C and −136.5 mm on L2W against the median, beyond 47.6 and 61.1 mm. The independent replay on the control stream already declares it there.

**Decisions in the author's absence, for review.**
1. The minimum voter count is fixed at four (the value of the offline tool) and travels as the token's value, not as a second key.
2. Below the minimum, each key is tested on its own change (the shared part declared), not left undeclared.
3. A key without a clock keeps the legacy test.
4. The shared part is not declared as a step of its own; the header states the assumption of between-satellite differences.
5. The pair property is audited at twice the threshold for a zero count, and reported at the threshold.
6. The legend comment line extends with the token, as the codebase requires; run A compares the data records.
7. Run A's user identity covers the controls' streams with the new user build, rather than re-running each key-off network's users: the key-off streams' data records are compared directly.

#### 14.2 2026-11-14: results of 14.1

**What ran.**
- **Network:** seven runs, all exit 0 with 240 of 240 epochs, each after a 10-epoch smoke (exit 0, 11 epochs; the key-on smokes write `discpv=4`, the key-off smokes no token):
  - on `gnss_processor_irdu_pv1.exe` (sha256[:16] `cee9504ce1d1424e`): `_rap_irdu_q1k0_nr3`, `_rap_irdu_zwk0_nr3`, `_rap_irdu_r3k0_r3c`, `_rap_irdu_q1pv_nr3`, `_rap_irdu_zwpv_nr3`, `_rap_irdu_r3pv_r3c`;
  - on fp2: `_rap_irdu_zwc_r3c`.
- **Users:** 220 float replays, all exit 0: 210 on the new user build (folders `_bpv1`) and 10 on `ur1` for run A.
- **Tools:** `scripts/irdu_build_identity.py` gains `--user-set`; grading by `scripts/irdu_late_start.py`, `scripts/irdu_user_resets.py`, `scripts/irdu_stream_identity.py` and `scripts/irdu_pv_audit.py`.

**A. Key off: PASS.**
- **The streams:** on Q1, the plain chain arm and r3c, the new executable's `products.txt` equals the fp2 control's in every data record. The one `#` legend line differs only by the declared optional token (`[,discpv=<n>]`), and the producer's counters (`logs/network_solution.csv`) are identical.
- **The r3c control:** the fp2 re-run reproduces the original `_rap_irdu_zw_r3c` byte for byte.
- **The users:** identical between `ur1` and the new build on the control streams: Q1 15 of 15 with the recipe and 15 of 15 with the rule; the plain chain arm and r3c 10 of 10.

**The streams with the key on (B, C, D).**
- Every C, B and P value equals the control's; only the S record's token and the counters differ.
- The declared value steps equal the reference counts.
- The independent replay (`scripts/irdu_pv_audit.py replay`) matches the producer's counter increments key for key: "in both" equals both totals on every run, none only one side.
- A replay of the file from its first epoch (the users starting at 0 min) prints the producer's run-end counts of the test: group-epochs 478, keys tested 13 862, 13 862 and 14 994, declared 536, 987 and 1 960. This is IRD-7's replay bar.

| run | legacy test (control) | declared with the key | reference | last declared step | keys declared that the legacy test missed | epochs with the shared move above the bar | largest shared move |
| --- | --- | --- | --- | --- | --- | --- | --- |
| Q1 | 1 542 | 536 | 536 | 15.5 min | 51 | 40 group-epochs (21 epochs, all but two before 13 min) | 1.269 m (0.5 min) |
| plain chain arm | 1 798 | 987 | 987 | 35.0 min | 128 | 44 | 1.269 m |
| r3c | 2 610 + 4 forced | 1 960 + 4 forced | 1 960 | 119.5 min (through the run) | 215 | 32 | 0.994 m |

The keys the legacy test missed are user-visible moves the phase bias alone did not show; the largest pairs below come from them.

**D. The audit (r3c, with Q1 and the plain chain arm for comparison).** Pair jumps of the clock less phase bias difference with neither satellite declared, over the run:

| run | above the bar, control | above the bar, key | above twice the bar, control | above twice the bar, key | largest undeclared, control | largest undeclared, key |
| --- | --- | --- | --- | --- | --- | --- |
| r3c | 3 899 | 5 280 | 76 | **0** | 435.5 mm (1.0 min, G04-G24 L1C) | 119.5 mm |
| Q1 | 36 | 1 773 | 5 | **0** | 510.2 mm (2.0 min, G14-G26 L2W) | 117.0 mm |
| plain chain arm | 971 | 3 152 | 50 | **0** | 510.2 mm | 121.0 mm |

The construction check holds on every run: with the key, no undeclared pair jump exceeds twice the bar. The legacy test let user-visible jumps of up to half a metre through undeclared: 74 of r3c's 76 fall in the first 15 minutes, and Q1's largest (510.2 mm) is at 2.0 min, the epoch at which the clock anchor re-engages every satellite, moving the clocks and not the phase biases. Between one and two bars the key leaves more pair jumps undeclared than the legacy test (the pair property stated in 14): on the beds without re-seats all in the first 30 minutes on Q1, and all but 3 in the first 45 minutes on the plain chain arm.

**E. The negative control: PASS.** The fixture's true slip is declared on G22 at 15.0 min on both signals (and again at 15.5 min), as the replay predicted.

**The users.** Float, median over the five users per 15 minutes from each user's start (mm), and the users' phase resets summed.

Q1 (B):

| start | users | control | key | declarations off | resets, control to key |
| --- | --- | --- | --- | --- | --- |
| 0 | recipe | 2168 939 582 203 165 116 110 106 | 2048 650 483 326 110 132 70 98 | 2237 1102 283 194 64 85 68 73 | 1 611 to 607 |
| 0 | rule | 959 336 318 158 190 99 64 59 | 770 191 115 101 67 68 34 38 | 798 166 120 71 65 73 43 40 | 1 611 to 607 |
| 15 | recipe | 1182 734 226 170 116 116 116 | 535 763 316 77 90 87 85 | the key's, bin for bin | 408 to 0 |
| 15 | rule | 561 397 197 192 99 64 59 | 601 141 97 93 51 31 31 | the key's | 408 to 0 |
| 30 | recipe | 759 179 179 101 119 119 | 944 188 147 94 124 123 | the key's | 27 to 0 |
| 30 | rule | 610 158 175 80 50 54 | 796 149 183 73 42 56 | the key's | 27 to 0 |

- **Bar 1 FAILS:** 13 of 18 bins against the pre-registered 80 %.
  - Users from 15 min pass in every bin: no declared step reaches them, the last being at 15.5 min. Users from 0 min with the rule pass 4 of 5 (the exception is 45 to 60 min, 101 against 71 mm).
  - Users from 0 min with the recipe fail 4 of 5: 326, 110, 132 and 98 mm against 194, 64, 85 and 73 mm with the declarations off. At 45 to 60 min and 75 to 90 min they are worse than the control as well (326 against 203 mm, 132 against 116 mm).
  - These users mix in the three satellites the stream does not correct (12.9). They keep 607 resets and carry the undeclared shared move between corrected and uncorrected satellites: the case the header's assumption excludes.
- **Bar 2 PASSES:** users from 30 min within 8 mm of the control from their 45th minute.
- **Bar 3 PASSES:** 0 resets from 30 min.

The plain chain arm (C):

| start | users | control | key | declarations off | resets, control to key |
| --- | --- | --- | --- | --- | --- |
| 0 | recipe | 1613 1259 785 286 317 236 83 85 | 1963 1310 776 427 234 170 140 130 | 8077 3720 2233 790 533 333 274 233 | 1 892 to 894 |
| 0 | rule | 848 532 380 172 147 125 83 75 | 1109 914 783 403 255 184 148 135 | 8663 3942 2268 1188 659 400 255 229 | 1 892 to 894 |
| 15 | recipe | 1231 1026 274 312 223 83 88 | 1231 1018 370 203 158 117 110 | 887 760 307 203 146 122 115 | 542 to 24 |
| 15 | rule | 786 437 167 149 124 83 75 | 889 926 351 224 154 131 124 | 1063 700 287 218 157 134 127 | 542 to 24 |
| 30 | recipe | 1130 273 336 221 86 94 | 758 253 263 195 92 90 | the key's | 83 to 0 |
| 30 | rule | 543 169 145 125 83 75 | 641 137 148 105 88 76 | the key's | 83 to 0 |

- **On this bed the declarations-off arm is far worse than the control from the start** (8 077 mm in its first bin, 233 mm in its last). The links float, and without declarations the users diverge: the declarations protect them here, unlike on Q1.
- **Bar 1** is met (17 of 18 bins) only because the declarations-off arm is the worse side; as written it does not discriminate on this bed.
- **Bar 2 FAILS by its wording:** the largest difference from 30 min is 26 mm, and it is an improvement (195 against 221 mm); no bin is worse by more than 6 mm.
- **Against the control,** users from 0 and 15 min end worse in the second hour: with the rule from 0 min, 255, 184, 148 and 135 mm against 147, 125, 83 and 75 mm; with the recipe from 0 min, worse at 45 to 60 and 90 to 120 min and better at 60 to 90 min.

r3c (D; the rule changes nothing here, since the stream corrects every satellite):

| start | control | key | resets, control to key |
| --- | --- | --- | --- |
| 0 | 989 630 375 404 264 183 118 121 | 983 404 330 248 197 183 143 146 | 4 063 to 3 387 |
| 15 | 661 480 397 266 174 108 118 | 521 367 352 238 179 153 152 | 2 509 to 2 073 |
| 30 | 794 238 233 186 138 150 | 503 240 354 203 143 146 | 2 211 to 1 968 |

**The tolerance FAILS:** 10 bins (5 per user set) are worse than the control by more than 10 mm or 10 %:
- from 0 min, 90 to 120 min: 143 and 146 against 118 and 121 mm;
- from 15 min, 75 to 105 min: 153 and 152 against 108 and 118 mm;
- from 30 min, 30 to 45 min: 354 against 233 mm.

The key is better than the control in most bins of the first hour, by up to 226 mm (from 0 min, 404 against 630 mm).

**The reading.** On the stream side the key does what it was built to do:
- the declared steps are the reference counts, and a replay reproduces them;
- no user-visible pair jump above twice the bar is left undeclared, where the legacy test left up to half a metre;
- the true slip is declared at its epoch.

Where every link is held (Q1) the users get the gain of 13.2's declarations-off arm, from 15 and 30 minutes and, with the 12.11 rule, from the start. Where the links float or re-seat (the plain chain arm, r3c), users that start while the network converges end worse than with the legacy declarations in several bins of the second hour.

That cost is not traced. One candidate, not tested: a satellite's split move (its clock and phase biases moving together) leaves the clock less phase bias unchanged, so the key does not declare it. It still moves the clock the user applies to its code, and no channel declares it, since the clock counter has no value test. The legacy test declared it through the phase bias, and the user re-floated the ambiguity against the new code level.

The key is a measured instrument, not a candidate setting: it stays default off, UNVALIDATED.

**Decisions in the author's absence, for review.** Those of 14, and:
- (8) Bar 1 of C is reported as met but non-discriminating, and bar 2 of C as failed by its wording though the difference is an improvement; neither is re-written.
- (9) Run A's users on the plain chain arm and r3c were compared from 30 min with the recipe only (10 pairs), as pre-registered.
- (10) The users of C and D ran on the new build with the controls' streams (identical data records), not on the key-off runs' streams.

**Not verified.**
- The code-side candidate.
- Observed data (not run, as assigned).
- The fixing users.
- Grading on the users' pivot pairs (the audit counts every pair of corrected satellites, a superset of the pairs a user forms).

### 15 2026-11-14: the users' rule extended to satellites without a phase-bias record (built; the graded runs pre-registered before they run)

**Assigned** by the requesting session after the author's approval. The author was asked "Have Opus extend the user-side rule to satellites without a phase-bias record?" and answered "Yes, add it". Default off. The requesting session re-graded section 14 (stream and users) and reproduced it; that key stays off, and nothing more is asked on it now.

**Why**, measured by the requesting session on observed data, offline (card ARNC and card ARNB sections 10 and 11, in the uncommitted worktree `rx_sat_bias`, read only):
- On the observed-data stream one Galileo satellite-epoch in eight offers a receiver of the L1C/L5Q type no complete pair of bias records, although the stream carries the satellite on L1X/L5X; the receiver then uses the satellite with the clock alone.
- Under the emission hold in its phase-clock mode, the clock less the phase bias is the constant of a generation. The clock record alone wanders by 83 mm (median, 30-minute windows) in the network's first hour.
- With those satellite-epochs left out (a filtered copy of the receiver's observation file), a user that starts with a cold network stays below 10 cm from 13 min in place of 52 (HARB00ZAF) and from 19 in place of 47 (DAV100ATA).
- The rule of 12.11 asks the store for a clock record; these satellites have one, so it leaves them in.

**What is built** (this branch; committed with this section, before any graded run):

1. **A second key**, `network.closed_loop.drop_unrecorded_phase_bias` (default false), derived into the consumer engine as `processing.ssr_drop_unrecorded_phase_bias`.
   - It includes the clock rule. The key of 12.11 keeps its meaning alone: the clock rule only, with its log lines and run-end line as before.
   - A second key rather than a widened first one, so that the behaviour of 12.11 with its key on cannot change.
2. **The criterion.** A satellite is left out when it has no clock record (as before), or when one of the raw phase signals the receiver uses for it has no phase-bias record.
   - The signals are the phase keys of the satellite's modelled observation, ionosphere-free labels excluded.
   - The phase-bias test is the apply site's own (`ObservationEquation` `addSsrBias`): `getPhaseBias` at the observation time with the bias age limit times the stale-hold factor when the bias staleness gate is on, and the latest record otherwise. The closed-loop consumer arms the gate (30 s times 2, so a record counts for 60 s), so a stale record counts as absent at both places.
   - A missing code bias is no reason: on the observed-data stream every GPS L1C row of an L1C/L2W receiver has a phase bias and no code-bias record, by the stream's convention.
   - When one signal lacks its record and the other has it, the whole satellite is left out, as the requesting session's measurement did.
3. **Both edges** (standing invariant 7), in the pure gate `UnrecordedSatGate`.
   - A Drop at the first sighting without the records, or when a record is lost. It names the missing record: the clock, or the phase bias of a named signal.
   - A Readmit with fresh states when every record is back.
   - A dropped satellite whose missing record changes kind stays out without a second Drop.
   - Counters by reason, and a run-end line naming the phase-bias drops.
4. **The freshness flip** (`flip_to_fresh`, `reset_flip` in `resetOnPhaseBiasDisc`). The rule runs in the epoch's preparation, before that reaction, so a satellite left out never reaches the flip test.
   - Its last applied state stays as it was while the satellite was in the solution (applied, or no entry at a first sighting), so the returning record makes no flip.
   - The readmit's fresh start takes the place of the reset the flip gave before (E02 at 19:46 on observed data: `flip_to_fresh;reset_flip`).
   - Unchanged: a satellite that stays in the solution (all its records present) and a satellite the rule does not see (the key off).
5. **Tests and records.** Unit tests on hand-built epochs (4 new cases, 11 in all: a missing phase bias naming the signal, its loss and return, a change of the missing record, the clock form equal to the state form), a ledger row (UNVALIDATED), loader text and Doxygen.
6. **Cleanup.** The card numbers in the comments of 12.10, 12.11 and section 12's oracle pin are removed; two log strings that name the oracle pin's card are kept.

**Tools.**
- `scripts/irdu_pb_remove.py`: a copy of a product stream without chosen satellites' code-bias and phase-bias records for chosen spans, clocks kept.
- `scripts/irdu_users.py`: user sets `dp` (the key) and `fr` (a filtered observation file, `make --rinex-dir`).
- The requesting session's `rinex_drop.py` and `conv_curve.py` (`D:/GnssProcessorSupport/presentation/agu26/fixing_control/tools/`), read and run, not changed.

#### 15.1 The runs, pre-registered

Executables: this section's build, copied as `gnss_processor_irdu_pb1.exe` (network not run; users, folders `_bpb1`). Each new configuration is smoked first (10 epochs). Simulator first.

- **S1. Key off.** The users are byte-identical to the previous user build's (`pv1`) on the same streams, by `scripts/irdu_build_identity.py`:
  - Q1's control stream, starts 0, 15 and 30 min, with the recipe and with the clock rule (`du`): 30 pairs;
  - r3c's control stream from 30 min: 5 pairs.
- **S2. Key on where every used signal has its record.** On r3c (the stream as emitted, `_rap_irdu_zwc_r3c`, and the reference arm with the holds, `_rap_irdu_g_r3c`), five users from 30 min, float and fixing, with the key and without: identical, 20 of 20. Otherwise, the satellite-epochs the key leaves out are listed and explained (a phase-bias record withheld while the clock is emitted would be such a case).
- **S3. The made case, a simulated stream with the hold.**
  - **The stream:** `_rap_irdu_gpb_r3c` is `_rap_irdu_g_r3c` (holds on, `pbhold=2`) without these code- and phase-bias records, all other lines byte-identical:
    - G13, both signals, 20.0 to 45.0 min (49 epochs: a record lost and returning);
    - G23, both signals, from the first epoch to 70.0 min (141 epochs; SZZ000SIM sees it rise at 51.5 min: a first sighting without records);
    - G05, L2W and C2W only, 60.0 to 80.0 min (40 epochs: one signal missing).
  - **The users:** five, from 0 and 30 min, float:
    - (a) the key off on the edited stream;
    - (b) the key off with each user's observation file filtered by `rinex_drop.py` from (a)'s trace (every satellite-epoch with a row without a phase bias removed);
    - (c) the key on with the user's own file.
  - **Bar:** (c)'s position trace identical to (b)'s, byte for byte, or every difference explained; the satellite-epochs the key leaves out equal the filter's.
- **O1. Observed data, user replays only** (no network run, no chain run).
  - **Receivers and streams:** HARB00ZAF, DAV100ATA and SCRZ00BOL, from 00:00 on `_rap_p26_n144` and from 18:00 on `_rap_nww24_n144`, on this branch's executable.
  - **Arms:**
    - (a) the configuration of `_rap_nw_obs_144_<site>` (from 00:00) and `_rap_nw_obsw18_144_<site>` (from 18:00) as it is;
    - (b) their filtered twins `_rap_nw_obsc00x_144_<site>` and `_rap_nw_obsw18x_144_<site>`;
    - (c) (a) with the key.
  - **Smoke:** these configurations were written for another executable, so a smoke first, and the keys of those configurations that this executable's loader does not read are listed.
  - **Bar:** (c) equals (b) in the position trace, or every difference is explained; the satellite-epochs the key leaves out equal the filter's: 1 298, 1 503 and 2 321 from 00:00; 1 277, 1 292 and 166 from 18:00.
- **O2. The gain on this executable:** the minute from which the 3D error stays below 10 cm, (a) against (c), at HARB00ZAF and DAV100ATA from 00:00. The requesting session's figures on its executable are 52 and 47 min against 13 and 19 min.
- **Optional:** the holds-off stream (`_rap_nwh24_n144`, filtered files `h00`, `h18`), reported beside without a bar.

**Prediction.** S1 identical. S2 identical unless the stream withholds a phase-bias record while emitting the clock. S3 and O1 (c) equal to (b) except where a stage ahead of the rule reads the satellite's observations (the coverage logs and traces of the epoch's opening, which do not reach the position). A position difference would be traced to its stage.

**Standing, 2026-11-14 (night).** Interrupted by the author's new priority (integer fixing in the network, ring by ring, on the simulator). The code, its 11 gate unit tests and the ledger row are committed with this section; the Debug build passes the 11 gate cases and the 91 tests of the neighbouring user-engine suites (102 in all), and the Release build links. None of the runs of 15.1 has run: S1 to S3, O1 and O2 wait, with their bars as written above. The edited stream of S3 exists (`_rap_irdu_gpb_r3c`); no user replay was made on it.

## 2026-11-20: superseded

Superseded with its parent card IRDU by the managing session's choice of 2026-11-20, under the author's deferral of the consolidation decisions ("I will defer to your choices on the consolidation 'decisions for the author'"); it follows the parent into the record.
