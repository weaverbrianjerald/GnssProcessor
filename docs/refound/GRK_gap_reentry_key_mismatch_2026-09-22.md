# GRK: gap re-entry response keyed on the tracker's signal, not the carried one

**STATUS**

- **id:** GRK
- **topic:** slip
- **status:** closed
- **summary:** ARMED 2026-09-22: `network.cycle_slip.tracker_modeled_first` names each band by the latched phase; gross post-fit code rows above 10 m 507 to 101, consumer-inert.
- **source:** [SCU](SCU_station_coordinates_unknowns_2026-09-22.md) section 7 (the gross code rows at SGOC00LKA named as a candidate for the regional bias); [GRS](GRS_gap_reentry_per_signal_2026-09-20.md) (the per-signal tier that closed the L5 hole; this is the band-1 hole of the row-level tier)
- **knobs / posture:** `network.cycle_slip.tracker_modeled_first` (built `c8232ad`, default OFF; ARMED in the RF-0hv working bases 2026-09-22, pinned in the goldens by card GRB, ledger row A10 RECOMMEND_SCOPED); the row-level response it acts through, `network.cycle_slip.gap_reentry_relevel`. Posture: the RF-0hv working bases (baseline field `runs/_rf0hv8_field6`, sim `runs/_rf0hv7_sim6`; from the arming `runs/_rf0hv9_{field,sim}6`)
- **dates in the text:** 2026-09-22
- **hashes / run dirs:** build arms `runs/_rap_grk_g0_field`, `_grk_g0_sim`, `_grk_g1_field_on`, `_grk_g2_sim_on`, `_grk_g3_field12_on`, `_grk2_*`, `_grk3_*`, `_grk4_*`, census key `grk`; exes `gnss_processor_grk.exe` (`E62932FA...`) to `gnss_processor_grk4.exe` (`CEB14EAB...`); read: `runs/_rf0hv8_field6`, `runs/_rap_k1_strip_l1c`, `runs/_rap_y1_drop_sgoc`, `runs/_rap_y2_drop_kit3`, `runs/_rap_y3_drop_both`, census keys `rf8`, `k1`, `y3`; exe `gnss_processor_rf7.exe` (`97547306...`)

**Outcome.** The row-level gap re-entry response missed band-1 re-entries because the slip tracker named band 1 L1C while the carry at L1W stations is keyed L1W, so the stale ambiguity folded across the break. The build passed every gate except G1's refloat count, MISSED by one (147 against 144 within 2; the emulation is a different intervention, card SID section 3); unreachable re-entries 21 to 0, consumer, census and clock wire unchanged. ARMED on the author's instruction (section 5). Card [SID](SID_signal_identity_2026-09-22.md) phase 2 (`network.roster_tracker`, default OFF) reproduces it exactly and retires it by construction once armed.

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

## 4. Build result (2026-09-22; Opus build worker in four rounds, every number re-graded by the manager from `runs/`)

### 4.1 The build

Main `c8232ad`. Knob `network.cycle_slip.tracker_modeled_first` (default OFF). The arc-level
tracker (`networkComputeRawObservables`) takes an optional per-station lookup of the LATCHED
phase per band (`PppModel::latchedCanonicalPhaseOnBandDigit`, new, beside the code accessor);
armed, a band whose latched phase is present in the raw record with its matching code is named
by that phase, otherwise the lexical rule applies for that band (a band the model does not carry
falls back as before). The source is the latch, not the epoch's admitted row set: the two agree
at every cell on this bed (byte-identical output either way), and the latch is what the carry is
keyed on. Two counters in both knob states: `gap_tracker_key_mismatch` (cells (station,
satellite, epoch, band) where the tracker's signal differs from the latched phase) and
`gap_reentry_unreachable` (ROW-LEVEL re-entry arcs at which the response found no carry under
the tracker's signal while a carry inside the max-gap window existed on the same band under a
signal that is tracked with its code at that epoch: the population the response could have acted
on). The counter's definition took three rounds to sharpen: the first counted every same-band
carry (46 OFF / 25 ON), the second required the carried signal to be present at the epoch (36 /
15; the absent cases are partial records whose modelled phase has no row that epoch, for example
BSHM00ISR G14 at TOW 270390: C1C without L1C, L1L with C1L), the third excluded the per-signal
tier, whose returning signal IS its key (a non-modelled L2L returning while the modelled L2W
tracked through is not a hole). Final: **OFF 21, ON 0**, equal to the fall of `gap_reentry_no_carry`
(133 to 112). The run-end line WARNS when the knob is OFF and the count is nonzero; the manifest
carries the knob when armed. Not fingerprinted (verified: no slip-QC token in
`networkSnapshotFingerprint`; the tracker's history is exempt from the snapshot). Suite
`TrackerModeledFirstTest`, 14 cases; `gnss_tests` 2434 ran, 2433 passed, 1 known skip. Exes
`gnss_processor_grk.exe` (`E62932FA...`, the solve) to `gnss_processor_grk4.exe`
(`CEB14EAB...`, the final counters); the three graded files of the ON arm are byte-identical
across all four builds, so the solve numbers of the first round stand.

### 4.2 Gates

| gate | bar | measured | verdict |
| --- | --- | --- | --- |
| G0 | knob absent, three files identical on both beds | identical (`runs/_rap_grk_g0_field`, `_grk_g0_sim`, again at `_grk2_`, `_grk3_`, `_grk4_`) | PASS |
| G1 | gross code rows above 10 m within 5 of the emulation's 103 | 101 | PASS |
| G1 | refloats within 2 of the emulation's 144 | 147 | FAIL by 1 |
| G1 | band-1 arcs bridged clean within 2 of 13 | 13 | PASS |
| G1 | consumer hourly p50 within 0.002 m of the emulation | largest difference 0.000 m | PASS |
| G1 | `gap_reentry_unreachable` 0 ON | 0 | PASS |
| G1 | `gap_reentry_unreachable` OFF reads the population (34 in the card) | 21 (final definition; the card's 34 counted shifted re-entries at the ten stations from the events, which is a different unit) | read |
| G2 | sim ON byte-identical to `runs/_rf0hv7_sim6` | identical | PASS |
| B1 | WTZZ00DEU hourly p50 within 0.005 m of the baseline | 0.560 0.059 0.028 0.030 0.036 0.045 against 0.560 0.059 0.027 0.029 0.036 0.041 m; largest 0.004 m | PASS |
| B2 | census paired against `rf8`: not worse | hours 2 to 6 no site moved by more than 0.01 m; 3D p50 over hours 4 to 6 better at 10 of 16 (median -0.0002 m); absolute mean Up smaller at 10 of 16 | PASS |
| B3 | 12 h against `runs/_rap_a8_field12` | 0.560 0.059 0.028 0.030 0.036 0.045 0.024 0.026 0.041 0.102 0.132 0.118 against 0.560 0.059 0.027 0.029 0.036 0.041 0.021 0.025 0.041 0.103 0.132 0.119 m; below 0.1 m 1022 against 1018; gross code rows 261 against 759; refloats 320 against 274; clock wire 0.0065 / 0.0045 against 0.0063 / 0.0041 m; trips 11 against 11 | read |

The refloat miss by one is the emulation's own doing: stripping L1C removed rows, while the knob
keeps them for the per-signal tier, which frees three more arcs. Card SID's audit (section 3 of
that card) showed the emulation and the knob are different interventions in any case: the
emulation removed 10 of the 46 mismatched station-band cells, the knob removes all 46 (band 1 at
all 18 L1W stations, band 2 at 28 stations where L2L or L2S sorts before L2W; the per-signal tier
answered 10 of the 14 band-2 re-entries, so the band-1 hole is the one that is total). The
statement of section 1 that the exposure is ten stations is corrected: all 58 stations report
C1C, so the tracker named band 1 L1C at every L1W station; the ten were the ones with a shifted
re-entry in the window.

### 4.3 Reading and recommendation

Consumer-inert at 6 h and 12 h (WTZZ00DEU within 0.004 m per hour, census flat, clock wire within
0.0004 m), which the card expected: what it removes is gross rows folded at full weight (507 to
101 at 6 h, 759 to 261 at 12 h) and a response hole at every L1W station. Recommendation: arm it in
the working bases now; it is also phase 2 of card SID (the tracker reads the roster), which
replaces the knob by construction when the roster lands. The decision is the author's.

### 4.4 What stays open

- The remaining gross rows (BSHM00ISR G22 ramp, STHL00GBR, CPVG00CPV, KIT300UZB): card RF-3's
  code-row QC.
- The band-2 mismatch's cost is not isolated (card SID).
- The tracker's comparator sorts on the band digit alone under `std::sort` (formally unstable
  order inside a band); the knob makes the order irrelevant where the latch exists, phase 2 of
  card SID removes the sort.

## 5. 2026-09-22: ARMED in the RF-0hv working bases

At the author's instruction `config/refound/{field,sim}_gps_dual_rf0hv.json` now carry
`network.cycle_slip.tracker_modeled_first: true` (one key plus dated comment lines). Working
bases only. The promotion-ledger row and the loader's posture row land in the next build
window (the posture table needs a rebuild and its parity check).

Confirmation arms from the armed bases with only the product path changed, exe
`gnss_processor_grk4.exe` (`CEB14EAB...`), both exit 0: `runs/_rf0hv9_field6` is
byte-identical to `runs/_rap_grk_g1_field_on` on `products.txt` (lines starting with `#`
removed), `closed_loop_trace.csv` and `network_epoch_status.csv`; `runs/_rf0hv9_sim6` is
byte-identical to `runs/_rf0hv7_sim6` on the same three files (one signal per band on the sim).
The field run's counters: `gap_reentry_unreachable` 0 of 112 `no_carry`.

**The baseline pair from this date: `runs/_rf0hv9_{field,sim}6`.** Field: consumer 3D p50 by
hour 0.560 0.059 0.028 0.030 0.036 0.045 m, clock wire 0.0045 / 0.0028 m, gross post-fit code
rows above 10 m 101. Sim: unchanged, 0.601 0.104 0.096 0.056 0.055 0.049 m. Census replays of
the field wire: key `grk` (identical wire).
