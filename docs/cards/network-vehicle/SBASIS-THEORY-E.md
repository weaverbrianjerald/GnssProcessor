# S-basis theory: estimability, the datum-invariance theorem and the Eq-34 gate

**STATUS**

- **id:** S-basis theory
- **topic:** network-vehicle
- **status:** reference
- **summary:** Reference for the S-system canon: no admissible re-datuming moves an estimable quantity, so wire wander needs an estimation-side cure; the Eq-34 integer-recovery gate binds any CC-S-like phase gauge.
- **dates in the text:** 2026-06-15 → 2026-08-15
- **knobs / posture:** none
- **hashes named in the text** (commit / exe / byte-cert): none
- **run dirs named in the text:** none
- **source:** `docs/archive/SPEC_sbasis_realization_contract.md` §E0, §E1, §E2, §E3, §E4, §E5, §E6, §E7, §E8

**Outcome.** The card collects the S-system canon (Odijk-2016, TK-2015, Hou-Zhang-2025, Teunissen's adjustment theory) behind the producer's datum choices. Estimable functionals are invariant in value and variance under any admissible datum, so the differential clock (~47 cm) and phase-bias wander cannot be moved by re-datuming. It names four estimation-side levers (the block product covariance carry ranked first) and a ten-item correctness punch-list. The ensemble gauge's "strongly validates" reading is SUPERSEDED by measurement (the ensemble, s2z and broadcast-anchored datums all A/B'd dead); the theory and the Eq-34 gate stay binding. The realized basis is in [S-basis contract](../network-vehicle/SBASIS-CONTRACT.md).

> Results are appended TO THIS FILE, below the verbatim text. The archived monolith is frozen; never append there.

---
<!-- BEGIN VERBATIM: SPEC_sbasis_realization_contract.md -->
## §E0 The corpus and the notation

| Tag | Source | What it gives us |
|-----|--------|------------------|
| **Odijk-2016** | Odijk, Zhang, Khodabandeh, Nadarajah, Teunissen (2016), *J Geod* 90:15–44, "On the estimability of parameters in undifferenced, uncombined GNSS network and PPP-RTK user models by means of 𝒮-system theory" (DOI 10.1007/s00190-015-0854-9 — paywalled; framework cross-checked against the 2011 algebra) | The exact estimability theory of **our** model: rank-deficiency enumeration, CC-R vs CC-S datums, S-transformations, user DD-integer, datum time-stability |
| **TK-2015** | Teunissen & Khodabandeh (2015), *J Geod* 89:217–240, "Review and principles of PPP-RTK methods" (DOI 10.1007/s00190-014-0771-3) | Six PPP-RTK mechanizations = **2 S-bases**; PPP-RTK is *relative* positioning; the UPD/FCB estimation-method **biasedness proof**; which part of the correction covariance the user actually uses |
| **Hou-Zhang-2025** | Hou & Zhang (2025), *J Geod* 99:3, "A generalized least-squares filter designed for GNSS data processing" | **TIME-domain** estimability — the rigorous replacement for the "engineering" large-variance init / fake dynamic models; two-transformation handover; NEQ reduction; flexible-window smoother |
| **AT-Teunissen** | Teunissen (2024, 2nd ed), *Adjustment theory: an introduction*, TU Delft OPEN | Foundations: BLUE/estimability, A-model/B-model duality, the datum model `E{y}=Ax, Bᵀx=0`, recursive/phased (Kalman) estimation, Lagrange/KKT minimum-constraint machinery |

Notation: `~` = estimable (S-basis-reduced) combination; `dt^s` satellite clock; `d^s_j`/`δ^s_j`
satellite code/phase bias (freq `j`); `z^s_{r,j}` integer ambiguity; `ι^s_r` slant iono;
`μ_j = λ_j²/λ_1²`; `μ_IF`, `μ_GF` the IF / GF combination row-vectors of `[μ,e]⁻¹`. `f` = #freq,
`n` = #receivers, `m` = #satellites, `k` = #epochs.

## §E1 The estimability spine, and the datum-invariance theorem

**Estimability condition** (AT Ch.1–2). A linear functional `θ = fᵀx` has a linear *unbiased*
estimator **iff `f ∈ range(Aᵀ)`**. In the undifferenced/uncombined model `A` is
column-rank-deficient, so raw per-satellite clocks / phase biases / code biases are **not**
estimable — only certain *combinations* are. "The satellite clock" does not exist as an estimable
number; only a datum-dependent lump does.

**Rank-deficient decomposition** (Odijk-2016 §2). `E{y}=Ax`, `rank A = r < n`, null space `AV = 0`.
Split `x = S·α̃ + V·β`: `α̃` = the `r` estimable functions, `β` = the `n−r` **inestimable**
directions = the minimum-constraint (datum) vector. The **S-transformation**

```
S = I − V [ (S⊥)ᵀ V ]⁻¹ (S⊥)ᵀ ,      S·V = 0,  S·S = S
```

is the idempotent oblique projector onto the estimable space along the null space; **its ROWS are
the estimable linear combinations** — literally what `NetworkSbasis` constructs. `S⊥` encodes which
directions the datum pins.

**Cross-datum consistency** (Odijk-2016 Eqs 9–11). For two datums `S, S'`: `S·S' = S`, `S'·S = S'`,
so `S'·x̂_S = x̂'_S` and `Q' = S'·Q·S'ᵀ`. You can convert any solution **and its covariance** from
one datum to another **without knowing which datum it came from** — the correct machinery for
comparing/converting products across our three clock datums (§A row 1a).

**The datum model** (AT Ch.5 §5.3) — the abstract skeleton of `NetworkSbasis`. Datum = constraint
`Bᵀx = 0`. Two-step solve:

```
Step 1 (ignore datum):  x̂_A = Q_xA Aᵀ Q_y⁻¹ y ,   Q_xA = (Aᵀ Q_y⁻¹ A)⁻¹
Step 2 (impose datum):  x̂   = [ I − Q_xA B (Bᵀ Q_xA B)⁻¹ Bᵀ ] x̂_A   ← the S-transform projector
```

Covariance becomes a **rank-b downdate** `Q_x̂ = Q_xA − Q_xA B(Bᵀ Q_xA B)⁻¹ Bᵀ Q_xA`, which is
**SINGULAR** (`Bᵀ Q_x̂ = 0`): the datum directions carry **zero variance** — conventions, not
measurements. For a single sum-to-zero gauge (`b=1`, `B=g`) this is exactly the rank-1 fold
`h hᵀ/β` with `h = Q_xA g`, `β = gᵀ Q_xA g` — the `g gᵀ/β` Schur fold already in the phase-ensemble
kernel. **⚠ The metric is `Q_xA`, NOT Euclidean** — even a plain `Σ = 0` constraint redistributes
covariance-weighted, not by arithmetic mean.

> ### 🔑 THE DATUM-INVARIANCE THEOREM — still in force
> Any estimable functional `fᵀx` (`f ∈ range(Aᵀ)`) is invariant in **value AND variance** under any
> admissible datum. Corollary: *the differential per-sat clock (~47 cm) and phase-bias (6–69 cm)
> wander are estimable quantities, hence provably unchanged by re-datuming.* No choice of
> clock/phase S-basis (s2z / bca / signal-hold / prioritized-anchor / ensemble) can move them. This
> is the first-principles proof of why every datum A/B came out NEGATIVE and why the "null-space
> leak" was correctly falsified. **The cure must alter ESTIMATION (weights / dynamics / model /
> transmitted covariance), not the gauge.** It is also the theory backing for
> `SPEC_pivot_visibility_gauge_defect.md` §4.1's "raw wire clock spread is not a verdict".

## §E2 The rank deficiencies, and what is estimable before any datum

Undifferenced/uncombined phase + code (Odijk Eq 12), per (r, s, j, epoch i):

```
E{Δφ^s_{r,j}} = g^sᵀ Δx_r + dt_r − dt^s + λ_j(δ_{r,j} − δ^s_j) − μ_j ι^s_r + λ_j z^s_{r,j}
E{Δp^s_{r,j}} = g^sᵀ Δx_r + dt_r − dt^s +   (d_{r,j} − d^s_j)  + μ_j ι^s_r
```

Ambiguities carry **no time index** (constant absent slips). Iono is either **vertical**
single-layer `ι^s_r = f^s_r · ι^s` (which makes DCBs estimable) or **slant** per-(r,s).

| Type | Coupling | Size |
|------|----------|------|
| 1a | receiver ↔ satellite clock | 1 |
| 1b | receiver ↔ satellite hardware (phase+code) bias | 2f |
| 2a | receiver clock ↔ receiver hardware bias | n−1 |
| 3a | satellite clock ↔ satellite hardware bias | m |
| 4 | receiver phase bias ↔ ambiguity | f(n−1) |
| 5 | satellite phase bias ↔ ambiguity | f·m |

**The exact count (Odijk Eq 24)** — what `declared_dimension` + the null-space gate implement at
runtime (§A rank bookkeeping):

```
# independent S-basis constraints  =  1 + 2f + (1+f)(n−1+m)               (Eq 24)
                    + (n−1+m)   if SLANT iono  (types 2b, 3b)
                    + ν         if REGIONAL (<~500 km, parallel LOS; type 0)
                    × (per-epoch multiples) for any parameter group WITHOUT a temporal constraint
```

**Datum-invariant even before choosing a datum:** the GEO row and the (vertical) ION row of the
null-space basis `V_net` are **ZERO** ⇒ **position/ZTD and vertical iono are estimable in absolute,
uncombined form**. Everything else (clocks, biases, ambiguities) is only a datum-dependent
combination. ⇒ **the consumer vertical error CANNOT be a position-datum artifact**; it must enter
through the biased clock/phase corrections.

**Code-bias / iono coupling** (Eqs 29–33, Table 8): on two frequencies only **one** code-bias DOF —
the geometry-free `d^s_GF` (a scaled DCB) — is estimable; the second band is a fixed multiple
(`d̃^s_j = μ_j·d^s_GF`, `j=1,2`). Modernized per-band DCBs (`j ≥ 3`) are estimable **only under
vertical iono**; under **slant** iono the GF code biases lump into the slant iono
(`ι̃^s_r = ι^s_r + d_{r,GF} − d^s_GF`) and dual-freq code bias is inestimable. This is the
estimability root of "SH-iono forces sat-code-bias, GLONASS excluded", and §A row 2b/3b is the
realization of exactly this row as a datum.

## §E3 CC-R vs CC-S, and where our features sit

- **CC-R** ("Common Clocks, pivot Receiver") — pin one receiver (clock, hardware biases, IF code
  biases, pivot-sat & pivot-rcv ambiguities). Estimable ambiguities are **exact integer double
  differences** ⇒ clean user AR. The reference jumps only when the pivot receiver changes (rare) ⇒
  **TIME-STABLE**. The *integer-ambiguity S-basis* `(Δx₁, d^ps, z₁^ps)` behind CC-1/DC/IRC/DSC.
- **CC-S** ("Common Clocks, mean Satellite") — pin the satellite **mean** via
  `Z_m = I_m − (1/m) e_m e_mᵀ`. Matches the IGS zero-mean-DCB convention. **But the reference set is
  TIME-VARYING** — the datum is silently redefined whenever a satellite rises or sets, so estimable
  clocks/DCBs **jump at every membership change**; Odijk names **Zhong et al. 2015**'s IGS DCB
  long-term variation as exactly this artifact. Ambiguities are **not directly integer** (they need
  the Eq-34 recovery, §E6). The *phase-bias S-basis* `(Δx₁, d^ps, δ^ps)` behind CC-2/UPD-FCB.

| GnssProcessor feature | Canon S-basis | Time-stability verdict |
|---|---|---|
| `clock_datum_sum_to_zero` (per-const `Σ dt^s = 0`) | **CC-S** / satellite-mean gauge | ⚠️ set-dependent — steps at every constellation-membership change; *theoretically the worst for time-stability* |
| `clock_datum_broadcast_anchored` (per-sat brdc anchors) | **inhomogeneous pin** `BᵀE{y} = b⁰` (external, dim-0, covariance-neutral) | ✅ set-**INVARIANT** — closest in spirit to CC-R's "jumps only on a rare reference change" |
| `clock_signal_basis_hold` (hold the IF-pin pair in time) | **fixed-clock-convention** — freezes `μ_IFᵀ d^s` across epochs | ✅ pins the code↔clock split in time |
| prioritized per-(sat,signal) **anchor pin** (phase bias) | **CC-R-like** integer-ambiguity basis; the `−z^s_{1,j}` inside `δ̃^s_j` | ⚠️ single point of failure; anchor handover steps the bias by whole cycles (defect-3 churn) |
| **ensemble** `Σ_r z^s_{r,σ} = 0` (phase bias) | **CC-S-like** phase-bias basis / **min-variance inner constraint** | ✅ anchor-free, handover-free — BUT needs the Eq-34 integer-recovery guarantee (§E6). **See the SUPERSEDED note in §E6.** |
| between-sat SD **pivot + handover** (`PivotManager`) | a choice of condition-basis `B` (`BᵀA = 0`) | handover = pure S-transform; any discontinuity beyond it is a code bug (defect 3) |

**Theory ranking for time-stability of the transmitted product:**
`broadcast_anchored ≳ signal_basis_hold > sum_to_zero`. A **product-domain** ranking; per the
datum-invariance theorem it does **not** change the consumer's estimable position.

## §E4 What the network transmits, and the correction covariance

- **Estimable satellite clock** (CC-R): `dt̃^s = [dt^s + d^s_IF] − [dt₁ + d_{1,IF}]` — the
  **ionosphere-free clock** (clock + IF code bias), referenced to an epoch-1 datum. Exactly the IGS
  IF-clock convention (cf. §B record C). Its **differential** (per-sat, non-common-mode) time drift
  is the consumer-unabsorbable part; the common-mode part is absorbed by the user receiver clock.
- **Estimable satellite phase bias** (CC-R): `δ̃^s_j = (fractional bias) − z^s_{1,j}` — it literally
  **carries an integer** (the pivot-receiver ambiguity). *That is the mechanism by which network
  phase biases give the user its integer property.* Re-anchoring shifts it by whole cycles ⇒
  defect-3 churn.
- **User ambiguity** = automatic **integer double difference**
  `z̃^s_{u,j} = [z^s_{u,j} − z^p_{u,j}] − [z^s_{1,j} − z^p_{1,j}]`. The **only** surviving
  network-datum footprint the user inherits is `[z^s_{1,j} − z^p_{1,j}]`; the user never needs to
  know the network pivot. ⇒ **PPP-RTK is RELATIVE positioning**, and an `n = 1` network collapses
  exactly to a single-baseline DD (TK-2015 §4.3, Eqs 69–72).
- **Iono must be transmitted UNDIFFERENCED per-satellite** (Table 11) — a vertical→slant mapping
  intervenes at the user, so between-satellite differencing it is inconsistent. (⚠ §B records that
  our wire carries **no** ionosphere record at all — the consumer receives half the pair.)
- **Integer preservation requires** (a) satellite phase biases are provided (else standard-PPP,
  non-integer and time-dependent, Eq 43); **AND** (b) network/user iono parameterizations are
  consistent, or DCBs are supplied — else DCBs lump into the user ambiguity and destroy integerness
  (Eqs 44–45).
- **The correction covariance the user actually uses** (TK-2015 §5.3, Eq 88): only the
  block-diagonal `diag(C_φ, C_p) ⊗ C_s` (× `1/n` if ambiguity-fixed). The rank-one `[e;e]`
  (clock/geometry) and `[−μ;μ]` (iono) parts lie in the user's own design range and are absorbed.
  **⚠ Caveat: a SINGLE-EPOCH, unbiased-correction result** — it lets us simplify the emitted
  stochastic model but does **not** cover time drift (defect 2).

## §E5 The four theory-grounded levers — all ESTIMATION-side

Three independent canon results converge: the §E1 datum-invariance theorem; BLUE S-basis invariance
(TK-2015 §4.1 — a rigorous network adjustment with *any* PPP-RTK datum gives the *same* user
position, so differences imply non-rigorous estimation, not a wrong datum); and our own NEGATIVE
datum A/Bs (s2z / bca / signal-hold) plus the falsified null-space leak. ⇒ **stop hunting the
gauge.**

**Lever A — datum TIME-stability / rigorous time-linking (Hou-Zhang-2025).** Is the transmitted
estimable combination **rigorously time-linked** (`z_k = z_{k−1}`, true small/zero process noise) or
**re-floated every epoch** under a diffuse large-Q prior? `z_k = z_{k−1}` with **true zero process
noise is the CORRECT model for a constant estimable function**; option (b) is the "engineering"
misspecification Hou-Zhang condemn (Fig 12: decimetre bursts on every re-track under KF, clean
under GLSF), and the large-variance init for newly-tracked satellites is the same anti-pattern.
**⚠ Do NOT compute a new ambiguity's predicted value from the current epoch (RTKLIB style)** — the
paper proves that reuses observations and breaks BLU.

**Lever B — full/block product covariance carry (Tienstra estimation-in-phases).** AT Ch.6 §6.6:
the producer→consumer split is **exact only if** Phase 2 receives the **full** Phase-1 covariance
`Qŷ_B1 = P_(QyB1)⊥ Qy`. Our **DIAGONAL** carry drops the cross-correlation among satellite
clocks/phase-biases — a **provable sub-optimality**: the consumer cannot correctly weight the
*correlated* differential wander it is fed. A/B: emit a per-satellite (or per-constellation)
**block** covariance. Good news (Eqs 160–164): the consumer's RAIM statistic needs only misclosures
+ their covariances, not raw `Qy`, so the product stream stays self-sufficient. *(Emit-path seams:
`NetworkProductEmitter` / `NetworkProductIO`; the gap is the "diagonal carry" item of
`project_network_ssystem_dia_assessment`.)*

**Lever C — decoupled phase clock (DC / IRC / DSC).** TK-2015 §2.3, §3.1: a phase clock decoupled
from the code clock (`δ̄^ps_IF` distinct from `dt̄^ps`) is a **rigorous, S-basis-consistent**
reparametrization. **Correction to the standing note "no separate code/phase clock per Odijk" — it
should be SOFTENED**: a decoupled clock adds no *information* and won't by itself fix defect 2, but
it is a **legal** datum and a genuine lever worth prototyping. It is not forbidden.

**Lever D — minimum-variance inner constraint (AT App.C Ex.3).** `min lᵀQ_y l s.t. Aᵀl = f` ⇒
`l = Q_y⁻¹A(AᵀQ_y⁻¹A)⁻¹f`: among admissible datums the **covariance-weighted inner constraint has
LEAST variance**. A single hard anchor pin dumps all datum variance onto one parameter; an
ensemble/inner constraint spreads it. Action: weight an ensemble gauge by parameter covariance
rather than by an unweighted mean.

*(Consistency check: the "coverage/float50" lever that cut differential clock wander 47 → 14 cm is
estimation-side — it changes which ambiguities are floated, hence the effective model/redundancy —
exactly the class the theory says can work.)*

**The absorbed reference's own ranked deep-dive list, kept because the ranking is the content:**
(1) **Lever B, product covariance carry** — the freshest, most concrete first-principles suspect;
emit per-sat/per-const block covariance and A/B the consumer vertical (the seams and the
"diagonal carry" gap above); (2) **Lever A, time-linking audit** — instrument whether the
transmitted datum quantities are `z_k=z_{k−1}` time-linked or re-floated, and replace the
engineering large-variance init with a data-derived (Hou-Zhang) init; (3) **the phase-ensemble
Eq-34 integer-recovery unit test** (§E6) — **it must pass before the ensemble datum can be trusted
or committed**; (4) **the §E7 correctness punch-list** — the rank-count assertion and the `sin²α`
meter are cheap, always-on, and bear directly on the 305-station non-PD stations; (5) **Lever C,
decoupled phase clock** — prototype emitting `δ̄^ps_IF` alongside the common clock, and A/B it only
if Levers A/B do not close the gap.

## §E6 The MANDATORY Eq-34 gate for any CC-S-like phase gauge

> ### ⚠️ MANDATORY correctness gate — still binding
> A CC-S-like gauge (the `Σ_r z^s_{r,σ} = 0` ensemble is one) has ambiguities that are **NOT
> directly integer** (Odijk-2016). They need the recovery transform (**Eq 34**):
> `z̈^s_{r,j} + Σ_{s≥2} z̈^s_{r,j} = [z^s_{r,j} − z^1_{r,j}] − [z^s_{1,j} − z^1_{1,j}]` (the CC-R
> integer double difference). Any claim that such a gauge "keeps the user DD integer" **IS** Eq 34.
> **Unit-test it explicitly:** run an ensemble-datum network and a CC-R (prioritized-anchor) network
> on the same data and assert the reconstructed user DD ambiguities are **bit-identical integers**,
> especially **across a handover epoch**. That is the pass/fail for the whole approach, and it
> **must pass before the ensemble datum can be trusted / committed.**

> ### SUPERSEDED BY MEASUREMENT — the ensemble gauge's *validation verdict* only
> The absorbed reference concluded that "the canon **strongly validates** the `Σ_r z^s_{r,σ} = 0`
> ensemble gauge" (min-variance per Lever D, anchor-free, handover-free, one common ensemble
> reference per satellite). **That verdict is SUPERSEDED, not the theory.** The ensemble PB datum,
> clock s2z and broadcast-anchored were subsequently **all A/B'd dead** in the field
> (`project_network_falsified_approaches`; §8.2 of `SPEC_pivot_visibility_gauge_defect.md` records
> the same list) because common-mode is consumer-absorbable, and that card's §4 measures the
> related prioritized-anchor posture as **NET-HARMFUL** on its fixture. The theoretical argument
> and the Eq-34 gate above **remain valid and binding**; the "strongly validates" reading does not.

## §E7 The ten-item correctness punch-list

1. **Rank-deficiency count assertion** (Eq 24). Assert
   `#S-basis constraints applied == 1 + 2f + (1+f)(n−1+m)` (`+ (n−1+m)` slant, `+ ν` regional) for
   the current `(f,n,m)`. Under-constrained ⇒ near-singular normal matrix (candidate for the
   305-station em25 non-PD stations); over-constrained ⇒ biased products. Cheap, exact, wire it
   into the epoch-1 estimability trace. *(§A's `declared_dimension` + null-space gate is the
   realized form.)*
2. **`sin²α` per-parameter estimability meter** (AT Ch.6, Eq 35): `sin²α = diag(N_reduced)/diag(N)`
   — **O(1) per parameter**, explicitly recommended by Teunissen "to detect singularities/rank
   defects". An always-on lightweight complement to the gated SVD (`trace_estimability`).
3. **Orthogonality / clean-datum test** (AT Ch.6, Eq 26): a datum column is a *pure re-gauge* iff
   `A₁ᵀ Q_y⁻¹ A₂ = 0`. If not, it leaks into the precision of every non-orthogonal estimable
   function (Eq 28). A concrete test for "pure S-transform" vs "perturbs the solution".
4. **`n = 1` network → baseline-DD regression** (TK-2015 Eqs 69–72). Drive the PPP-RTK consumer
   from a single-station `NetworkEstimator` run; the solution must equal the baseline-DD engine to
   within the absorbed iono term. A cheap deterministic test isolating product-contract /
   datum-reconstruction bugs from estimation issues.
5. **NO float-fraction UPDs** (TK-2015 §6, **Lemma 7**). Averaging `frac(·)` of **float**
   ambiguities is **provably biased** and breaks user integerness for `n > 1`. Emit phase biases
   from **ambiguity-resolved** quantities (Eq 106: `Σ_j frac(ẑ_{j,W})/n + â_{1,W}`), or restrict to
   the safe cases (single station, or fixed `ǎ_r`). Audit that we never ship float-fraction biases.
6. **Emit the SINGULAR product covariance** (AT Ch.5). If we transmit product covariance it must be
   the S-transformed **singular** `Q` (annihilated by `Bᵀ`), not the unconstrained `Q_xA` — else we
   tell the consumer a pure convention is uncertain.
7. **Innovation-based RAIM** (AT Ch.6, Eq 73). The detection statistic is `Σ v_kᵀ Q_v(k)⁻¹ v_k` on
   the **INNOVATION** `v_k = y_k − A_k x̂_{k|k−1}`, *not* the post-fit residual. Verify the
   clock-monitor/RAIM uses the predicted residual.
8. **Gate per-band OSB on the iono parametrization** (Odijk Table 8). Under slant iono dual-freq
   code bias is *not* separately estimable (lumped into iono); only `j ≥ 3` modernized DCBs
   survive. Emitting independent dual-freq OSB under slant iono is an over-parameterization — the
   estimability explanation for the GLONASS/SH-iono coupling. *(§A row 2b/3b now realizes this as
   an explicit GF datum row.)*
9. **Iono emitted UNDIFFERENCED per-satellite** (Table 11) — never between-satellite differenced.
10. **Datum re-derived over the CURRENT satellite set** on any membership change, modelled as
    Hou-Zhang's **two-transformation** handover (`Fᵀ` back, `F̃ᵀ` forward), carrying only the
    intersecting time-linked functions — no fabricated priors, no silent re-datum step.

## §E8 The estimation foundation (AT Ch.6 + Hou-Zhang-2025)

- **Measurement update.** B-form (Kalman gain) `K = Q Aᵀ(Q + A Q Aᵀ)⁻¹`, or A-form (information)
  `Λ_k = Λ_{k−1} + A_kᵀ Q_k⁻¹ A_k`. Numerically identical; the A-form maps to
  `shared_carry_full_cov` / `addSharedPriorDense` on the border. **⚠ Static-x caveat:** Ch.6 derives
  *only* the measurement update — no state transition, no process noise — so datum **TIME-stability**
  (the essence of defect 2) is **not** in Ch.6; it needs the dynamic layer.
- **Schur / border reduction** (AT §6.2, §6.4 Helmert): the arrowhead reduction behind
  `NetworkNormalEquations`. Reduction and gauge-fold **COMMUTE** in the bordered structure — the
  theoretical warrant for folding a sum-zero gauge as a rank-1 update on the reduced system from
  cached LDLᵀ factors.
- **Generalized LS filter** (Hou-Zhang-2025), the dynamic layer: only chosen linear functions
  `z = Fᵀx` carry a dynamic model, everything else (receiver clock, per-epoch iono, new-sat
  ambiguities) is re-estimated each epoch with **no** initial value and **no** large-Q;
  **two-transformation handover** (`Fᵀ` back, `F̃ᵀ` forward) rigorously carries only the
  intersecting time-linked functions when the satellite set changes ⇒ formalizes pivot handover as
  a *time-datum* operation and predicts a clean handover eliminates defect-3 churn; **null-space
  elimination** `x = S z̃ + G u`, `F̃ᵀG = 0`, projecting out nuisance `u` via `P⊥_{A^u}` — the same
  projector algebra as a minimum-constraint S-transform, source of the **23–32 % NEQ reduction**
  (eliminate iono + clocks per epoch); **flexible-window recursive smoother**
  `J^{z̃} = P^{z̃}(Φ̃^z)ᵀ(P^z)⁻¹` — collapses convergence to ~1 epoch and bridges rise/set without
  hard segment boundaries, a near-real-time complement to our full-arc RTS smoother.

---

# §F User-side conformance audit, 2026-06-15

*Absorbed 2026-08-15 from `docs/ssystem_conformance_2026-06-15.md` (now `docs/attic/`).* **⚠ Scope:
this is the USER side** — BSSD consumer, MLAMBDA, real-time SSR — audited on 2026-06-15, **not** the
network producer of §A–§E. **Every code anchor below is a 2026-06-15 snapshot and drifts.**
**§F7 is the network's design charter**, cited from code as "§7". *(There is no §F6: the absorbed
doc's §6 compatibility-audit table is folded into §F5, where it belongs with the enforcement
discussion.)*

**Method (for weight, not repetition):** 7 code-recon passes over the measurement model, state
schema, pivot machinery, corrections/bias path, ambiguity resolver, atmosphere and config; every
load-bearing code claim **adversarially re-verified** against source. One hypothesised gap (SSR yaw
fields unconsumed) was upgraded to **CONFIRMED**. Correction made during that pass and preserved:
the `markAmbiguityUnresolvable` / GLONASS exclusion lives around `MeasurementModel.cpp:186` (skip)
and `:650`, `:669` (mark) — **not `:629`**.

Primary sources: Odijk, Teunissen, Zhang (2011), "PPP-RTK platform performance based on
single-frequency GPS data", Curtin GNSS Research Centre (Eqs 1–9 read in full); Odijk et al. (2016)
*J Geod* 90(1):15–44 (DOI 10.1007/s00190-015-0854-9, paywalled; framework reconstructed from
established knowledge and cross-checked against the 2011 algebra); Teunissen & Khodabandeh (2015)
*J Geod* 89(3):217–240 (DOI 10.1007/s00190-014-0771-3); Baarda (1973); Teunissen (1985) zero-order
design / S-transformations.

