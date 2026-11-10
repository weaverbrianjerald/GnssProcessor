# ALC-BAND: band-scoped level-check windows reconsidered

**STATUS**

- **id:** ALC-BAND
- **topic:** consumer
- **status:** closed
- **summary:** Band-scoped restart WITHDRAWN: WL and IF both depend on both selected arcs, so paired restarts stay; per-band dual-code levels would need their own calibrated design; runtime unchanged.
- **source:** [ALC section 7](ALC_amb_level_check_2026-09-15.md), user request, 2026-09-17
- **knobs / posture:** unchanged; existing paired windows, default OFF
- **hashes / run dirs:** `runs/_alc_band_20260917/`
- **dates in the text:** 2026-09-17

**Outcome.** The card asks whether the level check can restart only the band that changed and keep a one-band window (ALC section 7 item 2). Each ambiguity error enters both WL and IF, so neither history belongs to one band; a dual-code per-band level (E1, E2) does separate the arcs, at 2.56 and 3.27 m per sample against 1.49 m for IF. The claim is withdrawn (44 focused tests pass). [ALC2](ALC2_restart_policy_adoption_2026-09-19.md) section 6 leaves coverage to a new calibrated observable; the level check has been off in the working bases since 2026-09-20 ([PBH](PBH_phase_bias_hold_2026-09-19.md) section 8).

## 1. Registered scope

Distinguish scoped invalidation of the existing WL/IF statistics from a new
band-specific observable. Derive dependencies from the local innovation model,
check counterexamples and any viable alternative, and review restart/response
contracts. No runtime implementation, tuning or new positioning campaign.

## 2. Decision — 2026-09-17

**Withdraw the claim that "restart only the band that changed and keep a
one-band window" is the obvious next increment to the existing ALC. Keep
the current paired-window restart contract.** A band-specific level is possible,
but it is a different monitored quantity with different dependencies, precision
and calibration. Retaining a projected component of the old information is
different from retaining either existing WL/IF history unchanged.

The approximately 20% historical check/sample fraction motivates understanding
lost coverage; it does not establish that more checks would be correct or
beneficial. [PBT-AUDIT](PBT-AUDIT_translation_premise_2026-09-16.md) questions the
blanket translation policy causing many restarts, while
[ALC-STAT](ALC-STAT_statistics_audit_2026-09-17.md) shows that the monitor is not
yet calibrated. Recovering coverage before resolving those premises can simply
increase interventions with an uncertain cause.

## 3. Existing statistic dependencies

For the simplified local innovation model, let g be common modeled-error
residual, I the L1 first-order ionosphere error, e1/e2 the phase ambiguity
errors in metres, and q=(f1/f2)^2. Temporarily omit the surviving bias/noise
terms identified in ALC-STAT:

```
p1 = g - I   + e1       c1 = g + I
p2 = g - q*I + e2       c2 = g + q*I

WL = (f1*e1 - f2*e2)/c
IF = (q*e1 - e2)/(q-1)
```

Here c is speed of light, WL is in cycles, and IF is in metres. The signs of
the underlying phase/code ionosphere terms agree with
[ESA's observable model](https://gssc.esa.int/navipedia/index.php/GNSS_Basic_Observables);
the equations above and the alternatives below are derived for this project's
pre-fit innovations, not a new externally specified correction convention.

Both coefficients of both ambiguity errors are nonzero. A one-metre change in
either ambiguity changes **both** channels. Existing windows contain scalar
combinations of the two ambiguity arcs; there is no separate WL or IF history
belonging to just one band. A reset/birth/reseed of either selected ambiguity
invalidates the interpretation of retained WL and IF samples as belonging to
the current pair. Numerical knowledge of the reset's state delta does not
establish a valid historical frame transformation.

Splitting each combination into raw band contributions and independently
averaging different time spans also breaks the nuisance cancellation. A
controlled witness uses correct ambiguities at both times, with g/I changing
from 0/0 to 17/6 m. Each complete simultaneous four-row sample gives exactly
zero WL and IF. Mixing old L1 rows with new L2 rows gives nonzero values in
both. The same failure occurs with two independently averaged constant
stretches. Cancel nuisances within a simultaneous sample before considering
separate temporal histories.

With only a single band's phase and code, any linear combination `a*p+b*c`
that removes g requires `a+b=0`; removing I also requires `-a+b=0`.
Only a=b=0 satisfies both. `p-c` retains -2I; `p+c` retains 2g. An external
ionosphere or geometry estimate can supply information, but then its error,
covariance and dependence on the tested ambiguity become part of the test.

## 4. A viable alternative, with costs

Single-band *phase dependence* is achievable while retaining **both codes**.
Form each sample simultaneously:

```
E1 = p1 - (q+1)/(q-1)*c1 + 2/(q-1)*c2
E2 = p2 - 2*q/(q-1)*c1   + (q+1)/(q-1)*c2
```

In the simplified model E1=e1 and E2=e2. Changing p2 alone does not alter E1,
and vice versa. This provides a mathematically legitimate direction in which
one ambiguity's history might survive a reset confined to the other phase
arc. It does **not** imply that any event labeled with the other band is
irrelevant: both levels depend on both code corrections and signal identities.
A half-metre C2 error changes both levels by more than a metre in the witness.
The shared and signal-specific nuisance biases in ALC-STAT still survive.

For same-epoch inputs, this is an invertible change of the existing two-channel
basis:

```
WL = (f1*E1 - f2*E2)/c
IF = (q*E1 - E2)/(q-1)
```

Thus some information about the unchanged ambiguity can be projected from old
paired samples; a reset does not render every linear combination of the old
information useless. Conversely, the new basis supplies no extra observation
information when support is identical and full covariance is retained.

The tradeoff becomes substantial if each band is tested marginally. For GPS
L1/L2 and independent white measurement errors of 0.5 m code / 0.005 m phase:

| Quantity | Per-sample standard deviation | Standard error at n=20 |
|---|---:|---:|
| Existing WL | 0.41468 cycles | 0.09273 cycles |
| Existing IF | 1.48920 m | 0.33300 m |
| E1 | **2.56404 m** | **0.57334 m** |
| E2 | **3.26585 m** | **0.73027 m** |

E1/E2 noise correlation is **0.999551**, from the shared amplified code noise.
These figures are analytic noise propagation for the illustrative iid model,
not field uncertainty estimates or a detection-power calibration. Both E1/E2
are in metres; the current WL floor in cycles cannot be transferred to them.
Their near-common code error means that two large marginal levels do not
establish two bad ambiguity arcs. Marginal thresholds also fail to exploit the
precise differential direction retained by the full covariance.

Different E1/E2 time supports can be defensible under an explicit model that
both histories still estimate their respective current levels. They must not
be substituted silently into the old same-support statistic and uncertainty.
Each E sample has already canceled g/I, so this case is distinct from mixing
raw contributions in section 3. Its cross-window covariance includes the
shared epochs **and** temporal correlations across the two supports:
`Cov(mean E1, mean E2) = sum_{t in W1,u in W2} Cov(E1[t],E2[u])/(n1*n2)`.
The stationarity, current-state-reference and event-selection qualifications
from ALC-STAT still apply.

## 5. Distinguish three scopes

| Proposed change | Assessment |
|---|---|
| Keep current WL/IF history across a reset to either selected ambiguity | Reject for the current statistic; both quantities depend on both arcs. |
| Avoid restarting a selected pair when an unrelated signal changes | Potentially valid dependency bookkeeping, if the event proves no selected dependency changed. No coverage benefit established on the GPS-dual bed. |
| Keep per-ambiguity E1/E2 histories using both codes | Mathematically viable new observable basis; requires its own admission, covariance, null/power and response design. Defer implementation. |

The current engine already compares the **selected pair's** ambiguity
generations, so a change to an unrelated key does not affect that comparison.
However, `noteAmbLevelDisturbed_(sat)` clears by satellite, and SSR-break
callers supply only that scope. Narrowing those notifications would require
preserving the affected identities and shared-event scope at the call sites.
It would not justify retaining the WL/IF history when a selected key changes.

Separate WL and IF window lengths are **channel-scoped**, not band-scoped.
Both channels still depend on both arcs and both must respect their boundaries.
Separate lengths are also statistical tuning; they do not solve the coverage
issue when the same events invalidate both channels.

Finally, window retention and refloat scope are different decisions. Neither
an existing WL/IF trip label nor a noisy marginal E1/E2 level proves that only
one ambiguity should be reset. [RF-1 section 1.4](RF-1_float_ambiguities_design.md)
records a related producer
hazard: freeing one band while the sibling remains stiff can re-establish a
phase-precise conditional relation and inherit the sibling's level error.
That is a reason to verify consumer post-reset coupling and covariance, not
proof that the identical producer failure occurs in this consumer. No
single-band response change is justified by this window argument alone.

## 6. Recommended order and gates

Keep the present paired-window/generation safeguard and complete the
observation-only calibration work named in ALC-STAT. First count which events
discard which selected dependencies, with their reasons and sample times;
historical aggregate restart counts do not answer this.

If retained information remains worthwhile, write a separate design around
E1/E2 (or another explicitly modeled observable). Require:

- Identity of the tested phase arc, both code signals/correction frames, and
  the time support. Classify phase-only events separately from shared code,
  clock, frame and model changes; account for indirect KF updates to the
  supposedly unaffected ambiguity.
- A defensible transformation for known frame moves; actual slips, reseeds
  and uncertain correction changes cannot inherit that permission merely
  because their numeric deltas are available.
- Covariance and a falsifiable null for the retained direction, including
  shared code noise, unequal supports, bias alternatives and selection.
- Independent null/power experiments, then matched observe/act positioning
  evidence for the proposed response. More full windows alone is not success.

This review closes the design-premise question. It does not select or implement
a replacement monitor, promise restored coverage, or promote ALC/PBT.

## 7. Verification

Four tests in `tests/test_amb_level_band_scope.cpp` check existing channel
dependencies, asynchronous-support failure, dual-code per-band separation,
the full four-row basis identity and noise propagation. The alternative
formulas exist only in that test file. Existing engine tests already check
that a one-band reset discards the paired window, including after covariance
contraction, missing rows, changed signal pairs and value translations.

Reproduction from the repository root:

```powershell
scripts/vs2026_build.ps1 -Config x64-Release -Target gnss_tests
build/x64-Release/gnss_tests.exe --gtest_filter=AmbLevelBandScope.*:AmbLevelStatistics.*:AmbLevelCheckTest.*:AmbLevelArcTest.*:PbTranslationPremiseTest.*:PbStepTranslateTest.*
python scripts/docs_index_check.py
git diff --check
```

Results will be appended after the checks. No production source or configuration
is changed by ALC-BAND.

Final verification, 2026-09-17: VS 2026 x64-Release test target build PASS
(20 s); **44 tests from six suites PASS**, including the four new dependency
witnesses and the existing statistics, arc-boundary and translation tests.
Documentation index and `git diff --check` PASS. Logs are
`runs/_alc_band_20260917/{build,tests,docs_check}.log`. No new positioning run,
runtime option, threshold change or production window implementation.
