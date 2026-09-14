# LV residual — the residual is not an extrapolation artifact, and `rx_cb_carry_q` is not simply too tight

**STATUS**

- **id:** LV residual
- **topic:** ionosphere
- **status:** closed
- **summary:** the residual is not an extrapolation artifact, and `rx_cb_carry_q` is not simply too tight
- **dates in the text:** 2026-08-05
- **knobs / posture:** —
- **hashes named in the text** (commit / exe / byte-cert): —
- **run dirs named in the text:** —
- **source:** `docs/archive/SPEC_level_information_budget.md` — 18, 18.1

> Results are appended TO THIS FILE, below the verbatim text. The archived monolith is frozen; never append there.

---
<!-- BEGIN VERBATIM: SPEC_level_information_budget.md -->
## 18. Is the residual an EXTRAPOLATION artifact?  No — measured (2026-08-05)

Fair challenge to §14/§15's metric: `a₀₀ − GIM_mean` compares two GLOBAL means, and ours is an SH field
constrained only where the network has pierce points. `scripts/field_vs_gim_footprint.py` settles it: it
synthesizes the estimated field from `logs/network_vtec_sh.csv` on a lat/lon grid, samples the
time-interpolated IONEX on the same grid, and reports the cos-latitude-weighted mean difference both globally
and restricted to a FOOTPRINT — grid points within `--radius` of any station. (At a 450 km shell the pierce
point sits ~13° from the station at el 20°, so 5–15° brackets "where we observe".) First correction to a §14
assumption: **these 61 stations are globally distributed**, not Europe-weighted — lat −78…+79, lon −170…+175,
8/13/25/15 across the four longitude quadrants; a 15° footprint already covers 69 % of the sphere, 5° covers
9.8 %.

12 h means, DOY-129 (obs 10 / iono 25):

| arm | footprint (5°) | global | fp rms | gl rms |
|---|---|---|---|---|
| 24 canonical | **-9.7** | -6.1 | 11.2 | 6.9 |
| 25 LV-1+2+4+5 | **+22.6** | +11.2 | 27.3 | 15.1 |
| 29 LV stack, q=1e-6 | **+27.4** | +15.4 | 31.9 | 18.5 |

**In every arm the disagreement is ~2× LARGER over the footprint than globally.** So `a₀₀ − GIM_mean` was not
measuring extrapolation — if anything it UNDER-reported, because the field's error is concentrated where the
data is. The §14/§15 ranking stands and is strengthened: the LV stack's footprint error (+22.6) is 2.3× the
canonical's (-9.7), its pointwise rms 2.4×, so the shape degrades too, and the hourly shape is the same ramp
(-7.9 at h0 → +20.0 h3 → +36.4 h4 → +33.1 h11). ⚠ **§26.1 shows this footprint/global structure is an
obs-mask-10 artifact at scale** — at obs 15 the inversion flips.

### 18.1 And is `rx_cb_carry_q` simply too tight?

The q axis is exhausted and is not the discriminator: **tighter is meaningless** (at q = 1e-9 m²/s the
inflation over a 30 s epoch is 3e-8 m² against a ~1e-2 m² posterior — already a random CONSTANT); **looser is
worse**, measured (q = 1e-6, τ ≈ 2.8 h, moved the 12 h footprint residual +22.6 → +27.4 and brought the
plateau forward two hours, §15); **much looser is catastrophic** (the carry-OFF limit is arm 14's -9
TECU/epoch collapse); **and the canonical posture runs the SAME q = 1e-9** with a -9.7 footprint residual —
same q, 32 TECU apart. So q moves the answer by ~5 TECU while the de-mean + full admission move it by ~32
TECU: q is second-order, and what the level responds to is WHAT KIND OF MEMORY holds the receiver-DCB common
mode (§15.1), not how fast it decays.

---

