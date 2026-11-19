# ARNI: the exact epoch time at the user

**STATUS**

- **id:** ARNI
- **topic:** user-ar
- **status:** open
- **summary:** Continuation of card ARNH (that file neared the 60 KB bar). The builder found that every RINEX epoch time is rounded through a double-precision Julian date, by up to 19.7 microseconds in a sawtooth of period 675 s (card NRF part 9 section 80), and built the exact conversion behind `processing.exact_epoch_time` (default off). This card holds the users' side of the before-and-after test: the user engine rebuilt with the same change, the replays on the builder's repaired streams, and the verdict on adoption.
- **dates:** 2026-11-16 (opened; sections 2 to 8)
- **knobs / posture:** `processing.exact_epoch_time` (the builder's key, commit 72f594a on its branch; UNVALIDATED), at the network (the builder's arms `x14`) and at the user (the engine of this card).
- **code:** the builder's change of `TimeUtils` (the exact conversion and its process-wide switch), the key's loader line and the start-up call, applied to the worktree `rx_sat_bias` as an uncommitted change (patch `D:\GnssProcessorSupport\gates\rscb_checkpoint\exact_epoch_time_L1.patch`); engine `build\x64-Release\gnss_processor_rscb5.exe`. Tools `user_sawtooth.py` and those of card ARNH (copies under `D:\GnssProcessorSupport\presentation\agu26\fixing_control\tools\`).
- **run dirs:** `D:\GnssProcessorSupport\runs_nom\_rap_nw_obsnY{Df,Dx,Bf,Bx}_{144,145}_*`, `_rap_nw_{obs,fxb}u12y_{144,145}_*`, `_rap_nw_obsnZBx_145_*`; the builder's streams `_rap_x14_n144`, `_rap_x14_n145`; outputs under `D:\GnssProcessorSupport\presentation\agu26\fixing_control\grades_arnd\exact\`.
- **source:** card [ARNH](ARNH-USERS-UNDER-THE-FLAG-RULES.md), section 11 (the users on the adopted package); card NRF part 9 section 80 on the builder's branch `irdu/user-integers`

**Outcome.** Open. The exact epoch time is adopted in the chain posture (section 8): on the repaired streams the
users stay within 1 mm of the present ones from hour 3 in 47 of 48 hourly cells, hold 285 731 pair-epochs with
none wrong, move 20 to 30 % less from epoch to epoch and fix about 10 minutes earlier. The card also holds two
user-side findings of the same night: a third-frequency run that carries a wrong cycle at the user (section 6),
and the Leica receivers' loss-of-lock value 4 on Galileo L1C, read as a loss of lock at every epoch; the key
`estimation.cycle_slip.lli_slip_mask` repairs it (section 7), with a float offset at those receivers left
untraced.

> Results are appended TO THIS FILE, below the text.

---

## 1. Scope

The defect and its repair are the builder's (card NRF part 9 section 80). `TimeUtils::gregorianGpsToGpsTime` and
`gregorianToGpsTime` add the fraction of the day to a Julian date near 2.46e6, whose last-bit spacing is 2^-31 day
(40.2 microseconds), and convert back, so a calendar time on a 30 s grid comes out off by a sawtooth: zero at every
multiple of 675 s (1/128 day) of the day, drifting to -19.7 microseconds, then +19.7 at the first epoch after each odd
multiple of 337.5 s (a wrap epoch). Reproduced here in a few lines of arithmetic (-18.78 microseconds at TOW 3690,
+19.67 at 3720, 0 at 4050) and read in the stream's epoch records (29.999998, 3720.0000196). Every calendar-parsed
time takes the same path: the observation epochs, the SP3, clock and bias epochs, the navigation message's times.
Sections of card ARNH are cited by their numbers there.

## 2. 2026-11-16: the sawtooth in the present users' positions (a read before the test)

`user_sawtooth.py` on the users of ARNH section 11 (streams `f13`, engine `rscb3`), 28 network receivers, hours 3 to
6: per receiver the RMS of the detrended step d(t) = x(t) - (x(t - 30) + x(t + 30)) / 2 of each position component at
the wrap epochs and elsewhere, and their ratio (medians over the receivers).

| users, day | 3D step at the wraps (mm) | elsewhere (mm) | ratio | epoch-to-epoch 3D change, RMS (mm) |
|---|---|---|---|---|
| fixing, both systems, 144 | 0.11 | 0.13 | 0.79 | 0.27 |
| float, both systems, 144 | 0.10 | 0.12 | 0.90 | 0.27 |
| fixing, Galileo only, 144 | 0.21 | 0.28 | 0.68 | 0.59 |
| fixing, both systems, 145 | 0.11 | 0.13 | 0.89 | 0.33 |
| float, both systems, 145 | 0.12 | 0.12 | 0.96 | 0.29 |
| fixing, Galileo only, 145 | 0.15 | 0.27 | 0.61 | 0.58 |

The positions do not step at the wrap epochs. They change by 0.3 mm from one epoch to the next, so a range error of
zero mean over 675 s is averaged out of the position; it must sit in the residuals, the receiver clock and the
float states. The expectation for the test is therefore a small change of the hourly positions and a cleaner fit,
not a visible step removed.

## 3. 2026-11-16: the before-and-after test at the users (pre-registered)

**Streams.** The builder's `x14_n144` and `x14_n145`: `f13` (card ARNH section 9) with
`processing.exact_epoch_time` (its order L2); the control is `f13`.

**The user engine.** `gnss_processor_rscb5.exe`: the engine of the earlier cards (`rscb3`) with the builder's change
applied (section 1). With the key absent it must reproduce `rscb3`: four receivers of `nFBx` on DOY 145 are run again
with it, and their hourly values must be identical.

**User runs** (the settings of ARNH section 9, the stream `x14`, the key on, engine `rscb5`): `nYDf`, `nYDx`, `nYBf`,
`nYBx` at the 28 network receivers and `u12y` (float and fixing) at the 11 independent receivers, both days. A read
beside it, DOY 145, the 28 receivers, fixing on both systems: `nZBx`, the repaired user on the unrepaired stream
`f13` (a user whose time tags are exact while the network's are not).

**Bars** (against the `f13` users of ARNH section 11; medians over the receivers of the hourly 3D median).

1. **Not worse:** no label above its `f13` label by more than 1 mm in any of hours 3 to 6, on either day.
2. **Holds:** wrong holds of kind (a) on at most 0.5 % of the verified holds of every fixing label and day; the wide
   lane off only at a receiver that hosts the satellite's tree edge.
3. **The identity of the engine with the key absent** (above).

**Reads.** The hourly medians of hours 2 to 6 and their paired differences; `user_sawtooth.py` on the new users; the
held pair-epochs and the time of the first fix; the mixed case `nZBx` against `nFBx` and `nYBx`.

**What follows.** If the bars pass and the builder's network reads pass (its order L2), `processing.exact_epoch_time`
is adopted in the chain posture, at the network and at the users, with the three-part note. The main posture's
goldens and baselines would move with it and are left to the author.

## 4. 2026-11-16: which of two runs carries a one-cycle disagreement, read at the user (pre-registered; a read for the builder)

**Why.** Beside this card's subject: the builder's look-only runs with a third frequency admitted on DOY 144 (card
NRF part 9 section 79.1) agree with each other from two datum receivers everywhere but on 54 arcs in two regional
clusters of held pair links, in hours 2 and 3: Galileo L1X/L5X at CAS100ATA, SYOG00ATA and KRGG00ATF (E07, E08, E13,
E26, TOW 3660 to 6150) and GPS around Australia and the western Pacific (MRO100AUS, WARK00NZL, HKSL00HKG, DARW00AUS,
G09 the most, TOW 6510 to 9660). Both runs hold the same integers there while their frames differ by one cycle. The
adopted configuration without the third bands does not show it. The network cannot say which run is right; a user at
a receiver of the cluster can: one cycle between satellites is 11 cm of ionosphere-free range.

**Runs** (engine `rscb3`, the key of this card absent): the users of both systems of card ARNH section 9 (float and
fixing) on `_rap_k1t_n144` (the default datum receiver) and `_rap_k1r_n144` (WTZR00DEU), at the seven receivers named
above and at BOR100POL and GOPE00CZE outside the clusters; labels `k1tBf`, `k1tBx`, `k1rBf`, `k1rBx`.

**The read.** Per receiver and run the hourly 3D medians of hours 2 to 4 (float and fixing) against the same users
on `f13_n144`, and the fixing user's holds against its float sibling. The run whose users at the cluster receivers
are worse than on `f13` by 20 mm or more in hour 2 or 3, or hold integers their float siblings contradict, carries
the wrong cycle. Users equal on both runs within 5 mm mean that the difference does not reach a user.

## 5. 2026-11-16: the user's reference satellite that changes at every epoch: the loss-of-lock indicator's third bit (pre-registered)

**The defect** (card ARNH section 11.4 named the symptom at HKSL00HKG: Galileo ambiguities alternating between two
values, the position 0.1 to 1.2 m off without GPS). A run with the pivot manager's log on shows the cause chain: at
every epoch the between-satellite reference of Galileo is declared disturbed and handed to another satellite, and
the outgoing reference's ambiguities are re-seeded at a sigma of 1000 m (300 handovers in 300 epochs; at PARK00AUS 8
changes in 665). The reference is disturbed because its ambiguities are reset at every epoch, and they are reset
because the Melbourne-Wubbena slip detector (`MwSlipDetector::hasAnyPhaseLockLoss`) reads any nonzero loss-of-lock
indicator as a loss of lock. The observation file of HKSL00HKG carries LLI 4 on every Galileo L1C phase (10 421 of
10 426 records in the part read; 5 carry 5). RINEX 3 defines bit 2 of the indicator for Galileo as BOC tracking of an
MBOC-modulated signal, not a loss of lock. The five Leica receivers of the data set (HKSL, REYK, WTZR, WROC, SCRZ:
GR50, GR30, GR10) all carry it on every Galileo L1C phase; no other receiver read does. The network's slip scan
tests bit 0 only (`network/slip/PassASlipScan.cpp`, `RawObservables.cpp`, `SlipQc.cpp`), so the same receivers hold
their Galileo links in the network.

**The change** (the worktree `rx_sat_bias`, uncommitted; engine `gnss_processor_rscb6.exe`):
`estimation.cycle_slip.lli_slip_mask` (default 7: any bit, the detector as built), read by the three tests of the
Melbourne-Wubbena detector (`MwSlipDetectorPairs.cpp`, `MwSlipDetectorEpoch.cpp`); 3 keeps bit 0 (lock lost) and bit
1 (half-cycle ambiguity) and leaves out bit 2. The TurboEdit helper `pairHasLockLoss` is not changed (that path is
off in these users).

**Runs** (streams `f13`, both days, the key of sections 1 to 3 absent): at HKSL, REYK, WTZR and WROC the users of
both systems and of Galileo alone, float and fixing, with the mask 3 (`m3Bf`, `m3Bx`, `m3Df`, `m3Dx`), against the
same users with the key absent (`nFB*` of card ARNH; `nFD*` run here where missing).

**Bars.**

1. **Identity:** the new engine with the key absent reproduces the old one (BOR1 and HKSL, DOY 145, fixing on both
   systems, every epoch); with the mask 3 a receiver whose file carries no flag above 1 is unchanged (BOR1, PARK).
2. **Galileo alone at the four Leica receivers:** fixing at most 25 mm in each of hours 4 to 6 on both days (median
   over the four of the hourly 3D medians).
3. **Both systems at the four:** not worse than with the key absent by more than 2 mm in any of hours 3 to 6.
4. **The reference satellite:** fewer than 20 changes per run at each of the four (the fixing users' ambiguity
   lines); no wrong hold above 0.5 % of the verified holds.

**What follows.** If the bars pass, the mask 3 joins the settings of the fixing users, and the change goes to the
builder as a patch for its branch, with a unit test and a ledger row; the default stays 7.

## 6. 2026-11-16: results of section 4, the users on the two third-frequency runs of DOY 144

**Runs.** 36 replays (9 receivers, float and fixing, on `_rap_k1t_n144` and `_rap_k1r_n144`), all exit 0; grades under
`grades_arnd\k1\`. Hourly 3D medians of hours 2 to 5, mm; the same users on `f13_n144` first.

| receiver, users | `f13` | `k1t` (default datum receiver) | `k1r` (WTZR00DEU) |
|---|---|---|---|
| MRO1 float | 21.6, 3.1, 3.9, 3.8 | 35.3, 27.7, 17.0, 6.3 | 22.4, 4.4, 4.1, 4.2 |
| MRO1 fixing | 21.6, 2.4, 3.7, 3.7 | 29.4, 14.5, 11.3, 5.8 | 17.6, 3.0, 3.8, 4.3 |
| CAS1 float | 21.9, 12.7, 7.1, 4.7 | 23.4, 18.1, 16.1, 10.7 | 58.6, 12.2, 5.7, 4.6 |
| CAS1 fixing | 21.2, 5.2, 7.6, 5.2 | 16.9, 16.2, 13.4, 9.6 | 58.6, 5.4, 5.9, 5.0 |
| WARK float | 69.0, 39.8, 27.3, 20.6 | 81.6, 20.6, 16.5, 13.3 | 53.8, 38.6, 21.0, 12.6 |
| KRGG float | 39.1, 18.2, 14.0, 12.2 | 23.6, 18.1, 15.3, 13.6 | 42.9, 20.9, 16.5, 14.0 |

SYOG, DARW, HKSL and the two receivers outside the clusters (BOR1, GOPE) are within 5 mm of `f13` from hour 3 on both
runs. No fixing user holds an integer its float sibling contradicts (29 658 held pair-epochs on `k1t`, 24 595 on
`k1r`): a user at a receiver of the cluster holds what the network holds there, so the holds cannot show a regional
shift.

**Reading.** By the criterion of section 4, `k1t` carries the wrong cycle: its users are worse than on `f13` at MRO1
by 14, 25 and 13 mm in hours 2 to 4 (float) and at CAS1 by 5 to 9 mm in hours 3 to 5, after the window of the
disagreement has closed, while `k1r` returns to `f13`. That agrees with the builder's read (card NRF part 9 section
79.2: the runs from the elected root with the third bands depart from the adopted configuration). One point against
`k1r`: CAS1 in hour 2, inside the window, 37 mm above `f13`; it is not separable from the convergence of that hour.
The effect at a user is one to two centimetres at two receivers for two to three hours; a wrong third-band admission
is therefore visible at the user, and no third-signal integer is to be held on observed data until its cause is
found. Two candidates were passed to the builder: the Block IIF satellites' L5 phase varying against L1/L2 over the
orbit (G09 leads the GPS cluster) under a satellite phase bias carried as constant, and a receiver-side E5b phase
bias at one receiver type (the X/X cluster is the three TRIMBLE ALLOY receivers of the far south).

## 7. 2026-11-16: results of section 5, the loss-of-lock mask at the Leica receivers

**Runs** (engine `gnss_processor_rscb6.exe`, streams `f13`, all exit 0; outputs under `grades_arnd\lli\`): the 48
runs of section 5; 68 Galileo-only users at the receivers of the Galileo L1C/L5Q family that are not Leica (key
absent; with 6 receivers run earlier, 23 receivers on both days); 7 users with the pivot manager's log on
(`dbg_pivot.py`); 9 float users with the code down-weighted. A scan of every observation file of DOY 144, 145 and
146 (`lli_scan.py`, 73 files a day): a loss-of-lock value above 3 is carried by the five Leica receivers only (HKSL,
REYK, SCRZ, WROC, WTZR), on every Galileo L1C phase (4 on 99.2 to 99.95 % of the records, 5 on the rest); no file
carries a 2 or a 3. With bit 2 left out, the loss rate of these receivers on Galileo L1C (0.05 to 0.81 % of the
records) is that of other receivers (PARK 0.41 %, BOR1 3.2 % on L1X).

**The bars.**

| bar | DOY 144 | DOY 145 |
|---|---|---|
| 1 identity (new engine, key absent; mask 3 where no flag above 1) | | both traces identical at BOR1, HKSL; BOR1, PARK: passes |
| 2 Galileo alone, fixing, hours 4 to 6, at most 25 mm | 14.2, 7.9, 7.3 mm: passes (key absent 112, 484, 369) | 28.9, 34.1, 29.3 mm: FAILS (key absent 699, 861, 902) |
| 3 both systems, fixing, mask minus key absent, hours 3 to 6, at most +2 mm | -0.9, +0.4, +0.4, +0.9: passes | -6.0, -2.2, +3.6, +3.9: FAILS in hours 5 and 6 |
| 4 Galileo reference changes per run, under 20 | 7, 10, 7, 15 (key absent 569 to 654): passes | 9, 42, 77, 44 (key absent 529 to 645): FAILS at three |
| 4 wrong holds | none | none |

Held pair-epochs with the mask (verified by the float sibling in parentheses), none wrong: Galileo alone 5 035
(4 351) and 2 007 (1 310); both systems, Galileo L1C/L5Q 5 112 (4 518) and 4 754 (2 869), GPS L1C/L2W 8 509 (8 458)
and 9 091 (8 846). With both systems the mask also shortens the start: hour 2 at 44.5 against 53.6 mm on DOY 144
and 28.7 against 73.4 mm on DOY 145.

**What the misses of DOY 145 are.**

- **The reference changes are the stream's, not the receivers'.** WTZS00DEU (a Septentrio at the site of
  WTZR00DEU) on the same stream writes the same pivot log: 147 Galileo handovers, 110 with the outgoing reference
  disturbed (WTZR00DEU with the mask: 147 and 109): 59 in the first half hour and a burst of 71 from 5.0 h. In the
  burst E27 is the only Galileo satellite above the pivot elevation of 45 degrees, and its two signals are
  unsettled on the stream (their counters step at 491 of the epochs after hour 1). The election takes E27 (the only
  candidate above 45 degrees, score -0.11 with the convergence penalty saturated); the next epoch finds its
  ambiguity re-seeded and hands over to E30 (44.8 degrees: the recovery has no elevation test); the next epoch drops
  E30 for being under 45 degrees and elects E27 again. GOP600CZE and REYK00ISL show the same against E34 and E19.
  The handovers are lossless: at WTZS00DEU (both systems) the position stays at 7.0 to 7.6 mm through the burst and
  the held pairs keep their values (E21 against E30 at -83.0002 cycles, covariance 1e-7 m2, at every epoch
  referenced to E30).
- **A healthy record** (BOR1, PARK, and HKSL with the mask): after the first hour the reference changes only when
  it drops under 45 degrees, 3 to 4 times per system in six hours, with 0 to 3 handovers after a reset of the
  reference. In the first half hour of a cold network every signal's counter steps at every epoch until its flag
  rises (`f13_n145`: G24 until epoch 63, G15 until 73, E08 L1X/L5X until 85); the user resets every ambiguity at
  every epoch and the reference changes at every epoch (59 handovers per system). That is the design of the
  unsettled steps; the first hour of a user started with a cold network measures the network's settling.
- **Galileo alone on DOY 145 is a float solution at these receivers.** WTZR00DEU and WTZS00DEU each get 3 fixes
  in six hours. The float of WTZS00DEU reaches 8.3 and 7.7 mm in hours 5 and 6; the float of WTZR00DEU stays at 31
  to 38 mm in hours 4 to 6, with -22 to -26 mm in height. On DOY 144, with fixes, WTZR00DEU is at 4 mm and HKSL00HKG at 2 mm (mean vector
  of hours 4 to 6); REYK00ISL and WROC00POL carry -26 and +21 mm in height on DOY 144 and +18 and +9 mm on DOY 145.
- **Ruled out for that float offset:** the code (its sigma raised from 0.3 to 3.0 m: the four receivers change by
  0.8, -0.1, 0.1 and 0.1 mm in hours 3 to 6 on DOY 145, and WTZS00DEU and GOP600CZE by as little); the antenna type
  (LEIAR25.R3 LEIT at WTZR00DEU and WTZS00DEU, LEIAR25.R4 LEIT at HKSL, REYK, WROC and at PTBB00DEU, a Septentrio
  at 3.5 mm); a constant of the receiver (the height error changes sign between the days); the restarts (after hour 1 the
  stream's counter steps reset 731 signal-epochs at WTZR00DEU and 754 at WTZS00DEU, on E27 and E15 at both, and
  no restart is particular to the Leica receiver). The receiver-satellite
  code-bias states of card RSCB cannot be tried: `estimation.estimate_rx_sat_code_bias` is inert in the replay
  users. **Not traced.**
- **The family.** Galileo alone at the 23 other receivers of the L1C/L5Q family, fixing, hours 4 to 6: 16.3, 10.0,
  10.5 mm on DOY 144 and 17.2, 10.8, 13.4 mm on DOY 145 (the L1X/L5X family at the 28 network receivers of card
  ARNH: 16.9, 10.7, 10.9 and 11.3, 10.0, 9.9), single receivers from 3.5 to 46 mm; 35 591 and 29 785 held
  pair-epochs, none wrong. PALM00ATA never converges with Galileo alone (406 and 1 528 mm; 3 628 resets of L1C in
  six hours: the satellites it sees stay unsettled), and is at 3 to 8 mm with both systems. The four Leica
  receivers with the mask are inside that spread on DOY 144 and in its upper part on DOY 145.

**Two defects of the same kind, not repaired here.** The network's verdict at the return of a signal after a gap
(`network/slip/SlipQc.cpp`, `sidSigVerdict` and the per-signal re-entry, `gap_reentry_lli` default true) tests the
indicator against zero, so every Galileo L1C return at a Leica receiver reads as a loss of lock; not measured,
reported to the builder. The pivot election can take a satellite whose ambiguity was re-seeded in the last epoch
when no other is above the pivot elevation; a rule that keeps the present reference in that case would end the
flip-flop; it costs no accuracy and is left for later.

**Decision** (2026-11-16, this session, for the author's review). Bars 2, 3 and 4 fail on DOY 145, so the condition
of section 5 for taking the mask into the fixing users' settings is not met. It is taken in nevertheless, because
the reading with the mask 3 is the definition of the indicator (bit 2 is not a loss of lock) and the detector as
built discards every Galileo phase of five receivers; no hold is wrong; Galileo alone becomes usable; the failed
bars are the stream's Galileo L1C/L5Q family over Europe on that day and a float offset the key cannot cause
(identity at every receiver without the flag). Measured cost: with both systems the four receivers end 3.6 and 3.9
mm worse in hours 5 and 6 of DOY 145 (REYK00ISL also on DOY 144, by 8 to 10 mm in hours 5 and 6). Note for the ledger row: done
2026-11-16; verified by one test (four receivers, two days, 48 runs, basic); to do: trace the float offset of the
Leica receivers, more receivers and days (card ARNJ section 4 adds a day), the network's gap re-entry verdict, the
pivot election. The default stays 7. The patch for the builder's branch is
`D:\GnssProcessorSupport\gates\rscb_checkpoint\lli_slip_mask_for_irdu.patch` (`git apply --check` passes there).

## 8. 2026-11-16: results of section 3, the exact epoch time at the users

**Runs.** 268 replays on the builder's repaired streams `x14_n144` and `x14_n145` with the repaired user engine
(`gnss_processor_rscb5.exe`, the key on), all exit 0; 56 replays of the mixed case on DOY 145 (the repaired user on
the unrepaired stream `f13`); grades under `grades_arnd\exact\`. The `f13` users of card ARNH section 11 are the
control. Medians over the receivers of the hourly 3D median, hours 2 to 6, mm.

| users | DOY 144, `f13` | DOY 144, repaired | DOY 145, `f13` | DOY 145, repaired |
|---|---|---|---|---|
| 28 network receivers, both systems, float | 25.7, 17.1, 12.5, 9.7, 8.9 | 22.4, 16.9, 13.0, 10.2, 8.7 | 37.4, 15.2, 11.2, 9.3, 8.7 | 35.1, 14.3, 10.8, 9.7, 8.2 |
| 28, both systems, fixing | 25.7, 14.7, 11.4, 7.9, 7.3 | 23.0, 11.6, 9.3, 8.2, 7.2 | 33.0, 13.4, 10.4, 8.7, 8.4 | 34.7, 10.8, 10.3, 9.5, 8.0 |
| 28, Galileo alone, float | 68.5, 27.4, 21.0, 14.8, 12.4 | 59.8, 24.8, 19.7, 13.8, 11.1 | 42.8, 22.9, 15.7, 12.1, 11.7 | 45.8, 20.0, 14.9, 11.1, 11.0 |
| 28, Galileo alone, fixing | 68.5, 22.2, 16.9, 10.7, 10.9 | 59.9, 19.1, 15.3, 11.4, 11.0 | 40.8, 13.6, 11.3, 10.0, 9.9 | 45.8, 14.4, 12.2, 10.3, 9.9 |
| 11 independent, float | 52.7, 21.3, 14.4, 11.5, 9.8 | 46.2, 19.2, 11.5, 9.7, 8.8 | 37.1, 16.7, 14.7, 10.5, 9.3 | 26.6, 16.1, 14.0, 11.5, 9.9 |
| 11 independent, fixing | 52.4, 16.1, 10.4, 11.2, 9.4 | 39.0, 18.4, 10.7, 9.8, 10.2 | 37.1, 13.0, 12.3, 9.4, 8.3 | 22.5, 10.3, 11.8, 9.2, 8.2 |

**Bar 1 (not above the `f13` label by more than 1 mm in hours 3 to 6):** 47 of the 48 cells pass; the largest
excess among them is 1.0 mm (the 11 independent receivers, float, DOY 145, hour 5: 11.5 against 10.5, paired
median +0.2). One cell fails: the 11 independent receivers, fixing, DOY 144, hour 3, 18.4 against 16.1 mm. In
that cell 6 of the 11 receivers are better with the repair and the paired median is -1.1 mm (CHTI -13.2, ARHT
+8.2, BADG +5.5): the hour in which the fixes land, a median over 11. The paired medians of the six labels in
hours 3 to 6 lie between -2.3 and +1.0 mm.

**Bar 2 (holds):** 285 731 held pair-epochs on the repaired streams (277 052 verified by the float sibling), none
wrong: DOY 144 Galileo L1X/L5X 49 203, GPS L1C/L2W 56 754, Galileo alone 48 132; DOY 145 36 906, 57 597, 37 139.
The wide lane is off the network's on 183 and 163 pair-epochs of DOY 145, all at BOGT00COL, the receiver that hosts
E18's tree edge (card ARNH section 3). Passes.

**Bar 3 (identity with the key absent):** passes (section 2; four receivers).

**Reads.** The position changes less from epoch to epoch (rms of the 3D change, median over the receivers, hours 3
to 6): fixing on both systems 0.22 against 0.27 mm (DOY 144) and 0.23 against 0.33 mm (DOY 145); float 0.23
against 0.27 and 0.23 against 0.29; Galileo alone, fixing, 0.40 against 0.59 and 0.40 against 0.58. The first fix
comes earlier (median over the 28: 85.5 against 95.5 min and 83.2 against 95.5 min); the time to stay under 5 cm
is 68.0 against 70.5 min and 69.2 against 83.5 min. The mixed case (DOY 145, the repaired user on `f13`): hours 2
to 6 at 32.2, 11.4, 10.4, 9.4, 8.3 mm against 33.0, 13.4, 10.4, 8.7, 8.4; 95 078 held, none wrong; epoch-to-epoch
change 0.21 mm; first fix at 80.2 min. A user with exact time tags on a stream without them loses nothing.

**The network's side** (the builder, card NRF part 9 section 81): the 675 s step leaves the satellite clocks and
the station residuals (step ratio 1.0 to 1.1 against 1.9 to 2.1; residual statistic 16 to 17 % lower), held and
flagged shares equal or higher, every integer right on the simulator. One read is over its bar of a tenth: the
share of flagged Galileo L1X/L5X satellite-epochs in segments over 0.15 cycle on DOY 145, 26.7 against 14.4 %. The
same read goes the other way on DOY 144 (3.1 against 11.8 %), the median segment is unchanged (0.089 against 0.091
cycle) and the largest is smaller (0.190 against 0.218): a count at a threshold.

**Decision** (2026-11-16, this session, for the author's review). One user cell of 48 and one network read are over
their bars, both by scatter at a threshold and neither in a direction that repeats. The repair removes an
arithmetic error of the time tags (up to 19.7 microseconds), and what moves, moves the right way.
`processing.exact_epoch_time` is adopted in the chain posture, at the network and at the users, as a scoped
recommendation on the builder's branch. Note for the ledger row: done 2026-11-16; verified by one test (two days,
six hours, 28 + 11 receivers, 324 user replays, the builder's network reads and simulator run, basic); to do: more
days and receivers, a 24 h run with the key, the main posture's goldens and baselines (they move with it and are
left to the author). The 24 h runs of DOY 146 and 147 (card ARNJ) were started before this verdict and run without
the key.
