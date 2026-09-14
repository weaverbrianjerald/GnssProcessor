# EXT-CB defect 2 — arm 30 and the 303-station EXT-CB arm — DEFECT 2 is largely a station-support artifact

**STATUS**

- **id:** EXT-CB defect 2
- **topic:** ionosphere
- **status:** closed
- **summary:** arm 30 and the 303-station EXT-CB arm — DEFECT 2 is largely a station-support artifact
- **dates in the text:** 2026-08-05
- **knobs / posture:** `processing.elevation_mask_deg`, `estimation.ionosphere_weighting.elevation_cutoff_deg`
- **hashes named in the text** (commit / exe / byte-cert): —
- **run dirs named in the text:** —
- **source:** `docs/archive/SPEC_level_information_budget.md` — 20, 20.1, 20.2, 20.3

> Results are appended TO THIS FILE, below the verbatim text. The archived monolith is frozen; never append there.

---
<!-- BEGIN VERBATIM: SPEC_level_information_budget.md -->
## 20. Arm 30 (LV-1+2, no LV-4) and the 303-station EXT-CB arm (2026-08-05)

Both on build `Aug 4 2026 23:27:25`, both at the **pre-2026-08-05 masks** (`processing.elevation_mask_deg` 10,
`ionosphere_weighting.elevation_cutoff_deg` 25) — §20.3.

### 20.1 Arm 30 — the §19 thesis holds

`_ext63_gps_dual_3h_lv12.json`, 3 h, 360 epochs, 331.8 s wall. Predictions written into the config before the
run, scored honestly: (1) "level tracks the stack within ±3 TECU, i.e. ramps" — **roughly right**, h0 +2.95,
h1 +1.49, h2 +8.73 (stack +4.15 / +4.80 / +4.85), and it ramps EARLIER; (2) "**data side stays at the
canonical's 0.009–0.010 m, not the stack's 0.0130**" — **CONFIRMED**, 0.00930 at h2–3; (3) "prires rxcb σ̂₀
stays near the canonical's ~1.0" — **MISSED**, 6.55, nearly the stack's 8.40; (4) "folds ~43 %" — **MISSED**,
57.0 % (12502 / 21929).

Datum-invariant grade, h2–3, identical 132 967 phase observations in all three arms:

| arm | rms_phase | vs canonical | prires rxcb σ̂₀ | a₀₀−GIM h2 | sh_health |
|---|---|---|---|---|---|
| canonical | 0.00900 m | — | 1.06 | −3.89 | 0/0/0 |
| **arm 30 (LV-1+2)** | **0.00930 m** | **+3.3 %** | 6.55 | +8.73 | **0/0/0** |
| stack (LV-1+2+4+5) | 0.01300 m | +44 % | 8.40 | +4.85 | 0/0/0 |

**The two costs are independent and separately attributable**, which prediction 3's miss revealed: **LV-4
(admission) costs the DATA side** — +44 % postfit phase rms, and ONLY when armed; **LV-2 (de-mean) costs the
PRIOR side** — prires rxcb σ̂₀ 1.06 → 6.55 with LV-4 absent, arguably the de-mean working as designed (it
deliberately moves each fold's RHS off what the carry remembers) but a cost nonetheless, and §19.2 wrongly
attributed the whole σ̂₀ inflation to the stack as a unit. Prediction 4's miss is §13.3's channel made
visible: LV-1 moves `x_lin(cb)` and the legacy inject gate thresholds `|value_abs − x_lin(cb)|`, so arming
LV-1 RAISES the fold rate (43 % → 57 %) without touching the gate.

**Consumer replay does NOT rank the postures** (`_ext63_replay_3h_lv12.json`, 11.2 s). Hours 0–2 3D error,
median / mean / max in m — arm 30: 3.99 / 4.77 / 9.7, 4.36 / 3.97 / 4.7, 2.27 / **50.4** / **1149**;
canonical: 2.14 / 2.73 / 7.1, 2.14 / **35.5** / **308**, 3.54 / 3.50 / 3.8; stack: 5.21 / 5.01 / 6.2, 4.28 /
4.52 / 6.0, 3.71 / **220.3** / **979**. All three sit at 2–5 m medians with one catastrophic excursion in the
first 3 h, in DIFFERENT hours. **Arm 30's verdict rests on the postfit residuals, not on this.**

### 20.2 ★ DEFECT 2 is largely a STATION-SUPPORT artifact — measured at 303 stations

`_ext305_gps_dual_1h.json`: the EXT-CB posture of record transplanted onto the 303-station AGU network, SH
held at deg 8 / single layer / a₀₀ anchor 32.24±2 so **network size is the single variable**. 121 epochs,
2979.8 s wall (24.6 s/epoch), exit 0, SH health gate 0/0/0, 2 of 303 stations occasionally dropped.

| | EXT-63 canonical | **EXT-305 canonical** |
|---|---|---|
| stations / phase obs (20–60 min) | 61 / 87 936 | **301 / 423 169** |
| median postfit phase rms | 0.01020 m | **0.00600 m (-41 %)** |
| a₀₀−GIM, hour 0 | **−15.83** | **−1.98** |
| a₀₀−GIM, run mean / sd | -8.38 / 5.53 (12 h) | **−1.99 / 3.64** (1 h) |
| footprint (5°) / global mean | -9.7 / -6.1 (12 h) | **−5.71 / −1.82** (1 h) |
| footprint / global rms | 11.2 / 6.9 | **6.25 / 4.00** |

**The 303-station arm's IGNITION hour (-1.98) is better than any settled hour the 61-station canonical reaches
in twelve** (its best is −3.81 at h3; only h11's +0.97 is closer, and that is a sign crossing). The
datum-invariant instrument agrees independently: postfit phase rms -41 %. Two honest caveats: the footprint
comparison is **NOT time-matched** (1 h mean against §18's 12 h means — the MATCHED comparison is the hourly
global series, -15.83 vs -1.98 at hour 0, the stronger of the two anyway); and §14's "~2 h convergence budget"
was measured at 61 stations, while the 305 arm shows essentially no ignition transient (hour-0 mean -1.98 ±
3.66 vs the 61-sta -15.83 ± 4.93), so **that budget looks like a station-support artifact too.** A 3 h arm
(`_ext305_gps_dual_3h.json`, **written, ~2.2 h wall, still unrun**) would settle whether the 305 level then
walks the way §15 saw at 61.

§18's structural finding SURVIVES at this scale and mask: the residual is still ~3× larger over the footprint
than globally (-5.71 vs -1.82). What changes with station support is the MAGNITUDE — every metric roughly
halves. (⚠ §26.1: at obs 15 the footprint/global relation inverts.)

### 20.3 Mask posture changed after these runs (user directive, 2026-08-05)

`processing.elevation_mask_deg` 10 → **15** and `estimation.ionosphere_weighting.elevation_cutoff_deg` 25 →
**15**, both at the lineage root `_ft63_24h_conv.json` (no config in either chain overrides them; verified
resolved 15/15 for all 63- and 305-sta arms).

⚠ **This invalidates every denominator in §9–§20.** All arms above ran at 10/25. ⚠ The 25° iono cutoff was
VALIDATED and SHIPPED (`project_global_sh_iono`): at 10° the 305-sta field diverged a₀₀ 296 → -305 TECU with
clock-Δ rms 77× worse. The posture check (`GnssConfigNetworkPosture.cpp:369-373`, `:553-565`) requires ≥ 24.9°
and will now WARN on every network run; the deg-15 SH sanction at ≥ 150 stations is conditioned on the 25°
cutoff. ⚠ The recorded path to a lower mask is lever 2 — *carrier-smooth the code GF seed, THEN* lower to
10–15°. That prerequisite is not built; the seed is still a per-epoch code GF estimate (~0.26–0.37 m scatter).
**Prediction, stated before the first 15/15 run: the level deficit gets WORSE, because lowering this mask
historically pushed a₀₀ DOWN and DEFECT 2 is a₀₀ too LOW.** → **FALSIFIED, §21.**

---

