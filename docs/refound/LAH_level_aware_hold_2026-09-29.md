# LAH: the level-aware phase-bias hold (hold the phase-visible clock)

**STATUS**

- **id:** LAH
- **topic:** network-vehicle
- **status:** open
- **summary:** 2026-09-29, pre-registered, built and gated the same day, default off, not armed. Card CTU2 section 9 item 1: the phase-bias hold freezes the phase bias while the exact coupling time update moves the satellite's level (clock and every phase bias together), so the level reaches the wire's phase side (cold consumer 14 m in field hour 1). Every hold that keeps the phase-visible clock c - p at its generation-start value plus a motion term is level-blind on the phase side; the motion term is the only choice. The smoothed motion (the estimator's own model) was derived twice and rejected: the estimator states the ionosphere-free phase-visible clock to a few millimetres while it wanders centimetres, so the rule passes the ionosphere-free phase-bias wander (census 1.76 / 0.75 m). Built: the zero-motion member, `network.emit_pb_hold_phase_clock` (S line `pbhold=2`, M per constellation). Gates: identity off, wire equal to two offline constructions to 1.4e-14 m, suite 2 703 / 2 704 plus 1 skip. 6 h census: exact update 2.27 / 0.75 / 0.57 / 0.38 / 0.26 / 0.20 m with the plain hold, 0.59 / 0.17 / 0.12 / 0.08 / 0.10 / 0.08 with this one, control 0.67 / 0.17 / 0.10 / 0.08 / 0.09 / 0.07; named C1W regressions (KOKB, QUIN, UTQI, ARHT). Posture limit: within a generation the phase-visible clock motion is the a priori product's (valid with COD finals, not with brdc_rts).
- **source:** [CTU2](CTU2_level_recursion_followup_2026-09-29.md) section 9; [CTU](CTU_coupling_time_update_2026-09-28.md) sections 9.4 and 13.4 (h); [PBH](PBH_phase_bias_hold_2026-09-19.md); [PBF](PBF_pb_hold_presence_edges_2026-09-28.md)
- **knobs / posture:** `network.emit_pb_hold_phase_clock` (default false; requires `network.emit_pb_hold`, refuses `network.clock_level_carry`); emit-side only, the solve and the C and B records unchanged; no golden, ledger or posture change
- **dates in the text:** 2026-09-29
- **hashes / run dirs:** exe `gnss_processor_lah2.exe` sha256 prefix `eb8eb29edbb08458` (`_lah1` `2f30a5fb38cfdbd8`); arms `runs/_rap_s8_*`, `runs/_rap_s9_*`; offline wires `runs/_rpl_*`, `runs/_lah_*`; census keys `lahfrzP`, `lahcfrzP`, `msc6_*_{ctl,frz,cfrz,ex}`; section 6

## 1. The question

Card CTU2 section 9 item 1. The hold (card PBH) writes each (satellite, signal) phase bias at its
generation-start value while the clock is emitted as estimated, so the wire's phase-visible clock
c - p moves with every clock move. Under the control (the card 1d time update) the level is frozen
and the clock moves little within an arc, so this was cheap. Under the exact update the level
converges: the estimator moves the satellite's clock and every phase bias together along u_s
(invisible to phase), and the held phase bias turns that code-only correction into a phase-side
drift. Cold consumer p50 by hour at 6 h (card CTU2 section 10): sim 1.49 / 0.78 / 0.41 m with the
hold against 0.97 / 0.16 / 0.06 for the control; field 14.0 / 3.45 / 0.90 m against 0.56 / 0.06 /
0.03.

## 2. The family of level-blind holds

The consumer's rows carry the clock C and the phase bias P as -C on code and -C + P on phase, with
a receiver clock common to code and phase and float ambiguities constant within an arc. Two wire
errors leave its position unchanged: a common shift of every satellite clock (the receiver clock
takes it on code and phase together), and a constant per (satellite, signal) phase-side offset (the
float ambiguity takes it). Any hold of the form

    C(t) = c(t|t),   (C - P_j)(t) = (c - p_j)(t0|t0) + Phi(t)

keeps the absorbed offset of generation start and puts every clock move on the code side only, the
level included, whatever the motion term Phi. Three members:

| member | Phi(t) | P_j(t) |
| --- | --- | --- |
| hold off | the estimator's phase-visible change, wander included | p_j(t|t) |
| smoothed hold | c(t|t) - c(t0|t), the fixed-point smoothed clock motion since t0 | p_j(t0|t0) + [c(t0|t) - c(t0|t0)] |
| phase-clock hold (built) | 0, up to the common mode | p_j(t0|t0) + [c~(t) - c~(t0)] |

The current hold is none of these: its phase side carries Phi plus the clock's revision of c(t0),
and under the exact update that revision is the level.

The smoothed hold is the member the estimator's own model prescribes (two independent derivations
agree, section 7): the clock's retroactive revision of c(t0) moves to the phase bias, so the level
passes, phase-bias-only revisions are held and true clock motion reaches the phase side. It is
computable exactly as a coupling row with no observation rows, carried by the exact time update of
card CTU. In the coordinates (K, J, D) of one satellite (K = sum_j w_j p_j the level, J the
phase-bias ionosphere direction, D = c - K the ionosphere-free phase-visible clock, the only
direction the clock walk moves), the per-epoch revision of c(t0) is rho dc + (1 - rho) sum_j w_j
dp_j, with rho the model's regression of D(t0) on D(t). Under the estimator's covariance D is known
to a few millimetres while the walk adds 0.93 mm per epoch, so rho decays within minutes (about
1 / (1 + n / 15) on a numerical toy of the posture), and the smoothed hold becomes the
ionosphere-free phase-bias transfer that card CTU2 section 9 already rejected: it passes the
ionosphere-free phase-bias wander (2 to 5 cm within an arc on the sim, against a model statement of
a few millimetres). The smoothed hold is exact for the level and fails on the estimator's
phase-visible inconsistency, which is what the hold exists to hide. The phase-clock hold is its
q -> 0 limit: no clock motion is passed within a generation.

## 3. Offline evaluation

The hold is emit-only, so the hold-on and hold-off wires of one arm differ in the P values alone
(the C, B, R, U and H records are identical, checked on `_rap_s7_ex_{s,f}` against
`_rap_s7_exh0_{s,f}`). Any rule that uses emitted quantities is therefore written offline from the
hold-on wire's generation structure and the hold-off wire's estimates, and replayed through the
consumer-only driver. The replay reproduces the live consumer: `_rpl_hold_s` (the unmodified wire)
writes the same `closed_loop_trace.csv` as `_rap_s7_ex_s`, 10 to 17 s per 6 h replay.

Cold consumer 3D p50 by hour (m), exe `gnss_processor_ctu15.exe`:

| wire | sim h1 to h6 | field h1 to h6 |
| --- | --- | --- |
| golden control, hold (`_rap_s6_ctl`) | 0.968 0.161 0.056 0.067 0.057 0.068 | 0.560 0.059 0.028 0.030 0.039 0.048 |
| exact, hold (`_rap_s7_ex`) | 1.491 0.780 0.410 0.122 0.053 0.036 | 14.00 3.448 0.897 0.345 0.272 0.132 |
| exact, hold off (`_rap_s7_exh0`, live) | 0.865 0.379 0.298 0.306 0.228 0.202 | 6.369 1.311 0.515 0.337 0.356 0.376 |
| exact, ionosphere-free phase-bias transfer (`_rpl_if`) | 2.240 0.225 0.082 0.054 0.067 0.115 | 2.803 2.234 0.304 0.241 0.176 0.389 |
| exact, smoothed proxy rho = 1 / (1 + n / 60) | 0.153 0.079 0.053 0.047 0.056 0.095 | 1.863 0.974 0.262 0.207 0.183 0.270 |
| exact, phase-clock hold (`_rpl_dc`) | 0.340 0.160 0.057 0.039 0.038 0.060 | 0.116 0.025 0.016 0.020 0.030 0.026 |
| control, phase-clock hold (`_rpl_ctldc`) | 0.346 0.159 0.056 0.039 0.038 0.060 | |

The phase-clock hold gives the same sim consumer on the control and on the exact wire: its hour-1
gain is not a level effect but the frozen per-satellite phase-visible clock correction, which on
the sim is the truth (the a priori clock is the simulator's) and on the field is the COD final
clock. The field-bed read is the 16-site census (`census2h.py`, hours 1 / 2, median of the site
p50 against the reference key `rf3gl2nb`): control 0.666 / 0.169, exact with the hold 2.266 /
0.752, ionosphere-free transfer 1.809 / 0.842, smoothed proxy 1.761 / 0.753, exact with the
phase-clock hold 0.590 / 0.166 (hour 2: 10 sites better, 5 worse, the C1C sites 6 of 6 better),
control with the phase-clock hold 0.514 / 0.167. The phase-clock hold's weak spot is hour 1 at
three sites, QUIN 0.22 to 1.74, POHN 0.65 to 3.62 and KOKB 0.61 to 5.72 m, mostly vertical; it
appears on the control producer too and disappears for consumers that start at 00:20 (census from
00:20: control 0.305 / 0.120, control with the hold 0.300 / 0.085, exact with the hold 0.312 /
0.098, exact with the plain hold 1.005 / 0.501), so it belongs to the producer's first-epoch
settling, not to the hold. Named in advance for the gates below.

## 4. The build

`network.emit_pb_hold_phase_clock` (default false), a mode of `network.emit_pb_hold`
(`src/core/filter/NetworkPbHold.cpp`). Per emitted epoch, before the hold's sweep:
M += the median, over the satellites with an emitted clock at this and the previous emission, of
the emitted clock step (`pbHoldCommonClockStep`), and c~_s = c_s - M. A generation start records
c~_s(t0); a held emission writes p(t0) + [c~_s(t) - c~_s(t0)]. The emitted clock is the frame-held
value in `sol` (card CLK-R), so a common move (a pivot handover, the frame hold's re-anchor) stays
on the clock through M, and a satellite back inside `emit_pb_hold_max_gap_s` picks up its whole
move since t0. An epoch with the phase bias but no clock reuses the last transfer; a generation
started without a clock is held plainly (both counted). The generation rules, the `gen=`
declaration and the consumer are unchanged: the consumer applies whatever `bias_m` the store holds
and re-floats on `gen=` only (card PBH section 6.6), and an emitter value-jump flag on a held key is
consumer-inert. S line: `pbhold=2` in place of `pbhold=1` (a reader testing `pbhold != 0` still
reads a hold; `SbasisContract::emit_pb_hold_phase_clock`, compared by `checkContract`). The
product-file legend line is unchanged, so an off run writes the same file.

Invariant 7: the decoupling edge's cost is still the |estimate - held| distribution, which under the
mode measures the phase-visible wander the hold hides with the level transferred; the mode adds
the |transfer step| and |transfer| distributions and the counters `n_pbh_frz_moved`,
`n_pbh_frz_noclock`, `n_pbh_frz_noref`, `n_pbh_frz_epochs`, `n_pbh_frz_coupled` (a coupled
clock and phase-bias mark is transferred, not lost). Run-end line `pb hold, phase clock (LAH)`.
The re-coupling edge is the generation start, unchanged. The hold state is not snapshotted
(as for card PBH): a warm resume starts every key as a first sighting.

Posture limit: within a generation the consumer's phase-visible clock motion is the a priori
product's. With the COD finals (the field) and the simulator's own clock (the sim) that is the most
accurate motion available; with `apriori_source brdc_rts` it would drop decimetres of real
broadcast-clock motion, and the smoothed member (or a motion term with an honest phase-visible
covariance behind it) would be owed. The directive to retire the a priori clock product makes this
limit a real constraint on arming, stated here rather than discovered later.

## 5. Gates

Arms of 6 h: the exact posture of card CTU2 section 7 (`_rap_s7_ex_{s,f}`) and the golden control
(`_rap_s6_ctl_{s,f}`), each with only `network.emit_pb_hold_phase_clock: true` added
(`_rap_s8_frz_{s,f}`, `_rap_s8_cfrz_{s,f}`), plus the unchanged exact configuration on the new exe
(`_rap_s8_g0_{s,f}`). The sim starts at 00:30:15 (card CTU2 section 1).

**U0.** `tests/test_pb_hold_phase_clock.cpp` (suite `PbHoldPhaseClockTest`, 14 cases: the loader
requirements and refusals, a level move held on the phase side where the plain hold moves it, a
common move and a single constellation's common move left on the clock, a satellite back inside
max_gap, an epoch without the clock, a generation without a reference, a new generation's rebase
and its reset of the last transfer, an emitted epoch without satellites, the coupled-mark census,
the per-constellation median, the `pbhold=2` contract round trip, the FC-1 refusal). The full
Debug suite on exe `_lah2`: 2 704 tests in 343 suites, 2 703 passed, 1 fixture-gated skip
(`FixtureSmokeTest.Sp3HeaderTimeConversionMatches`); the hold suites `PbEmitHoldTest`,
`PbHoldPresenceTest` and `PbHoldPhaseClockTest` 27 of 27.

**G0, identity.** With the knob off the new exe writes `_rap_s8_g0_{s,f}` byte-identical to
`_rap_s7_ex_{s,f}` (exe `_ctu15`): `products.txt`, `closed_loop_trace.csv` and
`network_epoch_status.csv`, both beds.

**The wire equals the offline construction.** Every P record of the live wires equals the offline
wires of section 3, and every other line is identical: `_rap_s8_frz_s` max |difference| 3.6e-15 m
over 45 060 records, `_rap_s8_frz_f` 1.4e-14 m over 62 322, `_rap_s8_cfrz_s` 3.6e-15 m against
`_rpl_ctldc_s`, `_rap_s8_cfrz_f` 1.4e-14 m over the 20 961 records of the 2 h offline wire. Two
independently written offline constructions (this card's `hold_rewrite.py` and the adversarial
review's `wires4.py`) agree to 1.4e-14 m.

**Consumer, 6 h.** Cold consumer 3D p50 by hour (m):

| arm | sim h1 to h6 | field h1 to h6 |
| --- | --- | --- |
| control, plain hold (`_rap_s6_ctl`) | 0.968 0.161 0.056 0.067 0.057 0.068 | 0.560 0.059 0.028 0.030 0.039 0.048 |
| control, phase-clock hold (`_rap_s8_cfrz`) | 0.346 0.159 0.056 0.039 0.038 0.060 | 0.116 0.023 0.017 0.019 0.029 0.025 |
| exact, plain hold (`_rap_s7_ex`) | 1.491 0.780 0.410 0.122 0.053 0.036 | 14.00 3.448 0.897 0.345 0.272 0.132 |
| exact, phase-clock hold (`_rap_s8_frz`) | 0.340 0.160 0.057 0.039 0.038 0.060 | 0.116 0.025 0.016 0.020 0.030 0.026 |

**Census, 2 h.** The 16-site census on the live field wires reproduces the offline census site for
site (keys `lahfrzP`, `lahcfrzP` against `lahfrz`, `lahctlfrz`): median of the site p50, hours 1 /
2, reference `rf3gl2nb` 0.670 / 0.169, exact with the phase-clock hold 0.590 / 0.166, control with
it 0.514 / 0.167, exact with the plain hold 2.266 / 0.752. The census replays must arm
`network.closed_loop.pb_hold_presence_flip` (card PBF, on both goldens): the census tool's base arm
(`_rf0hv10_field6`) predates it, and without it ARHT reads 18.85 m in hour 2 on the control with
the phase-clock hold (0.247 m with it), deterministic on rerun.

**Census, 6 h** (`census6h.py`, PBF on, median of the site p50 by hour):

| wire | h1 | h2 | h3 | h4 | h5 | h6 |
| --- | --- | --- | --- | --- | --- | --- |
| control, plain hold | 0.666 | 0.169 | 0.104 | 0.077 | 0.092 | 0.070 |
| exact, plain hold | 2.266 | 0.752 | 0.568 | 0.380 | 0.256 | 0.203 |
| exact, phase-clock hold | 0.590 | 0.166 | 0.120 | 0.081 | 0.101 | 0.077 |
| control, phase-clock hold | 0.514 | 0.167 | 0.119 | 0.079 | 0.073 | 0.053 |

By family, exact with the phase-clock hold against the control (median change, better / worse at
1 cm): the ten C1W sites +0.147 (4/6), +0.048 (4/5), -0.024 (5/4), +0.007 (3/5), +0.001 (4/5),
-0.004 (5/4); the six C1C sites -0.310 (4/2), -0.036 (6/0), -0.009 (3/1), -0.007 (1/1), +0.029
(0/4), +0.012 (2/3). The largest regressions, hours 2 to 6 (control > phase-clock hold, m): KOKB
0.22 > 1.50, 0.22 > 0.13, 0.15 > 0.06, 0.13 > 0.07, 0.04 > 0.03; ARHT 0.07 > 0.55, 0.03 > 0.69,
0.02 > 0.21, 0.03 > 0.24, 0.03 > 0.18; QUIN 0.08 > 0.38, 0.02 > 0.06, 0.02 > 0.07, 0.02 > 0.03,
0.02 > 0.04; UTQI and ANMG smaller. On the control producer the same sites regress except ARHT,
which stays near the control after hour 2 (0.25, 0.08, 0.08, 0.04, 0.05).

**Telemetry.** `_rap_s8_frz_s`: 44 986 held emissions, every one moved, 0 without a clock, 0
without a reference; |transfer step| p50 1.2 mm, p90 5.0 mm, max 0.69 m; |transfer| p50 0.29 m,
p90 0.73 m, max 1.01 m; 37 coupled marks transferred. `_rap_s8_frz_f`: 62 211 held emissions;
step p50 0.7 mm, p90 3.3 mm, max 6.87 m; transfer p50 0.28 m, p90 0.53 m, max 12.9 m; 100 coupled
marks transferred. The |estimate - held| distribution (what the hold hides) is 1.75 m p50 on the
sim and 1.30 m on the field, against 1.71 m and 1.46 m under the plain hold on the same producer:
it is dominated by phase-bias frame re-realizations (phase-anchor re-pins and handovers under the
same key), which both holds hide by design.

## 6. Hashes and run dirs

Exe `gnss_processor_lah1.exe` sha256 prefix `2f30a5fb38cfdbd8` (the arms `_rap_s8_*`, the census
keys `lahfrzL`, `lahcfrzL`, `lahfrzP`, `lahcfrzP`, `msc6_*_{ctl,frz,cfrz,ex}`); after the review's
fixes (section 7) `gnss_processor_lah2.exe` sha256 prefix `eb8eb29edbb08458`, gated on
`_rap_s9_frz_{s,f}` (products and consumer trace byte-identical to `_rap_s8_frz_{s,f}`: the fixes
change no GPS-only output) and `_rap_s9_g0_{s,f}` (knob off, byte-identical to `_rap_s7_ex_{s,f}`
on `_ctu15`: products, consumer trace, epoch status). Offline wires
`runs/_rpl_*` (this card) and `runs/_lah_*` (the adversarial review). Session scratchpad tools:
`hold_rewrite.py` (offline hold rules + replay arm), `pcmp.py` (P-record comparison),
`mkarm.py` (arm derivation), `census2h.py`, `census6h.py`, `p50.py`.

## 7. Theory provenance and the code review

Two derivations were run independently of this card's own: one blind (asked to derive what the
emitted phase bias should be, given the consumer's absorption and the three motions of an update),
one adversarial (asked to refute the smoothed hold). The blind derivation reached the smoothed hold
exactly, with the equivalent form P_j(t) = p_j(t|t) + [phi_j(t0|t) - phi_j(t0|t0)], the fixed-point
clone as a carried coupling row with no observation rows, and the caveat that the rule hides the
wander only as far as the estimator's phase-visible covariance is honest. The adversarial review
confirmed the algebra (the clone's recursion equals the exact update's coupling-row form; the
level coefficient m'u is 0.9978 to 0.9997 on a numerical toy of the posture), showed the rho decay
and the resulting ionosphere-free transfer, measured the smoothed proxy on both beds and the census,
and recommended the zero-motion member built here. Its numerical toy and offline wires are listed in
section 6.

A two-lens code review (correctness; off-path identity and the wire contract), each finding
verified by a separate refutation attempt, confirmed thirteen findings; none changes a GPS-only
number. Dispositions:

- One median across constellations would push a single constellation's common move (its own pivot
  handover, the frame hold's per-constellation re-anchor) into the other constellations' held phase
  biases. Fixed: M is kept per constellation (`pbHoldCommonClockStep` takes the constellation).
- An emitted epoch with no satellite left the previous clocks in place, so the next step bridged
  it. Fixed: such an epoch clears them.
- The FC-1 fast clock tier moves the clock between full epochs without the held phase bias (a
  sawtooth on the phase side). Fixed: the loader refuses `network.fast_clock.enable` with the mode.
- The posture limit was only an info line. Fixed: a warning with `network.apriori_source
  brdc_rts`; not a refusal, since the smoothed member is the owed replacement, not a prohibition.
- `n_pbh_frz_moved` counted rounding residue as a move and `n_pbh_frz_coupled` counted epochs
  without a clock. Fixed (a 1 um threshold; the clock required). The run-end line reports each
  distribution with its own sample count.
- The helper's declaration split `networkPbHold`'s documentation from its declaration. Fixed.
- The product-file legend declares `[,pbhold=1]`, not the value 2. Kept: changing the legend would
  change every product file's header, and the off path is byte-identical by rule; the value is
  documented on `SbasisContract::emit_pb_hold_phase_clock` and here.
- The live consumer's contract fingerprint (`ApplicationRealtime.cpp`) does not key on the hold
  or its mode, and the warm-resume fingerprint does not include either. Pre-existing for card PBH,
  unchanged here; the hold state is not snapshotted, so a warm resume starts new generations.
- The card lacked its gate and hash sections at review time (added), and the tests lacked the
  cases above (added).

## 8. Disposition

Built and gated, default off, not armed; no golden, ledger or posture change. What it does: it
removes the consumer loss the exact update's level motion caused under the plain hold (6 h census
2.27 / 0.75 / 0.57 / 0.38 / 0.26 / 0.20 m to 0.59 / 0.17 / 0.12 / 0.08 / 0.10 / 0.08 m), which was
card CTU2 section 9 item 1 and the block on its gates G6 and G7. Against the golden control the
exact update with the phase-clock hold is better in hour 1 (0.59 against 0.67 m), equal in hour 2,
and 4 to 16 mm worse in hours 3 to 6; the control with the phase-clock hold is the best wire
measured (0.51 / 0.17 / 0.12 / 0.08 / 0.07 / 0.05 m).

Arming is the author's decision, and three facts bear on it:

1. The posture limit (section 4). Within a generation the consumer's phase-visible clock motion is
   the a priori clock product's. That is what makes the hold both level-blind and better than the
   control in hour 1, and it makes the COD final clock load-bearing on the wire's phase side, which
   the standing directive to retire the a priori clock product works against. Under
   `apriori_source brdc_rts` the mode is not valid (the loader warns).
2. The named regressions, all at C1W sites: KOKB (hour 2 1.50 m against 0.22), QUIN, UTQI and, on
   the exact producer, ARHT through hour 6 (0.18 to 0.69 m against 0.02 to 0.07). They appear on the
   control producer too (ARHT excepted after hour 2), so they belong to the frozen phase-visible
   clock rather than to the exact update. Their mechanism is not established; the hour-1 part was
   traced by the adversarial review to the producer's first-epoch settling (it disappears for
   consumers starting at 00:20), but KOKB's hour 2 and ARHT's persistence are not explained by it.
3. The root that forces the motion term to zero is the estimator's phase-visible inconsistency: the
   ionosphere-free phase-visible clock wanders 2 to 5 cm within an arc on the sim, where its
   covariance states a few millimetres. With an honest phase-visible covariance the smoothed member
   (section 2) would be the exact answer on any a priori, including the broadcast one. That is a
   Method-1 item for the estimator (why the float ambiguities' revisions move the phase-visible
   clock without the covariance saying so), not for the hold.

Owed, in order: the mechanism of the C1W regressions (KOKB, ARHT first); the phase-visible
consistency of the estimator (an instrument like card CTU2 section 4's, on the phase-visible
functional); then the smoothed member, if the covariance becomes honest; a regenerated sim
fixture (card CTU2 section 1).

## 9. The family swap of the named regressions (2026-09-29)

A discriminating read before any mechanism: each C1W census site replayed with the other band-1
family (`census6h.py`, PBF on, 6 h, exe `_lah2`), on the control wire (`_rap_s6_ctl_f`), the
exact wire with the phase-clock hold (`_rap_s8_frz_f`) and the control wire with it
(`_rap_s8_cfrz_f`). The consumer's `processing.signal_priorities.GPS` loses C1W / C1P / C1Y (keys
`ctlC`, `frzC`, `cfrzC`), and for the sites whose latched phase is L1W also L1W / L1P / L1Y (keys
`ctlCC`, `frzCC`, `cfrzCC`, the full L1C + C1C family); the realized signals were read from
`closed_loop_sat_trace.csv`.

A consumer-side fact came first: the replay consumer keeps a band-1 code row only when the code's
tracking attribute matches the band-1 phase's. KOKB, HARB, POHN and DAV1 latch L1C phase with C1W
code, so their default consumers run with no band-1 code at all (empty `code_sig`, no code
innovation); forcing C1C gives them the full C1C family. At the L1W sites forcing C1C alone removes
the band-1 code, so the clean swap there is the full L1C + C1C family. The missing band-1 code
moves the control little (KOKB 0.61 / 0.22 m with none, 0.78 / 0.21 m with C1C in hours 1 / 2).

Consumer 3D p50 by hour (m), phase-clock hold on the exact wire (control in brackets, hours 1 to 6):

| site | latched family | as latched | other family |
| --- | --- | --- | --- |
| KOKB | L1C / C1W (no code) | 5.72 1.50 0.13 0.06 0.07 0.03 (0.61 0.22 0.22 0.15 0.13 0.04) | C1C: 5.74 1.53 0.13 0.06 0.07 0.03 (0.78 0.21 0.22 0.15 0.12 0.04) |
| POHN | L1C / C1W (no code) | 3.62 0.50 0.28 0.18 0.15 0.10 (0.65 0.40 0.40 0.23 0.17 0.16) | C1C: 3.54 0.58 0.30 0.19 0.13 0.05 (0.69 0.42 0.41 0.23 0.15 0.10) |
| ARHT | L1W / C1W | 2.96 0.55 0.69 0.21 0.24 0.18 (0.68 0.07 0.03 0.02 0.03 0.03) | L1C + C1C: 1.08 0.17 0.04 0.02 0.03 0.02 (0.57 0.06 0.03 0.03 0.02 0.03) |
| QUIN | L1W / C1W | 1.74 0.38 0.06 0.07 0.03 0.04 (0.22 0.08 0.02 0.02 0.02 0.02) | L1C + C1C: 0.16 0.05 0.02 0.02 0.03 0.02 (0.39 0.07 0.02 0.02 0.02 0.01) |
| UTQI | L1W / C1W | 1.41 0.39 0.27 0.15 0.14 0.12 (1.33 0.25 0.32 0.18 0.18 0.13) | L1C + C1C: 1.37 0.37 0.25 0.14 0.14 0.12 (1.21 0.23 0.31 0.17 0.18 0.13) |

Reading. The regressions are two different things. At ARHT and QUIN the phase-clock hold's
regression is a family effect carried by the L1W PHASE: on the L1C + C1C family it disappears (and
the hold beats the control), while C1C code on an L1W phase keeps it (ARHT 1.67 / 0.66 / 0.71 m).
At KOKB and POHN it is a site effect of hours 1 and 2 that no family change touches, consistent with
the adversarial review's first-epoch settling. UTQI is noisy on every wire and family and shows no
clear effect of the hold. Owed next for the L1W part: why a held L1W phase bias that follows the
satellite's clock is worse for an L1W consumer than one frozen outright, while the same rule helps
an L1C consumer (candidates: the sparse L1W family's own phase-bias datum and generation edges, and
an L1W inter-signal phase bias that is not constant). Owed for the consumer: whether dropping the
band-1 code on a mixed L1C / C1W latch is intended.

## 10. The premise of the smoothed member's rejection, corrected (2026-09-29)

Card [PVN](PVN_phase_visible_nees_2026-09-29.md) measured the estimator's covariance of the
ionosphere-free phase-visible clock D directly: on the sim its per-epoch update is consistent (NIS
0.87 to 1.03) and its state conservative (stated sd 3 to 28 cm against a within-arc wander of 2 to
5 cm, NEES 0.2 to 0.3); on the field both read below 1. Sections 2 and 8 item 3 above took D as
stated to a few millimetres (the adversarial review's numerical toy, and card CTU2 section 5's
2.2 mm p50, which is the size of one epoch's update, not the state's uncertainty). With the stated sd
as measured, the smoothed member's regression of D(t0) on D(t) stays near 1 for hours, so on these
beds it would behave like the phase-clock hold while passing the modelled walk, and would adapt to a
broadcast a priori through the walk's own q. The rejection in section 2 therefore stands only for the
offline proxy (rho = 1 / (1 + n / 60)); the smoothed member itself is owed a build.

## Annotation (2026-10-01): armed on the goldens by card EXL, as an interim hold

`network.emit_pb_hold_phase_clock` is armed on the goldens with card CTU's exact update
([EXL](EXL_exact_update_lah_promotion_2026-10-01.md)): under the plain hold the exact update costs the
field consumer 3.878 / 2.245 m in hours 1 / 2, with this hold 0.108 / 0.024 m. Interim: valid only
with a precise a priori clock product. Its named regressions stand on the 6 h census of card EXL
section 4: ARHT00ATA +0.404 m over hours 2 to 6, KOKB00USA hour 2 0.22 to 1.48 m, QUIN00USA +0.041 m.
