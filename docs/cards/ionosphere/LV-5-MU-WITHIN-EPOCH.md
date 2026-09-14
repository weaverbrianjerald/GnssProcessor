# LV-5 — μ computed within the epoch, with the review addendum's requirements

**STATUS**

- **id:** LV-5
- **topic:** ionosphere
- **status:** closed
- **summary:** μ computed within the epoch, with the review addendum's requirements
- **dates in the text:** 2026-08-05
- **knobs / posture:** `network.rx_cb_carry_demean_within`
- **hashes named in the text** (commit / exe / byte-cert): —
- **run dirs named in the text:** —
- **source:** `docs/archive/SPEC_level_information_budget.md` — 12, 12.1, 12.2, 12.3, 12.4, 12.5, 16

> Results are appended TO THIS FILE, below the verbatim text. The archived monolith is frozen; never append there.

---
<!-- BEGIN VERBATIM: SPEC_level_information_budget.md -->
## 12. Card LV-5 — μ computed within the epoch (2026-08-05)

`network.rx_cb_carry_demean_within`, default OFF, fingerprinted when armed. Requires `rx_cb_carry_demean` —
with the de-mean off **the loader forces LV-5 off** — the LV-5 arming block at
`src/core/config/GnssConfigLoadNetwork.cpp:7510-7516` sets `network_rx_cb_carry_demean_within_ = false` when
`rx_cb_carry_demean` is off, and the sibling `if (network_rx_cb_carry_demean_ ||
network_rx_cb_carry_demean_within_)` guard is at `GnssConfigLoadNetwork.cpp:7433-7443`.  (Two anchors this doc
carried for it, `GnssConfigLoadNetwork.cpp:6630` and `GnssConfigLoadNetwork.cpp:7338-7348`, have drifted — the
BEHAVIOUR is unchanged; prefer the symbol.) That forcing is what makes a
config setting only `rx_cb_carry_step_gate` genuinely the LV-4-alone posture (§19, §23).

### 12.1 The defect

LV-2 reduced μ at the serial post-solve harvest and applied it to the NEXT epoch's folds. That lag is a
feedback delay whose loop gain scales with how much carry information folds — harmless at 33 % admission (arm
19, settled sd 4.98 TECU), a ~3-epoch limit cycle at 97.5 % (arm 21, settled sd 15.53, range 54.1). **The lag
was never necessary:** both terms of the carry innovation — the stashed ABSOLUTE posterior and the FROZEN
`x_lin(cb)` — are known before PASS B.

### 12.2 The change

A new serial seam, `networkRxCbDemeanPrepass`, called from the driver **between PASS A and PASS B**
(`ApplicationNetwork.cpp`, right after `networkSdEwlAr`). It walks the stations PASS A marked present, forms
`innov = value_abs − x_lin(cb)` and `λ = 1/(var + q·Δt)` for every carried entry inside the gap horizon, and
reduces them with the same `rxCbDemeanMu` helper, thin-mean floor and freeze semantics as LV-2 — only the
epoch it is applied to differs. PASS B reads `ctx.carry.rx_cb_demean_mu` exactly as before: one serial writer,
parallel readers. The post-solve harvest still reduces the FOLDED set's mean, now purely as a **diagnostic**:
the folded set is marginally smaller than the prepass candidate set (a column can drop out of the local
block), so the difference between the two is the RESIDUAL common-mode force the correction left behind — the
only honest way to tell a working de-mean from one that merely ran, and what the run-end line reports.

### 12.3 Measured

| arm | window | settled mean | settled sd | settled range | end | folds |
|---|---|---|---|---|---|---|
| 13 canonical | 1 h | 17.45 | 3.71 | 12.2 | 20.95 | 43.5 % |
| 19 +LV-1+LV-2 | 1 h | 37.43 | 4.98 | 14.3 | 33.08 | 51.9 % |
| 21 +LV-4 | 1 h | 36.93 | **15.53** | **54.1** | 46.15 | 97.5 % |
| **23 +LV-5** | 1 h | **35.24** | **4.69** | **13.9** | **32.68** | 97.5 % |

Against GIM 32.24 TECU. The limit cycle is gone (sd below even arm 19's, with 1.9× the carry information
admitted), the level ends **on the reference**, and:

- **Residual common mode: mean |resid| 0.0208 m against max |μ| 8.50 m** — the within-epoch correction nulls
  the frame force to ~0.2 % of its size. (12 h: 0.0235 m — it does NOT grow with the window.)
- Fold count unchanged by LV-5 (7182/7365, as arm 21) — it changes WHEN μ is computed, not which rows fold,
  exactly as predicted. 124 LV-4 erases, 0 thin-mean stand-downs, in-band 121/121, gate 0/0/0.
- **Arm 22 (`_ext63_gps_dual_20ep_lv125`, the 20-epoch wiring check, ignition window):** a₀₀ sd 13.06 →
  **5.99**, folds unchanged (1147 vs 1148), 12 erases, mean |resid| 0.0057 m against the same max |μ| 8.50 m.
  20 ep is a wiring check, not a level grade.

### 12.4 Predictions that missed

- **"max |μ| falls back toward ~2 m."** It did not: 8.50 m, essentially arm 21's 10.15. μ is genuinely that
  large once the full carry folds — it is draining a real common mode every epoch, not chasing its own tail.
  The lag removal fixed the correction's PHASE, not its size.

### 12.5 Recommended posture

**LV-1 + LV-2 + LV-4 + LV-5 together.** LV-4 without LV-5 is unstable; LV-5 without LV-4 is nearly inert. All
four remain loader-default OFF. ⚠ **SUPERSEDED by §14.4** on the 12 h window — do NOT arm the stack for
settled work. The pairing note (LV-4 needs LV-5; LV-5 alone is inert) survives.

---

## 16. LV-5 requirements (review addendum, 2026-08-05 — was mis-numbered §12; kept for the record)

*Subsection numbers below are the original 12.1–12.5 and are deliberately NOT renumbered.*

### 12.1 The μ vote MUST be robust — LV-5 opens a contamination window LV-4 cannot close

LV-4's erase runs at the SERIAL HARVEST, **after** injection; LV-5 moves the μ vote to **before** the solve,
so a station that steps THIS epoch votes its stepped innovation into μ before anything can catch it.
Magnitudes: the field has produced **~200 km worst-case innovations** at cold start (the cold_clock_seed
class); one such voter among 61 Λ-equal stations kicks μ by ~3.3 km, and the demeaned η then injects a ~3 km
COMMON-MODE error into every station's prior at stiffness Λ for one epoch. Even an ordinary 10 m receiver
reset pollutes μ by ~0.16 m — the size of the signal the trace watches. Requirement: screen the voters with
the SAME robust peer-relative idiom §11.2 validated —

```
z_i = |innov_i − median_i(innov)| / max(1.4826·MAD_i, formal σ floor)
z_i > k  ⇒  excluded from μ (STILL folds; erase remains harvest's job)
```

μ = Λ-weighted mean over SURVIVORS, with a counter for excluded voters (invariant #4); below the min_stations
floor of SURVIVORS fall back to last epoch's μ (one lag-1 epoch is benign) and count it. The upstream defenses
do NOT cover this window: `cold_clock_seed` sanitizes x_lin and the admission screen's station-median centring
cancels receiver-clock commons on the DATA path — neither sees the carry's vote. (Implemented as the S2H peer
screen, §28.)

### 12.2 Ordering + consistency invariants

- **No station-state mutation between the μ sweep and the inject reads** (slip refloats, re-allocations,
  re-seeds all upstream). Assert/comment at the seam.
- **One shared eligibility predicate** for "will this row fold", used by BOTH the sweep and PASS B — any
  drift breaks Σλ(innov−μ)=0 silently. Run-end counter: the realized Σλ(innov−μ) over actually-injected rows,
  WARN if not ≈ 0.
- Serial fixed-order reduce (61 adds); per-station partials into pre-sized slots.
- μ stops being carried state: drop the stored field, update the conditional fingerprint token.

### 12.3 Theory conformance (answers "is this ad hoc" — it is not)

The de-mean is **NGS LASER slide 5-86's S-transformation** (S = I_m − Eᵀ(KEᵀ)⁻¹K), rank-1 case: family
null-space basis E = 𝟙 (the common mode), constraint K = λᵀ/Σλ ⇒ S = I − 𝟙kᵀ ⇒ (Sv)_i = v_i − μ. Carried
priors are 5-74 prior-information rows realized in a PREVIOUS adjustment's S-system; folding them
untransformed smuggles a second, inconsistent datum into the epoch — that is the flywheel, named. LV-4 is the
datum-invariance theorem applied to testing (the old gate thresholded an S-DEPENDENT quantity; the step
statistic is estimable). This card is the first landed slice of the mapping's top open item
(`SPEC_pivot_visibility_gauge_defect.md §8`).

**Known, accepted deviation**: 5-86 transforms mean AND covariance; the exact covariance transform is Λ → Λ −
λλᵀ/Σλ, a dense cross-station pre-reduce down-date — the architecturally BLOCKED shape (same reason the
post-reduce SumZeroGauge died). ~~The implemented mean-only transform zeroes the net common-mode FORCE but
keeps the common-mode STIFFNESS ≈ Σλ, i.e. a VISCOSITY that rate-limits frame motion~~ **SUPERSEDED BY §13
(that characterization was WRONG): the demeaned targets' weighted mean is mean_λ(x_lin) — the FROZEN
first-epoch frame — so the mean-only transform is a position PIN to the seed frame at stiffness Σλ, not a
viscosity. §14 measures the consequence (the 12 h ramp to +20) and §15 falsifies the q-tracking patch: under
the de-mean the target is frozen BY CONSTRUCTION, so no q can make it track.** The closing sentence stands and
is now load-bearing: freeing the direction requires the common mode out of the carry's INFORMATION — the 5-86
transform at the REDUCED level (the handover S-transform card), not a pre-reduce dense prior and not a q
tweak. (§28's S2H takes the third route: transform the harvested REALIZATION post-solve — O(n), no
cross-station prior rows.)

### 12.4 Cold start / born-wrong-static (the residual exposure, named)

The carry map is empty at epoch 1, so cold-start monsters cannot enter the LV loop directly; but a
contaminated FIRST HARVEST freezes a born-wrong value that never steps, and LV-4 is structurally blind to it
(steps only). The designed detector is the prires census `rxcb` family (4σ̂₀); a cheap hardening is a
first-harvest holdoff riding `warmup_epochs`. Measure first — if the census shows the class is empty on this
lineage, the holdoff stays unbuilt.

### 12.5 Grades for the LV-4+LV-5 validation arms (write into the configs)

Arm-21 posture + LV-5: settled sd 15.53 → **≤ 5**, no ~3-epoch alternation, max |μ| back to ~2 m and damping;
admission stays ~97.5 %, erases ~1-2 % (LV-5 changes the net force, never who folds); excluded-voter counter ≈
0. Then re-run the ± LV-1 attribution pair under LV-4+LV-5 (predict: LV-1 now exactly inert — its only
actuator was the retired screen), re-measure the 1 h headline (35.09/37.19 are stale, taken under censoring or
oscillation), check the 01:00 GIM area mean before interpreting any overshoot, and read deg-1 (predict end <
29). → Scored in §12.3/§12.4 and §13.1: **the sd gate PASSED, max |μ| MISSED, and "LV-1 now inert" MISSED** —
which is what produced §13.

---

