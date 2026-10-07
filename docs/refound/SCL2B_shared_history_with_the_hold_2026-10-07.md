# SCL2B: the shared history with card SMH's rows (X2b), graded, and X2 with X2b landed default off

**STATUS**

- **id:** SCL2B
- **topic:** network-vehicle
- **status:** open
- **summary:** 2026-10-07, the continuation of card SCL2 (whose file is near the 60 KB bar): amendment A1 of card SCL2 section 11 built and graded. X2b projects card SMH's hold rows onto X2's shared history (no new knob), so the hold's past clock receives the revision the history carries. The solve is byte-identical to X2 (only phase-bias records move), so the level gates read as X2's: de-commoned level 0.017 / 0.019 m in hour 6 against B1 alone's 0.042 / 0.045 (broadcast / COD), the statement consistent. On the consumer side A1 repairs what X2 broke: the hold's walk-share error on the COD bed 7.0 to 8.8 mm (X2 14.3 to 16.9, B1 11.0 to 11.8); the sim census +0.0034 m with 2 stations worse (X2 +0.0120 / 5; B1 +0.0049 / 2); the field census median +0.0030 m (B1 +0.0034), against B1 alone -0.0036 and 0 sites worse; WTZZ00DEU within the bar. Still failing: G1's ratio clause (the code's realization, card SCL2 section 12), G4's site count (4 against 3; B1 alone 5, the same sites), and SZZ000SIM in hours 2 and 3 (+0.028 / +0.040 m against B1 alone on beds sharing one realization). Cost 1.100 of B1 alone. X2 and X2b landed default off (section 5); nothing armed.
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
