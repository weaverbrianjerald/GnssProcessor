# GRK: the gap re-entry response keyed on the slip tracker's signal, not on the carried one

**STATUS**

- **id:** GRK
- **topic:** slip
- **status:** open
- **summary:** READ 2026-09-22 (manager; one emulation arm, no build). DEFECT of the row-level gap re-entry response (card GRS's predecessor, `network.cycle_slip.gap_reentry_relevel`): the raw slip tracker forms its band-1 signal as the lexically lowest phase with a matching code (L1C before L1W), while the carried ambiguity at a station whose priority latch chose L1W is keyed L1W. At the 10 field stations that track both, the response looks for a carry under L1C, finds none and returns (`no_carry`): 34 of the run's 88 shifted re-entries are answered on band 2 only and the stale L1W ambiguity folds across the break. Emulation (L1C stripped at those 10 stations, `runs/_rap_k1_strip_l1c`): gross post-fit code rows above 10 m 507 to 103 (SGOC00LKA G13: 393 to 1), refloats 128 to 144, band-1 arcs bridged clean 1 to 13; consumer, census and clock wire unchanged. Fix pre-registered in section 3 (the tracker prefers the station's MODELED signal per band, so that the tracker, the carry and the response agree by construction); NOT built: the author's decision.
- **source:** [SCU](SCU_station_coordinates_unknowns_2026-09-22.md) section 7 (the gross code rows at SGOC00LKA named as a candidate for the regional bias); [GRS](GRS_gap_reentry_per_signal_2026-09-20.md) (the per-signal tier that closed the L5 hole; this is the band-1 hole of the row-level tier)
- **knobs / posture:** none built. Proposed: `network.cycle_slip.tracker_modeled_first` (default OFF). Posture: the RF-0hv working bases (baseline field `runs/_rf0hv8_field6`, sim `runs/_rf0hv7_sim6`)
- **dates in the text:** 2026-09-22
- **hashes / run dirs:** `runs/_rf0hv8_field6`, `runs/_rap_k1_strip_l1c`, `runs/_rap_y1_drop_sgoc`, `runs/_rap_y2_drop_kit3`, `runs/_rap_y3_drop_both`, census keys `rf8`, `k1`, `y3`; exe `gnss_processor_rf7.exe` (`97547306...`)

## 1. The read

`runs/_rf0hv8_field6` carries 507 post-fit code rows above 10 m at full weight, 393 of them
at SGOC00LKA on G13 in hours 5 and 6 (C1C, C1W and C2W alike, +17 and +28 m; the arc
re-entered at TOW 275490 after a 5-epoch gap with a GF step of -22.4 m). The events of
that epoch: `reentry_relevel L2W refloat` and `arc_resume L1C shifted_fresh; pl=0; dd=0;
slip=0`. The response freed band 2 and never reached band 1.

Cause, in the code:

- `src/core/slip/NetworkRawObservables.cpp` lines 55 to 66: the arc-level tracker forms
  the pair from `raw_observed` as the lexically lowest phase signal per band that has a
  matching code (`std::sort` on the band digit, then `ph.front()`), so a station reporting
  L1C and L1W tracks band 1 as L1C.
- With `processing.canonical_signal_rule: priority` (card CSL) 18 field stations latch L1W
  as their canonical band-1 signal; their carried ambiguities are keyed L1W. Ten of them
  also report L1C with C1C: BOGT00COL, GCGO00USA, MBAR00UGA, SGOC00LKA, STHL00GBR,
  SVTL00RUS, ULAB00MNG, VOIM00MDG, WIND00NAM, YKRO00CIV.
- `gapReentryRespondArc` (`src/core/slip/NetworkSlipQc.cpp`) looks up `local_carry` at
  `(station, satellite, sig)` with the tracker's `sig`: no entry under L1C, `no_carry`,
  return. The Card-G classification applies the same predicate, so the ledger reads
  `shifted_fresh` with `pl=0` for these arcs by construction (what the response cannot
  see it cannot count as missed).
- Band 2 is not affected: L2W sorts before L2X and is the priority latch as well.

Population in the 6 h run: 88 shifted re-entries, 34 at the ten stations (BOGT00COL 10,
YKRO00CIV 5, SGOC00LKA 5, GCGO00USA 5, STHL00GBR 3, MBAR00UGA 2, WIND00NAM 2, VOIM00MDG 1,
ULAB00MNG 1); 14 `common_band keep` events name L1C in their LLI basis at the same
stations.

## 2. Emulation (config-only arm, 6 h): `runs/_rap_k1_strip_l1c`

The L1C observation type stripped from the ten stations' RINEX copies (their C1C rows
stay), so the tracker's band-1 signal becomes L1W and matches the carry.

| | `runs/_rf0hv8_field6` | `runs/_rap_k1_strip_l1c` |
| --- | --- | --- |
| post-fit code rows above 10 m (all stations) | 507 | 103 |
| at SGOC00LKA G13 | 393 | 1 |
| `reentry_relevel` refloats | 128 | 144 |
| band-1 arcs `clean_bridged` / `clean_fresh` | 1 / 22 | 13 / 13 |
| band-1 arcs `shifted_fresh` | 88 | 85 |
| WTZZ00DEU 3D p50 by hour (m) | 0.56 0.06 0.03 0.03 0.04 0.04 | 0.56 0.06 0.03 0.03 0.04 0.04 |
| epochs below 0.1 m | 611 | 611 |
| clock wire, all / consumer-tracked (m) | 0.0045 / 0.0028 | 0.0045 / 0.0028 |
| census (16 sites, key `k1` against `rf8`): hours 2 to 6 | | 0 better, 0 worse (median 0.000 m) |

The gross rows of the row-level hole are gone; the consumer does not see the difference at
6 h (the remaining 103 rows are BSHM00ISR's G22 ramp, STHL00GBR, CPVG00CPV and KIT300UZB,
which are not this class). The band-1 `shifted_fresh` count stays at 85 because the
emulation moves the ten stations' arcs from unreachable to reached-and-freed, and a freed
arc reads `shifted_fresh` by construction (section 1).

Two arms that drop the stations SCU section 7 named as candidates for the regional Up
bias over Asia (`runs/_rap_y1_drop_sgoc`, `_y2_drop_kit3`, `_y3_drop_both`): WTZZ00DEU
within 0.02 m per hour of the baseline, clock wire 0.0042 to 0.0045 m; census key `y3`
against `rf8`: WUH200CHN mean Up -0.169 to -0.160 m, BADG00RUS -0.130 to -0.148 m. The
gross rows at those two stations are NOT the regional bias; that item stays open in
card SCU.

## 3. Fix, pre-registered (NOT built)

- F1. `network.cycle_slip.tracker_modeled_first` (bool, default OFF). Armed, the arc-level
  tracker (`NetworkRawObservables.cpp`, the `ph` selection) prefers, on each band, the
  phase signal that the station's model carries for that band (the signal present in
  `observed`, which is the canonical latch's choice) and falls back to the lexically lowest
  raw phase with a matching code only on a band the model does not carry (band 5 on a
  dual posture). The per-signal tier of card GRS is untouched (it keys on the signal).
- F2. Telemetry in both knob states: a run-end count of arcs whose tracker signal differs
  from the carried key on the same band (`gap_tracker_key_mismatch`), and the count of
  shifted re-entries that returned `no_carry` while a carry existed on the same band under
  another signal (the unreachable population, 34 here). OFF, a WARNING at run end when
  the count is nonzero.
- F3. Byte identity OFF on both beds; the sim (one signal per band) byte-identical ON.
- F4. Pre-flight: invariant 1 (the response the carry needs, now reaching every arc),
  invariant 4 (F2's counters), invariant 7 (the re-entry edge, already an event).

Gates and bars (field 6 h from `runs/_rf0hv8_field6/arm.json`, exe from the build):

- G0. Knob absent: the three graded files identical on both beds.
- G1. Knob ON equals the emulation `runs/_rap_k1_strip_l1c` on: gross code rows above 10 m
  (103, within 5), refloats (144, within 2), band-1 `clean_bridged` (13, within 2), and the
  consumer's hourly p50 within 0.002 m. The unreachable count of F2 reads 0 ON and 34 OFF.
- G2. Sim ON byte-identical to `runs/_rf0hv7_sim6`.
- B1. WTZZ00DEU hourly p50 within 0.005 m of the baseline (the class is consumer-inert at
  6 h; the bar guards against a regression, not for a gain).
- B2. Census paired against `rf8`: not worse (hours 2 to 6 better or equal at more sites
  than worse).
- B3. 12 h against `runs/_rap_a8_field12`: refloats, gross rows, hourly p50.

Recommendation: build it. Small, bounded (one selection rule plus counters), and it
closes a response hole at 10 of 58 stations that produces gross rows at full weight;
those rows sit in the wire's inputs whether or not this consumer sees them.
