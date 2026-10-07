# LV-1: bias-free per-LOS iono seeding

**STATUS**

- **id:** LV-1
- **topic:** ionosphere
- **status:** closed
- **summary:** The stated seed-gradient mechanism is FALSIFIED: the seed is a FRAME, not an authority (the carry-OFF arms agree to 0.006 TECU); diagnostic-keep, since it sets the frame LV-2 pins.
- **dates in the text:** none (built and graded 2026-08-05, per [LV impl](../ionosphere/LV-IMPL-RECORD.md))
- **knobs / posture:** `network.iono_seed_field_frame` (+ `_min_sats`, default 5), loader default OFF, conditional fingerprint `lv1seed:<min_sats>`; run-end counters `lv1_seeds_reframed`, `lv1_fallback_raw`
- **hashes named in the text** (commit / exe / byte-cert): none
- **run dirs named in the text:** none as `runs/` paths; arms `_ext63_gps_dual_20ep_a00gim_sl`, `…a00gim_sl_nc`, `_agu63_nolane_30ep` (each + LV-1)
- **source:** `docs/archive/SPEC_level_information_budget.md` §3; outcomes in §10 and §13 to §14 ([LV impl](../ionosphere/LV-IMPL-RECORD.md), [LV correction](../ionosphere/LV-CORRECTION-AND-12H.md))

**Outcome.** The card asked whether seeding each LOS's iono from the SH field, without the receiver's GF bias, lifts the collapsing ionosphere level. The decisive carry-OFF arm still collapsed to −37.1 TECU: the seed is a linearization point, a solve no-op (§10.1). With the carry on, LV-1 acts through the frame-dependent inject gate (a defect, fixed by LV-4) and by booking the GF bias into the DCB seed that LV-2's de-mean pins (§13.3). The 12 h DD-fraction readout stayed uniform (0.2503). Verdict: diagnostic-keep ([LV](../ionosphere/LV-REF.md)); the LV stack is not armed for settled work (§14).

> Results are appended TO THIS FILE, below the verbatim text. The archived monolith is frozen; never append there.

---
<!-- BEGIN VERBATIM: SPEC_level_information_budget.md -->
## 3. Card LV-1 — bias-free per-LOS iono seeding  (PRIMARY)

### Current behaviour

`StateInitializer::computeIonosphereSeed` (`StateInitializer.cpp:263`) seeds each per-LOS iono state at
allocation from the GF of product-referenced code residuals: `iono_seed = ((P1−mod1) − (P2−mod2)) /
(alpha1−alpha2)`. `mod` contains the satellite side and geometry but NOT the receiver DCB, so the receiver's
(d_r,band1 − d_r,band2)/(α1−α2) rides into **every seed of that station** — the bias-in frame. Its own comment
notes the seed propagates into per-row predicted values and downstream sat-bias/ambiguity seeds, so the frame
contamination spreads at birth. This runs at cold start AND at every re-allocation (slip refloat, gap
re-birth) — which is why the fix must live inside this function, not at a call site.

### Change

Inside `computeIonosphereSeed`'s network use (gate on `ProcessingMode::PPP_RTK_Network` — user PPP has no
field to reframe against), replace the station-common content of the seed with the field's own value at the
IPP:

```
seed'_los = seed_los − station_offset_r + field_slant(IPP_los)
```

- `station_offset_r` = the **median over this station's per-sat seeds this epoch** (median, not mean —
  single-sat outliers must not move it). Computed once per station per allocation epoch; needs ≥ `min_sats`
  (default 5) seeds, else **fallback to the raw seed** and count it.
- `field_slant(IPP)` = m(el) × the SH field's current value at the LOS's IPP. At true cold start the field
  evaluates to the a00 prior mean — the designed authority doing its job.
- Practical shape: `computeIonosphereSeed` is per-observation and cannot see other satellites. Do the reframe
  one level up, or give StateInitializer a per-epoch, per-station cache: collect raw seeds, compute the median
  once, reframe all. Keep the `|seed| < 200 m` sanity check on the RAW seed.

### Events (invariant #1)

- Re-allocation mid-run (slip refloat, gap re-birth): same path runs; the median is taken over THAT epoch's
  live seeds; the field is warm — the re-born LOS inherits the current frame, not the raw bias-in frame. That
  is the required event response and it falls out of putting the fix inside the seed path.
- Station appearing with < min_sats: raw-seed fallback + counter (`lv1_fallback`). A handful of bias-in LOS
  cannot move the field.

### Knob, counters, trace

- `network.iono_seed_field_frame` (bool, loader default false, armed in every EXT-CB config from day one).
- Run-end counters `lv1_seeds_reframed`, `lv1_fallback_raw`, and ONE per-station log line at cold start.
  **Log the FIELD-REFERENCED offset**, not the raw median: `station_offset_r −
  median_los(field_slant(IPP_los))`. ⚠ **§10.4 correction**: this was advertised as "a direct estimate of the
  station's rx GF bias", cross-validatable station-by-station (± a few TECU) against `extcb_gim_verdict.py`'s
  `bias_TECU` column — the fleet table whose range is **MAC1 −33.6 … KRGG +45.9**. Measured, the offset is
  **dominated by the station's MEDIAN SLANT IONOSPHERE** (BOR100POL +9.71 m ≈ +59.8 slant-TECU at a ~30 TECU
  vertical field). The log line says "hardware bias ⊕ median slant ionosphere" for that reason, and the
  cross-check requires removing the median slant first. It is NOT the free cross-validation this section
  advertised.

### Validation arms + predictions (write these into the configs before running)

1. `_ext63_gps_dual_20ep_a00gim_sl` + LV-1 (carry ON): trough ≥ 15 (arms 12/13 dipped to ~6-7), deg-1 peak <
   30 (arm 12: 34.6), breathing reduced in a 1 h follow-up.
2. **THE DECISIVE ONE — `…a00gim_sl_nc` + LV-1** (carry OFF, arm-14 posture): if the mechanism model is right
   the -9 TECU/ep collapse largely DISAPPEARS. If it still collapses, STOP. → **It still collapsed; §10.1.**
3. Self-contained spot-check: `_agu63_nolane_30ep` + LV-1. → arm 26, §17 item 4.
4. Bonus readout (measure, do not gate): DD fractional parts on a settled ≥1 h run. Rationale on record: the
   bias-in seed frame puts a PERSISTENT, fleet-structured error into every ambiguity born under it — receiver
   biases cluster by manufacturer fleet, **measured range -12..+16 ns, up to ~70 slant-TECU between
   mixed-hardware stations**, and the standing record attributes **99.3 % of NL fractional variance to iono**.
   Prediction: `med|frac_cyc|` moves off the uniform-0.25 signature. → **Dead at 12 h too (§17 item 0: 0.2503
   vs 0.2498 on 322 936 rows).**

