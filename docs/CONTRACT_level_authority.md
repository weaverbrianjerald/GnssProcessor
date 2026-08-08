# CONTRACT — the ionosphere LEVEL and receiver-DCB frame of the emitted products

Card **LV-3** of `docs/SPEC_level_information_budget.md`.  Companion to
`docs/SPEC_product_cbpin_contract.md` (which declares the satellite code-bias
*signal pair* the product is internally consistent in); this one declares the
**level / common-mode** half of the same question: *whose convention realizes the
global ionosphere level and the receiver-DCB common mode in a given run, and what
does a consumer inherit as a result.*

Written 2026-08-05 from the measured EXT-CB ladder (`docs/HANDOFF_ext_cb_iono_level.md`,
arms 2–19).  **No code change belongs to this card** — the frames below already
exist; what was missing was anyone writing down which one is in force.

---

## 1. Why a level needs a declared owner

The global ionosphere LEVEL (the SH field's a₀₀) and the receiver-DCB COMMON MODE
are one confounded direction.  Only the mapping-function spread separates them,
and that separator is weak: the observation geometry constrains between-satellite
and between-station *structure* far better than it constrains the pair
(level, Σ rx-DCB).  So the level is never "estimated" in the ordinary sense — it
is **realized by whichever piece of the design puts information on that direction**,
and the emitted products inherit that choice.

Measured information on the common-mode direction, DOY-129, 61 GPS stations:

| channel | info/epoch on the level direction | note |
|---|---|---|
| a₀₀ climatology anchor at σ = 2 TECU | **0.25** | folds EVERY epoch, so it stands against a walk |
| rx-DCB zero-mean datum at σ = 10 m | **0.01** | N-INDEPENDENT (u = 1/√N·**1** ⇒ uᵀ(ΛI)u = Λ) |
| rx-CB carry at var ≈ 1e-2 m², ~60 stations | **~100** | carries LAST epoch's realization — memory, not a datum |
| mapping-function spread (the physical separator) | weak | the only channel that is about physics |

The third row is the finding this contract exists to prevent from recurring: for
most of the ladder the frame was owned by an *accident* — the rigidity of an
unrelated carry — and neither of the two designed authorities was in charge.
`network.rx_cb_carry_demean` (card LV-2) removes that row's net force; this
document says who is supposed to own the direction once it is gone.

## 2. The contract, per posture

### 2.1 Self-contained (W6): the satellite code-bias sum-to-zero

`network.estimate_sat_code_bias` (W6) gauges the estimated satellite code biases
with a per-(constellation, signal) **sum-to-zero**.  That is not an arbitrary
numerical convenience — it is **the same convention the IONEX/DCB community
realizes its products in** (zero-mean satellite DCBs).  Adopting it fixes the
confounded direction at the value the rest of the world's ionosphere maps use,
which is *why* the self-contained control's a₀₀ lands GIM-compatible (23–28 TECU
against COD's 32.24) with **no external data at all**: convention agreement, not
luck.

- **Level authority:** the satellite code-bias sum-to-zero gauge.
- **Receiver-DCB common mode:** determined *through* the satellite gauge and the
  ionosphere; a receiver-side datum is legal alongside but REDUNDANT (the loader
  says so).
- **Consumer inherits:** an ionosphere level and satellite biases in the
  zero-mean-satellite-DCB S-basis.  A consumer combining these products with
  externally-published DCBs in the same convention is frame-consistent; one that
  mixes in a product realized in a *receiver* zero-mean convention is not.

### 2.2 EXT-CB ladder: the a₀₀ climatology anchor

Under `network.external_sat_code_bias` the satellite biases come from the
external `.BIA` and the satellite gauge is *given away* — with it, the only anchor
the receiver-DCB common mode had.  Two facts then decide the frame:

1. `network.rx_code_bias_datum` (zero-mean over receivers, σ = 10 m) stops the
   walk but pins the **wrong value**: the 61 stations' true C1C−C2W biases have a
   network mean ≈ −5 ns ≈ −1.55 m ≈ **−15 slant-TECU** (measured estimator-free,
   `scripts/extcb_gim_verdict.py`).  Forcing Σ rx-DCB = 0 pushes that mean into
   the confounded direction — i.e. into the level, low by ≈ mean(b)/⟨m⟩ ≈ 11–15
   TECU.  Convention ≠ physics, by exactly the receiver-bias mean.
2. The fix is therefore a **frame choice**, not a tuning: adopt the reference
   product's level explicitly through the per-epoch mean-reverting a₀₀ anchor,
   `estimation.global_vtec_sh.mean_vtec_prior_tecu = 32.24`, `…_sigma_tecu = 2.0`
   (validated arms 9/12/13).  It must be the PER-EPOCH anchor: an initial-only
   prior does not stand against the walk.

- **Level authority:** the a₀₀ climatology/GIM anchor (32.24 ± 2 TECU).
- **Receiver-DCB common mode:** the zero-mean receiver datum — but note it is now
  a *convention*, not a measurement; the physical mean lives in the level, which
  the anchor holds.
- **Canonical posture:** SINGLE-layer deg-8 + a₀₀ GIM anchor + `rx_code_bias_datum`
  — frame-commensurate with the single-shell GIM the level is graded against.
  Dual-layer belongs to the self-contained lineage where Iono-2 mech-A earned it.
- **Consumer inherits:** a level realized in COD's IONEX frame and receiver DCBs
  in a zero-mean-over-THESE-receivers convention.  The receiver set is part of
  the frame: a different station list realizes a different zero.
- **Scope:** this posture imports external information by design.  It is the
  validation ladder, **not** the self-contained product (standing directive: no
  external-data dependence in the self-contained posture — the GIM stays
  diagnostics-only there).

### 2.3 Neither armed

`network.external_sat_code_bias` without `rx_code_bias_datum` and without an
informative a₀₀ anchor leaves the direction to the weak mapping-function spread
alone.  Measured: a₀₀ walks to **−80 TECU in 20 epochs with its formal σ
collapsing** (arm 2).  This is not a configuration to grade; it is a rank defect
with a slow fuse.

## 3. What is NOT a level authority (paid for, do not retry)

- **The rx-CB carry.**  It carries ~100 info units on the common mode — 400× the
  designed datum — but toward *last epoch's realization*.  That is memory, and it
  makes the level a flywheel: arms 11/13 breathe ±6 TECU on a 20–30 min cycle
  with it, and arm 14 free-falls at −9 TECU/epoch without it.  Neither is a
  designed equilibrium.  Keep the carry (its between-station DIFFERENCE rigidity
  is load-bearing) and remove its common-mode force with
  `network.rx_cb_carry_demean` (LV-2).
- **Tightening the rx-DCB datum σ.**  A stiff zero-mean imports the −5 ns
  convention offset into the level at datum stiffness — the original defect,
  hardened.
- **SH degree / representation.**  Falsified: degree 8 → 15 moved ignition the
  WRONG way (12.78 → 6.18) and rings pointwise at 61 stations (−446 TECU vs GIM).
- **The per-LOS iono SEED.**  See §4 — it cannot move the solve at all.

## 4. The seed is a FRAME, not an authority (LV-1's real scope)

`StateInitializer::computeIonosphereSeed` seeds each per-LOS iono state from the
geometry-free code combination, which carries the receiver's own GF hardware bias
(`mod` removes the satellite side and the geometry, not the receiver DCB).  It is
tempting — and the LV-1 card originally argued — that this "bias-in frame" pulls
the field, at ~0.04 info × N_LOS ≈ 15 units.

**It does not, and cannot.**  On the network path the per-station state vector is
a frozen LINEARIZATION POINT (written once at setup, never written back from the
solve), the per-epoch absolute solve carries **no Kalman prior on the local
nuisances**, and every row touching a GPS/Galileo iono column forms its RHS
against x_lin with that row's own A_r coefficient.  A seed shift δ therefore
gives y → y − A_r e_c δ, so u_s → u_s − N_sr e_c δ and u_r → u_r − N_rr e_c δ,
and

    b_red = u_s − N_sr N_rr⁻¹ u_r  →  b_red      EXACTLY

with x_r absorbing the shift, so the ABSOLUTE estimate x_lin + x_r is unchanged
too (invariance theorem, 2026-08-01).  **Measured confirmation, 2026-08-05:** with
the rx-CB carry OFF, arming `network.iono_seed_field_frame` reproduced the arm-14
collapse trajectory to within 1.8 TECU on a −37 TECU excursion, endpoints
matching to 0.006 TECU (arm 16 vs arm 14) — no level channel.

What the seed frame DOES reach is every consumer that reads a **prefit** quantity
in the x_lin frame, and those change row sets, which is a real actuator:

1. **The rx-CB carry's inject gate** — it tests |value_abs − x_lin(cb)| against
   10σ + 0.5 m, and `initializeCodeBiases` derives x_lin(cb) by subtracting the
   *seeded* iono, so the seed frame decides which stations' carried priors fold at
   all.  Measured (arm 15 vs arm 12, carry ON, single knob): folds 448 → 382,
   skips 710 → 776, and the ignition trough **6.06 → 13.81 TECU**.  The level
   moved by ~8 TECU — entirely through which carry priors were admitted.
2. The prefit census / fuse screens (trace-only unless `prefit_fuse_sigma` is
   armed).
3. The GLONASS local-nuisance anchors, whose rows pull toward x_lin with y = 0
   (GPS-only postures never build them).

**Contract consequence:** the emitted level is a function of the seed frame *only
through the gate*, and a gate that thresholds a frozen arbitrary linearization
point is a defect, not a design.  Grade LV-1 on gate statistics; own the level
with §2's authority and LV-2's de-mean.

**Update 2026-08-05 — that gate is now fixable** (`network.rx_cb_carry_step_gate`,
card LV-4, SPEC §11): the screen moved to the frame-invariant epoch-over-epoch step
of the ABSOLUTE posterior, judged against a robust peer-relative scale.  Measured
same-run A/B: the retired screen would have withheld **74 % of the folds** the
invariant one admits, while the invariant one erases ~1–2 % on genuine outliers.
Two contract-relevant consequences:

- **The seed frame stops reaching the level at all** once LV-4 is armed — LV-1's
  only actuator was that gate, so with it fixed the invariance theorem's conclusion
  becomes the whole story and the emitted level depends on §2's authority alone.
  That is the state this document wants.
- **The retired screen was silently acting as an information limiter.**  Admitting
  the full carry makes the level limit-cycle (a₀₀ settled sd 4.98 → 15.53 TECU over
  1 h) because LV-2's μ was applied lag-1 and the loop gain scales with admission.
  So the level's budget was accidental at a second level too.  **Card LV-5
  (`network.rx_cb_carry_demean_within`, SPEC §12) closes it**: μ is reduced between
  PASS A and PASS B and folded against the same epoch, and the settled sd goes
  15.53 → **4.69** with the level ending on the GIM reference (32.68 vs 32.24) and
  the residual common-mode force measured at **0.02 m against max |μ| 8.50 m**.

**~~Declared EXT-CB posture (2026-08-05): LV-1 + LV-2 + LV-4 + LV-5 together.~~
SUPERSEDED same day by the 12 h verdict (SPEC §14): the stack's level RAMPS to
+15…+21 TECU past hour 2 — the 1 h landing this declaration was written on was an
ignition transient.  The posture of record for settled work is the CANONICAL one
(no LV level stack; SPEC §14.4/§17), and the principled replacement is the
S-consistent harvest (SPEC §17 item 3 — built 2026-08-07 as
`network.rx_cb_harvest_demean`, card S2H).**  What survives of this paragraph:
the pairing note (LV-4 without LV-5 oscillates, LV-5 without LV-4 nearly inert)
and both cards' validity as fixes to the defects they name (§11/§12).

## 5. What a consumer should be told (and what it currently is not)

Today the emitted stream declares its satellite-bias signal pair (`cbpin=`,
`docs/SPEC_product_cbpin_contract.md`) but says **nothing** about the level
authority.  A consumer therefore cannot tell an a₀₀-anchored product from a
sum-to-zero-gauged one, and the two differ by the receiver-bias mean (~15 TECU on
this network).  The natural extension, when a card next opens the S-line grammar:

- `lvlauth=` — one of `satzero` (W6 sum-to-zero), `a00anchor:<mean>±<sigma>`,
  or `none`;
- `rxdcb=` — `zeromean:<n_stations>` when `rx_code_bias_datum` is armed, since
  the receiver SET is part of that frame.

Both are interpretation hints of the `cbpin=`/`agek=` class (never replay-emitter
construction parameters).  Not built; recorded here so the next product-format
card does not have to re-derive the requirement.

## 6. Optional, DEFERRED: an internal a₀₀ climatology from the broadcast stream

`network.a00_prior_from_brdc` (SPEC §5.2) would compute the a₀₀ target from the
Klobuchar coefficients already in the ingested navigation data — an
area-weighted global mean of the Klobuchar vertical delay, converted to TECU at
L1 — and feed it through the existing `mean_vtec_prior_*` channel at LOOSE σ
(10 TECU), giving a posture WITHOUT the GIM anchor an operationally
self-contained level authority.

**Prerequisite verified 2026-08-05:** the RINEX-nav ION ALPHA/BETA records ARE
retained — `ProductParser::parseKlobuchar` → `ProductCache::getKlobucharCoeffs()`,
with `Ionosphere::loadKlobucharCoeffs` as a direct-from-header fallback (both
normalize the Fortran `D` exponent, so 2024 DOY-129/131 files both parse).  No
parser work is part of this card.

**Deliberately not built** (the SPEC ranks it LAST, conditional on LV-1/LV-2):
the coefficients become available only after product load, so the target must be
applied as a RUNTIME override of `GlobalVtecShConfig::mean_vtec_prior_tecu`
rather than at config-load — a real design decision (who owns the override, how a
warm resume fingerprints it) that should not be made as a side effect of a
climatology helper.  Worth doing only if ignition speed still matters after LV-2,
and only for postures with no GIM anchor.

---

## 7. How to grade a level claim (tooling, 2026-08-05)

Every level verdict on this ladder up to now was quoted against **one number**,
"GIM 32.24 TECU" — the global mean of the day's 00:00 IONEX map.  That is fine for
an ignition arm covering minutes and **wrong for anything longer**: the global mean
VTEC itself moves through the day (measured on DOY-129: 32.0 TECU at 00 UT, 36.9 at
08 UT, 33.9 at 24 UT).  A run whose a₀₀ rises may simply be tracking it.

`scripts/gim_global_mean_series.py <run_dir> …` computes the area-weighted
(cos-latitude) global mean of every map in the day's IONEX, interpolates it in time,
and reports `a₀₀(t) − GIM_mean(t)` with hourly statistics.  That functional is the
right comparison: a sun-fixed SH expansion's a₀₀ **is** the global mean, because the
basis is orthonormal under (1/4π)∫dΩ so Ȳ₀₀ ≡ 1.

Caveats to quote with any number it produces: IONEX maps carry a few TECU of their
own error and their global mean leans on data-free ocean/southern interpolation; and
a₀₀ is a GLOBAL mean while the network sees one footprint, with the unsupported
hemisphere Kaula-extrapolated.  It is a level COMPARISON, not truth.

### Related

- `docs/SPEC_level_information_budget.md` — cards LV-1/LV-2/LV-3 (this is LV-3).
- `docs/HANDOFF_ext_cb_iono_level.md` — the measured ladder, arms 2–19.
- `docs/SPEC_product_cbpin_contract.md` — the signal-pair half of the frame contract.
- `docs/SPEC_rx_cb_carry.md` — the carry whose common-mode force LV-2 removes.
- `docs/SPEC_beidou_w6_datum.md` — the satellite-side gauge §2.1 leans on.
