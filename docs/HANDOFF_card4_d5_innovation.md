# CARD 4 D5 — carried-DD innovation gate: the estimation-side fine-slip reset (Opus builds, Fable gates)

*Fable-specced 2026-07-12 (evening). Parent: spec D5 Def-1
(docs/HANDOFF_card4_invariant_carry.md), user slip architecture (memory
`feedback-slip-detectors-by-rate`): the MW/LLI pre-detection layer is
DELIBERATELY relaxed (big slips only); the estimation side must reset the
finer ones. Under `carry_invariant` that second layer is currently ABSENT
(the legacy within-epoch RAIM adaptation keys off phase-leveling rows that the
bypass empties). This card is the Card-4-native replacement — and the enabler
for the em10 field config (probe evidence: em10 tames the hr-4 event ~4× but
its float floor is ~6× worse from uncaught fine slips; memory
`project-m1-card4-phasec3c` probe matrix). Build on HEAD. Pre-flight CLAUDE.md
Standing invariants — this card IS invariant #1's fine-scale event response.*

## Mechanism (all quantities already cached by the fold — zero extra solves)

Per folded component, per epoch, AFTER the accepted solve, BEFORE
`installPosterior`:

- Innovation (K-vector): `v = z_free − Gᵀ x̂_s − ẑ` — already computed inside
  `ddPriorPosteriorMean` (`innov`). Expose it per block.
- Exact innovation covariance under H0 (no slip), derived from the folded
  system (Fable, verified against Cov(ẑ⁺−ẑ) = Pᶻ − Pᶻ⁺):

```
Cov(v) = (Pᶻ + B) − Gᵀ Σ_xs G  =  M⁻¹ − Gᵀ Σ_xs G
```

  (MINUS the border term — x̂_s is prior-influenced; this is NOT the naive
  Pᶻ+B+GᵀΣG. Sanity: Σ_xs=0 ⇒ Pᶻ+B; PSD by construction since it equals
  M⁻¹(Pᶻ−Pᶻ⁺)M⁻¹.) All pieces cached: M stored per FoldedDdPrior, B = M⁻¹−Pᶻ,
  G stored, Σ_xs = the datum-constrained shared covariance the estimator
  already computes. New kernel method mirroring the posterior readouts, e.g.
  `ddPriorInnovation(block, x_s, shared_cov, &v, &cov_v)`.
- Per-chord standardized test: flag chord i when `|v_i| / sqrt(Cov(v)_ii) > k`
  (config `k`, default 4.0). Optional secondary: component chi-square
  `vᵀ Cov(v)⁻¹ v` vs χ²_K logged as diagnostic only (no action in v1).

## Response (reset, NOT repair — user directive; repair is parked)

v1 (mandatory): a flagged chord is treated as slipped —
1. at THIS epoch's `installPosterior`, install the flagged chord FRESH
   (data-only tap-off, exactly the birth path) instead of the prior-mixed
   posterior — stops propagation of the stale value;
2. mark its arc into the slip set for the NEXT epoch's `buildDdPriors`
   (membership exclusion — the C3c path), so any dependent cycle re-taps.
The one already-solved epoch retains bounded contamination (the fold happened);
q_z is the backstop. Counter: `innov_flags`, `innov_retaps` in the Card-4
run-end line.

v2 (config-gated, OPTIONAL, default off): when ≥N chords flag in one epoch,
route the flagged set into the EXISTING within-epoch re-solve
(`resolve_dropping` → `buildCarryDdPriors` rebuild — the machinery is already
wired from C3c §3) so the epoch itself is cleaned. Do not build v2 if it
threatens the schedule; v1 is the card.

## Detection-power honesty (grade accordingly — do NOT retire as inert)
With today's σ_v ≈ 2 cyc (em25) the k=4 floor is ~8-10 cyc: the gate catches
the mid-scale class (below MW's big-slip floor, above the noise). Its power
grows as the DD σ floor drops (em10+D5 iterations, repr work). At em10 the
per-arc σ_v is larger but the slip POPULATION is far larger too — the em10
acceptance below is the honest meter.

## Config (D7 conventions)
```
network.carry_invariant_innov_gate    bool   false  master (requires carry_invariant)
network.carry_invariant_innov_k       double 4.0    per-chord flag threshold (standardized)
network.carry_invariant_innov_refold  bool   false  v2 within-epoch refold (requires innov_gate)
```
Load rule: innov_gate && !carry_invariant → reject. Warn if k < 3 (false-alarm
storm shreds the carry). OFF = byte-identical (gate never evaluated).

## Gates (Fable; production seams, negative controls fail numerically)
1. **MidSlipCaughtByInnovationGate**: warm a tight carry (the C3c test-net
   fixture), inject a mid-magnitude integer jump (choose it BELOW the fixture's
   coarse-slip signal path and ABOVE k·σ_v) WITHOUT signaling the slip; gate ON
   ⇒ exactly that chord flags + re-taps fresh (innov_flags==1), neighbours keep
   their variance; next epoch clean; the estimable DD re-converges.
2. **NEGATIVE control**: same jump, gate OFF ⇒ contamination visible on the
   same assertion (the C3c unsignaled-control pattern).
3. **FalseAlarmFloor**: no jump, N noisy epochs, k=4 ⇒ innov_flags ≈ 0 (bound
   it; a nonzero budget must be justified numerically from the fixture noise).
4. Config-parse regression; OFF byte-identity structural; suite green (681/1
   baseline).

## Field acceptance (Fable runs; ~10 min total, Release)
- **em25 3h reference re-run**: expect ≈ no change (1.50 class; few fine slips
  at 25°) — a large move here = false-alarm shredding, FAIL.
- **em10 3h (THE acceptance)**: floor currently ~8.3 cyc/ep (hr profile
  20.7/12.2/8.3/9.3). PASS = substantial, honest closure toward the em25
  class — target ≤ ~3 cyc/ep steady-state; report the achieved fraction
  either way (partial closure + correct counters is reportable progress, the
  residual then quantifies the non-slip low-el effects: tropo mapping,
  multipath, weighting).
- **em10 event window (03:30-04:10)**: the ~4× event-taming benefit must
  SURVIVE (post-ignition bucket ≤ ~40 cyc/ep class; no regression vs the
  probe).
- Counters sane: innov_flags rate ≫ at em10 vs em25; no refloat storm; ledger
  balanced.

## Out of scope
Slip REPAIR (parked, user decision); per-LOS repr-error states (holstered
pending em10+D5 outcome); clock-q re-test (needs clock_model armed — separate
config gotcha); GLONASS; high-rate TurboEdit (rate-licensed elsewhere).
