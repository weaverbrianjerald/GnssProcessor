# SPEC — O2: the `carry_invariant`-lineage accumulator

**Status: DERIVATION ONLY, PANEL-REVIEWED — no code written, nothing built, nothing
run.  Verdict: DO NOT BUILD.  The accumulator this card was filed to design is
unreachable in two of the three subspaces it would act on, floor-limited in the
third by a quantity that has proved invariant to every intervention yet tried, and
already present as an armed channel that measured null.**

**The panel refuted four of the derivation's own supporting arguments.  They are
recorded as retractions in §8 rather than deleted — three of them were wrong in ways
that the record could repeat.**  What survives is a stronger negative result and one
corrected reading of a headline verdict.  ⚠ **§7 (the "common-cycle, not
ionospheric" lead) was RETRACTED the same day** — "equal in cycles" is 0.132° inside
`span{geo, iono}` and formally undetermined on dual-band field lineages; see
`SPEC_phase_windup_sign.md` §6.  Chasing it did, however, turn up a proven defect
in a core correction — an inverted carrier-phase wind-up sign — recorded in that
same card.

Owner: QUEUE item 8-family successor, filed by `SPEC_iono_carry_8b.md` §6.6 and
`QUEUE_principled_replacements.md` §8(b) as "the O2 / carry_invariant-lineage
accumulator (mid-arc transport NOT the leveling carry), needs its own derivation card
before ranking".  This is that card.  Session 2026-08-21.

---

## §1 The premise does not transfer

8(b)'s closure named the plateau as "the stationary equilibrium of the per-epoch
ZWD⇄iono⇄level exchange under mid-arc zero-q level lock-in", and filed O2 as the same
question on a posture whose transport is not the leveling carry.  **Both named
ingredients are absent on that posture:**

| ingredient | status under `carry_invariant` | anchor |
|---|---|---|
| zero-q leveling level transporting the LOS iono at phase precision | **structurally empty** — inject skipped, harvest starved | `ApplicationNetworkAssembly.cpp:2739-2741`; `NetworkRunContext.cpp:974,2405` |
| ZWD carry integrating the displaced state | **INERT** — it rides `carry_warm = pl_on && …` | `NetworkRunContext.cpp:2304`; `ApplicationNetwork.cpp:1440`; the code states it at `NetworkRunContext.cpp:2779-2781` |

The whole `_ft63_*` / `_ext63_*` family arms `carry_invariant` with `phase_leveling`
absent ⇒ `carry_warm` false for the entire run, so ZWD, receiver clock and receiver
phase bias are all re-estimated **white every epoch**.  O2 is therefore not "8(b) on
a different vehicle"; it is a different question, posed in §2–§4.

**And the accumulator it would install already exists.**  `sh_dev_carry` (mech-B) is
a per-(station,satellite) first-order Gauss–Markov ionospheric deviation carry,
`rep_error_tau_s = 900 s`, **armed on the entire pdfix lineage**.  P3 measured
removing it a complete null.  `SPEC_iono_carry_8b.md` §3.5 already forbids a second
one ("one temporal channel per physical column"), and the loader hard-disarms
`carry_iono` under `carry_invariant` (`GnssConfigLoadNetwork.cpp:217-224`).  The
2×2 the card would have asked for — **mech-B armed × q_z = 0 × PD-safe fold** — is
already run four times (`runs/pdfix_qz0`, `_deg15`, `_rep15`, `_iofloat`).

---

## §2 The three-way decomposition — two subspaces are gauge, not estimable

Split the per-LOS slant-iono error against the bipartite (receiver × satellite)
observation graph: `e_r^s = a_r + b^s + c_r^s` (station-common + satellite-common +
cycle space).

`[derived]` **`a_r` is an EXACT null direction of the data rows**, from the shipped
coefficients (`MeasurementModel.cpp:1456-1540`, `StateInitializer.cpp:153-173`):

| row family | absorber | why exact |
|---|---|---|
| code, reference bands | `<SYS>_DCB` | the DCB coefficient is `c_j = α_j/(α₁−α₂)` — **exactly proportional to the iono coefficient α_j**, so one scalar covers both bands |
| code, band ≥3 | `<SYS>_IFB<b>` | coefficient exactly 1.0 |
| phase, signal σ_j | RPB `δ_{r,sys,σ_j}` | coefficient exactly +1, at per-(station, system, **signal**) granularity — precisely the granularity of α_j |

Only the SH tie row's all-ones direction breaks it, deweighted by
`common_mode_sigma_m = 50 m` — by design
(`ApplicationNetworkAssembly.cpp:995-999`).  `b^s` is likewise absorbed by satellite
code and phase bias.

**MEASURED, on the realized graphs** (panel, reconstructing the spanning forest as
`arcs \ chords` from the blob's `A`/`H` rows): every fundamental cycle alternates
receiver/satellite — 341/341, 343/343, 349/349, 319/319 at four epochs, **0
violations** — so the vertex-potential cancellation `Σχ(α_r + β^s) = 0` is exact for
*any* additive per-receiver or per-satellite quantity.  That also kills, without a
run, the RINEX quarter-cycle worry (a per-(receiver,signal) constant is an α_r) and
the eq34 tree pins (a choice of α, β, not a bias).

⇒ **The part 8(b) measured as the plateau's carrier — the time-varying
station-common component — is a gauge quantity here.  No accumulator on the iono
column can reach it.**  Its memory is its absorbers' memory, which on this posture
is the rx code-bias carry: exactly what `SPEC_level_information_budget.md` §15
already found.  **That channel belongs to the level-authority card (QUEUE item 5 /
LV-6), not here.**

---

## §3 The cycle space — what the carry can reach, and the floor it hits

On a `carry_invariant` lineage **every phase row fits its own arc exactly** — each
phase row carries its own private ambiguity column and no other row touches it
(`MeasurementModel.cpp:1621-1630`), as the codebase itself states
(`GnssConfigLoadNetwork.cpp:4428`).  MEASURED: post-fit σ̂₀ splits **phase 0.0809 /
code 3.3982** on `pdfix_best6h`, phase 0.3744 / code 4.0125 on `pdfix_qz0` — the
phase pool is 0.08–0.37 of its assumed σ, and raising q_z drives it 4.6× closer to
zero.  **The carry is the sole source of phase-side redundancy on this lineage.**

`[derived]` Its structural reach, per (const,signal) component with V vertices and E
arcs: the carry pins K = E − (V−1) directions and leaves V−1 free forever.  On the
giant GPS L1C/L2W component at ep0242: **686 of 840 phase-row degrees of freedom
(81.7 %) restored, 154 permanently free** — the per-receiver and per-satellite
lumps.  That bound is independent of q_z, of run length, and of accumulator quality.

**The floor, MEASURED across every arm on the lineage.**  Rotating each chord's 2×2
band block into (NL, WL) and back-solving σ_eff from the measured resultant:

| posture | σ_formal (WL) | σ_eff | ratio |
|---|---|---|---|
| `pdfix_v3` / `best6h` / `on` / `v4` / `v5b` (q_z = 1e-4) | 0.117 cyc | 0.365–0.389 | 3.0–3.3× |
| `pdfix_iofloat`, `mechBoff`, `l2off` (representation interventions) | 0.117–0.119 | 0.352–0.382 | 3.0–3.3× |
| `pdfix_sdewl` / `mwwl` (lane arms) | 0.111–0.115 | 0.387–0.422 | 3.4–3.8× |
| `pdfix_qz0`, `_deg15`, `_rep15`, `_iofloat` (q_z = 0) | 0.041–0.043 | 0.351–0.452 | 8.6–10.5× |

> **σ_eff is invariant at 0.35–0.45 cycles across five independent representation
> interventions (σ_rep 5→50, 5→1.5, deg 8→15, layer-2 off, mech-B off), across both
> q_z values, and across both lane arms — moving by less than the null's own
> scatter.**

The independent per-chord estimate on true 4-arc cycles agrees: formal σ 0.110/0.141
cyc against a measured non-integer dispersion of **0.338/0.289 cyc point estimate,
≥ 0.256 cyc at 95 %**.

⇒ **The accumulator's one reachable subspace is limited by something no intervention
on the lineage has moved.  Building a second accumulator against a floor that five
existing interventions could not touch is the empty-population error, prospectively.
DO NOT BUILD.**

---

## §4 MEASURED — the carry saturates, and it saturates everywhere

Carried Pᶻ median, giant block, split by band (the panel's correction — pooling the
two bands hides a 2× split):

| snapshot | band-1 var (m²) | band-2 var | σ_b1 (L1 cyc) | σ_b2 (L2 cyc) |
|---|---|---|---|---|
| `best6h` ep0122 → ep0604 | 0.01505 → 0.01292 | 0.02927 → 0.02745 | 0.645 → 0.598 | 0.701 → 0.679 |
| `qz0` ep0122 → ep0242 | 0.00139 → 0.00083 | 0.00370 → 0.00224 | 0.197 → 0.152 | 0.249 → 0.194 |

The q_z arm is **flat from hour 1 to hour 5**; the q_z = 0 arm is 8.5–14× tighter and
still falling as `P ≈ R/K`.  Generalized by the panel to a posture the pdfix pair
never covered: `golden63_12h_internal` (lane OFF, 12 h) is flat to ±25 % from hour 1
to hour 11.

`[derived, corrected]` The applied increment is **not** `q_z·dt·I`.
`NetworkZStore::predict` has no production caller; the field time update is
`NetworkInvariantCarryDriver::beginEpoch:443-473`, and with `lane_carry.enable` true
and `q_wl_scale` defaulting to **0.01** it applies `Q = Rᵀ diag(q_z, 0.01 q_z) R dt`:
Q₁₁ = 1.152e-3, Q₂₂ = 1.878e-3, **Q₁₂ = 1.440e-3 m²** — the NL:WL ratio is 100:1.
Corrected back-out: R_eff(b1) = 0.12–0.20 m², R_eff(b2) ≈ 0.39–0.49 m²,
**K\* = 10–16 epochs = 5–8 min.**  ⚠ K\* is a *prediction*: the archive holds no
snapshot between ep0001 and ep0122, so "reached within minutes" is inferred.  What is
*observed* is "already at the floor by hour 1".

The corrected model then passes two cross-checks it was not fitted to: the implied
band ratio R₂/R₁ = 2.65 against the directly measured 2.66/2.69 in the q_z = 0 arm
(0.6–1.5 %), and the implied per-epoch σ ratio 1.627 against the **ionospheric**
scaling (f₁/f₂)² = 1.6469 (−1.2 %) rather than the cycle-common λ₂/λ₁ = 1.2833
(+27 %).  ⚠ Pᶻ is the filter's own covariance, so this says the **modelled** limiting
variance is iono-scaled — it does not independently measure reality.  Hold that
against §7.

---

## §5 The over-confidence is real, is already shipped knowledge, and is scoped to the lane

The carried covariance is optimistic — but three corrections to how this card first
stated it:

**(a) It is already measured and printed at run end.**  The D5 LS-VCE reports
σ̂₀ = **7.09×** (`pdfix_qz0`), **4.24×** (`pdfix_v3`), **3.38×** (`pdfix_best6h`) —
variance inflation 50×/18×/11× — and the loader banner in each log already says the
raw formal `Cov(v)` is "10–30× optimistic".  The independent snapshot derivation
(2.0–3.1× in σ) lands inside that.  **This card did not discover the over-confidence;
it re-derived a shipped number.**

**(b) It lives in the WL lane, not per band.**  Per band the carry claims σ = 0.57 cyc
⇒ R_pred = 0.0016 and delivers 0.049 — *honest and useless*.  Only in the WL
direction, which exists only under `lane_joint`, does it claim σ = 0.117 cyc
(R_pred = 0.76) and deliver the null.

**(c) It does not exist on lane-OFF postures at all.**  `golden63_12h_internal`,
`ext63_dual_12h_ref`, `golden305_gps` (lane OFF, `carry_invariant` + `pd_safe` on):
per-band formal σ 0.48–1.50 cyc, R_pred < 0.02 under every wavelength hypothesis.

⇒ **The over-confident cell exists only where `q_wl_scale = 0.01` makes the WL
direction 100× stiffer than the data supports.**  That is a *new and independent
mechanism* for the standing "MW/WL as conditioning — do NOT arm; net-harmful"
verdict, and it sits on the same over-retention signature CLAUDE.md's Aug-4 lane A/B
already recorded.  It is not an O2 finding; it is a lane finding, and the lane is
already OFF by directive at both scales.

---

## §6 MEASURED — the carried DD does not converge onto anything

The natural next hypothesis — that the accumulator converges onto a bias — is
**refuted by direct test.**  Tracking each chord's fractional part across two
snapshots one hour apart on the **q_z = 0 arm, where the filter adds exactly
nothing**, restricted to chords with unchanged cycle length and σ < 0.6 cyc at both
ends:

| arm | pair | n | R(Δf) | null floor | p | med \|Δf\| |
|---|---|---|---|---|---|---|
| `qz0` | ep0122→0242 | 108 | 0.0716 | 0.0853 | 0.575 | 0.222 |
| `best6h` | ep0122→0242 | 75 | 0.0635 | 0.1023 | 0.739 | 0.264 |
| `best6h` | ep0242→0484 | 30 | 0.0532 | 0.1618 | 0.918 | 0.268 |

A converged bias gives R(Δf) → 1.  This is **complete re-randomization**: the carried
DD's error wanders by a full cycle-width per hour at zero process noise.  Corroborated
on a different lineage — `golden63_12h_internal`, chords with Pᶻ diag < 0.03 m² at
both ends of a 1.008 h gap (n = 272): **97.1 % move by more than 5 cycles**, median
|Δẑ| = 132 cyc, frac(Δẑ) uniform.

**The carried ẑ is being re-seeded, not accumulated.**  Candidate drivers, all live
and all counted: 4 098 slip refloats and 411 tree swaps in 720 epochs against ~700
standing chords.  **They cannot be separated from genuine wander, because the blob
carries no birth-epoch or age field** — which makes adding that field the single
concrete, minimal next step (§9).

---

## §7 ~~THE OPEN LEAD — the residual is not ionospheric~~ — **WITHDRAWN 2026-08-21**

> **⚠ RETRACTED, same day, by `SPEC_phase_windup_sign.md` §6.  Do not cite this
> section's conclusion.**  "Equal in cycles" is **not a separate direction**: in
> per-band cycle coordinates `cc = 0.5691·geo + 0.4313·iono` with a residual of
> **0.230 % of ‖cc‖ (0.132°)**, and the phase observation equation confines the DD
> ambiguity error to exactly `span{geo, iono}`.  On **GPS L1/L2 — every field
> lineage — the separation is formally undetermined** (2 observations, 2 basis
> vectors span ℝ²).  Both statistics below are one coordinate of one 2-parameter
> mixture: "equal in cycles" appears whenever the geo : iono split is near
> 0.57 : 0.43, and the measured splits are 59 : 41 (field), 56 : 44 (§13.6's 1 h
> fixture — which reproduces it exactly) and **33 : 67 on the settled 480-epoch arm,
> where the signature disappears**.  The data prefers `geo + iono` over `cc + iono`
> on 63.7 % of rows (z = 64.5), and the empirical second direction sits 0.03–0.09°
> from `geo`.  **The ionospheric component was there all along — it is the dominant
> one (67 %) on the settled arm.**  Phase-windup is separately retired as a carrier:
> measured `|windup| ≤ 0.98 cyc` with within-arc range median 0.087 / max 0.632 and
> 0/377 arcs above 1.0 cyc, and it is structurally absent from §13.6's arm
> (`accumulate_phase_windup = true` there, injected through the same function).
>
> The section is kept verbatim below because the *measurements* in it are sound; only
> the inference is wrong.  The hunt it launched did find a real defect — an inverted
> wind-up sign — recorded in `SPEC_phase_windup_sign.md`.

The one thing in this card that points somewhere new, and it points away from the
premise of the entire 8-family:

- `scripts/grade_dd_spillway.py:131-165` computes the cross-band step ratio **in
  metres**, where common-cycle (dN₁ = dN₂) = λ₂/λ₁ = **1.2833** and raw iono =
  (f₁/f₂)² = **1.6469**.  The measured 1.262–1.265 in nine arms is therefore the
  **common-cycle** direction.  **The ionospheric direction has never been observed.**
- Independently and truth-graded, `SPEC_synthetic_minimum_network.md` §13.6: the
  per-band DDs carry an extra error component **equal-in-cycles across bands** — all
  three bands at the same ~5.0 cyc med|e|, so it **cancels exactly in every
  between-band difference**; per-band:WL 12.8× measured against 3.5× iono-predicted;
  leading candidate carrier **phase-windup**, "capable of full-turn integer wraps".

~~Set against §4: the *modelled* limiting variance is iono-scaled to 1.2 %, while the
*measured* residual is equal-in-cycles.  **The estimator believes it is
ionosphere-limited and the data says the dominant residual is something else.**~~
**Withdrawn — see the banner above.  §4's iono-scaled modelled variance and the
measured residual are in fact CONSISTENT; the apparent discrepancy came from reading
a mixture coordinate as an independent direction.**

---

## §8 Retractions — four supporting arguments the panel refuted

Recorded rather than deleted, because three could be repeated.

**R1 — "P6(b) is a statement about q_z, not cadence or geometry."  REFUTED.**  The
counterfactual exists on the same substrate: at q_z = 0 the formal σ falls 8–14× and
the carried z's resultant stays at the uniform null; the geometry lever was also moved
(`pdfix_gg`, DD pool 387 → 1922 keys) with med|frac| unmoved.  `pdfix_qz0` *is* the
run-length control the card demanded, and it produces no integer information either.
**What survives is a different and narrower criticism**: P6(b)'s statistic has *zero
power* — at the recorded med σ of 5.0–5.4 cyc, med|frac| = 0.25 by arithmetic.  Read
honestly over 387 independent keys it is a **lower bound**, R_crit(p = 0.05) = 0.088
⇒ DD non-integer error **≥ 0.30 cycles**.  "NO DD integer information" over-reads a
bound as an absence.  (Prior art, better attributed: `SPEC_carry_aware_covariance.md`
§1 already stated that all 4 865 final-hour DDs report σ > 1 cyc, median 6.02, so
"P6(b)'s six-hour null could not have returned anything else".)

**R2 — the fractional statistic on the snapshot blob.  RETRACTED as constructed.**
`CarriedBlock::z` is the DD of the state **deviation from the linearization point**,
not the absolute DD; the absolute value is `(z + z_lin)/λ` and **`z_lin` is not in
the blob** (measured contribution: median 78.5 cyc, p90 9 971, max 243 393).
Uniformity of `frac(z_blob/λ)` is forced by construction.  The "15×" was doubly
wrong — wrong object, and a resultant *ratio* is not an error ratio, since R is
exponentially sensitive to σ.  **The conclusion survives on the correct instrument
and survives harder**: `network_wl_lane_trace.csv` *is* the de-referenced carried z,
1:1 with carried chords — 247 468 rows, circR **0.0017** against a Rayleigh null of
0.0018, med|f| 0.2499, and flat at the null in every chord-age stratum (0–3, 25–50,
50–100, ≥100 epochs) on both arms.

**R3 — the cross-band-ratio corroboration.  RETRACTED, and it inverts.**  This card
derived the ratio in *cycles* (correctly, α₂λ₁/λ₂ = λ₂/λ₁ = 1.2833) and matched it
against a measurement taken in *metres*.  In the grader's units the inference
reverses — see §7, which is what the corrected reading actually says.

**R4 — significance computed on non-independent rows.  CORRECTED.**  Carried rows are
correlated (mean off-diagonal ρ̄ ≈ 0.010–0.015 ⇒ n_eff ≈ 87 where n = 589), and the
solve-side trace has 387 distinct DD keys behind 16 132 rows (median 35 repeats
each).  Collapsed per key, neither arm rejects uniform (qz0 p = 0.193, q_z = 1e-4
p = 0.763; arm difference 1.5σ).  The earlier reading of a "weak but real"
concentration at q_z = 0 was an artifact of treating repeats as independent.

Two further claims are **UNVERIFIED rather than refuted**: that the carried-DD error
is the same defect as 8(b)'s in a different vehicle (magnitude is consistent —
0.29–0.34 L1 cyc ≈ 0.34–0.40 TECU DD-equivalent — but nothing attributes it to the
exchange rather than to tropospheric gradients, PCV mismatch, multipath or §7's
windup); and that the situation is a standing-invariant-#1 instance (on the shipped
posture the carried z is *not* zero-q and *does* have a wired, tested response — D5 v3
corroborated re-tap; the bare zero-q instance is `qz0`, already fenced as "a
DIAGNOSTIC SUBSTRATE, NOT A POSTURE").

---

## §9 Disposition

**The O2 accumulator card is CLOSED, unbuilt.**  It is not ranked into the queue.
§1 (the channel already exists and measured null), §2 (two of three subspaces are
gauge), §3 (the third is floored at 0.35–0.45 cyc, invariant to five interventions)
are each independently sufficient.

Three residues, in priority order:

**A — the equal-in-cycles residual (§7).**  The measured cross-band direction is
common-cycle, not ionospheric, and the truth-graded sim independently finds an
extra error component that cancels exactly in every between-band difference, with
phase-windup named as the leading candidate.  This is the lead worth taking, and it
is not an 8-family question at all.

**B — a birth-epoch / age field on the carried chord (§6).**  The carried ẑ
re-randomizes completely over an hour at zero process noise, and refloat, tree swap
and genuine wander cannot be separated because the blob has no age field.  It is a
snapshot-schema addition, solve-inert.  Note this is also the precondition for any
time-domain carry instrument — the one this card originally proposed cannot be built
without it.

**C — two hygiene items found in passing, both filed, neither an O2 question.**
(i) The canonical 24 h field lineage `_ft63_24h_conv.json` carries `pd_safe` **absent**
and runs **12.8–15.3 % negative-variance carry for 23 hours**.  (ii) All 7
`carry_invariant` sim configs resolve `carry_invariant_qz` absent ⇒ 0.0 **and**
`carry_invariant_innov_gate` absent ⇒ false — a stiff zero-q carry with no D5 event
response.  The §13.7 runaway arm is exactly this posture.

> **CORRECTED 2026-08-21.**  The resolved values above are right; the invariant-#1
> conclusion drawn from them was wrong, and the census was incomplete.  Audited by
> resolving all 1004 configs through their `extends` chains (`scripts/gnss_config_chain.py`)
> rather than grepping:
>
> - **`carry_invariant` resolves true in 633 configs, not ~203** — the literal-grep
>   figure undercounts ~3×.  The `qz ≤ 0 ∧ ¬innov_gate` predicate matches **8**, not 7:
>   the 7 sim configs plus one FIELD config, `config/archive/_eq34_carryinv_3h.json`
>   (50 real IGS stations, DOY-129).
> - **All 8 nonetheless HAVE a wired event response, so invariant #1 is satisfied.**
>   D5 is not the carry's only response.  The C3c slip funnel `carry_slipped_ambs` —
>   fed by `cycle_slip.mw_detect` and `cycle_slip.if_phase_td`, read by `ambSlipped()`
>   to re-tap the chord from a diffuse prior — is guarded by `if (carry_invariant)`
>   **alone** (`NetworkSlipQc.cpp:411`, `:903`), so it is live from epoch 1 with the D5
>   gate off.  The 7 sim configs arm both detectors explicitly; the field one gets
>   `mw_detect` from the `phase_leveling` auto-arm at `GnssConfigLoadNetwork.cpp:3815`.
> - Across all 633, **zero** configs have no response at all (526 via a pre-fit
>   detector, 107 via the D5 gate).  The invariant held by convention across every
>   config, not by construction — which is the real gap, and it is now closed: see the
>   invariant-#1 interlock at `GnssConfigLoadNetwork.cpp:6759` (two tiers, WARN-only,
>   solve byte-identical; tests in `tests/test_codebias_network_config.cpp`).
>
> **What survives, and it is sharper than the original claim.**  The §13.7 fixture
> injects **zero** cycle slips (`runs/simnet60_tri/*/truth_slips.csv`: 61 files, 61
> lines, all headers, 0 data rows), so the armed detectors are idle *by construction*.
> D5 is the only response that fires on MODEL ERROR rather than on a detected raw slip.
> So on that arm the effective event response really is nil — not because the config is
> unguarded, but because its guard has nothing to trigger on — and at `q_z = 0` a
> model-error excursion cannot leak off either.  That is a testable single-knob
> hypothesis rather than a posture complaint.
>
> **MEASURED THE SAME DAY, and the hypothesis is REJECTED.**  Three same-build 480-epoch
> arms (`_simnet60_tri_slant_trio_480_ab_{base,d5,d5cal}.json`; full table in
> `SPEC_synthetic_minimum_network.md` §13.7.1).  The control reproduces §13.7 on the
> current build.  Arming D5 uncalibrated LOOKS like a large win (settled EWL at true
> integer 3.3 % → 11.2 %) but the counters expose it: 140 126 flags over 480 epochs
> against 960 carried DDs is a ~30 % carry turnover **every epoch**, so the "win" is the
> carry being dismantled and the arm regressing toward the float twin.  Calibrated — the
> only way `innov_k = 4` is correct, since raw formal Cov(v) is 10–30× optimistic (M2) —
> the flags drop 5.4× and the EWL gain vanishes back to control (3.6 % vs 3.3 %), while
> per-band med|e| improves 15.6 % and stays ~160× worse than float.  **The runaway is the
> estimator on this posture, not the missing gate.**  §9(C)(ii) is therefore a hygiene
> item only, exactly as it was filed — it is not a lead on the O2 question, and the
> mechanism card owed by §13.7 should not spend an arm on the innovation gate.

**Explicitly NOT recommended**, each priced and rejected: a second iono accumulator
(§1, §3); reducing `representation_sigma_tecu` (P3b FAIL — it is the *absolute*
tie's weight, so it over-constrains the level while delivering nothing differential);
enlarging the SH basis (P3b expensive null — the functional model is not what
declares the residuals independent); a spatially-correlated representation model
(**priced empty at 63 stations**: median nearest-neighbour 2063 km, 17 of 1953 pairs
under 200 km, median pair separation 8478 km — it is a 305-station card, where 48 %
of stations have a neighbour inside 200 km); and any reshaping of Q_z, whose only
shipped vehicle is `q_wl_scale`, already at 0.01 and already identified in §5 as
where the covariance becomes dishonest.

---

## §10 What is NOT established

- **§7 is a lead, not a result.**  Phase-windup is `SPEC_synthetic_minimum_network.md`
  §13.6's *candidate*, not a demonstrated carrier.  Nothing here tests it.
- The §3 floor of 0.35–0.45 cyc is measured on the pdfix lineage (63 stations, GPS,
  DOY-129) and generalized only as far as the arms listed.  It is a *dispersion*,
  and §6 shows it is not a static bias — but what it *is* remains open.
- The instrument that produced the historical verdicts is compromised twice over —
  carry-blind, and pairing everything against one lexically-lowest PRN per
  (const,signal).  MEASURED on `pdfix_best6h` (720 epochs, 57 053 rows, subsample cap
  absent ⇒ unlimited): 10 reference satellites ever used, dominated by G03 (47.5 %)
  and G02 (34.2 %); **38 of 63 stations ever appear**; and **zero co-located pairs are
  ever sampled** — WTZZ appears 2 588 times while WTZR and WTZS, 69 m away, appear not
  once.  This does not overturn any verdict (§3 and §6 rest on the carried object) but
  it means the historical evidence never covered the sub-population where a
  prediction would be unambiguous.  Re-pointing that selection rule is cheap and
  solve-inert; it is a precondition for grading residue A on short baselines.
- No new run was made, and no truth grade of the **carried ẑ** exists on any posture.
  `scripts/simnet_amb_integer.py` does grade absolute integers against truth and has
  been run with `carry_invariant` armed (§13.7) — but on the solve-reconstructed DD,
  and on the q_z = 0 / no-innov-gate posture of §9(C)(ii), so it measures that posture
  rather than the field's.
