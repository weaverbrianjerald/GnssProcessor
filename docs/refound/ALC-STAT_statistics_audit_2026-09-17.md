# ALC-STAT: statistical interpretation before tuning

**STATUS**

- **id:** ALC-STAT
- **topic:** consumer
- **status:** closed
- **summary:** Statistical interpretation audited; four new tests and 40 focused tests pass. ALC is not a calibrated ambiguity-correctness test; no tuning or runtime change.
- **source:** [ALC](ALC_amb_level_check_2026-09-15.md), user request, 2026-09-17
- **knobs / posture:** unchanged; window_ep=20, k=4, existing floors; default OFF
- **hashes / run dirs:** `runs/_alc_stat_20260917/`
- **dates in the text:** 2026-09-17

## 1. Registered scope — 2026-09-17

Exercise the production `AmbLevelCheck` with independent Gaussian measurement
noise, stationary correlated code noise, and known nuisance offsets. Separate
independent-window rejection probabilities from streaming trip-event rates.
Read saved PBT traces descriptively; they do not label true ambiguity errors or
record every monitor admission/reset. No runtime, threshold, window or posture
changes, and no new positioning campaign. This is an interpretation audit,
not a field false-alarm calibration or promotion.

## 2. Interpretation — 2026-09-17

**The arithmetic is a useful empirical inconsistency monitor. Its present
denominator and event policy do not establish a calibrated "4 sigma ambiguity
error" test. Do not tune k or length from the historical trip ratios.**

`AmbLevelCheck::moments` computes sample standard deviation about the window's
own mean, divides by sqrt(n), and floors the result. This estimates uncertainty
of a constant mean under independent observations; it does not include a
persistent offset's magnitude or identify its cause. For independent Gaussian
zero-mean observations with an inactive floor, the ratio follows Student t with
n-1 degrees of freedom. With n=20, the single-channel two-sided probability at
4 is **0.0007661923**, versus **0.0000633425** for a standard normal: **12.10x**
larger. The floored test is no more permissive than the unfloored t test under
this ideal null, but is no longer exactly t-distributed when the floor binds.
This is the standard distinction between estimated and known population
variance in [NIST's mean-test reference](https://www.itl.nist.gov/div898/handbook/prc/section2/prc22.htm).

The actual residual null is `E[WL]=E[IF]=0` within the admitted window. That is
stronger than "both ambiguities are correct": it also requires all surviving
nuisance terms to have zero mean. The deterministic production-detector tests
show:

- Correct ambiguities with a +0.5 m code-only offset on both bands give exactly
  the same WL/IF statistics as -0.5 m phase ambiguity errors on both bands.
  Both trip. The monitor cannot distinguish these explanations.
- A common +0.5 m term on all four rows cancels. A +0.5 m phase-only receiver
  clock term survives as +0.5/lambda_WL cycles and +0.5 m and trips. The prior
  header's claim that a separate phase clock cancels was false. RF-0hv has
  `estimate_phase_clock=false`, so this is a general API qualification, not a
  finding that this state caused the saved field results.

The combinations cancel the shared geometry/clock/troposphere and modeled
first-order ionosphere terms. Unmodeled code/phase biases, multipath, correction
errors and other non-canceling effects remain. A trip does not isolate a band
or prove that refloating both ambiguities is the right repair.

The engine harvests pre-fit innovations from each epoch's state, after which
the KF can change that state. Thus a window averages errors against a sequence
of prior estimates; it is not twenty independent measurements of the current
post-update ambiguity. A correct Kalman model can produce white innovations;
filtering alone is not evidence of correlation. Whiteness and covariance need
checking for this model, its products, admission and restart policy.

## 3. Controlled production-detector experiments

`tests/test_amb_level_statistics.cpp` calls the real combination helpers and
`push`, with the unchanged defaults n=20, k=4 and floors 0.02 cycles / 0.05 m.
Each independent experiment has **250,000 windows**, a stationary start per
window, seed 20260917, independent bands, phase white noise sigma=0.005 m,
and code stationary AR(1) noise with marginal sigma=0.5 m. All physical and
estimated ambiguities are correct and all noise has zero ensemble mean.
AR(1) phi changes persistence, not marginal code variance. These are synthetic
stress tests, not fitted models of the field data. Fixed seeds aid reproduction;
normal-distribution implementations can differ across standard libraries.

| Code phi | WL threshold exceeded | IF threshold exceeded | Either channel | 95% Wilson interval for either |
|---|---:|---:|---:|---:|
| 0 | 182 | 195 | 376 / 250000 = **0.1504%** | 0.1360–0.1664% |
| 0.5 | 9865 | 9787 | 18771 / 250000 = **7.5084%** | 7.4058–7.6124% |
| 0.9 | 112085 | 112890 | 170838 / 250000 = **68.3352%** | 68.1526–68.5173% |

The two marginal counts include simultaneous exceedances. There were 1, 881
and 54137 simultaneous exceedances respectively; the real detector labels these
WL because it checks WL first. Therefore a historical IF share is not the IF
channel's marginal detection probability. The WL/IF correlation from shared
independent measurement noise in this ideal fixture is **0.35487**, so the
channels are not independent even when time samples are.

The iid standard deviations are 0.414680 cycles and 1.489202 m per epoch;
their ideal standard errors at n=20 are 0.092725 cycles and 0.332996 m. No floor
binds in the iid or phi=0.5 samples; 57 phi=0.9 windows bind at least one floor.
The iid channel rejection counts agree with the analytic t tail within the
predefined six-binomial-standard-deviation test tolerance.

For a stationary scalar process with autocovariance gamma(h),

```
Var(mean) = [n*gamma(0) + 2*sum_{h=1}^{n-1} (n-h)*gamma(h)] / n^2
```

For pure AR(1), this is sigma²/n times
`D = 1 + 2*sum((1-h/n)*phi^h)`. At n=20, D is 2.8000 for phi=0.5 and 11.0942
for phi=0.9. Moreover, centering each short correlated window biases its sample
variance downward relative to marginal variance:
`E[s²] = sigma²*(n-D)/(n-1)`. This compounds the missing covariance terms.
Measured ratios `E[mean²]/E[ALC sigma²]` are WL/IF **0.9996/0.9992**,
**3.0765/3.0923**, and **23.3127/23.6512**. These are ratios of aggregate
moments, not per-window confidence bounds. NIST explicitly identifies checking
autocorrelation as necessary to justify the usual
[s/sqrt(n) formula](https://www.itl.nist.gov/div898/handbook/eda/section3/eda331.htm).

Separate stationary streams use seed 17092026 and **1,000,000 samples per phi**.
Here windows overlap, the detector clears on trips, and no external engine
restarts or state feedback are imposed:

| Code phi | Full-window checks | WL events | IF events | Events / 1000 samples |
|---|---:|---:|---:|---:|
| 0 | 988600 | 288 | 311 | **0.599** |
| 0.5 | 769587 | 6172 | 5954 | **12.126** |
| 0.9 | 179607 | 26523 | 16655 | **43.178** |

Those are operational event rates for these streams. They differ from the
independent-window probabilities because exceedances cluster and clears suppress
subsequent checks. No independent-binomial confidence interval is applied to
these dependent events. A clear creates a distinct counted event, not proof
that the next event is statistically independent. External PBT/reset selection
and `act` feedback change the operational distribution again.

## 4. Saved-trace read and limits

The reusable `scripts/alc_statistics_audit.py` reads the PBT-only controls
`runs/_pbt_sim6` and `runs/_pbt_field6`, avoiding ALC refloat feedback. It excludes
the first 600 s, selects lexicographically first paired GPS L1/L2 signals,
rejects missing/nonfinite inputs, and splits at cadence gaps or pair changes.
It also reports a stricter screening: split at >5 mm phase-bias changes,
discontinuity-counter changes or explicit product events. It retains stretches
with at least 20 samples. Current-epoch samples start the new stretch.

For each stretch, lag-1 autocorrelation is
`sum((x[t]-mean)*(x[t+1]-mean))/sum((x[t]-mean)^2)`; stretches are equally
weighted in the reported quantiles. Cadence is 30 s on both beds. Short-stretch
autocorrelation estimates are noisy and biased; these are descriptive numbers,
not significance tests or fitted AR parameters.

| Product-screened stretches | Stretches / samples | WL lag-1 p50 / p90 | IF lag-1 p50 / p90 | Median absolute IF stretch mean |
|---|---:|---:|---:|---:|
| Sim | 53 / 2434 | -0.033 / 0.147 | -0.010 / 0.138 | 0.536 m |
| Field | 96 / 2902 | 0.069 / 0.465 | 0.106 / 0.511 | 0.246 m |

Without product screening, contiguous-pair stretches give sim IF lag-1 p50/p90
-0.014/0.074 and field 0.019/0.346; both views are in `statistics.json`.
The field has a more persistent upper tail in these descriptive measures.
Neither view establishes a stationary field null. Nonzero means may reflect
ambiguity error, nuisance bias or transient model/state mismatch; they cannot
be assigned to one cause from these combinations alone.

The old traces contain rounded innovations and post-update ambiguity variance,
but not full-window statistics for every check, complete ambiguity generations,
all excluded-row reasons or a true ambiguity-error label. Visible stretches
cannot reconstruct the actual monitor population. The saved runs also predate
ALC-ARC. In particular, the historical 106 field / 26 sim `act` trips and their
5.6 / 4.25 median ratios are **not false-alarm estimates or evidence for a new k**.
The earlier 500-epoch no-trip unit realization is a smoke test, not a rate bound.

## 5. Decision before tuning

Keep k, window length, floors and default-OFF posture unchanged. The historical
position improvements remain empirical results; the proposed statistical
interpretation does not explain or validate their cause. The old approximately
1.33 m IF / 0.37 cycle WL figures are nominal four-standard-error thresholds
under iid noise, not minimum detectable biases at a specified power. Detection
power and false-alarm budget must both be stated to claim an MDB.

The next calibration work should have this order:

1. Define the null and acceptable **false refloats per satellite-hour / run**,
   plus detection power/delay for specified ambiguity errors. Keep code-bias,
   physical phase-bias and known frame-change injections as separate cases.
2. Add observation-only telemetry for every eligible check: sample times,
   selected pair/generations, admission/restart reasons, both means and scales,
   floor flags and both exceedances. Run matched OFF/observe streams on the
   corrected implementation and verify unchanged consumer state/products.
   Observe still clears on trips, so log enough samples to replay alternatives.
3. Test the covariance model against known-null simulated ensembles and
   independent field evidence. For a linear combination a of innovations,
   the modeled instantaneous variance is `a*S*a'`, with `S=H*Pminus*H'+R`
   under the KF assumptions. Include product uncertainty/cross-covariance where
   absent from that model; for a window use the full temporal covariance or
   an independently validated approximation. Do not blindly add P to the
   existing sample scale and count the same uncertainty twice.
4. Calibrate the **complete** two-channel, overlapping, selected and restarted
   monitor on independent null runs before comparing k/window choices. Validate
   power under injected defects and the position cost of acting separately.

An effective sample-size substitution alone does not restore a Student-t law,
remove nuisance bias or solve the selection problem. Longer windows can make
a persistent nuisance more significant while still failing to identify its
cause. Existing evidence does not select a justified replacement threshold,
window length or covariance estimator.

This calibration gap blocks statistical claims and promotion. It does not
change the existing experimental default-OFF runtime. A six-hour observed
false-alarm calibration is deliberately not claimed by this audit.

## 6. Reproduction and verification

Commands from the repository root:

```powershell
scripts/vs2026_build.ps1 -Config x64-Release -Target gnss_tests
build/x64-Release/gnss_tests.exe --gtest_filter=AmbLevelStatistics.*:AmbLevelCheckTest.*:AmbLevelArcTest.*:PbTranslationPremiseTest.*:PbStepTranslateTest.* > runs/_alc_stat_20260917/tests.log
python scripts/alc_statistics_audit.py --test-log runs/_alc_stat_20260917/tests.log --output runs/_alc_stat_20260917/statistics.json
python scripts/docs_index_check.py
git diff --check
```

The Python read requires NumPy (2.4.6 used); it does not require SciPy. It
computes the analytic t tail by transformed Simpson integration and checks
convergence at double resolution. Production tests use the existing C++ test
target. Source changes in `AmbLevelCheck.hpp` are comments correcting the
statistical contract; no executable production statement or configuration is
changed by ALC-STAT. Verification results follow below.

Final verification, 2026-09-17: VS 2026 x64-Release `gnss_tests` build PASS
(81 s); **40 tests from five suites PASS**, including four new statistical
tests (750,000 independent windows and three million streaming samples).
The script completed on both saved beds and produced finite JSON results;
Python compilation and the documentation index check passed. Build and test
logs plus the complete numeric census are under `runs/_alc_stat_20260917/`.
No new producer/consumer positioning run was made, and no golden or default
was changed. The missing operational calibration remains the next work item,
not a completed result of this audit.
