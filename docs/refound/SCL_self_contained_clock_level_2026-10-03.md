# SCL: a self-contained satellite clock level (the broadcast clock as the a priori)

**STATUS**

- **id:** SCL
- **topic:** network-vehicle
- **status:** open
- **summary:** OPEN, built 2026-10-03, not armed. The goal: a producer whose satellite clock does not need a precise a priori clock product (the field golden's hour-1 consumer, 0.108 m, rests on card LAH's phase-clock hold, valid only with one). On a bed with SP3 orbits and the broadcast clock as the a priori, card SMH's smoothed hold at a broadcast-grade anchor q (5e-6 m2/s) carries the broadcast clock's motion to the phase side, but the satellite clock level then locks (sim 0.104 m against 0.023 at the golden q; with the COD a priori too, 0.065 m: q alone), stated about 4 times too tight in standard deviation. Cause (U1, section 4.1): card CTU's per-row time update drops the cross-station time-update covariance, harmful only under per-satellite phase anchors as the producer elects them (unit network: level variance over-stated 6-fold at 16 stations). BUILT: B1, a one-epoch clock clone in the shared border (`network.carry_coupling_clock_clone`), with card SMH made consistent with it, and B2, the handover's dense S P S^T (`network.handover_stransform_dense`) with `network.clock_anchor_datum_keep`; B1 requires B2 (an interlock found by B2's runs). Off byte-identical; suite passes. Read (sections 9, 10): on the COD bed at q 5e-6 B1 returns the field consumer from 0.673 / 0.286 m to the golden's 0.107 / 0.024; on the broadcast bed the level halves (6 h 0.105 to 0.042 m, not converging further) and the 6 h field census goes from a median +2.8 cm and 10 sites worse against COD to +0.3 cm and 5 worse (sim census +0.5 cm, 2 worse); B2 removes the handover's 31 datum decouples and is consumer-invisible on the golden posture. Gate failures recorded: G1's level bars at epochs 120 / 239, G2's hidden fraction, G4's site count. Owed: the remaining level residue, the native broadcast posture, a per-satellite q.
- **source:** [CTU](CTU_coupling_time_update_2026-09-28.md) sections 4, 9 and 13; [CTU2](CTU2_level_recursion_followup_2026-09-29.md); [LAH](LAH_level_aware_hold_2026-09-29.md) sections 2, 4 and 10; [PVN](PVN_phase_visible_nees_2026-09-29.md); [SMH](SMH_smoothed_hold_clone_2026-09-29.md) sections 9 and 10; [REV](REV_revision_member_replay_2026-09-30.md); [WSX](WSX_within_station_cross_terms_2026-09-30.md) sections 6 and 7; [PRM](PRM_parameter_registry_2026-09-29.md) section 16.6; [EXL](EXL_exact_update_lah_promotion_2026-10-01.md); `docs/cards/clock/Q3-PHYSICAL-FLOOR.md` section 7
- **knobs / posture:** new, default off: `network.carry_coupling_clock_clone` (B1; requires `network.carry_coupling_exact_update`, `network.shared_carry_full_cov` and an active `network.handover_stransform_dense`; refuses `network.clock_model`, `network.clock_anchor_gauge_free`, `network.shared_carry_gauge_free`), `network.handover_stransform_dense` (B2; INERT without `network.handover_stransform` or the dense carry; refuses `network.handover_stransform_before_rows`, `network.carry_rx_clock_in_block`), `network.clock_anchor_datum_keep` (INERT without B2 or the anchor). The bed (section 2) is a data root; arms vary `network.clock_anchor_q` and the hold, with `network.clock_level_monitor` / `network.phase_visible_monitor`. Nothing armed; no golden, ledger or posture change
- **dates in the text:** 2026-10-03
- **hashes / run dirs:** exe `gnss_processor_scl0.exe` (a copy of `gnss_processor_prm3b.exe`, sha256[:16] `cc68a5125f13f2e7`) for section 3; data roots `runs/_scl_data_bc` (CLK sha256[:16] `d8683e16e7fcdfcd`) and `runs/_scl_data_cod` (`c5ce95d511850634`); arms `runs/_rap_scl0_*` (superseded, section 2), `runs/_rap_scl1_*` to `runs/_rap_scl8_*` (2 h; `scl4` 6 h); data roots `runs/_scl_data_pivtruth`, `runs/_scl_data_pivonly`; census keys `s1c`, `s1bqs`, `s1b`; unit-network reads in a worktree at main `3959f24` (section 4.1)

## 1. The question

The C record is a correction to the a priori clock, and the consumer adds it to its own copy of
the same a priori (`GnssCorrections.cpp:161, :344`). Under card LAH's phase-clock hold the wire's
phase-visible clock C - P_j is frozen within a generation up to a constellation-common term, so the
consumer's phase-visible clock motion within a generation is the a priori product's (card LAH
section 4). With the COD finals that motion is close to the truth; with the broadcast clock it is
not. On the current golden posture the precise a priori is load-bearing in five places: the clock
pivot pin (the pivot's correction is 0), card Q3's anchor q (2.9e-8 m2/s, fitted to the motion of
the corrections beyond COD, `Q3-PHYSICAL-FLOOR.md` section 7), the phase-clock hold, the consumer's
clock, and the S line's a priori identifier.

The end state of the standing directive on external data is SP3 orbits plus what arrives on the
signal. The card's question: with the broadcast clock as the a priori, can the producer's
satellite clock (the level on the code side, the phase-visible clock on the phase side) reach the
precise-a-priori posture's quality, and what in the estimator stops it.

## 2. The bed

No producer mode pairs SP3 orbits with the broadcast clock: `apriori_source brdc_rts` takes the
broadcast orbit too, refuses the goldens' satellite code bias without the SH field
(`GnssConfigLoadNetwork.cpp:4171-4190`), and its ephemeris cut-overs step the a priori. The bed
therefore writes the broadcast clock into the CLK product and leaves everything else as the golden
reads it:

- `runs/_scl_data_bc`: every GPS `AS` record of the COD CLK of 2024-128 / 129 / 130 replaced by the
  broadcast clock af0 + af1 dt + af2 dt^2 of `BRDC00IGS_R_20241290000_01D_MN.rnx` at that epoch (the
  ephemeris last transmitted at or before the epoch; on 2024-128 / 130 the nearest toc); on 2024-129
  each cut-over's step removed at the cut-over epoch (the declared re-level of a native broadcast
  posture, emulated), so the a priori is continuous; G01 dropped (broadcast health 63, 150 m from
  COD all day). SP3, ERP, OSB, GIM and the navigation file copied; `static` a junction to
  `data/static`.
- `runs/_scl_data_cod`: the same root with the COD CLK, G01 dropped (the control for the set change).

Producer and consumer read the root through `files.internal_data_root`; the census replays take it
as an extra setting. The first build of the roots lacked the neighbouring days (the golden's 3-day
window loads 11 product files, the root then 7): the field orbits were extrapolated at the day edge
and the field consumer started at 22.8 m. Those arms (`runs/_rap_scl0_*`) are superseded; the sim
arms (from 00:30:15) agree with the rebuilt ones to the millimetre.

The sim's satellite clocks are the COD CLK (`EphemerisProvider.cpp:236-242`), so on the sim the a
priori error is known exactly: e(t) = broadcast - COD, written per record to
`runs/_scl_data_bc/walk_bc.csv`. Graded with card SMH's G3 grader re-pointed at the re-based
fixture (the hidden fraction of e's de-commoned motion on the wire, the clock error with e and the
datum share removed) and with `levelnees.py` with e added (the level error in the pivot frame
against the code-only bound 1/I_L).

**The broadcast clock's error** (de-commoned per epoch over the healthy GPS satellites, 2024-129,
against COD): rms of the increment over 1 / 10 / 120 / 240 epochs, median over satellites, 18 / 48
/ 118 / 157 mm with the cut-over steps, 15 / 32 / 77 / 119 mm without them; cut-over steps p50 85 mm,
p90 190 mm, max 0.79 m (414 steps). The random-walk-equivalent q is 4.4e-6 to 5.8e-6 m2/s at lags of
15 min to 2 h and 1.7e-5 at 30 s (a white part): 150 to 600 times card Q3's 2.9e-8.

## 3. Fresh evidence (2026-10-03, 2 h, exe `_scl0`, the sim from 00:30:15)

**3.1 The in-process consumers** (3D p50 by run hour, m). Single-knob arms from the goldens: the
data root, then the anchor's q, then the hold.

| arm | a priori | anchor q | hold | field WTZZ00DEU | sim SZZ000SIM | field IF code spread h2 | sim IF code spread h2 |
| --- | --- | --- | --- | --- | --- | --- | --- |
| `scl1_c` | COD | 2.9e-8 | phase-clock | 0.108 / 0.024 | 0.223 / 0.099 | 0.105 | 0.056 |
| `scl1_cs` | COD | 2.9e-8 | smoothed | 0.128 / 0.029 | 0.233 / 0.091 | 0.105 | 0.056 |
| `scl1_b` | broadcast | 2.9e-8 | phase-clock | 1.191 / 0.743 | 0.518 / 0.425 | 0.097 | 0.067 |
| `scl1_bs` | broadcast | 2.9e-8 | smoothed | 0.871 / 0.379 | 0.475 / 0.219 | 0.097 | 0.067 |
| `scl1_bq` | broadcast | 5e-6 | phase-clock | 1.204 / 0.754 | 0.526 / 0.429 | 0.119 | 0.110 |
| `scl1_bqs` | broadcast | 5e-6 | smoothed | 0.099 / 0.021 | 0.219 / 0.075 | 0.119 | 0.110 |

Every arm graded with `ctugrade.py` emitted every epoch with 0 anchor trips and 0 S-basis completions.

**3.2 The 16-site field census** (`census2h.py`, PBF on, accumulating consumers, the data root of
the wire; median of the site p50, hours 1 / 2): COD with the phase-clock hold (`s1c`, reference)
0.417 / 0.110; broadcast with the phase-clock hold (`s1b`) 1.223 / 0.628, every site worse in hour 2;
broadcast, q 5e-6, smoothed hold (`s1bqs`) 0.550 / 0.157, paired hour-2 change +0.060 m at the C1W
median (3 better, 6 worse) and +0.063 m at the C1C median (1 better, 5 worse). WTZZ00DEU sits inside
the European cluster and flatters the wire, as card MSC found.

**3.3 The anchor q on the sim, against truth** (smoothed hold, broadcast a priori,
`runs/_rap_scl2_*`; COD control `scl2_cod`):

| anchor q (m2/s) | broadcast motion hidden from the phase side | clock error, code side, by hour (m) | level e2/(1/I_L) at epoch 239 | IF code spread h2 | phase-visible NIS (card PVN) |
| --- | --- | --- | --- | --- | --- |
| 2.9e-8 | 0.726 | 0.057 / 0.034 / 0.038 | 1.41 | 0.067 | 7.16 |
| 2e-7 | 0.510 | 0.055 / 0.052 / 0.045 | | 0.073 | |
| 1e-6 | 0.254 | 0.069 / 0.066 / 0.060 | 7.87 | 0.079 | 3.64 |
| 2e-6 | 0.201 | 0.085 / 0.081 / 0.080 | | 0.091 | |
| 5e-6 | 0.180 | 0.105 / 0.104 / 0.104 | 8.76 | 0.110 | 1.38 |
| 2e-5 | 0.213 | 0.115 / 0.112 / 0.105 | | 0.111 | 0.46 |
| COD control | | 0.048 / 0.023 / 0.022 | 0.68 | 0.056 | 0.17 |

The hidden fraction is over every held emission; the clock error is the de-commoned rms of C + e
plus the datum share; the level column is `levelnees.py`'s median normalized error against the
code-only bound. The consumer in this ladder is one site and changes sign between neighbouring
rungs (0.75 / 0.39 m at 2e-6, 0.22 / 0.08 at 5e-6), the ignition lottery of
`project_ignition_level_redraw_noise`; it is not read.

**3.4 The clock's epoch-to-epoch error.** The per-epoch change of the level error (pivot frame, e
removed, after the first half hour) has an rms of 24 / 19 / 14 mm at q 2.9e-8 / 1e-6 / 5e-6 on the
broadcast bed and 4.9 mm on COD. The carrier phase observes the clock's increment through 15 or
more stations per satellite at a few millimetres; a filter that used it would track the broadcast
clock's 15 mm per-epoch motion to that level at any q that admits the motion.

**3.5 One-change arms on the mechanism** (sim, broadcast a priori, smoothed hold, `runs/_rap_scl3_*`):

| arm | level error rms at epoch 60 / 120 / 239 (m, pivot frame) | e2/(1/I_L) at 239 | IF code spread h2 | anchor trips |
| --- | --- | --- | --- | --- |
| 60 stations, q 5e-6 (`scl2_q5e6`) | 0.100 / 0.116 / 0.107 | 8.76 | 0.110 | 0 |
| 31 stations, q 5e-6 (`scl3_h30q5`) | 0.100 / 0.111 / 0.115 | 3.99 | 0.113 | 0 |
| 31 stations, q 2.9e-8 (`scl3_h30q0`) | 0.087 / 0.090 / 0.044 | 0.68 | 0.085 | 0 |
| within-station cross terms on, q 5e-6 (`scl3_xcq5`) | 0.247 / 0.232 / 0.231 | 11.01 | 0.206 | 2 |
| clock datum sum-to-zero, q 5e-6 (`scl3_s2zq5`) | (no pivot frame) | | 0.123 | 2 |
| clock datum sum-to-zero, q 2.9e-8 (`scl3_s2zq0`) | (no pivot frame) | | 0.209 | 23 |
| COD control (`scl2_cod`) | 0.043 / 0.030 / 0.027 | 0.68 | 0.056 | 0 |

(The 31-station arms drop every second station of the roster, keeping S30000SIM.)

**3.6 Six hours** (`runs/_rap_scl4_*`, the same arms as section 3.1). Field WTZZ00DEU by hour:
COD with the phase-clock hold 0.108 0.024 0.016 0.020 0.030 0.026 m (the golden's numbers with G01
dropped); broadcast with the phase-clock hold 1.191 0.743 0.495 0.326 0.353 0.327; broadcast, q 5e-6,
smoothed hold 0.099 0.021 0.015 0.009 0.017 0.024, with the IF code spread 0.119 / 0.205 m in hour 2 /
the last hour against COD's 0.105 / 0.177 and 6 phase-guard trips against 0. Sim: the broadcast
smoothed arm's clock error stays at 0.105 to 0.097 m through all six hours (COD 0.048 to 0.016), and
SZZ000SIM reads 0.219 0.075 0.046 0.033 0.017 0.030 (COD 0.223 0.099 0.081 0.019 0.020 0.027). No
clock-pivot handover occurs in 6 h on the bed: with G01 out, G02 is the pivot throughout.

**3.7 The locked level is an ignition draw** (field, q 5e-6, the smoothed hold). The golden posture
with G01 kept as the GPS clock pivot (`runs/_rap_scl10_hq5_{f,s}`, 6 h, COD a priori; the "before"
arms of gate G7): field WTZZ00DEU 0.741 0.312 0.217 0.053 0.047 0.041 m, IF code spread 0.378 m in
hour 2, 31 anchor trips (all at the 04:42:30 handover); sim 0.276 0.080 0.070 0.043 0.018 0.025. Two
2 h field arms separate the pivot from the a priori (`runs/_rap_scl11_*`): COD with G01 dropped,
0.673 / 0.286 m, IF spread 0.353; the golden root with card HLT's `network.sat_health pivot` (G02
elected, G01 processed), 0.731 / 0.311 m, IF spread 0.372. The pivot does not decide it, and the
broadcast bed's 0.099 / 0.021 m with an IF spread of 0.119 (section 3.6) is not a property of the
broadcast a priori: at q 5e-6 the level does not converge, so its error is whatever the first epochs
drew (field 0.35 m with COD, 0.12 m with the broadcast clock; sim 0.065 and 0.104 m), and the
consumer inherits the draw. The field consumer of section 3.1 is therefore one draw, not a result;
the census (section 3.2) is the read. B1's gate G1 reads the level against truth on the sim and G3
the field's IF spread, on both a priori.

## 4. The reading

**The phase side is not the open problem.** Card SMH's smoothed hold passes the clock walk the
estimator attributes to time, and with the anchor's q at the broadcast grade the estimator
attributes the broadcast clock's motion to time: the in-process consumers match the COD posture on
both beds (section 3.1), the census recovers most of the broadcast loss (section 3.2), and card
PVN's phase-visible NIS reaches 1 near q 5e-6 to 1e-5 (section 3.3), a truth-free statement that the
q is right. This is card SMH's G3 reading (an injected walk on the old fixture) reproduced on the
re-based fixture with the real broadcast error, and on the field.

**The level locks when the clock walks.** At q 5e-6 the satellite clock level stops converging:
the error is flat from epoch 60 (0.10 to 0.12 m), its size does not change when half the stations
are dropped (so its excess over the code-only bound doubles with the station count, 4.0 to 8.8),
and the IF code spread follows on both beds (truth-free; field 0.097 to 0.119 m). The clock's
epoch-to-epoch error is 14 mm where the phase supports a few millimetres (section 3.4). Neither the
datum (sum-to-zero is no better) nor the within-station half of the time-update covariance (worse,
as card WSX found for the contrasts) is the cure.

**Mechanism (derived; the unit gate of section 6 is its test).** Card CTU's per-row form updates
each station's carried row on the moved border separately and drops the time-update covariance
between rows, M_a' B M_b with B = P_KK - P_KS (P_SS + Q_S)^-1 P_SK. At the golden q the inflation is
small, the rows keep their clock coefficient (m_new close to M) and the dropped term is negligible:
card WSX's unit network keeps the level with the batch. At a broadcast-grade q, Q_S dominates the
previous clock's variance: m_new tends to 0 on the clock and B tends to the previous border's own
covariance, so every station's row states its ambiguity absolutely with an independent copy of the
previous clock's uncertainty. The n rows of a satellite then state its satellite-common combination
about n times tighter than it is, the phase rows hand that statement to the new clock, and the
code rows can no longer move the level: card CTU's lock again, entered through the dropped
cross-station term instead of the shrunk coefficient. The exact cure keeps the shared uncertainty
shared: carry the previous epoch's walked clocks as border columns (one-epoch stochastic cloning),
so the rows keep their harvested coefficients on the clone, the walk sits in the border prior once,
and nothing is copied per row.

**What the precise a priori was doing.** With COD's error walking at 2.9e-8 the clock barely moves
between epochs, B stays small and the per-row form is nearly exact; the phase-clock hold then hides
the phase-visible motion, which COD already carries. A broadcast a priori removes both supports at
once: the motion must be estimated (it can be, section 3.1) and the clock must be allowed to walk,
which exposes the lock. Card PAC's pinned-pivot level and the clock-pivot handover's re-expression
(card PRM section 16.6: the fold's diagonal S-transform against rows that are already exact; S P S^T
owed) are the same family of inexactness in the border's time and frame updates; the handover is
taken here because a walked clock makes its variance term larger and because the Q3 anchor
decouples every clock of the constellation at that edge (4 epochs at the legacy q 1e-3), which
under the smoothed hold also kills every clone.

**4.1 U1 read (2026-10-03, before any producer code).** The unit network of
`tests/test_coupling_exact_update.cpp` with its clock walk made a test parameter (`gQClk`) and two
new recursion modes: mode 4, the clock clone with the joint harvest (exact by construction), and
mode 5, the clone with the producer's per-station harvest. Worktree at main `3959f24`; level of each
non-pivot satellite at the final epoch against the batch (variance ratio, error over the batch
sigma, the worse of the two satellites).

First read, the fixture's own datum (station 0 holds every satellite's ambiguities):

| stations | q (m2/s) | mode 3, shipped per-row | mode 4 | mode 5 |
| --- | --- | --- | --- | --- |
| 3 | 2.9e-8 | 1.0001, 0.011 | 1.0000, 0.000 | 1.0000, 0.004 |
| 3 | 5e-6 | 1.0001, 0.005 | 1.0000, 0.000 | 0.9999, 0.004 |
| 16 | 2.9e-8 | 0.9990, 0.027 | 1.0000, 0.000 | 0.9987, 0.048 |
| 16 | 5e-6 | 0.9987, 0.091 | 1.0000, 0.000 | 0.9986, 0.053 |

With one station holding every satellite's datum, the between-satellite phase-visible clock is
observed at phase precision each epoch, B stays at that size, and the dropped term does no harm.
The producer's datum is not that: the prioritized phase datum elects an anchor per satellite, so
the between-satellite phase-visible clock runs through float ambiguities (card PVN: its stated sd is
2.9 cm on the sim, the level's size). Second read, with satellite s anchored at station s
(`gAnchorRotate`, the probe `RotatedAnchorProbeSCL`):

| stations | q (m2/s) | mode 3, shipped per-row | mode 4 | mode 5 |
| --- | --- | --- | --- | --- |
| 3 | 2.9e-8 | 1.0010, 0.026 | 1.0000, 0.000 | 0.9989, 0.047 |
| 3 | 5e-6 | **0.7492, 1.398** | 1.0000, 0.000 | 0.9636, 0.109 |
| 8 | 2.9e-8 | 1.0019, 0.100 | 1.0000, 0.000 | 0.9927, 0.182 |
| 8 | 5e-6 | **0.2779, 0.880** | 1.0000, 0.000 | 0.9309, 0.128 |
| 16 | 2.9e-8 | 0.9999, 0.041 | 1.0000, 0.000 | 0.9808, 0.345 |
| 16 | 5e-6 | **0.1628, 0.591** | 1.0000, 0.000 | 0.9152, 0.048 |

The mechanism of section 4 holds with the producer's datum: at a broadcast-grade walk the shipped
form states the level up to 6 times too tight in variance, worse with every added station (the
production sim's normalized level error doubles from 31 to 60 stations, section 3.5), and the clone
removes most of it. The clone algebra is exact (mode 4). Mode 5 keeps a residue at both q (0.92 at
5e-6, 0.98 at 2.9e-8 with 16 stations): the per-station harvest drops the cross-station covariance
the clone's posterior leaves, the second-order term of section 5. At the golden q it is slightly
worse than the shipped form, so B1 is for the broadcast-grade q and G6 reads its cost at the golden
q. (Mode 4 misses the fixture's 1e-8 border tolerance by 2e-9 on two phase-bias values, the
rounding the test documents for L2; the same in information form, which the producer would use.)

Earlier in the day, before the rotated read, this section recorded the first table as a
falsification and section 6's rule as stopping the build. The rotated read supersedes it: the
first read tested a datum the producer does not use.

The level-innovation monitor (`network_level_innov.csv`, hour 2) gives the producer's own
statement on the sim: stated level sd 2.6 cm at q 5e-6 against 2.9 cm on COD, while the truth error
is 10.4 and 2.3 cm. Its NIS reads 1.1 on every arm because the single-epoch code variance (0.155 m2)
dominates the innovation; it cannot see the over-statement (the blindness card WSX found for card
PVN's instrument).

**4.2 The production bisection (2026-10-03, sim, 2 h, q 5e-6).** With the COD a priori (no true
walk) the level still locks: 0.070 / 0.064 / 0.065 m by hour at q 5e-6 and 0.058 / 0.048 / 0.049 at
1e-6, against 0.048 / 0.023 / 0.022 at 2.9e-8 (`runs/_rap_scl6_*`): the walk's variance, not the a
priori's error, is the cause, and the broadcast error adds to it (0.104). The pivot's own a priori
does not decide it (pivot at truth 0.102 to 0.105; only the pivot broadcast 0.065 to 0.070;
`runs/_rap_scl5_*`), nor does any one of 14 structural knobs (`runs/_rap_scl8_*`: the code datum,
the second code channel, the receiver code-bias carry and datum, the screens, the guard space, the
jump tests, S-basis completion, the re-admission seed, the station prior order, the drop decouple,
the phase-anchor re-reference, the ionosphere deweighting; all 0.062 to 0.070 except the
re-reference off, which breaks the solve), nor card 1f's gauge projection (inert on the exact
route, `runs/_rap_scl7_*`). The datum read of section 4.1 is the one structural difference that
reproduces it.

## 5. The build (default off; the goldens byte-identical)

**B1, the clock clone (`network.carry_coupling_clock_clone`, requires
`network.carry_coupling_exact_update` and `network.shared_carry_full_cov`; refuses
`network.clock_model` and `network.clock_anchor_gauge_free`).** Each epoch, every satellite clock
with a row in the stashed dense block gets a clone column c_s(t-1) in the shared border for that
epoch only (estimator key `sat|CLKC`, registered after every other column and before
`beginEpoch`, so no existing column index moves). The border prior is the joint Gaussian of
(s(t), clones): the stashed block over the previous border with each clock stood in for by its clone,
and the walk c_s(t) - c_s(t-1) with the fold's own variance Q_s (the anchor's decision, unchanged).
The coupling-row epoch the rows read is a copy of card CTU's with each cloned clock moved onto its
clone with Q = 0, so a row's harvested clock coefficients stay where they were harvested and the
walk enters once, in the border; card CTU's epoch itself is unchanged for card SMH's clone. The
rank gate and the S-basis completion read the border without the clones (a clone has a prior and
rows, not observations). After the solve the clones are marginalized at the stash (they are not
stashed) and at the harvest: a row's coefficient on a clone is regressed onto the free border
columns through the posterior covariance, and the clone's conditional variance is added to the
row's own (and within a station to the F3 block), which is mode 5 of the unit network. Telemetry:
clones registered and referenced per epoch, row terms on clones, the harvest's regression mass and
added variance, the clone's posterior standard deviation (the size of the second-order term), and
an INERT warning when no row references a clone. Sites: section 5.1.

**B2, the dense S-transform at the clock-pivot handover (`network.handover_stransform_dense`).** At
a clock-pivot handover the stashed border (values and `prev_carry_cov`) is re-expressed by
S P S^T, S = I - u e_q' over the constellation's clock family, in place of the diagonal bump
var_i + v_i^2 var_q (card PRM section 16.6's owed cure), and the clones of B1 with it. A second knob
on top, `network.clock_anchor_datum_keep`, leaves card Q3's anchor engaged at a handover whose carry
was re-expressed by B2 (counted), so the clones of B1 and of card SMH survive the edge. Sites:
section 5.1.

**The anchor's q** is a configuration value of the bed (5e-6 m2/s, the broadcast error's
random-walk equivalent at 15 min to 2 h and card PVN's NIS near 1). A per-satellite q derived online
from the phase-visible NIS is listed as owed (section 7), not built here.

**Not in this card:** a native producer mode pairing SP3 orbits with the broadcast clock (the
declared re-level at each cut-over, the hold's transfer net of that re-level, the replay driver's
broadcast feed and the S line's identifier); the bed's continuous CLK file emulates its declared
re-level. Also not here: arming anything, and the goldens' a priori (they stay on COD).

**5.1 Sites** (read-only code map at main `3959f24`, 2026-10-03; line numbers drift).

| step | site |
| --- | --- |
| knob, snapshot, fingerprint token | `GnssConfig.hpp`, `GnssConfigLoadNetwork.cpp` beside the CTU block (about 4745), `NetworkRunContext.cpp` knobs (about 134) and fingerprint (about 931), posture row |
| clone column kind | `NetworkEstimator.{hpp,cpp}`: registration, key, label, the dense-fold identity map (`denseFoldTerms_`), skipped by `toSolutionEpoch` and the closure counts |
| registration | `ApplicationNetworkAssembly.cpp` `networkAssemblyRegisterColumns_`, before `est.beginEpoch()` (about 2857) |
| the rows' epoch | `NetworkRunContext.cpp` `networkCouplingExactPrepare` (4017 to 4176): a second epoch with the clocks on their clones; PASS B's builder pointer (`ApplicationNetworkAssembly.cpp` about 3829) |
| the joint fold | `NetworkRunContext.cpp` dense fold (3820 to 3995) and `NetworkEstimator::addSharedPriorDense` |
| gate guard | `NetworkSolveGlue.cpp` S-basis completion (about 856) and rank gate (about 1035) |
| harvest marginalization | `NetworkRunContext.cpp` `networkHarvestLocalCarries` / `harvestCoupling` (4329, 4569 to 4613) and the F3 block |
| card SMH | `NetworkPbHoldClone.cpp` capture skips clone keys and reads card CTU's epoch |
| teardown | `NetworkTeardown.cpp`: one run-end line and the INERT warning |

B2's kernel sits in `NetworkHandoverStransform.{hpp,cpp}` beside `applyClockStransform` (which already
holds the carry): r = P e_q, P(i,j) += -u_i r_j - r_i u_j + r_q u_i u_j over the constellation's
clock family u, the incoming pivot's row and column zeroed, the outgoing pivot appended with value
-c0; idempotent; singular, so an undo needs a saved block. The anchor keep sits in
`NetworkClockAnchor.cpp` at the datum decouple (about 418 to 428).

## 6. Gates (frozen before the build)

One Release exe built from main after card PRM phase 3b step 2 grades every arm (invariant 6).

| gate | bar |
| --- | --- |
| U0 | Full Debug suite with no new failure; `make_golden_config.py --verify` PASS; `docs_index_check.py` passes. |
| U1, the prediction (read, section 4.1) | The unit network at q = 5e-6 m2/s with each satellite anchored at its own station: the shipped per-row form fails the level bars (variance ratio 0.749 / 0.278 / 0.163 at 3 / 8 / 16 stations). Committed as a test at the fixture's 3 stations: mode 3's level variance ratio below 0.8 at 5e-6 and above 0.99 at 2.9e-8. |
| U2 | The clone with the joint harvest (mode 4) equals the batch to 1e-7 at both q and both anchor layouts; the clone with the per-station harvest (mode 5) has a level variance ratio above 0.95 and an error below 0.2 batch sigma at 5e-6 with rotated anchors (3 stations); knob off bitwise. Estimator: a clone column is registered after every other column, labelled, folded by identity, and never emitted. B2: the dense transform equals the explicit S P S^T of the block to 1e-12, is idempotent, and leaves the knob-off path untouched. |
| G0 | Knobs off: 2 h, both beds, golden and broadcast bed, byte-identical to the same exe's controls (products without comment lines, `closed_loop_trace.csv`, `network_epoch_status.csv`). |
| ON | Run-end counters: clones per epoch above 0, rows pointed at a clone above 0, 0 factor failures; no INERT warning. |
| G1, the level (sim truth, broadcast bed, q 5e-6, smoothed hold) | Level error rms at epochs 60 / 120 / 239 at most 1.5 times the COD control's (0.043 / 0.030 / 0.027 m); median e2/(1/I_L) at epoch 239 at most 1.5; IF code spread in hour 2 at most 0.070 m. |
| G2, the phase side (sim truth) | Broadcast motion hidden over every held emission at most 0.10 (0.180 before); per-epoch rms of the level error's change at most 8 mm after the first half hour (14 mm before, COD 4.9). |
| G3 (field, truth-free) | IF code spread in hour 2 at most the COD control's + 0.010 m (0.105); phase-visible NIS mean in [0.7, 1.5]. |
| G4, census (field) | 2 h, then 6 h (`census2h.py` / `census6h.py`, the bed's data root, PBF on, accumulating consumers): against COD with the phase-clock hold, median paired change of the site p50 at most +0.010 m in hour 2 (2 h) and over hours 2 to 6 (6 h), and at most 3 sites worse by more than 0.02 m; reported against the broadcast wire without the build (`s1bqs`). |
| G5, census (sim) | `simcensus6h.py` with the bed's data root: the same statistic and bar against the COD control. |
| G6 (the precise posture) | The golden posture (COD, q 2.9e-8) with B1 on: WTZZ00DEU, SZZ000SIM and the 2 h census within 0.005 m of the control in every hour, level and IF spread no worse (the dropped term is small there). |
| G7, the handover (B2) | On a 6 h arm with a clock-pivot handover (the goldens have one at 04:42:30): with B2 and the anchor kept, 0 datum decouples, and the level and phase-visible errors in the 10 epochs after the handover within the run's p95 of the other epochs (sim truth). |
| G8 (cost) | 6 h loop time within 15 % of the same-exe control. |

Amendment (2026-10-03, 12:55, written while B1 was being built and before any B1 result): G1 is
also read on the COD a priori at q 5e-6 (the sim, `runs/_rap_scl6_codq5_s` as the control, no true
walk), with the same bars against the golden-q COD control (0.043 / 0.030 / 0.027 m); and G3 on the
field's COD a priori at q 5e-6 (control `runs/_rap_scl11_codq5_f`, IF spread 0.353 m): the IF spread
in hour 2 within 0.010 m of the golden-q COD control's 0.105 m. Section 3.7 is why: the locked level
is an ignition draw, so a single a priori can pass by luck.

If G1 or G2 fails with U2 passing, the reading is recorded and the remaining inexactness is attributed before any
further build. Arming is the author's decision after G4 and G5.

## 7. Order, and what stays owed

1. After card PRM phase 3b step 2 lands: U1 (the prediction, unit network only), then B1, U0,
   U2, G0 and the ON counters.
2. G1 to G3 on 2 h arms of the bed, both beds; G6 on the golden posture.
3. G4 and G5 (2 h, then 6 h).
4. B2 with U2's transform gate and G7 on 6 h arms.
5. Owed after this card: the native broadcast-clock posture (section 5), the per-satellite q from
   the phase-visible NIS, the retirement of COD from the goldens (the author's decision), and the
   C1W regressions of card LAH (KOKB00USA, ARHT00ATA), which the broadcast wire reproduces at KOKB
   (2.43 m in hour 2 against 2.63 m on the COD reference).

## 8. Tools

In `scripts/clock_level/` (moved from the session scratchpad with the code commit):
`scl_brdc_clk.py` (the broadcast clock against a CLK product: statistics, and a broadcast CLK
file), `scl_brdc_apriori.py` (the bed's CLK files and the a priori error file), `scl_grade.py` (the
one-line scorecard per arm: the pivot-frame level against truth and its bound, the per-epoch clock
error step de-commoned, the hidden broadcast motion through `g3grade.py`, the IF spread and trips,
card PVN's NIS, the consumer), `scl_levelnees_w.py` (`levelnees.py` with the a priori error added),
`scl_lvl_events.py` (level-error steps at phase-datum events against other epochs),
`scl_hybrid_wire.py` (a wire with the code side of one run and the phase side of another, for the
consumer replays of section 9). `g3grade.py` grades the re-based fixture unchanged (its truth OSB
file is identical in GPS to the new fixture's).

## 9. B1 built and read (2026-10-03)

**The build** (worktree at main `3959f24`, uncommitted at the time of this section; exes
`gnss_processor_sclbase.exe` `d7d3fc72b17a17aa` = the base, `gnss_processor_scl1.exe` `2724cb1856a41608` =
B1, `gnss_processor_scl2.exe` `ecdeb8198b0a9093` = B1 with card SMH made consistent with it). A new
seam `filter/NetworkClockClone.{hpp,cpp}`; the clone is a flag on the estimator's column descriptor
(key `sat|CLKC`, label `CLKC:sat`), registered after every other column; the rows read a copy of
card CTU's epoch with the cloned clocks on their clones; the joint fold is in information form
(stand-ins at the stashed variance, the walk factor at Q = var_eff - P); the gate, the S-basis
completion and the datum ratio read a view with the clones marginalized (a principal submatrix
would break the declared clock gauge); the harvest regresses clone coefficients onto the border
through the solve's reduced normals and adds their conditional variance, the clock-family deficit
restored on the pinned clock. Deviations from section 5, each in the build's notes: the capture
keeps the marginal over s(t) for card Q3, PVN and SMH and adds a companion joint capture for the
within-epoch replay; `network.shared_carry_gauge_free` is also refused.

**The smoothed hold had to follow.** With B1 alone the solve improved and the consumer under card
SMH degraded (sim 0.219 / 0.075 to 1.172 / 0.511 m, field 0.099 / 0.021 to 0.337 / 0.132): hybrid
wires replayed through the consumer put the whole loss on the wire's phase side (B1's code side with
the base phase side 0.150 / 0.108 m, the base code side with B1's phase side 0.461 / 0.116, base
0.136 / 0.107), the phase-clock hold was unchanged by B1, and with no hold B1 improved the consumer
(1.332 / 0.953 to 0.682 / 0.345). Card SMH's clone rows advanced on card CTU's epoch and missed the
revision the solve now puts on the previous clock through its clone. Fixed (exe `_scl2`): the
smoothed rows advance on the clone epoch, read the clones' revision, and are re-expressed through the
same regression as the coupling rows (unit test: the joint form equals the batch's fixed-point
smoother to 7e-9 m; the production row is within the per-station residue, +0.26 sigma at q 5e-6 with
rotated anchors, against +1.76 sigma for card CTU's row).

**Gates** (2 h unless stated; sim from 00:30:15):

| gate | reading | verdict |
| --- | --- | --- |
| U0 | Debug suite 2805 tests, 2788 passed, 17 data-fixture skips (empty `data/` in the worktree), 0 failed; `make_golden_config.py --verify` PASS (69 armed); `config_posture_check.py --verify-loader` 0 gaps; `docs_index_check.py` OK | PASS |
| U1 | committed form: mode 3 variance ratio 0.7492 at 5e-6, 1.0010 at 2.9e-8 (rotated anchors, 3 stations) | PASS |
| U2 | mode 4 = batch within 1e-7 at both q and both datums; mode 5 0.9636 / 0.109 sigma; estimator clone tests (7) | PASS |
| G0 | knob off byte-identical to the base exe: golden field, golden sim, broadcast bed sim and field (products, consumer trace, epoch status); `_scl2` again on the goldens; B1 with the phase-clock hold `_scl2` = `_scl1` | PASS |
| ON | clones 29.4 per epoch (sim), 28.6 referenced, 0.8 idle; 220 474 rows with clone terms; 0 fold fallbacks, 0 gate-view failures; harvest 221 304 rows marginalized; clone posterior sd p50 4.4 mm, max 12.2 mm | PASS |
| G1, broadcast bed | level rms at epochs 60 / 120 / 239: 0.042 / 0.072 / 0.056 m (off 0.100 / 0.116 / 0.107; bar 0.065 / 0.045 / 0.041); e2/(1/I_L) at 239 3.6 (bar 1.5); IF spread 0.064 m (bar 0.070); 6 h de-commoned clock error 0.049 0.043 0.044 0.040 0.039 0.042 m (off 0.105 to 0.097, COD 0.048 to 0.016) | FAIL (120, 239, e2); IF spread PASS |
| G1, COD at q 5e-6 (amendment) | 0.041 / 0.052 / 0.042 m (off 0.068 / 0.072 / 0.065); e2 1.3; IF spread 0.064 | FAIL at 120 and 239 by 7 and 1 mm; e2 and IF PASS |
| G2 | hidden broadcast motion 0.283 (off 0.180; bar 0.10); per-epoch clock-error step, de-commoned, 7.3 mm (off 9.5, COD 5.6; bar 8) | hidden FAIL; step PASS (see below) |
| G3 | field IF spread, broadcast 0.096 m (off 0.119; bar 0.115); COD at q 5e-6 0.109 (off 0.353; bar 0.115); phase-visible NIS 1.47 on the broadcast field arm (6 h) | PASS |
| G4, 2 h | per-site hour-2 change against COD with the phase-clock hold: median -0.003 m (off +0.060), 4 sites worse by more than 2 cm (off 11), 4 better | median PASS, sites FAIL by one |
| G4, 6 h | median of the site p50 by hour: COD 0.417 0.110 0.044 0.055 0.038 0.044; broadcast off 0.550 0.157 0.126 0.079 0.063 0.070; broadcast with B1 0.501 0.121 0.086 0.071 0.055 0.072. Per-site change of the mean over hours 2 to 6: median +0.003 m (off +0.028), 5 worse by more than 2 cm (off 10), 4 better | median PASS, sites FAIL |
| G5, 6 h sim census | per-station change over hours 2 to 5 against COD: median +0.005 m with B1 (off +0.006), 2 worse (off 3), 0 better | PASS |
| G6 | golden posture with B1 (phase-clock hold): consumer identical to the millimetre on both beds; sim level at 239 0.033 against 0.025 m; IF spread +0.001 | consumer PASS; level slightly worse (the mode-5 residue at the golden q) |
| G7 | B2, section 10 | |
| G8 | not measured (concurrent arms) | owed |

Notes on the reading. G2's step metric as written in section 6 was taken in the pivot frame without
removing the per-epoch common mode; card CLK-R's frame hold removes the common step from the emitted
clocks, so the pivot's own broadcast walk (about 12 mm per epoch) read as error on every satellite
(the 14 mm "before" figure). Recomputed de-commoned: COD 5.6 mm, broadcast 9.5, broadcast with B1
7.3; the bar of 8 mm is read on that. The sim census grader bins hours from the day start, and the
sim census replays start at 01:01:30 and run past the wire's end at 06:30:15 (their sixth hour has
no products); hours 2 to 5 are read. The field consumer at WTZZ00DEU is worse with B1 (6 h 0.233
0.093 0.038 0.026 0.034 0.046 m against 0.099 0.021 0.015 0.009 0.017 0.024) while the census
improves; WTZZ is one site (card MSC).

**Reading.** B1 is the mechanism's cure in production: on the COD bed at q 5e-6 the field consumer
returns from 0.673 / 0.286 m to the golden's 0.107 / 0.024 and the IF spread from 0.353 to 0.109 m;
on the broadcast bed the level halves and the field census goes from a median +2.8 cm and 10 sites
worse against COD to +0.3 cm and 5 sites worse. A residue remains: the level settles near 4 cm
instead of converging (stated about 2 times too tight in standard deviation), the per-station
harvest's second-order term (the clone's posterior sd, 4.4 mm p50, shared by every row of a
satellite) being the leading candidate; at the golden q it costs 8 mm of level at epoch 239 (G6).
The self-contained wire (broadcast clock, smoothed hold, q 5e-6, B1) is at the COD posture's median
on both censuses with a tail of 5 field sites; it is not armed.

## 10. B2 built and read (2026-10-03)

**The build** (exe `gnss_processor_scl3.exe` `6b5b827c33bbcfea` in the worktree; on main as part of this
card's code commit). `applyClockStransformDense` in `NetworkHandoverStransform.{hpp,cpp}`: at a
clock-pivot handover the stashed border is re-expressed by S P S^T over the constellation's clock
family (u = 1 on every clock row of the constellation in the block), the incoming pivot's row and
column zeroed, the outgoing pivot appended at -c0 with its old-frame row; values through the carry
traversal; a parked clock without a row takes the diagonal rule; an edge the kernel cannot apply
falls back to the diagonal kernel (counted). The seam records the edge for B1's fold, which then
stands the clones in across the edge in the outgoing frame with walks referencing the incoming
pivot's clone (c_s(t) - c_s(t-1) + c_q(t-1)). `network.clock_anchor_datum_keep` leaves card Q3's
anchor engaged for every clock the dense edge re-expressed with its self-check passed. The first
build's self-check judged symmetry over the whole stashed block, which is symmetric only to
rounding, so every dense edge read failed and the keep and the clone stand-in silently refused; the
armed-only counters showed it (invariant 4), the check now reads the written rows, and every arm was
rerun on the fixed exe.

**A defect of B1 found here, and the interlock.** With B1 and the diagonal kernel, the golden
handover (G01 to G02, 04:42:30) left the constellation's 31 clones free while the rows still
referenced them; their common mode was a null direction the clock datum does not remove, and the
solve failed for 30 epochs (690 of 720 emitted; both beds). The 2 h arms of section 9 never met a
handover. The loader now refuses `network.carry_coupling_clock_clone` unless
`network.handover_stransform_dense` is armed and active (with `network.handover_stransform` and
`network.shared_carry_full_cov`); unit test case `nodense`.

**Gates.**

| gate | reading | verdict |
| --- | --- | --- |
| U2 (B2) | `HandoverStransformDense`, 9 tests: every entry of the kernel's block equals the explicit S P+ S^T (Eigen) to 1e-12 of max abs P, values to 1e-12 m; x'_q = 0 and a zero row; every clock contrast and every bias against a contrast unchanged to 1e-12; a second call the identity; the result positive semi-definite; the diagonal kernel over-states a clock contrast by exactly 2 var_q; the not-applied paths and the knob-off seam bitwise; B1's joint fold across the edge equals the explicit joint law to 1e-7 | PASS |
| U0 | Debug suite 2814 tests, 2797 passed, 17 data-fixture skips, 0 failed (worktree); on main after the interlock, section 11 | PASS |
| G0 | all new knobs off, `_scl3` against the base on the 2 h goldens: identical (the same hashes as section 9); B1 on and B2 off, `_scl3` against `_scl2`: identical | PASS |
| ON | golden 6 h, both beds: 1 dense edge each (31 clocks, the outgoing pivot appended, 0 fallbacks, 0 self-check failures; variance the diagonal bump would have over-stated 3.7e-2 m2 field, 1.4e-2 m2 sim; largest contrast change below 1e-18 m2); with the keep 31 anchored clocks kept, 0 refused | PASS |
| G7 | datum decouples at the edge 31 to 0 on both beds and in the SCL regime; card SMH's clones killed by the withdrawal 62 to 0 (sim), 98 to 6 (field, the 6 from two phase-guard trips); sim truth, the datum-free level in the 10 epochs after the edge: 0.020 m against the run's p95 of 0.029 (golden posture), 0.041 against 0.045 (SCL regime with B1); the per-epoch level change at the edge 3.2 to 0.9 mm (golden posture). The phase-visible half of the bar was not computed | PASS on the level and the decouples; phase-visible half owed |

**Consumer.** On the golden posture B2 alone and with the keep is invisible: WTZZ00DEU and SZZ000SIM
are identical by hour to three decimals, the products differing from the edge on by at most 3.8 mm
in the clocks (card PRM section 16.6's prediction: the rows were already exact). In the SCL regime on
the golden root (G01 kept, q 5e-6, the smoothed hold, 6 h): with B1, B2 and the keep the field reads
0.126 0.027 0.023 0.018 0.029 0.042 m and the sim 0.233 0.132 0.105 0.051 0.017 0.027, against
0.741 0.312 0.217 0.053 0.047 0.041 and 0.276 0.080 0.070 0.043 0.018 0.025 without B1 (section 3.7):
the lock's field cost removed in hours 1 to 3. Hours 4 and 5 against B1 alone mostly measure B1's
30-epoch outage, not B2's covariance.

**Open.** Under B2 card SMH's orphaned clock-family deficits rise from 2 to 64 (sim) and 94 (field):
at the edge epoch the hold's row update still routes deficits to the outgoing pivot, which has left
the layout (not localized per epoch, not fixed). The one-epoch common term Q_q of the walk frame
across the edge is not added (about 12 mm at q 5e-6, documented in the headers). The appended
outgoing pivot is parked at the stash and seeds on its return (G01 at 279420) at a loose 3.27 m2,
where the controls return it cold. A skipped epoch right after a dense edge (the carry in the
incoming frame while the stash's pivot still names the outgoing one) is untested.

## 11. Landed on main (2026-10-03)

The worktree's code with the B1-requires-B2 interlock applied to main (`e3482f0` + this card's code
commit). Main's Release exe `gnss_processor_scl.exe` `8a1aa9255f3321d8`: 2 h goldens with every new
knob off byte-identical to the base exe on both beds (products a19e503fded57b5d field, d77b703c7c54a914
sim; consumer trace and epoch status likewise); B1 + B2 + keep on the broadcast sim bed byte-identical
to the worktree's `_scl3`. Main's Debug suite 2814 tests: 2813 passed, 1 fixture-gated skip, 0
failed. `make_golden_config.py --verify` PASS; `config_posture_check.py --verify-loader` 0 gaps.

## 12. The residue re-read (2026-10-06)

Card [SCL2](SCL2_level_residue_under_the_clone_2026-10-06.md) section 3 re-reads section 9's residue.
G1's pivot-frame reads on the broadcast bed include the pinned pivot's own broadcast walk through
card CLK-R's frame hold; de-commoned, the residue is 0.042 to 0.048 m on the COD and broadcast beds
alike at every q from 1e-6 to 2e-5. "Stated about 2 times too tight" is the epoch-239 value of a
ratio that grows to 3.3 at 6 h. The per-station harvest's term named in section 9 is the mechanism
in the unit network at production scale, where it compounds with the horizon (section 4.1's 60-epoch
read did not reach it).
