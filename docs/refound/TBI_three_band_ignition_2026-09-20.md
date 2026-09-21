# TBI: the three-band ignition

**STATUS**

- **id:** TBI
- **topic:** network-vehicle
- **status:** closed
- **summary:** CLOSED 2026-09-20, H1 STANDS on both beds: the network closes band 5's code datum by asserting a zero inter-signal correction per satellite (22 999 rows in 6 h), the truth is metres with a block structure (raw triple-code combination against the wire's: correlation 0.947 field, 0.962 sim), the network pushes the misfit into band 2, and the WIRE carries the damage (a dual consumer on the triple wire lags; band-5 code removed at the stations alone gives 0.27 0.10 0.05 0.04 0.06 0.02 m and 535 epochs below a decimetre). No config-only cure; the build is card B5D. Original summary: Pre-registered 2026-09-20. With the three knobs of the day armed, GPS L5 is healthy from hour 3 and best in hour 6 (0.03 m) and still not a gain: the triple field arm has 314 epochs below a decimetre of 720 against 603 of the dual pair, because its first two hours are slow (1.47 and 0.21 m against 0.43 and 0.07 m). The manager's set-up read finds the lag Up-dominant, and finds the consumer's band-5 code rows misfitting the wire by about a metre per satellite for hours, in two groups that follow the wire's own band-5 code bias. The leading hypothesis is the band-5 code datum of AB2 section 1: a per-satellite assertion of a zero inter-signal correction. The card freezes the hypotheses, one batch of arms on both beds, the reading, and the L5 adoption bars before any arm is run.
- **source:** [AB2](AB2_l5_third_frequency_2026-09-20.md) sections 1 (Datum), 8 and 11 (`sB_isc`); [CSL](CSL_canonical_signal_latch_2026-09-20.md) sections 4 and 5
- **knobs / posture:** `processing.frequency_mode: triple` with band-5 codes in the GPS priority list, on the RF-0hv working bases of `8fe757f` (hold, gap re-entry response, per-signal rider, priority latch); no new knob in this card
- **dates in the text:** 2026-09-20
- **hashes / run dirs:** `runs/_rf0hv5_{field,sim}6`, `runs/_rap_{lF_tri,lS_tri,gF_tri,hC_tri_all,sB_isc}`; exe `build/x64-Release/gnss_processor_csl.exe`

## 1. What is known (every number re-read from the run directories by the manager, 2026-09-20)

| arm | posture | consumer 3D p50 by hour (m) | sub-dm of 720 | T4_attrib | if_slip (band 5) | T0_band_scope | clock wire, hours 2 to 6, tracked / all (m) |
| --- | --- | --- | --- | --- | --- | --- | --- |
| `_rf0hv5_field6` | field dual | 0.43 0.07 0.06 0.08 0.08 0.07 | 603 | 126 | 113 (0) | 409 | 0.0069 / 0.0128 |
| `_rap_lF_tri` | field triple | 1.47 0.21 0.09 0.11 0.09 0.03 | 314 | 242 | 241 (112) | 705 | 0.0073 / 0.0182 |
| `_rf0hv5_sim6` | sim dual | 0.60 0.10 0.10 0.06 0.06 0.05 | 448 | 0 | 0 | 194 | not re-read |
| `_rap_lS_tri` | sim triple | 0.56 0.16 0.08 0.04 0.04 0.04 | 430 | 0 | 0 | 276 | not re-read |

**The lag is Up-dominant.** Consumer by half hour, first three hours: 3D p50, then the
median signed Up (m).

| arm | 3D p50 by half hour | median Up by half hour |
| --- | --- | --- |
| field dual | 1.27 0.18 0.07 0.08 0.05 0.08 | +1.07 +0.17 -0.01 -0.05 -0.05 -0.08 |
| field triple (`lF_tri`, priority latch) | 3.64 0.73 0.29 0.14 0.10 0.09 | +3.54 +0.67 +0.27 +0.13 +0.08 -0.08 |
| field triple (`gF_tri`, count latch) | 1.63 0.41 0.31 0.27 0.14 0.16 | +1.06 +0.17 -0.01 -0.04 -0.03 -0.13 (its lag is East: +0.25 +0.34 +0.28 +0.24 +0.13 +0.09) |
| sim dual | 1.74 0.19 0.09 0.11 0.11 0.07 | -1.23 -0.05 +0.07 +0.11 +0.11 +0.06 |
| sim triple | 1.39 0.24 0.17 0.16 0.11 0.05 | +0.42 +0.08 +0.16 +0.16 +0.10 +0.05 |

**The consumer's band-5 code rows misfit by about a metre, per satellite, for hours.**
`closed_loop_sat_trace.csv`, the p50 of the absolute post-fit code residual by half
hour (m):

| arm | band 1 | band 2 | band 5 |
| --- | --- | --- | --- |
| field dual | 0.13 0.12 0.12 0.14 0.12 0.18 0.21 0.18 | 0.11 0.09 0.11 0.10 0.10 0.13 0.12 0.11 | |
| field triple (`lF_tri`) | 0.19 0.14 0.14 0.14 0.14 0.23 0.25 0.23 | 0.32 0.20 0.28 0.24 0.23 0.36 0.38 0.38 | 1.08 0.94 1.04 0.78 0.86 1.12 1.16 0.31 |
| field triple (`gF_tri`) | 0.20 0.13 0.16 0.17 0.16 0.23 0.18 0.19 | 0.21 0.18 0.24 0.26 0.24 0.34 0.34 0.33 | 1.03 0.89 1.04 0.86 0.91 1.17 1.12 0.38 |
| sim triple | 0.60 0.48 0.45 0.44 0.38 0.39 0.39 0.35 | 0.69 0.48 0.46 0.38 0.36 0.37 0.51 0.62 | 0.62 0.55 0.52 0.58 0.50 0.45 0.51 0.47 |

On the field the band-5 code misfits four to six times more than bands 1 and 2, and
the band-2 misfit is two to three times the dual pair's; on the sim the three bands
are equal. The epoch mean of the signed band-5 residual over satellites is near zero
(a receiver-common term is absorbed), the spread across satellites is 0.8 to 1.1 m,
and the per-satellite signed medians are persistent and fall in two groups that
follow the wire's own band-5 code bias `cb_m` (consumer at WTZZ00DEU, signal C5X):

| satellite | median residual, hours 1 to 2 (m) | hours 3 to 6 (m) | wire `cb_m` median (m) |
| --- | --- | --- | --- |
| G11 | | +1.62 | -4.72 |
| G14 | +0.76 | +1.05 | -4.60 |
| G23 | +0.71 | +1.25 | -4.58 |
| G28 | | +1.76 | -5.27 |
| G06 | | -0.03 | +2.02 |
| G10 | | -0.97 | +1.53 |
| G24 | -1.00 | -0.06 | +1.41 |
| G25 | | +0.01 | +2.65 |
| G30 | -1.13 | -0.99 | +2.53 |
| G32 | | -0.55 | +0.60 |

The wire's band-5 code bias hardly moves (p50 of the distance to its arc-end value 0.09
m in the first half hour, 0.05 m or less afterwards), where the band-1 column of the same
wire moves by 0.24 m (p90 0.89 m against 0.28 m on the dual wire).

**What the record already holds.** AB2 section 1, Datum: band 5 is closed by the
per-signal GF pin at the same c T_GD (`NetworkSbasis.cpp`, the block near lines 903 to
970), which asserts a zero inter-signal correction between L1 and L5 and is counted
as `n_sat_gf_isc_zero`; the channel's sum-to-zero row is then suppressed, so that
assertion is the band's only code datum; `sat_isc_state` is guarded to band 2. AB2
section 11, sim arm `sB_isc` (a constant C5 code bias per satellite, sigma 2 m, on the
posture of that hour): 1.20 0.14 0.05 0.05 0.06 0.05 m against 0.56 0.16 0.08 0.04 0.04
0.04 m, read then as "a slower first hour only" because the question then was the
post-fit refloat loss, not the ignition.

**Two more facts of the set-up read.** The priority latch changes two things on the
field triple against the count latch: six stations move to C1W / L1W (CSL), and 16
stations that track L1C phase with C1W code (CEBR00ESP, DJIG00DJI, GOP600CZE,
GRAZ00AUT, GUAM00GUM, HOLB00CAN, JPLM00USA, KIT300UZB, MAJU00MHL, MRC100USA,
NIUM00NIU, NYA200NOR, PALM00ATA, PARK00AUS, PTBB00DEU, SEYG00SYC) move their CODE to
C1C because the code follows the phase's attribute, so the C1W code columns that the
consumer uses lose support (28 to 18 stations); band 5 is C5Q at 29 stations and C5X
at 30 under both rules. And the within-arc clock wire does not rank the first hour:
`hC_tri_all` has the best first hour of any triple arm (0.31 m) and the worst wire
(0.0127 / 0.0270 m); the first hour is a LEVEL question, which that instrument removes
by construction.

## 2. Hypotheses (frozen before any arm of this card)

- **H1, leading: the zero inter-signal-correction assertion on band 5.** With the
  slant ionosphere and the satellite clock fixed by bands 1 and 2, a satellite's
  band-5 code bias is estimable up to one constant per signal family, so a
  per-satellite pin is not a datum choice but n - 1 assertions, each wrong by that
  satellite's true correction (metre level). The wire then carries a band-5 code
  bias that does not fit any receiver's band-5 code, satellite by satellite. The
  consumer's code-dominated first hours are pulled by it until the phase takes over,
  and the network pushes part of the same misfit into what bands 1 and 2 share (the
  consumer's band-2 residual rises). Predictions: **P1a** the network's own band-5
  code post-fit means per satellite are common to the stations and agree with the
  consumer's per-satellite medians in sign and size (correlation 0.8 or more over
  the satellites both see); **P1b** the sim with a constant per satellite on the
  band-5 code, on today's armed posture, reproduces a lag of the first two hours
  that scales with the constant's sigma; **P1c** the field triple with the band-5
  CODE removed everywhere, consumer included, and the band-5 phase kept, ignites
  like the dual pair; **P1d** a dual consumer replayed on the triple wire separates
  the two halves: if it ignites like the dual pair the loss sits in the consumer's
  band-5 code rows, if it lags the network has absorbed the assertion into the
  clocks or the ionosphere.
- **H2: the code-follows-phase move** of 16 stations' band-1 code under the priority
  latch. It is present on the dual pair too, whose first hour under the priority
  latch (0.43 m) is faster than under the count latch (0.53 m), so alone it does not
  explain the triple; it may explain why the triple's lag is Up under one latch and
  East under the other.
- **H3: band-5 slip traffic** (`if_slip` 241 with 112 on band 5, `T0_band_scope` 705
  against 409) delays the band-5 phase-bias columns or refloats the consumer's
  satellites in hours 1 and 2. Prediction: the tally by hour is front-loaded, and the
  consumer's L5 phase rows enter later than its L1 and L2 rows.
- **H4: anything else** on the band-5 path of the consumer (the prior of its receiver
  band-5 code bias, the hold generations of the L5 phase-bias columns).

## 3. The batch (one table, run by an Opus worker, four arms at a time)

| tag | bed | the one change | the question |
| --- | --- | --- | --- |
| `tR_dual_on_dual` | field | consumer replay of the dual wire (`network.closed_loop.replay_products_path`) | instrument validity: the replay reproduces the closed loop of `_rf0hv5_field6` |
| `tR_tri_on_tri` | field | consumer replay of the triple wire, triple consumer | instrument validity on the triple |
| `tR_dual_on_tri` | field | consumer replay of the TRIPLE wire by a DUAL consumer | P1d |
| `tA_nocode5` | field | band-5 code types stripped at every station and the consumer, phase kept | P1c (latch of bands 1 and 2 checked unchanged) |
| `tS_isc05`, `tS_isc1`, `tS_isc2` | sim | a constant per satellite on the band-5 code, sigma 0.5, 1 and 2 m, stations and consumer | P1b and its dose |
| `tS_isc1_nocode5` | sim | the sigma 1 m arm with the band-5 code stripped everywhere | the sim's answer to P1c |

The worker may add one-change arms that the reads call for, stated in its report with
the question each one answers. A RINEX-edit arm is attributed only after the
canonical-signal latch has been compared with its base (the lesson of CPA section 5).

## 4. The reading and the bars (frozen)

- **H1 stands** if P1a holds and at least one of P1b and P1c holds. The cure is then a
  build card of its own (the band-5 code datum estimated: a free column per
  satellite and signal, closed by the family's sum-to-zero row in place of the
  per-satellite assertion; default OFF, byte-identical OFF, the change of datum an
  event at both edges). This card scopes it and does not build it.
- **H1 falls** if P1a fails (the network's band-5 code rows fit where the consumer's
  do not), or if both P1b and P1c fail.
- **L5 adoption bars**, for whatever posture comes out of the cure: field triple, 6 h:
  every hour within 0.03 m of the dual pair or better, 570 or more epochs below a
  decimetre, clock wire at or below 0.0075 m on the consumer's satellites and 0.0140
  m over all; sim triple within 0.03 m of the sim dual pair in every hour; then a 12
  h field arm never worse than `runs/_rap_lD_dual12` by more than 0.03 m from hour 3.
  The ledger rows of every knob involved are read before any recommendation.

## 5. 2026-09-20: TBI-1 result: H1 stands on both beds; the WIRE carries the damage; there is no config-only cure

Read and batch by an Opus worker, exe `gnss_processor_csl.exe`, no source edited. The
manager re-read from the run directories: every scorecard line, the latches, the
band-5 row counts, the wire identity of the two strip arms, the clock wire, P1a with
an independent script, P1e by re-running the worker's scripts after reading them
(`tbi_p1e.py`, `tbi_p1e4.py`, `tbi_p1e5.py` in the session scratchpad), the first-row
epochs of H3, and the code lines of the scoping. What is the worker's alone is marked.

**P1e, estimator-free: HOLDS, on both beds.** The triple code combination that is free
of geometry and of the first-order ionosphere, D = 0.226180 P1 - 1.226180 P2 + P5, from
the RINEX files with each station's latched signals, elevation above 20 degrees,
median per (station, satellite), split by least squares into a station term and a
satellite term B. Field: family C5Q 29 stations, 17 satellites, 245 cells, fit
residual 0.119 m rms, B across satellites 2.381 m (sd); family C5X 30, 17, 247, 0.217 m,
2.437 m; the two families agree to a median of +0.046 m with 0.216 m sd over the 17
satellites. The six Block III satellites sit 4 to 5 m above the eleven Block IIF
ones. The same combination W of the wire's emitted code biases carries 0.430 m rms of
a 2.973 m truth over the nine satellites the consumer sees, and B - W reproduces the
same combination R of the consumer's code residual medians: correlation 0.947, slope
1.019, rms 2.647 against 2.493 m.

| satellite | B (m) | W (m) | B - W (m) | R = a r1 + b r2 + r5 (m) | r1, r2, r5 (m) |
| --- | --- | --- | --- | --- | --- |
| G06 | -0.100 | -0.157 | +0.057 | -0.092 | -0.150 +0.269 -0.102 |
| G11 | +4.163 | +0.336 | +3.826 | +4.038 | +0.052 -1.693 +1.577 |
| G14 | +4.455 | +0.915 | +3.540 | +2.312 | +0.137 -0.893 +0.813 |
| G23 | +4.280 | +0.469 | +3.811 | +2.782 | +0.236 -1.182 +0.906 |
| G24 | -0.111 | -0.256 | +0.145 | -0.527 | -0.280 +0.556 -0.155 |
| G25 | 0.000 | 0.000 | 0.000 | 0.000 | -0.678 +0.185 +0.007 |
| G28 | +4.697 | +0.257 | +4.440 | +4.623 | +0.299 -2.066 +1.648 |
| G30 | -0.324 | -0.571 | +0.248 | -1.755 | -0.121 +0.795 -1.127 |
| G32 | -1.357 | -0.103 | -1.254 | -1.345 | -0.405 +0.866 -0.565 |

(B, W and R each referred to their median over the nine, which is G25.) The misfit
lands on band 2 as much as on band 5. **The sim carries the same defect:** 60 stations,
32 satellites, 980 cells, fit residual 0.087 m, B across satellites 3.902 m, the wire
carries 0.558 of 3.111 m, correlation 0.962, slope 0.870. Section 1's "on the sim the
three bands are equal" is corrected: a median pooled over satellites hides a
per-satellite pattern, and the simulator injects band-5 satellite code biases with
the same block structure. The sim pays less because all 32 of its satellites carry
L5, so both groups are in view at every epoch, where the field consumer sees four L5
satellites in hours 1 and 2, two of each group (the worker's reading of the cause of
the Up sign; not tested further).

**P1a HOLDS.** Manager's independent read of `network_pf_rows.csv` (`tbi_regrade2.py`):
the network's band-5 code post-fit, station-median per satellite, is +0.79 to +1.36 m
on the six Block III satellites (G04 +1.04, G11 +1.08, G14 +0.84, G18 +0.94, G23 +0.79,
G28 +1.36) and -1.18 to -0.01 m on the eleven Block IIF ones, 0.791 m sd across
satellites; against the consumer's per-satellite medians the correlation is 0.945 and
the slope 1.08 over ten satellites (the worker's selection: 0.929 and 1.05). **The
network pushes the assertion into band 2:** the station-median of the band-2 code
post-fit per satellite has 0.740 m sd across satellites on the triple run against
0.066 m on the dual run (band 1: 0.240 against 0.110 m).

**P1c HOLDS, and P1d says it is the wire, not the consumer.**

| arm | the one change | consumer 3D p50 by hour (m) | sub-dm | median Up, half hours 1 to 3 (m) | clock wire, tracked / all (m) |
| --- | --- | --- | --- | --- | --- |
| `_rf0hv5_field6` | dual pair | 0.43 0.07 0.06 0.08 0.08 0.07 | 603 | +1.07 +0.17 -0.01 | 0.0069 / 0.0128 |
| `_rap_lF_tri` | triple | 1.47 0.21 0.09 0.11 0.09 0.03 | 314 | +3.54 +0.67 +0.27 | 0.0073 / 0.0182 |
| `tA_nocode5` | band-5 code removed at every station and the consumer, L5 phase kept | 0.27 0.10 0.05 0.04 0.05 0.05 | 535 | -0.17 +0.02 +0.01 | 0.0078 / 0.0153 |
| **`tB_nocode5_net`** | removed at the stations only | **0.27 0.10 0.05 0.04 0.06 0.02** | **535** | -0.17 +0.02 +0.01 | 0.0078 / 0.0153 |
| `tA_nocode5_cons` | removed at the consumer only | 1.58 0.20 0.10 0.12 0.10 0.05 | 234 | +3.37 +0.68 +0.26 | as `lF_tri` |
| `tR_dual_on_tri` | a DUAL consumer replayed on the triple wire | 1.48 0.16 0.10 0.11 0.14 0.07 | 226 | +3.25 +0.60 +0.18 | |
| `tR_dual_on_dual`, `tR_tri_on_tri` | replay controls | equal to their closed loops (603 and 314 epochs; half-hour p50 to 0.002 m) | | | |
| `tF_count_nocode5` | the count latch with the band-5 code removed (`gF_tri`: 0.69 0.31 0.15 0.14 0.12 0.17, 6) | 0.42 0.08 0.09 0.09 0.11 0.17 | 331 | | |
| `tE_gfzero` | `sat_gf_datum: zero` | 1.45 0.19 0.10 0.12 0.09 0.03 | 286 | +3.52 +0.65 +0.25 | |

The two strip arms have byte-identical wires; their bands-1-and-2 latches equal the
base's at 59 of 59 stations, and band 5 stays in the solve (85 388 and 85 553 phase
rows on L5X and L5Q in the arms' post-fit trace against 85 506 and 85 818 in the base;
2549 consumer L5X rows in each). Removing the band-5 code at the consumer alone changes nothing;
removing it at the stations alone is the whole gain, and a consumer that uses no band
5 at all is slowed by the triple wire. The damage is in what the network solves for
bands 1 and 2 under the assertion. Without the band-5 code the three-band network
beats the dual pair in hours 1 and 3 to 6 and loses hour 2 by 0.03 m (an East lag of
-0.21 -0.14 -0.13 -0.08 m by half hour, not read).

**P1b partial.** Sim arms with a constant per satellite on the band-5 code, sigma 0.5, 1
and 2 m: first half hour 0.96, 1.21, 2.41 m, second 0.14, 0.19, 0.50 m, monotone in the
dose, but the fixture's own arm sits at 1.39 m, between the last two, because the
fixture already carries 3.1 m of it. By hour: 0.21 0.14 0.06 0.05 0.05 0.05 (455), 0.31
0.10 0.04 0.04 0.06 0.05 (517), 1.20 0.14 0.05 0.05 0.06 0.05 (464, equal to AB2's
`sB_isc` to the digit). The sim strip arm: 0.98 0.08 0.07 0.04 0.03 0.05 m, 522 epochs,
against 430 of the sim triple and 448 of the sim dual pair; its first hour is not
better.

**H2 separable and second order; H3 and H4 refuted.** The band-1 code column of the
wire moves twice as much on the triple (p90 of the first half hour 0.89 m against 0.28
m) and returns towards the dual level with the band-5 code removed under the same
priority latch (0.41 m), so the band-5 code drives it, not the latch; the priority latch with the strip
is better than the count latch with it (535 against 331 epochs). The consumer's L5
rows enter at the same epoch as its L1 rows on all ten satellites, and the strip arms
carry the same band-5 slip traffic (`if_slip` 241, 112 on band 5) and ignite at 0.27 m.
The consumer's receiver band-5 bias is not it (the consumer-only strip is null).

**No config-only cure exists (tested).** `network.sat_gf_datum: none` removes the pair's
pin with the band-5 one and gives zero solution epochs on both beds;
`network.code_bias_gf_datum` is refused by the loader together with
`w6_iono_constraint: none`; `zero` moves the target only and is a null (`tE_gfzero`);
no emission filter by band exists. The run states the assertion itself: the triple
field run folds 22 999 per-signal rows beyond the pin pair, "of which 22 999 ASSERT an
inter-signal correction of ZERO", the dual run none; under the time link 45 397
geometry-free rows carry their own previous value and 75 are anchored at the
broadcast target, so the assertion is made at first sighting and held.

**Reading (section 4).** H1 stands: P1a and P1c hold, P1e holds on both beds, P1d
places the loss in the wire. The cause is the per-signal extension of the
geometry-free pin (`NetworkSbasis.cpp`, the loop at lines 903 to 970), written for
same-band channels where the correction is small, applied to a third BAND where it
is metres and, with bands 1 and 2 pinned, estimable. Against the adoption bars of
section 4 the emulation `tB_nocode5_net` passes the hourly clause (worst +0.03 m, hour
2) and fails the count (535 against 570) and the wire (0.0078 / 0.0153 against 0.0075 /
0.0140 m); it removes the band-5 code and does not cure the datum, so the bars apply
to the build. The build is card
[B5D](B5D_band5_code_datum_2026-09-20.md); this card is closed.
