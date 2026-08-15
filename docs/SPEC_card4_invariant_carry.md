# CARD 4 (M1) — Invariant-function carry: the generalized-LS filter seam

**Status: BUILT AND SHIPPING.** The design contract cited by `include/filter/NetworkInvariantCarryDriver.hpp:8` and `include/filter/NetworkZStore.hpp:8`; the `network.carry_invariant*` knob family is live in `GnssConfig.hpp`, and `carry_invariant` is treated as a real posture in CLAUDE.md's standing-armed-set discussion. Phases B/C/C3b/C3c and phase D (emit-layer re-base, §10) all landed. Successors: §13.

*Fable-specced 2026-07-11 from the Card 3 field campaign (memory `card3-field-grading-jul10`: 12-arm ladder, eq34 producer stuck 9–34 cyc/ep vs golden 0.085) and the 8-paper theory pack (memory `ppprtk-literature-pack`). Foundations: Teunissen/Khodabandeh/Psychas 2021 (generalized Kalman filter, J Geod 95:108) and Hou & Zhang 2025 (generalized least-squares filter, J Geod 99:3). Supersedes the D6-v2 border-promotion idea of the Card-3 eq34 datum handoff (`HANDOFF_eq34_datum_card3.md`, pruned in a8f044f) and, when armed, most of the carry hygiene built during the Jul-10/11 campaign. Phase D absorbed from `HANDOFF_card4_phaseD.md` (2026-07-12) 2026-08-15. Two design-lineage docs this card originally pointed at were pruned in `a8f044f` and are recoverable at `a8f044f^`: `HANDOFF_card4_phaseA_scout.md` (its adjudications survive inline in §5 and §12) and `HANDOFF_eq34_datum_card3.md`.*

## 1. The defect — why every Card-3 stabilization arm failed

Our time-linking machinery (D2/C1 shared PB carry, phase-leveling local carry, rpb_carry) attaches dynamic models to **S-dependent raw states**: per-key ambiguity floats, per-key satellite phase biases, per-station RPBs. Every one changes meaning when the S-basis changes — and at 50-station global scale the basis changes ~7×/epoch from satellite-pass physics (measured; irreducible by election policy). Each basis event either poisons the carried priors (the 03:46-class silent re-level: stiff prior × frame step; Psychas 2024b Fig. 3 shows jump impact *scales with* dynamic-model stiffness) or forces an erase (refloat storm; the erase→no-carry→hard-re-pin vicious cycle measured in arms B4–B7). The exact S-transformation of the carries (arm B8) is theoretically correct (Odijk 2016's own prescription for cross-basis comparison) but proved fragile in the full model — and even if fixed, it is a *repair* executed ~7×/epoch forever.

de Jonge (1998, §3.5) never faced this: his S-basis is extend-only inside a batch adjustment where dead basis columns persist through their historical epochs. The literature has **no running-filter solution** — Teunissen et al. 2021 provide the framework (dynamics on selected linear functions z_i = F_iᵀx_i; everything else estimated fresh, exactly decoupled) but leave Φᶻ across a changing linked set unconstructed. Supplying that construction, with z chosen so the transition is trivial, is Card 4.

## 2. The theory in three lines

Choose the time-linked functions z to be **S-system invariants**: double-differenced (DD) ambiguities. DDs are estimable under every datum, unaffected by any pivot/tree/handover choice, integer-valued, and constant in time (q_z = 0 between slips). Then the datum can be re-realized *freely every epoch* — nothing the filter remembers depends on it — and basis churn becomes a non-event by construction, not by hygiene.

The cycle-space identification makes Φᶻ trivial: for the (const,σ) bipartite observation graph with E arcs and a spanning tree of n_c+m_c−1 edges, the independent DDs are exactly the **fundamental cycles of the non-tree arcs (chords)** — dimension E − (n_c+m_c−1) per component. Arc events map to:

- **chord death** → delete that z entry (drop row/col of Pᶻ — pure marginalization, no transform);
- **chord birth** → append a z entry, initialized by tap-off ẑ_new = fᵀx̂ (GKF Eq. 22 pattern) with its conditional variance;
- **tree-edge death** → swap a chord into the tree; the fundamental-cycle basis change is a **unimodular integer matrix T**: z' = Tz, Pᶻ' = TPᶻTᵀ — exact, integer, zero information loss. This IS the Φᶻ construction the 2021 paper left open, and it is trivial *because* z is the cycle basis.

## 3. D1 — the z-store (per constellation×signal component)

- State: ẑ (m), Pᶻ — information or covariance form (implementer's choice; covariance form matches the small-block ops T·Pᶻ·Tᵀ and marginalization). Size: per (const,σ), E_cσ − (n_c+m_c−1); network-wide ≈ #arcs − #vertices ≈ **1400 at 50 sta / 2 const / 3σ** — dense Pᶻ ≈ tens of MB, fine. Per-component blocks keep ops local.
- Keys: a chord is (r, s) with its fundamental cycle defined by the CURRENT tree; store chord identity (station, sat, σ) + the cycle's edge list (or recompute from the tree on demand — the Card-3 tree machinery is the selector and STAYS).
- Dynamics: Φ = I, q_z = 0 (ambiguities constant). Cycle slips: refloat = delete + re-tap the affected z entries (slip detectors already localize the (station,sat,σ)); a slip on a TREE edge additionally forces a tree swap (unimodular T) before the refloat.
- The GKF estimability gate: per epoch require [F_t, A_tᵀ] full column rank — operationally, every chord's 4 ambiguity columns must be present in the epoch's reduced system; a chord whose arc has no phase row this epoch is simply not measured that epoch (prediction-only, allowed) — NOT deleted until the arc/state actually dies (extend-only spirit, roadmap I2).

## 4. D2 — what stays unlinked (u), what keeps existing dynamics, what is RETIRED

- Freshly estimated each epoch (u): satellite phase biases b_s, receiver phase biases δ_r (RPB states STAY — they make z truly integer per Card 3 D1), tree-edge ambiguities (pinned, see D4), receiver clocks.
- Existing dynamic treatments RETAINED (physical, estimable quantities): SH-VTEC coefficient dynamics, ZWD carry, clock oscillator model (clock_model), Card-2 time-linked code datum. Card 4 v1 scope is the PHASE/AMBIGUITY layer only.
- **RETIRED when Card 4 is armed (gated, not deleted):** D2/C1 shared PB carry (PhaseBias kind only — Clock/CodeBias entries unaffected), phase-leveling per-key local carry as a SOLVE input (may live on as diagnostics), rpb_carry, the eq34 reref/erase/skip hygiene, pin-to-round. The Card-1/1d/1e coupling machinery is bypassed for PhaseBias keys under the toggle.

## 5. D3 — folding the DD priors (the one structural change)

A DD prior row touches 4 local ambiguity columns at TWO stations: a cross-local-block row, which the current border solve (stations independent given the shared border) cannot represent. Three options, assessed:

- **(i) RECOMMENDED — ADJUDICATED by the phase-A scout: a JOINT rank-K per-component Woodbury fold on the retained reduction factors.** The DD/chord constraint is gauge-free (only local ambiguity columns; no shared column) — exactly the `SumZeroGauge::terms` (station, local_col) shape. Ambiguity columns are Schur-ELIMINATED post-reduce (not "addressable" as previously worded), but the kernel retains per-station N_sr / u_r / N_rr-LDLᵀ, and `foldReducedSumZeroGauges` (NetworkNormalEquations.cpp:522–590) already folds a cross-station local-column constraint post-reduce EXACTLY via Woodbury from those factors, no re-reduce. Generalize rank-1 → rank-K: Ñ_ss += G (Pᶻ + B)⁻¹ Gᵀ with B = Cᴸᵀ N_LL⁻¹ Cᴸ, folded JOINTLY per component — **fundamental cycles SHARE tree edges and Pᶻ is dense, so independent rank-1 scalar folds are INEXACT** (the scout's adversarial pass caught this). Needs: `SumZeroGauge::terms` extended with signed coefficients + multiple columns per station; store the K×K (Pᶻ+B) factor and G for the post-solve z reconstruction. Per-component K bounds the cost — no O(n³) border blowup.
- (ii) Border augmentation: add z as shared border states + hard consistency rows per station pair. Rejected v1: border grows by ~#chords (≫ current border), O(n³) blowup — the D6-v2 cost class again.
- (iii) Station-pair pre-elimination (form explicit between-station DDs of the observations). Rejected: reintroduces correlated-obs bookkeeping and breaks the per-station assembly (the BSSD cost, network-wide).

The auxiliary-column fallback is RETIRED by the phase-A scout: option (i) is buildable exactly on existing retained-factor machinery (foldReducedSumZeroGauges is the unique cross-station injection path; Card-1's stationRegression/addStationPrior are single-station-on-border and NOT templates for this).

## 6. D4 — per-epoch datum and the emission frame (the consumer contract)

- The per-epoch solve still pins a tree (Card 3 election, sticky machinery retained) — but targets are plain 0 every epoch and NO cross-epoch meaning attaches to the realized frame. All Card-3 frame-continuity machinery (pin-to-round, reref detection, carry hygiene) is unnecessary under the toggle.
- **Wire continuity moves to the EMIT layer** (zero filter impact): after the solve, re-base the emitted per-key biases onto the previous EMISSION frame — per (sat,σ): subtract the integer×λ (and per component: the common fractional δ-shift) that minimizes the step vs the last emitted value. An S-selection applied to outputs only; user DDs are invariant to it. The emitter's existing 0.05 m jump test remains the disc authority; steps now occur only at genuine events (slips on emission-reference paths, component splits), not at datum churn. **Built — see §10.**
- Product contract unchanged (v2 S line, amb_datum=SingleReceiverChain, signal_integer semantics). Consumer needs nothing new.

## 7. D5 QC instruments; D6 interactions; D7 config

### D5. QC instruments (from the 2021 paper)

- Innovation testing on z only (Def. 1 predicted residuals): v = ẑ_meas − ẑ_pred with its formal covariance — a principled per-DD slip/outlier test replacing carry-QC heuristics. Wire into the existing DIA/RAIM reporting.
- Optional (config): Lemma-1 parallel actual-covariance recursion — the honesty audit for q/R misspecification; cheap at z-dimension. Also serves M2 (LS-VCE) as its validation instrument.
- Counters: z-dim per component, chords born/died, tree swaps (T applications), tap-off inits, prediction-only chords — one run-end line.

### D6. Interactions and retirements

- Card 1/1d/1e: inert for PhaseBias keys when armed (toggle-gated bypass); Clock/CodeBias carry paths untouched. tuGaugeRestore unaffected (no new gauge-weighted rows: DD rows are gauge-FREE by construction).
- Card 3 machinery retained: RPB states + rank-floor prior (D1 needs them for integer z), the sticky tree (as chord/tree selector), the pin fold (targets 0), the union shared set, W5 gate (declared dim unchanged — DD prior rows are datum-free, no null-space footprint).
- eq34 sub-toggles pin_round / carry_shift: superseded when armed; keep for A/B.
- Self-AR (roadmap M5) plugs in naturally: MLAMBDA on ẑ, Pᶻ directly — the z-store IS the float DD solution the producer-AR needs; re-pin fixed DDs as hard z-priors.

### D7. Config (repo conventions)

```
network.carry_invariant            bool  false  master; requires phase_datum_eq34 (needs RPB split + tree); supersedes/bypasses PB shared-carry + PL local carry + eq34 hygiene for phase keys
network.carry_invariant_qz         double 0.0   z process noise (m²/s); 0 = constant ambiguities (correct absent slips)
network.carry_invariant_emit_rebase bool  true  emit-layer frame re-basing (integer + common-fractional continuity vs last emission)
network.carry_invariant_honesty    bool  false  Lemma-1 parallel actual-covariance instrument
```

Load rules: `carry_invariant && !phase_datum_eq34` → throw (needs the tree + RPB). Warn-INERT list: pin_round, carry_shift, rpb_q (subsumed). Byte-identity OFF is structural (no z-store allocated, all folds gated). *(As built: the requirement is enforced at `GnssConfigLoadNetwork.cpp:6187`; the parsed defaults match this table — `GnssConfig.hpp:5985-5987`, `GnssConfigLoadNetwork.cpp:4572-4581`.)*

## 8. Unit gate battery

In `tests/test_network_invariant_carry_driver.cpp` (gates 1–4, 6) and `tests/test_card4_emit_rebase.cpp` (gate 5).

1. **CycleBasisUnimodular** — synthetic component; kill a tree edge; assert T ∈ ℤ, |det T| = 1, and ẑ'/Pᶻ' equal a from-scratch re-solve.
2. **ChordLifecycle** — birth (tap-off init matches conditional mean/var), prediction-only epoch, death (marginalization matches Schur).
3. **FilterEqualsBatch** — 3-epoch fixture: Card-4 recursion == batch LS with ambiguity constancy constraints (the GKF equivalence, our recursive==batch gate-test pattern from Card 1).
4. **DatumFreedom** — same fixture, two different per-epoch tree realizations ⇒ identical ẑ, Pᶻ, and identical user-DD-invariant emissions after re-basing (the falsifier of the whole concept).
5. **EmitRebaseInteger** — emitted per-key steps across a forced tree change are 0 after re-basing; disc counter silent. Built as phase D's gate 5, §10.
6. Config-parse regression + suite green + OFF byte-identity hash.

## 9. Field falsifier — MEASURED (3 h eq34 lineage, Release, reproduced by Fable)

Specced target: 3 h golden lineage, `carry_invariant` on, datum-invariant DD step med → the **0.085 cyc/ep golden class** (the instrument that condemned Card 3's arms), with run-end counters showing tree swaps ≈ arc-death rate and zero refloat storms. A 12 h window only after/with M2-M3 (PL-floor divergence is a separate axis). Measured:

| arm | DD step (cyc/ep) | a00 tail | calib |
|---|---|---|---|
| control (legacy eq34) | 8.29 | −96 ± 110 | 22.3× (σ-incest) |
| carry ON, q_z = 0 | 2.433 | +11.4 ± 13.1 | 0.82 |
| carry ON, q_z = 1e-4 | **1.498** | +22.1 ± 3.1 | 0.49 |

**FIELD CONFIG DECISION: `carry_invariant_qz = 1e-4`** — it beats q_z = 0 post-slip-fix (1.50 vs 2.43 cyc/ep; a00 22.1 ± 3.1). This is the shipped value. Residual gap to the golden 0.085 class = honest shared-param σ-floor + genuine new-arc births. Those are **separate axes and explicitly NOT phase-D scope**; the consumer A/B (§11) is the arbiter of whether the gap matters.

## 10. Phase D as built — emit-layer re-basing + gate 5

*(Absorbed from `HANDOFF_card4_phaseD.md`, Fable-specced 2026-07-12 after the C3c gate PASS @967def1; lineage in memory `project-m1-card4-phasec3c`. That doc's §1 = this section's "wiring" block, its §2 = the "gate 5" block. Suite baseline at the time: 674/1. The 6 h/12 h hr-4 cliff-immunity runs were in flight and phase D did NOT gate on them.)*

**Wiring (was phase-D §1).** `carry_invariant_emit_rebase` — parsed since C3b, inert until phase D — is wired exactly as §6/D4 specifies, with one semantic D4 left open: **component death/rebirth — a component absent from the previous emission re-bases against nothing (fresh frame, disc fires once — correct).**

**Gate 5 / EmitRebaseInteger, with two negative controls (was phase-D §2).** A forced tree change through the PRODUCTION driver + emitter seam ⇒ emitted per-key steps exactly 0 after re-basing and the disc counter silent. Negative control 1: **re-base disabled ⇒ integer-class (≥ 1 λ) steps visible on the same assertion** — a numerical fail, not a throw. Negative control 2: **a slip on the emission-reference path ⇒ the step survives and the disc fires** — the re-base must NOT launder genuine events, and the gate asserts one.

**Counters.** Emitted re-base integer total + common-fractional shift per component, on the run-end Card-4 line — phase-D telemetry for the consumer A/B.

**OFF-path definition for this knob.** `emit_rebase` defaults **TRUE** when `carry_invariant` is armed, so the byte-identical OFF path is `carry_invariant = false`, NOT `emit_rebase = false`. Deferral triage per standing invariant #3.

**Out of scope for phase D, with reasons:** the honesty instrument (Lemma-1); prediction-only conditioning (field-inert, DATA-confirmed); GLONASS IE-FDMA; the 0.085-gap election levers.

## 11. Consumer A/B — acceptance and configs

*(Absorbed from `HANDOFF_card4_phaseD.md`.)* Consumer A/B with network iono supply: producer = eq34 + `carry_invariant` (q_z = 1e-4) + `emit_rebase`; staged consumer configs `_clAiono_pivot_3h` / `_clBiono_eq34_3h`. **Acceptance: MLAMBDA opens (ratio > 0) on the Card-4 arm, and consumer vertical does not regress vs the M4 bias-only baseline.**

## 12. Sequencing and open questions

- Build after roadmap I1–I4 outcomes (I2 extend-only membership reduces churn Card 4 must survive; I1 settles the B8 algebra question for the record). Independent of M2 (LS-VCE) — but wire the D5 honesty instrument so M2 has its measurement.
- OPEN-1: ✅ RESOLVED (phase-A scout) — see D3 option (i): joint rank-K per-component Woodbury on retained factors; aux fallback retired.
- OPEN-2: ✅ RESOLVED (phase-A scout) — **merge is the EASY direction: the bridge is a cut edge, so every pre-existing chord's fundamental cycle is UNCHANGED (T = block-IDENTITY, stronger than unimodular)**; carried ẑ/Pᶻ transfer verbatim; one bridge→tree edge, surplus bridges→new chords via tap-off. **Component id is PER-EPOCH CANONICAL (min sat label** — anchor on satellite labels, receiver pool indices are reused), not cross-epoch stable; **the z-store is keyed by CHORD identity, never component label.** An r1 re-root at merge forces NO z transform (cycles are fixed by the edge SET, not the root); any co-occurring tree-edge death is a separate D1 unimodular swap.
- OPEN-3: GLONASS — z entries for FDMA via the IE-FDMA (Λ⊗L)Ẑ transform (literature pack §7) — v2, after CDMA fields prove out.
- OPEN-4: warm-resume — snapshot ẑ/Pᶻ (add to NetworkStateSnapshot, fingerprint token `carry:inv`); a resume across toggle flip cold-starts. **Executed** — see `SPEC_warm_resume_event_card.md`.

## 13. Successors

- `docs/SPEC_pd_safe_dd_fold.md` — the fold's PD safety (the D3 arithmetic went indefinite in the field; `network.carry_invariant_pd_safe`).
- `docs/SPEC_carry_aware_covariance.md` — the D5 readback half: the covariance path was carry-blind (`network.carry_aware_covariance`).
- `docs/SPEC_carry_factored_pz_C2.md` — representation/perf and the INC-C2 impossibility proof (no exact bounded factored carry for dense Σ_ss).
- `docs/SPEC_warm_resume_event_card.md` — OPEN-4's serialize/resume outcome.

⚠ **Label note.** This doc's design labels are **D1–D7**. The perf spec's option labels are **D-a/D-b/D-c** and live in `SPEC_carry_factored_pz_C2.md`; the families are kept in separate files so a code citation of `D-c` is unambiguous. Do not introduce `D-a/D-b/D-c` here.
