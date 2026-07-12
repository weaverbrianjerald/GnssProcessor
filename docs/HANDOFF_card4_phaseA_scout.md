# CARD 4 (M1) — Phase-A scout report: OPEN-1 & OPEN-2

*Opus scout, 2026-07-11. READ-ONLY — nothing built, nothing edited. Answers the
two open questions the Card-4 spec (`docs/HANDOFF_card4_invariant_carry.md`,
"Sequencing & open questions") gated phase B/C on. Findings are grounded in
direct reads of the current tree (uncommitted on top of e9eda1d) and were
adversarially cross-checked by a 3-agent verification pass (2 refuters + 1
completeness sweep, workflow `card4-scout-verify`). This report is an input to
Fable's adjudication; the "Recommendation" and "Spec-correction punch list"
sections are proposals, not decisions.*

---

## TL;DR for the adjudicator

- **OPEN-1 → BUILD OPTION (i). The D3 auxiliary-column fallback is NOT needed.**
  A cross-station DD prior row is *gauge-free* (touches only LOCAL ambiguity
  columns, no shared column) and is exactly foldable **post-reduce** through the
  per-station factors the kernel already retains (`N_sr`, `u_r`, `N_rr` LDLᵀ).
  The existing `foldReducedSumZeroGauges` is the living proof + template: it
  already folds a constraint coupling local ambiguity columns *across stations*
  via a Woodbury update, exactly, with no re-reduce and no shared column.
  **One material correction to the spec's framing** (caught by the adversarial
  pass): the DD-prior fold is a **joint rank-K Woodbury**, not a loop of
  independent rank-1 scalar folds. Details in §OPEN-1.

- **OPEN-2 → merge is the EASY direction; T is block-IDENTITY on carried z.**
  Merging two components via a bridge leaves every pre-existing chord's
  fundamental cycle unchanged (the bridge is a cut edge — a graph theorem, not
  an approximation). Union the two z-blocks, make one bridge a tree edge, tap-off
  surplus bridges as new chords; re-key nothing. The only new machinery is a
  **per-epoch canonical component id** (not a cross-epoch-stable one — the z-store
  is keyed by chord, so it needs none) + a deterministic simultaneous-merge fold
  order. Details in §OPEN-2.

Neither verifier found a blocker. OPEN-1 came back PARTIAL (headline confirmed,
characterization sharpened); OPEN-2 and the completeness sweep came back
CONFIRMED.

---

## Architecture ground truth (verified, load-bearing for both answers)

The network producer is an **arrowhead** system
(`NetworkNormalEquations.hpp:14-52`): a shared satellite-parameter border `x_s`
(sat clock / phase bias / code bias / SH-VTEC) plus per-station **local** blocks
`x_r` (rx clock, ISB, ZWD, slant iono, **ambiguities**, RPB). Station-station
off-diagonals are exactly zero — stations couple **only** through `x_s`.

- `splitStation` (`ApplicationNetwork.cpp:2433-2553`) routes every
  non-position/non-shared observable column into the local block `A_r`, and
  records `h2local : H-column → A_r-column` (`:2519`). Satellite phase bias goes
  to the **shared** border `A_s` (`:2451-2459`); the receiver–sat **ambiguity**
  goes to **local** `A_r`. So an ambiguity is addressed post-split as
  `(station, local_col)` via that station's `h2local`.
- `reduce()` Schur-eliminates each station's local block onto the border
  (`NetworkNormalEquations.hpp:29-46`). **Post-reduce the ambiguity columns do
  not exist in `reduced_N_ss_`** — they are subtracted out. But the kernel
  **retains** each station's `N_sr`, `u_r`, and the `N_rr` LDLᵀ factor (the
  `Station` struct, `NetworkNormalEquations.hpp:683-688`), which is what makes
  post-reduce reconstruction and folding exact.

---

## OPEN-1 — cross-block DD-prior addressability → **option (i), rank-K joint fold**

**Question (spec):** "verify post-reduce ambiguity columns are addressable
across station pairs; else fall back to the auxiliary-column trick (D3)."

### Verdict: option (i) is feasible and exact; the D3 fallback is unnecessary.

Evidence, point by point:

1. **The reduction already folds a cross-station, local-column constraint —
   post-reduce, exactly, from the retained factors.** `foldReducedSumZeroGauges`
   (`NetworkNormalEquations.cpp:522-590`) takes `SumZeroGauge::terms`, an
   arbitrary-length list of `(station, local_col)` pairs spanning any number of
   stations (`NetworkNormalEquations.hpp:282-295`), and folds
   `Ñ_ss += g gᵀ/β`, `ũ_s += g·γ/β` with `n_r = N_rr⁻¹ e_col`, `β = Σ_r (n_r)_col`,
   `g = Σ_r N_sr n_r`, `γ = Σ_r (N_rr⁻¹ u_r)_col` — computed entirely from the
   cached per-station LDLᵀ factors, and stored `(g,β,γ)+terms` drive a matching
   `reconstructStation` correction (`:666-683`). This is the exact
   Sherman-Morrison Schur complement of adding a constraint to the block-diagonal
   local normals. It is **exercised and proven**: `SumZeroPhaseDatumRecovers…`
   (`tests/test_network_estimator.cpp:206-251`) matches the exact ensemble
   S-transform to 1e-6, and `Eq34UserIntegerRecovery…` (`:309-407`) proves the
   integer semantics.

2. **A DD/chord constraint is gauge-free — it touches only local ambiguity
   columns, never a shared column.** The eq34 phase row is
   `−clkˢ + pbˢ` (shared) `+ δ_r + zˢ_r` (local). A double difference of the
   ambiguity *states* `(zˢ_r − zᵖ_r) − (zˢ_q − zᵖ_q)` analytically cancels the
   receiver bias (inner SD) and the satellite phase bias (outer difference),
   leaving `±1` on **four local columns only**. The z-store is defined on these
   DD ambiguity floats (spec D1). So the constraint has the exact
   `(station, local_col)` shape `SumZeroGauge::terms` already carries — no shared
   column, confirming D6's "DD rows are gauge-FREE by construction."

3. **The post-reduce fold does not trip the reduce-once invariant, and the
   distinction from `addStationPrior` is real and carries over.**
   `foldReducedSumZeroGauges` mutates only `reduced_N_ss_/reduced_u_s_` and keeps
   the per-station factors **pristine** (storing the correction separately), so
   `reduce_recompute_count_` is untouched. By contrast `addStationPrior`
   rank-updates `st.Nrr_ldlt`/`st.u_r` in place and sets
   `reduced_ = false` (`NetworkNormalEquations.cpp:400`, "the station's Schur
   subtrahend changed → reduce() must rerun") — precisely because it wants all
   downstream consumers to see the mutated factor. A DD prior wants the
   gauge-style pristine-factor path and inherits its no-re-reduce property.

4. **Addressability across station pairs is already wired.** `h2local` is stored
   per station in the assembly result and consumed **post-reduce** at
   `ApplicationNetwork.cpp:7647, 8786, 8817, 8924, 9110, 9253`;
   `jointConditionalCovariance` (`:7717, :9036`) already reads cross-station
   `(station, local_col)` DD requests to attach DD formal σ. The read-side of the
   exact machinery this fold needs is in production today.

5. **No competing template, and the fallback is not pre-present** (completeness
   sweep). Card-1 `stationRegression`/`carry_conditional_coupling`
   (`NetworkNormalEquations.hpp:474-510`, used `ApplicationNetwork.cpp:9375-9413`)
   regresses **one** local column on the **shared border** — single-station, not
   cross-station. `jointConditionalCovariance` is **read-only** (covariance
   harvest; injects nothing). `foldReducedSumZeroGauges` is the **unique**
   cross-station injection path. No per-DD auxiliary/nuisance column exists in the
   tree (grep clean; D3-fallback line only in the spec).

### The one material correction: it is a **rank-K joint** fold, not rank-1 scalars

My phase-A scout claim ("bounded generalization = signed coeffs + finite weight
`1+wβ` + nonzero target, on the existing per-gauge loop") is **over-optimistic
in its detail**, and the adversarial refuter caught why:

- The shipped fold processes each gauge **independently** with its own scalar `β`.
  That is exact **only** because those gauges are **hard (w→∞) constraints on
  UNOBSERVABLE gauge directions** — independent folding then merely picks a datum
  representative and leaves estimable quantities invariant.
- A Card-4 DD prior is a **soft, informative** constraint on **estimable** DD
  ambiguities. Fundamental cycles **share tree edges**, so their constraint rows
  `C` share ambiguity columns, giving a **non-diagonal** coupling
  `B = Cᴸᵀ N_LL⁻¹ Cᴸ`; and the carried `Pᶻ` is **dense** (spec D1: "dense Pᶻ ≈
  tens of MB"). Folding these rows independently would double-count / mis-couple
  the shared columns — **numerically inexact**.
- The exact fold is a **single rank-K Woodbury** with a K×K middle inverse:

  ```
  Ñ_ss += G (Pᶻ + B)⁻¹ Gᵀ ,     ũ_ss += G (Pᶻ + B)⁻¹ (ẑ − …)
  G  = Σ_stations N_sr · N_rr⁻¹ · Cᴸ,r        (n_shared × K)
  B  = Σ_stations Cᴸ,rᵀ · N_rr⁻¹ · Cᴸ,r       (K × K)
  ```

  As `Pᶻ→∞` (hard limit), K=1, unit coefficient, target 0, this collapses to the
  shipped `g gᵀ/β`. So it is the **same retained-factor machinery generalized
  from rank-1 to rank-K**, not a different mechanism — but the extension is
  larger than "(a)(b)(c) on the existing loop."

- **This decomposes per (const,signal) component**, which is exactly the spec's
  "per-component blocks keep ops local." Within a `(const,σ)` group the components
  are disjoint vertex sets, so they share no station/column; `B` and `Pᶻ` are
  block-diagonal across components, and the K×K solve is **per-component**
  (K = that component's chord count), bounded — no global O(n³) blowup, unlike
  the rejected option (ii).

### Recommendation to Fable (OPEN-1)

- **Adopt option (i).** Reject the D3 auxiliary-column fallback — it is
  unnecessary and strictly more invasive (+1 local column per chord, a
  `splitStation`/state-vector change) than extending the fold the kernel already
  ships.
- **Spec the reduce-layer extension as a joint rank-K per-component Woodbury
  fold** (`Ñ_ss += G(Pᶻ+B)⁻¹Gᵀ`), building on `foldReducedSumZeroGauges`'
  structure (pristine factors + stored correction for `reconstructStation`), not
  as an independent per-DD scalar loop. Store the K×K factor of `(Pᶻ+B)` and `G`
  per component for the reconstruction back-substitution.
- **Extend `SumZeroGauge::terms`** (or a new sibling struct) with a **per-term
  signed coefficient** and allow **multiple columns per station** in one term
  group, so `β_r = Cᴸ,rᵀ N_rr⁻¹ Cᴸ,r` picks up the intra-station cross terms of a
  4-column DD sharing a station.
- **PD/exactness confirmed:** the added term is PSD, `β>0` for any nonzero
  coefficient vector (quadratic form of PD `N_rr⁻¹`), and `Pᶻ+B` is PD, so the
  middle inverse never degenerates and reconstruction has no sign trap.

---

## OPEN-2 — deterministic z-store merge order → **union + tap-off, T = block-identity**

**Question (spec):** "component merge (two z-stores unify): union tree +
re-derived chord set; T is block-unimodular; spec the deterministic merge order."

### Verdict: merge is the easy direction. T is block-IDENTITY on carried z.

1. **Block-identity is a theorem, not an approximation.** Merged
   `T = T_A ∪ T_B ∪ {bridge}`. Because `T` is a tree and `T_A`/`T_B` each span
   their own vertex sets, exactly **one** tree edge crosses the A/B partition (a
   second crossing would close a cycle) — the bridge is a **cut edge**. A simple
   tree path between two vertices both in A cannot traverse a cut edge, so it
   stays inside `T_A`. Hence every pre-existing A-chord's fundamental cycle is
   **unchanged**, and likewise for B. Carried `ẑ`/`Pᶻ` entries of both components
   carry over **verbatim** — the transform on them is **identity** (a stronger
   statement than the spec's "block-unimodular"). Dimension check: post-merge
   cycle dim = `d_A + d_B + (k−1)` for `k` bridging arcs.

2. **One bridge joins the tree; surplus bridges are new chords; nothing is
   re-keyed** — and the *existing* incumbent-first BFS already enforces this with
   no new policy. `ApplicationNetwork.cpp:4426-4496` drains the incumbent queue
   (last epoch's live tree edges, seeded `:4216-4237`) to exhaustion before any
   new arc is explored; the first new arc to reach an unvisited cross-partition
   vertex becomes the tree edge, the rest hit visited vertices and become chords
   (tap-off init, GKF Eq. 22). So "which bridge" needs no new rule — it is
   whatever the deterministic BFS makes a tree edge.

3. **Component identity today is ephemeral and must be added — but only as a
   PER-EPOCH CANONICAL id, not a cross-epoch-stable one.** The union-find
   `find(v)` (`:4239-4258`) returns an epoch-local `R<idx>`/`S<sat>` string whose
   value depends on `unite()` order; `comp_rx`/`comp_sat` are rebuilt every epoch.
   `grp_arcs` (`std::map<size_t,…>`) and `grp_satrx` (`std::map<std::string,…>`)
   iterate in sorted/deterministic order (`:4143-4144`), so the grouping is
   already reproducible within an epoch.
   - The z-store is keyed by **chord identity** `(station, sat, σ)+cycle`, never
     by a component label (spec D1). So it needs **no** cross-epoch-stable
     component id: entries are re-grouped each epoch by re-derived union-find, and
     a label change is a harmless block rename.
   - **"min satellite id" is the right canonical choice** but the word "stable"
     over-claims: it flips when the min-id sat sets. That is fine — a satellite is
     in exactly one component per epoch, so min-sat is a **unique per-epoch
     canonical id**, which is all the merge order needs. Note `eq34_sat_root` is
     keyed `sat_signal → station NAME` (`:4281,:4390`); sat/name labels are
     stable, whereas receiver **pool indices are not** (the `Station` pool reuses
     slots, `NetworkNormalEquations.hpp:693-694`) — so anchoring the id on the sat
     label (not a receiver index) is correctly motivated.

4. **r1 re-election / re-rooting at merge forces no transform on z.** A
   fundamental cycle is fixed by the tree **edge set**, independent of the root;
   re-rooting changes only parent directions (`:4383-4396`), not the undirected
   edge set, and DDs are datum-invariant (spec lines 38-43). Any within-component
   tree-edge death that co-occurs is a **separate** D1 unimodular swap, composed
   with — not part of — the identity merge. No fractional-reference reconciliation
   is needed in the z-store regardless of which r1 each old component had;
   emission continuity is an emit-layer (D4) concern only.

### Recommendation to Fable (OPEN-2)

- **Deterministic merge order:** (1) canonical component id = **min satellite id**
  in the component (per `(const,σ)`); (2) process simultaneous merges by folding
  pairwise in ascending canonical bridge-key order (reuse the BFS tree-edge
  order), merged id = `min(id_A, id_B)`; (3) surplus bridging arcs become new
  chords in BFS discovery order; (4) carried `ẑ_A/Pᶻ_A`, `ẑ_B/Pᶻ_B` concatenate
  with **T = block-identity** (initial cross-covariance zero — the components were
  independent), new chords appended by tap-off.
- **Reword the spec:** "stable component identity" → "**per-epoch canonical**
  component identity"; and "re-derived chord set" overstates it —
  merged chords = `chords_A ∪ chords_B ∪ {surplus bridges}`, **no existing chord
  re-derived or re-keyed**. Anchor the id on the sat label, never a receiver pool
  index.

---

## Spec-correction punch list (for Fable to fold into the Card-4 handoff)

1. **OPEN-1 framing (`spec lines 210-212`)** is inaccurate as written:
   post-reduce ambiguity columns are **eliminated**, not "addressable across
   station pairs," and "verify splitStation exposes them post-reduce" is the wrong
   test. Correct statement: *the columns are gone, but the cross-station coupling
   is exactly foldable via the RETAINED per-station `N_sr` + `N_rr` LDLᵀ factors*
   (the `foldReducedSumZeroGauges` mechanism).
2. **D3 option (i)** should be respecified as a **joint rank-K per-component
   Woodbury fold**, not "sparse cross-block prior rows … on the retained
   ambiguity columns" implying independent rank-1 rows. Independent scalar folds
   are inexact whenever DD priors share tree edges (they always do) or `Pᶻ` is
   correlated (it is).
3. **`SumZeroGauge::terms` needs a signed-coefficient + multi-column-per-station
   extension** to express a 4-column DD (and >2-receiver fundamental cycles).
4. **OPEN-2**: "stable" → "per-epoch canonical" component id; "re-derived chord
   set" → "union + tap-off, existing chords unchanged"; id anchored on sat label.

## Residual questions this scout could NOT close (defer to Fable / phase B)

- **Where `Pᶻ` (the carried DD prior covariance) is assembled and folded** — the
  rank-K fold needs `(Pᶻ + B)⁻¹` per component per epoch; the z-store lifecycle
  (D1) and this fold are the same seam and should be specced together in phase B.
- **Cost of the per-component K×K solve at 50-sta scale** — bounded and
  per-component, but the actual chord counts per `(const,σ)` component on the
  golden lineage were not measured here (a phase-B instrumentation item; the D5
  counter "z-dim per component" is the right meter).
- **GLONASS (OPEN-3) and warm-resume (OPEN-4)** were out of this scout's scope.

---

## Method / provenance

Files read in full or in the cited ranges: `docs/HANDOFF_card4_invariant_carry.md`,
`docs/HANDOFF_eq34_datum_card3.md`, `include/filter/NetworkNormalEquations.hpp`,
`src/core/filter/NetworkNormalEquations.cpp` (fold + reconstruct + solve),
`include/filter/NetworkEstimator.hpp`+`.cpp`, `include/filter/NetworkSbasis.hpp`,
and the eq34 election + `splitStation` regions of
`src/core/app/ApplicationNetwork.cpp`. Adversarial verification: workflow
`card4-scout-verify` (3 high-effort agents — OPEN-1 refuter, OPEN-2 refuter,
completeness sweep); transcripts under
`…/subagents/workflows/wf_14f9201c-785/`. No blocker found by any agent; the
rank-K correction to OPEN-1 originated in the OPEN-1 refuter and is incorporated
above.
