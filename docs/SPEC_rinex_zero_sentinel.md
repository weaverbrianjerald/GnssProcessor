# SPEC — RINEX ingest hazards on DOY-129: the zero-sentinel guard (BUILT) and the structural-ingest guard (RETRACTED)

**Status.** §1–§6 (zero-sentinel guard) **BUILT 2026-08-01** — parser guard + config toggle + unit suite;
default **OFF** (legacy byte-identity). Field role: **latent-hazard guard**, explicitly NOT the FT63
era-1/era-2 owner (§3). §7 (structural-ingest guard) is ⛔ **RETRACTED before implementation — DO NOT BUILD**;
absorbed 2026-08-15 from `docs/SPEC_ingest_structural_guard.md` (now `docs/attic/`). The two cards are halves
of one story: the retracted card's fatal defect was a hand-rolled RINEX census, and §2 is the corrected-census
method. **Memory:** `project_ft63_fast_tier`. **Provenance:** every `runs/<name>` path here
(`runs/ft63_24h_allon`, `runs/ft63_leg19_off`, the `snap_archive` seeds) resolves under
`D:\GnssProcessor_archive\2026-08-12\runs\`, not the working tree. These measurements cannot be re-derived
here.

## 1. Defect

RINEX 3.0x §6: *"Missing observations are written as 0.0 or blanks."* A written zero is a data-not-available
sentinel, not a measurement.

- The RINEX **2.x** branch has always honored this — `value == 0.0 → continue`, comment "0.0 indicates
  observation not made" (live site [RinexParser.cpp:994](../src/core/rinex/RinexParser.cpp); this doc's
  original `RinexParser.cpp:986` has drifted — prefer the comment).
- The RINEX **3** branch ingested written zeros as real values (the v3 value loop in `parse()`): a 0-cycle
  phase alongside a valid ~2.1·10⁷ m pseudorange becomes a ~−2·10⁷ m phase pseudo-observation at phase weight
  if the cell survives the elevation mask. No downstream screen owns it on the FT63-class stack —
  `raim_within_epoch` is carry_warm-gated (INERT with leveling OFF) and carry-side innovation gates withhold
  folds, not rows.

## 2. Evidence (DOY-129 2024, FT63 set)

Full-day zero census over the 63 stations by numeric epoch parse, cross-checked against producer `n_pf_rows` —
the corrected-census method:

- **Only two stations emit written zeros: GOPE00CZE + GOP600CZE** (the Ondrejov zero-baseline pair, Trimble
  receivers) — 5 955 zero cells, of which 2 782 are zero-PHASE-with-valid-CODE. Zeros appear at weak signal
  (S1C ≈ 20–23 dBHz) on rising/setting satellites.
- Temporal correlation with the two FT63 eras is real but NOT causal. Era-1 (07:32:00 break): GOPE G06
  L1C+L2W zeros AT the break epoch, G05 both-band zeros one epoch before. Era-2 (19:26:00 break): GOP6
  G01/G02/G28 (+GOPE G28) zero-runs 19:05–19:25, ending one epoch before the break.
- **All DOY-129 zero cells sat BELOW the 10° elevation mask** — the affected satellites are absent from every
  producer pool (slip_raw, DD basis) in the zero windows; G01@GOP6 enters the pool at exactly elev 10.01°, 52
  min after its zero-run. The defect was **field-inert on this day**, which is exactly why it is latent: the
  zero regime (weak signal, low elevation) usually hides under the mask, but nothing guarantees it (ALLOY
  weak-signal zeros at 10–15° under scintillation would inject).

## 3. What this card is NOT

The FT63 era-1/era-2 root cause. The break epochs have clean, full, above-mask ingest — GF-blind scans
(per-observable BSD-TD2, phase-minus-code steps, receiver-common steps, detrended fine steps) exonerate every
class at the break epochs. Era-2's injector was identified the same day by the reduced-RHS class-attribution
instrument (`network.rhs_attrib_tow`) as an internal mechanism (`project_ft63_fast_tier`).

## 4. Design

One guard at the single v3 value-parse site (`RinexParser.cpp:685`, after `stod`):

```cpp
if (value == 0.0 && config_.rinexSkipZeroValues()) { ++zero_sentinels_skipped_; continue; }
```

- Toggle `processing.rinex_skip_zero_values` (default **false** = legacy), following the config-toggle
  convention: private member + Doxygen getter `rinexSkipZeroValues()` + loader line + arming info line.
- Counter logged at parse end per file when nonzero (invariant #4) — observable, never silent.
- Applies to every observation letter (C/L/D/S), matching the 2.x branch exactly. True zero crossings are not
  representable data (writers offset or blank them; a phase tracking through exactly 0.000 cycles at F14.3 is
  a measure-zero event the standard already sacrifices).

## 5. Validation

- Unit suite `tests/test_rinex_zero_sentinel.cpp` (suite `RinexZeroSentinelTest`, one-suite-per-file rule):
  legacy path ingests the zero (defect record); armed path drops it, keeps the valid same-band partner and all
  real values bit-exact.
- OFF-path byte-identity is structural (the guard is inside `if (armed)`).
- Golden A/B before any default flip; the default stays `false` per shared-path discipline and arming goes
  into new configs alongside the standard armed set. Promotion-ledger entry: candidate default-flip after one
  clean same-build golden comparison on a GOPE/GOP6-bearing set — the only stations where the bit-stream
  differs at all.

## 6. Deferred / adjacent (pre-declared)

- **Band-loss policy inconsistency** (flicker card INC-0: MBAR/G21 LOS-dropped vs BSHM/G07 L1-only-processed)
  — separate card, unchanged here.
- Consumer-side RTCM/SBF decoders untouched (MSM fields carry their own validity flags; the SBF path has its
  own decode).
- A "zero-with-valid-partner above mask" one-shot WARN (detection without the guard) was considered and
  dropped — the counter + info line under the armed guard covers observability, and unarmed legacy runs stay
  bit-silent by design.

---

## 7. The RETRACTED structural-ingest guard (absorbed from `SPEC_ingest_structural_guard.md`)

⛔ **RETRACTED 2026-08-01 before implementation — DO NOT BUILD.** Nothing in §7 is live code; no increment may
be revived without a *new* root cause and a census cross-checked against producer counters. Original title:
*"structural-ingest guard (mass-dropout census + single-frequency demotion + arc re-vet)"*; increments
**INC-A** (census), **INC-B** (single-frequency demotion), **INC-C** (field protection through the anomaly),
**INC-D** (per-arc re-vet quarantine).

### 7.1 The retraction, and the method lesson that is the doc's whole live function

The motivating "mass-dropout" was an **artifact of the investigating session's ad-hoc RINEX census**: it
string-matched space-padded epoch seconds (`19 26 0`) and silently missed every **zero-padding** writer (`19
26 00` — e.g. POTS, with a full 60-sat epoch present) at seconds=00 epochs *only*. The producer's own ingest
(`n_pf_rows` 3332 → 3328 across the break) shows **NO row loss** — the solve had full data at both engagement
epochs.

> **Standing rule: cross-check any census against producer counters, never hand-rolled
> parsing.**

### 7.2 The (now-VOID) census table — kept labelled, not deleted

**VOID — this table is the artifact, not evidence.** Retained only so a future session does not re-derive it
and re-reach the same wrong conclusion. G rows / rows-with-L2W; stations-to-zero-L2W:

| epoch | before → during → after | L2W | zero-L2W stations |
|---|---|---|---|
| era-1 07:32:00 (TOW 286320) | 655 → **364** → 646 | 616 → 345 → 616 | **26** |
| era-2 19:26:00 (TOW 329160) | 690 → **404** → 689 | 652 → 377 → 649 | **25+** |

The apparent drop was globally distributed (Alaska / Brazil / Japan / Svalbard / Antarctica / Euro cluster),
which is what made "common cause upstream of every receiver" look compelling. It was the zero-padding blind
spot.

### 7.3 Five FALSIFIED arms — pure do-not-retry content

All from `snap_20`, ~5 min/leg; a00 peak at the era-2 break is the discriminator. The break was invariant to
everything except the (illusory) shared 40 % row loss:

| arm | how it died (a00 peak) |
|---|---|
| eq34 cold-reattach is NOT causal | hygiene card armed ⇒ break unchanged, a00 5741 vs 5739 |
| deweight lag is not it | `rate_limit 1.0`, 9 LOS at ZERO weight in-epoch ⇒ a00 5738.3 |
| code garbage is not it | raw code GF ranges ≤ 2.6 m at the worst stations |
| satellite-common is not it | ΔGF movers are station-specific |
| unvetted-arc trigger is not it | G05/G21/G22/G29 excluded network-wide ⇒ a00 5725.8 |

### 7.4 The era-2 damage chain (mechanism sketch; its TRIGGER was misattributed) and the era baselines

The chain downstream of the (whatever-caused-it) epoch remains a useful mechanism sketch: the epoch loses **GF
iono anchoring network-wide while the IF-pin CB gauge rows hold**, so the solve slides along the W6
iono↔CB↔clock quasi-null ⇒ **a00 +5.7k TECU one-epoch step** + km-scale PB/CLK discharge along pre-existing
soft PB directions (NEQ-SVD: **σ_min UNCHANGED, RHS loading along weak-3 swings 83×**) ⇒ 48/76-min gate eras.
Era-2's **exit scar** (181 m between-sat total_IF re-shuffle, never heals) = the stiff CB carries (q 1e-10, τ
≈ 3.8 h) **latching the slid level**.

Era baselines to beat, era-2 unless noted: engage at 19:26:00; a00 peak 5739 / 5741 / 5738 / 5726 across arms;
**83 out-of-band epochs** in the 1 h window; across-gap total_IF re-shuffle **181 m** (era-1: 55.8 m, healed);
postfit phase σ₀ **2.95 → 56.7** at the break; exit-scar excursion 20:42→24:00 worst ~100 m (G07); QC panel
rx-CB withholds ~14.5 %, σ̂₀ 1.00, raw-erases 1308 over 24 h.

### 7.5 The guard gap, which remains real regardless of the retraction

**No existing mechanism censuses the ingest.** RAIM tiers are residual-scoped (T2/T3/T4 structurally inert on
this lineage), the iono deweight is pin-scoped, clock-runaway is magnitude-degenerate single-system, and the
SH gate is emission-scoped and fires only AFTER the damage.

### 7.6 The two increments the referrers flag as revivable (design only — NOT built)

Both would live under a nested `network.ingest_guard.*` block (the `cycle_slip` / `mw_wl` nesting precedent),
default-OFF, structurally byte-identical when off. Doctrine: adapt-and-EMIT (respond in-epoch with weights;
withhold stays the SH gate's job) and prefer-fallback (R inflation, not hard exclusion).

**INC-A — census-as-observability (anomaly-declaration arithmetic).** Once per epoch, SERIAL, before PASS-B
row consumption: count per constellation the total obs rows and rows per band-class (L1-only vs
dual-frequency-capable per arc; an arc is "dual" this epoch if BOTH bands of its station's preferred pair are
present). Reference = per-quantity **MEDIAN over the previous `ref_window_ep` (default 5) NON-anomalous
epochs**, window **LATCHED** so an anomaly epoch cannot poison its own reference. Declare ANOMALY when
`rows_now < (1 − anomaly_drop_frac) · median` (default `anomaly_drop_frac: 0.20`) **OR** `dual_now < (1 −
anomaly_drop_frac) · median_dual`, with `min_rows` (default 100) suppressing small-network false fires; exit
hysteresis `exit_frac` default 0.10. Events `INGEST/anomaly_enter` / `INGEST/anomaly_exit`; per-epoch census
row into `logs/network_ingest_census.csv` (week,tow,const,rows,dual,l1only,median_ref,anomaly).

**INC-D — per-arc re-vet quarantine (the standing hazard, user-specified).** `arc_revet_ep` (default 0 = off;
typical 3). **ANY arc transition** — re-entry after ≥ 1 absent epoch, OR band loss/regain (dual → L1-only →
dual) — starts a vet counter: until the arc completes `arc_revet_ep` CONSECUTIVE epochs with its full band
set, its rows fold at σ × `arc_revet_sigma_scale` (default 10.0). The counter **resets on any slip-funnel
hit** for that arc (`carry_slipped_ambs` ∪ `isAmbiguityPendingReseed`). `arc_revet_sigma_scale ≤ 0` means hard
withhold — available but **not** recommended (prefer-fallback). **First sightings are NOT transitions** — only
re-entry / band-change vet — so a cold start is not blanket-quarantined.

**INC-B / INC-C are the in-epoch response layer** and are not flagged revivable: INC-B demoted an arc that was
dual last epoch and is L1-only this epoch (σ × `sf_demote_sigma_scale`, default 10.0, phase AND code); INC-C
protected the field through the anomaly (SH pin rows at σ × `field_protect_sigma_scale` default 10.0,
`field_protect_tail_ep` default 2, skip the T5v2 deviation harvest, do not advance emit-side PB/CB
discontinuity baselines). INC-B/C required INC-A; INC-D was independent.

**Where it would have hooked** (design-only anchors, all drifted since — prefer the SYMBOL): INC-C's SH pin
rows in `appendShIonoRows` (the **INC-7** helper in `ApplicationNetworkAssembly.cpp`, which takes the
iono_deweight params — the anomaly scale composes there), the T5v2 deviation harvest in
`app/NetworkRunContext.cpp` (ctx lifecycle "harvest", skip-flag from the census verdict), trace streams
registered in `logging/NetworkRunTraces` (NOT the driver), events via `emitEvent` tier `"INGEST"`, counters in
`NetworkServiceCounters` (the retracted card cited `NetworkRunContext.hpp:623`; the struct now lives at
`NetworkRunContext.hpp:1662`) — anomaly_epochs, anomaly_max_drop_frac, sf_demotions, field_protect_epochs,
harvest_skips, revet_transitions, revet_holds, revet_slip_resets — with the run-end block in
`filter/NetworkTeardown.cpp` beside the adaptive-iono-q and hygiene blocks, carrying the invariant-#4
dead-wiring WARN (armed + zero census passes = wiring never ran). Config: nested parse in
`GnssConfigLoadNetwork.cpp` near sh_health_gate/adaptive_iono_q/hygiene, `IngestGuardConfig` POD in
`GnssConfigTypes.hpp` (nested-struct precedent `mw_wl`).

**Fingerprint decision and its precedent chain**: a forward-only weight/census policy is **NOT** appended to
the snapshot fingerprint, because stored-state meaning is unchanged — the **third precedent** after
`adaptive_iono_q` and eq34 hygiene, both recorded at the conditional-append site in
`networkSnapshotFingerprint` (the retracted card cited `NetworkRunContext.cpp:134-260`; the function now
starts at `NetworkRunContext.cpp:460`). That is what keeps shared-snapshot A/B legal.

### 7.7 The FT63 replay-harness recipe (reusable verbatim for any FT63-era replay)

- Seed = copies of `runs/ft63_24h_allon/snap_archive/` (archive path — see the header).
- Decode the generation from the text `M,<week>,<tow>` line — numbering drifts ~30 s/write.
- `state_snapshot_require: true`.
- **Era-2 legs**: `snap_20` (gen 19:06:30) + `start_offset_hours: 19.1125`, dur 3600.
- **Era-1 legs**: `snap_08` (gen 07:02) + offset ≈ 7.0375 (07:02:15), dur 3600.
- **NEQ probe**: emitted-count target (2236 pre-break / 2237 break) **plus** an `sh_health_gate: false` probe
  leg for the break epoch, because the gate is emission-only. Gate-E baseline the probe was to beat: Δx PB
  steps collapse to O(σ) against a baseline PB:G24:L5Q of −5470 (etc.), with weak-3 RHS loading back to
  O(±33).
- Exe: fresh **Release** build (CLAUDE.md §Build / `scripts/vs2026_build.ps1`), run as a renamed copy.
- Reference OFF-identity leg: `runs/ft63_leg19_off` — a00 5738.6 at TOW 329160.

### 7.8 Who owns the reopened question now

The retraction **REOPENS the era-1 / era-2 root cause**. Ownership passed to
`docs/SPEC_measurement_admission.md`, which says so in its own header. Era-2's injector was subsequently
identified by `network.rhs_attrib_tow` as an internal mechanism — see `project_ft63_fast_tier` and §3.

### 7.9 Deferral triage carried over from the retracted card

- **Real scintillation / EPB response** (gradual, sectoral, hours-long) — OUT of scope: that census was a
  **step** detector, and the deweight/adaptive-q lineage owns gradual iono stress. Field-inert for this card's
  verdict (both events are steps).
- **Multi-constellation thresholds deferred** — counters are per-constellation by design but GPS-only
  validated (the FT63 substrate).
- **Sub-network regional dropouts below the global threshold** are accepted as invisible: that class lacks
  the network-wide gauge-vs-data tension, and single stations going dark are already handled by presence
  pruning.
- Provenance of the DOY-129 events (flex-power attribution) is diagnostics-only commentary; nothing in a
  guard may depend on external schedules (no-external-data directive).
