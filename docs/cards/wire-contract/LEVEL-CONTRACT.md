# LV-3 contract — the normative contract for the ionosphere LEVEL and receiver-DCB frame of the emitted products

**STATUS**

- **id:** LV-3 contract
- **topic:** wire-contract
- **status:** closed
- **summary:** the normative contract for the ionosphere LEVEL and receiver-DCB frame of the emitted products
- **dates in the text:** 2026-08-01 → 2026-08-15
- **knobs / posture:** `network.estimate_sat_code_bias`, `network.external_sat_code_bias`, `network.rx_code_bias_datum`, `estimation.global_vtec_sh.mean_vtec_prior_tecu`, `network.iono_seed_field_frame`, `network.rx_cb_carry_step_gate`, `network.rx_cb_harvest_demean`, `network.a00_prior_from_brdc`
- **hashes named in the text** (commit / exe / byte-cert): —
- **run dirs named in the text:** —
- **source:** `docs/archive/SPEC_level_information_budget.md` — 30, 30.1, 30.2, 30.3, 30.4, 30.5, 30.6, 30.7

> Results are appended TO THIS FILE, below the verbatim text. The archived monolith is frozen; never append there.

---
<!-- BEGIN VERBATIM: SPEC_level_information_budget.md -->
## 30. CONTRACT — the ionosphere LEVEL and receiver-DCB frame of the emitted products (card LV-3)

*Absorbed 2026-08-15 from `docs/CONTRACT_level_authority.md` (now `docs/attic/`), card **LV-3** of this SPEC.
Subsection numbers preserve that document's: §1→§30.1, §2→§30.2, §3→§30.3, §4→§30.4, §6→§30.6, §7→§30.7. **Its
§5 (the unbuilt `lvlauth=` / `rxdcb=` S-line tokens) went to `docs/SPEC_product_cbpin_contract.md` §6.6**,
which owns the S-line grammar. That doc declares the satellite code-bias *signal pair* the product is
internally consistent in; this declares the **level / common-mode** half. **No code change belongs to this
card** — the frames already exist; what was missing was anyone writing down which one is in force.*

### 30.1 Why a level needs a declared owner

The global ionosphere LEVEL (a₀₀) and the receiver-DCB COMMON MODE are one confounded direction, separated
only by the weak mapping-function spread: the geometry constrains between-satellite and between-station
*structure* far better than the pair (level, Σ rx-DCB). **So the level is never "estimated" in the ordinary
sense — it is REALIZED by whichever piece of the design puts information on that direction, and the emitted
products inherit that choice.** Measured information on the common-mode direction, DOY-129, 61 GPS stations:

| channel | info/epoch on the level direction | note |
|---|---|---|
| a₀₀ climatology anchor at σ = 2 TECU | **0.25** | folds EVERY epoch, so it stands against a walk |
| rx-DCB zero-mean datum at σ = 10 m | **0.01** | **N-INDEPENDENT**: u = 1/√N·**1** ⇒ uᵀ(ΛI)u = Λ |
| rx-CB carry at var ≈ 1e-2 m², ~60 stations | **~100** | carries LAST epoch's realization — **memory, not a datum** |
| mapping-function spread (the physical separator) | weak | the only channel that is about physics |

The third row is what this contract exists to prevent from recurring: for most of the ladder the frame was
owned by an *accident* — the rigidity of an unrelated carry — and neither designed authority was in charge.

### 30.2 The contract, per posture

#### 30.2.1 Self-contained (W6): the satellite code-bias sum-to-zero

`network.estimate_sat_code_bias` (W6) gauges the estimated satellite code biases with a per-(constellation,
signal) **sum-to-zero** — **the same convention the IONEX/DCB community realizes its products in** (zero-mean
satellite DCBs), not an arbitrary numerical convenience. It fixes the confounded direction at the value the
rest of the world's ionosphere maps use, which is *why* the self-contained control's a₀₀ lands GIM-compatible
(**23–28 TECU against COD's 32.24**) with **no external data at all**: **convention agreement, not luck.**

- **Level authority:** the satellite code-bias sum-to-zero gauge.
- **Receiver-DCB common mode:** determined *through* the satellite gauge and the ionosphere; a receiver-side
  datum is legal alongside but **REDUNDANT** (the loader says so).
- **Consumer inherits:** level and satellite biases in the zero-mean-satellite-DCB S-basis. A consumer
  combining these with externally-published DCBs in the same convention is frame-consistent; one that mixes in
  a product realized in a *receiver* zero-mean convention is not.

#### 30.2.2 EXT-CB ladder: the a₀₀ climatology anchor

Under `network.external_sat_code_bias` the satellite biases come from the external `.BIA` and the satellite
gauge is *given away* — with it, the only anchor the receiver-DCB common mode had. Two facts then decide the
frame:

1. `network.rx_code_bias_datum` (zero-mean over receivers, σ = 10 m) stops the walk but pins the **wrong
   value**: the 61 stations' true C1C−C2W biases have a network mean ≈ -5 ns ≈ -1.55 m ≈ **-15 slant-TECU**
   (measured estimator-free, `scripts/extcb_gim_verdict.py`). Forcing Σ rx-DCB = 0 pushes that mean into the
   confounded direction — into the level, low by ≈ mean(b)/⟨m⟩ ≈ 11–15 TECU. **Convention ≠ physics, by
   exactly the receiver-bias mean.**
2. The fix is a **frame choice, not a tuning**: adopt the reference product's level through the **per-epoch
   mean-reverting** a₀₀ anchor, `estimation.global_vtec_sh.mean_vtec_prior_tecu = 32.24`, `…_sigma_tecu = 2.0`
   (validated arms 9/12/13). **It MUST be the per-epoch anchor: an initial-only prior does not stand against
   the walk.**

- **Level authority:** the a₀₀ climatology/GIM anchor (32.24 ± 2 TECU).
- **Receiver-DCB common mode:** the zero-mean receiver datum — now a **CONVENTION, not a measurement**; the
  physical mean lives in the level, which the anchor holds.
- **Canonical posture:** SINGLE-layer deg-8 + a₀₀ GIM anchor + `rx_code_bias_datum` — frame-commensurate with
  the single-shell GIM the level is graded against. Dual-layer belongs to the self-contained lineage where
  Iono-2 mech-A earned it.
- **Consumer inherits:** a level realized in COD's IONEX frame and receiver DCBs in a
  zero-mean-over-THESE-receivers convention. **The receiver SET is part of the frame: a different station list
  realizes a different zero.**
- **Scope:** this posture imports external information by design. It is the validation ladder, **not** the
  self-contained product (standing directive: no external-data dependence there — the GIM stays
  diagnostics-only).

#### 30.2.3 Neither armed

`network.external_sat_code_bias` without `rx_code_bias_datum` and without an informative a₀₀ anchor leaves the
direction to the weak mapping-function spread alone. Measured: **a₀₀ walks to -80 TECU in 20 epochs with its
formal σ collapsing** (arm 2). **This is not a configuration to grade; it is a rank defect with a slow fuse.**

### 30.3 What is NOT a level authority (paid for, do not retry)

- **The rx-CB carry.** ~100 info units on the common mode — 400× the designed datum — but toward *last
  epoch's realization*. That is memory, and it makes the level a flywheel: arms 11/13 breathe ±6 TECU on a
  20–30 min cycle with it, arm 14 free-falls at -9 TECU/epoch without it. Neither is a designed equilibrium.
  Keep the carry (its between-station DIFFERENCE rigidity is load-bearing) and remove its common-mode force —
  LV-2's approach, but see §13/§14 for why that particular removal is not the answer, and §28 for the one that
  is on the fold side.
- **Tightening the rx-DCB datum σ.** A stiff zero-mean imports the -5 ns convention offset into the level at
  datum stiffness — the original defect, hardened.
- **SH degree / representation.** Falsified: degree 8 → 15 moved ignition the WRONG way (12.78 → 6.18) and
  rings pointwise at 61 stations (-446 TECU vs GIM).
- **The per-LOS iono SEED.** §30.4 — it cannot move the solve at all.

### 30.4 The seed is a FRAME, not an authority (LV-1's real scope)

`StateInitializer::computeIonosphereSeed` seeds each per-LOS iono state from the geometry-free code
combination, which carries the receiver's own GF hardware bias (`mod` removes the satellite side and the
geometry, not the receiver DCB). It is tempting — and the LV-1 card originally argued — that this "bias-in
frame" pulls the field, at ~0.04 info × N_LOS ≈ 15 units. **It does not, and cannot.**

On the network path the per-station state vector is a frozen LINEARIZATION POINT (written once at setup, never
written back from the solve), the per-epoch absolute solve carries **no Kalman prior on the local nuisances**,
and every row touching a GPS/Galileo iono column forms its RHS against x_lin with that row's own A_r
coefficient. A seed shift δ gives y → y − A_r e_c δ, so u_s → u_s − N_sr e_c δ and u_r → u_r − N_rr e_c δ, and

    b_red = u_s − N_sr N_rr⁻¹ u_r  →  b_red      EXACTLY

with x_r absorbing the shift, **so the ABSOLUTE estimate x_lin + x_r is unchanged too** (invariance theorem,
2026-08-01). **Measured confirmation:** with the rx-CB carry OFF, arming `network.iono_seed_field_frame`
reproduced the arm-14 collapse trajectory to within 1.8 TECU on a -37 TECU excursion, endpoints matching to
**0.006 TECU** (arm 16 vs arm 14) — no level channel.

What the seed frame DOES reach is every consumer that reads a **prefit** quantity in the x_lin frame, and
those change row sets, which is a real actuator. **Three consumers, not one:** (1) the rx-CB carry's inject
gate, which tests |value_abs − x_lin(cb)| against 10σ + 0.5 m while `initializeCodeBiases` derives x_lin(cb)
by subtracting the *seeded* iono, so the seed frame decides which stations' carried priors fold at all —
measured (arm 15 vs arm 12, carry ON, single knob): folds 448 → 382, skips 710 → 776, ignition trough **6.06 →
13.81 TECU**; (2) the prefit census / fuse screens (trace-only unless `prefit_fuse_sigma` is armed); (3) the
GLONASS local-nuisance anchors, whose rows pull toward x_lin with y = 0 (GPS-only postures never build them).

**Contract consequence:** the emitted level is a function of the seed frame *only through the gate*, and a
gate that thresholds a frozen arbitrary linearization point is a defect, not a design. Grade LV-1 on gate
statistics; own the level with §30.2's authority.

**Update — the gate is now fixable** (`network.rx_cb_carry_step_gate`, card LV-4, §11): the screen moved to
the frame-invariant epoch-over-epoch step of the ABSOLUTE posterior against a robust peer-relative scale.
Measured same-run A/B: the retired screen would have withheld **74 % of the folds** the invariant one admits,
while the invariant one erases ~1–2 % on genuine outliers. Two contract-relevant consequences:

- ⚠ **"The seed frame stops reaching the level at all once LV-4 is armed" — FALSIFIED, corrected here
  2026-08-15.** The original CONTRACT §4 asserted LV-1's only actuator was that gate, so with it fixed the
  invariance theorem becomes the whole story and the emitted level depends on §30.2's authority alone.
  **Measured the same day (§13.1/§13.2): arms 23 vs 27 both carry LV-4+LV-5 and differ only in LV-1, yet
  settle at 35.24 vs 26.45 TECU (end 32.68 vs 25.11) with IDENTICAL row sets (7182 vs 7180 step checks, 124 vs
  125 erases). LV-1 has a SECOND channel — it sets x_lin(cb), which is exactly what LV-2's de-mean re-pins the
  common mode to.** The emitted level therefore does NOT depend on §30.2's authority alone whenever LV-2 is
  armed.
- **The retired screen was silently acting as an information limiter.** Admitting the full carry makes the
  level limit-cycle (a₀₀ settled sd 4.98 → 15.53 TECU over 1 h) because LV-2's μ was applied lag-1 and the
  loop gain scales with admission. **Card LV-5 (§12) closes it**: settled sd 15.53 → **4.69**, level ending on
  the GIM reference (32.68 vs 32.24), residual common-mode force **0.02 m against max |μ| 8.50 m**.

~~**Declared EXT-CB posture: LV-1 + LV-2 + LV-4 + LV-5 together.**~~ **SUPERSEDED by the 12 h verdict (§14):
the stack's level RAMPS to +15…+21 TECU past hour 2 — the 1 h landing that declaration was written on was an
ignition transient. The posture of record for settled work is the CANONICAL one (§14.4/§17); the principled
replacement is the S-consistent harvest (§17 item 3 — built 2026-08-07 as `network.rx_cb_harvest_demean`, card
S2H, §28).** What survives: the pairing note (LV-4 without LV-5 oscillates; LV-5 without LV-4 nearly inert)
and both cards' validity as fixes to the defects they name (§11/§12).

### 30.5 What a consumer should be told — MOVED

The unbuilt consumer-facing declaration (`lvlauth=` and `rxdcb=` S-line tokens, both interpretation hints of
the `cbpin=`/`agek=` class, never replay-emitter construction parameters) now lives in
**`docs/SPEC_product_cbpin_contract.md` §6.6**. The requirement in one line: the emitted stream declares
`cbpin=` but says NOTHING about the level authority, so **a consumer cannot tell an a₀₀-anchored product from
a sum-to-zero-gauged one, and the two differ by the receiver-bias mean (~15 TECU on this network).**

### 30.6 Optional, DEFERRED: an internal a₀₀ climatology from the broadcast stream

`network.a00_prior_from_brdc` (§5 item 2) would compute the a₀₀ target from the Klobuchar coefficients already
in the ingested navigation data — an area-weighted global mean of the Klobuchar vertical delay, converted to
TECU at L1 — fed through the existing `mean_vtec_prior_*` channel at LOOSE σ (10 TECU), giving a posture
WITHOUT the GIM anchor an operationally self-contained level authority.

**Prerequisite VERIFIED 2026-08-05:** the RINEX-nav ION ALPHA/BETA records ARE retained —
`ProductParser::parseKlobuchar` → `ProductCache::getKlobucharCoeffs()`, with `Ionosphere::loadKlobucharCoeffs`
as a direct-from-header fallback (both normalize the Fortran `D` exponent, so 2024 DOY-129/131 files both
parse). **No parser work is part of this card.**

**Deliberately not built** (ranked LAST, conditional on LV-1/LV-2): the coefficients become available **only
after product load**, so the target must be a **RUNTIME override** of
`GlobalVtecShConfig::mean_vtec_prior_tecu` rather than a config-load value — a real design decision (who owns
the override, how a warm resume fingerprints it) that should not be made as a side effect of a climatology
helper. Worth doing only if ignition speed still matters after LV-2, and only for postures with no GIM anchor.

### 30.7 How to grade a level claim (tooling, 2026-08-05)

Every level verdict on this ladder up to 2026-08-05 was quoted against **one number**, "GIM 32.24 TECU" — the
global mean of the day's 00:00 IONEX map. Fine for an ignition arm covering minutes and **wrong for anything
longer**: the global mean VTEC itself moves through the day (DOY-129: **32.0 TECU at 00 UT, 36.9 at 08 UT,
33.9 at 24 UT**), so a run whose a₀₀ rises may simply be tracking it.

`scripts/gim_global_mean_series.py <run_dir> …` computes the area-weighted (cos-latitude) global mean of every
map in the day's IONEX, interpolates in time, and reports `a₀₀(t) − GIM_mean(t)` with hourly statistics.
**That functional is the right comparison: a sun-fixed SH expansion's a₀₀ IS the global mean, because the
basis is orthonormal under (1/4π)∫dΩ so Ȳ₀₀ ≡ 1.**

**Caveats to quote with any number it produces**: IONEX maps carry a few TECU of their own error; their global
mean leans on data-free ocean/southern interpolation; and a₀₀ is a GLOBAL mean while the network sees one
footprint, with the unsupported hemisphere Kaula-extrapolated. **It is a level COMPARISON, not truth.** Pair
it with the footprint restriction per §26.4.

### Related

- `docs/SPEC_product_cbpin_contract.md` — the signal-pair half of the frame contract, and (§6.6) the level
  half's unbuilt product declaration.
- `docs/SPEC_rx_cb_carry.md` — the carry whose common-mode force LV-2 removes.
- `docs/SPEC_beidou_w6_datum.md` — the satellite-side gauge §30.2.1 leans on.
- `docs/SPEC_pivot_visibility_gauge_defect.md §8` — the reduced-level 5-86 transform (§16/12.3's escalation).

