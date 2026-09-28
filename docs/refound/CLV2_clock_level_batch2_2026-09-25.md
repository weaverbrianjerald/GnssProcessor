# CLV2: the clock level, return-plan batch 2 (the free column's own level, the consumer's response)

**STATUS**

- **id:** CLV2
- **topic:** network-vehicle
- **status:** open
- **summary:** BATCH 3 READ (section 5, 2026-09-28): the census loss is in the wire's PHASE rows. A hybrid wire with the batch-1 base's code rows only costs the census nothing (C1W sites -0.001 / +0.001 m); with its phase rows only it reproduces the loss (+0.418 / +0.151 m, WTZZ00DEU 0.977 m in hour 1). The hold transfers each declared level step to the phase bias, the estimator keeps only part of it in the clock (median kept fraction 0.54 at the arm, 0.89 at 2 h), and the phase row moves by the remainder, exactly (0.07 to 0.09 m rms, band-common). ARHT00ATA's divergence is a consumer defect (card PBF). Revised batch 3: a phase-exact re-base. Earlier summary: BUILT 2026-09-25, NOT ARMED (section 4): C0 passes (default off byte-identical, the consumer knob never reaches the producer); R-A2 moves the wire's C1W - C1C bias to the dual-tracking stations (effect 0.182 to 0.055 m, against CODE 0.155 to 0.110 m) but see-saws with the solve (0.058 m per epoch, 340 breaks) and costs the census C1W sites hour 1; the consumer response repairs the split emulation's hour 1 (0.516 to 0.222 m) and does nothing at the census; the candidate fails C3 (census +0.32 / +0.11 m against the no-CLV wire, all of it at the 10 C1W sites). The field's cost is the C1W users', not in the consumer's phase-blind direction and not in the ionosphere-free code; batch 3 proposed. Original summary: Build card, pre-registered 2026-09-25 at the author's direction to continue card
  CLV's return plan with batch 2 (card CLV section 8.6; the CLV file is near the size bar). Two
  builds, default OFF: R-A2, each free same-band code-bias column (GPS C1W, Galileo C1X) re-based
  from the same-band code difference at the stations that track both codes
  (`network.clock_level_free_cb_rebase`); and the consumer's response to a declared level step,
  a variance inflation along the stepped satellite's phase-blind ionosphere-ambiguity direction
  (`network.closed_loop.level_step_inflate`). R-C's declaration of the pivot-referenced common
  shift is sequenced again (section 2.3); R-B's datum pause is kept, its confirmation arm
  dropped.
- **source:** [CLV](CLV_clock_level_2026-09-22.md) sections 7 and 8 (the return plan and batch 1);
  [GAL](GAL_gps_galileo_dual_2026-09-22.md) (the adoption this card unblocks)
- **knobs / posture:** new `network.clock_level_free_cb_rebase` (bool, default false; inert
  unless `network.clock_level_carry`; fingerprinted `clv2:fcb`); new
  `network.closed_loop.level_step_inflate` (bool, default false) with
  `network.closed_loop.level_step_inflate_k` (default 1), derived into the consumer as
  `processing.ssr_level_step_inflate` and `processing.ssr_level_step_inflate_k`; no golden,
  ledger row or loader default changed
- **dates in the text:** 2026-09-25, 2026-09-28
- **hashes / run dirs:** code in the session worktree on main `335f640` beside card CLV's batch 1
  and card HLT (all uncommitted, default OFF); arms `runs/_rap_clv2_<bed>_<arm>` (2 h); census
  keys `c2<arm>` (`runs/_rap_msc_<site>_c2<arm>_d`); result: exe `gnss_processor_clv2.exe` sha256
  prefix `9d2d7cb13ea9f886`; section 5: exe `gnss_processor_clv3.exe` sha256 prefix
  `77643cd1db6677d9`, census keys `t3g0`, `t3b1`, `gtg0`, `gtb1`, `hbb`, `hbc`, `hbp`, hybrid
  wires `runs/_hyb_g0_b1{code,phase,both}`

## 1. What batch 1 left, and what was measured before this build

Card CLV section 8: the level re-base corrects a C1C user's ionosphere-free code against the CODE
products (0.29 to 0.19 m), leaves a C1W user at 0.36 to 0.39 m, and every variant costs a cold
consumer its first hour (the census C1W sites +0.33 to +0.58 m in hour 1).

**The free column's own error is in the stations' own data.** On the baseline wire
(`runs/_rap_hlt_f_g0`, identical to `_rap_clvrp1_f_g0`), 40 stations hold both a C1C and a C1W
code row of a GPS satellite in an epoch (39 965 cells in hour 1, 40 379 in hour 2). A two-way fit
of the post-fit residual difference r(C1W) - r(C1C) (a satellite effect and a station offset,
medians) gives per-satellite effects of 0.141 and 0.175 m rms in hours 1 and 2, correlated 0.68
and 0.74 with the wire's C1W - C1C bias error against CODE's OSB (0.135 and 0.155 m rms); removing
the effect would leave 0.113 and 0.123 m against CODE, the part that is CODE's own definition of
the C1W - C1C difference or noise (`scratchpad/ra2_premise.py`). The same batch-1 read on
`_rap_clvrp1_f_ppif` is 0.148 / 0.182 m, correlation 0.66 / 0.77. The difference is precise: its
robust scatter about the fit is 0.077 and 0.061 m per cell (0.04 to 0.06 m above 40 degrees,
0.13 to 0.15 m below 20), a fraction of a single code's, because both codes share the band's
multipath. In the solve the second code channel's rows carry a weight of 0.01 (scale 10), so the
column is set mostly by the ionosphere-free code of the stations that read band 1 through C1W,
which also see the frozen clock level.

**A consequence for the grading.** A C1W user's code error against CODE is its C1C error plus
a1 = 2.546 times the C1W - C1C bias error; with the bias matched to the stations the latter is
still 0.12 m against CODE, 0.30 m in the user's ionosphere-free code. That reference cannot grade
R-A2; the census C1W sites and the stations' own same-band residuals can.

## 2. The design

**2.1 R-A2 (`network.clock_level_free_cb_rebase`).** After each kept epoch's level re-base: at
every station, the post-fit residual of each code row of a satellite at any positive weight;
where the station holds the satellite's free same-band signal (a code on the band of the realized
pin pair's first signal that is not in the pair) and the pair's first signal, the cell
v = r(free) - r(first). Per (constellation, free signal), the batch-1 two-way fit
(`clockLevelTwoWay`: a satellite effect, one offset per station, the satellite effects summing to
zero, the robust screen, at least 3 cells) with weight 1/0.08^2 per cell; per (satellite, signal)
a filter of the level's form (information decayed with `network.clock_level_horizon_s`, capped at
1/0.02^2, restarted at a 4-sigma break); the step x = gain times the effect, de-meaned over the
family (information-weighted: the family's common level is the time-linked sum-to-zero row's
datum). The step moves the column through the carry references batch 1's follow uses (the carried
value, every coupling term's s_prev, the time-linked datum reference, the B record's lvl=
declaration). The difference is free of the clock, so the level shift does not enter it; while a
column's filter is armed the column no longer follows the level, because the follow share and the
own step would count the level's absorption twice. On the sim, where the free column only
absorbed the level, the two are the same move: the absorption reads as the same-band effect
-d/a1, and the column's net step equals the follow's. A failed level fit no longer ends the
epoch when R-A2 is armed. Galileo's two channel families are disjoint (card GAL), so Galileo C1X
has no cells and keeps the follow.

**2.2 The consumer's response (`network.closed_loop.level_step_inflate`).** An undifferenced
consumer absorbs a satellite's code-level error along the direction v of that satellite's slant
ionosphere and float ambiguities that moves its code rows and no phase row: ionosphere +1 and
each phase ambiguity (metres) +alpha_f, alpha_f = (f_L1 / f)^2 (the model's ionosphere partial is
+alpha on code, -alpha on phase, the ambiguity's +1). The least-squares share of a common code
error on (P1, P2) along v is (1 + gamma) / (1 + gamma^2) = 0.71 for L1/L2, card CLV section 1's
71 percent. The float ambiguities hold that share after the error is corrected, which is why the
consumer re-converges slowly. The emitter accumulates each satellite's declared steps (the C
record's lvl= and the B records' lvl=) in the SSR store; each epoch the consumer differences the
totals against its last reading (the first reading only records), takes sigma as the largest
change of any of the satellite's code-signal predictions, -(clock step) + (that signal's code-bias
step), and adds (k sigma)^2 v v^T to its covariance. The phase rows are unchanged along v, so the
next epochs' code rows re-determine it and nothing else loosens. A satellite without an
ionosphere state, with a frozen row or without an ambiguity is refused and counted. Run-end line
on every consumer path (live, replay, closed loop).

**2.3 R-C sequenced.** The pivot-referenced shifts move every other satellite of a constellation
by a common amount, which a multi-constellation consumer's inter-system bias must follow (batch 1:
+0.49 m on the Galileo sim). The only per-epoch declaration a consumer answers today, the E line's
datum flag, bumps every clock, code and phase counter of the constellation (the ambiguities are
reset), and the inter-system bias response it triggers defaults to 0
(`clockDiscIsbReinflateStd`). A dedicated per-constellation common-shift token and a consumer
carry of it are owed, built only if R-A2 and the response leave a pivot error on the field.

## 3. The gates (rapid mode: 2 h arms on card CLV's beds)

Beds: GPS field `f` (base `runs/_rf0hv10_field6`), GPS sim `s` (`runs/_rf0hv10_sim6`), the split
emulation `x` (`runs/_rap_gal_g3x2_sim6`), GPS+Galileo field `fg` (`runs/_rap_gal_g5s_field6`) and
sim `sg` (`runs/_rap_gal_g2b_sim6`). Arms per bed: `b1` = `network.clock_level_carry`,
`clock_level_pin_pair_read`, `clock_level_free_cb_follow: if`, `clock_level_datum_pause` (batch
1's surviving parts); `a2` = `b1` + R-A2; `cr` = `b1` + the consumer response; `a2cr` = both. Plus
`g0` on `f` and `s`. The census replays the `f` wires at the 16 sites for 2 h: keys `c2b1`,
`c2a2` (plain consumer), `c2cr` (the `b1` wire with the response), `c2a2cr`. Every comparison
among the arms of a bed shares the ignition (the re-base acts after the first solve), so card
HLT's level re-draw does not enter it. The bars were written before any batch-2 arm ran.

| gate | arms | bar |
| --- | --- | --- |
| C0 | `f_g0`, `s_g0` against `_rap_hlt_{f,s}_g0`; each bed's `cr` against its `b1` | the g0 records, epoch status and closed loop identical; `cr`'s product records identical to `b1`'s on every bed (the consumer knob never reaches the producer); the new unit tests and the full Release suite pass |
| C1 (R-A2) | `f_a2` against `f_b1` | the stations' same-band effect rms in hour 2 at most 0.05 m (`ra2_premise.py`); the wire's C1W - C1C bias against CODE not larger in hour 2; the census C1W sites' hour-2 paired change (`c2a2` against `c2b1`) at most 0, with more sites better than worse; the run-end R-A2 line with updates on GPS and none on Galileo |
| C2 (response) | `cr` against `b1` | `x` consumer hour 1 at most 0.30 m (batch 1 `x_ppif` 0.516); `s` consumer hour 1 not larger; the census pooled hour-1 change (`c2cr` against `c2b1`) at most -0.10 m and hour 2 at most +0.005 m; the run-end response line with answered steps and its refusals |
| C3 (candidate) | `a2cr` against the no-CLV baselines | the census against `rp1g0` (the baseline wire): pooled hour-1 change at most +0.02 m and hour-2 at most -0.005 m; WTZZ00DEU at most 0.570 / 0.069 m (card CLV's B1 bars); `s` datum-free level at epoch 240 at most 0.15 m; `x` C1X - C1C recovery in hour 2 at most 0.10 m; `fg` and `sg` consumer hour 2 at most 0.005 m above `_rap_hlt_{fg,sg}_g0` |

If C3 passes, the 6 h pair and card GAL's adoption census are re-run on the candidate for the
author's adoption decision; a failed gate is sequenced with the reading that failed it.

## 4. Result (2026-09-25, 2 h arms)

Exe `gnss_processor_clv2.exe` (sha256
`9d2d7cb13ea9f8860797f37cc81a93b222821dfdb7157585d8458259fbd67360`, the worktree build with card
HLT's code). Every number is read from the run directory named (`runs/_rap_clv2_<bed>_<arm>`).
Graders (read-only, session scratchpad): `clvrp_grade.py` (the consumer by hour, the anchor
decouples, the run-end lines and per bed card CLV's `clklevel.py --datumfree`, `g3_recovery.py`,
`clv_df_sys.py` and `gal_grade.py --sections isbtruth`), `ident2h.py`, `ra2_premise.py`,
`census2h.py` with `census_pooled.py`, and `userview.py` (a user's corrections on one wire minus
another, signal by signal).

**4.1 C0: PASS.** `f_g0` and `s_g0` are identical to `_rap_hlt_{f,s}_g0` (product records,
closed loop, epoch status). On all five beds `cr`'s product records and epoch status are identical
to `b1`'s (and `f_a2cr`'s to `f_a2`'s); only the closed loop differs, first at the arming epoch
(TOW 259350) on `f`. R-A2 is inert on the three sim beds: no sim station holds both GPS codes, the
run-end line counts 0 cells and warns, and `s_a2`, `x_a2`, `sg_a2` equal `b1`. The full Release
suite runs 2 659 tests from 336 suites: 2 642 pass and 17 skip (the fixture-gated ones; `data/`
is empty in the worktree); the 4 new tests are in `tests/test_clock_level_batch2.cpp` (suites
`ClockLevelFreeColumn`, `DeclaredLevelSteps`).

**4.2 C1 (R-A2): FAIL.** The mechanism works on average: the stations' same-band effect falls
from 0.148 / 0.182 to 0.021 / 0.055 m rms in hours 1 / 2 (bar 0.05 m in hour 2, missed by 0.005),
and the wire's C1W - C1C bias against CODE from 0.123 / 0.155 to 0.105 / 0.110 m, at the 0.118 m
floor section 1 set. But the solve pulls each column back every epoch toward the value the
C1W-reading stations' ionosphere-free code demands at full weight (the dual stations' rows weigh
0.01 there): in hour 2 each column steps 0.058 m rms per epoch about a mean of 0.001 m, with 340
filter breaks in 2 h and an accumulated step of 0.675 m rms per column (G03 C1W: pushes of +2.83
and -1.04 m, net +1.79). A C1W user sees that see-saw at a1 = 2.55 times in its
ionosphere-free code. Census, `c2a2` against `c2b1`: pooled hour 1 +0.130 m (2 sites better, 10
worse), hour 2 +0.001 m; the C1W sites +0.011 m in hour 2 (3 better, 5 worse). WTZZ00DEU 0.966 /
0.056 to 1.014 / 0.040 m.

**4.3 C2 (the response): FAIL, passing on the split emulation.** `x`: 0.516 / 0.071 to 0.222 /
0.048 m (bar 0.30 m in hour 1; 3 179 steps answered, the largest sigma 2.13 m). `s`: 0.358 /
0.088 to 0.514 / 0.067 m, hour 1 worse. `sg`: 0.652 / 0.056 to 0.559 / 0.052 m. `fg`: 0.386 / 0.054
to 0.380 / 0.053 m. WTZZ00DEU 0.966 / 0.056 to 0.954 / 0.054 m. Census, `c2cr` against `c2b1`: hour 1
-0.0006 m, hour 2 +0.0001 m (bar -0.10 m). The response was armed in every replay: 1 650 to 1 940
steps answered per site, none refused, the largest sigma 0.675 m (QUIN00USA).

**4.4 C3 (the candidate `a2cr`): FAIL.** Against the no-CLV wire (`rp1g0`): census pooled hour 1
+0.323 m (3 better, 11 worse), hour 2 +0.110 m (2 better, 12 worse); the 10 C1W sites +0.593 /
+0.185 m, the 6 C1C sites -0.008 / +0.018 m. WTZZ00DEU 1.004 / 0.037 m (bars 0.570 / 0.069 m: hour
1 fails). Passing: `s` datum-free level 0.07 m at epoch 240, `x` C1X - C1C recovery 0.080 m in hour
2, `fg` and `sg` hour 2 at 0.050 and 0.052 m against `g0`'s 0.063 and 0.060 m. The batch-1 base alone
(`c2b1` against `rp1g0`) is +0.279 / +0.048 m, the C1W sites again carrying it.

| bed, consumer p50 by hour (m), epochs below 0.1 m | `b1` | `a2` | `cr` | `a2cr` |
| --- | --- | --- | --- | --- |
| `f` (WTZZ00DEU; `g0` 0.560 0.059, 131) | 0.966 0.056, 102 | 1.014 0.040, 106 | 0.954 0.054, 107 | 1.004 0.037, 112 |
| `s` (`g0` 0.598 0.103, 53) | 0.358 0.088, 93 | = `b1` | 0.514 0.067, 101 | = `cr` |
| `x` | 0.516 0.071, 121 | = `b1` | 0.222 0.048, 124 | = `cr` |
| `fg` (`g0` 0.197 0.063, 143) | 0.386 0.054, 156 | 0.420 0.049, 147 | 0.380 0.053, 157 | 0.413 0.050, 153 |
| `sg` (`g0` 0.514 0.060, 115) | 0.652 0.056, 122 | = `b1` | 0.559 0.052, 125 | = `cr` |

**4.5 Reading: the field's cost is the C1W users', and it is not where the response acts.** The
response releases exactly the direction a declared level step moves for a C1W user under the
follow share, and the census does not move. A C1W user's view of the `b1` wire against `g0`
(`userview.py`, 20-minute bins, the per-epoch median over satellites removed): the
ionosphere-free code 0.012 to 0.030 m rms, the geometry-free code 0.05 to 0.10 m, the phase side
0.08 to 0.11 m per satellite, nearly constant after the first 20 minutes (0.006 to 0.010 m about
each satellite's mean) and stepping inside them (0.047 m). A C1C user instead gets the
ionosphere-free code correction (0.13 to 0.26 m rms, the correction CODE confirms, 0.29 to 0.19
m) with a similar phase side. None of these explains a C1W site's hour-1 loss of 0.4 to 0.6 m by
its size; that loss is unread.

**4.6 Verdict: NOT ARMED; batch 3 proposed (one change each, 2 h).**

1. Read the census C1W sites' consumer residuals and states around the arming epochs (TOW
   259350 to 259500) on `b1` against `g0`: the loss is there and is neither in the phase-blind
   direction nor in the ionosphere-free code.
2. R-A2 inside the solve rather than beside it: the dual stations' same-band rows at full weight
   (card SCC's scale 1, whose own trade that card measured) or a soft pseudo-observation on the
   free column, so the solve and the re-base stop pulling against each other.
3. The response stays a built option: it repairs hour 1 where the consumer absorbed a large level
   (`x` 0.516 to 0.222 m, `sg` 0.652 to 0.559 m) and costs the GPS sim's (0.358 to 0.514 m); its
   scale and dead band are untuned.
4. R-C's declaration stays sequenced. Galileo adoption stays behind this card, as the author
   decided.

## 5. Batch 3, item 1: the read of the C1W sites' loss (2026-09-28)

Section 4.6 item 1, done on 2 h census replays with card CST's per-satellite trace (the replay
driver now writes it; the 34 traced replays of the baseline wire `runs/_rap_clv2_f_g0` and of
`runs/_rap_clv2_f_b1` reproduce the census `closed_loop_trace.csv` of keys `rp1g0` and `c2b1`
byte for byte). Exe `gnss_processor_clv3.exe` (main `3ca0626` plus the replay trace), census keys
`t3g0`, `t3b1`. Graders in the session scratchpad: `census2h.py`, `cstdiff.py`, `armsteps.py`,
`pbflips.py`, `hybrid_wire.py`, `wirediff.py`.

**5.1 The loss is in the wire's phase rows, not its code rows.** Hybrid wires: the baseline
wire's records with the batch-1 base's row values substituted (a code row is -clock + code bias,
a phase row is -clock + phase bias; the substitution keeps the baseline wire's clock and
rewrites the bias so the row equals the donor's). `hbc` takes the code rows only, `hbp` the phase
rows only, `hbb` both (`runs/_hyb_g0_b1{code,phase,both}`). Median change of the hour-1 / hour-2
p50 against the baseline wire:

| wire | C1W sites (9, ARHT00ATA out, section 5.3) | C1C sites (6) | WTZZ00DEU |
| --- | --- | --- | --- |
| `hbb` (both) | +0.406 / +0.153 (7 worse, 2 better in hour 1) | +0.059 / -0.011 | 0.966 / 0.056 |
| `hbc` (code only) | -0.001 / +0.001 (0 worse, 1 better) | -0.030 / -0.003 | 0.547 / 0.064 |
| `hbp` (phase only) | +0.418 / +0.151 (7 worse, 2 better) | +0.096 / -0.005 | 0.977 / 0.056 |

(the baseline wire: WTZZ00DEU 0.560 / 0.059). The code side of the level re-base, including the
C1W follow share, costs the census nothing; the phase side carries the whole loss. `hbb` equals
the batch-1 base at 13 of 17 sites to the millimetre; it differs at ANMG00MYS, QUIN00USA and
ARHT00ATA, where the baseline wire's counters and tokens replace the donor's.

**5.2 Why the phase rows move.** On the batch-1 base the wire's phase bias of each satellite
differs from the baseline wire by exactly its accumulated declared steps (the C records' `lvl=`
sum): the hold (card PBH) transfers each declared step to the held value. The wire's clock
differs by the declared steps plus an undeclared remainder, the part of each step the
estimator's next solves do not keep. The wire's phase-row change is therefore minus that
remainder, exactly (to 1e-14 m while each hold generation lasts), and it is band-common (L1C
and L2W move by the same amount per satellite). The kept fraction differs by satellite; its
median over the satellites with an accumulated step above 0.05 m is 0.54 at TOW 259530
(interquartile 0.38 to 0.71), 0.67 at 260400, 0.82 at 262800 and 0.89 at 266370, with G12 near
0 (every step given back) and G08 and G23 above 1. The remainder is 0.068 to 0.089 m rms across
satellites from TOW 259530 on, grows over the first 10 to 20 epochs after the arm (TOW 259350)
and is then nearly constant; a float consumer converging in that window absorbs it into its
position. The station ambiguities of the carry move between the two wires by a band-common
0.05 to 0.35 m per satellite (median over stations), so the estimator re-partitions along the
phase-datum direction as well as the level direction; which carried state pulls the clock back
is not established here.

**5.3 ARHT00ATA is a consumer defect, not the level.** Its divergence on this wire (478.953 /
803.907 m) is an unanswered presence edge of a held phase bias that the Kalman peel misattributes;
card [PBF](PBF_pb_hold_presence_edges_2026-09-28.md) reads it and builds the response. With that
response the site reaches 0.762 m in hour 2 on this wire (0.066 m on the baseline wire): the
same phase-side cost as the other C1W sites, larger.

**5.4 Why the C1W and C1C sites differ.** Not established. A C1W site's consumer here reads
L1C phase with C1W code (HARB00ZAF), so both groups read the same phase family; the split may
follow the satellites in view during the arming window rather than the code family.

**5.5 Batch 3, revised (one change each, 2 h).**

1. A phase-exact re-base: the wire's phase rows unchanged across a re-base. Two places to act,
   to be read before either is built: (a) the solve side, the carried state that pulls the clock
   back after a step (the kept fraction per satellite is the instrument; the station
   ambiguities' band-common move is the first suspect), so that the estimator keeps each
   declared step; (b) the emit side, where the hold's transfer would have to follow the step the
   estimator kept rather than the declared one, which needs a measurement of the remainder that
   the real clock walk does not contaminate. Bar for either: the wire's phase-row change against
   the baseline wire below 0.01 m rms across satellites at every epoch, and the census C1W sites'
   hour-1 change at most +0.05 m.
2. R-A2 inside the solve (section 4.6 item 2) waits for item 1: its column see-saw is a code-side
   effect, and the code side is not where the census loss is.
3. The consumer response stays a built option; R-C stays sequenced; Galileo adoption stays
   behind this card.
