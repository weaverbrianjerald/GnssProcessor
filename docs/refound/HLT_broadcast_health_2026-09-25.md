# HLT: broadcast satellite health on the network path

**STATUS**

- **id:** HLT
- **topic:** network-vehicle
- **status:** open
- **summary:** BUILT 2026-09-25, NOT ARMED: neither health mode is consistently better at 2 h, because any satellite-set or pivot change re-draws the ignition-frozen clock levels; the mode decision is the author's.
- **source:** the author's direction of 2026-09-25 (the network path checks the broadcast health flag); [OWD](OWD_owed_defects_2026-09-22.md) section 4 (health on the network path) and
  section 1 (O3, the latch's healthy records); [CLV](CLV_clock_level_2026-09-22.md) section 8
  (the pivot as the clock level's reference)
- **knobs / posture:** new `network.sat_health` (string `off` | `pivot` | `exclude`, default
  `off`, byte-identical; solve-affecting and fingerprinted `hlt:<mode>`); no golden, ledger row
  or loader default changed
- **dates in the text:** 2026-09-25
- **hashes / run dirs:** code in the session worktree on main `335f640` beside card CLV's
  uncommitted return plan (default OFF; both landed on main later in `a104614`, 2026-09-28); exe `gnss_processor_hlt.exe` sha256
  `52d9f74ddce0895535468492bc26356fdf36d7eb1dfaba4957ef9ca2d8ea4aad`; arms
  `runs/_rap_hlt_{f,s,fg,sg}_{g0,pv,ex}` (2 h); bases `runs/_rf0hv10_{field,sim}6`,
  `runs/_rap_gal_g5s_field6`, `runs/_rap_gal_g2b_sim6`; census keys `hltpv`, `hltex`
  (`runs/_rap_msc_<site>_hlt{pv,ex}_d`), paired against `rp1g0`

**Outcome.** The question: whether the network should honour the broadcast health flag, set all day on DOY 129 for G01 (the GPS clock pivot), E14 and E18, which CODE carries at full rate. H0 and H1 PASS. The pre-registered rule selects "exclude" ("pivot" fails two bars), but "exclude" costs the GPS+Galileo sim 0.056 m in hour 2; NOT ARMED, sequenced behind card CLV, a 6 h pair owed for "pivot". Card [PAC](PAC_common_drift_pivot_clock_2026-10-01.md) later named the pivot mode a clock-product lever; card [SCL](SCL_self_contained_clock_level_2026-10-03.md) section 3.7 found the pivot does not decide the field's locked level.

## 1. The survey (measured before the build)

Both beds read the day's merged navigation file
`data/2024-129/products/BRDC00IGS_R_20241290000_01D_MN.rnx` (11 498 records). Thirteen satellites
broadcast no healthy record all day (card OWD, O3); three of them are processed by the network:

| satellite | records | health word | processed as |
| --- | --- | --- | --- |
| G01 | 14 | 63 in every record (T_GD -19.558 ns throughout) | the GPS clock pivot until TOW 276150 (4.71 h), when its eclipse hands the pivot to G02 |
| E14 | 386 | F/NAV 16 (E5a health field 1), I/NAV 130 (E1-B and E5b health fields 1) | an ordinary Galileo satellite on the GPS+Galileo wire |
| E18 | 319 | the same as E14 | the same as E14 |

The other ten (C35, C48, C50, C56, C58, C62, J02, J03, J04, J07) belong to systems the network
does not process. No GPS or Galileo satellite changes its status within the file. The precise
a priori vouches for all three: `COD0MGXFIN_20241290000_01D_30S_CLK.CLK` holds 2 880 clock
records each for G01, E14 and E18 (the same count as G02), and the 5-minute orbit file 289
epochs of G01 and of E14.

The clock pivot is elected in `NetworkSbasis::buildSharedDatum`: a declared
`network.clock_datum_satellite` when registered, else (the goldens arm `network.sticky_pivot`)
the incumbent while registered, else the lexically first registered satellite of the
constellation. G01 wins GPS by its number alone. The pivot's clock is pinned to its a priori and
every other satellite's clock is estimated against it. The Galileo pivots of the day, E02 and
then E03 (card OWD, section 4), are healthy.

## 2. The design

**The status at an epoch.** A broadcast record takes force at its clock reference epoch, the
network's availability rule for a record (`navAvailabilityGpsSeconds`, shared with the broadcast
staging and the O3 latch). A satellite's records form one group per message source (Galileo
I/NAV and F/NAV; one group for every other system), because the Galileo sources carry different
signals' health (I/NAV the E1-B and E5b fields, F/NAV the E5a field). At time t each group reads
its latest record at or before t, else its earliest record, and the satellite is unhealthy when
any group's record is. A record is unhealthy when its health word is nonzero; a Galileo record
when the word masked to the processed bands is (band 1: bits 0 to 2, band 5: bits 3 to 5,
band 7: bits 6 to 8, band 8: both E5 fields; every bit when no Galileo band is named). GLONASS
records are skipped. The result is a step function per satellite
(`NetworkSbasis::satHealthSeries`), built once at setup.

**"pivot".** The election passes over a registered satellite that is unhealthy at the epoch:
the sticky incumbent is kept only when healthy, otherwise the first healthy satellite in
lexical order is elected; with none healthy the lexical first stands (counted). A declared
`network.clock_datum_satellite` is kept even when unhealthy (an explicit choice; the setup warns).
The unhealthy satellite's observations are processed as before. The pick is a pure function
(`NetworkSbasis::electClockPivot`), and with no ineligible set it is the rule as it was.

**"exclude".** Right after the multi-station sync, beside card OWD's constellation door, the
observations of a satellite that is unhealthy at the epoch leave the synced epochs
(`MultiRinexSync::removeObservationsIf`), so every reader downstream sees the satellite absent
at that epoch; it is neither estimated nor on the wire.

**The edges (standing invariant #7).** A status change inside a run is an edge. Under
"exclude" the satellite vanishes from every station at once, as at a setting, and its return is
the re-admission edge the network already answers (`network.readmit_seed_horizon_s`, the
clock and code-bias seeds after an eclipse); under "pivot" a change of the elected satellite is
a clock-pivot handover, answered as the eclipse handover is (the S-transform and the declared
pivot on the S line). Triage (invariant #3): on DOY 129 there are 0 status changes of a GPS or
Galileo satellite inside any window, so both edges are field-inert on this bed, which cannot
test them; the unit tests exercise the series across a change.

**Telemetry (invariant #4).** At setup: the satellites flagged inside the window with their
flagged spans and the count of status changes inside it; under "exclude" the observations the
door removed per satellite. At run end: the (satellite, epoch) count of registered clock
satellites flagged unhealthy, the (constellation, epoch) count of picks that passed over one,
the count with none healthy, and an INERT warning when an armed mode never acted.

## 3. The gates (rapid mode: 2 h arms on the four beds of card CLV's batch 1)

The four beds and their bases: GPS field `f` (`runs/_rf0hv10_field6`), GPS sim `s`
(`runs/_rf0hv10_sim6`), GPS+Galileo field `fg` (`runs/_rap_gal_g5s_field6`), GPS+Galileo sim
`sg` (`runs/_rap_gal_g2b_sim6`). Per bed three arms on the same executable: `g0` (no change),
`pv` (`network.sat_health: pivot`), `ex` (`network.sat_health: exclude`). The bars were written
while the arms ran, before any was graded.

| gate | arms | bar |
| --- | --- | --- |
| H0 | `hlt_f_g0`, `hlt_s_g0` against `_rap_clvrp1_f_g0`, `_rap_clvrp1_s_g0` (card CLV's build) | product records, `network_epoch_status.csv` and `closed_loop_trace.csv` identical over the window (the refactored election is the old rule); the 12 new unit tests and the full Release suite pass |
| H1 | the `pv` and `ex` arms | "pivot": at least one pick passed over G01, none without a healthy satellite, the S line's GPS pivot G02, the Galileo pivot unchanged; "exclude": the door's count equals G01's observations in the window (field 2 136), no record of G01 (and on `fg`, `sg` of E14 and E18) on the wire, 0 flagged registered satellites |
| H2 | each mode against its bed's `g0` | the consumer hourly p50 (WTZZ00DEU on the field beds, the sim consumer on the sim beds) and the sim datum-free clock-level error at epoch 240 (card CLV's `clklevel.py`); on `f`, the 16-site census paired against key `rp1g0` (the replay of `_rap_clvrp1_f_g0`, identical to `hlt_f_g0` under H0), split by latched band-1 code |

A mode is proposed to the author for the working bases when, on both GPS beds, it is not worse:
the census's median paired change of the hour-2 p50 at most +0.005 m and of the hour-1 p50 at
most +0.02 m, the sim consumer's hour-2 p50 at most +0.005 m above `g0`, and the sim datum-free
error at epoch 240 not larger; with the GPS+Galileo beds reported beside them. Adoption is then
confirmed on a 6 h pair, the only window that holds G01's eclipse handover at 4.71 h, which
"pivot" removes on this day.

## 4. Result (2026-09-25, 2 h arms)

Every number is read from the run directory named (`runs/_rap_hlt_<bed>_<arm>`). Graders
(read-only, session scratchpad): `clvrp_grade.py` (the consumer by hour, the clock-anchor
decouples, and per bed card CLV's `clklevel.py --datumfree`, `clv_df_sys.py`, `gal_grade.py
--sections isbtruth` and the post-fit IF(C1C, C2W) code spread), `ident2h.py`, `census2h.py`
with `census_pooled.py` (the paired change of each site's hourly p50, pooled over the 16
sites), and the CODE comparison of card CLV section 8.3.

**4.1 H0: PASS.** `hlt_f_g0` and `hlt_s_g0` are identical to `_rap_clvrp1_f_g0` and
`_rap_clvrp1_s_g0` over the window (product records, `closed_loop_trace.csv`,
`network_epoch_status.csv`). The full Release suite runs 2 655 tests from 334 suites: 2 638 pass
and 17 skip (the fixture-gated ones; `data/` is empty in the worktree). The 12 new tests are in
`tests/test_sat_health.cpp` (suites `SatHealth`, `ClockPivotElection`, `SatHealthDoor`).

**4.2 H1: PASS.** At setup every arm lists 13 satellites flagged for the whole window (G01, E14,
E18 and the six BeiDou and four QZSS satellites the network does not process) and 0 status
changes. "pivot": the election passed over an unhealthy satellite at one (constellation, epoch)
per bed, the first epoch (the sticky rule keeps G02 from then on), with 0 fallbacks; the S line
declares `G:G02` with `E:E02` unchanged; the flagged registered (satellite, epoch) count is 240 on
the GPS beds (G01) and 720 on the GPS+Galileo beds (G01, E14, E18). "exclude": the door removed,
inside the window, G01 2 136 (field) and 4 217 (sim) observations, and on the GPS+Galileo beds
E14 5 495 and E18 2 103 (field), 4 227 and 4 438 (sim); no C record of a flagged satellite is on
the wire; 0 flagged registered; the S line `G:G02`. No arm has a pivot event inside 2 h. The
clock-anchor decouples fall at the same epochs in every arm of a bed (a few more satellites per
event under either mode on the sims and the GPS+Galileo field: 1 to 3, 1 to 2, 5 to 8).

**4.3 H2.** Consumer 3D p50 by hour (m) and epochs below 0.1 m of 240; the census as the pooled
median paired change (better / worse sites, 0.05 m in hour 1 and 0.01 m in hour 2):

| bed | `g0` | "pivot" | "exclude" |
| --- | --- | --- | --- |
| GPS field, WTZZ00DEU | 0.560 0.059, 131 | 0.412 0.074, 138 | 0.409 0.049, 138 |
| GPS field, census against `rp1g0` (0.670 / 0.169) | | h1 +0.027 (6 / 7), h2 -0.012 (8 / 4) | h1 -0.007 (7 / 6), h2 -0.011 (9 / 5) |
| GPS field, IF code against CODE, C1C user h1 / h2; C1W user | 0.290 / 0.293; 0.360 / 0.392 | 0.290 / 0.293; 0.361 / 0.395 | 0.266 / 0.269; 0.378 / 0.461 |
| GPS field, post-fit IF(C1C, C2W) spread | 0.19 / 0.27 | 0.19 / 0.27 | 0.16 / 0.23 |
| GPS sim, consumer | 0.598 0.103, 53 | 0.430 0.071, 94 | 0.638 0.094, 82 |
| GPS sim, datum-free level at epoch 240 | 1.15 | 1.21 | 0.93 |
| GPS+Galileo field, WTZZ00DEU | 0.197 0.063, 143 | 0.177 0.026, 172 | 0.307 0.065, 164 |
| GPS+Galileo sim, consumer | 0.514 0.060, 115 | 0.835 0.078, 84 | 0.960 0.116, 31 |
| GPS+Galileo sim, datum-free GPS / Galileo at epoch 240 | 1.130 / 0.951 | 1.257 / 0.927 | 0.981 / 0.908 |
| GPS+Galileo sim, consumer inter-system bias minus the prediction, hour 2 | +0.084 | +0.235 | -0.078 |

By the pre-registered rule "exclude" passes every GPS bar (census hour 1 -0.007 and hour 2
-0.011 m, sim hour 2 0.094 against 0.103 m, datum-free 0.93 against 1.15 m) and "pivot" fails
two (census hour 1 +0.027 against the +0.02 m bar, datum-free 1.21 against 1.15 m).

**4.4 Reading: the 2 h beds cannot resolve the two modes while card CLV's root is open.** A
change of the satellite set or of the pivot re-draws the clock levels frozen at ignition (card
CLV). On the GPS sim, with G01 left out of the comparison, the other satellites' levels at
epoch 240 move by 1.16 m rms under "pivot" and 1.22 m under "exclude" against `g0` (correlation
0.74 and 0.57), the size of the levels themselves (1.47 m rms; this read takes the wire's C
record at the epoch with the median over the GPS satellites present removed, where
`clklevel.py --datumfree` reads 1.15 m on `g0` by its own definition): G16 is
-3.91 m on `g0`, -3.67 m under "pivot" and inside 1.6 m under "exclude"; G02 goes from -0.52 to
-2.14 m under "pivot". The consumer's hour 2 then changes sign between beds for both modes:
"pivot" is better on the GPS sim, the GPS+Galileo field and the census, worse at WTZZ00DEU on the
GPS field and on the GPS+Galileo sim; "exclude" is better on the GPS field and the census, worse
on the GPS+Galileo sim (0.060 to 0.116 m, 115 to 31 epochs below a decimetre). The datum-free
bar passes or fails on the draw. Two readings are not draws: "pivot" leaves the wire's code
level against CODE unchanged (0.290 / 0.293 and 0.361 / 0.395 m), as a datum choice must, and
"exclude" lowers a C1C user's error (0.29 to 0.27 m) while raising a C1W user's (0.39 to 0.46 m
in hour 2).

**4.5 G01's data are usable on this day.** CODE's final products carry it at full rate. Its
field post-fit IF(C1C, C2W) code residual is among the largest (-0.58 / -0.72 m, rank 1 to 2 of
32), matched by a healthy satellite (G07, -0.59 / -0.71 m); its code level against CODE (+0.49 m
for a C1C user) ranks third, after G07 (+0.80 m) and G09 (-0.51 m); its phase residual rms is
2.0 mm (rank 6 of 32, median 1.2 mm).

**4.6 Verdict: BUILT, NOT ARMED; the mode decision is the author's and is sequenced behind card
CLV.** The pre-registered rule selects "exclude". Arming it on this evidence is not advised:
the GPS+Galileo sim reported beside the rule loses 0.056 m in hour 2 and 84 epochs below a
decimetre, every 2 h difference measured here is of the size of the level re-draw, and on this
day the precise products vouch for the satellites the flag would remove. Once card CLV's root
is fixed, the levels stop re-drawing and the 2 h beds can separate the modes; "pivot" then needs
its 6 h pair (G01's eclipse handover at 4.71 h is its one mechanism on this day). The setup's
flagged list names satellites of every system in the navigation file; restricting it to the
processed systems is a cosmetic follow-up.
