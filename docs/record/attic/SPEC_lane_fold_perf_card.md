# SPEC — Lane-fold efficiency (M5 perf card "LF-1")

**Status:** QUEUED (spec'd Jul-16 2026 night, Fable, on user command; nothing
built).  Motivating numbers measured the same night — see §1.  Blocks routine
305-station runs of the MW-WL/UWL lane stack; does NOT block any current
science (≤150-station legs run at ~50–60 s/ep).

**One sentence:** the WL-3 lane accumulator applies ~10³ sequential O(K²)
rank-1 covariance updates per epoch against dense per-component pair blocks
whose dimension K never shrinks — batch them (BLAS-3), stop folding
information-free rows on pinned cycles, retire dead chords, and fan the
independent components across cores, without touching a single decision path.

**Relation:** perf-only successor to the WL-3 lane-coordinate carry
(SPEC_lane_carry_card.md) and the UWL-1 tier (SPEC_uwl_card.md).  The
STRUCTURAL escape — a factored @f$P^z@f$ — was investigated and **CLOSED as
impossible for this carry** (memory `carry-driver-perf-c2-impossibility`); do
not re-propose it.  `network.parallel_carry` (Eigen-threaded GEMMs,
2.13–2.45×) is already in every number below.

---

## 1. Motivation (measured, Jul-16 2026 — do not relitigate)

1. **The lane accumulator is the wall, not the network.**  The 3card lineage
   (no lanes) runs 305 stations at ~28 s/ep with pcarry.  With the full lane
   stack: 150 sta ≈ 46–61 s/ep dd-build; **305 sta ≈ 384–617 s/ep** (Σnet
   ~600 s/ep — 12× for 2× stations; `_wlver_305sta_10ep_d10_sh8_full`).
   A 60-epoch 305-station leg is ~10 h wall; the user declined it.
2. **The cost is row-count × K².**  The 60-ep 150-sta leg folded 48,786
   MW(1,2) lane increments + 23,503 bridge increments ≈ **1,200 rows/ep**,
   each a sequential BLAS-2 rank-1 fold against the component's dense 2K×2K
   lane-shape covariance.  K doubles with stations ⇒ per-row cost ×4, row
   count ×2 ⇒ the observed ~12×.
3. **Most steady-state rows are information-free.**  By epoch 30+, ~65% of
   traced cycles are DECIDED and idempotently re-pinned at σ = 0.01 cyc; an
   MW increment at σ ≈ 0.06–0.2 cyc folded into a lane already pinned 6–20×
   tighter moves nothing.  The increments' job — converging UNdecided lanes —
   is over for that population.
4. **K never shrinks.**  Jul-14 profiling recorded K ≈ 7,834 carried DD
   chords with **0 ever marginalized** (suspected killChord lifecycle gap,
   flagged as INC-C0/C1 in docs/attic/HANDOFF_carry_driver_perf.md; INC-C1 has
   SINCE been closed as case (a) — see docs/SPEC_carry_factored_pz_C2.md §6).
   Set satellites' chords ride the dense matrix forever.
5. **Components are independent by construction.**  The pair-block system is
   block-diagonal over carry components (44 components at 150 sta under
   family pairs); nothing couples them inside the lane fold.

## 2. Non-goals

- No change to any DECISION path: statistics stay detector-side, FFR gates,
  closures, dedupe, cold serves untouched.
- No change to what information enters — only *when it is skipped as
  redundant* (INC-2, gated) and *how the linear algebra is applied*.
- No factored covariance (C2 closed).  No approximate/sparse Pᶻ.
- Not the D1 cov-consumers card (dd-frac/T1a sweeps — separate, ~14.5%).

## 3. Mechanism (four levers, independent knobs)

### 3.1 INC-0 — instrumentation split (log-only, first)

Extend the D0b sub-timers to split SB_DDBUILD into: base carry
build/fold/install vs `applyLaneMeasurement` vs `applyLaneFix` vs the WL/EWL
layer `build()`.  Per-epoch: rows folded per component, K per component,
rows skipped.  Every later increment is graded against this baseline.
(Invariant #4: the shares below are estimates until INC-0 measures them.)

### 3.2 INC-1 — blocked row application (`network.lane_carry.fold_blocked`)

Accumulate the epoch's lane rows PER COMPONENT and apply as one rank-m block
update (Cholesky-downdate-style or Woodbury with an m×m core) instead of m
sequential rank-1s.  Same flops to first order, but BLAS-3 vs BLAS-2 —
expected 3–8× on the row-fold share, more with pcarry's threaded GEMMs.
**Numerical contract:** a different operation ORDER ⇒ NOT bit-identical to
the sequential fold — this is the `parallel_carry` equivalence class, not the
`parallel_seam` byte-identical class.  Default OFF; ON requires the
physically-null gate (§5.3) before any graded use.

### 3.3 INC-2 — pinned-cycle increment skip (`network.lane_carry.skip_pinned_increments`)

Skip MW/bridge lane increments for a chord when (a) its last `applyLaneFix`
returned fold-or-unchanged within the last N epochs (N default 3), AND (b) no
refloat/slip/re-tap has touched the chord since.  Any funnel event clears the
condition (invariant #1: the slip response path must be provably unaffected —
a refloated chord resumes increments the same epoch).  Decision quality is
untouched by construction (decisions never read the folded increments); the
gate is lane-trace parity (§5.4).  Expected steady-state row cut 2.5–4×.

### 3.4 INC-3 — chord lifecycle (kill/marginalize policy)

Resolve the 0-marginalized finding: distinguish "chord dead" (satellite set,
station gone — MARGINALIZE properly, preserving the Schur complement onto
survivors) from "chord dormant".  Cap K's monotone growth.  This is the
correctness-sensitive increment: marginalization must not touch the datum
(S-basis untouched; the carry's invariant functions remain invariant) and
must round-trip the RTB-7 snapshot.  Requires its own unit battery.

### 3.5 INC-4 — component-parallel fold (`network.lane_carry.parallel_components`)

OMP over components with per-component sequential order preserved and a fixed
merge order — byte-identical is CLAIMABLE here (no cross-component coupling),
unlike INC-1.  Nesting hazard (measured, dev300s card): do NOT un-pin Eigen
inside the OMP region — components use pinned Eigen; the existing pcarry
un-pin stays OUTSIDE.  Amdahl note: at 150 sta one component may dominate K —
INC-0 measures the distribution before this is built.

## 4. Standing-invariants pre-flight

(1) No new stiff quantities, no event-response changes; INC-2's skip clears
on any funnel event — force-fire re-run required at its gate.  (2) Release
only.  (3) Deferrals triaged in §6.  (4) INC-0 counters make every skip/batch
visible; a lever that shows 0 in its counter is inert and its gate FAILS.
(5) Grading on lane-trace/dd-frac datum-invariant quantities.  (6) Every
knob default-OFF byte-identical; INC-1 explicitly pcarry-class (equivalence,
not identity) and its ON runs carry the same usage rule pcarry had until its
equivalence gate passed.

## 5. Validation

1. **Byte-id (all knobs OFF)** vs the pre-card exe on `_wlver_50sta_15ep_off`
   + `_wlver_50sta_10ep_uwl` (recorded hashes 90DB1548/DB1A081C,
   3A395593/B5A04846).
2. **Suite** green + new unit gates (blocked-fold algebra vs sequential on
   synthetic blocks to 1e-12; skip-condition lifecycle incl. refloat-clears;
   marginalization Schur correctness + snapshot round-trip).
3. **Physically-null gate (INC-1, INC-4):** run-end counters byte-identical
   (decided/fixes/injects/D5/deweight), dd-frac diagnostic drift ≤ a few 1e-3
   cyc, lane-trace constrained circR/med|f| parity — the pcarry precedent.
4. **Quality parity (INC-2):** 30-ep 150-sta A/B — constrained circR/med|f|,
   decided/ep, D5 within family of skip-OFF; force-fire shows unchanged
   post-slip re-fix latency.
5. **The number that closes the card:** 305-sta 10-ep smoke Σnet
   **≤ 90 s/ep** (from ~600) with all levers armed; 150-sta ≤ ~20 s/ep.

## 6. Increments & triage

0. Instrumentation split (log-only; REQUIRED first — the §3 shares are
   estimates).
1. Blocked application — biggest expected win, pure numerics.
2. Pinned-cycle skip — biggest expected row cut; needs the force-fire gate.
3. Chord lifecycle — correctness-sensitive; can land independently.
4. Component-parallel — after INC-0 measures the K distribution.

Deferrals: none field-blocking (the card itself is a perf unblock; every
lever is default-OFF).  305-station GRADED lane runs remain gated on this
card; ≤150-station science is unaffected throughout.

## 7. Open questions for the implementer

1. Does `applyLaneFix` rc==0 (unchanged re-pin) already early-out before the
   covariance touch, or does it re-fold the same row?  If it re-folds, the
   idempotent SET is a hidden per-epoch rank-1 per decided cycle — INC-0
   will show it; an early-out may be a one-line win ahead of INC-2.
2. INC-1 core: rank-m Woodbury with an m×m LDLT vs m sequential downdates —
   which preserves SPD better on the stiff lane-shape blocks (q_wl 0.01
   scale disparity)?  Prototype both on the 60-ep captured blocks.
3. INC-3: does the RTB-7 fingerprint need a marginalization-count term so a
   warm resume across a kill-policy flip is rejected (the carrycpljoint
   lesson)?
4. K distribution at 305 under family pairs: one giant G component (~2,700
   arcs) dominates — is component-splitting (forest partitioning) ever
   admissible without breaking cycle closure, or is INC-4's ceiling just
   Amdahl on that component?
