# SPEC — Lane-coordinate carry (M5 card "WL-3") + Appendix LF-1 (lane-fold perf)

**Status:** IMPLEMENTED (Jul-16 2026, Fable) — increments 1–4 landed + quick-gated (D5 373 vs control 358, constrained circR 0.471 vs WL-2's 0.343, wobble gone, injects retired). Increment 5 (dual-freq grading) awaits command. Default-OFF, byte-identical when off (bit-exact re-verified). Absorbed 2026-08-15: the implementation handoff (was `docs/HANDOFF_lane_carry_card.md`) is now **§9–§11**, and the LF-1 lane-fold perf card (was `docs/SPEC_lane_fold_perf_card.md`) is now **Appendix LF-1** with its own internal numbering preserved verbatim. Both retired originals live under `docs/attic/`. One `network.lane_carry.*` namespace, one document.

> **Family disposition banner.** WL-1 / WL-2 / **WL-3** / UWL-1 / LF-1 are all under the standing verdict **"MW/WL as conditioning — do NOT arm; net-harmful"** (CLAUDE.md). The AGU-26 amendment measured `lane_carry.enable: false` as the SPEED call at BOTH scales — 1.30× faster at 63 stations, **12× at 305** — and `applyLaneMeasurement` as **84.7 % of total runtime at 305 stations**, buying **0 % AR fix rate** with med|frac_cyc| uniform at 0.25. A RECORD, not an open build order. **`docs/CONFIG_promotion_ledger.md` is the knob authority** for arming state and for facts this document deliberately does not restate (e.g. `fold_blocked`'s per-row re-symmetrize is not a bit-exact no-op and drifted the lane-on solution ~1e-4 by epoch 4; `parallel_components` silences the carry driver's per-op timer so `trace_section_timers` undercounts the lane fold when both are on).
> **Provenance.** Every `runs/<name>` path below resolves under `D:\GnssProcessor_archive\2026-08-12\runs\` (archived off C: 2026-08-12).

**One sentence:** carry each primary-pair chord's TWO band DDs as one jointly-covarianced block — stiff along the phase-determined (NL) direction, honestly loose along the WL lane — and feed the lane per-epoch MW innovations (increments, never re-folded levels), so the carry itself becomes the code-coherence accumulator and the WL tightens continuously instead of by discrete injects.

**Relation:** the durable form of F6-fix option (B) (probe-endorsed, `SPEC_mw_wl_card.md` §15; memory `mw-wl-f6-fix-directions`), scheduled once WL-2 was green because the F-A attribution MEASURED the failure class this card removes (a re-folded statistic against an over-stiff carry elevates D5 network-wide through the shared border), and the F-B lane trace measured the residual roughness of discrete injects (constrained-cycle med|f| oscillating 0.01↔0.30 across epochs at 15 ep). WL-2's statistics (MW, bridge, iono de-bias), FFR gates, closure check, quarantine, force-fire harness and the lane-trace grader carry over UNCHANGED as this card's measurement sources and acceptance instruments.

---

## 1. Motivation (measured facts, do not relitigate)

- **F6:** per-band stiff carries pin the WL lane at unearned σ≈0.085 cyc at code-seed garbage; rows lose (float) or churn (stiff). Probe: authority transfers values but stiff-vs-stiff sprays D5 (+9 %); a loose lane is the durable direction.
- **F-A:** 147 σ0.01 EWL rows re-folded per epoch elevated D5 **~2× NETWORK-WIDE** (679 vs control 342 at 15 ep; border-mediated — 24 % of flags on untouched signal groups). Cured by not folding — proving "statistic re-folded as an independent measurement" is a real, measured failure class. WL-2 v1 therefore acts by discrete injects only.
- **F-B trace:** injected/held cycles reach circR **0.34** vs control **0.019** — the lift is real but ROUGH (re-taps knock lanes off between injects; med|f| oscillates 0.01↔0.30 per epoch). A carried loose lane receiving per-epoch increments tightens as √N and re-tightens itself after re-taps without waiting for the next FFR/disagree trigger.
- **σ̂₀ honesty:** the WL-1 §11 Q4 tripwire (LS-VCE ≫ 1 under the loose bolt-on) fired at **1.86 vs 1.42 control** in the fold-on shape — the pre-registered criterion for going tight.

## 2. Representation (the core design decision)

**One carry group per (constellation, primary signal PAIR)** replaces the two per-band groups for those signals. Within a pair group: the arc graph is the SAME (rx, sat) simple graph both bands share (v1 requires band-2 presence for membership; a band-1-only arc stays in a residual per-band group as today — no information lost, no multigraph). Each graph chord carries a **2-vector** z = (z₁, z₂), the two bands' cycle DDs (correction frame, meters — exactly today's per-band z's), with a joint 2K×2K per-component covariance Pᶻ (K chords). Equivalently, the (NL, WL) rotation — the LANE basis — is applied only to the PROCESS MODEL and the measurement update, never to storage: storage stays per-band (zero-migration reconcile, the W projection applies per band unchanged, all lifecycle algebra intact). `buildDdPriors` emits per component ONE DdPriorBlock with 2K rows — the K band-1 cycles then the K band-2 cycles (both on their own columns) — with the joint 2K×2K Pᶻ. The fold is already rank-general; `installPosterior` reads the joint posterior back. D5 gates per row exactly as today (a per-band innovation IS still per-band).

Why this shape: it changes NO cycle algebra (tree, reconcile, W, birth/death all per band as today, just co-located), adds no states, and the ONLY new math is the covariance coupling + the lane-aware process noise + the lane measurement rows.

**MEASURED DEVIATION — membership needed two additions this section did not anticipate** (§10 deviation 4): a census-preferred primary pair per constellation, and driver-side spanning-forest completion.

## 3. Mechanism

### 3.1 Lane-aware process noise (the honesty fix)

Per chord, per epoch, in the lane basis: q applied as Q = Rᵀ·diag(q_nl, q_wl)·R with R the (unit-normalized) NL/WL rotation for the pair's (λ₁, λ₂). `q_nl` = today's `carry_invariant_qz` (phase owns NL). A fresh chord's Pᶻ takes a lane-basis prior diag(σ_nl_fresh², σ_wl_fresh²); thereafter `q_wl` is a modest rate so the lane never re-earns stiffness it wasn't measured into. Both are config knobs (§5).

> ⚠️ **BOTH of this section's numeric prescriptions were FALSIFIED by measurement and the defaults changed** — §10 deviations 1 and 2. The SPEC said σ_wl_fresh ≈ the code-seed lane error (3 cyc) and q_wl = 10³ × q_nl. The live defaults are **`sigma_wl_fresh_cyc` = 0.0 (diffuse birth)** and **`q_wl_scale` = 0.01**.

### 3.2 Lane measurement rows (increments, never levels)

Per epoch, per eligible cycle: ONE lane row per pair-group component block — the WL functional row (+c/λ₁, −c/λ₂ on the cycle's columns, realized on the CARRIED block, not the estimator columns) with **z = THIS EPOCH'S MW(1,2) cycle DD** (the detector's per-epoch value, NOT the Welford mean) and R = the per-epoch MW noise (from the detector's running variance, floored). Folded INTO the carry block prior at buildDdPriors time (a KF measurement update on (z, Pᶻ) before projection — driver-internal, closed form, K-small). Each epoch's code information enters ONCE; the Welford accumulation now happens INSIDE the carried covariance — the estimator owns the accumulator (the WL-1 §11 Q4 "tight" form, scoped to the carry). The BRIDGE (EWL-fixed cycles) supplies a second, phase-precision lane measurement the same way (per-epoch GF_b increment, iono-de-biased per WL-2 §3.4), same once-only discipline; ⚠ bridge arcs had to be re-keyed to the census channels — §10 deviation 6. The detector Welford means remain DECISION-side only (FFR integer fixing).

### 3.3 What replaces the WL-2 inject/holds

Nothing to fight ⇒ nothing to exempt: with an honestly loose lane, the lane measurements pull the carried lane to the integers continuously; FFR-fixing (statistic-gated, unchanged) then collapses the lane variance to `fix_weight_sigma_cyc`. `injectCarriedChord` and the D5 exemption remain as the slip/re-tap fallback path but should fire ~never in steady state (telemetry proves it). WL-2's inject machinery stays compiled and default-armed under `wl_inject_enable` for the non-lane-carry shape (A/B comparability); `lane_carry.enable` supersedes it at runtime (mutex-warned).

> ⚠️ **The "ONE prior edit per integer decision change (fold-on-change)" mechanism was FALSIFIED and replaced** by decide-per-epoch + an idempotent lane-coordinate SET — §10 deviation 3. *(This section is what `GnssConfigLoadNetwork.cpp:6577` cites for the runtime supersession (re-grepped 2026-08-15) of the WL-2 hold/inject walk.)*

### 3.4 Lifecycle

Per-band algebra unchanged (reconcile/W per band; merge/split/swap co-located). A slip on EITHER band's arc refloats the chord's BOTH bands (they share the graph edge; today's funnel already keys per ambiguity — union them). A fresh chord re-enters with the §3.1 lane-aware prior — the post-re-tap lane loosens honestly and the next epochs' lane measurements re-tighten it: the F-B wobble self-heals without a trigger. ⚠ The union refloat is LANE-shape-only — §10 deviation 7.

## 4. Estimability / invariants

Same closed-cycle class throughout (both band rows are today's carried DDs; the lane rotation is a regular linear reparametrization of the SAME invariant pair). No datum touched; FIREWALL gate re-run mechanically. Standing invariant #1: the stiff NL direction keeps today's slip funnel; the WL direction is never stiff without measurement mass — the invariant's precondition dissolves by design and the force-fire battery (§6) proves the response anyway.

## 5. Config (`network.lane_carry.*`)

All keys live in ONE struct (`LaneCarryConfig`, `include/config/GnssConfigTypes.hpp`) parsed in one loader block. The first block is WL-3 **lane-SHAPE** knobs; the second is Appendix LF-1 **PERF** knobs — the code draws that line explicitly and it must not be blurred (perf knobs must stay OUT of the equivalence-shape discriminator: they do not change what folds).

| key | default | class | meaning |
|---|---|---|---|
| `enable` | false | shape | Master. Requires `carry_invariant` + `mw_wl.ewl_enable` (statistics source). OFF ⇒ byte-identical. **MECHANISM COMPLETE (Jul-16, increments 2–4); `applyLaneMeasurement` / `applyLaneFix` / `carriedPairValue` / `injectCarriedChordPair` all shipped** (symbols verified live in `NetworkInvariantCarryDriver`). *(An earlier revision of this table read "parsed + fail-loud-inert contract; mechanism NOT yet implemented" — the increment-1 state, contradicting this document's own header. Corrected.)* ✅ **DOC↔CODE DRIFT CLOSED 2026-08-15 (serial cleanup pass):** `include/config/GnssConfigTypes.hpp:909` carried the retired wording "config CONTRACT only — mechanism is a follow-up increment"; the comment now states the mechanism is complete and names the four shipped symbols. |
| `q_wl_scale` | **0.01** | shape | q_wl = scale × `carry_invariant_qz` along the lane. **The SPEC §3.1 value 1000.0 is FALSIFIED**: at the lineage qz = 1e-4 m²/s it adds ~12 WL cyc of lane variance per 30 s epoch, swamping every lane measurement and destroying each fix within one epoch (circR 0.471 at 0.01 vs 0.251 at scale 1). 0.01×qz ≈ (0.04 cyc)²/epoch keeps √N accumulation AND fix persistence. Loader clamp relaxed to ≥ 0. `1.0` = isotropic (part of the §6.1 equivalence shape). |
| `sigma_wl_fresh_cyc` | **0.0** | shape | Fresh-chord lane prior σ (cycles); 0 = diffuse birth. **The SPEC §3.1 value 3.0 is FALSIFIED**: the correction-frame origin is NOT the code-seed lane (D5 691 prior-only vs 125 q-only at 10 ep) — §10 deviation 1. `>0` survives only as a loud-warned study shape. |
| `lane_meas` | true | shape | Per-epoch MW(1,2) lane increments (§3.2). |
| `bridge_meas` | true | shape | Per-epoch iono-de-biased bridge lane increments on EWL-fixed cycles. |
| `fold_blocked` | false | **perf** (LF-1 INC-1, §3.2 of the appendix) | O(K) sparse `applyLaneFix` + the per-row re-symmetrize hoisted to one pass per block per epoch. **Changes the OPERATION ORDER ⇒ the `parallel_carry` EQUIVALENCE class, NOT bit-identical.** OFF ⇒ the sequential per-row fold (byte-identical). |
| `skip_pinned_increments` | false | **perf** (LF-1 INC-2, appendix §3.3) | Skip the per-epoch MW/bridge increments for a committed-decided chord. Any slip/D5/re-tap resumes increments the same epoch. Equivalence class (near-inert, not exactly null). OFF ⇒ byte-identical. |
| `skip_pinned_within_n` | 3 | **perf** (LF-1 INC-2) | The skip grace window (epochs) after a chord's last re-decision; clamped ≥ 1. INERT unless `skip_pinned_increments`. |
| `parallel_components` | false | **perf** (LF-1 INC-4, appendix §3.5) | OMP over independent components, one joint block per thread, each block's folds in order ⇒ **BYTE-IDENTICAL to serial** (unlike `fold_blocked`). Needs `parallel_stations` (n_threads>1) to fire; INERT unless the lane is joint. Eigen is re-pinned to 1 inside the region (the pcarry un-pin stays outside — the dev300s nesting trap). |

## 6. Validation — the acceptance RESULT

Gate design was: (1) equivalence gate, (2) unit battery, (3) §7.3 force-fire re-run, (4) 15-ep A/B vs the WL-2 green shape, (5) perf — blocks double in K per pair component (2K rows; Minv (2K)²·K cost), measured with D0/P0b, with the C2 impossibility analysis bounding expectations. Results, 15-ep 50-sta GPS `_wlver` harness, Release, same-build A/B, one knob apart:

| metric | WL-2 green shape (`_wlinj`) | WL-3 lane shape (`_lane`) |
|---|---|---|
| D5 flags | 358 | **373** (control family; storms are 649–1628) |
| constrained circR (pooled last-5) | 0.343 | **0.471** |
| constrained med\|f\| | 0.137 | **0.061** |
| P(\|f\|<0.15) | 0.519 | **0.651** |
| per-epoch wobble | med\|f\| 0.01↔0.30 oscillation | **≤ 0.064 for 11 consecutive epochs (3–13)** |
| injects | 24 | **0** (retirement telemetry, §3.3) |
| lane increments | — | **1005 MW + 112 bridge** rows |
| FFR decisions | 50 (holds+injects) | 41 → **9 moved + 32 re-pinned** |

**Acceptance §6.4 MET and exceeding the WL-2 baseline.** 10-ep smoke D5 218 (control 225). **Force-fire re-run (`_lane_ff`):** EWL **144 fixed = the WL-2 ff baseline** (evaporate → re-fix), **13 moved fixes, exactly 1 fallback inject** (|Δ| = 2.01 — the post-slip catch), **no storm (D5 440)**, no errors/NaN. **Suite at the time:** 761 run / 760 pass / 1 pre-existing skip, including **7 new WL-3 driver gates** (equivalence, lane prior/q hand values, lane KF hand value, fix semantics, union refloat, pair inject exemption, pair parallel-install bit-identity). *(Dated record — the suite is now 1360 cases / 196 suites.)*

**§6.1 equivalence gate, REFRAMED — see §10 deviation 5.** The strict ≤1e-9 field equivalence is unattainable BY DESIGN; the strict gate lives in the UNIT test and the field leg validates STRUCTURAL parity instead.

**Verification checklist (final build):** OFF byte-identity — `network_solution.csv` + `network_dd_fractional.csv` hash-identical vs `gnss_processor_fable_ab.exe` (pre-WL-3), **after ALL shared-path edits** (machinery hoist, detector lag fields, install refactor); WL-2 regression — `_wlinj` on this build reproduces **D5 358** and lane-trace **circR 0.343** exactly; quick-only per the staged cadence.

## 7. Increments — landed status

1. **1 (landed):** config contract — parsed, clamped, mutex-checked, loud NOT-implemented warn (the WL-1 Phase-B precedent).
2. **2 (landed):** pair-group foundation + equivalence gate — regroup, joint blocks, block-diagonal Pᶻ, the §6.1 proof. The risky increment.
3. **3 (landed):** lane-aware prior/q + per-epoch MW lane rows (§3.1–3.2) + A/B.
4. **4 (landed):** bridge rows + fold-on-change fixing (became the idempotent SET, deviation 3) + inject-retirement telemetry + the full §6 battery.
5. **5 (OPEN, awaits command):** dual-freq grading — the card's unique win, WL accumulation with NO third band — plus a graded run. §11 has the measured 15-epoch floor and why the harness cannot reach FFR grade.

## 8. Open questions for the implementer

*(Q1 below is cited BY LINE from `docs/SPEC_carry_aware_covariance.md` — cite it as "§8 Q1" from here on, never by line number.)*

1. **Residual per-band group for band-1-only arcs:** confirm the driver tolerates a station appearing in both the pair group and the residual group (disjoint ambiguities ⇒ disjoint columns — **expected yes**). ⚠ `SPEC_carry_aware_covariance.md` records this as *the weaker inference*: `N_rr⁻¹` is dense (the station's clock, ZWD, per-sat slant iono and every ambiguity are one block), so sharing a **station** is already sufficient to couple two post-reduce folds. "Disjoint columns" is not the right sufficiency test; read that card before leaning on this answer.
2. **The lane KF update inside buildDdPriors vs a driver pre-step:** pick the seam that keeps `beginEpoch` q-application and the update ORDER deterministic under `parallel_carry` (serial seam like the WL-2 statistics reads).
3. **Per-epoch MW value accessor:** the detector keeps the last raw MW per arc — expose a lagged per-epoch value + its epoch noise honestly (NOT the Welford mean; watch the F1-lag discipline). *(Landed as `arcZdLast` / `arcBridgeLast`, §9.)*
4. **D5 per-lane:** with 2K rows the flag stays per band-row; decide whether a lane-basis flag (NL vs WL innovations separately) reads cleaner — diagnostic first, gate change only if measured.

## 9. What landed where — symbol inventory  *(HANDOFF "What landed where")*

*(Was `HANDOFF_lane_carry_card.md` §2. Implements increments 2–4; increment 1 = the config contract, landed the same day.)*

- **Driver** (`NetworkInvariantCarryDriver`): pair groups (`GroupInput.pair` + per-arc `local_col2`/`slipped2`), joint [band-1; band-2] carried storage with per-band W projection, spanning-forest completion for pair components (`pair_tree_promotions`), lane-basis fresh prior + lane q (`laneRotation`), split-vs-joint emission (`BlockMeta.pair_band` 1/2 = §6.1 equivalence halves, 3 = joint 2K lane block), `applyLaneMeasurement` (scalar KF fold of a per-epoch increment), `applyLaneFix` (idempotent lane-coordinate SET — deviation 3), `carriedPairValue` / `injectCarriedChordPair`, pair handling in both `installPosterior` paths, and fold-on-change memory staged per build / committed at install (RAIM re-solve safe).
- **Detector** (`MwSlipDetector`): `arcZdLast` / `arcBridgeLast` — LAGGED raw per-epoch samples + per-sample Welford variance (the once-only increment sources; `prev_mw_lag` / `prev_gf_lag` snapshots ride every frame shift) — and `setBridgePreferredChannels` (deviation 6).
- **ApplicationNetwork**: WL machinery hoisted above the group build; census-preferred primary pair under `lane_carry`; pair-group routing (residual minority arcs stay per-band); MW-increment walk + bridge-increment/fix walk; pair inject fallback; pair lane-trace + D5 funnel (band-2 keys via `carry_arc_amb_key2`); the WL-3 run-end telemetry line.
- **Configs**: `_wlver_50sta_{10ep,15ep}_lane.json`, `_lane_lt` (trace), `_lane_equiv` / `_lane_equiv_ctl` (§6.1 leg), `_lane_ff` (force-fire). Exe: `build/x64-Release/gnss_processor_wl3_run.exe`.

## 10. Measured spec deviations 1–7 (each with its falsifying number; none speculative)

1. **§3.1 fresh lane prior (σ_wl_fresh = 3 cyc) FALSIFIED — default now 0 (diffuse birth).** The correction-frame origin is NOT the code-seed lane: a cycle's ambiguities seed at DIFFERENT epochs, so per-station clock error at seed time does not cancel in the lane (× (f₁−f₂) ⇒ **O(10²) cycles of mis-centering**). Measured **D5 691 (prior-only) vs 125 (q-only) at 10 ep**. The honest code-seed-scale information enters via the FIRST MW increment instead (correctly centered). σ>0 survives only as a loud-warned study shape.
2. **§3.1 q_wl = 10³ × q_nl FALSIFIED at the lineage qz — default now 0.01.** The numerology assumed q_nl ≈ 0; at qz = 1e-4 m²/s it adds **~12 WL cyc of lane variance per 30 s epoch**, swamping every lane measurement and destroying each fix within one epoch. 0.01×qz ≈ (0.04 cyc)²/epoch keeps √N accumulation AND fix persistence (**circR 0.471 vs 0.251 at scale 1**). Loader clamp relaxed to ≥ 0.
3. **§3.3 fold-on-change → decide-per-epoch + idempotent lane-coordinate SET.** A one-shot fold lets the decided lane drift back to MW quality under q + noisy increments (**measured med|f| 0.25 vs 0.06**). The fix is an information-idempotent SET (recenter + recollapse + zero cross-cov — WL-2's inject semantics on the lane coordinate, NL preserved by construction), re-applied every decided epoch. The F-A once-only discipline holds because the set does not accumulate; `applyLaneFix` returns moved-vs-re-pinned for telemetry.
4. **§2 pair-group membership needed two unanticipated additions.** (a) A **census-preferred primary pair per constellation** — the lexicographic tie-break fragments pairs across stations (L2L-vs-L2W), splitting the pair graph and forfeiting **~1/3 of the primary cycle space** (measured); only the majority pair joins the pair group, minority arcs stay fully per-band. (b) **Driver-side spanning-forest completion** — the pair subgraph has no upstream election and band-1's tree tags need not span it (**27 completions / 15 ep after (a); 903 before**).
5. **§6.1 strict ≤1e-9 FIELD equivalence is unattainable BY DESIGN** whenever the two bands' election trees differ or arc churn interacts with different fundamental-cycle paths (carried/born decisions are cycle-path-dependent; divergence starts at 0 at epoch 1 and grows with churn — measured, gate-off, both slip-semantics variants). The strict gate lives in the UNIT test (exact through noise + per-band slip + tree change); the field leg validates **STRUCTURAL parity** instead: carried dimension **904 vs 912**, births **1286 vs 1271**, refloats **321 vs 312**, **0 whole-block flicker**, D5 **310 vs 348**, boundary quantified by the completion counter. ✅ **This deviation — not deviation 4 — is what the equivalence-shape log string in `GnssConfigLoadNetwork.cpp:6590-6593` (re-grepped 2026-08-15; the finding's `:6498` is itself stale) describes ("cycle-path-dependent membership"). Its pointer read "§10 deviation 4" and was MIS-AIMED; CORRECTED to "§10 deviation 5" 2026-08-15 (serial cleanup pass) — citation text only, no semantics touched.**
6. **Bridge arcs re-keyed to the census channels** (`setBridgePreferredChannels`): the detector's min-name per-band selection starved the WL decision path to **9-of-29 cycles** at (rx,sat)s whose channels differ from the census pair.
7. **Union slip refloat (§3.4) is LANE-shape-only**; the equivalence shape uses per-band slip semantics (matching the two per-band groups it reproduces). Latent in practice because the PASS-A funnel is band-symmetric.

## 11. Deferrals, triaged with DATA  *(HANDOFF "Deferrals (triaged)")*

- **Cross-boundary cycles between the majority pair group and minority-channel residual groups are NOT carried** — **~8 of 912 DDs ≈ 1 %** on this fixture, **field-inert at this scale** (data, per invariant #3). A v2 quotient carry could recover them. *(UWL-1's `uwl_family_pairs` is the later recovery of the analogous Galileo case.)*
- **Dual-freq (no-bridge) lane maturity** (increment 5): control-population circR **0.050 vs 0.019 at 15 ep** — the √N accumulation is VISIBLE, but the census law σ(n) ≈ 0.5–0.7/√n needs **25–50 epochs** for FFR grade and the 15-ep harness cannot reach it. Increment 5's graded run (on command) is the test.
- **Late-window trace epochs 14–15** (med|f| 0.18 / 0.31 on n=9): end-of-run slips re-tapping constrained chords; the §3.4 self-heal needs ~2–3 epochs the run does not have. Not a mechanism defect (eps 3–13 clean); the longer graded run measures steady state.

---

# Appendix LF-1 — Lane-fold efficiency (M5 perf card "LF-1")

*Was `docs/SPEC_lane_fold_perf_card.md`, absorbed 2026-08-15. **Internal numbering (§1–§7) is preserved VERBATIM** because six code comments and two tests cite "SPEC_lane_fold_perf_card.md §3.2 / §3.3 / §5.2" — those anchors now read as "`SPEC_lane_carry_card.md` Appendix LF-1 §3.2" etc. (§5.2 = numbered item 2 of LF-1 §5, the unit gate asserting blocked-vs-sequential equivalence to 1e-12; the physically-null gate is item 3 = §5.3.) Nothing here renumbers WL-3's own §3.2/§3.3, which are DIFFERENT mechanisms (lane-measurement rows vs blocked fold).*

**Status — CORRECTED (the original header read "QUEUED … nothing built"; that is stale).** Three of the four numbered increments SHIPPED and each carries a `docs/CONFIG_promotion_ledger.md` entry:

| increment | knob | state |
|---|---|---|
| INC-0 instrumentation split | — | not accounted for in this card; the section-timer split largely exists elsewhere. |
| INC-1 blocked row application | `network.lane_carry.fold_blocked` | **SHIPPED** (equivalence class, not bit-identical). |
| INC-2 pinned-cycle increment skip | `network.lane_carry.skip_pinned_increments` (+ `skip_pinned_within_n`) | **SHIPPED.** |
| INC-3 chord lifecycle (kill/marginalize) | — | **PARTIAL / unresolved.** Whole-block marginalization counters exist (`chords_marginalized`) and the per-chord counter `chords_marg_perchord` was added as INC-C0; the AGU-26 63-sta A/B measured whole-block marginalizations lane ON = **0** vs lane OFF = **2549** over 121 epochs. The kill POLICY this section asks for is still not written. |
| INC-4 component-parallel fold | `network.lane_carry.parallel_components` | **SHIPPED** (byte-identical class). |

⚠️ **And the card's PREMISE is superseded.** Its motivating and closing numbers (§1, §5.5) predate the AGU-26 campaign. Measured at 305 stations on a settled GPS epoch: `lane_carry.enable: true` **310–325 s/ep**; `+ fold_blocked + parallel_components` **115 s/ep** (2.8×); `lane_carry.enable: false` **26.7 s/ep** (**12×**). `applyLaneMeasurement` is **84.7 % of total runtime at 305 stations** and the layer buys **0 % AR fix rate** with med|frac_cyc| uniform at 0.25. **The card's closer (§5.5, ≤ 90 s/ep) was never met (115), and more importantly the measured answer is to turn the layer OFF, not to make it affordable.** Keep this appendix as the record of what the four levers are and what they cost; do not resume optimizing a layer that is do-not-arm.

**One sentence:** the WL-3 lane accumulator applies ~10³ sequential O(K²) rank-1 covariance updates per epoch against dense per-component pair blocks whose dimension K never shrinks — batch them (BLAS-3), stop folding information-free rows on pinned cycles, retire dead chords, and fan the independent components across cores, without touching a single decision path.

**Relation:** perf-only successor to the WL-3 lane-coordinate carry (this document's §1–§11) and the UWL-1 tier (`docs/SPEC_uwl_card.md`). The STRUCTURAL escape — a factored Pᶻ — was investigated and **CLOSED AS IMPOSSIBLE for this carry** (`docs/SPEC_carry_factored_pz_C2.md`; memory `carry-driver-perf-c2-impossibility`); **do not re-propose it.** `network.parallel_carry` (Eigen-threaded GEMMs, 2.13–2.45×) is already in every number below.

## LF-1 §1. Motivation (measured, Jul-16 2026 — do not relitigate)

1. **The lane accumulator is the wall, not the network.** The 3card lineage (no lanes) runs 305 stations at ~28 s/ep with pcarry. With the full lane stack: **150 sta ≈ 46–61 s/ep dd-build; 305 sta ≈ 384–617 s/ep** (Σnet ~600 s/ep; `_wlver_305sta_10ep_d10_sh8_full`) — **12× for 2× stations**. A 60-epoch 305-station leg is ~10 h wall; the user declined it. *(Superseded by the AGU-26 numbers above — the ~600 s/ep figure predates `fold_blocked` / `parallel_components`.)*
2. **The cost law is row-count × K².** The 60-ep 150-sta leg folded **48 786 MW(1,2) lane increments + 23 503 bridge increments ≈ 1,200 rows/ep**, each a sequential BLAS-2 rank-1 fold against the component's dense 2K×2K lane-shape covariance. K doubles with stations ⇒ per-row cost ×4, row count ×2 ⇒ the observed ~12×.
3. **Most steady-state rows are information-free.** By epoch 30+, **~65 %** of traced cycles are DECIDED and idempotently re-pinned at σ = 0.01 cyc; an MW increment at σ ≈ 0.06–0.2 cyc folded into a lane already pinned 6–20× tighter moves nothing.
4. **K NEVER SHRINKS.** Jul-14 profiling recorded **K ≈ 7,834 carried DD chords with ZERO ever marginalized** (suspected killChord lifecycle gap, flagged INC-C0/C1 in `docs/SPEC_carry_factored_pz_C2.md` §7.2/§7.3 — RESOLVED there: INC-C0 measured, INC-C1 closed as case (a) in its §6). Set satellites' chords ride the dense matrix forever.
5. **Components are independent by construction.** The pair-block system is block-diagonal over carry components (**44 components at 150 sta** under family pairs); nothing couples them inside the lane fold.

## LF-1 §2. Non-goals

No change to any DECISION path (statistics stay detector-side; FFR gates, closures, dedupe, cold serves untouched); no change to what information enters — only *when it is skipped as redundant* (INC-2, gated) and *how the linear algebra is applied*; **no factored covariance (C2 closed), no approximate/sparse Pᶻ**; not the D1 cov-consumers card (dd-frac/T1a sweeps — separate, ~14.5 %).

## LF-1 §3. Mechanism (four levers, independent knobs)

### LF-1 §3.1 INC-0 — instrumentation split (log-only, first)

Extend the D0b sub-timers to split SB_DDBUILD into: base carry build/fold/install vs `applyLaneMeasurement` vs `applyLaneFix` vs the WL/EWL layer `build()`. Per-epoch: rows folded per component, K per component, rows skipped. Every later increment is graded against this baseline. (Invariant #4: the shares in §3 are estimates until INC-0 measures them.)

### LF-1 §3.2 INC-1 — blocked row application (`network.lane_carry.fold_blocked`)

Accumulate the epoch's lane rows PER COMPONENT and apply as one rank-m block update (Cholesky-downdate-style, or Woodbury with an m×m core) instead of m sequential rank-1s. Same flops to first order, but BLAS-3 vs BLAS-2 — expected 3–8× on the row-fold share, more with pcarry's threaded GEMMs. **NUMERICAL CONTRACT: a different operation ORDER ⇒ NOT bit-identical to the sequential fold — this is the `parallel_carry` EQUIVALENCE class, not the `parallel_seam` byte-identical class.** Default OFF; ON requires the physically-null gate (§5.3) before any graded use.

### LF-1 §3.3 INC-2 — pinned-cycle increment skip (`network.lane_carry.skip_pinned_increments`)

Skip MW/bridge lane increments for a chord when (a) its last `applyLaneFix` returned fold-or-unchanged within the last N epochs (N default 3, clamped ≥ 1), AND (b) no refloat/slip/re-tap has touched the chord since. **ANY funnel event clears the condition** (invariant #1: the slip response path must be provably unaffected — a refloated chord resumes increments the same epoch). Decision quality is untouched by construction (decisions never read the folded increments); the gate is lane-trace parity (§5.4). Expected steady-state row cut **2.5–4×**.

### LF-1 §3.4 INC-3 — chord lifecycle (kill/marginalize policy)

Resolve the 0-marginalized finding: distinguish "chord dead" (satellite set, station gone ⇒ MARGINALIZE properly, preserving the Schur complement onto survivors) from "chord dormant", to cap K's monotone growth. **This is the correctness-sensitive increment: marginalization must not touch the datum (S-basis untouched; the carry's invariant functions stay invariant) and must round-trip the RTB-7 snapshot.** Requires its own unit battery.

### LF-1 §3.5 INC-4 — component-parallel fold (`network.lane_carry.parallel_components`)

OMP over components with per-component sequential order preserved and a fixed merge order — **byte-identical is CLAIMABLE here** (no cross-component coupling), unlike INC-1. **NESTING HAZARD (measured, dev300s card): do NOT un-pin Eigen inside the OMP region** — components use pinned Eigen; the existing pcarry un-pin stays OUTSIDE. Amdahl note: at 150 sta one component may dominate K.

## LF-1 §4. Standing-invariants pre-flight

(1) No new stiff quantities, no event-response changes; INC-2's skip clears on any funnel event — force-fire re-run required at its gate. (2) Release only. (3) Deferrals triaged in §6. (4) INC-0 counters make every skip/batch visible; **a lever that shows 0 in its counter is inert and its gate FAILS.** (5) Grading on lane-trace/dd-frac datum-invariant quantities. (6) Every knob default-OFF byte-identical; INC-1 explicitly pcarry-class (equivalence, not identity) and its ON runs carry the same usage rule pcarry had until its equivalence gate passed.

## LF-1 §5. Validation

1. **Byte-id (all knobs OFF)** vs the pre-card exe on `_wlver_50sta_15ep_off` + `_wlver_50sta_10ep_uwl` — recorded hashes **90DB1548/DB1A081C** and **3A395593/B5A04846**.
2. **Suite** green + new unit gates (blocked-fold algebra vs sequential on synthetic blocks to 1e-12; skip-condition lifecycle incl. refloat-clears; marginalization Schur correctness + snapshot round-trip).
3. **Physically-null gate (INC-1, INC-4)** — the pcarry precedent: run-end counters byte-identical (decided/fixes/injects/D5/deweight), dd-frac diagnostic drift ≤ a few 1e-3 cyc, lane-trace constrained circR/med|f| parity.
4. **Quality parity (INC-2):** 30-ep 150-sta A/B — constrained circR/med|f|, decided/ep, D5 within family of skip-OFF; force-fire shows unchanged post-slip re-fix latency.
5. **§5.5 The number that was to close the card:** 305-sta 10-ep smoke Σnet **≤ 90 s/ep** (from ~600) with all levers armed; 150-sta ≤ ~20 s/ep. ⚠ **NOT MET and now moot** — measured 115 s/ep with `fold_blocked + parallel_components` at 305 sta, against 26.7 s/ep for `lane_carry.enable: false` (AGU-26; status block above).

## LF-1 §6. Increments & triage

0. Instrumentation split (log-only; REQUIRED first — the §3 shares are estimates).
1. Blocked application — biggest expected win, pure numerics. **SHIPPED.**
2. Pinned-cycle skip — biggest expected row cut; needs the force-fire gate. **SHIPPED.**
3. Chord lifecycle — correctness-sensitive; can land independently. **NOT SHIPPED.**
4. Component-parallel — after INC-0 measures the K distribution. **SHIPPED.**

Deferrals: none field-blocking (the card itself is a perf unblock; every lever is default-OFF). 305-station GRADED lane runs were gated on this card; ≤150-station science was unaffected throughout.

## LF-1 §7. Open questions for the implementer

1. Does `applyLaneFix` rc==0 (unchanged re-pin) early-out before the covariance touch, or re-fold the same row? If it re-folds, the idempotent SET is a hidden per-epoch rank-1 per decided cycle — INC-0 would show it; an early-out may be a one-line win ahead of INC-2.
2. INC-1 core: rank-m Woodbury with an m×m LDLT vs m sequential downdates — which preserves SPD better on the stiff lane-shape blocks at the q_wl 0.01 scale disparity? Prototype both on the 60-ep captured blocks.
3. INC-3: does the RTB-7 fingerprint need a marginalization-count term so a warm resume across a kill-policy flip is rejected (the carrycpljoint lesson)?
4. K distribution at 305 under family pairs: one giant G component (~2,700 arcs) dominates — is component-splitting (forest partitioning) ever admissible without breaking cycle closure, or is INC-4's ceiling just Amdahl on that component?
