# HANDOFF — PB emit-filter gate v2: censoring-free, model-scaled, rate-transferable

*Written 2026-07-14 (Fable). Implementer: Opus. Leaf-only card — does NOT touch
`ApplicationNetwork.cpp` (safe to run concurrently with the parallel-sweeps card;
one small merge point in `GnssConfigLoad.cpp`).*

## 1. Context (measured, Jul-14 dev300s campaign)

The PB emission-continuity filter's v1 reset gate is `|innov| > jump_k · σ̂_step`
with σ̂_step estimated from **smoothed (accepted) rows only** — a censored
estimator. Measured failure at 300 s cadence: when the true per-epoch step
(~5–10 cm) sits at gate scale, rejected rows are withheld from the estimator,
σ̂_step biases low, the gate tightens, more rows reject — a death spiral. Run
evidence (305-sta GPS+Gal smoke): 2059 resets of 4176 key-epochs, **92%
innovation**, wire PB disc floor 43% of keys/epoch.

The interim fix in the tier configs (`jump_k` 4→8, `meas_sigma_m` 0.05→0.10)
cleared 300 s (2013 smoothed / 98 ref-path / **0 innovation**; disc floor
180→~10/ep, ref-path only) but is **hand-tuned per rate** — at 30 s the implied
20 cm minimum gate is on the wrong side of a single-cycle L1 step (~19 cm).

**User mandate:** data/model-driven gating; ONE config must transfer 30 s ↔ 300 s
with k unchanged. Exemplar already in-tree: D5 v2 (`carry_invariant_innov_calibrate`)
crossed the rate swap with k=4 untouched because its threshold is a true Nσ
against an LS-VCE-calibrated σ̂₀. Same user directive lineage as MW
(`feedback_mw_thresholds_empirical`: MAD/Welford only, no hand thresholds).

## 2. Spec — three changes in `NetworkPbEmitFilter` (+ toggle)

All under ONE new toggle (v1 path byte-identical when OFF):

```cpp
bool network_emit_pb_filter_gate_v2_{false};  // GnssConfig pattern: member + getter + jval + summary log
```

`src/core/products/NetworkPbEmitFilter.cpp` anchors are line-precise today:

**(a) Kill the censoring** (anchor `The contaminated innovation is NOT fed`,
currently ~:90). v2 estimator input = every row **with a valid prior** whose
reset cause is NOT structural — i.e. include kSmooth rows AND kInnovation rows
(the censoring fix), exclude kInit / kGap (no valid prior) and kRefPathSlip
(exogenously flagged genuine event). The χ²₁ geo-mean is tail-safe at the
measured contamination (<1% genuine events at 30 s; ~0% at 300 s — every
innovation reset there was a false alarm), so feeding the rejected rows cannot
inflate the floor the way the v1 comment feared; that comment must be rewritten,
not silently contradicted.

**(b) Standardize by the model's predicted innovation σ** (anchor
`else if (std::abs(innov) > cfg_.jump_k * sstep)`, ~:76): gate on
`jump_k · sqrt(Ppred + R)` — `S = Ppred + R` already exists two lines below for
the Kalman gain; hoist it above the gate. This is where dt enters through the
model (`Ppred = var + q·gap`, per-key gap already internal via `last_seen_s` —
**no caller/signature change**), mirroring q_z·dt inside D5's formal Cov(v).

**(c) `meas_sigma_m` demoted to a seed**: with all-rows feeding, structural
collapse is impossible; keep the `kPriorRows` weak prior for the first epochs
and replace the 0.25×seed floor with a small absolute numerical guard (~1 mm).
`jump_k` returns to a universal default 4.0–5.0 = an honest false-alarm rate
(α = erfc(k/√2); at k=4, ~420 keys → ~0.03 false alarms/epoch, either cadence).

**(v2b — second increment, only after (a)+(b) gate):** two-component step-noise
model `R(dt) = σ̂²_solve + q̂_drift·dt`, both per-constellation, robust-fit over
the window's (innov², gap) pairs (the M2 `NetworkCarryVce` rate+floor pattern,
including its condition guard). At a FIXED cadence the pair is unidentifiable
(only the sum is observable) — REQUIRED graceful degradation to the v2a lumped
σ̂ when the gap spread fails the condition test. Note the physics motivating it:
measured σ_solve is ≈dt-independent (~6–8 cm median step at BOTH 30 s and
300 s), so v2a alone already transfers; v2b is refinement, triage it
field-inert if the condition guard rarely passes.

## 3. Residual-risk statement (write it into the header, honestly)

With an honest σ̂ ≈ 6–8 cm and k=4, the gate sits ≈ 25–30 cm at both rates. An
UNFLAGGED reference-path slip between ~2σ and k·σ̂ (e.g. a missed single-cycle
L1 step, 19 cm) smooths through and the filter converges to the new level over
~1/K epochs — a bounded, sub-gate transient the consumer never sees as a disc.
This risk class exists in v1-as-designed too (any k·σ̂ gate has it); upstream
coverage (C3c funnel + D5) makes the miss class rare. Do not pretend v2 removes
it; document the envelope.

## 4. Invariant pre-flight

1. Zero-q/stiff → event response: filter q unchanged; ref-path reset path
   untouched (hard reset, raw pass-through). The v2 gate must still fire on
   genuine unflagged events — unit-proven at both rates (G1b).
2. Release-only for any timed claim (not perf-relevant here).
3. Deferrals: v2b unidentifiability → explicit fallback, triaged field-inert.
4. Counters: extend the run-end filter line with v2 fields (σ̂ per
   constellation, n_fed_all_rows, and v2b's σ̂_solve/q̂_drift when active).
5. Grading: outputs-only leaf; dd-frac and the solve byte-unchanged by
   construction.
6. Default-OFF byte-identity: single toggle; OFF = v1 bit-for-bit.

## 5. Gates

- **G1 unit** (`tests/test_pb_emit_filter.cpp`, synthetic streams):
  (a) spiral regression — noise at gate scale: v1 spirals (assert the current
  behavior as documented baseline), v2 σ̂ converges to truth;
  (b) genuine-event detection with the SAME config (k=4) at dt=30 and dt=300 —
  both fire; sub-gate miss transient bounded as §3 predicts;
  (c) v2b identifiability: mixed-gap stream fits (σ_solve, q_drift) within
  tolerance; fixed-gap stream triggers the fallback;
  (d) ref-path pass-through and Counters unchanged in shape.
- **G2** build + full suite green; OFF byte-identity replay on the Run-B smoke
  artifacts' config (same build).
- **G3 300 s field A/B** (`_net305_3card_dev300s_gps_d8_em10_smoke.json` with
  v2 ON, k=4, seed left at default): expect ≥ parity with the hand-tuned arm —
  0 innovation-storm, disc floor ≈ ref-path only, σ̂ converged near the
  measured 5–10 cm.
- **G4 30 s field A/B** (`_net305_3card_gpsonly_30m.json`, v2 ON, SAME values):
  disc floor clears (emitter silent on smooth epochs) AND no laundering — every
  disc event the v1 run produced from a ≥0.5 m step still discs under v2.
- **G5 the transfer test (the card's reason to exist):** one config block, both
  cadences, k unchanged, G3+G4 both pass. Then retire the k=8/0.10 hand values
  from the tier configs in the same commit.

## 6. Pointers

`include/products/NetworkPbEmitFilter.hpp` (header doctrine — rewrite the
clean-gate paragraph), `src/core/products/NetworkPbEmitFilter.cpp` (:76 gate,
:90 feed policy), `tests/test_pb_emit_filter.cpp`, `GnssConfigLoad.cpp`
`loadNetworkSection_` (merge point with the parallel card — coordinate),
memory `project_pb_emission_continuity_card` + `project_dev300s_tier` (all
measured numbers above), `include/filter/NetworkCarryVce.hpp` (robust-fit
pattern for v2b).

## 7. Implementation status (v2a landed — Opus, 2026-07-14)

**v2a (changes a+b+c) implemented, default-OFF, G1+G2 PASS.**  Toggle
`network.emit_pb_filter_gate_v2` (`Config::gate_v2`).  v2b (two-component
`R(dt)=σ̂²_solve+q̂_drift·dt`) is DEFERRED to a second increment per §2 — v2a's
lumped σ̂ transfers because the measured step σ is ≈dt-independent.

Touched:
- **Leaf** — `NetworkPbEmitFilter.{hpp,cpp}`: (a) censoring-free feed (kInnovation
  rows feed `σ̂_step` too; kInit/kGap/kRefPathSlip stay out); (b) model-scaled
  gate `k·√(Ppred+R)` (S hoisted above the gate); (c) v2 floor = 1 mm absolute
  guard (`kSigmaFloorAbsM`), seed kept only as the weak prior.  Header clean-gate
  paragraph rewritten (not contradicted) + residual-risk envelope written in.
  `Counters::fed_rows` (n_fed_all_rows) + `stepSigmas()` telemetry seam added.
- **Config** — `GnssConfig.hpp` member+getter; `GnssConfigLoad.cpp` jval + v2
  summary-log line (the coordinated merge point).
- **ApplicationNetwork.cpp** — three isolated one-liners, each tagged
  `// PB-gate-v2 merge point`: Config-population (required for the toggle to
  reach the leaf), config-echo field, and the run-end v2 telemetry line
  (fed_rows + converged σ̂ per constellation).  All in setup/echo/run-end blocks,
  zero overlap with the per-LOS sweep region the parallel card edits — so the
  merge is a 3-line reconcile, not a conflict.  ("Leaf-only" can't literally
  exclude the Config-population line without leaving the toggle dead.)
- **Tests** — 3 new gates (spiral regression both arms + n_fed accounting;
  one-k transfer 30 s↔300 s; sub-gate bounded-transient §3).  9/9 PbEmitFilter,
  723-case suite green (1 pre-existing skip).

**Gates remaining (user-triggered field runs; Release only):**
- **G3 (300 s A/B)** turnkey: arm A = `_net305_3card_dev300s_gps_d8_em10_smoke.json`
  (v1, k=8/0.10, unchanged); arm B = `_net305_3card_dev300s_gps_d8_em10_smoke_pbv2.json`
  (NEW — gate_v2:true, k=4, seed 0.05).
- **G4 (30 s A/B)**: arm A = `_net305_3card_gpsonly_30m.json` (v1, k=4 defaults);
  arm B = same + `"emit_pb_filter_gate_v2": true` (one-key flip; k/seed already
  default).
- **G5 (the transfer)**: G3+G4 both pass with the SAME v2 block, k unchanged.
  **ONLY THEN**, in the passing commit, retire the per-rate hand values from the
  tier configs (`_net305_3card_dev300s_gps_d8_em10_*`: drop `emit_pb_filter_jump_k:8`
  / `emit_pb_filter_meas_sigma_m:0.1`, add `emit_pb_filter_gate_v2:true`).  NOT
  done here — the tier configs are byte-identical v1 until G5 clears.
