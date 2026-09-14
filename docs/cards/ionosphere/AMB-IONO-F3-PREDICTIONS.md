# F3 (pre-reg) — the pre-registered predictions for the F3 station block, and the round-2 / round-4 result amendments

**STATUS**

- **id:** F3 (pre-reg)
- **topic:** ionosphere
- **status:** closed
- **summary:** the pre-registered predictions for the F3 station block, and the round-2 / round-4 result amendments
- **dates in the text:** 2026-09-03 → 2026-09-04
- **knobs / posture:** `network.phase_leveling_var_floor_m2`, `network.phase_leveling_los_scale`, `network.phase_datum_handover_max_var_m2`, `network.phase_leveling_local_block`
- **hashes named in the text** (commit / exe / byte-cert): `ccfa9eba`, `bd16aca2`, `cdbeaee0`, `b7cea2ad`
- **run dirs named in the text:** `runs/nonint_base3h`, `runs/nonint_base3h_f1exe`, `runs/nonint_losblock3h`
- **source:** `docs/archive/SPEC_amb_iono_split_overcount.md` — §6, 6.1, 6.2, 6.3, 6.4, 6.5, 6.6, 6.7, 6.8

> Results are appended TO THIS FILE, below the verbatim text. The archived monolith is frozen; never append there.

---
<!-- BEGIN VERBATIM: SPEC_amb_iono_split_overcount.md -->
## §6 PRE-REGISTERED predictions (written 2026-09-04 BEFORE any round-2 result was read)

**Arms** (Release, same successor exe, sequential; result `runs/<arm>/ambint.txt`):
`nonint_base3h` (control, 3 h prefix), `nonint_vf005_3h` (`network.phase_leveling_var_floor_m2`
0.05 m²), `nonint_vf001_3h` (0.01 m²), `nonint_ploff_1h` (`phase_leveling` OFF, 1 h),
`nonint_dual_1h` (GPS C1C/L1C/C2W/L2W + Gal C1C/L1C/C5Q/L5Q, `frequency_mode` dual, 1 h),
and the F1 arm to follow, `nonint_los3h` (`network.phase_leveling_los_scale` ON, 3 h).

### 6.1 The window expectation must come from the CRLB curve, not the 24 h mixture

`scratchpad/record/predict_windows.py` (uniform steady-state arc ages 1..A over the §2.5 DD
curve, log-log interpolated; 4e5 draws):

| window | ages | L1C med\|e\| | round0 | WL round0 | EWL round0 |
|---|---|---|---|---|---|
| 1 h | 1..120 | 0.373 | 61 % | 96 % | 99.7 % |
| **3 h** | 1..360 | **0.134** | **87 %** | 98.5 % | 99.9 % |
| 24 h (check vs agent1's 0.120 / 88 %) | 1..720 | 0.079 | 93 % | 99 % | 100 % |

(The 24 h row is optimistic vs agent1's 0.120 because it lacks his elevation-weighted
age mixture; the 3 h row is the reference for the 3 h arms, the 1 h row for the 1 h arms.
Full-window grades include the ignition hour — grade the LAST 25 %.)

### 6.2 The var-floor arithmetic (M6 with a cap)

Once the floor binds, every row injects at weight 1/v_f, so the prior on I is stationary,
Σμ²/v_f, and the recursion becomes an exponential smoother with forgetting
a = (Σμ²/v_f)/(Σμ²/v_f + p_code):

    K_eff = 1/Σw² = (1+a)/(1−a) = 1 + 2·Σμ²/(v_f·p_code)        τ = 1 + Σμ²/(v_f·p_code)

p_code(I) is bracketed: the DD-visible cold value from the trace's own k=1..6 DD σ
(7.2 → 5.7 cyc L1C ⇒ v_I = (σ/2·λ₁)² = 0.47 → 0.29 m² ⇒ **2.1–3.4 m⁻²**) and the CRLB at
30° (19 m⁻²); the column-marginal row weights the code lens dumped (0.13–0.99 m⁻², var
1.0–7.5 m²) include the DD-invisible station-common share and UNDER-state it.  Point
estimate p_code ≈ 3 m⁻².

| v_f | p_code m⁻² | K_eff | τ ep | 3 h mixture L1C med\|e\| / round0 | formal DD σ floor 2√(v_f/Σμ²)/λ₁ |
|---|---|---|---|---|---|
| 0.05 | 1 / **3** / 10 | 278 / **93** / 29 | 140 / **47** / 15 | 0.138 / **0.274** / 0.691 cyc; 87 / **76** / 38 % | **0.89 cyc** |
| 0.01 | 1 / **3** / 10 | 1387 / **463** / 140 | 694 / **232** / 70 | 0.134 / **0.134** / 0.201; 87 / **87** / 85 % | **0.40 cyc** |

The floor does NOT bind at k = 7 (harvested var at k = 6 is 0.3–7.5 m² ≫ 0.05), so the
first-inject ratio is unchanged; it binds from k ≈ 8–10.

### 6.3 The prediction table (L1C unless stated; "settled" = last 25 % of the window)

| arm | med\|e\| settled | round0 | WL / EWL round0 | k=6→7 σ ratio | dd_sigma_cyc at k=10 / settled | \|e\|/σ settled | \|e\| vs age slope |
|---|---|---|---|---|---|---|---|
| **base3h** | 2.6–2.9 (24 h trace h1–h2: 2.55 / 2.70); full window 2.9–3.2 (h0 = 3.74) | 8–11 % | 30–40 % / 93–97 % | **0.49–0.51** | 0.80 / 0.02–0.04 | > 50 (trace h1: 73) | −0.05…0 |
| **vf005_3h** | **0.15–0.7, point 0.27** | 40–87 %, point 76 % | > 95 % / > 99 % | **0.49–0.51 (unchanged)** | ≥ 1.0 / **flat ≈ 0.9** | **0.2–0.8 — the formal σ turns CONSERVATIVE** | −0.4…−0.5 to age ≈ 2τ ≈ 100, then flat |
| **vf001_3h** | **0.13–0.20, point 0.134** (age-limited: behaves as the correct estimator over 3 h) | 85–87 % | 98 % / 99.9 % | 0.49–0.51 | ≥ 1.0 / **flat ≈ 0.4** | 0.3–0.5 | ≈ −0.5 |
| **ploff_1h** | **1.2–2.2, point 1.49** (single-epoch at the CURRENT elevation — BETTER than base, whose frozen level carries the 10° birth code) | 15–25 % | 50–60 % / 95–98 % | **≈ 1.0 (no inject; 0.9–1.0 from border convergence)** | ≈ σ₁(el) ≈ 2–3 / same | **0.6–0.8 (calibrated)** | 0 |
| **dual_1h** | 2.6–3.6, point 3.2 (σ₁ larger by √(6.93/3.71) = 1.37 with two codes; K_eff 3 not 2) | 7–11 % | 25–40 % / n.a. | **0.577 (±0.03)** | per-epoch ratio k=7..10 ≈ 0.71–0.80 | ≫ 10 (collapse persists at 1/√2 per epoch) | −0.05…0 |
| **los3h (F1)** | **0.13–0.25, point 0.134** | 75–87 % | 95–99 % / ≥ 99.5 % | **0.707 (±0.03)** | ≈ 2–3 (CRLB at 4–5 accumulated epochs) / tracks 0.11–0.33 | **0.7–1.5** | −0.4…−0.5 |

Secondary (from the code lens's T4-pool HYPOTHESIS: `NetworkRaim.cpp:236` admits any row with
|A_r| > 0.5 as "a phase row of this ambiguity" and the coupling row carries exactly 1.0, so
T4 ARC flags are the over-confident prior's OWN standardized residual): base3h T4 ARC ≈ 290
(24 h trace h0–h2: 63 + 122 + 103); **vf005 / vf001 / los3h < 100**; ploff ≈ data-side only,
≪ 290.  `refsat_reset` unchanged in every arm (≈ 486 in h0 then 55–65/h — it is a detector
rule, not a level effect).

### 6.4 Falsifiers — what refutes the mechanism

| observation | verdict |
|---|---|
| base3h differs from the 24 h trace's first 3 h by > 0.3 cyc med\|e\| or the k=6→7 ratio leaves 0.49–0.51 | the successor build changed the mechanism; every other arm is uninterpretable until re-based |
| vf005 or vf001 leave settled med\|e\| > 2 cyc WITH dd_sigma_cyc floored at 0.9 / 0.4 | the level is not the injected prior weight — **M6 refuted** |
| vf arms' dd_sigma_cyc keeps collapsing to 0.02 | the floor knob is inert on this path (check the manifest's `phase_leveling_var_floor_m2` and `Assembly:2910`) — arm uninterpretable, not a refutation |
| vf005 better than vf001 (non-monotone) | the floor-smoother model is wrong (K_eff is monotone in v_f for any p_code) |
| ploff shows a FROZEN level (lag-1 > 0.9, D(1) < 0.1 cyc, \|e\|/σ > 10) | the freezer is NOT the leveling carry — look at the S-basis / anchor structure; M6 refuted |
| ploff med\|e\| WORSE than base (> 3 cyc settled) | the carry adds real information beyond 2 epochs — K_eff derivation wrong |
| dual first-inject ratio 0.50 or K_eff ≈ 2 (from its own σ₁) | the over-count is B-independent ⇒ station-common, not per-band ⇒ M6 as written refuted |
| dual ratio 0.577 AND K_eff ≈ 3 | the B-dependence is confirmed — the strongest single confirmation available |
| los3h med\|e\| > 1.5 cyc or first-inject ratio < 0.6 | the scaling is not reaching the rows (check the B_eff histogram counter — invariant #4) or the mechanism is elsewhere |
| los3h intermediate (0.5–1.5) with ratio ≈ 0.7 | the station-common residue matters more than DD-invisibility allows, or B_eff mis-counts skipped siblings — F2/F3 owed |
| within one LOS-pair corr(e_L1C, e_L2W) < 0.95 on any arm still at ~3 cyc | the error is not one iono-direction scalar — M6 refuted (24 h bed: off-plane residual 2.3 mm says > 0.99) |

### 6.5 What the arms cannot decide

A 1 h arm cannot settle the LEVEL channel (SPEC_synthetic §13.8.1 makes 1 h legal for the
INTEGER question only); `ploff_1h` and `dual_1h` are graded on the first-inject ratio,
K_eff and time structure, not on absolute med|e|.  None of the arms grades the
station-common residue (§3) — that needs `network_ionosphere.csv` station-mean δI vs
`truth_atmo`, consumer-side.

### 6.6 Provenance of the numbers above

CRLB table and mixtures: `scratchpad/theory/crlb_amb.py` (agent1) and
`scratchpad/record/predict_windows.py` (this card).  Floor arithmetic: §6.2, closed form.
Trace anchors: §2.4 (k table), §2.2 (hourly med|e|), §2.3 (T4 hourly counts).

### 6.7 Results — AMENDMENT (2026-09-04, round 2; §6.3 untouched)

Arms as run: Release, `config/test_suite/_nonint_*.json`, 60 sta, tow 259230 + 360 epochs
(3 h) or 121 (1 h).  `nonint_base3h` on the 2026-09-03 exe; `nonint_los3h` and
`nonint_base3h_f1exe` on the F1 exe (2026-09-04 20:08).  **The GF-datum dual arm
(`nonint_dual_1h`) could not run**: exit 1, `NetworkSbasis` null-space gate 43–48 near-zero
eigenvalues against a declared S-basis dimension of 2, every epoch withheld, ZERO solution
epochs — two bands + `code_bias_gf_datum` is structurally under-specified (the loader interlocks
it; with triple mode + trimmed signals the run withheld every epoch).  The B = 2 reading comes
from `nonint_dual_v1_1h` — the **V1 posture** (`estimate_sat_code_bias` false, .BIA applied): a
DIFFERENT datum, the same carry path; graded on the first-inject ratio only.

#### 6.7.1 The results table (same columns as §6.3; "settled" = last 25 %; L1C unless stated)

| arm | med\|e\| settled | round0 | WL / EWL round0 | k=6→7 σ ratio L1C (L2W / L5Q / L7Q) | dd_sigma_cyc at k=10 / settled | \|e\|/σ settled (p90) | \|e\| vs age slope (runs ≥ 60) | T4 attrib_arc / refsat_reset |
|---|---|---|---|---|---|---|---|---|
| **base3h** | **2.720** (full 2.928) | 10.2 % | 36.5 / 97.9 % | **0.494** (0.548 / 0.561 / 0.570) | 0.804 / 0.024 | 83 (275) | −0.24 | 288 / 1170 |
| **vf005_3h** | **3.457** (WORSE) | 9.0 % | 72.5 / 99.8 % | 0.494 (unchanged) | 1.001 / **0.859** (floor binds from k ≈ 9) | 4.0 (14) | −0.28 | 0 / 1170 |
| **vf001_3h** | **3.635** (WORSE) | 9.6 % | 52.5 / 96.2 % | 0.494 | 0.812 / **0.424** | 8.3 (30) | −0.32 | 0 / 1170 |
| **ploff_1h** | **4.445** (1 h; WORSE than base) | 6.8 % | 49.3 / 98.6 % | **0.970** (0.985 / 0.992 / 0.993) | 5.50 / 6.45 | **0.8** (2) | −0.29 | 0 / 1051 |
| **dual_v1_1h** | 5.205 (1 h, V1 datum; L2W 5.459, L5Q 5.708) | 6.5 % | 31.5 / n.a. | **0.565** (0.604 / 0.636 Gal-L5Q) | 1.43 / 0.022 | 134 (782) | −0.32 | 94 / 1051 |
| **los3h (F1)** | **1.276** (full 2.374; L2W 1.133, L5Q 1.293, L7Q 1.517) | **23.1 %** | **87.4 / 100.0 %** | **0.588** (0.650 / 0.660 / 0.665) | 1.767 / 0.717 | **1.9** (6) | −0.36 (L2W −0.39, L5Q −0.38) | **0** / 1170 |

DD identity on the settled window (`--iono-join --stride 4`; e_b vs μ_b·DD(δI)/λ_b, L1C):

| arm | med\|pred\| (iono direction) | resid med\|.\| (non-iono) | slope / r² | reading |
|---|---|---|---|---|
| base3h | 2.659 | 0.258 | 1.01 / 0.94 | pure iono-direction, the 24 h bed reproduced |
| vf005_3h | 1.477 | **2.073** | 2.22 / 0.89 | iono part halved, a GEOMETRIC error appeared |
| vf001_3h | 1.809 | **2.182** | 1.76 / 0.78 | same, non-monotone vs vf005 |
| ploff_1h | 2.530 | 2.299 | 1.99 / 0.88 | single-epoch in EVERY direction |
| dual_v1_1h | 3.330 | 2.258 | 1.51 / 0.83 | different datum; not graded on level |
| **los3h (F1)** | **0.636** | **0.768** | 1.99 / 0.82 | 76 % of the iono-direction error removed; remainder ≈ half iono, half geometric / station-common |

Also measured: MW refloats 143 and PB-gauge anchor handovers 102 identical across every 3 h arm
(0 / 33 and 93 / 22 on the two 1 h arms); the σ schedule at k ≥ 60 is a population median over
mixed ages (los3h 0.71–0.74 flat, base3h still collapsing 0.074 → 0.024), so read the k ≤ 20
ladder for the recursion and |e|/σ for calibration.

#### 6.7.2 Verdict per pre-registered row

- **base3h — CONFIRMED** (7 of 8 columns inside the band; the successor build did not change
  the mechanism: 24 h trace h1–h2 2.55/2.70 vs 2.72, ratio 0.494 = 0.494, so §6.4 row 1 did
  not fire).  Age slope −0.24 vs the predicted −0.05…0 is MIXED: on a 3 h window the age bins
  alias elevation gain — `ploff`, K_eff = 1 by construction, shows the same −0.29 — so the
  24 h minlen-200 slope −0.033 stays the frozen-level instrument.
- **vf005 — FALSIFIED as written** (predicted 0.15–0.7, point 0.27; measured 3.46, WORSE than
  base by 0.74 cyc).  Confirmed sub-rows: σ floor 0.86 (predicted 0.89), first-inject unchanged
  0.494, T4 < 100 (0).  Failed: |e|/σ 4.0 (predicted 0.2–0.8 — σ still 4× optimistic), WL
  72.5 % (predicted > 95 %).
- **vf001 — FALSIFIED** (predicted 0.13–0.20; measured 3.64); σ floor 0.42 (predicted 0.40);
  NON-MONOTONE vs vf005 (3.64 > 3.46 in total AND 1.81 > 1.48 in the iono part) — §6.4's
  "vf005 better than vf001" falsifier fired too.
- **Why the var-floor predictions were wrong — plainly.**  §6.2 projected every row onto the
  iono direction and treated the floor as a cap on the iono-direction prior weight only.  A
  floor applies to the WHOLE row: it also releases the row's geometric / station-common share,
  which the stiff diagonal carry had — wrongly weighted but effectively — PINNED through the
  densely-carried border (the direction §3(f) said accumulates).  The identity split shows it:
  the iono part fell 2.66 → 1.48 / 1.81 cyc (the floor DID act on the iono direction, at 5–13×
  the predicted magnitude) while a NON-iono residual of 2.07 / 2.18 cyc appeared (base 0.26),
  slope 1.7–2.2, band ratios ≈ equal in cycles — the geometric signature, not (1, 1.28, 1.34).
  The theory skeptic's own floor model (r2_4: 0.43 / 0.19 cyc, |e|/σ ~0.7) was wrong for the
  same reason — both derivations omitted the station-common / geo coupling the skeptic himself
  measured on the ZWD (§3.1), and the ZWD under vf005 shows it directly: prior loosened to
  0.054 m, error 0.26–0.30 m median (base 0.027).  §6.4's "M6 refuted" reading of the > 2 cyc
  floor falsifier does NOT follow: that falsifier was written for a one-direction model; the
  direction-resolved instrument shows the iono direction behaved as the mechanism says and the
  total got worse for a reason outside that model.  B_fit = 3.00 (§2.7) and the F1 arm stand
  on their own instruments.  **Verdict on the knob: FALSIFIED as a fix — worse total, releases
  the geo direction; stays the emergency lock-in guard only.**  Open: why vf005 < vf001 in the
  iono part (§7).
- **ploff — MIXED.**  Ratio ≈ 1.0 CONFIRMED (0.970–0.993); |e|/σ 0.8 CONFIRMED (calibrated);
  WL 49 % and EWL 98.6 % inside; T4 0.  med|e| 4.45 **FALSIFIED** (predicted 1.2–2.2, BETTER
  than base; the theory skeptic's 4.0–4.7 was right).  §6.4's "ploff WORSE than base ⇒ the carry
  adds real information beyond 2 epochs ⇒ K_eff derivation wrong" fired as written, and the
  resolution is again directional: base's K_eff ≈ 2 holds in the IONO direction (data skeptic
  2.1) while its GEO direction accumulates through the border (§3(f)); a no-carry LOS is
  single-epoch in EVERY direction — identity split resid 2.30 (geo) + pred 2.53 (iono).  The
  prediction's "single-epoch at the current elevation" reasoning forgot the second direction.
- **dual — CONFIRMED on the ratio, level NOT GRADABLE.**  L1C 0.565 (predicted 0.577 ± 0.03;
  skeptic's per-signal 0.55 ± 0.02), L2W 0.604 (0.61 ± 0.02), Galileo L5Q 0.636; every band
  shifted +0.056 … +0.075 from triple (predicted +0.077); second step 0.678/0.696/0.705 vs
  triple 0.616–0.628 (B=2 model 0.655, B=3 0.556 — both arms sit 0.03–0.06 above their model,
  the excess the theory skeptic left open).  The settled 5.2 cyc is 1 h on a different datum
  (predicted 2.6–3.6 for the GF posture) — not a level test.  §6.4's strongest row ("0.577 AND
  K_eff ≈ 3") is half-taken: the ratio, yes; K_eff not measured on this posture.
- **los3h (F1) — MIXED, in the pre-registered "intermediate" row.**  med|e| 1.276 (predicted
  0.13–0.25 — FALSIFIED as a point; inside §6.4's 0.5–1.5 band); round0 23.1 % (75–87 %); WL
  87.4 % (95–99 %, just below), EWL 100 % ✓; first-inject 0.588 L1C / 0.650–0.665 others
  (predicted 0.707 ± 0.03 — FALSIFIED for L1C, marginal for the rest; the per-band ORDER is the
  station-common share collapsing at the station-wide rate — the same reason base L1C read
  0.473 < 0.500 per chord; the skeptic pre-predicted L1C ~0.68 and got 0.588, i.e. the share is
  larger than his estimate); |e|/σ 1.9 (p90 6) vs 0.7–1.5 — nearly honest, the p90 tail is the
  frozen residue; age slope −0.36 … −0.40 ✓ (with the ploff caveat: the extra −0.12 over base is
  the accumulation signal; the k ≤ 20 σ ladder 3.35 → 0.95 is the cleaner one); T4 288 → 0 ✓;
  run-end counter §5.  §6.4's row "los3h intermediate (0.5–1.5) with ratio ≈ 0.6–0.7 ⇒ the
  station-common residue matters more than DD-invisibility allows — F2/F3 owed" is the one
  that fired.  Identity split: iono part 0.64 (from 2.66), non-iono 0.77 (from 0.26).  **Verdict
  on the knob: VALIDATED on the sim bed as the per-LOS fix; not the whole fix.**
- **Byte identity (invariant #6) — PASS, MEASURED.**  sha256(`runs/nonint_base3h/products.txt`)
  = sha256(`runs/nonint_base3h_f1exe/products.txt`) =
  `1c1b60c7792e5f91ef6e4e2f659883603a9fc0ae8112d3a26f86c3355029229d` (8,601,257 bytes) — the
  2026-09-03 exe vs the F1 exe with the knob OFF, i.e. cross-build identical; `ambint.txt`
  identical to the printed digit; manifests differ by the one added key.
- **Secondary predictions**: T4 base ≈ 290 → 288 ✓; vf / los < 100 → 0 ✓ (stronger than
  predicted); ploff ≪ 290 → 0 ✓; `refsat_reset` unchanged → 1170 in every 3 h arm and 1051 in
  both 1 h arms (a detector rule) ✓.
- **§6.1 window rule**: no arm reaches the 3 h CRLB reference (0.134 cyc / 87 %); los3h's 1.28
  is 10× above it — the station-common channel, not the window.

#### 6.7.3 Provenance

`scripts/simnet_amb_integer.py <fixture> --run runs/<arm> --sigma-ratio --by-age --by-age-minlen 60
--iono-join --stride 4 --gal-lanes --check-lambda` (default output byte-identical to the round-1
grader; λ PASS ≤ 3.7e-7 on every arm; Gaussian consistency 3–16 % per band on the carried arms,
8–10 % on los3h settled).  Outputs: `scratchpad/arms/flags_grade_r2.txt` (five arms),
`scratchpad/record/los3h_flags.txt`, `scratchpad/record/sigma_ratio_r3.txt` (head-only
ladders, all six arms), `scratchpad/record/zwd_freeze_arms.out`; event counts by
`grep -c <class> runs/<arm>/logs/network_events.csv`.

---

### 6.8 F3 results — AMENDMENT (2026-09-04, round 4; §6.3 and §6.7 untouched)

Arms: Release, `config/test_suite/_nonint_*.json`, 60 sta, tow 259230 + 360 epochs, quiet wu93 bed.
`nonint_losblock3h` (F3, handover ceiling 1e-4 m²), `nonint_losblock3h_hv` (F3 + ceiling **1e-2**),
`nonint_los3h_hv` (F1 + 1e-2), OFF twin `nonint_base3h_f3exe`; two independent graders; MEASURED
unless marked.

#### 6.8.1 Truth grade vs the pre-registered line (settled = last 25 %; grader flags as §6.7.3, `--stride 1`)

| quantity | pre-registered (§5.1 / r3_5) | base3h | los3h (F1) | **losblock3h (F3)** | **losblock3h_hv** | verdict |
|---|---|---|---|---|---|---|
| L1C med\|e\| / round0 | 0.55–0.62 cyc / 42–46 % | 2.720 / 10.2 % | 1.276 / 23.1 % | **0.217 / 79.3 %** (full window 0.357 / 59.7 %) | **0.187 / 83.1 %** | **BEATEN 2.7×** (the > 0.8 falsifier not tripped) |
| L2W / L5Q / L7Q med\|e\| (round0) | ~0.66 / 0.76 / 0.86 | 3.223 / 3.614 / 3.974 | 1.133 / 1.293 / 1.517 | **0.266 / 0.287 / 0.287** (72.9 / 70.9 / 70.9 %) | 0.203 / 0.243 / 0.274 (80.8 / 76.5 / 73.2 %) | BEATEN |
| WL(1,2) / EWL(2,5) round0 | ~93 % / 100 % | 36.5 / 97.9 | 87.4 / 100 | **98.8 / 100** (med 0.065 / 0.022 cyc) | 99.0 / 100 | BEATEN / CONFIRMED |
| first-inject σ(7)/σ(6) L1C (L2W / L5Q / L7Q) | ≈ 0.707 | 0.494 (0.548 / 0.561 / 0.570) | 0.588 (0.650 / 0.660 / 0.665) | **0.720** (0.722 / 0.728 / 0.722) | identical | CONFIRMED (+3 %) |
| \|e\|/σ settled (p90) | ~1.0–1.1 | 83 (275) | 1.9 (6) | **0.8** (2) | 0.7–0.8 | CONFIRMED, slightly conservative |
| formal σ_DD settled, legs ≥ 30 (≥ 100) | "unchanged ~0.55" | 0.026 | 0.616 | **0.239 (0.190)** | 0.199 (0.153) | **FALSIFIED** — 0.55 was not the floor (§6.8.2) |
| implied B, mid-run newborn chords (a5 fit) | 1.00 ± 0.01, same mature-leg floor | 3.00 | 1.000 ± 0.005, v_m 0.38 cyc² | **1.000 ± 0.002, v_m −0.07 (floor GONE)** | 1.000 ± 0.002, v_m −0.09 | B CONFIRMED; floor FALSIFIED |
| DD identity slope / r² (grader, L1C) | — | 1.01 / 0.94 | 1.99 / 0.82 | **0.983 / 0.95–0.99** (resid med 0.045) | 0.98–1.00 | exact |
| \|e\| vs age slope (runs ≥ 60) | — | −0.24 | −0.36 | **−0.60** (1/√K = −0.5; elevation rises along the arc) | −0.62 … −0.68 | — |
| L1C by min-leg elevation 10-15 / 15-20 / 20-30 / 30-45 / 45-90° | 1.0 → 0.23 (elevation-limited) | flat 2.5–2.7 | 1.08 / 0.66 / 0.50 / 0.33 / 0.23 | **0.20 / 0.22 / 0.19 / 0.13 / 0.09** | — | **FALSIFIED** (flat) |
| ZWD h2 med\|err\| / \|err\|/σ / D(10) | ≤ 0.02 m / ~1 / < 0.01 | 0.027 / 4.9 / 0.008 | 0.127 / 2.2 / 0.039 | **0.0056 m / 1.0 / 0.0064** | 0.0053 / 0.9 / — | CONFIRMED |
| ZWD-geometry DD part Z (a3), med [rms] | ≤ 0.10 cyc | 0.201 [1.02] | 0.694 [1.88] | **0.032 [0.079]** | 0.03 [0.067] | CONFIRMED |
| T4 attrib_arc / T3 cell flags | 0 / — | 288 / 4900 | 0 / 0 | **0 / 1** (k = 42, ignition, unacted) | 0 / 1 | CONFIRMED |
| data-skeptic (a) c_a at a = 1..5 / (b) med\|r(100)\|/med\|r(0)\| / (c) DD level vs spread | (b),(c) PASS; (a) proxy-incapable | 0.18/0.04/0.03/0.02/0.03 / 0.718 / rising | 0.31/0.11/0.10/0.09/0.07 / 0.153 / flat | **0.477/0.257/0.234/0.189/0.250 = the 1/(a+1) law / 0.066 / 0.070, 0.047, 0.052 m flat** | identical ± 0.01 | all three PASS |

`los3h_hv` (F1 + 1e-2) on the same instrument: 1.200 / 24.8 %, WL 87.5 %, EWL 100 % — the ceiling
lift buys F1 0.08 cyc; F1 stays in §6.4's "intermediate" row.

#### 6.8.2 Why the prediction UNDER-shot — plainly (residue verifier; DERIVED from the MEASURED parts)

The §5.1 / r3_5 line took F1's settled iono-direction part (0.569 cyc) as the "honest per-LOS
accumulation floor" no local-local fix could lower at 3 h, and predicted F3 would remove only
the 0.69-cyc ZWD-geometry term.  Both halves were wrong, each for one reason:

1. **The 0.57 was not honest — it carried the ZWD wander.**  Per station, the frame-removed per-LOS
   iono error regressed on mf(el) has an elevation-shaped station term of rms 0.0975 of 0.1258 m
   under F1 (**60 % of the variance**) whose slope tracks the station's ZWD error at corr **−0.83**
   (−0.44 m/m); under F3 0.012 of 0.037 m (10 %), corr +0.14.  At DD level F1's "iono part"
   correlates with Z at −0.73 — a Z-projected share of rms **0.878 of 1.197 cyc (54 % of its
   variance)**: the ZWD wander entered F1's DD error at ~1.45× (0.46 via the iono part + 0.99 via
   ν) and round 3 credited only the 0.99.
2. **The rest was the diagonal carry's cross-band information LOSS.**  The single-LOS recursion
   with the F1 prior (diag(posterior) × 3) reproduces the B = 1 ladder exactly (0.7073 / 0.8168)
   yet its actual σ saturates at DIAG3/FULL = 1.06 / 1.31 / 1.58 / 1.73 at k = 100 / 200 / 300 /
   360 — the mature-leg floor v_m = 0.38 cyc² a5 fitted on F1, gone on F3 (−0.07).
3. **"The CRLB gap is age" is FALSIFIED.**  The white population CRLB on the 24 h producer run's
   own DD population reads L1C **0.145 / 86.1 % at h2–3 and 0.137 / 86.4 % at h12–24** (min-leg
   age p50 117 vs 119) — the 3 h settled quarter already IS the steady-state age mix; the F3
   residue is mechanism, not K.

Residue table (settled L1C, cycles; a3 subset ep ≥ 270 / legs ≥ 30 unless noted):

| component | base3h | los3h (F1) | **losblock3h (F3)** | losblock3h_hv |
|---|---|---|---|---|
| TOTAL med\|e\| (a3) [grader all rows / round0] | 2.594 [2.720 / 10.2 %] | 1.159 [1.276 / 23.1 %] | **0.187 [0.217 / 79.3 %]** | 0.159 [0.187 / 83.1 %] |
| per-LOS iono part, med [rms] | 2.565 [3.96] | 0.569 [1.20] | **0.183 [0.342]** | 0.155 [0.284] |
| — of which Z-projected (ZWD leaked into per-LOS levels), rms | 0.038 | **0.878** | 0.007 | 0.025 |
| ZWD-geometry part DD(mf·δZWD), med [rms] | 0.201 [1.02] | 0.694 [1.88] | **0.032 [0.079]** | 0.03 [0.067] |
| remainder | 0.048 | 0.057 | 0.032 | 0.031 |
| identity slope (a3) | ~1.1 | 2.13 | 0.97 | 0.96 |
| population CRLB mixture on the same rows (white) | 0.124 / 93.9 % | same | same | same |
| med \|e\|/σ_crlb (bound 0.674) | 12.2 | 5.58 | **0.970 (1.44×)** | **0.839 (1.24×)** |
| formal σ_DD (legs ≥ 30) / formal ÷ crlb / \|e\| ÷ formal | 0.026 / 0.14 / 90 | 0.616 / 3.15 / 1.90 | 0.239 / 1.10 / 0.79 | 0.199 / 1.03 / 0.80 |
| ZWD h2 med\|err\| / rms / \|err\|/σ (m) | 0.027 / 0.401 / 4.9 | 0.127 / 0.202 / 2.2 | **0.0056 / 0.0091 / 1.0** | 0.0053 / 0.0083 / 0.9 |
| PB-gauge handovers continuous / cold; leveling carries erased | 90 / 3; 57 | 0 / 93; 1653 | 0 / 93; 1653 | **70 / 23; 408** |

Where the remaining 1.44× (hv 1.24×) lives — the population-CRLB instrument
(`r4resid/crlb_pop.py`: white 0.003 / 0.30 m per sin el, I free per epoch, ρ′ known, each leg's
information summed over its PRESENT epochs since birth at the producer's elevations, DD = 4
independent legs) is the standing grade from here on:

| row class (losblock3h / hv) | med \|e\|/σ_crlb | reading |
|---|---|---|
| all four legs born mid-run (n = 1627) | **0.694 / 0.640** (0.230 vs CRLB 0.222) | AT the bound (sim GM code multipath is worth +4–12 % in σ at K = 100–300) |
| 1–2 / 3–4 ignition-born legs | 0.925 / 1.075 (hv 0.832 / 0.882) | the whole excess |
| ignition-born, NO cold-handover hit, NO T1/T0 refloat on the leg (n = 2085 / 5936) | 0.843 / 0.904 | 1.25–1.34× with no logged event; T1/T0 history moves nothing (± 0.02) |
| leg hit by a cold PB-gauge handover since birth | 1.163–1.206 (unhit 0.842) | ~1.4× σ cost on hit legs; hv shrinks the hit population to 93 rows |
| 1+ re-birth leg | 0.78 / 0.66 | gap re-births NOT measurable above the bound (§7 item 9) |
| by min-leg age [1,30) / [30,60) / [60,120) / [120,240) / [240,361) | 0.81 / 0.79 / 0.86 / 0.99 / 1.20 (hv 0.80 / 0.70 / 0.77 / 0.91 / 0.90) | the ignition class ageing in |
| by min-leg elevation (5 bins) | 0.97 / 0.92 / 1.03 / 0.98 / 0.88 | elevation-flat |

#### 6.8.3 The handover ceiling — census, refusal attribution, wire continuity

`network.phase_datum_handover_max_var_m2` (`NetworkPhaseDatum.cpp:376-403`: usable = ledger hit
∧ finite ∧ var > 0 ∧ var ≤ max_var, else `++handover_cold` and `++handover_refused_var` when
var > max_var; the ledger var is the successor's border-exact `pureConditionalLocalVarianceDiag`,
`NetworkRunContext.cpp:2806-2846`).  The lineage value **1e-4 m²** (σ = 1 cm; config line 764,
inherited from the camp60wu sticky base; loader default 1.0, banner "keep this TIGHT") was sized
against the OVER-COUNTED σ; F1 and F3 both make the successor's σ honest (settled P σ 3–5 cm), so
the round-3 prediction "F3 will be refused the same way" is CONFIRMED.

| arm | ceiling | continuous / cold (refused) | per hour cont \| cold | settled cont / cold | carries erased | 1(d) marked |
|---|---|---|---|---|---|---|
| base3h | 1e-4 | 90 / 3 (3) | 33,30,27 \| 0,3,0 | 21 / 0 | 57 | 174 |
| los3h (F1) | 1e-4 | 0 / 93 (93) | 0 \| 33,33,27 | 0 / 21 | 1653 | 264 |
| los3h_hv (F1) | 1e-2 | 67 / 26 (26) | 27,23,17 \| 6,10,10 | 13 / 8 | 461 | 197 |
| losblock3h (F3) | 1e-4 | 0 / 93 (93) | 0 \| 33,33,27 | 0 / 21 | 1653 | 264 |
| **losblock3h_hv (F3)** | 1e-2 | **70 / 23 (23)** | 17,26,27 \| 16,7,0 | **21 / 0** | 408 | 194 |

Refusal attribution at 1e-2 (`r4_wire/handover_age.py`, successor arc age from `station_residuals`
presence): under **F3 every refusal is a YOUNG successor** — cold ages n = 23, med 1680, max
**3120 s**, all in h0/h1; continuous ≥ 960 s (med 4170) — an AGE gate under honest σ, so the settled
window is fully continuous.  Under **F1 the refusals are per-BAND** — cold L1C 3 / L2W 7 / L5Q 12 /
L7Q 4 on arcs up to 10560 s old, 9 MIXED families (L1C continuous, other bands cold), four settled
(268530 G29, 269040 E33, 269280 E25, 269970 G31): the B_eff-scaled non-L1C bands sit above σ = 10 cm
while their L1C passes (DERIVED — no per-handover var is traced).  Neither reaches the ≥ 90/93 bar.

Wire continuity (`simnet_seam_tools.py steps`, phase-side P − C per (sat, signal), settled last 25 %):

| arm | n > 0.05 m | n > 0.5 cyc | step rms (m) | max (m) | epochs with a PB step > 1 m | matched to cold-handover tows |
|---|---|---|---|---|---|---|
| base3h | 47 | 22 | 0.0104 | 0.285 | 0 | — |
| los3h | 233 | 91 | 0.3095 | 14.25 | 7 | 7/7 |
| los3h_hv | 87 | 34 | 0.2077 | 14.58 | 4 | 4/4 (268530 6.39, 269040 14.58, 269280 4.45, 269970 10.96 m) |
| losblock3h | 200 | 85 | 0.3117 | 15.15 | 7 | 7/7 (268050 G32 10.38, 268530 G29 2.65, 268980 E10 7.06, 269040 E33 14.47, 269280 E25 7.59, 269640 G22 15.15, 269970 G31 9.07 m) |
| **losblock3h_hv** | **78** | **21** | **0.0111** | 0.291 | **0** | — (whole run after 20 min: 8 epochs > 1 m, all matched, all h0/h1) |

Wire ACCURACY is neutral to the ceiling on both mechanisms (clock / CB / rx-DCB identical to 3
digits; ZWD 0.0088 → 0.0081 under F3, 0.191 → 0.204 under F1); the only wire-side gain is
continuity (Psig med 0.049 → 0.031 m; settled integer-class drifts E 30 → 6).  **F1 + 1e-2 is NOT
a valid combination** (band-mixed handovers; consumer +0.43 m up bias, §6.8.4).  **F3 + 1e-2 is
the arm to carry forward** — a working value, not a promotion (§7 item 15).

#### 6.8.4 Wire and consumer (settled last 25 %; `simnet_grade.py --settle-frac 0.75`, `simnet_sat_clock_census.py`, `network_epoch_status.csv`)

| channel | base3h | los3h (F1) | los3h_hv | **losblock3h (F3)** | losblock3h_hv |
|---|---|---|---|---|---|
| sat clock residual rms / p50 / contrast rms (m) | 1.704 / 1.112 / 2.194 | **1.437 / 0.832 / 1.751** | 1.437 / 0.833 / 1.756 | 1.583 / 1.011 / 2.039 | 1.582 / 1.000 / 2.040 |
| sat clock vf / URA med (m) | 135958 / 0.0045 | 7423 / 0.0172 | 7472 / 0.0172 | 106552 / **0.0048** (base-like) | 112029 / 0.0047 |
| per-sat settled tails G12 / G16 / G09 / G18 (m) | 7.08 / 4.41 / 3.00 / 1.86 | 6.28 / 3.49 / 2.10 / 1.35 | 6.24 / 3.53 / 2.12 / 1.33 | 6.31 / 4.23 / 2.78 / 1.88 | 6.28 / 4.25 / 2.79 / 1.89 |
| sat code bias rms / p50 (m) | 0.814 / 0.046 | 0.504 / 0.011 | 0.504 / 0.012 | **0.507 / 0.011** | 0.507 / 0.012 |
| ZWD rms / p50 / max / \|err\|/σ (m); stations > 2 cm | 0.416 / 0.026 / 3.39 / 4.72; 12 > 0.10 | 0.191 / 0.122 / 0.84 / 2.59; 40 > 0.10 | 0.204 / 0.121 / 0.86 / 2.62 | **0.0088 / 0.0055 / 0.047 / 0.94; 1** (S26 0.021) | 0.0081 / 0.0051 / 0.035 / 0.91; 0 |
| rx-DCB contrast rms / formal σ (m) | 1.587 / 0.066 | 1.228 / 0.089 | 1.236 / 0.089 | **1.226 / 0.067** | 1.226 / 0.067 |
| rx clock between-station contrast rms (m) | 1.941 | 1.675 | 1.761 | 1.684 | 1.738 |
| σ̂0 post-fit / code / phase (epoch_status medians) | 1.198 / 1.699 / 0.939 | 0.053 / 1.063 / 0.0078 | 0.053 / 1.069 / 0.0078 | 0.920 / **1.104** / 0.830 | 0.932 / 1.105 / 0.848 |
| station_residuals rms(s) / \|s\| > 6 | 1.782 / 1014 | 0.275 / 0 | 0.274 / 0 | 1.179 / 0 | 1.182 / 0 |
| DD-fractional trace (tail 2700 s): med\|f\| / P(< 0.1) / circR | 0.253 / 0.195 / 0.016 | 0.248 / 0.202 / 0.006 | 0.248 / 0.202 / 0.005 | **0.203 / 0.265 / 0.152** | **0.185 / 0.292 / 0.214** |
| consumer SZZ000SIM settled 3D rms / p50 / max (m) | 0.414 / 0.411 / 0.477 | 0.296 / 0.239 / 0.548 | 0.535 / 0.544 / 0.650 | 0.295 / 0.280 / 0.392 | **0.245 / 0.233 / 0.359** |
| consumer up mean / E rms / N rms (m) | +0.191 / 0.091 / 0.350 | +0.007 / 0.225 / 0.165 | **+0.428** / 0.301 / 0.100 | +0.023 / 0.283 / 0.070 | +0.039 / 0.237 / 0.028 |
| consumer 30-min bins, 3D rms (m) | 2.42 / 3.06 / 1.77 / 0.76 / 0.44 / 0.41 | 2.32 / 0.77 / 0.66 / 0.28 / 0.15 / 0.34 | 2.09 / 0.55 / 0.41 / 0.49 / 0.45 / 0.56 | 2.13 / 0.72 / 0.47 / 0.23 / 0.24 / 0.31 | 2.13 / 0.77 / 0.79 / 0.24 / 0.21 / 0.27 |
| consumer walk (last-10 median − median at [2.33, 2.42) h) | −0.006 | +0.333 | +0.063 | +0.128 | +0.172 |

Readings.  (i) F3 equals or beats F1 on every station-common channel (ZWD 47× base, 22× F1; CB and
rx-DCB equal) and is the first arm to move the DD-fractional trace off uniform (circR 0.016 → 0.15 /
0.21); the P-record between-sat fractional vs truth stays uniform (0.25–0.31 cyc rms) and AR 0/360
on every arm.  (ii) The ONE channel worse than F1 is the satellite clock (rms 1.583 vs 1.437; G16 /
G09 / G18 tails at base level) with a base-like URA 0.005 m — better than base, a regression on F1
(§7 item 18).  (iii) F3's whitened rows do NOT enter the post-fit pool (n_pf_rows identical on all
five arms): the phase σ̂0 0.83 is the data rows fitted under an exact stiff prior, F1's 0.008 the
free-ambiguity zero; in the RAIM-attrib pool |effect| p50 ARC 0.454 → 1.463 (max 4.07 < 6), pool σ̂0
0.233 → 0.991, 0 acted (§7 item 4).  (iv) The final-40-min consumer WALK is NOT the cold handovers:
an EAST drift (blk_hv −0.15 → −0.32 m from ~267930, G12's re-emission epoch) on every F arm, wire
flat (clk contrast 2.110 → 2.089, cb_rms 0.559 → 0.518, n_sats 16) — blk_hv has ZERO settled cold
handovers and still walks; round 3's cold-handover half is FALSIFIED, its G12 half untested (§7 item
19).  (v) The los3h_hv +0.43 m up bias (264510 → 265710, that arm alone) is specific to F1 + lifted
ceiling — HYPOTHESIS.  (vi) Every pre-existing event family is identical across the five arms
(refsat_reset 1170, mw_wl_slip 143, anchor_handover 102, cta_* 64, CLK_STEP 11, topology 28 / 28);
only the PB-gauge mode and its 1(d) / 1f / 1g counters move.  Runtime +7.4 % ms/epoch (F3 and F1 +
ceiling alike; uncontrolled load).  prires / block-Baarda / congruence not armed in any `_nonint`
config — UNAVAILABLE, not clean.

#### 6.8.5 Invariants #6 / #4 / #7 on the F3 build

- **#6 byte identity — PASS, MEASURED.**  sha256(`products.txt`) `nonint_base3h` = `nonint_base3h_f1exe`
  = `nonint_base3h_f3exe` = `1c1b60c7792e5f91ef6e4e2f659883603a9fc0ae8112d3a26f86c3355029229d`
  (8,601,257 B) — three exes, both knobs OFF.  ON arms differ (los3h `ccfa9eba…`, los3h_hv
  `bd16aca2…`, losblock3h `cdbeaee0…`, losblock3h_hv `b7cea2ad…`).
- **#4 run-end counters (armed only; `main_log.log:803` / `:796`).**  losblock3h: `Network (leveling
  local-block F3): 21180 block(s) whitened, 950551 row(s) emitted, 8534 column(s) marginalized at
  inject, 0 pair(s) zeroed (harvest-stamp mismatch), 0 column(s) refused, 0 factorization
  fallback(s); block dim histogram <10:0 10-24:221 25-49:15834 50-74:5125 75+:0 (max 66);
  within-epoch refloat re-whiten 0 ok / 0 failed`; hv 951726 / 8859 / dims 218 : 15773 : 5189.
  21180 = 60 sta × 353 epochs (every station every epoch from k = 7); the F1 fallback never fired;
  the refloat re-whiten path was never exercised (T2 354 opportunities, 0 flags) — uncertified.
- **#7 edges.**  No new event population; the PB-gauge flip is inherited (§6.8.3).  Gap: the 8,534
  marginalized columns are not traced by reason (§7 item 16).

#### 6.8.6 What remains, and the 24 h pre-registration (HYPOTHESIS, written before any 24 h F3 arm exists)

Remaining F3 residue on this window, in order: (a) the cold PB-gauge handover erase edge (~1.4× σ on
hit legs; 408 carries still erased at 1e-2 — §7 item 15); (b) an ignition residue on legs born at
k = 0 with no logged event (1.25–1.34×; §7 item 8); (c) the rx-CB direction's 2× over-count (§7 item
17); (d) L7Q's satellite-arc-mean frame term (§7 item 3); (e) the satellite-clock gap to F1 (§7 item 18).

**Pre-registered 24 h F3 + hv reading** (same bed; grade the last 25 %, where 99.5 % of rows are
mid-run-born): settled L1C med|e| **0.14–0.15 cyc / round0 84–86 %** (L2W ~0.19–0.20 / 79–80 %,
L5Q ~0.19–0.20 / 78–80 %, L7Q ~0.18–0.19 / 79–81 %), WL(1,2) ≥ 99 %, EWL 100 %, ZWD |err|/σ ~1;
with the 1e-4 ceiling (every handover cold, ~550 erased carries / h) **0.16–0.17 / 82–83 %**.
Basis: the h12–24 CRLB mixture 0.136 / 86.4 % × the measured mid-run-born excess 1.03 × multipath
1.04–1.10 → f = 1.03–1.10; the cold lineage adds 1.1–1.2× on hit legs → f ≈ 1.15–1.25 (factor
table f 1.00 / 1.03 / 1.10 / 1.24 / 1.44 → 0.136 / 0.141 / 0.150 / 0.169 / 0.197 cyc, 86.4 / 85.8 /
84.5 / 82.0 / 78.5 %).  **FALSIFIERS:** settled L1C > 0.20 cyc or round0 < 80 % on the hv posture
(an unmodelled floor); |e| rising with age beyond 360 (an erase / handover mechanism still
active); ZWD |err|/σ > 1.5.

#### 6.8.7 Provenance

Wire + consumer: `<scratchpad>/r4_wire/` (grade_*_s75.{txt,json}, series_*.csv, steps_*, quality_*,
diff_*, drift_*, consumer_{30min,5min}, consumer_at_handovers, handover_age, events_census,
sigma0_consumer, ddfrac_*, census.txt).  Residue verifier: `<scratchpad>/r4resid/` (grader_*.txt;
build_arm.py + arms/*/{dd_e,los,frame}.npz; a1..a5_nonint_losblock3h{,_hv}.txt; **crlb_pop.py — the
population-CRLB instrument**; crlb_24h, crlb_f1model, crlb_ref, leak_test, handover_hit, ev_raim).
Recipe (extends §8): F3 arms = the F1 arm + `network.phase_leveling_local_block: true` (+
`network.phase_datum_handover_max_var_m2: 0.01` for `_hv`); `python "<scratchpad>/r4resid/crlb_pop.py"`;
`grep "leveling local-block F3" runs/nonint_losblock3h/logs/main_log.log`;
`sha256sum runs/nonint_base3h{,_f1exe,_f3exe}/products.txt`.

---


