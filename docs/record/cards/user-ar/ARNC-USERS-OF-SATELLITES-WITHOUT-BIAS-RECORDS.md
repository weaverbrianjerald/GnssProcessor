# ARNC: a user's satellites without their own bias records on the network's stream

**STATUS**

- **id:** ARNC
- **topic:** user-ar
- **status:** closed
- **summary:** Continuation of card ARNB (that file passed the 60 KB bar). On the network's stream one Galileo satellite-epoch in eight offers a receiver of the L1C/L5Q type no complete pair of bias records, although the stream carries the satellite on L1X/L5X. A user takes such a satellite with the clock alone, and on the stream with the emission hold that is not a phase correction: the clock record wanders by 83 mm (median, 30 min windows) in the network's first hour and the phase-bias record that cancels it is missing. Two remedies were measured offline on DOY 144 at three receivers: the satellite left out while it lacks the record (card ARNB section 11) and a fallback to the other signal pair's records (this card). Either brings a user who starts with a cold network to a decimetre in 13 to 29 min in place of 47 to 52 at two of the three receivers; the two agree within 2 mm in 23 of 30 receiver-hours; the code bias alone (the existing opt-in key, inert on the replay path) changes nothing. Of the pre-registered bars F-2 passed and F-1 and F-3 failed, both set tighter than one day at three receivers resolves.
- **dates:** 2026-11-14 (section 2 pre-registered before its runs, written as section 13 of card ARNB; section 3 the results)
- **knobs / posture:** none new. `processing.ssr_bias_band_fallback` (existing, default off) was read and found inert on the replay path; it is not changed.
- **code:** none. Analysis tools only (`stream_fallback.py`, `nw_fb.py`, `conv_curve.py` beside the tools of card ARNB).
- **run dirs:** `D:\GnssProcessorSupport\runs_nom\_rap_nw_obs{h18,w18,h00,c00}{f,k}_144_{harb,dav1,scrz}` (user replays), stream copies `_rap_{nww24,nwh24,p26}{f,k}_n144`, the key smoke `_rap_nw_obsc00ks_144_harb`; tools `D:\GnssProcessorSupport\presentation\agu26\fixing_control\tools\`.
- **source:** card [ARNB](ARNB-USER-FIXING-ON-NETWORK-PRODUCTS-CONTINUED.md) sections 9 to 12 (the lead, its test by removal, the reading of the stream's jumps)

**Outcome.** Closed 2026-11-20; measured on one day at three receivers, offline. A satellite that a user takes with the stream's
clock and without its phase-bias record harms a user who starts with a cold network: the hold splits one phase
correction over the two records. Leaving the satellite out or falling back to the other signal pair's records
removes the harm alike (a decimetre in 13 to 29 min in place of 47 to 52). The code bias alone does not. Nothing is
built; three options in the user software are named for the author's decision.

> Results are appended TO THIS FILE, below the text.

---

## 1. Scope

Card ARNB passed the 60 KB bar with these results; they continue here. Sections 2 and 3 were written on 2026-11-14
as sections 13 and 14 of card ARNB (the pre-registration before any run of it) and moved here the same night. Their
text is unchanged except for the section numbers and the references to card ARNB's sections. Card ARNB's sections 9
to 12 hold what leads here: the shared drift of the holds-off stream, the satellites without their records at the
test receivers, the test by removal and the reading of the stream's jumps. No network code, no key, no golden
change, no bar frozen. The rulings of 2026-11-09 stand.

## 2. 2026-11-14 (night): the fallback in place of the removal, pre-registered before any run

The author's go-ahead of 2026-11-14 ("Yes, test it") covers item 1 of card ARNB section 11's "Named, not run". This section
was written before any user run of it; the copies of the product files exist.

**The rule** (`stream_fallback.py`). Galileo has two signal pairs on the stream: C1C/C5Q with L1C/L5Q, and C1X/C5X
with L1X/L5X. Per epoch and satellite: when the phase records of one pair are incomplete (none or one of two) and
the other pair holds its four records, the incomplete pair's four records are written from the other pair's, band
by band. A record of the incomplete pair that was present is replaced too, so the two bands of a satellite never
mix the two pairs. Other constellations, the clocks and every other record are untouched.

**How often the case arises** (the 24 h stream, 85 954 Galileo satellite-epochs): both pairs complete on 73 401
(85.4 %); L1C/L5Q absent with L1X/L5X complete on 8 305 (9.7 %); L1C/L5Q on one signal with L1X/L5X complete on
2 597 (3.0 %); L1X/L5X absent with L1C/L5Q complete on 1 430 (1.7 %). One Galileo satellite-epoch in eight offers a
receiver of the L1C/L5Q type no complete pair of bias records although the stream carries the satellite.

**The copies.** `_rap_nww24f_n144`, `_rap_nwh24f_n144` and `_rap_p26f_n144`, each identical to its source outside
the Galileo code-bias and phase-bias records (compared with those lines removed). On the 24 h streams 10 902
satellite-epochs take L1C/L5Q from L1X/L5X and 1 431 the reverse (2 878 and 715 on the 6 h stream); 266
satellite-epochs have no complete pair and stay as they are.

**The hand-over** between a satellite's own records and the fallback is a step of the phase bias, and the reader
must see it as one. On the streams with the hold each fallback record carries a generation of its own (1 000 000
and above), so every hand-over is a change of generation, on which the user software re-floats the ambiguity. On
the holds-off stream the reader's jump test declares a step above 0.25 cycle; none of the 257 hand-overs of phase
records stays below it there (one does on the 24 h stream with the hold, E09 L1C by 44.6 mm, where the generation
declares it). The code biases of the two pairs agree to 8 to 17 mm in the median on the first band (largest 54 mm)
and to 1 to 4 mm on the second (largest 11 mm), on the stream as built as on the holds-off stream.

**The test.** The earlier runs' configurations with the product path set to the copy and nothing else changed (the
receiver's own observation file, the same executable). Arms h18, w18, h00 and c00 of card ARNB section 10 at HARB00ZAF,
DAV100ATA and SCRZ00BOL: 12 replays, no fix accepted (`nw_fb.py`). Smoke: 10 epochs at HARB00ZAF from 00:00 on the
6 h copy; E23 and E26, without a phase bias in the earlier run's first epochs, must carry code and phase biases in
the trace.

**Reading.** The 3D median per hour for three treatments of the same satellite-epochs: *as it was* (used with the
clock alone), *left out* (card ARNB section 11) and *fallback*.

**Bars, on the stream as built** (30 receiver-hours: HARB00ZAF and DAV100ATA from both starts and SCRZ00BOL from
00:00, hours 1 to 6):

- *F-1.* The fallback is within 2 mm of the better of the two other treatments, or better, in at least 24 of the
  30 receiver-hours.
- *F-2, the gain kept.* In the first hour after a cold start the fallback reads below 100 mm at HARB00ZAF and at
  DAV100ATA (as it was 248.2 and 252.3 mm; left out 52.8 and 65.3 mm).
- *F-3, the cost avoided.* In the 12 receiver-hours where leaving out cost more than 1 mm (DAV100ATA from 18:00,
  hours 1 to 6; HARB00ZAF from 00:00, hours 4 to 6; SCRZ00BOL from 00:00, hours 4 to 6) the fallback is not worse
  than "as it was" by more than 1 mm in at least 9.

**The holds-off stream** is reported beside the bars and not scored. Expected: in hours 4 to 6 no worse than the
removal by more than 5 mm. A satellite on the fallback follows the other pair's drift, and the difference between
the two pairs' shared drifts is ionosphere-like (card ARNB section 10, item 4).

**Prediction.** F-1, F-2 and F-3 pass. What would speak against the fallback: an hour-1 reading near "as it was",
which would put the harm in these satellites themselves and not in their missing records.

**Limits.** One day, three receivers, float positions, offline on copies of product files. Nothing is built in the
user software: there the fallback would be a lookup of the other pair's record where a satellite's own is missing,
with the hand-over handled as a declared step.

**Addendum to section 2, written before the runs: a second treatment, the code biases alone.** The user software
holds an opt-in key for this very case, `processing.ssr_bias_band_fallback` (default off): a code signal without a
record takes the code bias of the same band's other signal, and the phase never falls back, "because a
wrong-attribute phase bias breaks the integer property" (`GnssConfig.hpp`). Its periodic log line counts the case in
the earlier runs ("tracked signal missing while a same-band sibling bias exists": at HARB00ZAF from 00:00, 1 515
code and 7 486 phase lookups by 05:00).

The key cannot be tested as it stands on the replay path. It is armed on the process-wide store
(`RealTimeEngine::initialize`) and on the store of `Application::runRealTime`; neither the replay consumer
(`ApplicationConsumerReplay.cpp`) nor the in-process closed-loop consumer (`ApplicationNetworkSetup.cpp`) arms it on
the store it owns, so on those paths the key is inert. What it would do is emulated in copies:
`stream_fallback.py --code-only` writes a missing code-bias record from the same band's other signal and leaves the
phase records as they are (`_rap_nww24k_n144`, `_rap_nwh24k_n144`, `_rap_p26k_n144`, each identical to its source
outside the Galileo code-bias records). Twelve more replays (`_rap_nw_obs<arm>k_144_<site>`).

Reported beside the three treatments, without a bar. Expected: between "as it was" and the fallback of both kinds of
record. On the streams with the hold the phase record moves with the clock by centimetres per epoch (E23 L1C by
13.5 mm between the first two epochs of the 6 h stream), which a code bias alone does not supply.

## 3. 2026-11-14 (night): the fallback in place of the removal, results

**Runs.** Smoke passed (E23 and E26 carry code and phase biases; no row without a phase bias). 24 replays, all exit
0 (`_rap_nw_obs{h18,w18,h00,c00}{f,k}_144_{harb,dav1,scrz}`). The replays on the fallback copies hold no row without
a phase bias and keep the row counts of the earlier runs; the replays on the code-only copies hold no Galileo row
without a code bias and keep the rows without a phase bias (2 351 at HARB00ZAF from 00:00). The hand-over is seen by
the user software: HARB00ZAF on the copy of the 24 h stream, E02 at 19:46, `reset_hold_gen` on the phase side and
`reinflate_disc` on the code side.

**3D median per hour** (mm, hours 1 to 6), four treatments of the same satellite-epochs.

*Stream as built*

| receiver, start | as it was | left out | code biases alone | fallback |
| --- | --- | --- | --- | --- |
| HARB00ZAF, 18:00 | 36.7, 12.7, 30.2, 18.0, 15.8, 16.8 | 29.2, 7.7, 25.8, 14.6, 12.1, 11.9 | 37.0, 13.3, 30.1, 17.9, 15.7, 16.8 | 18.9, 9.6, 27.2, 16.1, 13.8, 13.1 |
| DAV100ATA, 18:00 | 25.2, 29.6, 16.9, 15.5, 16.2, 10.5 | 26.9, 32.3, 19.2, 17.8, 18.2, 11.6 | 25.4, 30.2, 17.1, 15.6, 16.2, 10.5 | 27.4, 34.0, 19.1, 17.1, 16.5, 10.4 |
| SCRZ00BOL, 18:00 (41 min) | 90.1 | 106.1 | 90.4 | 90.4 |
| HARB00ZAF, 00:00 | 248.2, 28.9, 7.4, 7.0, 7.3, 6.8 | 52.8, 15.4, 8.3, 11.1, 9.8, 8.2 | 248.8, 29.3, 7.5, 7.1, 7.4, 6.8 | 56.1, 18.7, 11.5, 12.5, 10.3, 7.7 |
| DAV100ATA, 00:00 | 252.3, 71.5, 24.1, 14.2, 12.2, 14.1 | 65.3, 33.5, 10.7, 8.7, 8.1, 8.9 | 252.3, 71.5, 24.1, 14.2, 12.2, 14.0 | 77.0, 35.2, 12.4, 9.6, 8.3, 5.9 |
| SCRZ00BOL, 00:00 | 136.6, 65.1, 47.0, 35.5, 30.0, 24.3 | 118.6, 65.3, 47.1, 37.4, 32.0, 27.1 | 136.3, 65.3, 47.4, 37.5, 32.0, 26.9 | 136.3, 65.3, 47.4, 37.5, 32.0, 26.9 |

*Holds-off stream*

| receiver, start | as it was | left out | code biases alone | fallback |
| --- | --- | --- | --- | --- |
| HARB00ZAF, 18:00 | 45.2, 29.8, 47.6, 28.0, 45.8, 55.1 | 26.3, 14.9, 40.9, 29.7, 43.5, 47.4 | 45.9, 31.0, 47.6, 27.9, 45.7, 55.0 | 21.8, 17.5, 42.8, 31.9, 48.2, 55.7 |
| DAV100ATA, 18:00 | 18.8, 9.1, 6.8, 11.0, 10.5, 15.9 | 26.5, 7.3, 7.7, 11.5, 13.7, 16.1 | 18.9, 9.5, 6.8, 11.1, 10.5, 16.0 | 22.8, 6.8, 6.2, 9.1, 12.8, 15.9 |
| SCRZ00BOL, 18:00 (41 min) | 69.0 | 76.8 | 70.0 | 70.0 |
| HARB00ZAF, 00:00 | 890.1, 111.5, 89.8, 49.9, 37.7, 31.1 | 567.1, 193.3, 146.3, 48.4, 42.9, 33.7 | 889.2, 110.6, 89.0, 49.6, 37.7, 31.1 | 617.7, 168.9, 128.2, 46.0, 40.4, 31.8 |
| DAV100ATA, 00:00 | 975.9, 277.9, 97.0, 67.2, 50.1, 41.0 | 471.0, 108.0, 45.5, 62.5, 43.7, 24.7 | 976.1, 278.6, 97.2, 67.2, 50.1, 41.0 | 467.1, 93.8, 45.8, 60.1, 42.1, 22.1 |
| SCRZ00BOL, 00:00 | 730.5, 35.1, 21.2, 34.4, 22.9, 62.7 | 674.5, 32.7, 21.8, 26.8, 21.2, 56.4 | 728.3, 37.2, 20.1, 24.5, 20.9, 56.5 | 728.3, 37.3, 20.1, 24.4, 20.9, 56.6 |

**The bars** (stream as built, `nw_fb.py bars`):

- *F-1 FAILS:* the fallback is within 2 mm of the better of the two other treatments, or better, in 19 of 30
  receiver-hours (bar 24).
- *F-2 PASSES:* first hour after a cold start 56.1 mm at HARB00ZAF and 77.0 mm at DAV100ATA (as it was 248.2 and
  252.3; left out 52.8 and 65.3).
- *F-3 FAILS:* in the 12 receiver-hours where leaving out cost more than 1 mm the fallback is within 1 mm of "as it
  was" in 3 (bar 9). It costs what leaving out costs: +2.2, +4.4, +2.2, +1.6, +0.3, -0.1 mm at DAV100ATA from 18:00
  (left out +1.7, +2.7, +2.3, +2.3, +2.0, +1.1); +5.5, +3.0, +0.9 at HARB00ZAF from 00:00 (+4.1, +2.5, +1.4); +2.0,
  +2.0, +2.6 at SCRZ00BOL (+1.9, +2.0, +2.8).

The prediction (three passes) failed on F-1 and F-3.

**Convergence** (`conv_curve.py`; the minute from which the 3D error stays below 100 mm, and below 50 mm, within
the first 120 min; stream as built):

| receiver, start | as it was | left out | code biases alone | fallback |
| --- | --- | --- | --- | --- |
| HARB00ZAF, 00:00 (cold network) | 52.0, 63.5 | 13.0, 45.0 | 52.5, 63.5 | 12.5, 48.0 |
| DAV100ATA, 00:00 (cold network) | 47.0, 109.5 | 18.5, 47.5 | 47.5, 109.5 | 28.5, 51.0 |
| SCRZ00BOL, 00:00 (cold network) | 33.0, 120.5 | 33.0, 120.5 | 33.0, 120.5 | 33.0, 120.5 |
| HARB00ZAF, 18:00 (network 18 h old) | 13.5, 20.0 | 11.0, 12.5 | 13.5, 20.0 | 10.5, 17.0 |
| DAV100ATA, 18:00 (network 18 h old) | 9.5, 16.0 | 10.0, 17.0 | 9.5, 16.0 | 10.0, 17.0 |

**What the readings say.**

1. *The harm is the phase without its phase-bias record, and it is a cold-start harm.* With either remedy a user
   who starts with the network reaches a decimetre in 13 to 29 min at HARB00ZAF and DAV100ATA in place of 47 to 52
   min, and 5 cm in 45 to 51 min in place of 64 to 110. On a network 18 hours old the gain is minutes at one
   receiver and none at the other.
2. *The code bias alone changes nothing:* within 0.6 mm of "as it was" in every receiver-hour at HARB00ZAF and
   DAV100ATA on the stream as built (1.2 mm on the holds-off stream). The existing key would not remove the harm.
3. *Fallback and removal are equal for these users:* within 2 mm of each other in 23 of the 30 receiver-hours; the
   seven others are four first hours (-10.3, +3.3, +11.7, +17.7 mm), two second and third hours at HARB00ZAF from
   00:00 (+3.3, +3.2) and one sixth hour (-3.0). The fallback re-floats more often: 58 phase resets at HARB00ZAF
   from 18:00 against 17 with the satellites left out and 44 as it was (a copy of a product file can only declare
   the hand-over; see "Named, not built").
4. *The few millimetres of the later hours have no consistent sign.* Against "as it was" the fallback is better by
   more than 2 mm in 13 receiver-hours and worse by more than 2 mm in 7. The cost that bar F-3 was to avoid is not
   lost geometry, because the fallback keeps the satellite and pays the same. Those hours sit on a height error of
   15 to 35 mm common to every treatment (DAV100ATA from 18:00: mean Up -10 to -34 mm in each), which the
   treatments move by a few millimetres either way. Bars F-1 and F-3 were set at 1 to 2 mm, tighter than one day at
   three receivers resolves: a weakness of the bars, recorded and not re-scored.
5. *SCRZ00BOL does not respond.* Its positions are identical to 0.1 mm under "as it was", the code biases alone
   and the fallback at 01:00, and its hourly medians under the last two agree to 0.1 mm. Its Galileo ambiguities
   stand at 0.17 to 0.23 m after an hour whether or not the satellite has its records (GPS 0.03 to 0.08 m; at the
   two other receivers Galileo reads 0.02 to 0.05 m), so its Galileo phase carries little weight. Not traced.
6. *Holds-off stream* (not scored). Expected: in hours 4 to 6 the fallback no worse than the removal by more than
   5 mm. Met in 14 of 15 receiver-hours (HARB00ZAF from 18:00, hour 6: +8.3 mm). After a cold start the two
   remedies again agree at DAV100ATA (first hour 976 to 467 and 471 mm) and both leave HARB00ZAF's second and third
   hours worse than as it was (111.5 and 89.8 against 168.9 and 128.2, and 193.3 and 146.3).

**Traced: why the clock alone hurts on the stream as built** (closes the first "Not traced" of card ARNB section 11 for that
stream). The hold runs in its phase-clock mode (`pbhold=2`): the phase-bias record follows the clock record, and
clock less phase bias is the constant of a generation (Galileo, hours 18 to 24: the clock and the phase bias each
change by 0.5 mm rms between consecutive epochs, their difference by 0.0 mm). The clock record is the network's
code-level clock, and it wanders against the constellation median: by 83 mm in the median over satellites (p90
297 mm) inside 30 min windows of the network's first hour, by 24 mm (35) in its second hour, by 7 mm (13) in hours
19 to 24, and by 15 mm (34) inside 2 h windows there. A satellite corrected by the clock alone takes that wander
into its phase; a satellite with its phase bias takes none. At the user: HARB00ZAF, stream as built, 18:30 to
19:45, the float ambiguity of E02 (no phase bias) ranges over 112 mm against 21 to 35 mm for four satellites with
their records. The sizes match the test: decimetres in the first hour of a cold network, millimetres to
centimetres later.

**The existing key on the replay path** (verified by a run, `_rap_nw_obsc00ks_144_harb`): with
`processing.ssr_bias_band_fallback` true the replay logs "SSR code-bias band fallback ON" twice and the satellite
trace is identical byte for byte to the earlier run over the 11 epochs of a smoke. Armed and inert there.

**Named, not built; each is the author's decision.** In the user software, for a stream that declares the hold:

1. *The guard:* a satellite with a clock record and no phase-bias record for the receiver's signal is left out
   until the record arrives (what card ARNB section 11 tested). Card IRDU's rule would do it once its criterion is the
   phase-bias record and not the clock record.
2. *The fallback in the correction store:* the other signal pair's phase bias, with the ambiguity kept float (the
   record's integer flag cleared) and the hand-over translated by the step, which the two records present at the
   hand-over epoch give exactly, so no re-float is needed. Equal to the guard in accuracy here; its case is a user
   with few satellites or in motion, not tested.
3. *The existing code-bias key armed on the stores that the replay consumer and the in-process closed-loop consumer
   own.* It does not address this harm.

**Limits.** One day, three receivers, static float positions, offline on copies of product files. No fixing run,
no network code, no key. The fallback records of the copies carry the other pair's integer flag unchanged.

## 2026-11-20: closed

Closed by the managing session's choice of 2026-11-20, under the author's deferral of the consolidation decisions ("I will defer to your choices on the consolidation 'decisions for the author'"); applied by the docs consolidation on branch `docs/consolidate-2026-11-20`, in the batch of the user series where the card's own text shows nothing owed. The user-engine code of this line is on main since 2026-11-17.
