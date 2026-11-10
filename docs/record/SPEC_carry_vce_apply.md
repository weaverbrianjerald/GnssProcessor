# SPEC — M2 v2: carry_vce_apply as LASER's variance-factor iteration

**Status**: **BUILT 2026-08-04, default-OFF — and COMMITTED** (`include/filter/NetworkCarryVceApply.hpp` landed in `2862318`; the "uncommitted at time of writing" caveat is retired 2026-08-15). The not-implemented loader error is gone; the requires-`carry_vce` error stays. Unit gates `tests/test_carry_vce_apply.cpp` (10 cases) pass; suite green at the time (2026-08-04) at 1212/1213, the one skip being the pre-existing fixture gate — ⚠ dated, not the present baseline (1360 / 196 as of 2026-08-12, CLAUDE.md). Wiring smoke §11. **NOT field-validated**: the §7 acceptance contract is untouched and unclaimed.
**Origin**: NGS LASER Guide v2.7 slides 5-93..5-95 (PDF pp. 405–407) — VCE "by use of factors" (Koch 1999 §§3.6.1–3.6.2; Lucas 1985): multiplicative factor updates on a diagonal system, positive by construction, iterate `(αᵢ²)ⱼ = (σ̂ᵢ²)ⱼ₋₁(αᵢ²)ⱼ₋₁`, converged when σ̂ᵢ² ≈ 1. Slide 5-96 is the verdict this card adopts: the factor method is LASER's default; BIQUUE adds an iteration-death mode (Cholesky termination on transient negative components) and agreed with the factor method in every test — **BIQUUE is out of scope permanently**.
**Memory — ALL binding**: `project_m2_lsvce_carryq` (the estimator, the field numbers, v2's pre-agreed acceptance contract — read in full before coding), `project_adaptive_iono_q_network` (era-2 discharge FALSIFIED adaptive-q on the measurement side; prediction-side CONFIRMED — this card lives on the prediction side and must stay there), `project_iono2_card` (anti-ratchet), `project_day129_pivot_cb_ratchet` (what a stiff wrong floor does), `project_laser_theory_mapping`.
**Scope discipline**: default-OFF, OFF byte-identical, **FINGERPRINTED** (solve-affecting; fresh convergence only; conditional-fingerprint discipline — seed and legs must all carry it, never flip mid-chain). Armed in exactly **one** shipped config — `config/test_suite/_vceapply_smoke_on.json`, this card's own ON arm (§11.1); nothing else in `config/` resolves it true, and it stays off the golden lineage per the no-promotion directive (SCOPED ledger row on validation). *(Corrected 2026-08-21: this line previously read "NOT armed in any shipped config", which contradicted §11.1.)*

---

## 1. Why — today's formal σ are floor artifacts

M2's monitor measured the hand carry floors 30–120× optimistic (robust, quiet epochs) and the M3 ladder proved the consequence: a 40× floor swing moved formal dd_σ by 14 % with IDENTICAL actual behavior — the formal covariance the products carry (including the URA channel) is a floor artifact at the carry level. The fix is not new theory: the monitor already estimates σ̂² per (family × constellation); LASER 5-95 is the standard, positivity-guaranteed way to feed a factor back. v2 makes `fold_var` track the measured scale under guards the falsified-approaches archive dictates.

## 2. The iteration, streaming form

Per (family × constellation) g, per epoch:

```
target_g   = robust_floor_g / hand_fold_var_g       the monitor's measured factor (PRIMARY = robust; the
                                                    mean is reported alongside, never applied — M2 verdict)
α²_g      ←  clamp( α²_g · step,  [1, apply_max] )  step = clamp(target_g/α²_g, [1/slew, slew])
fold_var   =  hand model evaluated as today, then × α²_g   at BOTH fold sites (pl inject, pb/cb inject)
sigma_hat_post_g = target_g / α²_g                  → 1 at convergence (LASER 5-95's σ̂ᵢ²)
```

**AS BUILT — the denominator is the measured hand fold variance, not the config floor.** The spec drafted `target = robust_floor / hand_floor_g` with `hand_floor_g` the config constant (`phase_leveling_var_floor_m2` etc.). That is well posed only when the floor BINDS, and two live cases break it: a lineage with no floor set (`= 0`, ratio undefined), and a carry whose `var_prev + q·Δt` sits well above the floor (ratio over-inflates). The build therefore has `NetworkCarryVce` accumulate the geometric mean of the fold variance **as the hand model formed it**, over the same rows and forgetting window (`Estimate::hand_fold_var`), and uses that as the denominator — equal to the config floor exactly when the floor binds, well posed everywhere else.

**Both halves of `target` are α²-INDEPENDENT, and that is load-bearing, not tidy.** `robust_floor` is a function of ê² alone and `hand_fold_var` is pre-scaling, so the update is a first-order tracker of a fixed point, not feedback through a lagged measurement. Driving the step from σ̂² measured on the *standardized* residuals — the naive reading of 5-95 — closes the loop through the monitor's exponential window, whose mean lag is φ/(1−φ) ≈ **86 epochs** at the 60-epoch default. A saturated 5 %/epoch ramp against an 86-epoch lag overshoots by e^(86·ln1.05) ≈ **67×** before it can see itself, then rings. With an α²-independent target the same ramp approaches monotonically and stops, because `step` is formed against the CURRENT α². The convergence READOUT is still σ̂² — just derived (`target/α²`) rather than fed back.

- **Inflate-only in v1: `α² ≥ 1` relative to the hand floor, structurally.** The dangerous direction is DEFLATION (a stiffer-than-truth prior is the lock-in mechanism of record — invariant #1, the pivot-CB ratchet); looser-than-truth is safe-but-slower. This clamp kills the ratchet class by construction, not by tuning.
- **Slew limiter** `apply_slew` per epoch (default 1.05): era-2's lesson is that a ramp at the configured rate limit still discharges — so the ramp is NOT the safety argument; the inflate-only clamp is. The slew keeps the solve's conditioning quiet, and BOTH clamp entries/exits are events (invariant #7).
- **Freeze on `!clean` epochs** (datum_changed — the monitor already skips them, `NetworkCarryVce::beginEpoch`) and below `min_rows` warm-up (census convention: the frozen state is flagged in the trace, never silent).
- **Anti-ratchet = the window.** `carry_vce_window_epochs` forgetting already decays robust_floor after an event era; the factor follows it back down (bounded below by 1). Unit test 5 proves the round trip; the iono2 v3 demean precedent is the design argument.
- **Convergence readout** (LASER 5-95): when α² tracks truth, the RE-standardized innovations' σ̂² → 1. Emit `sigma_hat_post_g` per group per epoch — the live convergence diagnostic and the field gate's calibration ratio.

Scope: `carry_vce_apply_families` default `"pl"` (largest population, best-measured). Constellation pooling only — no per-satellite factors (population too small; the D5 family precedent). PB/CB join only after the pl gate passes.

## 3. Interactions (each verified in-tree)

- **D5's estimator instance is UNTOUCHED** — it standardizes by its own σ̂₀ already; this card must not double-scale (assert: apply never touches the `"dd"` family instance in `NetworkInvariantCarryDriver`).
- **The prior-residual census** standardizes by `fold_var` as USED — with apply on, its `s` absorbs α² automatically (fold_var at the feed site is post-scaling). State this in both docs; the census's σ̂₀,f should converge toward 1 on applied families — a free cross-instrument check (field gate 4).
- **M2 monitor keeps running on the SAME rows** with the scaled fold_var; its estimate() then measures the RESIDUAL misfit (target→1). The pre-apply hand-floor ratio is preserved in the trace for continuity (`alpha2` column).
- **`carry_vce_apply` loader block** (`GnssConfigLoadNetwork.cpp:2462-2474`): keep the requires-`carry_vce` error; delete the not-implemented error; add the new knobs + fingerprint banner.

## 4. Cost

One multiply per fold row + O(groups) bookkeeping per epoch. Unmeasurable.

## 5. Config

| knob | type | default | note |
|---|---|---|---|
| `network.carry_vce_apply` | bool | `false` | exists; FINGERPRINTED |
| `network.carry_vce_apply_families` | string | `"pl"` | subset of `carry_vce_components` (hard error otherwise) |
| `network.carry_vce_apply_slew` | double | `1.05` | per-epoch max factor step (× or ÷) |
| `network.carry_vce_apply_max` | double | `1000.0` | α² ceiling; entry/exit evented |
| `network.carry_vce_apply_min_rows` | int | `200` | freeze below (per group), flagged |

Loader as built: `carry_vce_apply` still requires `carry_vce`; an apply family outside `carry_vce_components` (or outside pl/pb/cb) is a **hard error**, not a silent unity factor; `slew ≤ 1` warns (limiter disabled); `max < 1` is raised to 1; arming pb/cb warns UNVALIDATED (§8); and arming `pl` on a lineage where the phase-leveling carry is not folded (`!phase_leveling || carry_invariant` — Card 4's DD carry supersedes it, so `R.pl_amb` is empty and there are neither fold sites nor monitor rows) warns **INERT** before the run rather than after it. A **warm resume with the apply armed is a hard run error** — §8.

## 6. Unit tests

New file `tests/test_carry_vce_apply.cpp`, one suite.

1. **Factor convergence** (LASER 5-95 semantics): synthetic rows with true σ² = 25× the hand floor ⇒ α² → 25 within slew-limited time and re-standardized σ̂² → 1.
2. **Inflate-only**: true σ² = 0.1× floor ⇒ α² stays exactly 1 (never deflates).
3. Slew and ceiling clamps engage/release with events; freeze on !clean and warm-up.
4. **No cross-contamination**: apply on `"pl"` leaves pb/cb fold_var and the D5 instance bit-identical.
5. **Anti-ratchet round trip**: a 50-epoch synthetic event era inflates α², and after the era the window decay returns α² to within 10 % of pre-era within ~2 windows.
6. OFF byte-identity (structural); apply-OFF ≡ monitor-only ≡ current tree.

## 7. Field validation — the acceptance contract is pre-agreed (M2 memory, verbatim)

Controls: golden 3 h/305 + a 12 h window spanning the hr-4 cliff. Release only, smoke first, seamless lineage, same-build A/B (OFF / monitor-only / apply-pl).

1. OFF byte-identity vs current tree; monitor-only arm byte-identical products to apply-OFF (regression guard on the refactor).
2. **THE CALIBRATION RATIO (the card's acceptance)**: formal dd_σ under σ̂-weighting must match empirical DD-step statistics (→ ~1× from today's floor-dependent 14 % spread). **Explicitly NO divergence-related acceptance** — the hr-4 cliff is event-driven and out of scope; an apply arm that "fixes" the cliff is a red flag, not a win (it means the factor ate an event).
3. **No degradation**: dd-step and withhold counts not worse than the hand-floor baseline (grade tails, not just medians — the G29-era lesson).
4. **Cross-instrument closure**: census σ̂₀,f on applied families → 1; σ̂₀_global's prior-tension ratio drops accordingly (if those cards are armed).
5. **URA honesty**: emitted σ scales by ~√α² on applied families — compare the URA trace against M2's robust trace; they must now agree where they disagreed by 30–120×.
6. Product claims on total_IF only; α² trajectory + clamp/freeze event log attached to the report.

On PASS: SCOPED ledger row (evidence, arms, lineage). The knob stays default-OFF — no promotion step.

## 8. Deferral triage

| deferred | field-blocking? | evidence |
|---|---|---|
| Deflation (α² < 1) | **INERT by design** — the unsafe direction; revisit only with a per-family screen (the census) validated ON | invariant #1 history |
| PB/CB apply | **INERT** — gated behind the pl PASS; PB carries the meter-scale re-level tail (M2: mean/robust ~200-1000×) so its robust/mean split must be re-examined first | M2 field numbers |
| Per-satellite factors | **INERT** — population; constellation pooling v1 | D5 precedent |
| Rate component apply | **INERT** — the field is ALWAYS rate-frozen (constant-gap collinearity); Floor-only | M2 §estimator |
| σ̂/α² snapshot on warm resume | **BLOCKING for warm-chain arming** — α² restarting at 1 mid-chain changes the solve across the seam ⇒ v1 is fresh-convergence-only (fingerprint), and the snapshot (seedRobustFloor + α² field) is the v2 that would relax it | fingerprint rules |

**AS BUILT — the warm-resume deferral is ENFORCED, not documented.** The fingerprint alone cannot catch this case: an apply-ON seed and an apply-ON leg fingerprint *identically*, so the guard that exists for every other fingerprinted knob is silent exactly here. `networkTryWarmResume` therefore throws when `carry_vce_apply` is armed and the snapshot file exists. The fingerprint append is still there (it catches the apply-OFF-seed → apply-ON-leg direction), and α² carries a `[snapshot: EXEMPT]` note in `NetworkCarryState` saying why the exemption is a refusal rather than the harmless observer exemption its neighbours get.

## 9. Standing-invariant pre-flight

1. The apply touches stiff priors' VARIANCES only, inflate-only, with the monitor's event-skip inherited — no new stale-value carry path; slip/jump responses unchanged.
2. Release-only.
3. Deferrals above, one BLOCKING and said so.
4. α², clamp, freeze counters + `sigma_hat_post` are the run-end telemetry.
5. Gates on dd-step/total_IF.
6. Default-OFF byte-identical; three-arm same-build A/B.
7. Freeze/unfreeze and clamp edges are evented with identities — the apply is itself a coupling mechanism and is instrumented as one.

## 10. What this card explicitly does NOT do

Cross-references, not new content — the reasons live where the mechanisms do.

- No BIQUUE, ever (header: slide 5-96 + M2's fixed-W rationale).
- No deflation below the hand floors (§2 inflate-only clamp; §8 row 1); no measurement-side R adaptation (era-2 ground).
- No per-satellite granularity, no rate component, no PB/CB in the first field pass (§8 rows 2–4).
- Does not claim to fix the hr-4 cliff and must not be graded on it (§7.2).
- Does not promote itself: validated ⇒ SCOPED ledger row, default stays OFF (§7).

---

## 11. As built — where the code is

| concern | file |
|---|---|
| the iteration, α² state, clamps, freezes, edges, tallies | `include/filter/NetworkCarryVceApply.hpp` (header-only leaf) |
| `hand_fold_var` accumulator + `addRow`'s new optional arg | `include/filter/NetworkCarryVce.hpp` (additive; default arg ⇒ every pre-v2 caller unchanged) |
| α² lives here (cross-epoch, read by three TUs) | `NetworkCarryState::vce_apply` in `include/app/NetworkRunContext.hpp` |
| **fold site — pl inject** | `ApplicationNetworkAssembly.cpp`, `networkPassB_` (`var_eff *= factor(kPl, sat)`, after the floor) |
| **fold site — pb/cb inject** (both the diagonal and C1 dense branch, via one lambda so they cannot drift) | `NetworkRunContext.cpp`, `networkInjectD2CarryPrior` |
| harvest mirrors (monitor + census see the fold var AS USED, plus the hand value) | `networkHarvestLocalCarries` (pl) · `NetworkStashStatus.cpp` (pb/cb) |
| **the once-per-epoch step** | end of `networkStashStatus`, after the census drain |
| trace + events | `network_carry_vce_apply.csv` (`NetworkRunTraces::writeCarryVceApplyRows`) · tier `VCE_APPLY` in `network_events.csv` |
| run-end telemetry | `NetworkTeardown.cpp` (per-group, with NEVER-STEPPED and ceiling-pinned warnings) |
| fingerprint + warm-resume refusal | `NetworkRunContext.cpp` |
| knobs, validation, banner | `GnssConfigLoadNetwork.cpp` |
| gates | `tests/test_carry_vce_apply.cpp` (10 cases) |

**Step placement is the contract.** The step sits after the last feed of epoch N (the pl harvest in SB_LEVELING, then the pb/cb feed in SB_STASH_STATUS) and before any fold of epoch N+1, which makes literally true: *the fold of epoch N is scaled by α² estimated from every clean carry row through epoch N−1.* Strict predict-then-update, no self-reference, minimum achievable lag — a row cannot exist until the solve that produced it has run, and that solve already consumed the fold. It also guarantees the injector and the two harvest mirrors, all earlier in the same epoch, read one factor table, which is what lets the mirrors reconstruct the used weight exactly.

### 11.1 Wiring smoke (2026-08-04, Release, NOT the §7 acceptance)

Arms `config/test_suite/_vceapply_smoke_{off,on}.json` — 63 sta, GPS, DOY-129, same build, single variable. The substrate is `_ft63_mdb_raimlive.json` because it is `phase_leveling` + `!carry_invariant`, i.e. one of the few fast configs where the pl carry is actually folded.

A first 20-epoch probe (no `phase_leveling_var_floor_m2`): **hand fold var 8.33e-4 m² against a robust carry innovation of 9.00e-4 m² — ratio 1.081.** On an unfloored carry the hand stochastic model is essentially calibrated and the factor has nothing to do. **The 30–120× optimism M2 measured is a property of the FLOOR, not of the carry model** — so the smoke arms set `phase_leveling_var_floor_m2 = 4e-5` (the FT63 value, in BOTH arms so it is not the A/B variable) or the mechanism is never exercised. **Anyone reading M2's "30–120×" as a statement about `var_prev + q·Δt` will mis-scale this card.** Also on that probe: the factor moved off 1.0 only once, at the epoch where `target` first crossed 1 (0.157 → 1.081 as the carry matured), with the unit clamp holding it at 1 for the 16 epochs where the measured carry was TIGHTER than the model — the inflate-only guard doing its job — and both clamp edges plus the warm-up release landing in `network_events.csv` with identities.

**The 1 h arms (121 epochs, floor armed).** The mechanism behaves exactly as designed:

| | OFF | ON |
|---|---|---|
| α² (pl\|G) at run end | — (1.0) | **3.055** (peak 3.966) |
| `sigma_hat_post` | — | **1.000000**, held from ~epoch 41 to the end |
| steps / frozen / slew-clipped | — | 118 / 0 / 20 up, 15 down |
| epochs at ceiling / at unit | — | 0 / 16 (all during ignition) |
| monitor robust σ̂ (pl\|G) | 8.10e-4 m² | 1.92e-2 m² |
| monitor hand-FLOOR ratio | 20.2 | 479.5 |
| **measured hand FOLD-var** | — | 6.28e-3 m² |
| pl carry rows | 78 259 | 126 692 |
| products md5 | `BAC5F905…` | `574F0E59…` (differ) |

Reading, in order of what it establishes:

1. **The factor reaches the fold sites.** Products differ — liveness settled. α² climbed through the limiter, caught its target around epoch 41, then TRACKED it: `sigma_hat_post` pinned at 1.000000 while the target itself wandered 3.97 → 2.91 → 3.06. LASER 5-95's convergence criterion satisfied, and the no-overshoot behaviour the lag-free target buys (§2).
2. **The honest misfit is 3.1×, not 479×.** The monitor's hand-FLOOR ratio reads 479.5, but most rows never fold at the floor: the measured geometric-mean hand fold variance is 6.28e-3 m², two decades above the 4e-5 floor. The factor converged to 3.06 because that is what the model is actually off by. **The M2 "30–120×" numbers are floor ratios and must not be read as the factor this card will apply.**
3. **There is a benign self-consistency loop and it is stable.** `hand_fold_var` has no α² in its formula but reads `var_prev`, a posterior the previous epoch's α² helped set — so a larger α² raises `hand_fold_var`, which LOWERS the target. Negative feedback, one epoch of lag, rate-limited: it converges (measured above) to the self-consistent point where total fold variance matches measured innovation variance. Whether that fixed point is the RIGHT one is a §7 question, not a smoke question.
4. **⚠ A large side effect that is NOT a result.** The OFF control withheld **44 of 121** epochs on the A4 σ̂₀ ceiling; the ON arm withheld **0**, and last-epoch post-fit σ̂₀ went 2.91 → 0.83 all / 10.44 → 7.65 code / 1.10 → 0.037 phase. **Not a win**, for three reasons: (a) the OFF arm is a *deliberately over-stiffened* control — the 4e-5 floor was added to exercise the mechanism and is not this config's shipped posture; (b) inflating carry-prior variances mechanically relieves the tension the post-fit statistic measures, so "σ̂₀ fell" is partly definitional and partly a real fit improvement, and this run cannot separate them; (c) §7.2 says outright that an apply arm which makes a withhold problem disappear is a **red flag** until the calibration ratio is graded. Recorded because it is large and reproducible, not because it argues for the card.

**NOT established by this smoke**, still owed to §7: the calibration ratio (formal dd_σ vs empirical DD-step statistics), dd-step tails, cross-instrument closure against the census σ̂₀,f, URA honesty, and any claim on total_IF. Nor is the §7.1 **cross-build OFF md5 gate** run — the working tree carries several concurrent sessions' changes, so a pre-card build would not isolate this card. The OFF-identity argument here is structural and complete on its own terms: every fold site multiplies by a `double` that is literally `1.0` when the path is unarmed (`x * 1.0 == x` bit-exactly under IEEE-754, asserted family-by-family in unit gate 6), and the only other shared-path change is an accumulator in `NetworkCarryVce` that feeds no pre-existing output. The md5 gate is still the one to run before any field claim.

## 12. Correction (2026-10-07): the target omitted the group redundancy

**Defect.** The target of §2 was `robust_floor / hand_fold_var`. LASER's factor update (slides 5-93..5-95; Snow, *Adjustment Computations*, ch. 16) forms the group estimate as the weighted residual square sum divided by the group's redundancy. A carry row folded at `f = α²·p` with posterior variance `v` has partial redundancy `r = 1 − v/f`, and the carry residual `ê = prior − solved` has `E{ê²} = r·f·σ̂²`. `robust_floor` estimates `r̄·(carry variance)`, as its own Doxygen states, so the target read `r̄·α²σ̂²`, not `α²σ̂²`. For a scalar carry with true prior variance `a·p` and data variance `d`, `E{ê²} = r²(a·p + d)`, and the old target's fixed point solves `α⁴p² + α²p·d + d² = α²·a·p²`. It lies below `a` always, and once `d` exceeds about `a·p/4` there is no fixed point above 1, so the factor stayed at unity. The under-inflation was largest where the carry is stiff against its data (small `r`), the case the factor exists for. Found by the Snow / LASER theory audit of 2026-10-07; the path was default off and not armed on any golden, so no graded result moves.

**Change.** `NetworkCarryVce` keeps every existing channel bit for bit (`robust_floor`, `hand_fold_var` and the LS-VCE normals feed the armed monitor, card D5 and the census) and adds a redundancy-corrected geometric channel: per row `log(ê²/r)`, the hand fold variance and `log r`, only for rows with `r` in (1e-3, 1], read back as `Estimate::robust_var_r`, `hand_fold_var_r`, `rbar` and `n_eff_r`. A row with `r` at or below the floor carries no information on the component, and `r < 0` marks the coupled ambiguity carry's conditional variance above the prior (the census's RNEG population); both are left out and counted (`n_eff − n_eff_r`). `NetworkCarryVceApply` now steps toward `robust_var_r / hand_fold_var_r` and gates warm-up on `n_eff_r`. `network_carry_vce_apply.csv` gains four trailing columns (`robust_var_r_m2, hand_fold_var_r_m2, rbar, n_eff_r`); the run-end line reports both channels.

**Stability.** The target now depends on α² through `r`. In the scalar model it is `f(α²) = α²(a·p + d)/(α²p + d)`: increasing, with its only positive fixed point at `α² = a` and slope `d/(a·p + d) < 1` there. Read through the monitor's window lag, the update is a level tracker on a positive contraction, whose linearized recursion has coefficients summing below one and so decays without alternating at any lag. The failed form of §2's lag note (a multiplicative σ̂² step every epoch) is an integrator; this is not.

**Tests.** `tests/test_carry_vce_apply.cpp` now feeds the scalar model (the earlier feed drew `ê` with a variance independent of α² and fixed `r` at 0.5, which is why it could not see the defect) and gains two gates, 11 cases in all: `RecoversTheTrueFactorWhenThePriorIsStiff` (`d = 10p`, truth 25: the factor reaches 25, where a Python replica of the old target stayed at 1.0) and `RowsWithoutRedundancyAreLeftOutOfTheTarget`. On the same replica the old target also fails the existing per-constellation gate (2.3 against the truth 4). Full Debug suite under VS 2026: 2869 passed, 1 skipped (the fixture-gated SP3 case).

**Standing.** Still built, default off, unvalidated (§7 untouched). §11.1 reading 2 ("the honest misfit is 3.1×") and reading 3's fixed point were formed with the uncorrected target and under-state the factor by about `r̄`; re-run the wiring smoke before any §7 work. On the coupled `pl` family `r` comes from the harvested conditional variance and is an approximation: read `rbar` and `n_eff_r` against `n_eff` before trusting a `pl` factor. A warm-resume seed of the new channel does not exist (`seedRobustFloor` seeds only the uncorrected channel), consistent with §8's fresh-convergence-only rule.
