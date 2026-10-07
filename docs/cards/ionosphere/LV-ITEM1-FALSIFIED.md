# LV item 1: LV-4 alone at stage 1 (FAIL at the old masks, since retracted) and the audit

**STATUS**

- **id:** LV item 1
- **topic:** ionosphere
- **status:** superseded
- **summary:** LV-4 alone FAILED stage 1 on the SH health gate at obs mask 10 and carries the ladder's data-side cost; the mask study's section 23 retracted the FALSIFIED verdict at the current masks.
- **dates in the text:** 2026-08-05
- **knobs / posture:** `network.rx_cb_carry_step_gate` alone (LV-4; with the de-mean off the loader forces LV-5 off), arm `_ext63_gps_dual_1h_lv45.json`; obs mask 10 / iono cutoff 25
- **hashes named in the text** (commit / exe / byte-cert): none (the audit arms share the build of Aug 4 2026 23:27:25)
- **run dirs named in the text:** none as `runs/` paths
- **source:** `docs/archive/SPEC_level_information_budget.md` sections 19, 19.1, 19.2, 19.3, 19.4, 19.5, 19.6

**Outcome.** The question: does LV-4 alone (de-mean off) pass the 1 h stability gate? Four gates passed; SH health failed (46 of 121 epochs out of band). The section 13.5 audit put the cost on LV-4: every arm carrying it read +64 % postfit phase rms (0.0167 against 0.0102 m), and two instrument holes surfaced (`rxcb`-only prior residuals, no code rows in `pf_keyed`). At obs mask 15 the FAIL does not reproduce, the cost falls to +35 % and stage 2 passes, unpromoted ([LV mask study](../ionosphere/LV-MASK-STUDY.md) sections 23 and 24): FALSIFIED is retracted; the audit split and the holes stand.

> Results are appended TO THIS FILE, below the verbatim text. The archived monolith is frozen; never append there.

---
<!-- BEGIN VERBATIM: SPEC_level_information_budget.md -->
## 19. Roadmap item 1 FALSIFIED at stage 1, and §13.5's audit re-attributes the whole ladder (2026-08-05)

> ⚠ **PARTLY SUPERSEDED BY §23.**  Everything here stands as measured, but it was ALL measured at obs
> mask 10 / iono cutoff 25 — the denominator §21 voided.  Re-run at the current masks (§23): the
> stage-1 FAIL below (the SH band) does **not** reproduce, and §19.3's headline "+64 % postfit phase
> rms" is the worst cell of a mask-dependent series (+64 / +35 / +16 % at obs 10 / 15 / 30), not a
> constant of the mechanism.  §19.5's "item 1 is closed FALSIFIED" no longer follows from the evidence
> given for it.  §19.2/§19.4 (the audit split, the instrument holes) are re-confirmed by §23.4.

§17 ranked "LV-4 alone (+LV-5 machinery idle), demean OFF" as the untested candidate posture. **It fails at
stage 1, and the arm was already on disk**: `_ext63_gps_dual_1h_lv45.json` sets only `rx_cb_carry_step_gate`,
and with the de-mean off the loader forces LV-5 off (`GnssConfigLoadNetwork.cpp:7510-7516`), so that file
*is* LV-4-alone.

### 19.1 The 1 h stability gate

| §17 gate | threshold | measured | |
|---|---|---|---|
| folds | ≥ 95 % | 97.5 % (7179 / 7365) | pass |
| erases | ≤ 2 % | 1.76 % (126, max z 744.7, max robust σ̂ 1.702 m) | pass |
| settled sd | ≤ 5 TECU | 1.58 | pass |
| 3-epoch alternation | none | none | pass |
| **sh_health** | **0/0/0** | **46 out-of-band epochs / 8 engagements / 46 emissions withheld** | **FAIL** |

The level **collapses**: settled (20–60 min) a₀₀ mean **2.69 TECU**, min **-0.76** (unphysical), out of the
[2, 150] band for 46 of 121 epochs. It does not diverge the way arm 14 (carry OFF) did at -9 TECU/epoch — it
**parks** ~15 TECU below the canonical while riding the canonical's own breathing shape, recovering to 6.59 at
60 min while the canonical is at 20.95. Adding LV-1 barely moves it (4.44 / sd 0.82, 8 out-of-band). This is
the loader's own LV-4-without-de-mean warning, measured: full admission delivers the carry's common mode
intact and it owns the receiver-DCB frame outright. The missing fifth row of §15.1's table:

| posture | carry common mode | admission | level |
|---|---|---|---|
| canonical | rolling | 43 % | 1 h 17.45; 12 h wanders -8.4, ends +1.0 |
| **LV-4 alone** | **rolling** | **97.5 %** | **1 h 2.69, out of band 46/121 epochs** |

**§15.1's "the rolling memory is what TRACKS the level" is therefore too strong.** The canonical's rolling
memory tracks *at 43 % admission*; the same memory at 97.5 % drags the level out of the physical band. The
level responds to the KIND of memory AND HOW MUCH is admitted. (⚠ At the corrected masks this inverts —
§24.2.)

### 19.2 The §13.5 audit — and it does not say what §13.5 predicted

The feeds were **already armed** on this lineage (inherited from `_ft63_24h_conv.json` →
`_golden63_gps_1h_laneoff.json`), so the audit ran on arms already on disk, all from build `Aug 4 2026
23:27:25`. Decisive control: **the observation counts are identical across arms** — 1 582 051 phase
observations, 87 678 station-epochs, 61 stations in each 12 h arm; 87 936 phase observations in each 1 h arm.
Same data, different fit. 12 h arms, median postfit phase rms (`network_pf_keyed.csv`, STA rows):

| window | canonical | LV-1+2+4+5 | LV stack q=1e-6 |
|---|---|---|---|
| h0–12 | 0.01130 m | **0.01580 m (+40 %)** | 0.01290 m |
| h6–12 (settled) | 0.01110 m | **0.01600 m (+44 %)** | 0.01250 m |
| h2–3 | 0.00900 m | **0.01300 m (+44 %)** | 0.01140 m |

The prior side agrees rather than trading against it — prires `rxcb` σ̂₀ (a per-epoch global scale, emitted
identically on CLEAN and RESPONDED rows, over matching row totals 87 618 vs 87 423): canonical **1.92** → LV
stack **12.62** over 12 h, 1.06 → 8.40 in h2–3. Block-Baarda σ̂₀ 2.76 → 5.25. (`n_gt_4` fractions move the
*other* way — 0.31 → 0.03 — but that is only the 4σ̂₀ bar rising with σ̂₀; read σ̂₀, not the exceedance
count.) **So this is NOT the lane-A/B over-retention signature** (quieter priors bought with louder data): it
is louder on *both* sides. **Station congruence σ̂₀ (61.3 → 41.2) is the single instrument pointing the other
way, against four that agree — unexplained; do not resolve it editorially.**

### 19.3 The cost attributes to LV-4, and to LV-4 alone

1 h settled window (20–60 min), identical 87 936 phase observations in every arm:

| arm | LV-4 | de-mean | LV-1 | postfit phase rms | settled a₀₀ |
|---|---|---|---|---|---|
| canonical | – | – | – | 0.01020 m | 17.45 |
| **LV-1+2 (43 % admission)** | – | ✓ | ✓ | **0.01000 m** | **37.43** |
| LV-4 alone | ✓ | – | – | 0.01670 m | 2.69 |
| LV-4 + LV-1 | ✓ | – | ✓ | 0.01670 m | 4.44 |
| LV-2+4+5 | ✓ | ✓ | – | 0.01670 m | 26.45 |
| LV-1+2+4+5 | ✓ | ✓ | ✓ | 0.01660 m | 35.24 |

Every arm carrying LV-4 sits at 0.0166–0.0167; every arm without it at 0.0100–0.0102. **The de-mean and the
seed frame move the LEVEL by ~33 TECU and cost nothing measurable on the data side. LV-4 costs +64 % postfit
phase rms and moves the level DOWN ~15 TECU.** **This contradicts §15.2's "LV-4 makes the carry's information
admissible at all — keep."** LV-4's critique of the retired screen stands on its own terms — `|value_abs −
x_lin(cb)|` *is* frame-dependent, *is* blind to a between-epoch jump, and *did* withhold 67 % of folds with no
receiver event — but the audit adds that the folds it withheld are ones the observations do not support. The
broken screen was doing real work by accident: the accidental-budget disease again, one level below where
§11.3 found it.

### 19.4 Two invariant-#4 holes in the audit instrument itself (verified, not inferred)

- **`network_prior_residual.csv` emits ONLY the `rxcb` family**, although `prior_residual_families` is armed
  `"pl,pb,cb,zwd,rxcb"`. Four of five armed families produce zero rows (consistent with the posture: no
  estimated satellite code bias under EXT-CB, `phase_leveling` off on this lineage).
- **`network_pf_keyed.csv` has `n_code == 0` in ALL 138 749 rows** of the 12 h canonical run — the CODE half
  of the postfit-keyed instrument is structurally absent, so `s0_code` / `rms_code` are identically zero and
  **any code-side verdict read from this file is vacuous**. Systemic, not lineage-specific: the 303-station
  EXT-CB smoke's run-end line reads `12079 rows = 0 code + 5382 phase`.
- `network_block_baarda.csv` is floored at σ̂₀ = 1.0 for 36 % of rows over 12 h in the canonical and for
  **100 %** of its 1 h settled window. It separates at ≥ 3 h; do not read it at 1 h.

Usable data-side instruments on this lineage: `rms_phase` / `s0_phase` from `pf_keyed`, congruence σ̂₀, and (≥
3 h) block-Baarda σ̂₀.

### 19.5 What this changes

- Item 1 is closed FALSIFIED; it does not earn a 3 h or a 12 h stage. ⚠ **Retracted by §23.3.**
- Item 2 is discharged, with the opposite sign to the one it was written to detect.
- **The candidate posture is now LV-1 + LV-2 at 43 % admission (arm 19's posture), the only cell with both a
  good level and an uncompromised data fit** — and the only one never taken past 1 h. §14's ramp verdict was
  measured on the LV-4-bearing stack; whether the ramp survives without LV-4 is arm 30 (§20).
- §15.2's keep/drop list needs its first line inverted: LV-4 is not a prerequisite to keep, it is the
  ladder's data-side cost.

### 19.6 Reconciliation with §17 item 0 (the invariant-#5 course-correction)

- **The level numbers (a₀₀, §19.1) are FRAME quantities** — legal as a diagnostic, NOT as a grade. So "LV-4
  alone collapses the level to 2.69 TECU" is properly read as *"LV-4 alone moves the solution 15 TECU along a
  null direction, far enough that the emission band rejects it"*: the 46 withheld emissions are a real
  operational failure, but the TECU figure itself is convention, not error.
- **The postfit phase residuals (§19.2, §19.3) are datum-INVARIANT.** Residuals do not move under a reframing
  and the observation counts are identical across arms, so the LV-4 attribution (+64 % at 1 h, +40…44 % at 12
  h) is an invariant-legal grade — the strongest claim in this section, independent of any GIM comparison.

**Independently corroborated by item 0's own replay arms**, on a different instrument and code path: postfit
σ̂₀ `lv125` **1.4× louder** than the reference (0.0637 vs 0.0461 all; phase 0.0086 vs 0.0058) against §19.2's
1.40–1.48× on `pf_keyed` STA `rms_phase` — two sessions, two statistics, same sign and nearly the same ratio.
Item 0 also supplies the estimable-space consequence §19 could not: the stack's held-out WTZZ00DEU consumer
carries a **steady ~21 m 3D error through hours 10–11** (p95 ≈ median) coinciding with §14's pin-ramp era,
while DD fractions are identical (0.2503 vs 0.2498 on 322 936 rows). So the stack is estimability-neutral on
the DD channel and estimability-HARMFUL on the consumer.

⚠ Consequence for arm 30 (§20): its LEVEL result is a diagnostic. Its GRADE is the postfit phase rms and the
consumer replay.

---

