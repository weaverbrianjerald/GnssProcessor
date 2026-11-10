# SPEC — flicker/gap signal integrity (band-state classifier + degraded-phase demotion + re-vet)

**Status**: SPEC'd 2026-08-01 (FT63 session); **BUILT 2026-08-01** (§8). Field validation (§6) still
OPEN and belongs to the FT63 session (case-study arc + 24h counters on the replay harness). INC-0's code
audit and a first census are in §8; both ANSWER the §1 behaviour question and one contradicts a §2 belief.
**Memory context**: `project_ft63_fast_tier` (G21@MBAR microscope + falsification record),
`docs/SPEC_eq34_reattach_hygiene.md` §8 (ctx-map/telemetry patterns copied),
`feedback_prefer_fallback_over_drop` (R-inflation over exclusion), `feedback_reset_over_repair_biasless`
(refloat, never repair, for bias-less signals), `feedback_slip_detectors_by_rate` (GF/TurboEdit are
1s-RT only — nothing here may consume GF as a slip decision at 30s).
**Scope discipline**: default-OFF under `network.signal_integrity.*`; OFF structurally byte-identical;
NOT in the warm-resume fingerprint (forward-only weight/refloat policy — third precedent). **This card
does NOT claim the DOY-129 era breaks** — `runs/ft63_leg19_ablate4` excluded the flickering arcs and the
era-2 break was unchanged; the eras are a separate open investigation.
**⚠ Provenance**: every `runs/<name>` path here — `ft63_leg19_ablate4`, `ft63_leg19_off`, `snap_20` legs
— is absent from the working tree; `runs/` was archived to `D:\GnssProcessor_archive\2026-08-12\runs\` on
2026-08-12 (CLAUDE.md) and the paths resolve there.
**⚠ Anchor-drift map** (code re-grepped at HEAD 2026-08-15; the 2026-08-01 text cited pre-drift lines):

| cited 2026-08-01 | live at HEAD | what |
|---|---|---|
| `NetworkRunContext.cpp:134-260` | `:460` | `networkSnapshotFingerprint` (exemption recorded at its end) |
| `MeasurementModel.cpp:718` | `:748` (inside `ensureRequiredStates`, `:291`) | `pruneAmbiguitiesExcept` |
| `MeasurementModel.cpp:1824` / `MeasurementModel.cpp:1825` / `MeasurementModel.cpp:1793-1822` | `:1855` (per-signal `isUsableMeasurement` skip), `:1802` | row assembly over `obs.observed`; the satellite-scoped observation guards |
| `MeasurementModel.cpp:2096-2100` | `:1816-1817` reserve, `:2105-2106` resize | `satellite_ids` / `signal_types` row-alignment invariant |
| `ApplicationNetworkAssembly.cpp:218` | `:219` (`w = em.R_v.cwiseInverse()`); INC-2 block `:220`, map read `:248-258` | the fold point the σ-map is consulted at |
| `NetworkNormalEquations.cpp:277` | `:446-476` (`d_ref` / pin-aware scale `:467-473`) | the per-station PD guard |
| `NetworkPhaseDatum.cpp:118-122` | `:114-124` | `usable_keys` derived from the phase-bias STATES |
| `NetworkSlipQc.cpp:151-153` | unchanged (re-verified) | the MW/LLI drain's `getAmbiguities()` match |

---

## 1. Problem — the epistemics and the measured case

**Across any loss of lock the slip magnitude is unbounded and unknowable** (the phase counter is
arbitrary at re-acquisition; a proportional L1/L2 counter reset is invisible to GF AND MW). Recovery is
safe only if every path treats re-entry as code-anchored-fresh (error bounded by code noise ≈ tens of
cycles), and if the first post-recovery epochs — where no TD detector can operate — never carry full
phase weight. The dangerous window is structural: MW needs Welford history, IF-TD needs 3 consecutive
epochs, LLI is receiver-discretionary, GF is doctrine-barred at 30 s.

**Measured case (G21 @ MBAR00UGA, JAVAD TRE_3 DELTA, DOY-129 19:21-19:31, raw RINEX):**

| time | event | GF step | receiver flag |
|---|---|---|---|
| 19:22:00 | mid-arc slip | −1.42 m ≈ −6 L2W cyc | **none** |
| 19:25:00 | L2W missing (L1C present, healthy S1C) | LOS dropped by producer | — |
| 19:25:30 | re-entry | −1.44 m vs pre-gap | LLI=1 on BOTH bands |
| 19:26:00 | post-re-entry slip | −0.99 m ≈ +4 L2W cyc (−0.977 predicted, 1 cm) | **none** |

A weak semi-codeless L2W loop (S2W 28-39 dB) slipping near-integer counts repeatedly, receiver-flagged
ONCE. Both unflagged slips fell in windows where MW/IF were structurally blind (arc 0-1 epochs old).
Scale of the class: card-G counted **2482 (station,sat) re-entries in one 24h at 63 stations**
(~1.7/epoch) — constant weather, not a corner case.

**Behavior inconsistency to resolve (INC-0)**: the producer dropped the MBAR/G21 LOS for its L2-missing
epoch (both signal arcs re-entered with gap_ep=1), yet BSHM/G07 ran as a standing L1-only arc with live
code rows (the 39 km postfit cell). Whether a DUAL arc that degrades mid-arc ever CONTINUES
single-frequency (worst case: an unmonitorable phase at full weight with no gap registered) is unaudited.

## 2. Design — four increments

### INC-0 — behavior census (diagnostics only; land first)
Instrument, don't assume: counters for (a) LOS-dropped-on-band-loss epochs, (b) DEGRADED-continuation
epochs (a previously-dual arc contributing with fewer bands, if the audit finds the path exists), (c)
flicker events by outage length (1 ep / 2-5 / >5), (d) standing single-frequency arcs (born single, like
BSHM/G07). Locate the actual band-selection/drop policy and record it in §8. Acceptance: the counters
reproduce card-G's re-entry totals as a cross-check. *(ANSWERED — §8.1; census in §8.3.)*

### INC-1 — band-state classifier
Ctx-side per-(station,sat) presence/band history (eq34-hygiene age-map pattern; serial drain after PASS
A). States: STABLE (≥K consecutive epochs with the arc's full band set), REENTRY (absent ≥1 epoch, now
present), FLICKER-REGAIN (band lost then regained within ≤`flicker_max_ep`, LOS possibly never absent),
DEGRADED (dual-born arc currently contributing with fewer bands). Feeds INC-2/3 and emits SIGNAL-tier
events (`reentry`, `band_lost`, `band_regain`, `degraded_epoch`) with per-class counters. *(As built:
§8.2; the seam runs in the DRIVER, not the serial drain — §8.4 deviation 1.)*

### INC-2 — degraded/unvetted phase demotion (prefer-fallback)
While an arc is not STABLE its **phase rows** fold at σ × `unvetted_sigma_scale` (default 10.0; ≤0 =
withhold the phase rows — available, not default). **Code rows untouched** (slip-immune; they re-anchor
the float). Mechanism: the per-station per-sat scale-map consulted at fold time (the hygiene card's
`pinned_amb_sigma_scale` pattern; station-local, parallel-safe). The user requirement in one line: *an
unmonitorable phase never carries full weight.*

### INC-3 — regain refloat + re-vet
(a) On FLICKER-REGAIN (band returns without a producer-registered gap): **refloat the LOS's ambiguities
on ALL bands** by inserting the keys into the existing slip funnel (`carry_slipped_ambs` insertion at the
PASS-A/serial-drain boundary — the exact path MW/LLI/IF-TD detections take; reuse, do not invent a
parallel reset). Rationale: reset-over-repair; the G21 case proves flicker windows slip BOTH bands (L1
LLI at re-entry) and nothing can bound the sizes. True REENTRY already refloats via the funnel (card-G
verified 0 bridged) — no change, but assert it with a counter.
(b) Re-vet: STABLE requires `revet_ep` (default 3) consecutive full-band epochs after ANY transition;
until then INC-2's demotion applies. K=3 is chosen so MW (needs 2 TD samples) and IF-TD (needs 3 epochs)
are BOTH live for at least one epoch before the phase returns to full weight — the vet ends exactly when
monitoring begins.
(c) LLI at re-entry: audit that an LLI=1 on a re-entry epoch reaches the refloat drain even when the MW
arc is immature (believed yes — "LLI handled directly in the serial drain"); add the counter either way.

## 3. Knobs (all default-OFF; nested block)

`network.signal_integrity.{census, unvetted_sigma_scale (10.0), revet_ep (3), flicker_max_ep (5),
refloat_on_regain (true when block armed), withhold_instead (false)}` — INC-2/3 require `census`
(INERT-guard warn+disable otherwise). Loader section next to the hygiene/adaptive-iono blocks in
`GnssConfigLoadNetwork.cpp` (~:2027-2160; the as-built block is at `:5090`); `SignalIntegrityConfig` POD
in `GnssConfigTypes.hpp` (`:427` / `:912`, nested-struct precedent); arming info lines state the doctrine
(fallback-not-drop, reset-not-repair).

## 4. Wiring sites (as spec'd; **§8.2 is the as-built map and differs — see §8.4 deviation 1**)

Classifier + state map in the serial drain after PASS A (`ApplicationNetworkAssembly.cpp`, T5v2-aging
neighborhood), presence/bands from `modeled[i]` (per-station `PppObservation`s), map in `ctx.carry`;
INC-3a funnel insertion at the same serial point, upstream of the funnel consumers (the T5v2 erase reads
`carry_slipped_ambs`); INC-2 row demotion via a NetStation per-sat sigma-scale map consulted at the
PASS-B fold (hygiene `pinned_amb_sigma_scale` precedent); counters in `NetworkServiceCounters`
(aiq_/hyg_ style) with a run-end block in `NetworkTeardown.cpp` (`:810`) carrying the invariant-#4
dead-wiring warn, events via `emitEvent`; fingerprint exemption recorded at the
`networkSnapshotFingerprint` conditional-append site. Sizing estimate at spec time was ~100 lines,
suggesting inline placement in the drain — **superseded**: it landed as its own seam (§8.2), so the
"new seam TU + manual `CMakeLists.txt` entry only if the classifier outgrows the drain" caveat fired.

## 5. Unit tests

`tests/test_signal_integrity.cpp` (one suite). Classifier transitions (stable→flicker→regain→vet→stable;
reentry path; degraded path if it exists); demotion map only while unvetted; funnel insertion on regain
(keys land in `carry_slipped_ambs`); re-vet completion at exactly K; LLI-at-reentry counter; OFF ⇒ no
map, no scaling, byte-identical.

## 6. Field validation (FT63 session)

Replay mechanics as standing (snap_20 legs, `state_snapshot_require`, offsets; exe = fresh renamed
Release build; new same-build OFF leg as control).
| gate | content |
|---|---|
| 1 | A. Smoke: arming lines, counters live, OFF twin byte-identical. |
| 2 | B. OFF-identity leg vs `runs/ft63_leg19_off` (a00 5738.6 signature at 329160). |
| 3 | C. INC-0 census leg (24h or 3h): counters coherent with card-G (≈2482 re-entries/24h class); DEGRADED-continuation count reported (the audit answer). |
| 4 | D. **Case-study gate (armed leg from snap_20): the (MBAR,G21) arc** — regain at 19:25:30 must produce a refloat event + funnel insertion, demoted phase through 19:27:00 (K=3), and the unflagged +4-cycle slip at 19:26:00 landing in a demoted-weight window (grade: MBAR/G21 iono/postfit locally clean vs the OFF leg's poisoned window). Era behavior reported but NOT gated (eras unowned by this card). |
| 5 | E. QC panel flat vs OFF leg (D5 ±few %, rx-CB withholds ~14-15%, σ̂₀ 1.00, no refloat storm beyond the intended regain refloats; total_IF hourly panels no regression — the demotion must not measurably starve the solve: 2482 re-entries/day × K=3 epochs × phase-only ≈ 0.1% of phase rows demoted). |
| 6 | F. 24h armed run: full-day counters, no drift in quiescent verdict vs the 24h baseline outside the (unowned) eras. |

## 7. Deferral triage (pre-declared)

| # | deferral |
|---|---|
| 1 | The DOY-129 era root cause — separate investigation (in-assembly RHS class attribution instrument; see `project_ft63_fast_tier`). If this card's arms move the eras, REPORT, don't claim. |
| 2 | Era-1's receiver-cohort phase-spike lead (ALLOY/JAVAD ±20-40 m one-epoch spikes at 07:32:00) — related weather, different mechanism class (value spikes on CONTINUOUS arcs); a robust-R/spike-clip response is its own card if pursued. |
| 3 | 1 s RT tuning (K, flicker_max in RT units; TurboEdit interplay) — post-processing 30 s first. |
| 4 | Standing single-frequency arcs (born L1-only, e.g. BSHM/G07): INC-2 as spec'd vets TRANSITIONS; whether standing SF arcs' phase should be permanently demoted is a policy question for after INC-0's census quantifies them. |

## 8. Implementation record (BUILT 2026-08-01, a separate session)

Debug + Release both build clean; unit suite **1027 passed / 1 skipped** (the standing fixture-gated
skip), including the new 11-case `NetworkSignalIntegrity` suite. A 14-epoch Release smoke over the
measured window (19:20:45-19:28:00, DOY-129, the FT63 63-station set; scratchpad config off
`_ft63_hyg_smoke.json` with the snapshot channel and product path removed so nothing under `runs/` was
touched) shows the wiring **LIVE, not inert** — numbers in §8.3. Field gates (§6) are UNRUN and belong to
the FT63 session. **The ADDENDUM below was written while this was in progress and IS built** — D4/D5.

### 8.1 INC-0's answer: the band-selection policy, read from the code

| INC-0 answer |
|---|
| **A dual-born arc that loses a band mid-arc CONTINUES contributing, at full weight, with no gap registered and no event emitted. There is no dual-frequency requirement anywhere in the uncombined (network) modelling chain.** The decision is per-BAND and per-SIGNAL, never per-arc: |

| # | evidence |
|---|---|
| 1 | `PppModel::selectUncombinedSignals` (`src/core/model/PppModel.cpp:1006-1024`) tests each allowed band's priority-best code and priority-best phase **independently** against this epoch's measurements (`found_code` / `found_phase`, no `else`, no pairing, no rollback) and returns `!selected_signals.empty()`. The in-code policy statement is at `:904-905`: *"When the canonical is not observed this epoch, `selectUncombinedSignals` simply skips the band for this sat this epoch."* |
| 2 | Two further facts make a band-loss-driven LOS drop structurally impossible rather than merely unobserved: `allowed_bands` is measurement-INDEPENDENT (`:889` → `activeFrequencyBands`, `:148-160` returns the static band-map list), and the canonical (band,type)→signal map is RUN-LATCHED (`decideConstellationCanonicals` `:428`, early-out `:441`, latch `:493`), so `freq_bands` still contains band 2 after L2W vanishes. |
| 3 | `selectSignals` computes `comb_type` = single/dual/triple (`:1305`) then **discards it** on the uncombined path (`comb_type = "uncombined"` at `:1313`; `is_iono_free` forced false at `:1304`). Nothing downstream tests band count. |
| 4 | `processUncombinedObservation` is a flat per-signal loop with `bool success = false` (`:1699`) / `success = true` per accepted signal (`:1807`) / `return success` (`:1810`) — ONE surviving signal, even a lone code, is enough; `computeModels` admits the satellite on `!ppp_obs.modeled.empty()` (`:738`). |
| 5 | Row assembly enumerates `obs.observed` per signal with a per-signal `isUsableMeasurement` skip (`MeasurementModel.cpp:1855`, also `:1802`); the observation-level guards there are all SATELLITE-scoped (sat_pos, range, elevation mask, los, mf_w). |
| 6 | The ONLY band-related whole-LOS drop is `code_obs.empty()` (`ModelGeometry.cpp:254`) — **every** code gone. Separately, `broadcast_only` (`PppModel.cpp:709-723`) strips every `L*` key per-satellite when there is no precise/SSR clock: a code-only continuation, unrelated to band flicker. |


So §1's "behaviour inconsistency (INC-0)" resolves as: the DEGRADED-continuation path **exists and is the
default**; the MBAR/G21 LOS drop at 19:25:00 required something stronger than losing L2W (every code
gone, or a satellite-level guard failing). The smoke confirms BOTH classes occur — §8.3.

**Third structural blind window, not in §1's table.** A band flicker also blinds the raw STEP monitor for
two epochs, and nothing counted it before this card: `networkQualifyRawObservables` requires
`pit->second.obs.dual && r.dual` (`NetworkRawObservables.cpp:176-177`) to form ΔMW/ΔGF at all, so the
degraded epoch AND the epoch after it come back `measured = false`. Worse, a single-band epoch still
advances `h.prev` (`:289-293`), so **no gap is registered** — Card G cannot see a band-only loss at all.
That is why the flicker case needed its own instrument.

**Census-comparability caveat for §6C.** Card G reads `raw_observed` (the full RINEX superset,
`PppObservation.hpp:47`); this census reads `observed` (the SELECTED set that becomes rows, stripped at
`PppModel.cpp:642-651`). A band present in RINEX but excluded by `signal_mode` / `signalAdmitted` / the
canonical lock counts as "dual" to Card G while contributing NO rows. **The BAND counts are therefore NOT
comparable to Card G's; the LOS re-entry counts are** (both measure presence in the modelled set). Apply
§6C's "reproduce card-G's re-entry totals" to `reentries` only.

**Re-entry is code-anchored-fresh AT THE STATE LEVEL by construction.** `ensureRequiredStates` calls
`pruneAmbiguitiesExcept` (`MeasurementModel.cpp:748`) unconditionally with a keep-set built from this
epoch's `observed` phase keys — no grace epoch — so an absent arc's ambiguity state is gone before it
returns. Measured: `reentry_amb_state_live` = 0 over 5 re-entries. What CAN bridge an outage is the
phase-leveling / invariant-DD **carry**, one level up; that is Card G's instrument and this card does not
duplicate it.

### 8.2 Where it lives

| # | where |
|---|---|
| 1 | **New seam** `filter/NetworkSignalIntegrity.{hpp,cpp}` (+ CMake, header banner at `NetworkSignalIntegrity.hpp:5`): the classifier, the census, the σ-map publication, the WITHHOLD disposition and the regain refloat. Called from `ApplicationNetwork.cpp:1556` immediately after `networkAssemblyPrefilter_` — one line in the driver, per the seam contract. |
| 2 | **Row demotion**: `NetStation::phase_sigma_scale` (keyed by SATELLITE, `NetworkRunTypes.hpp:183`) consulted in `networkSplitStation_` right where `w = em.R_v.cwiseInverse()` is formed (`ApplicationNetworkAssembly.cpp:219`; INC-2 block `:220`, map read `:248-258`). `em.satellite_ids` / `em.signal_types` are row-aligned with H/R_v by an asserted invariant (`MeasurementModel.cpp:1816-1817`, `:2105-2106`), so phase-vs-code is `sig[0] == 'L'` — O(1), no matrix scan. Empty map ⇒ zero extra floating-point operations. Per-station `si_rows_demoted` because the fold is inside the parallel produce. |
| 3 | **Carried state**: `NetworkCarryState::si_arc` (per (station, satellite) `SignalIntegrityArc`) + `si_resume_warmup_ep` (`NetworkRunContext.hpp:278` / `:719` / `:1803`). |
| 4 | **Telemetry**: `NetworkSignalIntegrityTallies` in `ctx.counters.sig`; run-end block in `NetworkTeardown.cpp:810` (five lines incl. an explicit AUDIT line); SIGNAL-tier events `reentry`, `band_lost`, `band_regain`. |
| 5 | **Fingerprint**: confirmed NOT appended, exemption recorded in-place at the end of `networkSnapshotFingerprint` (`NetworkRunContext.cpp:460`) — the THIRD instance of that precedent, and what keeps the §6B/§6D shared-snapshot A/B legal. |


### 8.3 First measured census (14-epoch Release smoke, 63 stations, DOY-129 19:20:45-19:28:00)

| quantity | value |
|---|---|
| classifier passes / LOS-epochs | 14 / 7799 |
| first sightings | 576 |
| re-entries (gap 1 / 2-5 / >5) | 5 (4 / 0 / 1) |
| LOS disappearances, of which DUAL | 21, of which **16** |
| band losses with the LOS still present | 7 |
| …regained inside the window (FLICKER) | **2** |
| …reference set reduced as permanently gone | 2 |
| **`sf_continuation_epochs` (the AUDIT answer)** | **17** |
| standing single-frequency arcs / their LOS-epochs | 11 / 122 |
| code-only LOS-epochs | 40 |
| unvetted LOS-epochs / birth-EXEMPT / resume-warmup | 2308 / **2295** / 0 |
| deweighted LOS-epochs → PHASE rows demoted | 13 → **16** |
| flicker-regain refloats → funnel insertions | 1 → 2 |
| re-entry premise: already refloated vs NOT | **0 vs 4** (the 5th returned CODE-ONLY, so the premise does not apply) |
| LLI on a re-entry epoch → reached a funnel | **2 → 0** |

Readings, in order of how much they change the card:

| # | reading |
|---|---|
| 1 | **BOTH INC-0 classes are real.** 17 single-frequency continuation LOS-epochs of ever-dual arcs *and* 16 dual-arc disappearances, in 14 epochs. §1's "behaviour inconsistency" was not an inconsistency, it was two different mechanisms. |
| 2 | **The measured case reproduces exactly.** `network_events.csv` carries `SIGNAL,reentry,G21,MBAR00UGA,gap_ep=1;bands=2` at TOW **329130 = 19:25:30** — the spec's own re-entry epoch and its own `gap_ep=1`. With `revet_ep = 3` the demoted window is 329130-329220, so the unflagged +4-cycle slip at **19:26:00 (329160)** lands inside it, which is §6D's gate. The same station also shows a textbook FLICKER-REGAIN — `band_regain,G30,MBAR00UGA,L1W;bands=3,refloat` at 328920. |
| 3 | **INC-3c's suspicion is CONFIRMED, not confirmed-negative.** §2 said "believed yes"; the measurement says **2 of 2 LLI assertions raised on a re-entry epoch never reached a refloat funnel**. Cause: the MW/LLI drain matches an assertion against `StateManager::getAmbiguities()` (`src/core/slip/NetworkSlipQc.cpp:151-153`, re-verified 2026-08-15) and the re-entering arc's ambiguity has just been pruned, so `matched` stays false — while any carried invariant-DD chord for that arc (keyed independently of the StateManager) is NOT re-tapped. This card MEASURES the hole and deliberately does not close it: the detector-side fix is **owed by `docs/SPEC_slip_qc_hardening.md` §11 item 4**. |
| 4 | **§2's re-entry premise is falsified at this sample**: 4 of the 4 re-entries the premise applies to were not already refloated by a PASS-A detector (the 5th returned code-only — `SIGNAL,reentry,G21,BSHM00ISR,gap_ep=1;bands=0` — and has no ambiguity to refloat, so it is excluded rather than counted against the premise). Per §2 ("no change, but assert it with a counter") the card does not act on that, but the field report must carry the number; and at this rate §2's parenthetical "(card-G verified 0 bridged)" is about the CARRY, not the funnel, and must not be read as covering it. |
| 5 | **The demotion volume is negligible, because of the addendum**: 2295 of 2308 unvetted LOS-epochs are birth-exempt, leaving 13 LOS-epochs / **16 phase rows** demoted. §6E's "must not starve the solve" budget is met by orders of magnitude. Before the addendum's exemption the same run demoted 2308 LOS-epochs / 5798 rows — the cold-start blanket-vet the addendum forbids was ~99.7 % of the volume and ~0.3 % of the value. |

### 8.4 Deviations and extensions the spec did not state (all deliberate, all tested)

| # | deviation |
|---|---|
| 1 | **The seam runs in the DRIVER, right after `networkAssemblyPrefilter_`** — not in "the serial drain after PASS A" (§4), which runs AFTER the anchor election, so a seam there could not (a) put its refloats into the funnel before `networkAnchorHygiene` reads it, nor (b) prune a withheld key's phase-bias state before the election seats a pin on it. Same hole the eq34 hygiene card records as its deviation #1. The placement is one point wide: after the funnel union and the observability prune, before the candidate census. |
| 2 | **`withhold_instead` ERASES the LOS's phase observables; it does NOT zero the row weight.** A zero-weight row keeps its live H column, so the ambiguity column still enters `A_r` carrying no information and trips the per-station PD guard (`NetworkNormalEquations.cpp:446-476`), whose catch drops the **whole station** for the epoch — a per-arc demotion converted into the hard exclusion `feedback_prefer_fallback_over_drop` forbids. (Under Card-1 coupling the coupling rows may touch that column and rescue it, making the failure *config-dependent* and therefore worse.) Erasing the observable makes the column all-zero, which the existing dead-column scan drops cleanly. `raw_observed` is left alone — it belongs to the slip detectors. |
| 3 | **A withhold also prunes that station's satellite phase-bias states for the withheld signals.** `networkBuildPhaseDatumCandidates` derives `usable_keys` from the phase-bias STATES (`NetworkPhaseDatum.cpp:114-124`), not from the observables, so without the prune the election could seat a datum pin at a station whose supporting row this seam had just removed — the pin would silently fail to fold and the gauge would lose a constraint. |
| 4 | **The demotion is gated on the arc having CONVERGED (`ever_stable`)** — the ADDENDUM's cold-start exemption as a property of the ARC, not of the run. A never-converged arc has a diffuse ambiguity and no carry chord, so its own priors already are the demotion. Per-arc makes cold start exempt by PRINCIPLE while still exempting a genuinely rising satellite mid-run, `birth_exempt_los_epochs` making the exemption visible. |
| 5 | **RESUME WARMUP is a carried COUNTDOWN (`si_resume_warmup_ep`), not a state**, set by `networkTryWarmResume`, decremented per pass; while positive it OVERRIDES the per-arc verdict for every present LOS. It must be arc-independent: a resumed run starts with an empty band map, so every arc looks like a first sighting and deviation 4 would exempt exactly the situation the addendum says must be vetted. Knob `network.signal_integrity.resume_warmup_vet` (default true when armed). |
| 6 | **The demotion map is keyed by SATELLITE (per LOS), not per (satellite, signal).** The measured G21 defect is L2W-specific, which argues the other way — but across a loss of lock every band's counter is arbitrary at once (that same re-entry carries LLI on BOTH bands) and INC-3a already refloats all bands. Per-LOS also costs zero string concatenation inside the OpenMP region. |
| 7 | **Vet arithmetic**: the demotion tests `vet < revet_ep` BEFORE this epoch's increment, so the demoted window is the transition epoch plus `revet_ep` more — exactly §6D's "regain 19:25:30 … demoted phase through 19:27:00" at K = 3, and why the vet ends when monitoring BEGINS, not one epoch before. |
| 8 | **A band gone longer than `flicker_max_ep` LEAVES the reference set** (window inclusive), so a permanently single-frequency arc re-vets on its reduced set and returns to full weight. Without it §7's deferred standing-SF policy would have been answered by omission — every degraded arc demoted forever. `ever_dual` keeps it in the audit census regardless. |
| 9 | **REENTRY is asserted, never acted on** (§2's literal instruction); acting would add ~1.7 funnel entries per epoch to a channel the Iono-2 v3k erase policy is tuned on. A FLICKER-REGAIN inserts into BOTH `carry_slipped_ambs` and `carry_slipped_raw` — a band that physically vanished and returned IS a raw observable event, so the v3k split must ERASE the LOS's GM deviation rather than D5-KEEP a level carried across a loss of lock. |
| 10 | **Refloat insertions are NOT gated on a live ambiguity state.** Both a re-entering LOS and a regained band have had their ambiguity pruned (§8.1), so gating would make every insertion a no-op. The funnel is keyed by STRING and its consumers resolve against THIS epoch's arcs, so an insertion for a key PASS B is about to allocate lands correctly; `markAmbiguityPendingReseed` is a plain set insert, safe ahead of the allocation. |
| 11 | **Event discipline**: no per-epoch `degraded_epoch` row (the DEGRADED entry emits one `band_lost` and latches) and no event at all for a first sighting — at 305 stations the first processed epoch would otherwise write ~12 000 rows. Both volumes live in counters instead. |
| 12 | **Loader clamp** `unvetted_sigma_scale <= 1e4`: beyond that a phase row's information approaches the per-station PD bar at an anchor station, where `d_ref` can be the 1e10 hard datum pin. |
| 13 | **No separate "demote" knob.** §6C's census-only leg is expressed with the knobs §3 lists — `census: true, unvetted_sigma_scale: 1.0, refloat_on_regain: false` — and the loader names the resolved disposition (WITHHOLD / DEWEIGHT / NONE) in its arming line, so a user who typed 0.5 expecting a tightening is not met with silence. |

### 8.5 Known residuals (declare these in the field report)

| # | residual |
|---|---|
| 1 | **A DEGRADED window does not reset the arc's CARRY.** The remaining band's phase row is deweighted, but its carried DD chord / leveling value is not re-tapped, so a genuinely slipped remaining band is *attenuated* by `scale²` rather than removed. Resetting on suspicion for a CONTINUOUS arc would be a refloat storm, and there is no evidence of a slip — only an absence of monitoring. Stated so it is not mistaken for coverage. |
| 2 | **`withhold_instead` can still cost a datum pin** when EVERY station tracking a key withholds it in the same epoch (the key then leaves the union like a coverage-gated one). Non-default; grade the W5 gate on any arm that uses it. |
| 3 | **The resume warmup perturbs the first K epochs of a replay leg** relative to the continuous run (branch-faithfulness cost, the addendum's own trade-off). **Acceptability rule: it is acceptable for event-replay legs seeded ≥ K epochs before the event — our FT63 era legs start 38 epochs ahead, so they satisfy it with margin.** A byte-grade resume A/B must arm or disarm it symmetrically on both legs. |
| 4 | **`birth_exempt_los_epochs` is expected to be LARGE on any cold-start leg** (2295 of 2308 here). Read it against `new_arcs`, not absolutely. |
| 5 | The smoke ran a cold start into the DOY-129 era-2 window, so the SH health gate engaged at 19:26 and withheld 4 emissions. That era is **not owned by this card** (§7) and the smoke is not evidence about it either way. |

---

## ADDENDUM (Aug-1) — cold start vs warm resume

**BUILT — the as-built form is §8.4 deviations 4 and 5; only the principle is kept here.**

**Cold start is EXEMPT by principle, not oversight** (gate G): a phase row's damage potential = row
weight × tightness of the state it updates. At cold start tightness is ~zero (ambiguity sigma 1000 m,
diffuse field/biases) — the priors themselves are the demotion, "slip at epoch 1" is undefined, and LLI
(no warm-up needed) plus D5 (once carries ignite) cover the 2-3-epoch residual window on still-diffuse
floats. Do NOT blanket-vet cold starts.

**Warm resume is the true sibling of the flicker case and MUST be vetted**: the snapshot restores tight
carries but NO detector state (verified: `NetworkStateSnapshot` serializes no MwSlip/IF arcs), so the
first `revet_ep` epochs of every resumed run have network-wide detector blindness against fully-warm
state. Long-term alternative (deferred, snapshot format change): serialize detector arc state into RTB-7.
