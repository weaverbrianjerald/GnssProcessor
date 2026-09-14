# DOY-129 defects — defects A/B/C from the instrumented leg, the five wiring facts, and the measured policy counterfactuals

**STATUS**

- **id:** DOY-129 defects
- **topic:** raim-qc
- **status:** closed
- **summary:** defects A/B/C from the instrumented leg, the five wiring facts, and the measured policy counterfactuals
- **dates in the text:** 2026-07-28
- **knobs / posture:** —
- **hashes named in the text** (commit / exe / byte-cert): —
- **run dirs named in the text:** `runs/day129_guards`, `runs/day129_cont`
- **source:** `docs/archive/SPEC_day129_frame_guards.md` — C1, C2, C3, C4, C5, C6

> Results are appended TO THIS FILE, below the verbatim text. The archived monolith is frozen; never append there.

---
<!-- BEGIN VERBATIM: SPEC_day129_frame_guards.md -->
## C1 What the leg shipped — the disposition census

`network_epoch_status.csv`, all 600 epochs: `sh_gate_withheld` **278 (46.3 %)**, `emitted` **322
(53.7 %)**. The last 155 epochs (22:43 → EOD) all emitted — no engagement after 340980. The 278
withholds are **5 engagements** in two *qualitatively different* populations, which is why the
policy splits in two:

| era | tow span | epochs | a₀₀ at engage |
|---|---|---|---|
| **the seam** | 327600 → 334770 | **239** | 1410.8 TECU |
| chatter #1 | 338130 → 338550 | 14 | 0.5 |
| chatter #2 | 338850 → 339420 | 19 | −0.2 |
| chatter #3 | 340320 → 340380 | 2 | −8.7 |
| chatter #4 | 340860 → 340980 | 4 | 1.1 |

The seam era **swallowed the 20:47 excursion (tow 334020)** — the single event the whole guard
chain was built to validate against. **Consequence: the Phase-2.1 ON-path validation gate (iii)
CANNOT be read off this leg.**

## C2 DEFECT A — the seam engagement was CORRECT. The field really was diverged.

*The most important finding in the policy card, because it inverts the obvious fix.* Seed's last
epoch (327570, 18:59:30) vs the leg's first (327600, 19:00:00):

| | a₀₀ | rms_all | slant resid mean | slant resid max | `a00_in_band` |
|---|---|---|---|---|---|
| seed tail | **34.84** TECU | 3.15 | 13.86 m | 65.5 m | 1 |
| leg epoch 1 | **1410.83** TECU | 214.25 | 589.56 m | **50 071 m** | 0 |

then a₀₀ = 400 → −197 → −610 → −893 → −1053 → −1126 … recovering monotonically to **+4.8 TECU at
334770**. A gate that *shipped* those epochs would have shipped a global ionosphere **wrong by up
to 1150 TECU**. **The gate did its job. Any policy that converts the seam era into ordinary
emission is a policy to ship a known-bad product.** It is **not** a gate artifact and **not** a
datum artifact:

- `datum_changed` = **0** on every seam epoch (`network_epoch_status.csv` col 14), so the
  one-epoch SH-carry drop at `NetworkRunContext.cpp:849` did **not** fire.
- **The SH carry WAS restored and IS tight**: `runs/day129_guards/seed_immutable.txt` carries all
  **256** VtecSh coefficients, a₀₀ = **34.836556 ± √0.3265** TECU
  (`D,3,,,0,0,67,34.8365…,0.32645…,0`), folded pre-gate every epoch
  (`NetworkRunContext.cpp:835-910`).
- **What IS anomalous is the carry's PHASE side.** Leg epoch 1: `n_d5_flags` = `n_d5_retaps` =
  **7356** against **8016 restored DD** (`RTB-7c … restored invariant DD carry (8016 carried
  DD)`) — the D5 innovation gate **re-tapped 92 % of the restored carry in ONE epoch**. Epoch 2
  then refloated **7527** chords. `sigma0_pf_code` = **356.7** at epoch 1 against the seed's
  seeded σ̂₀ = **2.9144**.

That is the documented warm-resume D5 shock (RS card; `project_gg305_warm_resume_chain` recorded
3.3× re-tap churn at 305 stations), here measured at **92 %** re-tap and with its SH consequence
quantified for the first time: **the phase constraint collapses, the SH field is left
code-determined, and the stiff SH carry then ratchets the resulting garbage level back over 239
epochs.**

**Recovery is NOT complete at end of leg.** `sigma0_pf_code` 356.7 → **12.8** (still 4.4× the
seed's 2.91); `rms_ho` (RMS over degrees ≥ 1, a₀₀ stripped out) 213 → **5.8** against the seed's
converged **2.376**; a₀₀ still swinging **±20 TECU at 23:10**. **The leg is 5 h of recovery
transient, start to finish.**

## C3 DEFECT B — a₀₀ is the WRONG STATISTIC; the late chatter is that transient's tail

Reference day (`runs/day129_cont/seg01`, 2160 epochs, 06:00–24:00), hourly a₀₀ for the clock
hours the chatter occupies:

| hour | ref a₀₀ min | ref a₀₀ med | ref rms_all med | leg a₀₀ | leg rms_all |
|---|---|---|---|---|---|
| 22 | 22.72 | 30.57 | 4.84 | −8.7 … 22.7 | 5.83 … 8.60 |
| 23 | 20.53 | 26.28 | 4.02 | (same era) | (same era) |

The chatter era's a₀₀ sits **~25 TECU BELOW the reference realization at the same epochs**, while
its own rms_all is *quiet and stable* (5.83–8.60, robust σ 0.65). Two conclusions, and they are
the whole of defect B:

1. **The 0.5–2.8 TECU readings are NOT a nighttime physical floor** the band fails to accommodate
   — the reference run at those hours sits at 20–39 TECU. They are a depressed *LEVEL* left by
   the seam recovery.
2. **a₀₀ is datum-coupled and therefore the wrong quantity to gate on.** a₀₀ is the field's DC
   term; the network's IF-based estimation is a₀₀-insensitive and the level trades against the
   code-bias/DCB common mode, so its zero point is a property of *convergence history*, not of
   field health. **Gating a product on an absolute a₀₀ level grades a NON-DATUM-INVARIANT
   quantity — standing invariant #5.** Its own header admits the intent was a *divergence* WARN
   (a₀₀ → −91 TECU), not a plausibility audit (`GnssConfigTypes.hpp:1712-1713`).

The in-repo precedent for the right shape is `NetworkIonoDeweightGate.hpp:18-49`, whose header
opens with this exact argument ("Why a network-RELATIVE robust gate (not an absolute σ
threshold)") and which implements engage/release hysteresis on a robust z (`sigma_hi_` /
`sigma_lo_`, `NetworkIonoDeweightGate.hpp:165-217`).

## C4 DEFECT C — the CB detector's resume suppression is ONE epoch; the shock is ~10+

*(absorbed §0.4)* `NetworkRunContext.hpp:502-504` claims `cb_exc_prev_clk` rebuilds from the
first post-resume epoch, "which correctly suppresses a spurious trip on the resume seam itself
(no baseline ⇒ no delta ⇒ no trip)". **True for exactly one epoch.** Measured:

| tow | median \|Δδtˢ\| | max | ratio | verdict logged |
|---|---|---|---|---|
| 327630 (seam+1) | **172.37 m** | 1001.75 | 5.81 | trip → G01 CB reinit |
| 327660 (seam+2) | **54.45 m** | 128.22 | 2.35 | trip → G01 CB reinit |
| 327900 (seam+10) | **68.87 m** | 176.87 | 2.57 | trip → G01 CB reinit |
| 329670 / 329700 | 115.13 / 86.61 | 1552 / 364 | 13.48 / 4.20 | trip → G01 CB reinit |

`clock_std_m` at the seam epochs is **371 → 281 → 294 m** against a leg-end **13.3 m**. **All
three of the first trips are seam-shock artifacts**, and they consumed **three real CB reinits of
the pivot satellite**. The 19:21 reference event (tow 328920) produced **no** trip in this leg
while 329670/329700 did — the leg does not even reproduce the reference run's event timing,
corroborating C2's "the whole leg is a transient".

*Secondary observation, not a defect:* the pending disc bump queued at 327630 **landed at
334770**, the first epoch after the SH gate released — 2 h later, collapsed to a **single**
`cb_excursion_disc_landed` event for G01. **The Card-2 value-step/signal-coincidence rule holds**
(nothing was emitted in between), and the set-keyed queue correctly de-duplicated three reinits
into one signalled discontinuity.

## C5 The FIVE wiring facts any policy must respect

Verified in tree; a policy that ignores any of them cannot be implemented as written.

1. **Degrade-during-withhold is a DEADLOCK.** `NetworkSolPrep.cpp:140` early-returns on
   `sh_gate_engaged`; the URA-ramp *evaluation* is at `NetworkSolPrep.cpp:398` and `++n_emitted`
   at `:367` — both **after** it. `NetworkEmitStore.cpp:120-140` early-returns too. So during a
   withheld era neither the ramp-lift rule nor `n_emitted` advances: arm `warmup_ura_floor_m`
   together with the SH gate today and **the floor can never lift**. Grace must be decided
   **upstream of the `NetworkSolPrep.cpp:140` early-return.**
2. **The ramp restores as LIFTED on a warm resume** (`NetworkRunContext.cpp:536-542`, by design:
   "a warm-resumed producer is NOT degraded"). The existing machinery will therefore **never**
   degrade a post-seam era on its own. A resume grace needs an explicit re-arm.
3. **`warmup_ura_floor_m` and `warmup_epochs` are UNCONDITIONALLY in the resume fingerprint** —
   verified still true at `src/core/app/NetworkRunContext.cpp:589`, which appends both
   unconditionally *(the policy card's original anchor `NetworkRunContext.cpp:218` has drifted;
   the FACT is unchanged, so **D-G4's ruled conditional append is NOT implemented**)*. A
   fingerprint mismatch **refuses the warm resume** outright (`NetworkStateSnapshot.cpp:456`,
   `warmResumeAllowed`). Making that append conditional on `floor > 0` (the `cbexc=` / `shl2=` /
   `carrycpl*` pattern at `:256-314` of the same file) keeps every **unarmed** fingerprint
   byte-identical — but it does **NOT** let an armed run resume an unarmed seed: unarmed snapshot
   has no tag, armed run's live fingerprint has one ⇒ still a mismatch ⇒ still a cold start.
   **Conditional append is HYGIENE, not an arming path.**
4. **The floor covers the per-satellite clock URA only** (`NetworkSolPrep.cpp:440-441` sets
   `s.clock_ura_m`). The M4 contract-v2 combined σ channels — `sigc` (phase), `sigcc` (code) and
   `rho` — are computed separately at `NetworkSolPrep.cpp:834-900` and are **NOT floored**. A
   consumer weighting by `sigcc` (exactly what M4 built it for) would keep trusting a degraded
   era at full weight.
5. **The datum-pinned pivot satellite is SKIPPED entirely** (`NetworkSolPrep.cpp:425`, correctly
   — its variance is the artificial pin). The pivot is **G01**, whose code bias is the state that
   ratchets (Part A). **So the one satellite whose corrections carry the fault is the one
   satellite with no URA to degrade.**

## C6 Policy counterfactuals, measured on BOTH runs

Harness: Part E. `rms_ho` = RMS over degrees ≥ 1 (a₀₀ algebraically removed from `rms_all`).
Dwell `n/m` = engage after n consecutive out-of-band, release after m consecutive in-band.
**Denominators: the complete leg is 600 epochs (C1); the reference day is 2160 epochs (C3).**

| policy | leg withheld /600 | leg engagements | ref withheld /2160 | ref engagements | ref: catches 20:47? |
|---|---|---|---|---|---|
| a₀₀ ∈ [2, 150], dwell 1/1 — **as shipped** | 278 (46.3 %) | 5 | 214 (9.9 %) | 5 | yes (1 ep + 30 ep) |
| a₀₀ ∈ [0, 150], dwell 1/1 | 268 (44.7 %) | **5** | 213 (9.9 %) | 5 | yes |
| a₀₀ ∈ [−5, 150], dwell 1/1 | 253 (42.2 %) | **5** | 206 (9.5 %) | 5 | yes |
| a₀₀ ∈ [−5, 150], dwell 3/10 | 275 (45.8 %) | **3** | 233 (10.8 %) | 4 | yes (30 ep) |
| a₀₀ ∈ [−30, 150], dwell 1/1 | 226 (37.7 %) | **1** | 170 (7.9 %) | 5 | **NO** |
| a₀₀ ∈ [−30, 150], dwell 3/10 | 233 (38.8 %) | **1** | 180 (8.3 %) | 2 | **NO** |
| `rms_ho` > 12 / < 9, dwell 3/10 | 344 (57.3 %) | 1 | 386 (17.9 %) | 4 | yes (86 ep) |
| `rms_ho` trailing robust-z > 6 / < 3 | **0 (0.0 %)** | **0** | 177 (8.2 %) | 6 | yes (37 ep) |
| **`rms_ho`/2.376 > 4 / < 2.5, dwell 3/10, rail a₀₀∉[−60,200]** — RULED (D-G3) | 542 (90.3 %) | 1 | 435 (20.1 %) | 4 | yes (107 ep) |
| `rms_ho`/2.376 > 6 / < 3, dwell 3/10, same rail | 400 (66.7 %) | 1 | 393 (18.2 %) | 4 | yes (89 ep) |

**Four readings drive every ruling in Part D:**

* **Lowering the low edge to 0 or −5 does NOT stop the chatter** (5 → 5 engagements, → 3 only
  once time hysteresis is added). a₀₀ passes through −8.7 TECU in that era, so any edge in
  [−5, +2] is crossed; the chatter only dies at **≈ −30**.
* **Widening to −30 LOSES the 20:47 detection on the reference day.** The a₀₀ band's apparent
  grip on the damaging event depends on `low = 2` *and* `dwell = 1` — a hard argument against
  tuning a₀₀ as the load-bearing statistic.
* **A trailing-window robust z is INERT on this leg — 0 engagements.** The window adapted to the
  diverged level it was initialised inside. **The MW-thresholds directive's self-calibration
  pattern CANNOT be applied to a trailing window here**; the calibration must be *frozen* from a
  converged reference window.
* **A correctly calibrated field-shape gate withholds ~80–100 % of this leg.** That is the honest
  verdict on the leg's product stream, and it is why D-G1 is about *degrading* the era rather
  than shortening it.

---

# Part D — the six gate-policy rulings

*Absorbed from `docs/SPEC_day129_gate_policy.md` §1–§4, §7.* **Card type: POLICY. No code — and
none has been written.** Verified at absorption time: **none of `resume_grace_epochs`,
`resume_grace_ura_floor_m`, `sh_gate_ratio_engage` / `_release` / `_median_tecu`, or
`cb_excursion_suppressed_resume` exists anywhere in `src/` or `include/`.** The 2026-07-28 "ready
to implement" status is still accurate: **these MUSTs are live implementation contract, not
history.** **MUST** = binding; **CANDIDATE** = mechanism proposal, deviate with a stated
rationale. **ALL SIX RULED 2026-07-28.** One prerequisite remains and it is a **measurement, not
a decision** — the grace floor MAGNITUDE (Part E).

| # | ruling | binding in |
|---|---|---|
| **D-G1** | degrade, don't withhold | D-G1's 7 MUSTs |
| **D-G2** | grace cap **120 epochs**, with reversion to hard withhold | D-G1 MUST-5 + the cap table |
| **D-G3** | `rms_ho` **RATIO** to a calibrated median; a₀₀ demoted to a wide rail | D-G3 + its three knob traps + the ratio-not-z rule |
| **D-G4** | conditional fingerprint append (+ grace outside the fingerprint) | C5(3), D-G1 MUST-7 |
| **D-G5** | CB suppression **STATE-COUPLED**, 20-epoch floor | D-G5 MUST-1 |
| **D-G6** | ship the pivot during grace, with an explicit report note | D-G1 MUST-3 |

