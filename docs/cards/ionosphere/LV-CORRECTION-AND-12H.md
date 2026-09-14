# LV correction — what LV-2 actually does and what LV-1 is really for — and the 12 h verdict that made the 1 h result an ignition transient

**STATUS**

- **id:** LV correction
- **topic:** ionosphere
- **status:** closed
- **summary:** what LV-2 actually does and what LV-1 is really for — and the 12 h verdict that made the 1 h result an ignition transient
- **dates in the text:** 2026-08-05
- **knobs / posture:** —
- **hashes named in the text** (commit / exe / byte-cert): —
- **run dirs named in the text:** —
- **source:** `docs/archive/SPEC_level_information_budget.md` — 13, 13.1, 13.2, 13.3, 13.4, 13.5, 13.6, 14, 14.1, 14.2, 14.3, 14.4

> Results are appended TO THIS FILE, below the verbatim text. The archived monolith is frozen; never append there.

---
<!-- BEGIN VERBATIM: SPEC_level_information_budget.md -->
## 13. CORRECTION — what LV-2 actually does, and what LV-1 is really for (2026-08-05)

This section supersedes the INTERPRETATION in §4 and §10 (not their arithmetic). It was forced by an arm that
was supposed to be a formality.

### 13.1 The measurement that broke the story

With LV-4 armed, LV-1's only known actuator (the frame-dependent inject gate) is retired, so LV-1 should have
become INERT. Arm 27 (`_ext63_gps_dual_1h_lv245`, LV-2+4+5, LV-1 OFF) was run to confirm that — it was
predicted to reproduce arm 23. It did not:

| 1 h arm | settled mean | settled sd | end | step checks | erases |
|---|---|---|---|---|---|
| 23 LV-1+2+4+5 | **35.24** | 4.69 | **32.68** | 7182 | 124 |
| 27 LV-2+4+5 | **26.45** | 3.41 | **25.11** | 7180 | 125 |

A flat **-9 to -12 TECU** offset from epoch 10 onward, with the **row sets identical** (7182 vs 7180 checks,
124 vs 125 erases). So LV-1 moves the level by ~10 TECU without changing which rows fold — a channel nobody
had named.

### 13.2 The channel is LV-2 itself: the de-mean is NOT frame-invariant

The de-meaned prior on station *i* is `Δx_i ~ N(innov_i − μ, 1/λ_i)`, i.e. `x_abs,i ~ N(value_abs,i − μ,
1/λ_i)` with `μ = Σλ·innov / Σλ`; since `innov_i = value_abs,i − x_lin,i`, the information-weighted mean of
those targets is

    mean_λ(value_abs) − μ  =  mean_λ(value_abs) − [mean_λ(value_abs) − mean_λ(x_lin)]
                           =  **mean_λ(x_lin)**

**The de-mean does not free the common mode — it re-pins it to the mean of the frozen first-epoch
linearization points, with the carry's full Σλ ≈ 100 units.** It only ever moved the fold's RHS; the
information along the all-ones direction is untouched by construction. What LV-2 genuinely removes is the
FEEDBACK (last epoch's realization) — which is why the lag mattered and why LV-5 fixed the limit cycle. What
it does not remove is the memory: a rolling memory is swapped for a frozen one. `x_lin(cb)` is seeded once by
`StateInitializer::initializeCodeBiases`, which subtracts the *seeded iono* — so LV-1 sets it. That is the
whole channel, and it predicts the measured sign and size.

**Attribution pair (arms 28a/28b, `_ext63_gps_dual_1h_lv45{_lv1,}.json`): LV-4+LV-5 armed, de-mean OFF, LV-1
flipped.**

| configuration | LV-1 ON | LV-1 OFF | LV-1's effect |
|---|---|---|---|
| with LV-2 (arms 23/27) | 35.24 | 26.45 | **−8.79 TECU** |
| without LV-2 (arms 28a/28b) | 4.44 | 2.69 | **-1.76 TECU** |

(settled 20–60 min means; the arm-28 pair also reads mean −1.97, max 3.35 across the run.) ~80 % of LV-1's
effect vanishes with the de-mean off, and the remainder sits at the pure-FP amplification scale already
measured for a near-null level direction in a collapsed state (arms 14 vs 16: 1.8 TECU on -37). Note both
no-de-mean arms sit at 3–5 TECU: with the full carry admitted and nothing draining its common mode, the
flywheel owns the level outright — the control this pair needed, not a posture to use.

### 13.3 So what IS LV-1 for?

Not "bias-free per-LOS iono seeding" (a no-op, §10.1). Its real function is:

> **LV-1 puts the physically correct value into the frame that LV-2 pins.**

Without LV-1 the receiver's GF bias rides in the iono seed, so `initializeCodeBiases` books ≈ 0 into the DCB
and the de-meaned carry pins the DCB common mode at ≈ 0 — reproducing exactly the zero-mean convention that IS
DEFECT 2. With LV-1 the bias lands in the DCB seed where it belongs (network mean ≈ −1.55 m ≈ -15 slant-TECU,
measured estimator-free), the carry pins THAT, and the level rises by the same ~11–15 TECU the defect was
short: 26.45 → 35.24 settled, ending 32.68 against GIM's 32.24. The level lands on the reference for a *sound*
reason, but through a mechanism nobody designed, and one that is still a **frozen** frame: it cannot track
slow DCB drift and it inherits whatever epoch 1 happened to see.

### 13.4 What a principled version would need

To leave the common mode genuinely free, the carry's INFORMATION along that direction must be removed, not its
RHS — a rank-(n−1) cross-station fold. The per-station diagonal channel cannot express that (each station is a
separate local block), and the post-reduce `SumZeroGauge` route is structurally blocked against the Card-4 DD
priors. Two honest options: (1) **own it deliberately** — keep the pin, but at a per-epoch DATA-derived
estimate of the receiver-DCB mean rather than epoch 1's, promoting §13.3's accident into a designed authority
with a stated estimator and an event response; (2) **deweight the direction** — scale every carry fold's λ so
Σλ on the common mode is comparable to the datum's 0.01, keeping differences stiff by folding them in a
differenced parameterization, which needs a channel that does not exist today. → **§17 item 3 / §28 (card S2H)
is the built answer on the fold side**; option 1 remains the answer if the receiver-physical attribution ever
matters. Until then the level's owner is the epoch-1 DCB seed frame, and the run-end lines say so.

### 13.5 What these arms did NOT measure — the over-retention question is OPEN

Every level number above is `network_vtec_sh_health.csv`'s `a00_tecu`, the right instrument for the level. The
DATA-side question — do the newly admitted carry priors buy their information honestly, or is this
over-retention (quieter priors, louder observation residuals — the lane A/B signature)? — is **not answered**
and cannot be from these runs: the same file's `slant_resid_*` columns are FRAME-BROKEN by construction (they
compute `x_r − sh_slant` and omit `x_lin`, memory `project_network_xlin_invariance`) and LV-1 deliberately
moves `x_lin`, so their apparent 26.3 → 34.2 m rise is **uninterpretable**; `network_block_baarda.csv` is
effectively INERT here (σ̂₀ pinned at its 1.0 floor for 13 of 13 rows in three of the four arms); and the
prires census and postfit-σ̂₀ traces are not armed in `_ext63_gps_dual_1h.json`. **Before promoting this
posture anywhere, arm `trace_postfit_sigma0` + `trace_postfit_keyed` + the prires feeds and re-run the 1 h
pair.** → Discharged in §19.2 **with the opposite sign to the one it was written to detect**, re-confirmed in
§23.4/§24.3.

### 13.6 Status of the claims in this document (see also §14)

- §4 / §10.3 "the carry exerts ZERO net force on the common mode, so the designed authority owns the frame" —
  **WRONG as stated**; corrected above. The identity Σλ(innov−μ)=0 holds, but in delta coordinates.
- §10.1 (LV-1's seed-frame gradient is a no-op) — **stands**, and §13.3 is *why* LV-1 still matters.
- §11 (LV-4) and §12 (LV-5) — **stand** as fixes; their cost is §19.3/§23.3.
- Code, loader banner and run-end lines were corrected to match this section.

---

## 14. THE 12 h VERDICT — the 1 h result was an ignition transient (2026-08-05)

### 14.1 The grading changed first

Every level number in §10–§13 is quoted against **one** figure, "GIM 32.24 TECU" — the global mean of the
day's 00:00 IONEX map. Fine for arms covering minutes and **wrong for a 12 h arm**: the global mean VTEC
itself moves through DOY-129 (**32.0 TECU at 00 UT → 36.9 at 08 UT → 33.9 at 24 UT**), so a rising a₀₀ may
just be tracking it. New tool, `python scripts/gim_global_mean_series.py <run_dir> ...`: it area-weights each
IONEX map to a global mean, interpolates in time, and reports `a₀₀(t) − GIM_mean(t)` hourly. That functional
is the right comparison — a sun-fixed SH expansion's a₀₀ **is** the global mean (Ȳ₀₀ ≡ 1 under the orthonormal
convention). All numbers below use it; caveats to quote with any of them are §30.7.

### 14.2 What the pair says

`a₀₀ − GIM_mean`, hourly means, 61-station EXT-CB, DOY-129, obs mask 10 / iono cutoff 25 (the denominator §21
later voided — the RANKING is unaffected, the absolute values are not comparable across the mask change):

| hour | 0 | 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 | 9 | 10 | 11 | run mean | sd |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| canonical (arm 24) | **−15.8** | −8.2 | −3.9 | -3.8 | -9.2 | −8.1 | -13.2 | -8.2 | -7.2 | -14.1 | -9.8 | **+1.0** | **−8.38** | 5.53 |
| LV-1+2+4+5 (arm 25) | **+4.2** | +4.8 | +4.9 | +13.2 | +13.9 | +20.8 | +17.9 | +15.0 | +16.5 | +16.6 | +18.4 | **+20.2** | **+13.85** | 6.21 |

Both complete (1440 epochs each), same window, same cold start, the four LV knobs the only difference.

**The canonical posture's DEFECT-2 deficit is about HALF an ignition transient.** It starts -15.8 (what every
20-epoch and 1 h arm on this ladder measured), and from hour 2 it neither converges nor walks: it WANDERS
between -3.8 and -14.1 about a **−8.4 mean**, final hour **+1.0** — on the reference. So DEFECT 2 is real but
roughly half the size the ignition arms reported, and its sign is not even constant over a day. **The LV stack
does something different and worse**: it starts +4 — the best ignition level any arm on this ladder has
produced — then ramps UP to +15…+21 and stays there (37 → 50 TECU across hour 3, smooth, no step, station and
LOS counts flat), mean **+13.85**. By |mean error| the canonical is better by 1.7×, by final-hour error by
20×.

Carry mechanics were healthy throughout: LV-5's residual did NOT grow with the window (0.0208 m at 1 h →
0.0235 m at 12 h); LV-4 195 erases / 87423 checks (0.22 %), 0 stand-downs, folds 87423/87679 (99.7 %). The
cards do exactly what they claim. So the one-hour result that made the stack look like a fix was measuring the
thing the stack genuinely improves — **ignition** — and the settled window reverses the ranking. **The level
channel's own convergence budget is ~2 h** (memory `project_agu26_abstract_campaign`), and every LV grading
before this one sat inside it.

### 14.3 Why the drift is the expected consequence of §13

§13 established that the de-meaned carry does not free the receiver-DCB common mode — it PINS it to
`mean_λ(x_lin)`, the frozen first-epoch code-bias seed frame, with Σλ ≈ 100 units. A frozen pin is exactly
right at ignition (LV-1 makes it the physically correct value, hence +4 at hour 0) and exactly wrong later: as
the real field and the DCB/level split evolve, the pin cannot follow and the level moves to satisfy it. The
canonical posture has no such pin — its carry folds only ~43 % of the time and un-demeaned — so its level is
free to re-realize each epoch, and it converges. The mechanism and the measurement agree, which is the only
reason to believe either.

### 14.4 Recommendation (supersedes §12.5)

- **Do NOT arm the LV stack for settled/production work on this lineage.** Ignition is better; hours 3+ are
  worse by 10–25 TECU against the time-matched GIM.
- **LV-4 and LV-5 remain correct fixes to real defects** (a screen that thresholded a frozen frame; a lag-1
  loop that limit-cycled). They are not the problem — the problem is what they expose: a stiff carry whose
  common-mode pin is frozen.
- **The next card is §13.4 option 1, now with a measured motivation**: give the receiver-DCB common mode a
  per-epoch, data-derived authority so the pin TRACKS. Grade it on this same 12 h pair; the target is **|a₀₀ −
  GIM(t)| ≲ 4 TECU at hour 0 AND at hour 11**, which no posture achieves today.
- **Re-grade the self-contained posture the same way before drawing any conclusion there.** Arm 26 covers 15
  minutes — pure ignition (its measured block is §17 item 4).

---

