# SCL2B: the shared history with card SMH's rows (X2b), graded, and X2 with X2b landed default off

**STATUS**

- **id:** SCL2B
- **topic:** network-vehicle
- **status:** open
- **summary:** 2026-10-07, the continuation of card SCL2 (whose file is near the 60 KB bar): amendment A1 of card SCL2 section 11 built and graded. X2b projects card SMH's hold rows onto X2's shared history (no new knob), so the hold's past clock receives the revision the history carries. The solve is byte-identical to X2 (only phase-bias records move), so the level gates read as X2's: de-commoned level 0.017 / 0.019 m in hour 6 against B1 alone's 0.042 / 0.045 (broadcast / COD), the statement consistent. On the consumer side A1 repairs what X2 broke: the hold's walk-share error on the COD bed 7.0 to 8.8 mm (X2 14.3 to 16.9, B1 11.0 to 11.8); the sim census +0.0034 m with 2 stations worse (X2 +0.0120 / 5; B1 +0.0049 / 2); the field census median +0.0030 m (B1 +0.0034), against B1 alone -0.0036 and 0 sites worse; WTZZ00DEU within the bar. Still failing: G1's ratio clause (the code's realization, card SCL2 section 12), G4's site count (4 against 3; B1 alone 5, the same sites), and SZZ000SIM in hours 2 and 3 (+0.028 / +0.040 m against B1 alone on beds sharing one realization). Cost 1.100 of B1 alone. X2 and X2b landed default off (section 5, `70fef50`); nothing armed. A second noise realization (sections 7 to 9): X2b follows that realization's code (0.93 / 0.97) and equals B1 alone on the census, while its hour-6 level (0.028 / 0.025 m) misses the 0.020 bar because even an exact code follower lands at 15 to 28 mm across six realizations; the shared per-satellite component is the simulator's code multipath plus chance (no deterministic leak); the in-loop consumer's hours 2 and 3 are the history's revision of the ignition clones reaching card SMH's walk share, a 2 to 4 cm push that helps or hurts by the consumer's own error sign (from-start in-network census better on 3 of 4 beds; held out within a centimetre). The sim census replays started at 01:01:30 (the start offset applied twice); `SIMCENSUS_FROM_START=1` (section 10).
- **source:** [SCL2](SCL2_level_residue_under_the_clone_2026-10-06.md) sections 5, 6 and 9 to 12 (amendment A1 and its gates in section 11); [SCL](SCL_self_contained_clock_level_2026-10-03.md) sections 9 and 10; [SMH](SMH_smoothed_hold_clone_2026-09-29.md)
- **knobs / posture:** `network.carry_coupling_shared_history` (default off) and `network.carry_coupling_shared_history_rank` (default 1, range 1 to 8), requiring `network.carry_coupling_clock_clone` (card SCL's B1, which requires `network.handover_stransform_dense`); the hold rows' projection is automatic when card SMH's smoothed hold is armed with X2. Fingerprint token `shx:<k>` when armed (fresh convergence only). Nothing armed; no golden, ledger or posture change
- **dates in the text:** 2026-10-07
- **hashes / run dirs:** exes `gnss_processor_scl2x.exe` (X2, `da3114428f2bcc5e`), `gnss_processor_scl2xb.exe` (X2b, `5712591b77409b1d`), the base `gnss_processor_lres.exe` (`8a1aa9255f3321d8`); arms `runs/_rap_scl2xb_*` (G0, ON), `runs/_rap_scl2h_*` (gates); census keys `s2xb` (field), `s2xbs` (sim); the patch `runs/_scl2_handoff/x2b/x2x2b_full.diff` (22 files); the session's reports in `runs/_scl2_handoff/x2b/`

## 1. What this card continues

Card SCL2 built X2, the shared history: B1's per-station harvest replaced by a bounded set of history columns
that carry the clone's shared conditional across stations. X2 removed the early level lock (section 10) but card
SMH's smoothed hold kept B1's per-row form, so its past-clock estimate missed the history's revision and the
consumer paid on the phase side (section 11). Amendment A1 (pre-registered in card SCL2 section 11 with its gates,
before any code) projects each hold row onto the coupling rows' kept directions. Section 12 of card SCL2 showed the
remaining level excursions follow the code's realization.

## 2. The build (X2b)

At the update before emission, card SMH's hold rows move their clone and history terms onto the border as before,
restore the clock-family deficit, and keep their Y coefficients pending instead of adding their per-row conditional
variance; at the harvest's end the pending rows are projected on that epoch's Cholesky factor and kept directions
(history coefficients F_T V_kt, remainder ||F_T V_rest||^2 to the row's variance); a row still pending at the hold's
next harvest takes the per-row term. A row is only projected on its own epoch's compression (the hold's regression and
the harvest's agree on 235 of 235 epochs, maximum difference 0). Card SMD's block rows are included. Files: the X2 set
plus `NetworkPbHoldClone.{hpp,cpp}` and `NetworkSharedHistory.{hpp,cpp}`; tests in `tests/test_coupling_long_horizon.cpp`
(U4) and `tests/test_network_shared_history.cpp` (U5 and the gauge guard, 12 cases); a `/bigobj` line for the latter.

**A counter split after the first run (recorded as a deviation).** The first ON run counted 372 hold rows as projection
failures. They fall in epochs 2 to 7, where no carried row yet had a clone or history term and no compression formed
(190 rows in five clone epochs without a carry harvest, 182 at one harvest that compressed nothing); they take the
per-row term, as under B1. The counter was split by cause and the arms re-run; only the counter and its log line
changed, and the G0 hashes are the same in both rounds.

## 3. Gates (card SCL2 section 11's X2b table, the section 6 bars)

| gate | reading | verdict |
| --- | --- | --- |
| U0 | Debug suite 2831 tests: 2814 passed, 17 data-fixture skips, 0 failed; the golden verify PASS (69 armed); the posture check 0 gaps | PASS |
| U4 | the hold row's walk-share MSE over the batch's fixed-point smoother at epoch 240, k = 1: form p 1.0000 (static) and 1.0004 (swept); the per-row form 24.27 and 23.17 | PASS |
| U5 | the projection to 1e-12; a stale generation marginalizes; X2 off and the smoothed hold off bitwise; the gauge guard (a pinned pivot column's family sums unchanged) | PASS |
| G0 | X2 off byte-identical to the base exe on the four 2 h arms (the hashes of card SCL2 section 10; the exe of each arm recorded) | PASS |
| ON | hold rows projected 126 988 of 127 390 (6 h, broadcast sim), kept share p50 0.998, remainder p50 2.2e-3 (max 0.998); 0 projection failures where a compression formed; 0 stale marginalizations | PASS (on the split counter) |
| G1 | the solve is byte-identical to X2's: hour 6 0.0173 / 0.0186 m (bar 0.020), statement NEES 0.66 / 1.17 and 0.67 / 1.15; ratio 0.72 / 0.77; with G14 as the pivot (amendment A2) 0.0195, 0.773, 0.77 / 1.35 | hour-6 and NEES PASS; ratio FAIL (card SCL2 section 12) |
| G2 | windowed level NEES median 0.32 (broadcast), 0.58 (COD), 0.63 (G14 pivot) | PASS |
| G3 | byte-identical to X2 (the phase-clock hold does not exercise A1): SZZ000SIM and WTZZ00DEU within 0.3 mm | PASS |
| G4 | field census against `s16c`: median +0.0030 m; 4 sites worse by more than 0.02 m (chti +0.024, scrz +0.045, harb +0.052, ascg +0.103; B1 alone has the same four and dav1); against B1 alone (`s16on`) -0.0036 m, 0 worse, 1 better | median PASS, site count FAIL, B1 clause PASS |
| G5 | sim census against `s16cs`: +0.0034 m, 2 stations worse (S01, S09, as under B1 alone); against B1 alone -0.0024 m, 0 worse | PASS |
| G6 | 720 of 720 emitted, 0 datum decouples (31 kept), the sim level after the edge 0.024 against the run's p95 0.029 | PASS |
| G7 | PVN NIS 1.55 (sim), 1.46 (field); WTZZ00DEU against B1 alone -0.0006 -0.0122 -0.0001 -0.0023 -0.0077 m in hours 2 to 6; SZZ000SIM +0.0280 +0.0404 +0.0043 +0.0002 +0.0007 | NIS and field PASS; sim consumer FAIL (hours 2, 3) |
| G8 | loop time 1.100 of B1 alone (two interleaved repeats, one job at a time, 0 other processes; spread 0.3 %) | PASS |

## 4. Reading

**A1 does what it was built for.** On the COD bed, where the true walk share is 0, the hold's walk-share error falls to
7.0 7.5 8.7 8.7 8.8 mm in hours 2 to 6 (X2 14.3 to 16.9, B1 11.0 to 11.8), and the correlation of the past and current
clock errors returns to 0.90 to 0.96 (X2 0.65 to 0.84, B1 0.97). Against X2 the field consumer recovers 1.9 to 3.0 cm
in hours 4 to 6 and the field census median moves from +0.0093 to +0.0030 m; the sim census moves from +0.0120 to
+0.0034 m, better than B1 alone.

**What still fails.** G1's ratio clause measures the code's realization (card SCL2 section 12). G4's site count is the
broadcast-bed smoothed-hold posture's: B1 alone fails it with five sites, the same four among them, and no site is worse
than B1 alone by more than 2 cm. The in-loop sim consumer is 2.8 and 4.0 cm worse than B1 alone in hours 2 and 3 on the
broadcast, COD and G14-pivot beds, which share one RINEX realization; its replay shows +0.013 m in hour 2 against the B1
replay, the golden-root pair is flat in hours 2 and 3, and the census replays (from 01:01:30) do not see it. A
convergence-path draw fits; a systematic loss in the first hours is not excluded. The hold's past clock on the COD bed
is still worse than the current clock in hours 3 to 6 (23.7 / 21.3 / 20.1 / 22.0 mm against 22.1 / 18.3 / 17.0 /
18.8), and the projection drops the remainder's cross-covariance with the coupling rows (its share reaches 0.998 at a
row's first epoch): the hold is closer to the fixed-point smoother than under B1, not equal to it in production.

**Disposition.** X2 with A1 is the best clock level the producer has on the self-contained posture: the level
converges at the code-only rate and is stated consistently, and the consumer side is at or better than B1 alone on both
censuses. It is landed default off (section 5). Arming is the author's decision; the failures above are the record that
decision reads.

## 5. Landed (2026-10-07)

The X2 and X2b source (22 files, the patch `runs/_scl2_handoff/x2b/x2x2b_full.diff`) is committed on main with this
card, default off. Verified before the commit on a clean worktree of main `4c09b0a` with the patch applied: every file
byte-identical to the tested worktree; Release exe `gnss_processor_scl2L.exe` (sha256[:16] `4e26828097ae889f`); the
Debug suite 2831 tests, 2814 passed, 17 data-fixture skips, 0 failed (`NetworkSharedHistoryTest` 12 cases,
`CouplingLongHorizonTest` 5); the golden verify PASS (69 armed in regime), the posture check 0 gaps; the four G0 arms
byte-identical to the base exe (the hashes of card SCL2 section 10); the 2 h ON arm (X2 with card SMH's hold, broadcast
sim bed) byte-identical to the tested X2b exe's (`runs/_rap_scl2L_on_bs` against `runs/_rap_scl2xb_on_bs`). Another
workstream's uncommitted edits in `CMakeLists.txt` and `GnssConfigLoadNetwork.cpp` were left out of the commit (only this
patch's hunks staged).

## 6. Owed

- The in-loop sim consumer's hours 2 and 3 (a second noise realization, or the in-loop consumer on a seeded restart).
- The hold rows of epochs 2 to 7 (no compression yet) and the hold remainder's cross-covariance with the coupling rows.
- Whether the readmission seed's pre-park prior (1.2e-2 to 1.8e-2 m2) is consistent with a 12 to 55 min gap (card
  SCL2 section 12).
- The level grade that separates a lock from code-consistency (d_e minus the code-following prediction, card SCL2
  section 12) as a committed grader.
- Card SCL's owed items: the native broadcast-clock posture, a per-satellite q from the phase-visible NIS, card SMH's
  orphaned deficits at a dense edge, the census tail sites.

## 7. A second noise realization (pre-registered 2026-10-07, before any regeneration)

Every sim reading of cards SCL2 and SCL2B rests on one noise realization of `runs/simnet60_quiet_col_24h` (card SCL2
section 12: the level follows that realization's code; the in-loop consumer's hours 2 and 3 are one draw on three beds).
The read: the same fixture recipe (`config/sim/simnet60_quiet_col_24h.json`, the simulator that made it) with only the
noise seed changed (`noise.seed`; the per-station salts stay as derived), 7 h from the fixture's start, as
`runs/simnet60_quiet_col_s2`; the same OSB, clock and orbit truth, so the graders apply unchanged. Arms (6 h, the SCL
regime with card SMH's smoothed hold, RINEX re-pointed to the new realization): X2b against B1 alone on the broadcast and
the COD beds; the 16-station sim census on the broadcast pair.

Predictions (no bars change; a read for the arming decision):

1. X2b's de-commoned level in hour 6 at most 0.020 m on both beds and B1 alone's at least 0.035 m (the first
   realization: 0.017 / 0.019 against 0.042 / 0.045); X2b's statement NEES in window 691-720 in [0.5, 2.0].
2. If the in-loop SZZ000SIM excess of section 4 (+0.028 / +0.040 m in hours 2 and 3) is a draw, X2b minus B1 alone in
   hours 2 and 3 is within plus or minus 0.02 m on both beds without a shared positive sign; a repeat of +0.02 m or more
   in both hours on both beds reads as systematic.
3. The sim census X2b against B1 alone: median within plus or minus 0.005 m, at most 2 stations worse by more than
   0.02 m.
4. X2b's hour-6 level follows the new realization's code (card SCL2 section 12's code-following prediction, if the
   grader is ported by then): correlation at least 0.8.

## 8. The second realization read (2026-10-07)

`runs/simnet60_quiet_col_s2`: the col recipe with `noise.seed` 20261007 (col 20240315), 842 epochs, generated by the
same simulator binary (`gnss_simulator_sfr2.exe`, sha256[:16] `023cca23c861efca`); null gate passed (one station over 240
epochs byte-identical to the col fixture with the original seed). `noise.seed` drives the white code and phase stream only;
the multipath, phase-colour, wet-delay, receiver-clock and satellite-clock streams keep their own seeds. The truth the
graders read is unchanged; the ionosphere-free code noise of the two fixtures correlates 0.0015 per row. Arms
`runs/_rap_scl2s_{xb,b1}_{bs,cs}` (6 h, exe `gnss_processor_scl2L.exe`, the landed source), census keys `s2s_xb`, `s2s_b1`.

| prediction (section 7) | second realization | first | verdict |
| --- | --- | --- | --- |
| 1. X2b hour 6 at most 0.020 m on both beds; B1 alone at least 0.035 | X2b 0.028 (broadcast) / 0.025 (COD); B1 0.042 / 0.051 | 0.017 / 0.019; 0.042 / 0.045 | X2b FAILS, B1 holds |
| 1. X2b statement NEES 691-720 in [0.5, 2.0] | 3.04 / 2.60 (B1 13.7 / 18.2) | 1.17 / 1.15 | FAILS |
| 2. SZZ000SIM X2b minus B1 in hours 2 and 3 a draw (within 0.02 m, no shared positive sign) | +0.029 / +0.026 (broadcast), +0.018 / +0.012 (COD) | +0.028 / +0.040, +0.022 / +0.027 | FAILS as a draw; the systematic bar met on the broadcast bed only |
| 3. sim census X2b against B1 alone: median within 0.005 m, at most 2 worse | -0.0005 m, 0 worse, 0 better | -0.0024 m, 0 worse | HOLDS |
| 4. X2b's hour-6 level follows the realization's code (correlation at least 0.8) | 0.925 / 0.970 at hour 6 (0.950 / 0.970 over hours 3 to 6); B1 0.544 / 0.417 | 0.911 (COD, hours 3 to 6) | HOLDS |

**The level on this realization.** The level that follows this realization's code exactly (card SCL2 section 12's
prediction, now `scripts/clock_level/scl2_codepred.py`) is 27 mm rms over hours 3 to 6 (24 mm on the col realization), so
any estimator that follows its code misses the 0.020 m bar here; X2b tracks it to 10.5 / 7.3 mm (residual z^2 0.18 / 0.16)
and is 14 / 26 mm better than B1 alone in hour 6. A null with the wrong realization's prediction reads 0.49 / 0.52 at hour 6.

**A component that does not change with the noise seed (new, open).** Splitting the code-following prediction into the two
realizations' common part and their difference, the common part carries the excess over the bound (mean z^2 at hour 6 2.39
against 0.71 for the difference, each against its own white sd), and the two realizations' predictions correlate across
satellites at 0.30 to 0.55 by hour. G20 (-51 / -44 mm), G12 (-36 / -52) and G04 (+16 / +50) sit in it; truth-graded, G20
and G12 read -31 to -67 mm in all eight arm and realization pairs. Card SCL2 section 12's reading ("a draw of their own
code") is therefore incomplete: the fixture's code-minus-phase carries a per-satellite component of about 13 mm rms over
hours 2 to 6 that the noise seed does not change. The code multipath (its own seed, unchanged; Gauss-Markov 300 s, 2 / 3 cm
at the zenith) is the first candidate; an estimate puts its share at about 0.12 of the white variance, too small, and the
cause is open. Its bearing: the level bars and the statement NEES assume the code-only bound of white code, which the
fixture's code is not.

**The in-loop consumer.** Over both realizations and both beds X2b's in-loop SZZ000SIM is worse than B1 alone in hours 2
and 3 in 8 of 8 pairs (mean +0.024 m), and level by hour 4; the replays from 01:01:30 are flat on both realizations. A
consumer converging together with the producer in its first hours pays for X2b's early wire; a consumer starting an hour
later does not. Owed: the early-wire mechanism (the hold rows of the first epochs take the per-row term; the history is
young).

## 9. The two open threads of section 8, read (2026-10-07)

**The component the noise seed does not change: the code multipath, plus chance.** The simulator's code multipath
(Gauss-Markov 300 s, 2 / 3 cm at the zenith in L1 / L2, salted per station) has its own seed, so the col and s2 fixtures
share it exactly; its share of the code-following prediction is 6.0 mm rms at hour 6 (G20 -10.4, G12 -6.3, G04 +8.3 mm).
A fixture with the white noise and the multipath off (`runs/simnet60_quiet_col_t0`) bounds every deterministic leak of
the reconstruction at 2.6 mm per row and 0.0 mm in the prediction (the second-order ionosphere is absent from the
simulator). With the multipath removed from both, the common part's z^2 falls from 2.39 to 1.29, a 6 to 8 % tail of a
white null; four further realizations with both seeds changed (`runs/simnet60_quiet_col_t2` to `t5`) do not repeat the
pattern on average (correlation with it +0.25, -0.02, +0.29 at hour 6; G20 -33.5, -22.7, +16.8, -15.4 mm). Card SCL2
section 12's G20 and G12 are therefore the code multipath and the white noise of this fixture's seeds, not a fixture
systematic. Consequences for reading the sim level: an independent realization changes `multipath.seed` with
`noise.seed`; the code-following level's variance is about 1.12 times the white bound (the grader's scale 3.0 is then
about 3.2); and an exact code follower's hour-6 level ranges over 15.5 to 28.1 mm across six realizations (white null
median 15.8, 5 to 95 % 12.0 to 23.3), so section 7's prediction 1 (0.020 m on one realization) was not a property of the
estimator, and any absolute level bar on the sim is stated over several realizations or against the code-following
prediction (`scl2_codepred.py`), which X2b tracks at 7 to 11 mm on both realizations.

**The in-loop consumer's hours 2 and 3.** (Runs `runs/_rap_scl2t_*`, 303 consumer-only replays and four producer runs
with six stations held out.)

- The "flat replays" were a start-time artifact: a consumer replay inherits the producer arm's `start_offset_hours`
  (0.504167 on the sim) and the replay driver applies it from the wire's first epoch, so every sim replay, the census's
  included, started at 01:01:30, not 00:30:15. With the replay's offset at 0 the replays are byte-identical to the in-loop
  consumers on all eight wires. The sim census readings of cards SCL, SCL2 and SCL2B were all made from 01:01:30
  (internally consistent; a from-start census is a different statistic).
- Which side: of the hours-2-and-3 excess, X2b's code side with B1's phase side carries 0.02 and B1's code side with
  X2b's phase side 0.98; the difference is the phase-visible clock alone. When: the difference that matters grows from
  01:01 to 02:31. Mechanism: generation 1 of card SMH's hold holds 31 of 35 arcs born at 00:31; X2b's shared history
  revises their start clock D(t0 | t) for about 2.5 h, and A1's projection passes the revision into the walk share as
  phase-side motion (X2b minus B1 9 to 12 mm rms per satellite in hours 2 and 3), constant afterwards. The same component
  is A1's gain: the walk-share error in those hours is 7.0 to 7.3 mm against B1's 10.9 to 11.0.
- Who pays: every consumer converging through those hours receives a 2 to 4 cm push, mostly in Up. SZZ000SIM pays
  because its own Up error in those hours is +4 to +9 cm under B1 and the push adds to it; consumers starting from 01:01
  on gain. The from-start census of the 16 in-network stations: hour-2 median +0.0006 / -0.0126 / -0.0098 / -0.0141 m
  (col broadcast, col COD, s2 broadcast, s2 COD), the median p50 3D falling 88 to 80, 78 to 68, 101 to 82, 92 to 76 mm;
  held out (producers rerun without S05, S13, S21, S29, S45, S57; 7 external consumers): hour 2 / 3 median +0.8 / -5.1
  mm (col broadcast) and +8.5 / +1.3 mm (col COD), from hour 4 the wires agree.
- Reading: G7's sim consumer clause failed on one station's convergence sign; across consumers the effect is neutral to
  positive in-network and mixed within a centimetre held out. No producer cure is supported (not revising the ignition
  clones is X2's per-row hold, graded worse; a declared edge at the end of the build-up turns a 1 cm drift into a full
  re-convergence). Owed: a larger held-out census on both realizations before arming, read from the true start (replay
  offset 0); the replay offset itself, which `scripts/clock_level/simcensus6h.py` now exposes as an option (section 10).

## 10. The replay offset as an option (2026-10-07)

`scripts/clock_level/simcensus6h.py` gains `SIMCENSUS_FROM_START=1`, which sets the replays' `start_offset_hours` to 0 so
that they start with the wire (default unchanged: every existing key stays reproducible). Keys made the two ways are
different statistics and are compared only with keys made the same way.

## 11. A held-out census from the true start (pre-registered 2026-10-07, before any run)

Section 9 owes the external consumers' read before arming. Twelve stations are held out of the producer (S03, S08, S13,
S18, S23, S28, S33, S38, S43, S48, S53, S58, spread in longitude and latitude), on three realizations (col, s2, and t2,
whose multipath seed also differs), broadcast bed, the SCL regime with card SMH's hold, 6 h: X2b and B1 alone, 6 producer
runs per posture pair. Each held-out station is replayed as a consumer from the wire's first epoch (replay offset 0) on
its own fixture's RINEX, PBF on. Statistic: per station and realization, X2b minus B1 alone of the mean hourly p50 over
hours 2 to 6, and separately over hours 2 and 3; pooled over the 36 station-realizations. Reading (no bar changes): no
external-consumer cost if the pooled median is at most +0.005 m over hours 2 to 6 and at most +0.010 m over hours 2 and
3, and at most 20 % of station-realizations are worse by more than 0.02 m over hours 2 to 6; a cost otherwise, recorded
for the arming decision.

## 12. The held-out census read (2026-10-07)

Six producers (`runs/_rap_scl2o_{col,s2,t2}_{b1,xb}`, 48 stations, the twelve of section 11 held out; exe
`gnss_processor_scl2L.exe`), 72 consumer replays from the wire's first epoch (one checked byte-identical to the producer's
in-loop consumer). X2b minus B1 alone, pooled over 36 station-realizations: over hours 2 to 6 median -0.0009 m (station-
cluster bootstrap 90 %: -0.0063 to +0.0034), 0 worse by more than 0.02 m, 5 better; over hours 2 and 3 median +0.0021 m
(-0.0072 to +0.0087). Per realization (hours 2 to 6 / 2 and 3): col -0.0015 / -0.0003, s2 -0.0037 / -0.0027, t2 +0.0018 /
+0.0087. Section 11's reading: no external-consumer cost, every clause met. Outside the registered window, hour 1 costs a
pooled median +0.0117 m (15 of 36 worse by more than 0.02 m, 4 better; about 3 % of the hour-1 p50 of 0.34 m), earlier than
section 9's mechanism (01:01 to 02:31): X2b's earliest wire (the ignition clones and the hold rows of the first epochs).
The 36 samples share station effects across realizations (S43 -0.028 m on all three; S23 worse in hours 2 and 3 on all
three), and the census is on a 48-station network, weaker than the 60-station wire an arming would use.
