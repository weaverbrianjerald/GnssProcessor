# LV mask study: observation mask and iono cutoff at 61 and 303 stations, and item 1

**STATUS**

- **id:** LV mask study
- **topic:** ionosphere
- **status:** closed
- **summary:** The study CLOSES on obs mask 15, iono cutoff 25 at both scales: the cutoff owns the residual cost, the mask the footprint level; LV-4 alone passes stage 2, unpromoted.
- **dates in the text:** 2026-08-05
- **knobs / posture:** `estimation.ionosphere_weighting.elevation_cutoff_deg`, the observation mask (`elevation_mask_deg`); `rx_cb_carry_step_gate` (LV-4, item 1); `serialize_sbasis_contract` (section 25.7); new denominators arm 32 (61 sta) and arm 38 (305 sta)
- **hashes named in the text** (commit / exe / byte-cert): none; exes `gnss_processor_lv4stage1.exe` and `gnss_processor_f17.exe` (the build of Aug 5 2026 00:09)
- **run dirs named in the text:** `runs/ext63_dual_3h_m15i25`, `runs/ext63_dual_1h_lv4_m15i25`, `runs/ext305_dual_20min_m30i30`, `runs/ext63_dual_3h_lv4_m15i25`, `runs/ext63_dual_12h_`, `runs/replay_ext63_12h_`
- **source:** `docs/archive/SPEC_level_information_budget.md` sections 21, 21.1, 21.2, 22, 22.1, 22.2, 23, 23.1, 23.2, 23.3, 23.4, 23.5, 23.6, 23.7, 24, 24.1, 24.2, 24.3, 24.4, 24.5, 25, 25.1, 25.2, 25.3, 25.4, 25.5, 25.6, 25.7, 26, 26.1, 26.2, 26.3, 26.4

**Outcome.** The question: which observation mask and iono cutoff serve the SH level and the residuals. At 61 stations obs mask 10 to 15 bought 5 to 6 TECU of level and cutoff 25 to 15 cost +27 to 46 % postfit phase rms; at 303 the 15/25 cell tied 10/25 on rms and halved footprint error and breathing. LV-4's cost falls with the mask. Section 25.7 retracts the claim that the consumer has no reaction path. The re-founding later dropped the SH field from the producer ([NETWORK_REFOUNDING](../../refound/NETWORK_REFOUNDING_2026-09-14.md)).

> Results are appended TO THIS FILE, below the verbatim text. The archived monolith is frozen; never append there.

---
<!-- BEGIN VERBATIM: SPEC_level_information_budget.md -->
## 21. The mask 2×2 — the level gain and the residual cost come from DIFFERENT knobs (2026-08-05)

§20.3 changed both masks at once and its prediction was **FALSIFIED**, as was the companion prediction that
residuals would be flat-or-better. Because the two edits push opposite ways, a 2×2 was run — four 3 h
canonical EXT-63 arms, same build, same window, one knob apart:

| arm | obs mask | iono cutoff | a₀₀−GIM 3 h mean | sd | h0 | **rms_phase h2–3** | n_obs h2–3 | sh_health |
|---|---|---|---|---|---|---|---|---|
| 24 (old canonical) | 10 | 25 | ≈−9.31 | 5.53 | -15.83 | 0.00900 | 132 967 | 0/0/0 |
| 31 (the directive) | 15 | 15 | −3.02 | 5.66 | −6.75 | 0.01140 | 119 800 | 0/0/0 |
| **32** | **15** | **25** | **−4.10** | **4.42** | −7.23 | **0.00870** | 119 734 | **0/0/0** |
| 33 | 10 | 15 | **−14.48** | **13.07** | **−29.49** | 0.01310 | 133 039 | **58/2/58** |

**The two effects are cleanly separable and belong to different knobs.** **The LEVEL gain is the OBSERVATION
MASK**: at obs 15 the 3 h mean is -3.02 / -4.10 (iono 15 / 25), at obs 10 it is -9.31 / -14.48 — raising the
observation mask 10 → 15 is worth **~5–6 TECU of level** at either iono cutoff. **The RESIDUAL cost is the
IONO CUTOFF**: at iono 25 the h2–3 rms is 0.00900 / 0.00870 (obs 10 / 15), at iono 15 it is 0.01140 / 0.01310
— lowering the cutoff 25 → 15 costs **+27…46 % postfit phase rms** at either observation mask.

**⇒ Arm 32 (obs 15 / iono 25) dominates all four on BOTH axes.** Best residual of any arm including the old
canonical (0.00870 vs 0.00900, and 0.00800 vs 0.01020 in the 20–60 min window), a level 5.2 TECU better than
the old canonical, the LOWEST level sd of the four (4.42), 0/0/0 sh_health, and it keeps
`elevation_cutoff_deg` at the validated 25 so the posture check does not warn. **Arm 33 (obs 10 / iono
15) is the pathology, reproduced**: level 3 h mean -14.48 with sd 13.07, ignition hour -29.49, **58
    out-of-band epochs / 58 withheld emissions** — `project_global_sh_iono`'s 10°-mask divergence class at 61
    stations, a low observation mask feeding low-elevation code contamination into the SH constraint.

### 21.1 Why this is physically coherent

a₀₀'s only handle is mapping-function spread, so it needs LOW-elevation lines of sight — but those same LOS
carry code multipath, obliquity and near-horizon mapping error, and the two masks resolve that tension at
different places. The **observation mask** decides which LOS exist at all: raising it 10 → 15 removes the
worst multipath *before* it reaches any estimator. The **iono cutoff** decides which existing LOS may
constrain the SH field: lowering it 25 → 15 re-admits the 15–25° band as SH constraint rows, and those rows
fight the phase observations — what the +27…46 % postfit rms measures. §18's footprint/global inversion
(measured at 15/15: footprint −1.11 vs global -2.92, rms 4.85 vs 6.10, where 10/25 was -9.7 vs -6.1 and 11.2
vs 6.9) therefore belongs to the observation-mask half too. ⚠ That comparison is 3 h against §18's 12 h and is
NOT time-matched.

### 21.2 Recommendation

**obs mask 15 / iono cutoff 25** (arm 32). It captures the whole level gain the directive was after, avoids
the whole residual cost, and leaves the validated cutoff and its posture check intact. ⚠ Whatever is chosen,
**arm 32 or arm 31 becomes the new denominator** — every result in §9–§20 was measured at 10/25 and none of it
is comparable across this change.

---

## 22. ★ The mask trade is REGIME-DEPENDENT — it flips sign between 61 and 303 stations (2026-08-05)

§21 recommended obs 15 on a 61-station 2×2. The user's course-correction ("keep 15/15 but re-test at 305
stations first") was right and the retest **reverses the conclusion**. Matched pair, 303 stations, 1 h, SH deg
8, one variable — the mask posture:

| | EXT-305 @ 10/25 | EXT-305 @ 15/15 |
|---|---|---|
| a₀₀−GIM hour 0 | **-1.98** | **-14.78** |
| a₀₀−GIM run mean / sd | -1.99 / 3.64 | **−14.74** / 3.99 |
| postfit phase rms (20–60 min) | **0.00600 m** | **0.00990 m (+65 %)** |
| phase obs | 423 169 | 376 070 (-11 %) |
| sh_health | 0/0/0 | 0/0/0 |

**At 303 stations 15/15 is worse on BOTH axes** — the level degrades by 12.8 TECU and the residuals by 65 %.
At 61 stations the same change IMPROVED the level (-9.31 → -3.02):

| posture | 61 sta level | 61 sta rms | 303 sta level | 303 sta rms |
|---|---|---|---|---|
| 10/25 | -9.31 | 0.01020 | **-1.99** | **0.00600** |
| 15/15 | **-3.02** | 0.01540 | -14.74 | 0.00990 |
| 15/25 | -4.10 | **0.00800** | *see §26* | *see §26* |

Both predictions written into `_ext305_gps_dual_1h_m15i15.json` before the run are **FALSIFIED**: the residual
penalty did not shrink with 4.8× the observations (it grew, +51 % at 61 sta → +65 % at 303), and the level did
not land within ±1.5 TECU of zero (-14.78).

### 22.1 Why the sign flips — and why it was predictable from the original finding

**At 61 stations the field is information-starved**, so the 15–25° band's mapping spread is worth more than
its contamination and admitting it helps the level. **At 303 stations the field is already well constrained**
(a₀₀−GIM -1.98 at 10/25), so the same band adds mostly contamination, which dominates — the classic
low-elevation code failure. This is exactly the regime the 25° cutoff was ORIGINALLY set in:
`project_global_sh_iono`'s a₀₀ 296 → -305 TECU divergence was a **305-station** result. So the cutoff is a
large-network fix and §21's 61-station 2×2 was measuring a different regime — the 25° cutoff was the fix for
exactly this, and arm 33 shows the cutoff alone still holds the pathology at 61 stations too. **Do not
generalize a mask verdict across network size.** (Same shape as CLAUDE.md's lane-layer amendment.)

### 22.2 Standing recommendation

**`elevation_cutoff_deg` = 25 in BOTH regimes** — best cell at 61 stations (arm 32: rms 0.00800) and its
removal is catastrophic at 303. The OBSERVATION mask is the genuinely open knob: 15 helped at 61 stations, and
the **305 @ 15/25 cell is UNRUN** — the candidate for the best posture overall (~34 min). → **Run as arm 38,
§26.**

---

## 23. Roadmap item 1, re-run on the CURRENT denominator: §19's FAIL does not reproduce, and LV-4's cost is a FUNCTION OF THE MASK (2026-08-05)

§19 closed item 1 FALSIFIED at stage 1 on a single criterion — the SH health gate, 46 out-of-band epochs / 46
withheld emissions — measured inside the denominator §21 voided, on the axis the mask change moves most. So
item 1 was re-run at the corrected operating point. §19's mechanical claim was verified first and it
**stands**: with the de-mean off the loader forces LV-5 off
(`src/core/config/GnssConfigLoadNetwork.cpp:7510-7516`), so a config setting only `rx_cb_carry_step_gate`
genuinely *is* the item-1 posture. Independent instrument check: the grader used here reproduces §19.1's
published numbers to the digit off the same run directory (0.01670 m, a₀₀ 2.69 ± 1.59, min −0.76, 46/8/46,
97.5 % folds, 1.76 % erases).

### 23.1 Stage 1 at 61 stations, obs 15 / iono 25 — arm 34

`_ext63_gps_dual_1h_lv4_m15i25.json` vs arm 32 (`runs/ext63_dual_3h_m15i25`, whose first hour is the matched
control). Same build, same window, ONE knob apart, and — unlike §21's mask arms — **identical observations**:
78 931 phase obs / 4935 STA rows in both.

| §17 stage-1 gate | threshold | arm 34 @ 15/25 | §19's arm @ 10/25 |
|---|---|---|---|
| folds | ≥ 95 % | 98.9 % (7295/7374) | 97.5 % |
| erases | ≤ 2 % | **0.25 %** (18, max z 107.7) | 1.76 % (126, max z 744.7) |
| settled sd | ≤ 5 TECU | 1.42 | 1.58 |
| 3-epoch alternation | none | none (flip 0.29 vs control 0.34) | none |
| **sh_health** | **0/0/0** | **0/0/0 — PASS** | **46/8/46 — FAIL** |

**§19's stated cause of death does not reproduce.** All five §17 gates pass. The level effect **inverted sign
with the observation mask** — not predicted (the config predicted LV-4 would again push the level DOWN by the
same −14.8 TECU it moved it at 10/25): LV-4 alone reads **−28.37 ± 5.22** at 10/25 and **−2.57 ± 2.92** at
15/25, against the arm-32 control's -7.23 ± 2.83 (a₀₀−GIM, hour 0). ⚠ Both level figures are FRAME quantities
(invariant #5 / §19.6) read at 1 h, inside §14's ~2 h convergence budget — **diagnostic, not a grade.** What
is a grade is the postfit phase rms, and there LV-4 still costs: **0.00800 → 0.01080 m, +35 %.**

### 23.2 The mask/scale extreme at 303 stations — arms 35 / 36

`_ext305_gps_dual_20min_m30i30{,_lv4}.json`, obs mask 30 / iono cutoff 30, 20 min (40 epochs), 303 stations.
At 30/30 the iono cutoff stops binding (every surviving LOS is already ≥ 30°), so this is "the 15–30° band
deleted outright". Verified one-knob at both file and resolved-config level. Identical observations: 98 970
phase obs / 8977 STA rows in both.

| | arm 35 control | arm 36 LV-4 |
|---|---|---|
| **postfit phase rms** | **0.00730 m** | **0.00850 m (+16.4 %)** |
| a₀₀ (ep 10–40) | 28.52 ± 3.09 | 29.52 ± 2.19 |
| a₀₀−GIM run mean / sd | −6.65 / 6.12 | −5.43 / 5.53 |
| a₀₀ posterior σ | 0.4904 | 0.4096 |
| folds | 3609 / 11 966 (30.2 %) | 11 658 / 11 966 (97.4 %) |
| erases | — | 8 (0.07 %, max z 21.2) |
| retired screen would have withheld | — | 81.5 % |
| sh_health | 0/0/0 | 0/0/0 |

The **a₀₀ posterior σ against its own 32.24 ± 2.0 prior** was armed as a circularity trap before the run — at
a 30° mask a "good" level could just be the prior talking, and the prior sits within 0.3 TECU of the GIM 00h
global mean (32.0). It does not fire: 0.41–0.49 against 2.0, still firmly data-dominated (61-sta arms run
0.65–0.71, the 305 @ 10/25 arm 0.42). Mask starvation of a₀₀ is visible but mild — σ 0.42 → 0.49 for +20° of
mask.

### 23.3 The finding: LV-4's data-side cost is a function of the observation mask

Three one-knob pairs, each over identical observations within the pair:

| pair | masks | stations | control | LV-4 | cost |
|---|---|---|---|---|---|
| §19.3 | obs 10 / iono 25 | 61 | 0.01020 | 0.01670 | **+64 %** |
| arm 34 | obs 15 / iono 25 | 61 | 0.00800 | 0.01080 | **+35 %** |
| arm 36 | obs 30 / iono 30 | 303 | 0.00730 | 0.00850 | **+16 %** |

Monotone, and it is what §19.3's own mechanism predicts: the least-supported carry information is the
low-elevation code that sets the receiver DCBs; remove that code and the cost shrinks — by 4× across the range
tested. ⚠ **The confound, stated plainly: arm 34 → arm 36 moves BOTH the mask and the station count.** The
61-station step (10 → 15, +64 → +35 %) is clean; the step to +16 % is not attributable to the mask alone.
Separating it needs a 305 @ 15/25 LV-4 pair or a 61 @ 30/30 pair; neither is run.

**What this does and does not change:** §19's *measurement* stands everywhere it was taken, but its **headline
number is the worst cell on the axis**, not a constant of the mechanism — "LV-4 costs +64 %" is an
obs-mask-10, 61-station statement. §19.5's "item 1 is closed FALSIFIED" **no longer follows from the evidence
given for it**: its sole stage-1 failure was the SH band, and the band passes at both current operating
points. Item 1 is nevertheless **not recommended for promotion** on the invariant-legal axis — in every cell
measured LV-4 costs the data side and never pays for itself; at its best (+16 %) it buys ~1.2 TECU of level (a
frame quantity) and slightly lower breathing. The honest verdict is **"re-opened, ungraded past 1 h, and still
paying a real residual cost"** — not "falsified", and not "promote". §17's staged escalation is **earned**:
stage 1 passes at 61 sta @ 15/25. → Stage 2 spent, §24.

### 23.4 The §13.5 / §17-item-2 audit does not resolve

Arm 34 vs arm 32, settled 20–60 min:

| instrument | control | LV-4 | |
|---|---|---|---|
| prires `rxcb` σ̂₀ | 0.63 | 3.11 | 5× LOUDER (prior side) |
| postfit phase rms | 0.00800 | 0.01080 | +35 % LOUDER (data side) |
| congruence σ̂₀ | 126.3 | 38.7 | 3× QUIETER |
| block-Baarda σ̂₀ | 33–78 % floored at 1.0 | | INERT at 1 h — §19.4 |

Two instruments say worse, one says better, one is inert. **This is the same split §19.2 recorded and it is
still unexplained — do not resolve it editorially.** §19.4's two instrument holes were re-confirmed on this
lineage: `network_prior_residual.csv` emits only the `rxcb` family of the five armed, and
`network_pf_keyed.csv` has `n_code == 0` in every row.

### 23.5 A third cell for §22's mask axis, and what it implies for §22.2's open question

Arm 35 adds a third 305-station cell that is useful because at 30/30 **the iono cutoff is non-binding**, so it
reads the observation-mask cost with the cutoff confound removed:

| 305-sta cell | a₀₀−GIM mean / sd | postfit phase rms | phase obs |
|---|---|---|---|
| 10 / 25 | **-1.99 / 3.64** | **0.00580** | 166 753 |
| **30 / 30 (arm 35)** | **-6.65 / 6.12** | **0.00730** | 98 970 |
| 15 / 15 | -14.74 / 3.99 | 0.00840 | 148 831 |

(rms over ep 10–40 for window-matching with the 20 min arm; §22's 20–60 min figures differ in window, not
direction. Observation sets differ across mask — read direction only.) Raising the observation mask 10 → 30 at
303 stations, with the cutoff held harmless, costs **4.7 TECU of level, +26 % rms, and 41 % of the
observations**, and nearly doubles the level's epoch-to-epoch sd (3.64 → 6.12). **This cell predicts the unrun
305 @ 15/25 will lose to 10/25.** → **The prediction was right about the global number and wrong about what it
meant; §26.**

### 23.6 Predictions, scored

| arm | prediction | outcome |
|---|---|---|
| 34 | rms +60 % (0.0125–0.0135) | **MISSED** — +35 % (0.01080).  Direction right, magnitude wrong; the cost is mask-dependent (§23.3) |
| 34 | sh_health PASSES, level ~9.5 via LV-4's -14.8 offset | **half right for the wrong reason** — PASS confirmed, but LV-4 moved the level **UP** +6.7; the sign inverted with the mask |
| 34 | folds ~97.5 %, erases ~1.7 %, sd ≤ 5, no alternation | **CONFIRMED** (98.9 %, 0.25 %, 1.42, none) |
| 35 | the level gain does NOT continue to obs 30 | **CONFIRMED** — -6.65 vs 10/25's -1.99 |
| 35 | a₀₀ observability starves toward the 2.0 prior | **PARTIAL** — σ 0.42 → 0.49, real but mild; no circularity |
| 36 | LV-4 cost +10…25 %, below arm 34's +35 % | **CONFIRMED** — +16.4 % |
| 36 | LV-4 level above control by a few TECU | **CONFIRMED** — +1.00 TECU (29.52 vs 28.52) |

### 23.7 Provenance

Build `gnss_processor.exe` Aug 5 2026 00:09 (run as `gnss_processor_lv4stage1.exe`, the renamed-copy
convention) — the same binary as arms 31/32/33 and §22's 305 pair, so every comparison above is same-build per
invariant #6. Configs `_ext63_gps_dual_1h_lv4_m15i25.json`, `_ext305_gps_dual_20min_m30i30{,_lv4}.json`; runs
`runs/ext63_dual_1h_lv4_m15i25`, `runs/ext305_dual_20min_m30i30{,_lv4}`. Wall: 99 s, 338 s, 340 s. ⚠ **The
`extends` guard is 8 levels and the EXT-305 chain already sits at that depth.** Arm 36's first attempt died at
load ("chain deeper than 8 levels — refusing to continue"); the fix is to extend the same base as the sibling
arm and restate the overrides, which also makes the one-knob claim structural rather than inherited. (§6
carries the 61-sta version of the same rule.)

---

## 24. Item-1 STAGE 2 passes every gate — and sharpens the trade to its final form (2026-08-05)

`_ext63_gps_dual_3h_lv4_m15i25.json` (arm 37), 3 h fresh seamless lineage, vs the arm-32 control
(`runs/ext63_dual_3h_m15i25`), same 00:09 binary (run as `gnss_processor_f17.exe`), ONE knob apart, and — the
pair's decisive control — **byte-matched observation sets in every window** (77 988 phase obs in the settled
20–60 min slice, 119 734 in h2–3, 21 949 harvests; identical in both arms). Predictions were written into the
config before the run; scored in §24.4.

### 24.1 The stage-2 gate, measured

| §17 stage-2 gate | threshold | arm 37 | control |
|---|---|---|---|
| folds | — | **99.6 %** (21 866/21 949, 0 gate-withholds) | 44.2 % (12 191 withheld by the legacy screen) |
| erases | ≤ 2 % | **0.10 %** (22, max z 107.7 — the same first-hour event arm 34 saw) | — |
| settled sd | ≤ 5 TECU | 1.81 | 4.94 |
| 3-epoch alternation | none | none (flip 0.44 vs 0.37) | — |
| sh_health | 0/0/0 | **0/0/0** | 0/0/0 |
| h2–3 time-matched level | inside ±4..9 | **−0.67 ± 2.09** | +0.44 ± 1.30 |
| breathing | ≤ control's | run sd **2.40** | 4.42 |
| over-retention signature | absent | absent (§24.3 — BOTH sides louder, not the lane trade) | — |

**PASS on every clause. Stage 3 (12 h) is earned.**

### 24.2 The level channel: the best tracker any 61-station posture has shown

Time-matched hourly (`gim_global_mean_series.py`), a₀₀ − GIM(t): control (arm 32) -7.23 ± 2.83 / −5.52 ± 4.06
/ +0.44 ± 1.30 at hours 0/1/2, run mean -4.10 sd 4.42; **arm 37 (LV-4)** -2.57 ± 2.92 / −0.58 ± 1.37 / -0.67 ±
2.09, run mean **−1.27** sd **2.40**. No ramp (the §14 stack pathology is absent), half the breathing, hourly
means within ±0.7 TECU of the GIM from hour 1 on. **§19.1's "the same memory at 97.5 % admission drags the
level out of the physical band" was an obs-mask-10 artifact top to bottom**: at the current denominator, full
admission is the best level behaviour measured at this scale. (Diagnostic under invariant #5.)

### 24.3 The data side: +32 % persists settled, and every loud instrument agrees

| instrument (h2–3, identical obs) | control | arm 37 | ratio |
|---|---|---|---|
| **median rms_phase** (pf_keyed STA) | **0.00870 m** | **0.01150 m** | **+32.2 %** |
| median rms_phase, settled 20–60 min | 0.00800 m | 0.01080 m | +35.0 % (reproduces arm 34 exactly) |
| prires `rxcb` σ̂₀ (settled → h2–3) | 0.63 → 0.95 | 3.10 → **6.33** | 4.9× → 6.6× |
| block-Baarda G σ̂₀ (0 % floored, both) | 2.21 | 3.59 | 1.62× |
| congruence σ̂₀ | 63.7 | 26.3 | **0.41× (quieter — the lone dissenter, again)** |

Two new facts beyond §23.4: **block-Baarda's first usable read on an LV-4 pair** (3 h unfloors it — 0 %
floored in both arms at h2–3) lands on the data-side-louder camp; and the prior-side tension is **growing with
era** in the LV-4 arm (3.10 settled → 6.33 at h2–3, a doubling in two hours) where the control grows 0.63 →
0.95. Whether that compounds toward the §14 stack's 12.62 or plateaus is stage 3's fourth prediction — the
number that matters for item 3's design. The over-retention (lane-A/B) signature — quieter priors bought with
louder data — is absent: this posture is louder on BOTH sides while tracking the level best. **The trade in
its final, un-confounded form: the full-admission rolling carry buys the level channel at the price of fitting
both its priors and its observations worse.**

### 24.4 Predictions, scored

| # | prediction (in the config, pre-run) | outcome |
|---|---|---|
| 1 | h2–3 rms 0.0110–0.0125 (+26..44 %) | **CONFIRMED** — 0.01150, +32.2 % |
| 2 | level inside band, above control +2..6, sd ≤ 4.42 | **mostly** — band and sd PASS; above control at h0/h1 (+4.7/+4.9), crosses at h2 (-0.67 vs +0.44) |
| 3 | folds ≥ 98 %, erases ≤ 0.5 %, no alternation | **CONFIRMED** — 99.6 %, 0.10 %, none |
| 4 | prires 3–6×; congruence quieter; block-Baarda records a side | **CONFIRMED** — 4.9–6.6×; 0.41×; louder (1.62×) |

### 24.5 Standing after stage 2

Item 1 remains **not a promotion candidate** on the invariant-legal axis (§23's verdict, now with a settled 3
h confirmation), and simultaneously the **strongest evidence yet for §13.4/§15's tracking thesis**: what
tracks the level is admitted per-epoch code GF information flowing through a rolling memory. **LV-4-alone
demonstrates the authority exists; its cost says the carry is the wrong VEHICLE for it.** That is precisely
item 3's design brief ("S-consistent harvest" — per-epoch data-derived frame, no fold-side tax), which stage
3's σ̂₀-growth read (P4) will parameterize. Provenance: run `runs/ext63_dual_3h_lv4_m15i25` (54 traces
harvested), wall 248 s.

---

## 25. The excursion attribution: the product wire is MARK-FREE, the churn is posture-invariant, and the consumer has no reaction path anyway (2026-08-05)

§17 item 0's follow-up on the 12 h pair (`runs/ext63_dual_12h_{ref,lv125}` producers,
`runs/replay_ext63_12h_{ref,lv125}` consumers). The first finding is about the instrument.

### 25.1 There ARE no R/disc events — the wire carries values and nothing else

Both 12 h products contain exactly four record kinds: **45 311 C, 1 440 E, 1 440 H, 144 164 P.** Absent or
inert in BOTH arms:

- **zero `R` records** (per-satellite discontinuity bump), **zero `phase_disc` / `clock_disc` / `code_disc`
  optional fields**, **`datum_changed = 0` on all 1 440 E records** — through consumer excursions reaching 546
  m and 979 m;
- **no `S` contract record at all** on this lineage's wire;
- the `H` heartbeat reads `status=ok` with **σ₀ = 0.000 on every epoch** — its σ₀ feed is the within-epoch
  RAIM, carry_warm-gated and therefore INERT without phase_leveling: the standard-set's known gap, now a dead
  instrument ON THE WIRE;
- **every `P` record advertises `sigma=` ≤ 0.56 m** (mostly the 0.1 floor) — including epochs where its value
  steps by hundreds of metres (§25.2).

CLAUDE.md amendment 5's warning ("without `emit_clock_disc` the clock jumps are silent to the consumer")
generalizes to **every channel of this lineage's product**. The emitter HAS the machinery (per-sat disc
counters, `disc_threshold_m`, `emit_clock_disc`, set-change tracking — `NetworkProductEmitter.cpp:75-136`);
none of it reaches the wire. **The producer's event knowledge exists only on the producer's side.**

### 25.2 The step census (the disc record, computed after the fact)

Mid-arc steps in emitted values between CONSECUTIVELY-PRESENT epochs (gap re-inits excluded — those are
legitimate re-datums the consumer re-initializes through). Instrument promoted to
`scripts/product_step_census.py` — **run it on ANY product-affecting change**; a posture that moves the
mid-arc census has changed the consumer's world whatever the solve-side grades say:

| | ref | lv125 |
|---|---|---|
| phase-bias key-steps > 0.1 m | **387 on 244 epochs (16.9 % of epochs)** | 385 on 245 (17.0 %) |
| median / p90 / max \|step\| | 2.66 / 42.4 / **287.6 m** | 3.19 / 45.1 / 255.0 m |
| clock common-mode steps > 0.1 m | **0** | **0** |
| per-sat clock outliers > 0.5 m | **0** | **0** |

1. **The emitted PB stream steps constantly and enormously, unmarked** — a multi-metre unmarked step lands
   somewhere in the constellation about every sixth epoch, all under σ=0.1-class advertised uncertainty. The
   paired L1/L2 signature of the giants (G17 −215/−288 m at h4.99, G19, G09, G06…) is integer re-basing on
   reference-path slips, which the emit re-base preserves BY DESIGN (40 ref-path slip steps per 3 h; "91 % of
   emitted PB discontinuities are ref-path re-referencing", CLAUDE.md). The design assumes a datum step is
   harmless if marked or differenced away; on this wire it is neither. The census ties out against the
   producer's own counters — the PB emission-continuity filter reports **394 ref-path-slip resets in BOTH
   arms** (of 462/458 total), and the wire census independently recovers 387/385: **the producer counts every
   step it ships and marks none of them.**
2. **The churn is posture-invariant**: same satellites, same TOWs, same magnitudes, same 394 count in both
   arms — the LV stack neither causes nor cures it.
3. **The emitted clock is smooth on this lineage** at emission granularity — amendment 5's "13 product-wide
   clock jumps" belong to a different (phase-leveled) lineage; under EXT-CB the phase-bias channel carries the
   churn instead (free per-(sat,band) PBs make phase rows clock-blind).

### 25.3 Excursion onsets align with RE-REFERENCING EVENTS, not with value steps

Consumer excursion windows (err3d > 20 m) with producer-side onset context (−120 s..+60 s) and enrichment vs
each event's 12 h base rate:

| arm | window | peak | onset enrichment (× base) | PB steps at onset |
|---|---|---|---|---|
| ref | h1.51–1.68 (22 ep) | 308 m | **col_removed 12.7×**, refsat_reset 2.5× | 0 |
| ref | h3.30–4.70 (169 ep) | **546 m** | **refsat_reset 5.0×** | 2 |
| lv125 | h2.41–2.67 (32 ep) | **979 m** | eq34_reattach 2.1× | 4 |
| lv125 | h4.27–4.58 (38 ep) | 660 m | refsat_reset 5.0×, block_outlier 1.7× | 3 |
| lv125 | h6.48–6.60 (15 ep) | 293 m | col_removed 5.1× | 0 |
| lv125 | h9.51 (3 ep) | 42 m | **sbc_engage 7.6×**, refsat_reset 5.0×, col_removed 5.1× | 1 |
| lv125 | h10.47–11.99 (184 ep) | 22.8 m steady | col_removed 7.6× | 1 |

PB-step density INSIDE excursions is at base rate (0.27/ep) and clock steps are zero — the "a big product step
caused it" story fails. What marks the onsets is the **event class that changes which datum realizes the
product**: refsat resets, topology column removals, SBC gate engagements, eq34 re-attachments — all invariant
#7 edges, none with any wire representation. Consumer AR is essentially never fixed inside excursions (≈1
epoch per window; 0.3 % fix rate overall), so these are FLOAT-side failures and wrong-fix poison is ruled out.
Honest caveat: onset windows hold 2–6 events each, so the 5–13× enrichments are suggestive, not
mechanism-proof; the load-bearing facts are structural (mark-free wire, endemic unmarked steps, float-side
failures, §25.4).

### 25.4 The h9.5–11.5 storm: common stressor, differential response

Both producers show the SAME storm at h9.5–11.5 on the same observations — refsat_reset 0.142/ep (5–7× base)
and sbc_engage 0.154/ep (4.4×), epoch-for-epoch — yet the consumer response differs 3×: median 3D **6.72 m
(ref)** vs **20.72 m (lv125)**, the latter with p95 ≈ median over 184 contiguous epochs > 20 m, i.e. a stable
bias during exactly §14's pin-ramp era. This is §17 item 0's "the late-era stack product is INTERNALLY
inconsistent" with the stressor identified: the storm's re-referencing is the moment the pinned rx-CB frame
and the drifting iono/clock content get pulled apart into estimable space.

### 25.5 What this changes (and does not)

1. ~~**The wire needs its event channel armed** — but that alone is INERT downstream: the closed-loop /
   replay consumer has **no reaction path to any per-satellite disc**.~~ **RETRACTED — FALSE, see §25.7.** The
   card is two-sided by construction: emit the marks (producer, cheap, machinery exists) + react to them
   (consumer seam). Until both exist, "adapt-and-EMIT" is invisible at the consumer — invariant #7's consumer
   edge, formalized.
2. **Churn is confirmed as the campaign blocker and it is posture-independent** — no LV/level decision
   changes it. Level cards and the churn card are separable work streams.
3. The mark-free H heartbeat (σ₀ ≡ 0) upgrades the standing-set note "within-epoch tier is carry_warm-gated ⇒
   inert without leveling" from a producer-side caveat to a **consumer-facing defect**: the one health signal
   on the wire is structurally dead on any non-leveled lineage.

### 25.6 Item 0's diagnostic rider, discharged: the fleet-bias mean does NOT drift diurnally

Estimator-free hour-bucketed sweep (`extcb_gim_verdict.py` instrument via `scripts/fleet_bias_diurnal.py`: raw
RINEX + COD SP3/OSB vs the day's hourly GIM maps, same 30-station subsample, el > 20°) — fleet bias mean /
median / std by hour: 00 −14.76 / −22.92 / 21.50; 02 −14.31 / −22.28 / 22.03; 04 −15.26 / −21.99 / 21.56; 06
−16.00 / −23.28 / 20.51; 08 −15.35 / −23.79 / 22.18; 10 −16.15 / −23.91 / 22.81 — hour 0 reproduces the
root-cause memo to the digit — instrument closure.) **The mean moves −1.4 slant TECU in ten hours** (median
−1.8) — quasi-static against the level channel's ±6 TECU breathing and §14's −4..−15 wander. Per-station drift
is ±2–5 TECU sd and the tail is geography-coded, not hardware-coded: the largest drifters (**HKSL −16, SGOC
−10, GAMB +7, COCO −6**) are all equatorial-anomaly-band stations, i.e. GIM misfit moving through local time —
the SPREAD/dipole channel, not the mean. Two conclusions: **the -5 ns convention offset is stable hardware**
(the zero-mean-datum analysis stands as a STATIC frame choice), and **the a₀₀ breathing is estimator
realization noise, not receiver physics** — consistent with §24.2, where doubling the admitted information
halved it.

### 25.7 CORRECTION (2026-08-05, same day): §25.5's central claim was FALSE — the reaction path exists, fires, and doesn't touch the excursions

Caught by the user; measured by the probe arms (`_ext63_replay_12h_ref_{twin,ctl,rtlog}.json`, runs
`replay_ext63_12h_ref_{twin,ctl,rtlog}`). **The current statement of edge 1 is
`HANDOFF_edge_response_queue.md`**; this records what changes in §25's own claims.

1. **"No reaction path" — RETRACTED.** `Application::runConsumerReplay` drives a `RealTimeEngine`
   (`ApplicationConsumerReplay.cpp:222`) and `processEpoch` calls all five `RealTimeEngineSsrBreaks` handlers
   unconditionally (`RealTimeEngine.cpp:479-483`). In the very arm §25 graded, the phase-bias-disc reaction
   fired **83 ambiguity resets over 12 h** — invisibly, because the `rt_engine` logger defaults OFF (the
   `logging_config_.enable_rt_engine = lg.value("enable_rt_engine", false)` line, live at
   `GnssConfigLoad.cpp:3069`; this doc's older `GnssConfigLoad.cpp:3075` has drifted) and its log was 0 bytes.
   Twin run with the logger armed: solve byte-identical (`closed_loop_trace.csv` /
   `consumer_replay_hourly.csv` md5-equal), 83 resets visible. **Method lesson, now a standing corollary of
   invariant #4: an empty log is not an inert path — check the logger's enable flag, and verify absence of a
   BEHAVIOR at the call-graph level, not by grepping one file.**
2. **"Mark-free wire" (§25.1) — stands for the marks, with two refinements.** The phase channel needs no
   serialized mark: `NetworkProductReader::replay` re-derives disc counters by replaying values through an
   emitter (`emitToStore`), which is why the reaction lives. And the wire is not merely unread-able but UNREAD
   in one case: **all 1 440 H heartbeat records are dropped by the batch replay reader** (only
   `NetworkProductTail::lastStatus` surfaces H; replay does not use the tail).
3. **A real producer-side defect found by the probe** (edge-queue §1.3): the v1 wire carries no S contract
   (`serialize_sbasis_contract` defaults false), so the replay emitter falls back to a flat 0.05 m disc
   threshold while the producer ran 0.25·λ — **producer and consumer count different events** (45 spurious
   band-2 resets, 5 missed band-1, of 437 producer-true events). Fix is arming that one default-OFF knob; **NO
   code change** — the flat threshold is the symbol `NetworkProductEmitter::kDefaultDiscThresholdM` (declared
   `include/products/NetworkProductEmitter.hpp:160`, used to construct the emitter at
   `src/core/app/ApplicationNetworkSetup.cpp:969`, restated for the serialized contract at `:1088` — single
   source of truth, no config knob). *(Earlier drafts of §25.7 and §27 cited `ApplicationNetworkSetup.cpp:950`
   and `ApplicationNetworkSetup.cpp:1051`; both were drifted. Prefer the SYMBOL.)* **EXECUTED AND PASSED
   2026-08-05** (arms `ext63_dual_3h_contract` vs arm 32, replays `replay_3h_{v1ctl,v2}`): arming the knob is
   header-only with the E/C/P record stream **byte-identical** (0 diff lines, invariant #6 holds) and removes
   **exactly** the predicted spurious band-2 population — 17 → 13 resets, all four L2W, L1W unchanged — worth
   **last-30 3D RMS 11.68 → 10.20 m (-12.7 %)** on identical observations. Arm it on any producer run that
   will be replayed or tail-consumed. Correction recorded there: a v1 wire defaults ALL FIVE replay-emitter
   parameters, but the clock channel's silence is NOT a transit artifact — the clock counter has no magnitude
   jump test, so with `emit_clock_disc=1` correctly carried it still fires 0 for want of an EVENT.
4. **The load-bearing negative result: the 83 live reactions have ZERO enrichment at excursion onsets** (0
   resets at both onsets; 0.91× inside windows). §25.3's conclusion survives and sharpens: the excursion
   driver is the re-referencing event class, which has no wire representation, and no amount of PB-disc
   marking/reacting reaches it. §25.5's original success criterion ("excursion mass drops") is retracted for
   that channel; the revised criterion is onset ENRICHMENT of a new event-class record (edge-queue §1.6).
5. **⚠ Scope caveat on all §25 replay numbers: the replay arms ran at the void 10/25 posture** (07:52, before
   the 09:01 root-mask edit). Reproduction requires restating `elevation_mask_deg: 10` +
   `elevation_cutoff_deg: 25` (the twin config does). Measured while proving the twin: the consumer is
   bit-identical under the iono cutoff (15 vs 25) and moves with the OBS mask (last-30 RMS 2.78 m at 15 vs
   6.70 m at 10) — and **invariant #6 (same-build) is INSUFFICIENT for pairing: pair on the resolved CONFIG
   CHAIN as well**, because a concurrently-edited shared base re-postures every descendant silently.

---

## 26. The 305 @ 15/25 cell: a data-side TIE, and the footprint INVERTS the level verdict — the mask study closes on a unified posture (2026-08-05)

`_ext305_gps_dual_1h_m15i25.json` (arm 38) — the cell §22.2 nominated and §23.5 predicted would lose to 10/25.
303 stations, 1 h, same 00:09-family binary (`gnss_processor_f17.exe`), one knob vs the m15i15 sibling (cutoff
15 → 25 + paths; phase obs 375 563 vs 376 070, −0.13 %, not material). Wall 2124 s.

### 26.1 The completed 305 factorial

20–60 min settled window, matched across all cells; level from `gim_global_mean_series.py` (time-matched);
footprint from `field_vs_gim_footprint.py --radius 5 --every 1` with the RUN'S OWN 303-station config (⚠
§26.4):

| 305 cell | rms_phase | ep10–40 rms | a₀₀−GIM global | **footprint(5°)** | breathing sd | a₀₀ post-σ | sh |
|---|---|---|---|---|---|---|---|
| 10/25 | **0.00600** | 0.00570 | **-1.99** | −6.34 (rms 6.81) | 3.64 | 0.433 | 0/0/0 |
| 15/15 | 0.00990 | 0.00830 | -14.74 | −3.22 (rms 3.95) | 3.99 | 0.281 | 0/0/0 |
| **15/25 (arm 38)** | 0.00620 | **0.00560** | −5.81 | **−3.51 (rms 3.90)** | **1.86** | 0.433 | 0/0/0 |

**Data side: a statistical tie.** Restoring the validated cutoff recovers ~95 % of 15/15's +65 % penalty
(0.00990 → 0.00620 vs 10/25's 0.00600); each of 10/25 and 15/25 wins one window by ≤ 3 %. §22's "at 303
stations 15/15 is worse on BOTH axes" was a CUTOFF statement, not an obs-mask statement.

**Level: the global mean and the footprint DISAGREE, and the footprint is the honest instrument.** On the
global mean, §23.5's monotone prior held (-1.99 → -5.81 for obs 10 → 15). Over the network's own pierce points
it reverses: arm 38 -3.51 vs 10/25's -6.34, footprint rms halved (3.90 vs 6.81). §18's structure ("the
residual is ~3× larger over the footprint than globally, in every arm") is therefore an **obs-mask-10 artifact
at scale**: at obs 15 the inversion flips, in BOTH obs-15 cells (15/15's footprint is -3.22 — its catastrophic
−14.75 global mean is almost entirely the extrapolated hemisphere). Decomposition, coherent with §21.1:

- the **obs mask owns the footprint** — the low-elevation code that damages the field lives exactly where the
  network has data (obs 10 → footprint -6.3; obs 15 → -3.2..-3.5);
- the **cutoff owns the residuals and the extrapolated hemisphere** — cutoff 15 lets the 15–25° band fight
  the phase data (+60 % rms) and warps the Kaula-extrapolated side (global −14.7);
- **10/25's celebrated -1.99 global mean is won by CANCELLATION** between a bad footprint and the
  opposite-signed unsupported hemisphere — the best-looking global number in the factorial belongs to the arm
  with the worst field where consumers are. (15/15's a₀₀ posterior σ 0.281 — tightest formal confidence, worst
  level — is the same lesson from the other side: contaminated rows talk confidently.)

Stability is not close: arm 38 halves the breathing against both siblings (run sd 1.86, settled 1.64) —
consistent with §24.2's reading (breathing is realization noise; cleaner admitted information quiets it).

### 26.2 The mask study CLOSES: one posture serves both scales

**obs 15 / iono 25** is best-or-tied at BOTH scales: at 61 stations it dominates outright (arm 32 — best rms
of any arm, +5 TECU of level); at 303 it ties the rms, wins the footprint level ~2×, wins breathing 2×, keeps
the validated cutoff, and the posture check stays silent. The only axis 10/25 still wins is the global-mean
diagnostic — which §26.1 shows is won by cancellation. §22's "do not generalize a mask verdict across network
size" survives as method; its practical sting is gone: the scale-dependence belonged to the CUTOFF arm of the
2×2.

**Recommended config action** (not taken here — the masks were set by directive): the lineage root
`_ft63_24h_conv.json` currently pins 15/15; the one-line edit
`estimation.ionosphere_weighting.elevation_cutoff_deg: 15 → 25` lands the unified posture everywhere, is
validated at both scales, and un-warns every network run. **New denominators for all future comparisons: arm
32 (61 sta) and arm 38 (305 sta).**

### 26.3 Predictions, scored

| # | prediction (in the config, pre-run) | outcome |
|---|---|---|
| 1 | rms 0.0062–0.0072, better than 15/15, worse than 10/25 | **CONFIRMED** at the good edge — 0.00620 (+3.3 % vs 10/25); recovery larger than predicted, and arm 38 WINS the ep10–40 window |
| 2 | level -2..-7, does not beat 10/25's -1.99 | **CONFIRMED on the global mean** (-5.81) — and exposed as the wrong instrument: the footprint verdict reverses (-3.51 vs -6.34) |
| 3 | sh_health 0/0/0; obs ≈ m15i15 within ~1 % | **CONFIRMED** — 0/0/0; -0.13 % |
| 4 | 10/25 remains the best 305 posture on the grade | **MISSED in substance** — it is a tie on the grade, and 15/25 wins every consumer-relevant axis |

§23.5's monotonicity prior was right about the number it named and wrong about what the number meant. **A
verdict read off a FRAME quantity (the global a₀₀) at one operating point does not survive a better
instrument.** Level claims from here on quote BOTH the time-matched global series and the footprint
restriction.

### 26.4 Instrument note (invariant #4, near-miss)

`field_vs_gim_footprint.py` run bare on a 305 run **silently uses its default 61-station `--config` for the
footprint AND reads only epoch 1** (`--every` default) — it printed a plausible-looking, wrong table before
the right flags were found. **Any future footprint quote must carry `--config <the run's own network> --radius
5 --every 1`.** (The §20.2/§18 numbers were produced with correct flags; this note is for the next session
that isn't.)

---

