# SPEC — RINEX 3 zero-sentinel guard (`processing.rinex_skip_zero_values`)

**Status**: BUILT 2026-08-01 (Fable session, era-2 investigation day) — parser guard +
config toggle + unit suite landed; default **OFF** (legacy byte-identity).  Field
role: **latent-hazard guard** — explicitly NOT the FT63 era-1/era-2 owner (see §3).
**Memory context**: `project_ft63_fast_tier` (the eras), the DOY-129 zero census
(this doc §2).

## 1. Defect

RINEX 3.0x §6: *"Missing observations are written as 0.0 or blanks."*  A written
zero is a data-not-available sentinel, not a measurement.

- The RINEX **2.x** branch has always honored this
  ([RinexParser.cpp:986](../src/core/rinex/RinexParser.cpp) — `value == 0.0 → continue`,
  comment "0.0 indicates observation not made").
- The RINEX **3** branch ingested written zeros as real values
  (RinexParser.cpp `parse()` v3 value loop): a 0-cycle phase alongside a valid
  ~2.1·10⁷ m pseudorange becomes a ~−2·10⁷ m phase pseudo-observation at phase
  weight if the cell survives the elevation mask.  No downstream screen owns it
  on the FT63-class stack: `raim_within_epoch` is carry_warm-gated (inert with
  leveling OFF), innovation gates on the carry side withhold folds but not rows.

## 2. Evidence (DOY-129 2024, FT63 63-station set)

Full-day zero census (numeric epoch parse, cross-checked against producer
`n_pf_rows` — the corrected-census method):

- **Only two stations emit written zeros: GOPE00CZE + GOP600CZE** (the Ondrejov
  zero-baseline pair; Trimble receivers) — 5 955 zero cells total, of which
  2 782 are zero-PHASE-with-valid-CODE.  Zeros appear at weak signal
  (S1C ≈ 20–23 dBHz) on rising/setting satellites.
- Temporal correlation with the two FT63 eras is real but NOT causal:
  era-1 (07:32:00 break): GOPE G06 L1C+L2W zeros AT the break epoch, G05 both-band
  zeros one epoch before.  Era-2 (19:26:00 break): GOP6 G01/G02/G28 (+GOPE G28)
  zero-runs 19:05–19:25, ending one epoch before the break.
- **All DOY-129 zero cells sat BELOW the 10° elevation mask** (verified: the
  affected satellites are absent from every producer pool — slip_raw, DD basis —
  in the zero windows; G01@GOP6 enters the pool at exactly elev 10.01°, 52 min
  after its zero-run).  The defect was therefore **field-inert on this day** —
  which is exactly why it is latent: the zero regime (weak signal, low
  elevation) usually hides under the mask, but nothing guarantees it (ALLOY
  weak-signal zeros at 10–15° under scintillation would inject).

## 3. What this card is NOT

The FT63 era-1/era-2 root cause.  The eras' break epochs have clean, full,
above-mask ingest (GF-blind scans: per-observable BSD-TD2, phase-minus-code
steps, receiver-common steps, detrended fine steps — all classes exonerated at
the break epochs).  Era-2's injector was identified the same day by the
reduced-RHS class-attribution instrument (`network.rhs_attrib_tow`) as an
internal mechanism (see `project_ft63_fast_tier`).

## 4. Design

One guard at the single v3 value-parse site (`RinexParser.cpp`, after `stod`):

```cpp
if (value == 0.0 && config_.rinexSkipZeroValues()) { ++zero_sentinels_skipped_; continue; }
```

- Toggle `processing.rinex_skip_zero_values` (default **false** = legacy).
  Follows the config-toggle convention (private member + Doxygen getter
  `rinexSkipZeroValues()` + loader line + arming info line).
- Counter logged at parse end per file when nonzero (invariant #4): the guard is
  observable, never silent.
- Applies to every observation letter (C/L/D/S), matching the 2.x branch
  exactly.  True zero crossings are not representable data (writers offset or
  blank them; a phase tracking through exactly 0.000 cycles at F14.3 is a
  measure-zero event the standard already sacrifices).

## 5. Validation

- Unit suite `tests/test_rinex_zero_sentinel.cpp` (suite `RinexZeroSentinelTest`,
  one-suite-per-file rule): legacy path ingests the zero (defect record); armed
  path drops the zero, keeps the valid same-band partner and all real values
  bit-exact.
- OFF-path byte-identity is structural (the guard is inside `if (armed)`).
- Golden A/B before any default flip: per the shared-path discipline the default
  stays `false`; arming goes into new configs alongside the standard armed set.
  Promotion-ledger entry: candidate default-flip after one clean same-build
  golden comparison on a GOPE/GOP6-bearing set (the only stations where the
  bit-stream differs at all — every other station's parse is untouched).

## 6. Deferred / adjacent (pre-declared)

- **Band-loss policy inconsistency** (flicker card INC-0: MBAR/G21 LOS-dropped
  vs BSHM/G07 L1-only-processed) — separate card, unchanged here.
- Consumer-side RTCM/SBF decoders: not touched (MSM fields carry their own
  validity flags; SBF path has its own decode).
- A "zero-with-valid-partner above mask" one-shot WARN (detection without the
  guard) was considered and dropped — the counter + info line under the armed
  guard covers observability; unarmed legacy runs stay bit-silent by design.
