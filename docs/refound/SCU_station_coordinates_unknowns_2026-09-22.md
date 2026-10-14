# SCU: station coordinates, held or estimated, and the roster audit

**STATUS**

- **id:** SCU
- **topic:** network-vehicle
- **status:** parked
- **summary:** PARKED 2026-09-22 (the author's decision): MIZU00JPN, held 1.6 m from its data, is DROPPED from the field roster; constrained coordinate states are deferred until the network works.
- **source:** the author's question of 2026-09-22; [UPO](UPO_consumer_up_offset_2026-09-22.md) sections 5.7 and 6 (the site-specific offsets left open, the congruence table)
- **knobs / posture:** none built. Proposed: `network.station_position_estimate`, `network.station_position_sigma_m`, `network.station_position_free`, `network.station_position_release_t` (section 6). Posture: the RF-0hv working bases as armed on 2026-09-22 (baseline pair `runs/_rf0hv7_{field,sim}6`; after the drop, field `runs/_rf0hv8_field6`, 58 stations in `config/refound/field_gps_dual_rf0hv.json`)
- **dates in the text:** 2026-09-22
- **hashes / run dirs:** `runs/_rap_u4_s10` (identical to `runs/_rf0hv7_field6`), `runs/_rap_m1_drop_mizu`, `runs/_rap_m2_mizu_moved`, `runs/_rap_m3_drop_maju_lck4`, `runs/_rap_m4_drop_solo_ulab` (leave-out keys `lo3`, `lo4`), roster replays `runs/_rap_ros_<station>_<key>` with keys `ros6`, `ros7u`, `ros7`, `rosm1`, `rosm2`, census key `m2`; exe `gnss_processor_upo.exe` (`E2E177AB...`); instrument `runs/_msc/scripts/msc_roster_audit.py`. The drop: `runs/_rf0hv8_field6`, exe `gnss_processor_rf7.exe` (`97547306...`), census key `rf8`; 12 h `runs/_rap_a8_field12` against `runs/_rap_a7_field12`

**Outcome.** Question: estimate the station coordinates or hold them? A roster audit (each station replayed as a consumer on the network's own wire) found MIZU00JPN's held coordinate off its data; the drop takes the all-satellite clock wire from 0.0068 to 0.0045 m. The station-congruence screen had flagged it in every run, unread: it detects and cannot measure, and a flag is not always a coordinate. All-free coordinates are nearly unestimable next to free satellite clocks; the answer, a constrained state with a release, is not started. Owed items: [OPEN_ITEMS](../status/OPEN_ITEMS.md).

## 1. The question

The network holds every station at a configured coordinate. Card UPO showed what a wrong
held coordinate does: the station's own wet delay takes about a quarter of a height
error and the satellite clocks take the rest, regionally. The question: would coordinate
unknowns, initialized within centimetres or metres, be better than forcing the network
to absorb fixed reference position errors?

## 2. The roster audit (new instrument, no build)

`runs/_msc/scripts/msc_roster_audit.py` replays each roster station as a closed-loop
consumer on a wire and reports the mean E / N / U of the free position over hours 4 to 6
against the configured coordinate reduced to the antenna reference point (the consumer's
own truth path). The replay is leave-in (the station's data helped form the wire with
its coordinate held), so a coordinate error is under-reported by the station's own share
of the wire: 7 percent at MIZU00JPN (section 3). It costs 14 s per station and 3 minutes
for the roster. It is a truth-free estimate of what a free coordinate state at that
station would converge to.

On the armed base (key `ros7`, wire of `runs/_rap_u4_s10`), 54 of 59 replays converge
with every mean component below 1 m:

| | value |
| --- | --- |
| median absolute mean Up | 0.029 m |
| median horizontal offset | 0.031 m |
| absolute mean Up above 0.05 m / above 0.10 m | 14 / 7 stations |
| horizontal offset above 0.05 m | 15 stations |
| central Europe (11 stations) | coherent: East +0.009 to +0.029 m, Up -0.011 to -0.037 m |
| not converged | BSHM00ISR (gross code rows), SGOC00LKA, KIT300UZB (0.05 m in hour 3, leaves from hour 4), VOIM00MDG (an excursion of tens of metres): the consumer's unbounded reaction to bad data, not coordinates |
| stable and large | MIZU00JPN: E +1.363, N -0.588, U -0.121 m, scatter 0.065 m in Up |

The offsets of 0.05 to 0.2 m at remote stations (KOKV00USA, GUAM00GUM, WIND00NAM,
LCK400IND, DARW00AUS, MAJU00MHL, ULAB00MNG) wander by a decimetre from hour to hour:
slow convergence on a sparse wire, not yet evidence of a coordinate error. One station
is different in kind.

## 3. MIZU00JPN: held 1.6 m from its data

- Leave-out estimate (MIZU00JPN removed from the roster, `runs/_rap_m1_drop_mizu`, then
  replayed on that wire): E +1.746 +1.485 +1.526 +1.497 +1.468 +1.450 m by hour, N -0.435
  -0.710 -0.679 -0.668 -0.636 -0.605 m, U -0.134 +0.073 -0.046 -0.121 -0.092 -0.148 m;
  hours 4 to 6: **E +1.4715, N -0.6366, U -0.1204 m**; scatter in hour 6: 0.011 / 0.012 /
  0.027 m.
- The roster value equals the output of `scripts/resolve_itrf_ecef.py` to the digit
  (-3857169.9416, 3108693.7043, 4004040.6697: the twelfth ITRF2020 solution of the
  station, from 2020:111, propagated linearly to 2024.35). The parser behind it
  (`pyviz/gnssviz/eval/truth.py`, `SinexTruth`) selects the solution and applies position
  plus velocity; it applies no post-seismic deformation. MIZU00JPN lies in the deformation
  field of the 2011 Tohoku earthquake, and ITRF2020 publishes the post-seismic models of
  such stations separately from the SSC file. Size and direction (1.6 m towards ESE,
  thirteen years after the event) fit that reading; the model file is not in the tree, so
  the attribution is not verified numerically.
- The same resolver produced the truth coordinates of the census sites. A census site
  with a post-seismic model has a wrong truth. Unread (ANMG00MYS is the first candidate).

### 3.1 What the error costs (two config-only arms on the armed base, 6 h)

| arm | clock wire against the reference clocks, all / consumer-tracked (m) | WTZZ00DEU 3D p50 by hour (m) | congruence statistic at MIZU00JPN |
| --- | --- | --- | --- |
| armed base (`runs/_rap_u4_s10`) | 0.0068 / 0.0033 | 0.55 0.06 0.03 0.03 0.03 0.03 | 29.1 |
| MIZU00JPN dropped (`m1_drop_mizu`) | 0.0045 / 0.0028 | 0.56 0.06 0.03 0.03 0.04 0.04 | (absent) |
| MIZU00JPN moved by the leave-out estimate (`m2_mizu_moved`) | **0.0044 / 0.0028** | 0.55 0.06 0.03 0.03 0.03 0.03 | 1.43 |

One station of 59 with a wrong coordinate carried a third of the all-satellite clock
error. The clock wire is graded against reference clocks that do not depend on the
roster, so this is an independent confirmation that the moved coordinate is the right
one. Moved, the station's own replay lands at E +0.015, N -0.001, U -0.009 m.

Census of 16 independent sites, key `m2` against `upos10`: the absolute mean Up over
hours 4 to 6 is smaller at 13 (median change -0.0061 m, median 0.042 to 0.027 m); the 3D
p50 over hours 4 to 6 is better at 11 (median -0.0043 m); median p50 by hour 0.677 0.235
0.118 0.095 0.097 0.078 to 0.671 0.189 0.111 0.083 0.094 0.069 m. KOKB00USA 0.189 to
0.124 m (mean Up -0.192 to -0.086 m: half of that site's offset was this station),
QUIN00USA 0.080 to 0.022 m, UTQI00USA 0.232 to 0.157 m, BAKE00CAN 0.056 to 0.033 m. Worse:
BADG00RUS 0.129 to 0.215 m, POHN00FSM 0.112 to 0.174 m, ANMG00MYS 0.116 to 0.132 m;
WUH200CHN unchanged (0.173 to 0.183 m).

Roster replays, key `rosm2` against `ros7`: 3D p50 better at 33 of 54 (median
-0.0018 m), absolute mean Up smaller at 25 (median +0.0008 m), median horizontal offset
0.031 to 0.024 m. The Pacific rim improves (GUAM00GUM 0.224 to 0.130 m, KOKV00USA 0.172
to 0.115 m, SOLO00SLB 0.127 to 0.097 m, SVTL00RUS 0.047 to 0.021 m) and central Asia gets
worse (URUM00CHN 0.083 to 0.130 m, ULAB00MNG 0.142 to 0.195 m, LCK400IND 0.145 to
0.177 m, CUSV00THA 0.063 to 0.079 m), together with BADG00RUS and POHN00FSM in the
census: a second disturbance in that region that this error partly masked. Unread.

## 4. The detector that was already armed

The station-congruence screen (`docs/SPEC_redundancy_census.md`; measure-only, in every
run of this program) tests each station's post-fit rows for a coherent coordinate error
and prints a run-end table to `stdout.txt`. Run-median statistic geo(t); the roster
median is 0.87 to 0.91 in all four runs:

| run | GRAZ (H 1.964) | KRGG (1.033) | GUAM (0.623) | MIZU | MAJU | LCK4 | ULAB | SOLO |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| roster at the monument (`runs/_rf0hv6_field6`) | 26.10 | 8.99 | 3.87 | 21.60 | 6.97 | 2.83 | 3.76 | 2.70 |
| roster at the antenna (`runs/_rap_u1_field_on`) | 0.53 | 0.68 | 2.54 | 28.40 | 8.68 | 4.78 | 4.06 | 3.25 |
| plus the second code channel (`runs/_rf0hv7_field6`) | 0.53 | 0.74 | 2.36 | 29.10 | 7.94 | 4.92 | 3.95 | 3.22 |
| MIZU00JPN moved (`runs/_rap_m2_mizu_moved`) | 0.56 | 0.75 | 1.82 | 1.43 | 7.27 | 5.40 | 3.44 | 4.06 |

- As a DETECTOR it is clean: it ranked the eccentricities of card UPO by size, it clears
  when the coordinate is corrected, and it had MIZU00JPN at 22 to 29 in every run. Nothing
  read the table. Still above 4 after both corrections: MAJU00MHL, LCK400IND, SOLO00SLB
  (ULAB00MNG at 3.4 to 4.1): the next suspects, unread. ITRF2020 ends at 2021.0, so an
  equipment change or an earthquake after that date is in no solution of the SSC file.
- As an ESTIMATOR it fails by construction on this vehicle: its coordinate estimate at
  MIZU00JPN is E -0.002, N -0.031, U +0.044 m against a true 1.6 m, and -0.100 m Up at
  GRAZ00AUT against 1.964 m. The carried float ambiguities absorb the constant part of a
  coordinate error at birth; the post-fit rows keep only its change with the geometry.
  Measuring the error needs a coordinate state that is estimated together with the
  ambiguities (what the replayed consumer does), not a test on residuals.

### 4.1 A flag is not always a coordinate error (leave-out replays of the four flagged stations)

Two arms on the base with MIZU00JPN moved, each dropping two flagged stations
(`runs/_rap_m3_drop_maju_lck4`, `runs/_rap_m4_drop_solo_ulab`), then the dropped stations
replayed on the wire formed without them (`runs/_msc/scripts/msc_leaveout.py`, keys `lo3`,
`lo4`). Mean over hours 4 to 6 (E, N, U in m), and Up by hour:

| station | statistic | E | N | U | Up in hours 4, 5, 6 |
| --- | --- | --- | --- | --- | --- |
| MAJU00MHL | 7.3 to 8.7 | +0.013 | -0.011 | -0.040 | -0.081 -0.065 +0.026 |
| LCK400IND | 4.8 to 5.4 | +0.031 | -0.034 | -0.169 | -0.200 -0.155 -0.151 |
| SOLO00SLB | 3.2 to 4.1 | -0.078 | -0.046 | +0.084 | +0.179 +0.092 -0.019 |
| ULAB00MNG | 3.4 to 4.1 | +0.051 | +0.050 | -0.146 | -0.142 -0.201 -0.094 |

- MAJU00MHL carries the second largest statistic of the roster and no coordinate error at
  the level of 0.04 m: its flag is something else at that station (unread).
- LCK400IND and ULAB00MNG read -0.15 to -0.17 m in Up, and so do the INDEPENDENT census
  sites of the same region on the wire these two arms derive from (key `m2`: WUH200CHN
  -0.181 m, BADG00RUS -0.167 m; URUM00CHN -0.125 m among the roster replays `rosm2`). That is a regional bias of the wire over
  central and east Asia, not the coordinates of those two stations. Candidates, unread:
  the gross code rows held at full weight at SGOC00LKA and KIT300UZB (the two stations of
  that region whose own replays leave by metres), a held height in the region.
- Consequence for section 6: a released station takes up a regional bias as if it were
  its coordinate and stops constraining the region, which is a loss where the roster is
  thin. The static release list is safe; the automatic release needs a discriminator
  between a station's own error (MIZU00JPN: no neighbour shares it) and a shared one.

## 5. Held, free, or constrained

- **All free is not estimable next to free satellite clocks.** With orbits held and
  clocks free per epoch, a common translation d of all stations changes a range by
  -e.d = -(x_sat - x_sta).d / rho. For constant rho the first part is a satellite term
  and the second a receiver term, both absorbed by the clocks; what stays observable is
  the variation of 1 / rho over the sky. Checked numerically at one epoch (this roster's 59
  positions, a 30-satellite constellation, 7 degree mask, 569 rows): next to free satellite
  and receiver clocks 0.054 of the norm of a common translation's partials survives, 0.043
  with the wet delays free as well, against 0.32 to 0.93 (0.32 to 0.81) for ONE station
  freed while the others are held. A common height change trades with clocks and wet delays
  in the same way. This is the known collinearity
  of the GNSS geocentre (Rebischung, Altamimi and Springer 2014, Journal of Geodesy 88).
  A roster with every coordinate free floats by centimetres to decimetres, and every
  user inherits the float one to one. Tight priors on most stations ARE the datum: the
  frame becomes the weighted mean of the priors, and no rank defect is added as long as
  every station keeps a finite prior.
- **The held positions are the network's strength.** They are what separates clocks,
  wet delays and float ambiguities quickly. A prior of 0.01 to 0.02 m keeps that strength.
  On the float vehicle the data does not beat such a prior within hours (the free consumer
  at WTZZ00DEU reaches 0.01 m in hour 3), so a station with a good coordinate is left where
  it is, which is the correct outcome, and the uncertainty of its coordinate enters the
  stochastic model instead of being asserted as zero.
- **What estimation buys is the blunder and the stale coordinate.** Two classes in one
  roster (eccentricities at 48 stations, a missing post-seismic model at one), both found
  by hand. A released station converges like a consumer (MIZU00JPN:
  0.02 m scatter by hour 6) and contributes to the clocks as it converges, which is a
  smooth version of "drop it, then put it back where its data says". It needs no external
  file, which matters under the standing rule against external data dependence.
- **What it does not buy.** Sub-daily station motion is not a constant (ocean tide
  loading is OFF in this posture: a modelling item). A tight prior resists a 0.05 m error
  by design (about half is absorbed after hours); that class is for the detector and the
  release, not for the prior.
- **Practice.** Real-time clock services hold or tightly constrain their stations to
  weekly solutions; analysis centres that estimate coordinates do so in daily batches
  with minimal constraints. Constrained with a release is the form that fits a filter.

**Answer.** Yes, as constrained states with a release, not as free unknowns; holding stays
the behaviour of every station whose prior is good. The detector exists and works; the
missing half is the adaptation.

## 6. Proposed build (NOT started; frozen at the author's go-ahead)

Requirements:

- K1. `network.station_position_estimate` (default OFF): three constant states per
  roster station (local East, North, Up corrections to the configured coordinate after
  the reduction of card UPO), with the partial -e in every code and phase row of the
  station, companion code rows included. Station-local, carried like the wet delay.
- K2. Prior `network.station_position_sigma_m` (E, N, U; default 0.01, 0.01, 0.02);
  process noise zero by default. The loader refuses a posture in which fewer than three
  quarters of the stations keep the tight prior (section 5, first point).
- K3. Release, first increment: a static list `network.station_position_free` (prior
  10 m). Second increment, only with the discriminator of section 4.1: automatic release
  when the congruence statistic stays above `network.station_position_release_t` over a
  window and no neighbour shares the offset; one way (the converged estimate's
  own covariance is the constraint afterwards). Both edges are events with counters and
  identities (invariant 7); the release epoch is declared.
- K4. Telemetry in both knob states: the congruence run-end table becomes a WARNING that
  names every station above 4 (the instrument nothing read). Armed: a per-station trace
  of correction and sigma, a run-end table, the run manifest, an armed-only fingerprint.
- K5. Default OFF byte identity on both beds; consumer and wire format untouched.

Gates and bars:

- G0. Knob absent: the three graded files identical on both beds.
- G1 (sim truth). New `rapid_arm.py` op `coord` (moves a configured coordinate only).
  Errors of (E 1.5, N -0.6, U -0.1), (U 0.3), (E 0.1) and (U 0.05) m at the four stations
  nearest the sim consumer. OFF: the defect reproduces. ON with the four in the free
  list: corrections within 0.03 m horizontally and 0.05 m in Up of the injected values by
  hour 6; consumer hourly p50 from hour 3 and the clock wire against truth within 0.005 m
  and 0.0005 m of the sim baseline. ON with no free list: not worse than OFF, and the
  run-end warning names the first three.
- G2 (sim, no injection). ON: consumer within 0.005 m per hour and clock wire within
  0.0005 m of the baseline (a correct roster must cost nothing).
- G3 (field). MIZU00JPN in the free list: correction within 0.05 m of (+1.47, -0.64,
  -0.12) m by hour 6; clock wire at most 0.0046 / 0.0029 m (the scripted arm of section
  3.1); congruence statistic at MIZU00JPN below 4. With card UPO's knob OFF and GRAZ00AUT
  and KRGG00ATF in the free list: Up corrections within 0.05 m of 1.964 and 1.033 m.
- B1. WTZZ00DEU: every hourly p50 within 0.005 m of `runs/_rf0hv7_field6`.
- B2. Census and roster replays: not worse than the scripted arm (keys `m2`, `rosm2`),
  paired per site.
- B3. 12 h read against `runs/_rap_a7_field12`.

Invariants pre-flight. 1: a zero-q carried state with a wired response (the release;
the tight prior bounds what a held station can absorb). 3: deferrals triaged at the
build. 4: counters of released stations, of corrections above 3 sigma, of rows that
carry the partial. 6: default OFF, byte identity. 7: the release is an event at its one
edge; a released station's growing weight in the clocks is bounded by its own covariance.

## 7. What stays open

- The regional Up bias of -0.13 to -0.18 m over central and east Asia (section 4.1; it
  includes WUH200CHN), what MAJU00MHL's flag is, and SOLO00SLB (0.08 m East, not settled).
  The gross code rows at SGOC00LKA and KIT300UZB are not the cause: with both stations
  dropped (`runs/_rap_y3_drop_both`, census key `y3`) WUH200CHN reads -0.160 m and BADG00RUS
  -0.148 m (card GRK section 2).
- The resolver applies no post-seismic model: roster AND census truths. A stop-gap is
  available today (move MIZU00JPN by the leave-out estimate, or drop it: section 3.1);
  it puts a number derived from this network's own solution into the roster, which the
  build would make unnecessary. The author's decision.
- The four replays that leave by metres on bad data (BSHM00ISR, SGOC00LKA, KIT300UZB,
  VOIM00MDG): the consumer's unbounded reaction, card MSC.
- From card UPO section 5.7: the negative
  wet delays of CAS100ATA and SCTB00ATA, the soft hours 10 to 12, the consumer's mask.

## 8. 2026-09-22: the author's decision. MIZU00JPN dropped; the build deferred

Decision: drop MIZU00JPN from the field roster now; constrained coordinate states and
offset detection are for later, once the network works. Until then the sim has known
station positions, and the field roster is held as it is. The card is parked; the two
instruments (the station-congruence table, the roster audit) stay in use.

What the audit supports about the 58 remaining coordinates, stated at its resolution: no
station shows an error of its own. The resolution is 0.02 to 0.03 m in central Europe
(section 2) and 0.05 to 0.2 m at the remote stations, whose 6 h replays are not settled
and partly carry a regional bias (section 4.1). ITRF2020 ends at 2021.0 and the resolver
applies no post-seismic model, so a later equipment change or a post-seismic term below
that resolution is not excluded.

`config/refound/field_gps_dual_rf0hv.json`: the station's roster entry removed (58
stations) plus dated comment lines; the sim base is untouched. Confirmation arm from the
base with only the product path changed, exe `gnss_processor_rf7.exe` (`97547306...`),
exit 0: `runs/_rf0hv8_field6` is byte-identical to the scripted arm
`runs/_rap_m1_drop_mizu` on `products.txt` (lines starting with `#` removed),
`closed_loop_trace.csv` and `network_epoch_status.csv`.

**The baseline pair from this date: field `runs/_rf0hv8_field6`, sim `runs/_rf0hv7_sim6`
(unchanged).**

| | 59 stations (`runs/_rf0hv7_field6`) | 58 stations (`runs/_rf0hv8_field6`) |
| --- | --- | --- |
| WTZZ00DEU 3D p50 by hour (m) | 0.549 0.057 0.027 0.030 0.033 0.029 | 0.560 0.059 0.027 0.029 0.036 0.041 |
| mean Up by hour (m) | +0.726 +0.017 +0.004 -0.022 -0.026 -0.025 | +0.794 +0.015 -0.003 -0.023 -0.030 -0.039 |
| epochs below 0.1 m (reported, not a bar) | 610 | 611 |
| clock wire, all / consumer-tracked (m) | 0.0068 / 0.0033 | 0.0045 / 0.0028 |
| post-fit refloats / anchor events / trips | 87 / 127 / 1 | 85 / 127 / 1 |
| census median p50 by hour (m), keys `upos10` and `rf8` | 0.677 0.235 0.118 0.095 0.097 0.078 | 0.673 0.169 0.103 0.082 0.095 0.067 |
| census median count below 0.1 m | 331.5 | 339.5 |

Census `rf8` against `upos10`: the absolute mean Up over hours 4 to 6 is smaller at 12 of
16 (median change -0.0050 m, median 0.042 to 0.029 m); the 3D p50 over hours 4 to 6 is
better at 10 (median -0.0040 m). Better: QUIN00USA 0.080 to 0.019 m, KOKB00USA 0.189 to
0.124 m, UTQI00USA 0.232 to 0.166 m, BAKE00CAN 0.056 to 0.032 m. Worse: POHN00FSM 0.112
to 0.181 m (its East mean +0.030 to +0.136 m; the same on the wire with the station moved,
so it is not the loss of the station's data), BADG00RUS 0.129 to 0.176 m, ANMG00MYS 0.116
to 0.138 m. Roster replays of the identical wire: key `rosm1`.

The price of dropping instead of moving is at WTZZ00DEU: hour 6 reads 0.041 m against
0.029 m (0.030 m with the station moved, section 3.1).

12 h (`runs/_rap_a8_field12` against `runs/_rap_a7_field12`): 3D p50 by hour 0.560 0.059
0.027 0.029 0.036 0.041 0.021 0.025 0.041 0.103 0.132 0.119 m against 0.549 0.057 0.027
0.030 0.033 0.029 0.015 0.029 0.039 0.091 0.134 0.115 m (within 0.012 m at every hour);
epochs below a decimetre 1018 against 1059 (reported; the difference sits in the soft
hours 10 to 12, where the p50 is at the threshold); clock wire 0.0063 / 0.0041 m against
0.0099 / 0.0060 m; post-fit refloats 204 against 228, trips 11 against 10.
