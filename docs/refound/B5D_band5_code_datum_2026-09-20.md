# B5D: the band-5 code datum

**STATUS**

- **id:** B5D
- **topic:** network-vehicle
- **status:** open
- **summary:** Build card, pre-registered 2026-09-20. On a three-band lineage the per-signal extension of the geometry-free pin asserts a zero inter-signal correction between L1 and L5 for every satellite, where the truth is metres and estimable (card TBI). The build leaves an extra-band code-bias column free where the data determine it and closes each signal family with its sum-to-zero row. Default OFF, byte-identical OFF. Requirements, gates and bars frozen here before the build.
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
