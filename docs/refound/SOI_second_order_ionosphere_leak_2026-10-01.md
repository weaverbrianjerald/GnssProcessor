# SOI: is the second-order ionospheric term what leaks from the field stations into the satellites' phase-visible clocks?

**STATUS**

- **id:** SOI
- **topic:** network-vehicle
- **status:** open
- **summary:** PRE-REGISTERED 2026-10-01 (the outside review's note after card FWS). The leak card FWS found is per line of sight and the same on both frequencies in metres; the second-order ionospheric term fits that signature (the ionosphere-free combination keeps a 1/f^3 residual, it changes sign with the line of sight's angle to the geomagnetic field, it is largest at low latitudes in daylight). A truth-free test on existing traces, no build: the per-line-of-sight term predicted from the estimator's own slant ionosphere and the dipole field of `MagneticField.cpp`, against the signed post-fit ionosphere-free phase residuals (T1) and against the per-satellite walk share's changes (T2), on the field with the sim as the null.
- **source:** [FWS](FWS_field_walk_share_sources_2026-10-01.md) sections 2.1, 4 and 5; [PAC](PAC_common_drift_pivot_clock_2026-10-01.md); `include/atmosphere/HigherOrderIonosphere.hpp`, `src/core/atmosphere/MagneticField.cpp` (the dipole); `src/core/filter/NetworkIonoDiag.cpp` (the slant ionosphere trace, `iono_total_m` on L1 in metres); `src/core/filter/NetworkFrameDiag.cpp` (the post-fit rows, signed v = y - A x)
- **knobs / posture:** none; traces only
- **dates in the text:** 2026-10-01
- **hashes / run dirs:** traces of `runs/_rap_fws_wuacc6_f` (field 6 h, accumulated wind-up, the primary), `runs/_rap_rev1_blk6_f` (field 6 h control), `runs/_rap_rev1_blk6_s` (sim 6 h, the null); script `scripts/clock_level/soi_test.py`

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
