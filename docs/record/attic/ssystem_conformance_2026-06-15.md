# GnssProcessor vs. Odijk S-System PPP-RTK — Conformance Analysis & Incremental Backlog

**Date:** 2026-06-15
**Scope:** How closely the GnssProcessor estimation engine follows the S-system (S-basis) PPP-RTK
theory of Odijk, Teunissen, Khodabandeh et al., and what the *network* side (an ultimate project
goal) requires that the current *user* side does not provide.

**Method:** multi-agent audit — 7 code-recon passes over the measurement model, state schema, pivot
machinery, corrections/bias path, ambiguity resolver, atmosphere, and config; theory grounded against
the open-access Odijk–Teunissen–Zhang (2011) single-frequency PPP-RTK paper (Eqs. 1–9 read in full)
plus established knowledge of Odijk et al. (2016, *J. Geod.* 90:15–44) and Teunissen & Khodabandeh
(2015, *J. Geod.* 89:217–240); every load-bearing code-claim adversarially re-verified against source.
All structural claims below were confirmed; one hypothesized gap (SSR yaw fields unconsumed) was
upgraded from "unverified" to "confirmed."

---

## 1. Verdict

The **user side is a faithful, S-basis-*invariant* realization of Odijk PPP-RTK — realized through
differencing operators and a frozen-pivot covariance pin, not through Odijk's explicit
minimum-constraint / estimable-bias parametrization.** The estimable functions the filter carries
(geometry, ZWD, slant-iono-lumped-with-DCB, DD-integer ambiguities) are the S-basis-invariant
observables of the 2016 model, related to it by a one-to-one S-transformation. The integers MLAMBDA
recovers are the same double-differenced integers `N_{1r}^{ps}` of Odijk Eq. 9. Conformance is high,
and the architecture is correct for a user engine.

**The one structural divergence that matters:** the code never uses an explicit S-basis. Every Odijk
rank deficiency is disposed of by *differencing it away*, *importing it as a product correction*, or
*absorbing it into the float ambiguity* — never by an explicit minimum constraint. This is a
legitimate *implicit* S-basis (differencing **is** an S-basis choice). But it means **no object in the
code declares the convention it assumes** — which IF code-bias pair sits in the clock, which ambiguity
datum sits in the phase bias, which pivot. Sufficient for a *consumer*; the entire problem for a
*producer* (the network), which must declare a convention the consumer can reproduce.

---

## 2. Theory recap (the bookkeeping that matters)

The undifferenced, uncombined code/phase equations are column-rank-deficient. The estimable quantities
are only certain *lumps*; a full-rank system requires a **minimum-constraint S-basis** (one constraint
per deficiency dimension). The deficiencies:

| # | Source | Consequence on estimable parameters |
|---|---|---|
| (a) | Clock / timing datum (rcv↔sat clock) | hold one clock; estimable "clock" absorbs IF code-bias lump |
| (b) | Code biases ↔ clock (IF part) and ↔ iono (GF/DCB part) | code biases wholly non-estimable; IF→clock, DCB→iono |
| (c) | Phase biases ↔ ambiguities (per freq) | only DD ambiguities survive as integers; pivot ambiguities hide in estimable phase biases |
| (d) | Iono ↔ DCB | estimable iono = iono + DCB lump (Eq. 3) |
| (e) | Ambiguity ↔ phase bias (integer survivor) | DD integer `N_{1r}^{ps}` is the survivor |
| (f) | ISB datum (multi-GNSS) | one inter-system bias fixed per extra constellation |

Network-estimable products (Odijk Eq. 2/4): estimable **satellite clock** = true clock + IF satellite
code bias (+ regional ZTD lump); estimable **satellite phase bias** = true phase bias + a code-bias
lump + **the network pivot-receiver's non-integer ambiguity `M_{1,1}^{ps}`**. That hidden ambiguity is
exactly what makes the user's ambiguities come out as *double differences* (Eq. 9) even though the user
never sees the network pivot receiver.

**Compatibility (the crux, Teunissen–Khodabandeh 2015):** corrections are **S-basis-covariant**. The
user recovers integers *only if* it adopts an S-basis compatible with the network's — same satellite
pivot, same IF code-bias convention in the clock, same ambiguity datum and wind-up model in the phase
bias, same sign. The various PPP-RTK mechanizations (clock+phase-bias, IRC, decoupled-clock, FCB/UPD)
are all *different S-basis choices of the same model*, inter-transformable, identical in estimable
content.

**Three user models** (increasing iono constraint strength → faster AR, greater risk): iono-**float**
(slant iono free), iono-**weighted** (network iono as soft pseudo-obs with finite σ — the regional
workhorse), iono-**fixed** (σ→0, hard hold; dense-network / short-baseline only).

---

## 3. Per-rank-deficiency conformance table

How GnssProcessor disposes of each deficiency. The three mechanisms are *differenced*, *imported*,
*absorbed-into-ambiguity*. **No row uses an explicit minimum-constraint equation** — that is the
defining structural fact.

| Odijk deficiency | Code mechanism | Anchor |
|---|---|---|
| **(a) Clock datum** | Receiver clock estimated (PPP) or **differenced away** (BSSD zeroes the column); satellite clock **imported** as IGS IF clock | `BssdTransform.cpp:221`, `PppModel.cpp:1367` |
| **(b) Code-bias IF↔clock** | Satellite side absorbed into imported IF clock; receiver side estimated as IFB/DCB states in the *same IF datum* via GF-IF coefficients (`μ1/dmu`, `μ2/dmu`) | `StateInitializer.cpp:103` |
| **(c) Phase-bias↔ambiguity** | **No phase-bias state anywhere.** Satellite phase bias **imported** (SSR/BIA); receiver phase bias **differenced away**; uncovered → absorbed into float ambiguity + flagged `markAmbiguityUnresolvable` | `MeasurementModel.cpp:1160`, `StateManager.cpp:1103` |
| **(d) Iono↔DCB** | Estimated per-sat L1-referenced slant iono = iono + DCB lump (Eq. 3); pivot iono column zeroed in SD | `MeasurementModel.cpp:1098`, `BssdTransform.cpp:236` |
| **(e) Ambiguity↔phase-bias** | DD integers fixed by MLAMBDA; **ZD** state per-(sat,signal), AR differences SD→DD internally (`buildDdPairs`, elevation pivot); baseline holds explicit DD states | `AmbiguityResolver.cpp:386`, `:561`; `StateKeySchema.hpp:94`, `:256` |
| **(f) ISB datum** | GPS hardcoded inter-system reference; ISB estimated per non-GPS system, differenced away in same-system SD | `StateManager.cpp:54`, `StateKeySchema.hpp:451` |

---

## 4. The implicit S-basis the code actually adopts

**PPP-RTK (BSSD on):**

- **Receiver datum** — fixed by between-satellite SD: receiver clock, phase clock, code biases
  (DCB/IFB), ISB all driven out by the differencing operator (`BssdTransform.cpp:221–235`). Stronger
  than Odijk's "nominate receiver 1": SD eliminates the *whole* receiver-side block per constellation.
- **Satellite datum** — the **per-constellation pivot satellite** (`PivotManager`, GLONASS excluded).
  Pivot ambiguity/iono/sat-bias are the held quantities. Realized as a **hard pin**: covariance→0 via
  `freezePivotAmbiguityCovariance` / `freezePivotIonoAndBiasCovariance` (`PivotManager.cpp:849`, `:881`),
  value→0 via BSSD column-zeroing (`BssdTransform.cpp:236–251`). *Two places realize "value 0,
  variance 0," not one.*
- **Clock/bias convention datum** — *imported, not chosen by the code*: satellite clock = IGS
  ionosphere-free code-bias-absorbed clock referenced to the BIA `SATELLITE_CLOCK_REFERENCE_OBSERVABLES`
  pair (`ProductParser.cpp:1003`); satellite code biases = per-signal OSB made IF-clock-consistent on
  the receiver side via GF-IF coefficients; satellite phase biases = per-signal OSB from SSR. **The
  code adopts whatever S-basis the product files were generated under** and re-expresses its receiver
  states to match (the GF-IF coefficient choice is the only active alignment).
- **Inter-system datum** — GPS, hardcoded.

**Baseline (DD):** satellite datum = per-constellation pivot (`DdPivotManager`, SNR-ranked); receiver
datum = fixed reference station. DD removes *both* satellite-side product error and receiver-side
terms, so the baseline solution is **product-convention-insensitive** — the robust fallback whose
correctness does not depend on the §6 compatibility contract.

**Versus Odijk's explicit S-basis:** Odijk fixes one parameter per deficiency and *retains the
estimable biases as named parameters you can print, broadcast, and audit*. GnssProcessor fixes the
same dimensions but **discards** the satellite-side estimable biases into imported corrections and
**discards** the receiver-side estimable biases into the differencing operator. Estimable observables
are identical; the difference is that **Odijk's parametrization is invertible to the conventions, and
the code's is not.**

---

## 5. Status of the three cited S-system benefits

### 5.1 Hardware-bias network/user compatibility — present, but enforced only *negatively*

The one place the S-basis contract is *actively enforced* is GF-IF alignment (`StateInitializer.cpp:103`):
the clock-reference observable pair is read from the BIA DESCRIPTION (`ProductParser.cpp:1003`) and the
receiver code-bias states are aligned to that IF datum. Correct and S-system-aware.

Everything else is enforced *negatively*: a wrong pivot/datum/sign/wind-up convention degrades to
**failed AR validation → no fix** (ratio + bootstrap success-rate ≥0.999 + per-DD residual cap,
`AmbiguityResolver.cpp:642`), never a silent wrong fix. Safe, but there is **no positive cross-check**
that the SSR phase-bias product's S-basis matches the user's pivot choice, and the sign conventions
(`kSsrPhaseBiasSign = +1.0`, `kSsrCodeBiasSign = +1.0`, `MeasurementModel.cpp:1157`) are empirically
pinned to one provider.

### 5.2 Atmospheric constraints — float + weighted present; one critical real-time gap

- **Iono-float** — fully present (per-sat L1-referenced slant iono state, RW Q mapped to slant by
  `1/sin²el`, `MeasurementModel.cpp:1098`).
- **Iono-weighted** — present, **GIM-only**. `applyGimIonoUpdate` injects the IONEX value as a per-sat
  soft pseudo-obs; `R` is per-constellation `diag(var) + σ_c²·11ᵀ`, the all-ones block leaving the
  per-constellation common mode (iono datum) free (`Application.cpp:552`). Config block
  `estimation.ionosphere_weighting.*` (`GnssConfigLoad.cpp:934`); σ→0/σ→∞ sweeps the full
  fixed↔weighted↔float continuum.
- **Iono-fixed** — absent as a first-class model (emulate via GIM-weighting σ→0; state remains estimated).
- **GAP (highest-value for real-time):** **no SSR-STEC path.** `SsrCorrectionStore` carries
  orbit/clock/bias only; the SSR-to-STEC adapter is explicitly unbuilt (`RealTimeEngine.cpp:176`).
  Odijk's *workhorse* weighted-iono model — the thing that enables fast/instantaneous AR — is GIM-only
  in post-processing and **entirely absent in real-time**, directly capping real-time time-to-fix.
- **Troposphere** — ZWD RW + GMF/VMF3 + optional Chen-Herring gradients + a soft ZWD constraint
  anchoring the BSSD [up,ZWD] null space (`GnssConfigLoad.cpp:893`). Correct.

### 5.3 Integer AR — S-system-correct, but production is WL-only

The resolved quantity is the genuine DD integer, *provided* the satellite phase bias is removed as a
known correction — exactly Odijk §3.2:

- Receiver phase bias removed by **BSSD** (rows that can't pair to a pivot are rejected precisely
  because "the Kalman filter would then absorb the receiver clock and biases," `BssdTransform.cpp:106`).
- Satellite phase bias removed by the **known SSR correction** (`MeasurementModel.cpp:1160`), or, where
  uncovered, the ambiguity is `markAmbiguityUnresolvable` and excluded from LAMBDA — the code correctly
  **refuses to integer-fix a bias-contaminated ambiguity** (`MeasurementModel.cpp:650`, `:669`).
- GLONASS correctly excluded (FDMA inter-channel bias doesn't cancel in SD).

**Caveats:**

1. **Production AR is WL-only.** The shipped configs set `"wl_only": true`
   (`config/ppp_rtk_internal_AR.json:69`, `config/realtime_ppp_rtk_AR.json:123`) — MW-derived WL
   integers applied as Kalman pseudo-obs, **NL LAMBDA never searched**. The full WL+NL cascade and the
   per-band DD LAMBDA exist but are unexercised by live configs. "Integer AR" today = WL-fixed,
   float-NL — be precise about this in convergence/accuracy claims.
2. Integer property is decomposed in the uncombined L1/L2 domain (MW-WL + LAMBDA-NL), **not** via IF.
   IF combination is float-only, AR-excluded (`integer_capable=false`, `MeasurementModel.cpp:1922`). No
   Standard-PPP IF→(WL,NL) integer path exists yet → the satellite-pivot/SSR-phase-bias machinery is
   the *only* route to integers.

---

## 6. Network/user compatibility audit — what AR silently trusts

The contract the code depends on but does not enforce. For AR to be integer-valid the consumed
satellite phase bias must (i) be generated under the same per-satellite pivot ambiguity datum the user
differences against, (ii) use a wind-up model the user reproduces, (iii) match the IF clock's code-bias
convention, (iv) match the assumed sign. Status:

| Requirement | Enforcement |
|---|---|
| (iii) IF code-bias convention | **Active** — GF-IF alignment to the BIA reference pair (`StateInitializer.cpp:103`). *Risk:* if clock product and BIA OSB come from different providers with different IF reference pairs, alignment uses the BIA pair; no cross-check of clock-product provenance. |
| (i) Phase-bias ambiguity datum / pivot | **Negative only** — mismatch → failed validation → no fix. No positive check. |
| (ii) Wind-up / yaw model | **CONFIRMED GAP** — SSR `yaw_angle_deg`/`yaw_rate_deg_per_s` are *decoded* (`RtcmSsrDecoder.cpp:333`) but **read nowhere**; `computePhaseWindUp` derives its own yaw-steering attitude from sun/velocity geometry (`GnssCorrections.cpp:1426`). Consistency rests on both sides independently computing the same yaw — a known **eclipse/noon-maneuver AR hazard** and an S-basis-adjacent convention a network must eventually pin. |
| (iv) Sign | **Empirically pinned** to one provider (`kSsr*BiasSign = +1.0`). Onboarding a second SSR provider is a live hazard. |
| Discontinuity reset | **Active** — `noteSsrDiscontinuity` forces `resetAmbiguity` on a bias-jump (`RealTimeEngine.cpp:1082`). |

Net: a convention mismatch degrades to *no fix*, never a *wrong fix* — acceptable, but the engine is
**not self-describing** about the S-basis it assumes.

---

## 7. Implications for the NETWORK side (the project goal)

Goal in S-system terms: estimate, from multiple stations, the estimable **satellite clock** (Eq. 2)
and estimable **satellite phase bias** (Eq. 4, carrying the pivot-receiver ambiguity lump) under a
**declared S-basis**, and broadcast them so a user reproducing that S-basis recovers integers. Orbits
from the IGS stream are sufficient.

**Assets the current architecture already provides (genuine reuse, not a rewrite):**

1. The measurement model is **undifferenced + uncombined at its base** (`buildJacobianRow`), with
   differencing applied as a *post-assembly* S-transform (`BssdTransform::apply`). A network estimator
   runs on exactly this substrate — it just must *not* apply the receiver-side differencing and instead
   estimate the satellite-side lumps on the undifferenced rows.
2. `StateManager`'s dynamic key→index blocks already support per-(sat,signal) bias states
   (`makeSatBiasKey`, `sat_bias_indices_`). The *fallback* satellite code-bias state (fires only on
   missing product coverage, `MeasurementModel.cpp:1128`) is structurally the network's
   satellite-code-bias parameter — promote it from fallback to first-class.
3. The GF-IF decomposition (`StateInitializer.cpp:103`) is the same IF/DCB algebra the network clock
   product must carry on the satellite side — share one bias algebra across network and user.

**What is genuinely missing (the real work — additive, a new path alongside the user path):**

1. **A multi-station joint estimator.** `BaselineMulti` is a *star topology* — N rovers each
   independently DD'd against one fixed reference, **no inter-rover correlation, no joint
   satellite-parameter estimation** (`GnssConfigTypes.hpp:84`). There is no normal-matrix-level
   coupling across stations and no shared satellite-side unknowns. This is the largest gap. *Structural
   reason it's required:* a single-receiver BSSD pipeline **cannot** estimate satellite phase biases —
   between-satellite SD on one receiver removes the receiver datum but leaves the satellite phase bias
   confounded with that station's pivot ambiguity. You need multiple stations (so the satellite-indexed
   bias is observed across receivers) and you must stop differencing the receiver side away (or
   difference between *receivers*, keeping satellites).
2. **An explicit satellite-*phase*-bias state** — exists nowhere today. New key in `StateKeySchema`,
   new block in `StateManager` mirroring `sat_bias_indices_`.
3. **An explicit minimum-constraint S-basis** — the one S-system mechanism the codebase has *never*
   used. Pick reference receiver, pivot satellite per constellation, clock datum, ambiguity datum, ISB
   datum; record them in the product so users reproduce them.

**Concrete path:**

- Build the network estimator on the existing undifferenced+uncombined `buildJacobianRow`, but with
  satellite-side lumps as **explicit estimable states** and an **explicit minimum-constraint S-basis** —
  i.e. for the network, deliberately do the thing the user side avoids. Add satellite phase-bias states,
  promote the satellite code-bias state, add per-station receiver clock/bias/ambiguity states across a
  *stacked multi-station* normal matrix.
- Apply Odijk's explicit S-basis as minimum constraints (fix reference-receiver clock/biases, fix
  pivot-satellite-and-reference-receiver ambiguities, fix the ISB datum). Doing this on the network
  side does **not** perturb the (correct, differencing-based) user side.
- **Adopt the IGS-SSR clock+phase-bias S-basis as the broadcast convention** so products are drop-in
  for the existing user consumer. The user side already applies SSR phase bias and resets on
  `discontinuity_counter`; if the network emits in that same convention/datum, the existing
  `SsrCorrectionStore` path is the validation harness. **Strongest leverage:** the network's
  correctness criterion becomes operational and closed-loop — *"does our own user engine recover
  integers from our own products on an independent station?"*
- Reuse the GF-IF decomposition for the satellite-side IF/DCB split so network and user share one
  algebra. `PivotManager` is reusable as the network's satellite-datum selector, **but** the network
  needs a *single global* pivot per constellation, stable over the broadcast interval — not the
  per-receiver sticky-hysteresis pivot the user uses. The handover S-transform
  (`applyCovarianceReReference`) is the right tool but must be applied network-wide and reflected in the
  product datum.

---

## 8. Honest divergences / risks

1. **No explicit S-basis / minimum-constraint object** — legitimate for a user engine, but blocks
   self-description and is the structural prerequisite for §7.
2. **Phase-bias S-basis compatibility enforced only negatively** — no positive cross-check; sign pinned
   to one provider. Second-provider onboarding is a live hazard.
3. **SSR yaw fields decoded but unused** (`RtcmSsrDecoder.cpp:333` → read nowhere; `computePhaseWindUp`
   computes attitude independently). Eclipse-season AR hazard; wind-up is part of the phase datum a
   network must pin.
4. **Production AR is WL-only** (`wl_only:true`) — "integer AR" today is WL-fixed, not full-integer.
5. **No SSR-STEC iono-weighting path in real-time** — Odijk's fast-AR workhorse is GIM-only / post-proc.
6. **GPS-hardcoded ISB datum + GLONASS exclusion** — theory-faithful for a user engine; a multi-GNSS
   *network* product must decide and *declare* ISB/IFB conventions per constellation explicitly.
7. **Frozen-pivot hard-zero pin vs. soft constraint** — equivalent in exact arithmetic; in finite
   precision the handover S-transform `P ← T·P·Tᵀ` is where datum-transfer error could accumulate. DD
   handover has PSD/symmetrize guards (`DdPivotManager.cpp:208`); confirm the PPP-RTK
   `applyCovarianceReReference` path has equivalent conditioning protection before introducing
   network-scale handovers.

---

## 9. Incremental backlog (address later)

Ordered to serve the network-PPP-RTK + real-time goals; each item is independently landable.

**P0 — unblock real-time PPP-RTK quality (user side, near-term):**

- [ ] **SSR-STEC iono-weighting path.** Add a STEC/iono push to `SsrCorrectionStore` and a real-time
      adapter feeding the existing weighted-iono pseudo-obs mechanism (`applyGimIonoUpdate` is the
      post-proc template). Highest-value real-time atmospheric gap; directly improves time-to-fix.
      (§5.2)
- [ ] **Decide on WL+NL in production.** Either flip the live configs off `wl_only` and validate the NL
      LAMBDA cascade, or document explicitly that production AR is WL-only by design. (§5.3)

**P1 — S-basis hardening (makes the user side self-describing, prerequisite for network):**

- [ ] **Consume SSR yaw fields in `computePhaseWindUp`**, or document that independent yaw modeling is
      the intended contract and gate AR through eclipse seasons accordingly. (§6, §8.3)
- [ ] **Positive S-basis cross-check** at product ingestion: verify the phase-bias product's pivot /
      ambiguity-datum / sign convention against the engine's assumptions; warn loudly on mismatch
      instead of relying on silent AR-validation failure. (§6)
- [ ] **Cross-check clock-product provenance against the BIA IF reference pair** to catch mixed-provider
      IF-datum inconsistencies. (§6 (iii))

**P2 — network side (the new construction):**

- [ ] **Explicit satellite phase-bias state** — new `StateKeySchema` key + `StateManager` block
      mirroring `sat_bias_indices_`; promote the fallback satellite code-bias state to first-class. (§7.2)
- [ ] **Multi-station joint estimator** — replace the `BaselineMulti` star topology with a stacked
      multi-station normal matrix sharing satellite-side unknowns; estimate on undifferenced+uncombined
      rows without receiver-side differencing. (§7.1)
- [ ] **Explicit minimum-constraint S-basis machinery** — reference receiver, per-constellation global
      pivot satellite, clock datum, ambiguity datum, ISB datum; *record* the S-basis in the emitted
      product. (§7.3)
- [ ] **Product emit path** in IGS-SSR clock+phase-bias convention; wire the existing user consumer as a
      closed-loop integer-recovery test on a held-out station. (§7)
- [ ] **Network-scale pivot handover** — global per-constellation pivot stable over the broadcast
      interval; confirm `applyCovarianceReReference` conditioning matches the DD path's PSD/symmetrize
      guards. (§7, §8.7)

---

## 10. Sources

- Odijk, Teunissen, Zhang (2011), "PPP-RTK platform performance based on single-frequency GPS data,"
  Curtin GNSS Research Centre — **primary source, Eqs. 1–9 read in full.**
- Odijk, Zhang, Khodabandeh, Odolinski, Teunissen (2016), *J. Geod.* 90(1):15–44,
  DOI 10.1007/s00190-015-0854-9 — canonical S-system estimability treatment (paywalled; framework
  reconstructed from established knowledge, cross-checked against the 2011 algebra).
- Teunissen & Khodabandeh (2015), "Review and principles of PPP-RTK methods," *J. Geod.* 89(3):217–240,
  DOI 10.1007/s00190-014-0771-3.
- Foundational S-system theory: Baarda (1973); Teunissen (1985), zero-order design / S-transformations.

*All code line numbers reflect the working tree as of 2026-06-15 and may drift; treat them as anchors,
not addresses. A handful were corrected against source during the verification pass (notably the
`markAmbiguityUnresolvable`/GLONASS exclusion lives around `MeasurementModel.cpp:186` (skip) and `:650`,
`:669` (mark), not `:629`).*
