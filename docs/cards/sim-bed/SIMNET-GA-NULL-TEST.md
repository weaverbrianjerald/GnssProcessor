# G-A: the NULL test, the first absolute grade

**STATUS**

- **id:** G-A
- **topic:** sim-bed
- **status:** closed
- **summary:** The first absolute grade: the n = 3 network is estimable but not determinable (ZWD +4.31 m systematic, +0.029 m at n = 5), and every internal instrument called it clean.
- **dates in the text:** 2026-08-11
- **knobs / posture:** INT posture (GPS-only, quiet, `storm_scale` 0), 240 epochs at 30 s, graded on the settled half; fixtures `config/sim/simnet3_ga.json` / `_simnet5_ga.json`, producer arms `config/test_suite/_simnet3_ga.json` / `_simnet5_ga.json`; bisect arm `_simnet3_bare` (correction stack off)
- **hashes named in the text** (commit / exe / byte-cert): none (exe `gnss_processor_simnet.exe`, no hash)
- **run dirs named in the text:** none
- **source:** `docs/archive/SPEC_synthetic_minimum_network.md` §9.5

**Outcome.** The question: how far the network producer sits from truth on a quiet 2 h sim arc at n = 3 and n = 5. Clock vf is 768.2 and 392.3; over the n = 3 arc the median URA falls from 6.63 to 0.176 m while the median clock error grows to 3.38 m. A bisect arm with the correction stack off reproduces the result to three decimals. Measured later: the n = 50 trend and how to read a clock vf ([G-A 50-sta](../sim-bed/SIMNET-50STA-QC-TIERS.md) §9.7.1), and the code-bias vf, about 4.9e+05 ([W6+SH arm](../sim-bed/SIMNET-W6SH-ARM.md) §12.7).

> Results are appended TO THIS FILE, below the verbatim text. The archived monolith is frozen; never append there.

---
<!-- BEGIN VERBATIM: SPEC_synthetic_minimum_network.md -->
## §9.5 G-A RESULTS — measured 2026-08-11 (the first absolute grade)

Arms: `config/sim/simnet3_ga.json` / `_simnet5_ga.json` → `synth-network` →
`config/test_suite/_simnet3_ga.json` / `_simnet5_ga.json`, Release
(`gnss_processor_simnet.exe`), 240 epochs = 2 h @ 30 s, GPS-only, INT posture, quiet
(`storm_scale` 0), graded by `scripts/simnet_grade.py` over the settled second half.
Producer: 240/240 epochs emitted, **0 skipped, 0 withheld**, rank gate passed every epoch,
`declared_dimension` = 1 as predicted (§5).

| channel | statistic | **n = 3** | **n = 5** |
|---|---|---|---|
| C (sat clock) | residual rms | 5.517 m | **2.183 m** |
| C | residual mean | +0.651 m | −1.318 m |
| C | contrast rms (datum-free) | 7.980 m | 2.494 m |
| C | **vf** | **768.2** | **392.3** |
| P (phase bias) | SD frac, L1C rms | 0.213 cyc | 0.350 cyc |
| P | SD frac, L2W rms | 0.213 cyc | 0.346 cyc |
| ZWD | residual **mean** | **+4.307 m** | **+0.029 m** |
| ZWD | residual rms | 4.558 m | 1.056 m |
| ZWD | **vf** | **87.5** | **6.5** |
| consumer | 3D rms | 3.645 m | 3.098 m |
| consumer | UP mean | +3.495 m | −2.701 m |

### The finding

**1. The n = 3 network is estimable but not determinable, and nothing internal says so.**
ZWD carries a **+4.31 m systematic** against a 0.12 m truth, dragging the consumer's height
(+3.50 m of the 3.55 m error is UP).  Two more stations remove the bias entirely (+0.029 m)
⇒ a determinability deficiency of the minimum network, **not** a modeling error.

**2. The formal σ collapses while the true error grows.**  Median URA vs median |C − truth|
over the n = 3 arc:

| epoch | 0 | 1 | 5 | 20 | 60 | 120 | 239 |
|---|---|---|---|---|---|---|---|
| median URA | 6.63 m | 2.34 m | 0.343 m | 0.242 m | 0.178 m | 0.122 m | 0.176 m |
| median \|C − truth\| | 1.44 m | 0.77 m | 1.13 m | 1.53 m | 1.95 m | 1.95 m | 3.38 m |
| implied vf | 0.05 | 0.11 | 10.7 | 15.0 | 74.3 | 257 | 371 |

The covariance says *converging*; truth says *diverging*.  **Every internal instrument
called the run clean**: σ̂₀ = 0.0419, zero RAIM flags on all six tiers, zero withholds, rank
gate passing, no discontinuities.  A confidently wrong estimator satisfies every internal
grade this project has ever applied.

**3. Not a correction mismatch, and not the injected biases.**  A bisect arm
(`_simnet3_bare`) with the ENTIRE optional correction stack off on both sides (relativity,
Shapiro, solid tide, pole tide, sat + rx antenna, wind-up) and the receiver code biases
removed reproduces the result to three decimals — C rms 5.5151 vs 5.5171 m, ZWD 4.3055 vs
4.3073 m, vf 87.46 vs 87.53.  One arm, many knobs, but the *direction* is unambiguous: the
residual does not live in the correction stack.

**4. vf remains ≫ 1 even at n = 5** (392 clock, 6.5 ZWD) — the formal σ understates the
true error by ~20× (clock) and ~2.5× (ZWD) in σ.

### What this does NOT establish

- **Not the field cb vf ≈ 10 question yet** — no B channel in the INT posture, so the
  code-bias vf is unmeasured; the W6 arm (§2.2) is required and not yet run.
- **A small-network vf does not extrapolate** — ZWD vf fell 87.5 → 6.5 for n = 3 → 5; the
  field runs 63–305 stations.  Any "the estimator's σ is off by X" must name its n.
- **The P channel is not yet a sharp test** — SD fractional residuals 0.21–0.35 cyc, i.e.
  the ambiguities have not resolved in 2 h at this scale, so §4.3's "exactly integer × λ" is
  untested rather than falsified.
- Ignition is excluded by grading the settled half, but 2 h is the MINIMUM (§2).

