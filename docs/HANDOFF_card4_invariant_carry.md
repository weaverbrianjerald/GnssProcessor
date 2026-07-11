# CARD 4 — Invariant-function carry: the generalized-LS filter seam

*Fable-specced 2026-07-11, from the Card 3 field campaign (memory
`card3-field-grading-jul10`: 12-arm ladder, eq34 producer stuck 9–34 cyc/ep vs
golden 0.085) and the 8-paper theory pack (memory `ppprtk-literature-pack`).
Foundations: Teunissen/Khodabandeh/Psychas 2021 (generalized Kalman filter,
J Geod 95:108) and Hou & Zhang 2025 (generalized least-squares filter, J Geod
99:3). Design handoff — NOT built. Supersedes the D6-v2 border-promotion idea
of `HANDOFF_eq34_datum_card3.md` and, when armed, most of the carry hygiene
built during the Jul-10/11 campaign.*

## The defect (why every Card-3 stabilization arm failed)

Our time-linking machinery (D2/C1 shared PB carry, phase-leveling local carry,
rpb_carry) attaches dynamic models to **S-dependent raw states**: per-key
ambiguity floats, per-key satellite phase biases, per-station RPBs. Every one
of these quantities changes meaning when the S-basis changes — and at
50-station global scale the basis changes ~7×/epoch from satellite-pass
physics (measured; irreducible by election policy). Each basis event either
poisons the carried priors (the 03:46-class silent re-level: stiff prior ×
frame step; Psychas 2024b Fig. 3 shows jump impact *scales with* dynamic-model
stiffness) or forces an erase (refloat storm; the erase→no-carry→hard-re-pin
vicious cycle measured in arms B4–B7). The exact S-transformation of the
carries (arm B8) is theoretically correct (Odijk 2016's own prescription for
cross-basis comparison) but proved fragile in the full model — and even if
fixed, it is a *repair* executed ~7×/epoch forever.

de Jonge (1998, §3.5) never faced this: his S-basis is extend-only inside a
batch adjustment where dead basis columns persist through their historical
epochs. The literature has **no running-filter solution** — Teunissen et al.
2021 provide the framework (dynamics on selected linear functions
z_i = F_iᵀx_i; everything else estimated fresh, exactly decoupled) but leave
Φᶻ across a changing linked set unconstructed. Supplying that construction,
with z chosen so the transition is trivial, is Card 4.

## The theory in three lines

Choose the time-linked functions z to be **S-system invariants**: double-
differenced (DD) ambiguities. DDs are estimable under every datum, unaffected
by any pivot/tree/handover choice, integer-valued, and constant in time
(q_z = 0 between slips). Then the datum can be re-realized *freely every
epoch* — nothing the filter remembers depends on it — and basis churn becomes
a non-event by construction, not by hygiene.

The cycle-space identification makes Φᶻ trivial: for the (const,σ) bipartite
observation graph with E arcs and a spanning tree of n_c+m_c−1 edges, the
independent DDs are exactly the **fundamental cycles of the non-tree arcs
(chords)** — dimension E − (n_c+m_c−1) per component. Arc events map to:

- **chord death** → delete that z entry (drop row/col of Pᶻ — pure
  marginalization, no transform);
- **chord birth** → append a z entry, initialized by tap-off
  ẑ_new = fᵀx̂ (GKF Eq. 22 pattern) with its conditional variance;
- **tree-edge death** → swap a chord into the tree; the fundamental-cycle
  basis change is a **unimodular integer matrix T**: z' = Tz, Pᶻ' = TPᶻTᵀ —
  exact, integer, zero information loss. This IS the Φᶻ construction the 2021
  paper left open, and it is trivial *because* z is the cycle basis.

## Design

### D1. The z-store (per constellation×signal component)

- State: ẑ (m), Pᶻ — information or covariance form (implementer's choice;
  covariance form matches the small-block ops T·Pᶻ·Tᵀ and marginalization).
  Size: per (const,σ), E_cσ − (n_c+m_c−1); network-wide ≈ #arcs − #vertices ≈
  1400 at 50 sta / 2 const / 3σ — dense Pᶻ ≈ tens of MB, fine. Per-component
  blocks keep ops local.
- Keys: a chord is (r, s) with its fundamental cycle defined by the CURRENT
  tree; store chord identity (station, sat, σ) + the cycle's edge list (or
  recompute from the tree on demand — the tree machinery from Card 3 is the
  selector and STAYS).
- Dynamics: Φ = I, q_z = 0 (ambiguities constant). Cycle slips: refloat =
  delete + re-tap the affected z entries (slip detectors already localize the
  (station,sat,σ)); a slip on a TREE edge additionally forces a tree swap
  (unimodular T) before the refloat.
- The GKF estimability gate: per epoch require [F_t, A_tᵀ] full column rank —
  operationally, every chord's 4 ambiguity columns must be present in the
  epoch's reduced system; a chord whose arc has no phase row this epoch is
  simply not measured that epoch (prediction-only, allowed) — NOT deleted
  until the arc/state actually dies (extend-only spirit, roadmap I2).

### D2. What stays unlinked (u) and what keeps existing dynamics

- Freshly estimated each epoch (u): satellite phase biases b_s, receiver
  phase biases δ_r (RPB states STAY — they make z truly integer per Card 3
  D1), tree-edge ambiguities (pinned, see D4), receiver clocks.
- Existing dynamic treatments RETAINED (they model physical, estimable
  quantities): SH-VTEC coefficient dynamics, ZWD carry, clock oscillator
  model (clock_model), Card-2 time-linked code datum. Card 4 v1 scope is the
  PHASE/AMBIGUITY layer only.
- RETIRED when Card 4 is armed (gated, not deleted): D2/C1 shared PB carry
  (PhaseBias kind only — Clock/CodeBias entries unaffected), phase-leveling
  per-key local carry as a SOLVE input (it may live on as diagnostics),
  rpb_carry, the eq34 reref/erase/skip hygiene, pin-to-round. The Card-1/1d/1e
  coupling machinery is bypassed for PhaseBias keys under the toggle.

### D3. Folding the DD priors — the one structural change

A DD prior row touches 4 local ambiguity columns at TWO stations: it is a
cross-local-block row, which the current border solve (stations independent
given the shared border) cannot represent. Three options, assessed:

- **(i) RECOMMENDED — sparse cross-block prior rows in the reduce step.**
  Extend NeqAccumulator/the reduction with a "coupling rows" pass that folds
  rows spanning two stations' local blocks (plus nothing shared). Cost: the
  two blocks' reduction couples — handle by folding DD priors AFTER per-
  station reduction, on the RETAINED ambiguity columns (the reduction already
  keeps ambiguity columns for the pin fold; verify splitStation exposes them
  post-reduce). Exactness preserved; implementation is a bounded extension of
  existing machinery.
- (ii) Border augmentation: add z as shared border states + hard consistency
  rows per station pair. Rejected v1: border grows by ~#chords (≫ current
  border), O(n³) blowup — the D6-v2 cost class again.
- (iii) Station-pair pre-elimination (form explicit between-station DDs of
  the observations). Rejected: reintroduces correlated-obs bookkeeping and
  breaks the per-station assembly (the BSSD cost, network-wide).

v1 fallback if (i) proves invasive: fold the DD prior as TWO half-rows via an
auxiliary per-DD nuisance column (splits the 4-column row into 2×2-column
rows sharing the auxiliary) — algebraically exact, keeps blocks separable,
+1 local column per chord at one station.

### D4. Per-epoch datum and the emission frame (the consumer contract)

- The per-epoch solve still pins a tree (Card 3 election, sticky machinery
  retained) — but targets are plain 0 every epoch and NO cross-epoch meaning
  attaches to the realized frame. All Card-3 frame-continuity machinery
  (pin-to-round, reref detection, carry hygiene) is unnecessary under the
  toggle.
- **Wire continuity moves to the EMIT layer** (zero filter impact): after the
  solve, re-base the emitted per-key biases onto the previous EMISSION frame —
  per (sat,σ): subtract the integer×λ (and per component: the common
  fractional δ-shift) that minimizes the step vs the last emitted value.
  This is an S-selection applied to outputs only; user DDs are invariant to
  it. The emitter's existing 0.05 m jump test remains the disc authority;
  steps now occur only at genuine events (slips on emission-reference paths,
  component splits), not at datum churn.
- Product contract unchanged (v2 S line, amb_datum=SingleReceiverChain,
  signal_integer semantics). Consumer needs nothing new.

### D5. QC instruments (from the 2021 paper)

- Innovation testing on z only (Def. 1 predicted residuals): v = ẑ_meas −
  ẑ_pred with its formal covariance — a principled per-DD slip/outlier test
  replacing carry-QC heuristics. Wire into the existing DIA/RAIM reporting.
- Optional (config): Lemma-1 parallel actual-covariance recursion — the
  honesty audit for q/R misspecification; cheap at z-dimension. Also serves
  M2 (LS-VCE) as its validation instrument.
- Counters: z-dim per component, chords born/died, tree swaps (T
  applications), tap-off inits, prediction-only chords — one run-end line.

### D6. Interactions and retirements

- Card 1/1d/1e: inert for PhaseBias keys when armed (toggle-gated bypass);
  Clock/CodeBias carry paths untouched. tuGaugeRestore unaffected (no new
  gauge-weighted rows: DD rows are gauge-FREE by construction).
- Card 3 machinery retained: RPB states + rank-floor prior (D1 needs them for
  integer z), the sticky tree (as chord/tree selector), the pin fold
  (targets 0), the union shared set, W5 gate (declared dim unchanged — DD
  prior rows are datum-free, no null-space footprint).
- eq34 sub-toggles pin_round / carry_shift: superseded when armed; keep for
  A/B.
- Self-AR (roadmap M5) plugs in naturally: MLAMBDA on ẑ, Pᶻ directly — the
  z-store IS the float DD solution the producer-AR needs; re-pin fixed DDs as
  hard z-priors.

### D7. Config (repo conventions)

```
network.carry_invariant            bool  false  master; requires phase_datum_eq34 (needs RPB split + tree); supersedes/bypasses PB shared-carry + PL local carry + eq34 hygiene for phase keys
network.carry_invariant_qz         double 0.0   z process noise (m²/s); 0 = constant ambiguities (correct absent slips)
network.carry_invariant_emit_rebase bool  true  emit-layer frame re-basing (integer + common-fractional continuity vs last emission)
network.carry_invariant_honesty    bool  false  Lemma-1 parallel actual-covariance instrument
```
Load rules: `carry_invariant && !phase_datum_eq34` → throw (needs the tree +
RPB). Warn-INERT list: pin_round, carry_shift, rpb_q (subsumed). Byte-identity
OFF is structural (no z-store allocated, all folds gated).

## Tests (gate)

1. **CycleBasisUnimodular** — synthetic component; kill a tree edge; assert
   T ∈ ℤ, |det T| = 1, and ẑ'/Pᶻ' equal a from-scratch re-solve.
2. **ChordLifecycle** — birth (tap-off init matches conditional mean/var),
   prediction-only epoch, death (marginalization matches Schur).
3. **FilterEqualsBatch** — 3-epoch fixture: Card-4 recursion == batch LS with
   ambiguity constancy constraints (the GKF equivalence, our recursive==batch
   gate-test pattern from Card 1).
4. **DatumFreedom** — same fixture, two different per-epoch tree realizations
   ⇒ identical ẑ, Pᶻ, and identical user-DD-invariant emissions after
   re-basing (the falsifier of the whole concept).
5. **EmitRebaseInteger** — emitted per-key steps across a forced tree change
   are 0 after re-basing; disc counter silent.
6. Config-parse regression + suite green + OFF byte-identity hash.

## Falsifier (field)

3h golden lineage, `carry_invariant` on: datum-invariant DD step med → the
**0.085 cyc/ep golden class** (the instrument that condemned Card 3's arms);
run-end counters show tree swaps ≈ arc-death rate with zero refloat storms;
then consumer A/B with iono supply (configs staged) — MLAMBDA opens (ratio>0)
on the eq34+Card4 arm. 12h window only after/with M2-M3 (PL-floor divergence
is a separate axis).

## Sequencing & open questions

- Build after roadmap I1–I4 outcomes (I2 extend-only membership reduces churn
  Card 4 must survive; I1 settles the B8 algebra question for the record).
  Independent of M2 (LS-VCE) — but wire the D5 honesty instrument so M2 has
  its measurement.
- OPEN-1: reduction-layer contract for (i) — verify post-reduce ambiguity
  columns are addressable across station pairs; else fall back to the
  auxiliary-column trick (D3).
- OPEN-2: component merge (two z-stores unify): union tree + re-derived
  chord set; T is block-unimodular; spec the deterministic merge order.
- OPEN-3: GLONASS — z entries for FDMA via the IE-FDMA (Λ⊗L)Ẑ transform
  (literature pack §7) — v2, after CDMA fields prove out.
- OPEN-4: warm-resume — snapshot ẑ/Pᶻ (add to NetworkStateSnapshot,
  fingerprint token `carry:inv`); a resume across toggle flip cold-starts.
