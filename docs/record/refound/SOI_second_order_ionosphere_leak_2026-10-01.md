# SOI: the second-order ionospheric term as the field stations' leak

**STATUS**

- **id:** SOI
- **topic:** network-vehicle
- **status:** closed
- **summary:** FALSIFIED 2026-10-01: the second-order ionospheric term is 0.7 mm rms where the estimator cannot absorb it, against 4.1 mm of slow residual, with no residual slope and no walk-share correlation; it is not the leak.
- **source:** the outside review's note after card FWS; [FWS](FWS_field_walk_share_sources_2026-10-01.md) sections 2.1, 4 and 5; [PAC](PAC_common_drift_pivot_clock_2026-10-01.md); `include/atmosphere/HigherOrderIonosphere.hpp`, `src/core/atmosphere/MagneticField.cpp` (the dipole); `src/core/filter/NetworkIonoDiag.cpp` (the slant ionosphere trace, `iono_total_m` on L1 in metres); `src/core/filter/NetworkFrameDiag.cpp` (the post-fit rows, signed v = y - A x)
- **knobs / posture:** none; traces only
- **dates in the text:** 2026-10-01
- **hashes / run dirs:** traces of `runs/_rap_fws_wuacc6_f` (field 6 h, accumulated wind-up, the primary), `runs/_rap_rev1_blk6_f` (field 6 h control), `runs/_rap_rev1_blk6_s` (sim 6 h, the null); script `scripts/clock_level/soi_test.py`

**Outcome.** The card asked whether the second-order term, which survives the ionosphere-free combination and changes sign with the line of sight's angle to the geomagnetic field, is the per-line-of-sight leak card FWS found. The residual slope on the predicted term is -0.02 +/- 0.08 where full size would read +1 at t about 13, and the walk-share r^2 at 120 epochs is 0.000 (bar 0.25): S1, S2 and S4 FAIL, S3 holds. Card [I2S](I2S_second_order_iono_sign_2026-10-01.md) settled the sign left open (the engine's convention was inverted, now fixed). The fallback state went to card [LGM](LGM_los_phase_nuisance_2026-10-01.md), the long-scale remainder to card [LSR](LSR_long_scale_remainder_2026-10-01.md).

## 1. The term and its sign

The quasi-longitudinal Appleton-Hartree index of a right-hand circularly polarized wave travelling
along the field is that of the mode that co-rotates with the electrons, so its phase advance grows
when the propagation direction k (satellite to receiver) is parallel to B. With
q = 7527 c STEC B cos(theta_k) (STEC in electrons per square metre, B in tesla), the code delay is
+q / f^3 and the phase delay -q / (2 f^3). The ionosphere-free combination of the two phases keeps

    p = q / (2 f1 f2 (f1 + f2)),

about 6 mm for 100 TECU and B cos(theta_k) = 3e-5 T. At low latitudes B is nearly horizontal and
northward, so p changes sign between lines of sight looking north and south: the signature no
station-wide or region-wide error has. The engine's `HigherOrderIonosphere` (and the POINT reference
it was checked against) form B with the receiver-to-satellite vector and a positive code coefficient,
the opposite sign; the engine's term (`model_corrections.enable_higher_order_ionosphere`) is off on
both goldens and needs a global ionosphere map, which the self-contained posture does not load, so
nothing models the term on these runs. This card's prediction uses
the sign derived here and reports the sign found.

The term's own time scale is the pass (STEC and geometry change over tens of minutes to hours); at
0.06 mm per TECU, minute-scale TEC fluctuations of a few TECU give a few tenths of a millimetre. The
physics therefore expects the term, if it leaks, in card FWS's long scales (the 60- and 120-epoch
persistence and the unattributed third at two hours), not in its short-scale colour (lag-1
autocorrelation 0.65, decorrelated within 10 minutes).

## 2. The instrument

Per station, satellite and epoch: STEC from `iono_total_m` (L1 metres, 40.3e16 / f1^2 per TECU; the
receiver's code-bias split leaves a station offset of a few TECU, inside the test's tolerance), the
line of sight from the trace's azimuth and elevation at the station's ECEF, a pierce point at 450 km,
B from the dipole constants parsed from `MagneticField.cpp`; p as in section 1. The sim's simulator
applies no higher-order term, so the sim's p is a computed quantity with nothing behind it (the null).

**T1, the line of sight.** The ionosphere-free post-fit phase residual 2.5457 v(L1) - 1.5457 v(L2W)
(v = y - A x, obs minus model), over station-satellite arcs of at least 40 epochs. The estimator
absorbs a station-common part each epoch (receiver clock), a satellite-common part (satellite clock)
and a constant per arc (ambiguity), so p is put in the same form: double-centred per epoch (station
and satellite means removed, iterated), then de-meaned per arc. Pooled regression of the residual's
20-epoch block means on p's, both arc de-meaned, with arc-clustered errors. Placebo: p of a different
satellite of the same station over the same epochs (same ionosphere and region, wrong geometry).
Secondary (the outside review's form): across arcs, the rank correlation of the residual's
persistence (rms of its 20-epoch block means) with p's size (rms of its block means).

**T2, the walk share.** For each satellite's L2W block clone (card REV's trace) and lag k of 5, 20,
60 and 120 epochs: the realized change of the walk share Phi_D, against the change of p averaged over
the lines of sight present at both ends. Card FWS section 2.1 fixes the sign: the walk step follows
the satellite-mean residual with r -0.785, so an unmodelled delay e moves D by -g e (0 < g <= 1).
Both series per-epoch de-commoned across satellites.

## 3. Predictions, written before any number was computed

If the second-order term is the leak:

- **S1 (T1, sign and size).** On the field primary, the pooled slope of the residual on the
  absorbed-form p is between +0.3 and +1.5 with a clustered t of at least 5; the placebo's slope is
  within 0.1 of zero; the sim's within 0.1 of zero.
- **S2 (T2, the walk share at long lags).** On the field primary, the regression of the walk share's
  change on the satellite-mean change of p has a slope between -1.2 and -0.2 at 60 and 120 epochs,
  with r at most -0.2; the sim's |r| below 0.05.
- **S3 (the scale).** At 5 epochs, p explains less than 10 % of the walk share's change variance on
  the field (r^2 < 0.1) whatever S2 finds: the term is too slow for the short-scale colour.
- **S4 (the unattributed remainder).** The term explains the long-scale remainder card FWS left if,
  on the field primary, r^2 at 120 epochs is at least 0.25.

The secondary rank correlation is reported but decides nothing: the term's size, the colour and the
daylight tropics share a region, so a positive rank correlation is confounded; the signed T1 is the
test.

**What follows.** S1 and S2 hold: the term is a source, and its self-contained correction (the
estimator's own slant ionosphere, the dipole or a full geomagnetic synthesis, both static) becomes a
build card on the network path, sized by S4. S1 holds and S2 fails: the term is in the residuals and
the estimator absorbs it elsewhere; it is not the walk share's source. Both fail: the outside
review's fallback, a per-line-of-sight ionosphere-free Gauss-Markov nuisance state in the station
blocks separated from the ambiguity by its time constant, is registry work (card PRM) and the
author's call.

## 4. The test (2026-10-01)

`scripts/clock_level/soi_test.py` on the three 6 h runs (about 340 000 lines of sight each, 1 111 to
1 126 arcs of at least 40 epochs):

| | field primary (`fws_wuacc6_f`) | field control (`rev1_blk6_f`) | sim (`rev1_blk6_s`, the null) |
| --- | --- | --- | --- |
| \|B.k\| p50 / p90 | 20.6 / 36.4 uT | 20.6 / 36.4 uT | 17.7 / 33.5 uT |
| slant TEC p10 / p50 / p90 | 11 / 40 / 110 TECU | 11 / 41 / 109 TECU | 25 / 55 / 127 TECU |
| \|p\| p50 / p90 | 1.40 / 4.72 mm | 1.39 / 4.73 mm | 1.86 / 4.39 mm |
| rms of p in the absorbed form; its 20-epoch block means | 0.68; 0.67 mm | 0.67; 0.66 mm | 0.62; 0.61 mm |
| rms of the residual's 20-epoch block means | 4.09 mm | 4.64 mm | 4.77 mm |
| T1 slope (clustered se, t), r | -0.017 (0.077, -0.2), -0.003 | +0.027 (0.078, +0.3), +0.004 | +0.028 (0.071, +0.4), +0.004 |
| T1 placebo slope (se), r | +0.092 (0.091), +0.012 | -0.107 (0.133), -0.012 | -0.051 (0.073), -0.005 |
| T1 epoch-level slope (se) | -0.010 (0.077) | +0.049 (0.079) | +0.061 (0.069) |
| arcs with a positive / negative within-arc slope (p block rms at least 0.3 mm) | 290 / 257 | 273 / 267 | 295 / 282 |
| rank correlation, residual persistence with p's size | +0.221 | +0.223 | +0.169 |
| T2 r at lag 5 / 20 / 60 / 120 | +0.008 / -0.005 / -0.017 / -0.015 | +0.005 / -0.008 / -0.052 / -0.123 | -0.007 / -0.002 / -0.004 / -0.017 |
| T2 slope at lag 60 / 120 (se) | -0.19 (0.68) / -0.13 (0.96) | -0.82 (0.86) / -1.38 (1.27) | -0.03 (0.37) / -0.08 (0.42) |
| T2 rms of the satellite-mean change of p / of the walk share, lag 60 | 0.56 / 6.27 mm | 0.55 / 8.62 mm | 0.54 / 4.44 mm |
| the same, lag 120 | 1.05 / 8.83 mm | 1.02 / 11.39 mm | 1.06 / 5.08 mm |

**S1 fails**: the slope is -0.02 with a standard error of 0.08, where the term at full size would
read +1 at a t of about 13; the term is absent from the residuals at the level of a fifth of its
predicted size, and the placebo and the null read the same. **S2 fails**: r is -0.02 at 60 and 120
epochs on the primary (r^2 0.000); the control's -0.12 at 120 epochs sits within its own standard
error (slope -1.4 +/- 1.3). **S3 holds**, trivially. **S4 fails** (r^2 0.000 against 0.25). The
secondary rank correlation is +0.22 on the field and +0.17 on the sim, which has no such term: the
confound section 3 named (larger residuals and larger slant TEC share low elevations and the
tropics), not the term.

## 5. Reading (2026-10-01)

**The second-order ionospheric term is not the field stations' leak.** It is too small to be: in the
ionosphere-free combination it is 1.4 mm at the median line of sight on this day (4.7 mm at p90),
and the part the estimator cannot absorb into clocks and ambiguities is 0.7 mm rms, a sixth of the
residuals' slow part (4.1 mm); averaged over a satellite's stations its change over two hours is
1 mm against the walk share's 8.8 mm. The data agree: no slope at the residual level and no
correlation with the walk share at any lag, on either field run. Its time scale also rules it out
for the short-scale colour, as section 1 expected. The term is real physics at the millimetre level
and a consistency item for the observation model, not a lever on this card's question.

**The sign question of section 1 stays open.** The test could not see the term, so it does not
adjudicate the engine's convention (the receiver-to-satellite vector with a positive code
coefficient, the opposite of the derivation here); the term is off on every posture.

**What follows.** Section 3's third branch: the outside review's fallback, a per-line-of-sight
ionosphere-free Gauss-Markov nuisance state in the station blocks, separated from the ambiguity by
its time constant (card FWS's colour decorrelates within about 10 minutes; the sim arm that
reproduced it used 15 minutes), is registry work (card PRM) and the author's call. The long-scale
remainder card FWS left is not this term; its two remaining candidates (station-set changes as
satellites rise and set, and daytime tropospheric variability at the tropical stations) are the
next bisection.

## 6. Annotation (2026-10-01, card I2S)

The sign question section 5 left open is adjudicated in card
[I2S](I2S_second_order_iono_sign_2026-10-01.md): section 1's derivation is correct and the engine's
convention was inverted. The engine projected B on the receiver-to-satellite line of sight with a
positive code coefficient, and POINT carries the same inversion with nothing in its application to
compensate it. A first-principles eigenmode solve, the literature read with its own direction
conventions, a handedness anchor from the field-validated wind-up model, and Ginan and PRIDE PPP-AR
all give the code term +7527 c STEC (B.k) / f^3 with k from satellite to receiver. The engine and its
oracle were fixed together. This card's prediction already used the propagation direction
(`scripts/clock_level/soi_test.py`), so sections 4 and 5 stand unchanged.
