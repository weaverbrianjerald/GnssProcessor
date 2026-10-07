# PBT-AUDIT: when a phase-bias update warrants an ambiguity translation

**STATUS**

- **id:** PBT-AUDIT
- **topic:** consumer
- **status:** closed
- **summary:** Blanket translation premise FALSIFIED on seven production-ledger counterexamples and controls; deterministic datum translations remain valid; a frame-transfer contract is recommended, runtime unchanged.
- **source:** [PBT](PBT_consumer_pb_translate_2026-09-15.md), September 16 review
- **knobs / posture:** existing PBT behavior; no runtime or default change
- **dates in the text:** 2026-09-16
- **hashes / run dirs:** `runs/_pbt_audit_20260916/`

**Outcome.** The card tests PBT's claim that every applied phase-bias change must be absorbed by the ambiguity, on the production `PbStepTranslateLedger::react`. Translation is right for a known datum change and wrong for a physical bias change (ambiguity error −0.30 m) or a clock and bias redistribution, so the blanket rule is FALSIFIED; 24/24 tests pass. [PBH](PBH_phase_bias_hold_2026-09-19.md) later found 0.2 % of the translated mass to be a known frame move and re-aimed the contract to an emit-side phase-bias hold, which replaced the translation in the working bases on 2026-09-20.

## Pre-registered scope

Use the production `PbStepTranslateLedger::react` with controlled state values and
an independently specified observation equation. Separate a deterministic datum
change from a physical bias change and an improved bias estimate. Include a
clock/bias redistribution and the 5 mm dead band. These are premise tests, not a
field campaign or a proposal to switch off the measured PBT improvements.

The tests must distinguish prediction continuity from agreement with observations.
No threshold tuning, replacement reaction policy or golden promotion is included.

## 1. Verdict

The unconditional claim in PBT §1.1 and `PbStepTranslateLedger.hpp` that every
applied phase-bias change must be absorbed by the ambiguity is false. The
translation is valid for a known change in the ambiguity's coordinate frame.
An ordinary correction update is not, by itself, such a change.

This does not invalidate the historical field improvements. It invalidates the
argument for generalizing the mechanism to every update and every provider.
PBT remains an experimental continuity policy with measured benefits on its
specific fixture. The existing default-OFF behavior is unchanged by this audit.

## 2. Observation consistency, rather than prediction continuity

Use this codebase's sign convention and metres:

```
phase observation = m + b_physical + A_physical + noise
phase prediction  = m_hat + b_product + A_state
```

Here `m` contains geometry, clocks, atmosphere and the other modeled terms.
`MeasurementModel.cpp` adds both the applied phase bias and ambiguity to the
prediction. For a fixed observation and fixed `m_hat`, PBT gives

```
db = b_product(k) - b_product(k-1)
A_preupdate(k) = A_previous - db
b_product(k) + A_preupdate(k) = b_product(k-1) + A_previous
r_translate = r_hold + db
```

That is an identity preserving the previous prediction. It does not establish
that the current observation should have the previous prediction.

Separate a known frame offset `G` from the physical correction. If
`b_product = b_physical + G` is exact, the matching ambiguity is
`A_state = A_physical - G`. With no consumer cycle slip, its required change is
`-dG`, not `-db_product`. When `db_physical` is nonzero, PBT also removes that
physical change from the predicted phase, leaving the filter to estimate it
again through its state update.

The IGS SSR specification explicitly distinguishes a constant carrier ambiguity
from a phase bias that can vary with temperature, and requires consistency
among clocks, biases and other corrections. It also permits time-varying
signal-common datum offsets. These are different effects in the same products.
See [IGS SSR v1.00, §4.3, printed pp. 12–15](https://files.igs.org/pub/data/format/igs_ssr_v1.pdf).
The tests use the repository's prediction-side sign; the specification defines
the transmitted correction on the observation side. This audit is not a decoder
sign audit.

## 3. Controlled witnesses using the production ledger

`tests/test_pb_translation_premise.cpp` calls the actual
`PbStepTranslateLedger::react`; observations and expected residuals are specified
independently. Initial ambiguity is 12 m and the illustrative step is 0.30 m.

| case | correct response / control | current PBT result |
| --- | --- | --- |
| Known deterministic PB-only datum change; observation unchanged | translate ambiguity by −0.30 m | correct: residual 0, covariance unchanged |
| Physical PB increases 0.30 m; observation and accurate product increase together | retain the true ambiguity; residual 0 | ambiguity error −0.30 m and phase residual +0.30 m, with or without declaration |
| Improved product removes a 0.30 m error; ambiguity prior independently accurate | retain ambiguity; new residual 0 | preserves the old +0.30 m residual |
| Same improved product, but old product error was entirely absorbed into the float ambiguity | translate that absorbed error out | correct: recovers ambiguity 12 m, residual 0 |
| Clock, code bias and PB all increase equally; combined corrections unchanged | retain ambiguity; residual 0 | PB-only translation creates +0.30 m phase residual |
| Same 0.04 m datum trajectory represented as ten 0.004 m updates versus one 0.04 m update, without intervening measurement updates | an exact frame transport would have the same endpoint | small steps all skipped: residual −0.04 m; one step translated: residual 0 |

A seventh test takes the physical-change residual into an explicitly reduced
two-state Kalman calculation: one geometry/range coordinate and one ambiguity,
code row `[1,0]`, phase row `[1,1]`, prior variances 1 and 0.0001 m²,
measurement variances 0.25 and 0.000025 m². The spurious phase residual moves the
geometry coordinate by **0.2998126171 m**. This is a counterexample demonstrating
possible leakage with a stiff ambiguity, not a predicted field ENU error or a
full-engine performance result. A flexible ambiguity could absorb more instead.

The two improved-estimate cases are intentional: an update is neither always
harmful nor always beneficial to translate. Its value and declaration bit alone
do not describe how much of the previous error the consumer already absorbed.

The dead-band case is secondary. `record()` advances the baseline even when
`react()` skips a small step, so skipped changes never accumulate toward a later
translation. Actual measurement updates can absorb them, but the 5 mm setting
is an approximation, not a bound on total accumulated error or an exact
coordinate transformation.

## 4. What the current producer and consumer actually know

- `NetworkEstimator.cpp:1052` exports the estimated shared phase-bias column
  directly. `NetworkPhaseBias` documents its reference-receiver ambiguity lump;
  this is an estimable bias in a chosen datum, not a pure frame-offset channel.
- `NetworkProductEmitter.cpp:514-561` increments the phase discontinuity counter
  on forced events **or** a value-jump test. A counter bump therefore does not
  certify a known deterministic frame transformation.
- `SsrPhaseBiasEntry` carries bias, integer flags, a counter and optional formal
  uncertainties. It carries no separate signed datum delta or event provenance
  that distinguishes physical bias evolution from a frame move.
- `RealTimeEngineSsrBreaks.cpp:337` passes only the applied values and the combined
  declaration/freshness flag to PBT. `PbStepTranslateLedger::react` has no clock,
  code-bias, ionosphere or producer-basis information. It cannot distinguish the
  controlled cases above. Merely restricting PBT to declared steps is insufficient.

The clock/bias redistribution witness is specifically a consistency test:
the phase correction is `-clock + PB`, and the code correction is `-clock + CB`.
Equal changes leave both unchanged. Translating PB alone is not invariant to
that redistribution. Translating *every combined-correction change* would still
cancel genuine physical corrections, so that is not a complete replacement either.

## 5. Covariance scope

Unchanged covariance is exact for a known additive constant. If the transported
frame offset is uncertain, the scalar relation is

```
Var(A - G) = Var(A) + Var(G) - 2 Cov(A,G).
```

A general linear basis transformation uses `P_new = T P_old T^T`. Whether datum
uncertainty is already represented in the consumer state, external products or
their correlations has to be specified; blindly adding a product variance can
double count it. Current PBT does not establish this stochastic contract.

## 6. Recommended next implementation

1. Define a producer-to-consumer **frame-transfer contract**: old/new frame identity,
   affected signals, the signed transform (including coupled clock/code changes),
   and whether its offset is known or estimated. Compute the frame component from
   the actual basis transition, rather than guessing it from a median or jump size.
2. Translate only that declared, known frame component. Let physical product
   evolution enter the measurement prediction with an explicit uncertainty model.
   Preserve a conservative refloat/recovery path when continuity cannot be proved.
3. Add matched injected-event tests: datum-only change, physical bias step/ramp,
   combined clock/CB/PB redistribution and product-estimate refinement. Preserve
   the current PBT arm as an empirical comparator and grade the same cold consumer
   and wire instruments on both beds before choosing or promoting a replacement.

ALC repairs can mitigate states that PBT preserves incorrectly; they cannot prove
that blanket translation is the right transition model. The field data reviewed
here do not identify how much of the historical PBT win or remaining error comes
from each source. No new field accuracy attribution is claimed.

## 7. Validation and scope of changes

- VS 2026 Release test-target build: PASS, 23 s.
- **24/24 tests PASS:** seven new premise tests, nine existing PBT tests, eight
  ALC arc-boundary regressions. Passing the new tests confirms the counterexamples
  above, not the correctness of blanket translation.
- Only the audit record, historical cross-reference, documentation index, test
  source and CMake test registration changed in this audit. Production code,
  thresholds, configurations and defaults did not change.
- No estimation campaign, new product format, runtime fix or promotion was run.
- Build/test logs: `runs/_pbt_audit_20260916/`.
