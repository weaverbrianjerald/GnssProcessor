# S-basis conformance — the per-deficiency conformance table, the implicit S-basis the code adopts per mode, the compatibility audit and the design charter for the network side

**STATUS**

- **id:** S-basis conformance
- **topic:** network-vehicle
- **status:** reference
- **summary:** the per-deficiency conformance table, the implicit S-basis the code adopts per mode, the compatibility audit and the design charter for the network side
- **dates in the text:** 2026-06-15 → 2026-08-15
- **knobs / posture:** `estimation.ionosphere_weighting`
- **hashes named in the text** (commit / exe / byte-cert): —
- **run dirs named in the text:** —
- **source:** `docs/archive/SPEC_sbasis_realization_contract.md` — §F1, §F2, §F3, §F4, §F5, §F7, §F8, §F9

> Results are appended TO THIS FILE, below the verbatim text. The archived monolith is frozen; never append there.

---
<!-- BEGIN VERBATIM: SPEC_sbasis_realization_contract.md -->
## §F1 Verdict, and the one structural divergence

The **user side is a faithful, S-basis-INVARIANT realization of Odijk PPP-RTK — realized through
differencing operators and a frozen-pivot covariance pin, not through Odijk's explicit
minimum-constraint / estimable-bias parametrization.** The estimable functions the filter carries
(geometry, ZWD, slant-iono-lumped-with-DCB, DD-integer ambiguities) are the S-basis-invariant
observables of the 2016 model, related to it by a one-to-one S-transformation. The integers MLAMBDA
recovers are the same double-differenced integers `N_{1r}^{ps}` of Odijk Eq. 9. Conformance is high
and the architecture is correct **for a user engine**.

**The one structural divergence that matters:** the code never uses an explicit S-basis. Every
Odijk rank deficiency is disposed of by *differencing it away*, *importing it as a product
correction*, or *absorbing it into the float ambiguity* — never by an explicit minimum constraint.
That is a legitimate *implicit* S-basis (differencing **is** an S-basis choice). But it means **no
object in the code declares the convention it assumes** — which IF code-bias pair sits in the
clock, which ambiguity datum sits in the phase bias, which pivot. **Sufficient for a consumer; the
entire problem for a producer**, which must declare a convention the consumer can reproduce.
(§A–§D are that declaration, built since.)

## §F2 Per-deficiency conformance table (2026-06-15 anchors)

The three mechanisms are **differenced / imported / absorbed-into-ambiguity**; **NO row uses an
explicit minimum-constraint equation** — the defining structural fact.

| Odijk deficiency | Code mechanism | Anchor (2026-06-15) |
|---|---|---|
| **(a) Clock datum** | Receiver clock estimated (PPP) or **differenced away** (BSSD zeroes the column); satellite clock **imported** as the IGS IF clock | `BssdTransform.cpp:221`, `PppModel.cpp:1367` |
| **(b) Code-bias IF↔clock** | Satellite side absorbed into the imported IF clock; receiver side estimated as IFB/DCB states in the *same IF datum* via GF-IF coefficients (`μ1/dmu`, `μ2/dmu`) | `StateInitializer.cpp:103` |
| **(c) Phase-bias↔ambiguity** | **No phase-bias state anywhere.** Satellite phase bias **imported** (SSR/BIA); receiver phase bias **differenced away**; uncovered ⇒ absorbed into the float ambiguity + flagged `markAmbiguityUnresolvable` | `MeasurementModel.cpp:1160`, `StateManager.cpp:1103` |
| **(d) Iono↔DCB** | Estimated per-sat L1-referenced slant iono = iono + DCB lump (Eq. 3); pivot iono column zeroed in SD | `MeasurementModel.cpp:1098`, `BssdTransform.cpp:236` |
| **(e) Ambiguity↔phase-bias** | DD integers fixed by MLAMBDA; **ZD** state per-(sat,signal), AR differences SD→DD internally (`buildDdPairs`, elevation pivot); baseline holds explicit DD states | `AmbiguityResolver.cpp:386`, `:561`; `StateKeySchema.hpp:94`, `:256` |
| **(f) ISB datum** | GPS-hardcoded inter-system reference; ISB estimated per non-GPS system, differenced away in same-system SD | `StateManager.cpp:54`, `StateKeySchema.hpp:451` |

Supporting theory recap: the estimable **satellite clock** = true clock + IF satellite code bias
(+ regional ZTD lump); the estimable **satellite phase bias** = true phase bias + a code-bias lump
+ **the network pivot-receiver's non-integer ambiguity `M_{1,1}^{ps}`** — that hidden ambiguity is
exactly what makes the user's ambiguities come out as double differences (Eq. 9) even though the
user never sees the network pivot. Corrections are **S-basis-covariant** (TK-2015): the user
recovers integers *only if* it adopts an S-basis compatible with the network's — same satellite
pivot, same IF code-bias convention in the clock, same ambiguity datum and wind-up model in the
phase bias, same sign. The **three user models** in increasing iono-constraint strength (faster AR,
greater risk) are iono-**float**, iono-**weighted** (the regional workhorse) and iono-**fixed**.

## §F3 The implicit S-basis the code actually adopts, per mode

**PPP-RTK (BSSD on):**

- **Receiver datum** — fixed by between-satellite SD: receiver clock, phase clock, code biases
  (DCB/IFB) and ISB are all driven out by the differencing operator
  (`BssdTransform.cpp:221-235`). **Stronger than Odijk's "nominate receiver 1"**: SD eliminates the
  *whole* receiver-side block per constellation.
- **Satellite datum** — the **per-constellation pivot satellite** (`PivotManager`, GLONASS
  excluded). Pivot ambiguity / iono / sat-bias are the held quantities, realized as a **hard pin in
  TWO places**: covariance→0 via `freezePivotAmbiguityCovariance` /
  `freezePivotIonoAndBiasCovariance` (`PivotManager.cpp:849`, `:881`), and value→0 via BSSD
  column-zeroing (`BssdTransform.cpp:236–251`). *Two places realize "value 0, variance 0", not one.*
- **Clock/bias convention datum** — ***IMPORTED, not chosen by the code***: satellite clock = the
  IGS ionosphere-free code-bias-absorbed clock referenced to the BIA
  `SATELLITE_CLOCK_REFERENCE_OBSERVABLES` pair (`ProductParser.cpp:1003`); satellite code biases =
  per-signal OSB made IF-clock-consistent on the receiver side via GF-IF coefficients; satellite
  phase biases = per-signal OSB from SSR. **The code adopts whatever S-basis the product files were
  generated under** and re-expresses its receiver states to match (the GF-IF coefficient choice is
  the only active alignment).
- **Inter-system datum** — GPS, hardcoded.

**Baseline (DD):** satellite datum = per-constellation pivot (`DdPivotManager`, SNR-ranked);
receiver datum = the fixed reference station. DD removes *both* satellite-side product error and
receiver-side terms, so the baseline solution is **product-convention-INSENSITIVE** — the robust
fallback whose correctness does not depend on the §F5 compatibility contract.

**Versus Odijk's explicit S-basis:** Odijk fixes one parameter per deficiency and *retains the
estimable biases as named parameters you can print, broadcast and audit*. GnssProcessor fixes the
same dimensions but **discards** the satellite-side estimable biases into imported corrections and
**discards** the receiver-side estimable biases into the differencing operator. Estimable
observables are identical; the difference is that **Odijk's parametrization is invertible to the
conventions and the code's is not.**

## §F4 What is enforced ACTIVELY vs only NEGATIVELY

**The only actively enforced S-basis contract is GF-IF alignment** (`StateInitializer.cpp:103`):
the clock-reference observable pair is read from the BIA DESCRIPTION (`ProductParser.cpp:1003`) and
the receiver code-bias states are aligned to that IF datum. Correct and S-system-aware.
**Everything else is enforced NEGATIVELY**: a wrong pivot / datum / sign / wind-up convention
degrades to **failed AR validation ⇒ no fix** (ratio + bootstrap success rate ≥ 0.999 + per-DD
residual cap, `AmbiguityResolver.cpp:642`) — never to a silent wrong fix. Safe, but there is **no
positive cross-check**.

**Atmosphere.** Iono-float fully present (per-sat L1-referenced slant iono state, RW Q mapped to
slant by `1/sin²el`, `MeasurementModel.cpp:1098`). Iono-weighted present but **GIM-only**:
`applyGimIonoUpdate` injects the IONEX value as a per-sat soft pseudo-obs, `R` is
per-constellation `diag(var) + σ_c²·11ᵀ` with the all-ones block leaving the per-constellation
common mode (the iono datum) free (`Application.cpp:552`); config
`estimation.ionosphere_weighting.*` (`GnssConfigLoad.cpp:934`); σ→0 / σ→∞ sweeps the full
fixed↔weighted↔float continuum. Iono-**fixed** is absent as a first-class model. Troposphere: ZWD
RW + GMF/VMF3 + optional Chen-Herring gradients + a soft ZWD constraint anchoring the BSSD
[up, ZWD] null space (`GnssConfigLoad.cpp:893`) — correct.

**Integer AR is S-system-correct, but ⚠ PRODUCTION AR IS WL-ONLY.** The resolved quantity is the
genuine DD integer *provided* the satellite phase bias is removed as a known correction (Odijk
§3.2): receiver phase bias removed by **BSSD** (rows that cannot pair to a pivot are rejected
precisely because "the Kalman filter would then absorb the receiver clock and biases",
`BssdTransform.cpp:106`); satellite phase bias removed by the **known SSR correction**
(`MeasurementModel.cpp:1160`), or, where uncovered, the ambiguity is `markAmbiguityUnresolvable`
and excluded from LAMBDA — the code correctly **refuses to integer-fix a bias-contaminated
ambiguity** (`MeasurementModel.cpp:650`, `:669`); GLONASS correctly excluded (FDMA inter-channel
bias does not cancel in SD). Two caveats:

1. **The shipped configs set `"wl_only": true`** (`config/ppp_rtk_internal_AR.json:69`,
   `config/realtime_ppp_rtk_AR.json:123`) — MW-derived WL integers applied as Kalman pseudo-obs,
   **NL LAMBDA never searched**. The full WL+NL cascade and the per-band DD LAMBDA exist but are
   unexercised by live configs. **"Integer AR" today = WL-fixed, float-NL — be precise about this
   in convergence/accuracy claims.**
2. The integer property is decomposed in the uncombined L1/L2 domain (MW-WL + LAMBDA-NL), **not**
   via IF. The IF combination is float-only and AR-excluded (`integer_capable = false`,
   `MeasurementModel.cpp:1922`). No Standard-PPP IF→(WL,NL) integer path exists ⇒ the
   satellite-pivot / SSR-phase-bias machinery is the **only** route to integers.

## §F5 The compatibility audit — what AR silently trusts, and the confirmed gaps

For AR to be integer-valid the consumed satellite phase bias must (i) be generated under the same
per-satellite pivot ambiguity datum the user differences against, (ii) use a wind-up model the user
reproduces, (iii) match the IF clock's code-bias convention, (iv) match the assumed sign.

| Requirement | Enforcement |
|---|---|
| (iii) IF code-bias convention | **ACTIVE** — GF-IF alignment to the BIA reference pair (`StateInitializer.cpp:103`). *Risk:* if the clock product and the BIA OSB come from different providers with different IF reference pairs, alignment uses the BIA pair; **no cross-check of clock-product provenance**. |
| (i) Phase-bias ambiguity datum / pivot | **NEGATIVE only** — mismatch ⇒ failed validation ⇒ no fix. No positive check. |
| (ii) Wind-up / yaw model | **⚠ CONFIRMED GAP** — SSR `yaw_angle_deg` / `yaw_rate_deg_per_s` are *decoded* (`RtcmSsrDecoder.cpp:333`) but **read NOWHERE**; `computePhaseWindUp` derives its own yaw-steering attitude from sun/velocity geometry (`GnssCorrections.cpp:1426`). Consistency rests on both sides independently computing the same yaw — a known **eclipse / noon-manoeuvre AR hazard** and an S-basis-adjacent convention a network must eventually pin. |
| (iv) Sign | **EMPIRICALLY PINNED** to one provider (`kSsrPhaseBiasSign = kSsrCodeBiasSign = +1.0`, `MeasurementModel.cpp:1157`). **Onboarding a second SSR provider is a live hazard.** |
| Discontinuity reset | **ACTIVE** — `noteSsrDiscontinuity` forces `resetAmbiguity` on a bias jump (`RealTimeEngine.cpp:1082`). |

Net: a convention mismatch degrades to *no fix*, never a *wrong fix* — acceptable, but **the engine
is not self-describing about the S-basis it assumes.**

## §F7 Implications for the NETWORK side — the design charter

*(The section `src/core/app/ApplicationNetwork.cpp:15` and
`src/core/app/ApplicationNetworkSetup.cpp:27` cite as "§7".)*

Goal in S-system terms: estimate, from multiple stations, the estimable **satellite clock** (Eq. 2)
and estimable **satellite phase bias** (Eq. 4, carrying the pivot-receiver ambiguity lump) under a
**DECLARED S-basis**, and broadcast them so a user reproducing that S-basis recovers integers.
Orbits from the IGS stream are sufficient.

**Assets the architecture already provides (genuine reuse, not a rewrite):** (1) the measurement
model is **undifferenced + uncombined at its base** (`buildJacobianRow`), with differencing applied
as a *post-assembly* S-transform (`BssdTransform::apply`) — a network estimator runs on exactly
that substrate, it must simply *not* apply the receiver-side differencing and instead estimate the
satellite-side lumps on the undifferenced rows; (2) `StateManager`'s dynamic key→index blocks
already support per-(sat,signal) bias states (`makeSatBiasKey`, `sat_bias_indices_`), and the
*fallback* satellite code-bias state (fires only on missing product coverage,
`MeasurementModel.cpp:1128`) is structurally **the network's satellite-code-bias parameter in
embryo** — promote it from fallback to first-class; (3) the GF-IF decomposition
(`StateInitializer.cpp:103`) is the same IF/DCB algebra the network clock product must carry on the
satellite side — share one bias algebra across network and user.

**What was genuinely missing (three pieces) — and its OUTCOME.** *(2026-08 status: **all three are
BUILT and in production.** Kept as the charter because the code cites it.)*

1. **A multi-station joint estimator.** `BaselineMulti` is a *star topology* — N rovers each
   independently DD'd against one fixed reference, **no inter-rover correlation, no joint
   satellite-parameter estimation** (`GnssConfigTypes.hpp:84`). *Structural reason a network is
   required:* a single-receiver BSSD pipeline **CANNOT** estimate satellite phase biases —
   between-satellite SD on one receiver removes the receiver datum but leaves the satellite phase
   bias confounded with that station's pivot ambiguity. You need multiple stations (so the
   satellite-indexed bias is observed across receivers) **and** you must stop differencing the
   receiver side away. ✅ **BUILT** — `NetworkEstimator` / `NetworkNormalEquations`.
2. **An explicit satellite-PHASE-bias state** — existed nowhere in 2026-06. ✅ **BUILT** —
   `StateManager::getSatPhaseBiasStates`, PB keys in `StateKeySchema`.
3. **An explicit minimum-constraint S-basis** — the one S-system mechanism the codebase had *never*
   used. ✅ **BUILT** — `NetworkSbasis`, with a **runtime rank assertion** (§A rank bookkeeping).
   The **product emit path** is `NetworkProductEmitter` / `NetworkProductIO` with a declared **S
   line** (§D), and **network-scale pivot handover** machinery plus a persistent `last_pivot` map
   are live.

**The concrete path (as charted, and largely executed):** build the network estimator on the
existing undifferenced+uncombined `buildJacobianRow`, with satellite-side lumps as **explicit
estimable states** and an **explicit minimum-constraint S-basis** — for the network, deliberately
do the thing the user side avoids (add satellite phase-bias states, promote the satellite code-bias
state, add per-station receiver clock/bias/ambiguity states across a *stacked multi-station* normal
matrix). Apply Odijk's explicit S-basis as minimum constraints (fix reference-receiver
clock/biases, fix pivot-satellite-and-reference-receiver ambiguities, fix the ISB datum); doing
this on the network side does **not** perturb the (correct, differencing-based) user side.
**★ STRONGEST LEVERAGE — adopt the IGS-SSR clock+phase-bias S-basis as the broadcast convention**
so products are drop-in for the existing user consumer: the user side already applies SSR phase
bias and resets on `discontinuity_counter`, so if the network emits in that same convention/datum
the existing `SsrCorrectionStore` path is the validation harness, and **the network's correctness
criterion becomes operational and closed-loop: *"does our own user engine recover integers from our
own products on an independent station?"*** Reuse the GF-IF decomposition for the satellite-side
IF/DCB split so network and user share one algebra. `PivotManager` is reusable as the network's
satellite-datum selector, **but the network needs a SINGLE GLOBAL pivot per constellation, stable
over the broadcast interval** — not the user's per-receiver sticky-hysteresis pivot. The handover
S-transform (`applyCovarianceReReference`) is the right tool but must be applied network-wide and
reflected in the product datum.

## §F8 Honest divergences / risks (as of 2026-06-15)

1. **No explicit S-basis / minimum-constraint object** — legitimate for a user engine, but blocks
   self-description and is the structural prerequisite for §F7. *(Closed on the network side; still
   true of the user side.)*
2. **Phase-bias S-basis compatibility enforced only negatively** — no positive cross-check; the
   sign is pinned to one provider. **Second-provider onboarding is a live hazard.**
3. **SSR yaw fields decoded but unused** (`RtcmSsrDecoder.cpp:333` → read nowhere;
   `computePhaseWindUp` computes attitude independently). Eclipse-season AR hazard; wind-up is part
   of the phase datum a network must pin.
4. **Production AR is WL-only** (`wl_only: true`) — "integer AR" today is WL-fixed, not
   full-integer.
5. **No SSR-STEC iono-weighting path in real-time** — Odijk's fast-AR workhorse is GIM-only /
   post-proc.
6. **GPS-hardcoded ISB datum + GLONASS exclusion** — theory-faithful for a user engine; a
   multi-GNSS *network* product must decide and *declare* ISB/IFB conventions per constellation
   explicitly.
7. **§8.7 — frozen-pivot hard-zero pin vs soft constraint.** Equivalent in exact arithmetic; **in
   finite precision the handover S-transform `P ← T·P·Tᵀ` is where datum-transfer error
   accumulates.** The DD path has PSD/symmetrize guards (`DdPivotManager.cpp:208`).

   > **⚠ ANSWERED 2026-08-15 by reading the code — the 2026-06 question is RULED, and the true
   > state is sharper than either reading.** (a) **The user-path half is ruled deliberately
   > unguarded, with rationale, at the call site**: `include/filter/CovarianceReReference.hpp:22-29`
   > states that "the single-receiver user BSSD path (`PivotManager`) deliberately runs UNGUARDED —
   > at user scale the groups are small (~8–12 states) and handovers are rare, so FP error in
   > T·P·Tᵀ does not accumulate", mirrored at `src/core/filter/PivotManager.cpp:371-374`. So there
   > is nothing to "confirm" on that side. (b) **The network-path half is a stated MUST with ZERO
   > call sites — an actionable defect, not an audit question.** The same header says the
   > minimum-constraint NETWORK producer "MUST call `enforcePsd` after the transform", and
   > `enforcePsd` was extracted into the shared `CovReReference` namespace precisely so the network
   > producer could reuse it — yet a repo-wide grep finds exactly one non-test call site,
   > `src/core/filter/DdPivotManager.cpp:221` (the DD path). **The network producer never calls it.**
   > (c) ✅ **FIXED 2026-08-15 (serial cleanup pass).** Both citations were written bare as
   > "conformance doc §8.7" (`include/filter/CovarianceReReference.hpp:22`,
   > `src/core/filter/PivotManager.cpp:373`) and resolved only into `docs/attic/`; both now read
   > `docs/SPEC_sbasis_realization_contract.md §F8.7`. Comment text only — no semantics touched.
   > **(b) remains OPEN: the network producer still never calls `enforcePsd`.**

## §F9 The backlog, with its 2026-08 outcome

**P0 — unblock real-time PPP-RTK quality (user side) — BOTH STILL OPEN:**

- [ ] **SSR-STEC iono-weighting path.** `SsrCorrectionStore` carries orbit/clock/bias only and the
      SSR-to-STEC adapter is **explicitly unbuilt** (`RealTimeEngine.cpp:176`), so Odijk's
      *workhorse* weighted-iono model — the thing that enables fast/instantaneous AR — is
      **GIM-only in post-processing and entirely absent in real-time, directly capping real-time
      time-to-fix**. Add a STEC/iono push to `SsrCorrectionStore` and a real-time adapter feeding
      the existing weighted-iono pseudo-obs mechanism (`applyGimIonoUpdate` is the post-proc
      template). (§F4)
- [ ] **Decide WL+NL in production**, or document explicitly that production AR is WL-only by
      design. (§F4)

**P1 — S-basis hardening (makes the user side self-describing) — ALL STILL OPEN:**

- [ ] **Consume the SSR yaw fields in `computePhaseWindUp`**, or document that independent yaw
      modelling is the intended contract and gate AR through eclipse seasons accordingly.
      (§F5, §F8.3)
- [ ] **Positive S-basis cross-check at product ingestion**: verify the phase-bias product's pivot /
      ambiguity-datum / sign convention against the engine's assumptions; warn loudly on mismatch
      instead of relying on silent AR-validation failure. (§F5)
- [ ] **Cross-check clock-product provenance against the BIA IF reference pair** to catch
      mixed-provider IF-datum inconsistencies. (§F5 (iii))

**P2 — the network side. ✅ ALL FIVE BUILT AND IN PRODUCTION** (see §F7's outcome, and §A–§D for
what they actually realize). The five as the absorbed charter listed them, with their §F7 anchors:
explicit satellite phase-bias state — new `StateKeySchema` key + `StateManager` block mirroring
`sat_bias_indices_`, promoting the fallback satellite code-bias state to first-class (§7.2);
multi-station joint estimator — replace the `BaselineMulti` star topology with a stacked
multi-station normal matrix sharing satellite-side unknowns, estimating on
undifferenced+uncombined rows without receiver-side differencing (§7.1); explicit
minimum-constraint S-basis machinery — reference receiver, per-constellation global pivot
satellite, clock datum, ambiguity datum, ISB datum, and *record* the S-basis in the emitted product
(§7.3); the product emit path in IGS-SSR clock+phase-bias convention, wiring the existing user
consumer as a closed-loop integer-recovery test on a held-out station (§7); network-scale pivot
handover — a global per-constellation pivot stable over the broadcast interval (§7, §8.7).
**Do not read the P2 list as open work** — with one carried residue: that last item also asked to
"confirm `applyCovarianceReReference` conditioning matches the DD path's PSD/symmetrize guards",
and §F8.7 above records what that confirmation actually found (the network producer never calls
`enforcePsd`).
