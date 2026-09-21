# TBI: the three-band ignition

**STATUS**

- **id:** TBI
- **topic:** network-vehicle
- **status:** open
- **summary:** Pre-registered 2026-09-20. With the three knobs of the day armed, GPS L5 is healthy from hour 3 and best in hour 6 (0.03 m) and still not a gain: the triple field arm has 314 epochs below a decimetre of 720 against 603 of the dual pair, because its first two hours are slow (1.47 and 0.21 m against 0.43 and 0.07 m). The manager's set-up read finds the lag Up-dominant, and finds the consumer's band-5 code rows misfitting the wire by about a metre per satellite for hours, in two groups that follow the wire's own band-5 code bias. The leading hypothesis is the band-5 code datum of AB2 section 1: a per-satellite assertion of a zero inter-signal correction. The card freezes the hypotheses, one batch of arms on both beds, the reading, and the L5 adoption bars before any arm is run.
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
