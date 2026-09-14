# LV-2 — rx-CB carry common-mode de-mean — works as designed, but the de-mean is NOT frame-invariant

**STATUS**

- **id:** LV-2
- **topic:** ionosphere
- **status:** closed
- **summary:** rx-CB carry common-mode de-mean — works as designed, but the de-mean is NOT frame-invariant
- **dates in the text:** —
- **knobs / posture:** `network.rx_cb_carry_demean`
- **hashes named in the text** (commit / exe / byte-cert): —
- **run dirs named in the text:** —
- **source:** `docs/archive/SPEC_level_information_budget.md` — 4

> Results are appended TO THIS FILE, below the verbatim text. The archived monolith is frozen; never append there.

---
<!-- BEGIN VERBATIM: SPEC_level_information_budget.md -->
## 4. Card LV-2 — rx-CB carry common-mode de-mean

### Current behaviour (anchors)

*Line anchors drift; prefer the SYMBOL. The originals recorded here were
`ApplicationNetworkAssembly.cpp:2458-2540` (inject) and `NetworkRunContext.cpp:2171-2254` (harvest); both have
drifted — the live sites are given below.*

- Inject (parallel produce, per station): the Card LV-2 rx-CB inject block,
  `ApplicationNetworkAssembly.cpp:2767`, gate at `ApplicationNetworkAssembly.cpp:2805` — each carried rx-DCB
  folds a diagonal prior toward its harvested ABSOLUTE value; `rxCbInjectGate` (10σ + 0.5 m guard, `var_eff =
  var + q·Δt`, q = 1e-9 m²/s) may skip (counted `pl_cb_innov_skips`).
- Harvest (serial, post-solve): `networkHarvestLocalCarries` (`NetworkRunContext.cpp:2094`) —
  `rx_cb_carry[{station, key}] = LocalCarry{value_abs, var, now}`.
- Measured: at var ≈ 1e-2 m², the carried set puts ~**100 info units on the common mode** vs the datum's 0.01
  — the carry, not the datum, owns the frame, and it owns it with LAST epoch's realization (memory). Skip rate
  ~58 % in the EXT-CB arms.
- Arm 14 proves the DIFFERENCE rigidity is load-bearing (collapse without it). So: keep the carry, remove
  only its net common-mode force.

### Change

Lag-1 weighted de-mean, race-free within the existing produce/harvest split:

1. Produce: accumulate into the station's `StationAssembly R` two sums over its FOLDED rx-CB rows —
   `pl_cb_sum_lam_innov += λ·innov`, `pl_cb_sum_lam += λ` (keyed by constellation from day one).
2. Serial harvest: `μ_c = Σλ·innov / Σλ`; store in `ctx.carry` (`NetworkCarryState`).
3. Next epoch's inject: `η_i = λ_i·(innov_i − μ_c)`, and the gate judges `|innov_i − μ_c|`.
4. Λ is `1/var_eff` — the same quantity the fold uses.

By construction Σλ(innov−μ) = 0. ⚠ **§13 CORRECTION: that identity holds in DELTA coordinates only. In
absolute terms the de-meaned targets' weighted mean is `mean_λ(x_lin)` — the de-mean RE-PINS the common mode
to the frozen first-epoch linearization frame; it does not free it.**

**Do NOT implement the "demeaned target" variant** (pull every state toward mean-free absolute values): that
converts the carry into a STIFF zero-mean datum (~100 info) which would import the -5 ns convention offset
into the level at 400× the designed datum stiffness — the original DEFECT 2, hardened.

### Events (invariant #7)

- μ is lag-1: on the FIRST epoch inject un-demeaned (μ=0).
- After a mass re-seed / RAIM re-reduce epoch where few rows folded: if `Σλ` is below a floor (fewer than 8
  stations contributed), **freeze μ** at its previous value rather than trusting a thin mean. Counter
  `lv2_mu_frozen`.
- Trace μ_c per epoch on an EXISTING stream — do not create a new one for one scalar.

### Knob

`network.rx_cb_carry_demean` (loader default false). It changes carried-state meaning mid-chain, so append it
to the snapshot fingerprint **conditionally when armed** — the `rxcb:1` conditional-append pattern in
`networkSnapshotFingerprint`, live at `NetworkRunContext.cpp:593` (the original anchor
`NetworkRunContext.cpp:494-498` has drifted).

### Validation arms + predictions

1. `_ext63_gps_dual_20ep_a00gim_sl` ± demean (single knob): skip rate collapses from ~58 %. → **MISSED,
   §10.4.**
2. 1 h follow-up: breathing damps; carry fold fraction rises from 43 %.
3. LV-1 + LV-2 on the 1 h: the candidate to re-open the 305-station gate.

