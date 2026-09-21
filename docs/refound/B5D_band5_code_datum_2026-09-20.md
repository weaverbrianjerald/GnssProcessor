# B5D: the band-5 code datum

**STATUS**

- **id:** B5D
- **topic:** network-vehicle
- **status:** open
- **summary:** BUILT 2026-09-20, NOT ARMED: the assertion is gone (22 999 rows to 0), the wire carries 2.78 of the 2.97 m per-satellite truth (0.43 before), the network's band-2 contamination is cured (0.740 to 0.119 m) and the triple field consumer goes from 1.47 0.21 0.09 0.11 0.09 0.03 m and 314 epochs below a decimetre to 0.68 0.09 0.06 0.11 0.09 0.04 m and 390, which fails the L5 adoption bars (dual pair 603; the band-5-code-removed emulation 535). The emulation had also removed the band-5 wide-lane detector pair (55 detections, 28 in the first hour), an unread lead. Original summary: Build card, pre-registered 2026-09-20. On a three-band lineage the per-signal extension of the geometry-free pin asserts a zero inter-signal correction between L1 and L5 for every satellite, where the truth is metres and estimable (card TBI). The build leaves an extra-band code-bias column free where the data determine it and closes each signal family with its sum-to-zero row. Default OFF, byte-identical OFF. Requirements, gates and bars frozen here before the build.
- **source:** [TBI](TBI_three_band_ignition_2026-09-20.md) section 5; [AB2](AB2_l5_third_frequency_2026-09-20.md) section 1; RF-0d and RF-0f (the per-signal pin and its same-band exception)
- **knobs / posture:** new `network.sat_gf_extra_band_free` (default false); RF-0hv working bases of `8fe757f` with `processing.frequency_mode: triple`
- **dates in the text:** 2026-09-20
- **hashes / run dirs:** bases `runs/_rap_{lF_tri,lS_tri}`, `runs/_rf0hv5_{field,sim}6`; emulation `runs/_rap_tB_nocode5_net`

## 1. What is built

`NetworkSbasis::buildCodeBiasDatumRows`, the per-signal loop (`NetworkSbasis.cpp` lines
903 to 970 at `7d4d70e`): for an extra signal whose band is neither band of the pin
pair, the row (d_k - d_ref) / (mu_k - 1) = c T_GD is an assertion, not a datum. With
the slant ionosphere and the clock fixed by the pinned pair, d_k is determined by
any (station, satellite) cell that carries code on the pair's two bands and on the
signal, up to one constant per signal family, which the receiver band-5 code bias
shares. The channel's sum-to-zero row (the block at lines 1152 to 1212, suppressed
today by the completeness test at 1170 to 1180 because every column is pinned) is that
constant's datum.

Requirements (the builder chooses the implementation after reading RF-0d, RF-0f and
the sum-to-zero time link, and follows the `sat_isc_state` precedent for the config
struct, the loader, the manifest, the fingerprint and the posture table):

- **R1** default OFF; with the knob OFF the row list and every product are
  byte-identical.
- **R2** with the knob ON an extra-band column that the data determine this epoch gets
  no assertion row, and its (constellation, signal) channel has exactly one level
  row.
- **R3** a column that the data do not determine this epoch (no cell with the three
  codes) never brings an absolute broadcast-frame value into a family that stands in
  the sum-to-zero frame: it is carried at its own previous solved value, or, at a
  first sighting, held and kept off the wire until it is first determined (the RF-0f
  precedent), and it is counted. The count of such column-epochs on the field day is
  reported (invariant #3: a deferral is triaged with data).
- **R4** the null-space gate's observed dimension equals the declared one on every
  epoch of the validation arms (`network_estimability.csv`, `n_below` as in the base).
- **R5** a re-reference of a family's level (the veteran link's reseed at a change of
  the satellite set) is an event at both edges: the satellites are marked, the marks
  are counted, and the consumer's reaction to them is measured (invariant #7).
- **R6** run-end telemetry beside `n_sat_gf_isc_zero`: extra-band columns freed,
  carried undetermined, held at first sighting.
- **R7** unit tests on the pure row builder: no assertion row and one level row with
  the knob ON; identical rows with it OFF; a two-family roster gauges to full rank
  with two level rows; an undetermined column follows R3; a set change re-references
  through the veteran link and marks.

## 2. Gates and bars (frozen)

- **G0** byte identity with the knob OFF on the dual field base, the triple field base
  and the triple sim base (products and event ledgers).
- **G1 mechanism, triple field, 6 h, knob ON:** zero assertion rows on determined
  extra-band columns; R4; the wire's triple code combination W matches the raw
  combination B of TBI section 5 (rms of B - W over the consumer's satellites at or
  below 0.30 m, 2.65 m today); the consumer's band-5 code residual p50 at or below 0.35
  m in every half hour (0.8 to 1.2 m today) and its band-2 residual back at or below
  0.15 m (0.11 m on the dual pair, 0.2 to 0.4 m today); the network's band-2 code
  post-fit station-median per satellite at or below 0.15 m sd across satellites (0.740
  today, 0.066 on the dual run).
- **B1 field triple, 6 h:** the L5 adoption bars of TBI section 4 (every hour within
  0.03 m of the dual pair or better, 570 or more epochs below a decimetre, clock wire
  at or below 0.0075 m tracked and 0.0140 m over all), and never worse than the
  emulation `tB_nocode5_net` by more than 0.03 m in any hour.
- **B2 sim triple, 6 h:** every hour within 0.03 m of the sim dual pair or better, and
  448 or more epochs below a decimetre.
- **B3 the wire's bands 1 and 2 are clean:** a dual consumer replayed on the knob's
  triple wire has a first hour at or below 0.50 m and a second at or below 0.10 m
  (1.48 and 0.16 m today).
- **B4, only if B1 passes:** a 12 h field arm never worse than `runs/_rap_lD_dual12` by
  more than 0.03 m from hour 3.
- **Not armed by this card.** A pass makes a recommendation to the author with the
  ledger row; the ledger rows of `network.sat_gf_datum` and `network.sat_isc_state`
  are read first.

## 3. 2026-09-20: B5D-1 result: built; the datum is cured; the consumer bars are NOT met

Build by an Opus worker on main `4746aa8`, exe `gnss_processor_b5d.exe`: the mechanism
sits in the RF-0d per-signal loop of `NetworkSbasis::buildCodeBiasDatumRows` (a
determined extra-band column takes no row; an undetermined one with a previous value
carries its own combination; an undetermined first sighting is held by a level row
in the channel's frame and kept off the record in `NetworkSolPrep`, the RF-0f
precedent); the determination roster is built in `NetworkSolveGlue` beside RF-0f's,
keyed on the realized pin pair; loader interlock (inert with a warning without
`sat_gf_datum`), manifest entry, run-end line in `NetworkTeardown`; suite
`SatGfExtraBandFreeTest` (8 cases). `gnss_tests`: 2341 ran, 2340 passed, the one known
skip (the worker's run; the manager re-ran the new suite and the two touched ones, 54
cases, all pass). The knob is not in the snapshot fingerprint, as `sat_gf_datum` and
`sat_isc_state` are not. Every number below was re-read from the run directories by
the manager unless marked.

**G0 PASS.** Knob OFF, new exe, against the base runs: `products.txt` and
`network_events.csv` sha256 equal on the triple field base (`6a3ef17f5f317738`,
`827211d3375a3ac2`), the dual field base (`72b5324bf12761d6`, `e5281e88fb4674e4`) and the
triple sim base (`0b2a04cc9183d993`, `f72aeffa973df800`).

**G1: five of seven clauses pass** (`runs/_rap_b5_tri`).

| clause | bar | before | with the knob | |
| --- | --- | --- | --- | --- |
| assertion rows on determined extra-band columns | 0 | 22 999 | 0 (22 999 column-epochs freed) | PASS |
| level rows | one per channel | suppressed | sum-to-zero rows suppressed 2767 to 1327 | PASS |
| null-space gate | as the base | | equal to the base in `network_estimability.csv`; the worker reports observed equal to declared on all 720 epochs, no withhold | PASS |
| rms of B - W over the consumer's satellites | 0.30 m | 2.647 | 0.483 (G28 alone +1.277; 0.241 without it, the worker's figure) | FAIL |
| consumer band-5 code residual p50 by half hour | 0.35 m | 0.78 to 1.16 | 0.10 0.08 0.12 0.10 0.13 0.26 0.13 0.10 | PASS |
| consumer band-2 code residual p50 by half hour | 0.15 m | 0.20 to 0.38 | 0.12 0.11 0.14 0.12 0.14 0.19 0.22 0.19 | FAIL in three of eight |
| network band-2 code post-fit, station-median per satellite, sd across satellites | 0.15 m | 0.740 | 0.119 (band 5: 0.791 to 0.123; band 1: 0.240 to 0.186) | PASS |

The wire now carries 2.780 m of the 2.973 m per-satellite truth (0.430 m before).

**B1 FAIL, B2 FAIL on the first hour alone, B3 FAIL narrowly, B4 not reached.**

| arm | consumer 3D p50 by hour (m) | sub-dm | 3D p50, half hours 1 to 3 (m) | median Up, half hours 1 to 3 (m) | clock wire, tracked / all (m) |
| --- | --- | --- | --- | --- | --- |
| `_rf0hv5_field6` (dual pair) | 0.43 0.07 0.06 0.08 0.08 0.07 | 603 | 1.27 0.18 0.07 | +1.07 +0.17 -0.01 | 0.0069 / 0.0128 |
| `_rap_lF_tri` (triple, knob OFF) | 1.47 0.21 0.09 0.11 0.09 0.03 | 314 | 3.64 0.73 0.29 | +3.54 +0.67 +0.27 | 0.0073 / 0.0182 |
| **`_rap_b5_tri`** (triple, knob ON) | **0.68 0.09 0.06 0.11 0.09 0.04** | **390** | 1.30 0.31 0.10 | +1.12 +0.30 +0.08 | 0.0072 / 0.0188 |
| `_rap_tB_nocode5_net` (the emulation) | 0.27 0.10 0.05 0.04 0.06 0.02 | 535 | 0.58 0.16 0.13 | -0.17 +0.02 +0.01 | 0.0078 / 0.0153 |
| `_rap_b5_dual_on_b5tri` (B3: a dual consumer on the knob's wire) | 0.61 0.11 0.08 0.09 0.13 0.06 | 349 | 1.26 0.24 0.11 | +1.08 +0.23 0.00 | |
| `_rap_tR_dual_on_tri` (the same on the old wire) | 1.48 0.16 0.10 0.11 0.14 0.07 | 226 | 3.76 0.61 0.21 | +3.25 +0.60 +0.18 | |
| `_rf0hv5_sim6` (sim dual pair) | 0.60 0.10 0.10 0.06 0.06 0.05 | 448 | | | |
| `_rap_lS_tri` (sim triple, knob OFF) | 0.56 0.16 0.08 0.04 0.04 0.04 | 430 | | | |
| **`_rap_b5_simtri`** (sim triple, knob ON) | **0.75 0.06 0.05 0.03 0.03 0.05** | **534** | | | |

B1: the first hour is 0.25 m over the dual pair and the fourth 0.03 m over; 390 epochs
against 570; the wire passes on the consumer's satellites (0.0072 m) and fails over all
(0.0188 against 0.0140 m); against the emulation the first hour is 0.41 m worse. B2:
hours 2 to 6 are all better than the sim dual pair and 534 epochs pass; the first hour
is 0.75 against 0.60 m. B3: 0.61 and 0.11 m against 0.50 and 0.10 m. The 12 h arm was run
as a diagnostic only (`runs/_rap_b5_tri12` against `runs/_rap_lD_dual12`): 0.68 0.09 0.06
0.11 0.09 0.04 0.04 0.06 0.13 0.22 0.24 0.21 m and 648 epochs against 0.43 0.07 0.06 0.08
0.08 0.07 0.08 0.06 0.06 0.14 0.19 0.17 m and 994; worst from hour 3 is 0.08 m over.

**R3 and R5.** No extra-band column was undetermined on either bed (field 22 999 freed,
0 carried, 0 held; sim 22 586, 0, 0), so the carry and hold paths are unit-tested and
never exercised; the deferral is field-inert on this day by that count. The level
re-referenced three times on the field and once on the sim, all at first
appearances, marking no satellite. The consumer's band-5 code-bias discontinuity
bumps go from 29 to 37 (the worker's count).

**Reading.** The assertion is gone and the datum is right: the wire carries the
per-satellite truth, the network no longer pushes it into band 2, and the consumer's
band-5 code fits at a decimetre. On the triple posture the knob is a gain in the
first three hours (0.68 0.09 0.06 against 1.47 0.21 0.09 m; 390 against 314 epochs) and
on the sim from hour 2 (534 against 430 and against 448 of the dual pair). It does not
reach the emulation, and the L5 adoption bars fail, so L5 stays out of the working
bases and the knob is not recommended for arming; it is inert on a dual lineage
(zero extra-band rows).

**The emulation removed a second thing.** Without the band-5 code the wide-lane
detector has no band-5 pair. `T0_mw_detect` / `mw_wl_slip`: 66 on the knob arm, 15 on
the emulation, 15 on the dual pair; 55 of the 66 name an L5 signal, 47 of them
refloat, 28 fall in the first hour and 18 at MAC100AUS. `T0_band_scope` is 705
against 461 with 404 against 225 of action `all`, and `T4_attrib` 230 against 158. The
gap between the knob arm and the emulation is therefore a lead on the band-5
wide-lane pair's detections at ignition, which no arm of this card separates from
the datum; whether they are slips is not read. Also open: G28's column sits 1.28 m
from the raw combination although 37 of 37 stations determine it (the worker's
census); a warm resume across a flip of the knob would carry the previous frame's
code biases into the new frame with no gate (untested; every arm here is cold; a
blocking deferral for a warm lineage, inert for the cold beds).

**Standing of the knob.** Built, default OFF, byte-identical OFF, the mechanism
verified; not armed; no ledger row yet (owed with the decision on the band-5
detector lead). The card stays open for that decision.

## 4. 2026-09-20: two manager reads after the build (no new mechanism)

**A dual consumer on the emulation's wire** (`runs/_rap_tR_dual_on_tB`, a replay of
`runs/_rap_tB_nocode5_net/products.txt` by the dual consumer, 12 s): 0.26 0.08 0.05 0.03
0.10 0.06 m and 505 epochs below a decimetre, against 0.43 0.07 0.06 0.08 0.08 0.07 m and
603 of the dual pair and 0.61 0.11 0.08 0.09 0.13 0.06 m and 349 on the knob's wire. A
three-band network that uses the L5 phase and no band-5 code gives a user of bands 1
and 2 a faster first hour and better hours 3, 4 and 6, and loses hours 2 and 5 by 0.01
and 0.02 m; by the count of epochs below a decimetre the dual pair stays ahead of
every three-band arm of the day (603 against 535, 505, 390, 349, 314).

**The band-5 wide-lane detections of the knob arm are not obviously false.** Of the 55
`mw_wl_slip` events that name an L5 signal (pairs L2W;L5Q 26, L2W;L5X 14, L1C;L5Q 10,
L1W;L5X 5), 36 have a phase-only `T1_if_slip` at the same station and satellite within
60 s, 29 an LLI event, 23 a gap event within 90 s; 19 are wide-lane-only. 19 of the 28
first-hour ones fall in the first ten minutes of the cold start. The lead of section
3 therefore reads: the cost, if it is there, is in the RESPONSE to these detections
at ignition (action `all` 404 against 225) or in the band-5 code rows themselves, and
separating the two needs a pair scope for the wide-lane detector or a code weight
per band, neither of which exists as a knob.
