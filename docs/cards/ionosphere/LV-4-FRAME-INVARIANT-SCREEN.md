# LV-4 — the carry screen made frame-invariant — fixed, and it exposed a second defect

**STATUS**

- **id:** LV-4
- **topic:** ionosphere
- **status:** closed
- **summary:** the carry screen made frame-invariant — fixed, and it exposed a second defect
- **dates in the text:** 2026-08-05
- **knobs / posture:** `network.rx_cb_carry_step_gate`
- **hashes named in the text** (commit / exe / byte-cert): —
- **run dirs named in the text:** —
- **source:** `docs/archive/SPEC_level_information_budget.md` — 11, 11.1, 11.2, 11.3, 11.4

> Results are appended TO THIS FILE, below the verbatim text. The archived monolith is frozen; never append there.

---
<!-- BEGIN VERBATIM: SPEC_level_information_budget.md -->
## 11. Card LV-4 — the carry screen, made frame-invariant (2026-08-05)

`network.rx_cb_carry_step_gate` (+ `_k_sigma`, default 10.0), default OFF, fingerprinted when armed. Requires
`carry_rx_code_bias`.

### 11.1 The defect

The inject gate tested `|value_abs − x_lin(cb)|` at 10σ + 0.5 m. Both terms are absolute, but `x_lin` is
FROZEN at its first-epoch seed and the network solve is never written back, so the statistic is just the
standing correction `x_r` — a property of where the seed landed. Measured consequences: **48–67 % of ALL carry
priors withheld for whole runs** with no receiver event anywhere (arm 13: 4127/7304; arm 19: 3511/7304); and
it is structurally BLIND to the jump it exists for — a step between epochs t−1 and t leaves the carried value
pre-jump and `x_lin` unmoved, so at t's inject the statistic contains no information about the event.

### 11.2 The fix

The fold-time gate keeps only its frame-free checks (gap, variance sanity). The event response moves to the
SERIAL post-solve harvest — the one place where two ABSOLUTE realizations of the same bias exist side by side
(the fresh posterior `x_lin + x_r` and the carried value), so their difference is invariant under any
reframing, reseeding or datum change. Screen, per code-bias family, per epoch:

```
e_i  = value_abs_new,i − value_abs_prev,i          (invariant)
σ̂    = max( 1.4826·MAD_i(e_i) , formal σ floor )   (robust, this epoch's own scale)
z_i  = |e_i − median_i(e_i)| / σ̂                   (peer-relative)
z_i > k  ⇒  ERASE entry i                          (reset-not-repair)
```

Three choices, each paid for. **Robust, not formal**: a first cut standardized `e` by its formal variance (σ ≈
0.52 m) and erased **432 of 777 entries in 20 epochs**, leaving 10 of 61 stations carrying at run end with the
level unstable — at ignition the confounded direction genuinely re-realizes by metres per epoch, so the formal
variance understates the true step scatter by 2–3 orders and any formal-σ test is a guaranteed mass-trigger;
the MAD scale is the RAIM σ̂₀ idiom, floored at the formal σ so a freakishly quiet epoch cannot make the
screen hypersensitive. **Median removed**: a step common to the whole network is a FRAME MOVE, not N
simultaneous receiver resets — the same distinction LV-2 draws for the fold. **Erase, not overwrite**: a
genuine jump means this epoch's solve folded a stale pre-jump prior, so its posterior is suspect too; one
epoch cold, then the next harvest re-seeds from data — a first sighting's lifecycle. Below
`rx_cb_carry_demean_min_stations` entries the screen STANDS DOWN (counted): a thin epoch must not define what
"normal" is (invariant #7).

### 11.3 Measured — the screen is fixed, and it exposes a second defect

Arm 20 (`_ext63_gps_dual_20ep_lv12_ig`, 20 ep) and arm 21 (`_ext63_gps_dual_1h_lv12_ig`, 1 h), both = arm
18/19 + this knob:

| | folds / harvests | erases | retired screen would have withheld |
|---|---|---|---|
| arm 20 (20 ep) | **1148 / 1219** (arm 18: 381) | 11 (0.96 %) | **852 of 1148 (74.2 %)** |
| arm 21 (1 h) | **7182 / 7365** (arm 19: 3793) | 123 (1.7 %) | ~74 % |

The same-run A/B is the finding: **three quarters of the withheld population was a frame artifact.** Max
robust σ̂ 1.70 m (so "normal" epoch-to-epoch re-realization really is metre-class), max z 744 (real outliers
exist and are found). Arm 20's own 20-epoch readings: 60/61 stations still carrying at run end, max z 111, a₀₀
trough 19.55 and mean 39.23 but noisy (19.5–61.6) **inside ignition, so not quotable as a level** (§14).

**But arming it destabilizes the level, and that is the second defect, not a regression:**

| arm | 1 h a₀₀ mean | settled sd (20–60 min) | settled range |
|---|---|---|---|
| 13 (base) | 16.36 | 3.71 | 12.2 |
| 19 (LV-1+LV-2) | 35.09 | **4.98** | 14.3 |
| 21 (+LV-4) | 37.19 | **15.53** | **54.1** |

Arm 21's a₀₀ limit-cycles between ~15 and ~55 TECU on a ~3-epoch period, max |μ| rising 1.96 → 10.15 m.
Mechanism: LV-2's μ is applied **lag-1** and the loop gain is proportional to how much carry information folds
— at 33 % admission (arm 19) the correction is stable, at 97.5 % it oscillates: textbook lag-1 feedback on a
stiff loop. **So the broken screen was accidentally acting as the loop-gain limiter** — the same disease this
whole spec is about, one level down.

### 11.4 Verdict and the next card

- LV-4 is **correct and closes the screen defect** — arm it whenever you want the carry's information
  actually admitted, and read the same-run A/B line.
- **Do NOT arm it without LV-5**: at full admission the lag-1 de-mean oscillates.
- **Next card (LV-5): make μ WITHIN-EPOCH.** → BUILT, §12; arm 21's settled sd 15.53 → 4.69.
- ⚠ §19.3/§23.3 add the cost side this verdict predates: LV-4 costs **+64 / +35 / +16 % postfit phase rms**
  (mask-dependent) and is **not a promotion candidate**.

---

