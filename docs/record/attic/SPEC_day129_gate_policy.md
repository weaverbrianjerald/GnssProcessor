# SPEC — gate policy: seam grace, a₀₀ band hysteresis, CB-detector resume window

**Card type: POLICY ONLY. No code.** The three defects this card addresses live in
`src/core/filter/NetworkSolveGlue.cpp`, `NetworkSolPrep.cpp`, `NetworkEmitStore.cpp`,
`NetworkClockQc.cpp` and `include/app/NetworkRunContext.hpp` — all **uncommitted**
and owned by the session that built Phase 2.1/2.2. **No other session may edit
them.** This document is the policy decision record + validation design; the
implementation is the owning session's.

**Lineage.** `docs/SPEC_day129_frame_guards.md` (the guard card) →
`docs/REPORT_day129_frame_guards_phase01.md` §6 (the arming pass that built the SH
gate and launched the chain) → this. Companion for the *estimation*-side half of
defect A: `docs/SPEC_warm_resume_event_card.md` + `docs/REPORT_warm_resume_event.md`
(the "RS card").

**Convention.** **MUST** = binding; **CANDIDATE** = mechanism proposal, deviate with
a stated rationale. Standing invariants (CLAUDE.md) are pre-flighted in §6.

**Evidence status.** Every number in §0 was measured *this session* from
`runs/day129_guards/` and `runs/day129_cont/seg01/`. The guarded leg
(`runs/day129_guards/leg01/`) **completed** at 2026-07-28 00:45:46 — final tow
345570 (23:59:30), 600 epochs, `n_emitted` 1042 cumulative (chain STATUS: COMPLETE).
All figures below are the **complete** leg.

**ALL SIX DECISIONS RULED 2026-07-28 — this card is ready to implement.** D-G1 degrade ·
D-G2 cap 120 · D-G3 `rms_ho` ratio · D-G4 conditional append · D-G5 state-coupled CB
window · D-G6 ship the pivot with a note. §1–§4 state them as binding, §7 tabulates
them, §9 sequences the build. One prerequisite is a measurement, not a decision: the
grace floor MAGNITUDE must come from a consumer-replay sweep (§7).

---

## 0. Evidence base

### 0.1 What the leg actually shipped

`network_epoch_status.csv` disposition census, all 600 epochs:

| disposition | epochs | share |
|---|---|---|
| `sh_gate_withheld` | 278 | **46.3 %** |
| `emitted` | 322 | 53.7 % |

(The 46.3 % matches the "~45 % of leg epochs withheld" in the card's framing. The
last 155 epochs, 22:43 → EOD, all emitted — no engagement after 340980.)

The 278 withholds are **5 engagements**, and they are two *qualitatively different*
populations — which is why this card splits into two policies:

| era | tow span | epochs | a₀₀ at engage |
|---|---|---|---|
| **the seam** | 327600 → 334770 | **239** | 1410.8 TECU |
| chatter #1 | 338130 → 338550 | 14 | 0.5 |
| chatter #2 | 338850 → 339420 | 19 | −0.2 |
| chatter #3 | 340320 → 340380 | 2 | −8.7 |
| chatter #4 | 340860 → 340980 | 4 | 1.1 |

The seam era swallowed the **20:47 excursion (tow 334020)** — the single event the
whole guard chain was built to validate against. Consequence for the chain, stated
plainly: **the Phase-2.1 ON-path validation gate (iii) cannot be read off this leg.**

### 0.2 Defect A — the seam engagement is CORRECT. The field really was diverged.

This is the most important finding in this card, because it inverts the obvious fix.

Seed's last epoch (327570, 18:59:30) vs leg's first (327600, 19:00:00):

| | a₀₀ | rms_all | slant resid mean | slant resid max | `a00_in_band` |
|---|---|---|---|---|---|
| seed tail | **34.84** TECU | 3.15 | 13.86 m | 65.5 m | 1 |
| leg epoch 1 | **1410.83** TECU | 214.25 | 589.56 m | **50 071 m** | 0 |

then a₀₀ = 400 → −197 → −610 → −893 → −1053 → −1126 … recovering monotonically to
+4.8 TECU at 334770. A gate that *shipped* those epochs would have shipped a global
ionosphere wrong by up to 1150 TECU. **The gate did its job.** Any policy that
converts the seam era into ordinary emission is a policy to ship a known-bad product.

The mechanism is **not** a gate artifact and **not** a datum artifact:

* `datum_changed` = **0** on every seam epoch (`network_epoch_status.csv` col 14), so
  the one-epoch SH-carry drop at `NetworkRunContext.cpp:849` did **not** fire.
* The SH carry **was** restored and **is** tight: `runs/day129_guards/seed_immutable.txt`
  carries all 256 VtecSh coefficients, a₀₀ = `34.836556 ± √0.3265` TECU
  (`D,3,,,0,0,67,34.8365…,0.32645…,0`), and it folds pre-gate every epoch
  (`NetworkRunContext.cpp:835-910`).
* What *is* anomalous is the carry's **phase side**. Epoch 1 of the leg:
  `n_d5_flags` = `n_d5_retaps` = **7356** against **8016 restored DD**
  (`RTB-7c … restored invariant DD carry (8016 carried DD)`) — the D5 innovation gate
  re-tapped **92 %** of the restored carry in one epoch. Epoch 2 then refloated
  **7527** chords. `sigma0_pf_code` = **356.7** at epoch 1 against the seed's seeded
  σ̂₀ = 2.9144.

That is the already-documented warm-resume D5 shock (RS card; `project_gg305_warm_resume_chain`
recorded 3.3× re-tap churn at 305 stations), here measured at 92 % re-tap and with its
SH consequence quantified for the first time: **the phase constraint collapses, the SH
field is left code-determined, and the stiff SH carry then ratchets the resulting
garbage level back over 239 epochs.**

**Recovery is not complete at end of leg.** `sigma0_pf_code` 356.7 → 12.8 (still 4.4×
the seed's 2.91); rms over degrees ≥ 1 (`rms_ho`, a₀₀ stripped out) 213 → 5.8 against
the seed's converged **2.376**; a₀₀ still swinging ±20 TECU at 23:10. **The leg is 5 h
of recovery transient, start to finish.**

### 0.3 Defect B — the late chatter is that same transient's tail, and a₀₀ is the wrong statistic

Reference-day (`runs/day129_cont/seg01`, 2160 epochs, 06:00–24:00) hourly a₀₀ for the
same clock hours the chatter occupies:

| hour | ref a₀₀ min | ref a₀₀ med | ref rms_all med | leg a₀₀ | leg rms_all |
|---|---|---|---|---|---|
| 22 | 22.72 | 30.57 | 4.84 | −8.7 … 22.7 | 5.83 … 8.60 |
| 23 | 20.53 | 26.28 | 4.02 | (same era) | (same era) |

So the chatter era's a₀₀ is **~25 TECU below the reference realization at the same
epochs**, while its rms_all is *quiet and stable* (5.83–8.60, robust σ 0.65). Two
things follow, and they are the whole of defect B:

1. **The 0.5–2.8 TECU readings are not a nighttime physical floor** the band fails to
   accommodate — the reference run at those same hours sits at 20–39 TECU. They are a
   depressed *level* left by the seam recovery.
2. **a₀₀ is datum-coupled and therefore the wrong quantity to gate on.** a₀₀ is the
   field's DC term; the network's IF-based estimation is a₀₀-insensitive and the level
   trades against the code-bias/DCB common mode, so its zero point is a property of
   convergence history, not of field health. Gating a product on an absolute a₀₀ level
   grades a **non-datum-invariant** quantity — standing invariant #5. Its own header
   admits the intent was a *divergence* WARN (a₀₀ → −91 TECU), not a plausibility audit
   (`GnssConfigTypes.hpp:1712-1713`).

The in-repo precedent for the right shape is `NetworkIonoDeweightGate.hpp:18-49`,
whose header opens with this exact argument ("Why a network-RELATIVE robust gate (not
an absolute σ threshold)") and which implements engage/release hysteresis on a robust
z (`sigma_hi_` / `sigma_lo_`, `NetworkIonoDeweightGate.hpp:165-217`).

### 0.4 Defect C — the CB detector's resume suppression is one epoch, the shock is ~10

`NetworkRunContext.hpp:502-504` claims: *"`cb_exc_prev_clk` rebuilds from the first
post-resume epoch, which correctly suppresses a spurious trip on the resume seam
itself (no baseline ⇒ no delta ⇒ no trip)."* True for exactly one epoch. Measured:

| tow | median \|Δδtˢ\| | max | ratio | verdict logged |
|---|---|---|---|---|
| 327630 (seam+1) | **172.37 m** | 1001.75 | 5.81 | trip → G01 CB reinit |
| 327660 (seam+2) | **54.45 m** | 128.22 | 2.35 | trip → G01 CB reinit |
| 327900 (seam+10) | **68.87 m** | 176.87 | 2.57 | trip → G01 CB reinit |
| 329670 / 329700 | 115.13 / 86.61 | 1552 / 364 | 13.48 / 4.20 | trip → G01 CB reinit |

`clock_std_m` at the seam epochs is 371 → 281 → 294 m against a leg-end 13.3 m. **All
three of the first trips are seam-shock artifacts**, and they consumed three real CB
reinits of the pivot satellite. The 19:21 reference event (tow 328920) produced **no**
trip in this leg, while 329670/329700 did — i.e. the leg does not even reproduce the
reference run's event timing, corroborating §0.2's "the whole leg is a transient".

Secondary observation (not a defect, worth recording): the pending disc bump queued at
327630 landed at **334770**, the first epoch after the SH gate released — 2 h later,
collapsed to a single `cb_excursion_disc_landed` event for G01. The Card-2
value-step/signal-coincidence rule holds (nothing was emitted in between), and the
set-keyed queue correctly de-duplicated three reinits into one signalled discontinuity.

### 0.5 Five wiring facts that constrain any policy

Verified in tree; a policy that ignores any of them cannot be implemented as written.

1. **Degrade-during-withhold is currently a deadlock.** `NetworkSolPrep.cpp:140`
   early-returns on `sh_gate_engaged`; the URA-ramp *evaluation* is at
   `NetworkSolPrep.cpp:398` and `++n_emitted` at `:367` — both **after** it.
   `NetworkEmitStore.cpp:120-140` early-returns too. So during a withheld era neither
   the ramp lift rule nor `n_emitted` advances: arm `warmup_ura_floor_m` together with
   the SH gate today and the floor can never lift. **Grace must be decided upstream of
   the `NetworkSolPrep` early-return.**
2. **The ramp restores as LIFTED on a warm resume** (`NetworkRunContext.cpp:536-542`,
   by design: *"a warm-resumed producer is NOT degraded"*). The existing machinery will
   therefore **never** degrade a post-seam era on its own. A resume grace needs an
   explicit re-arm, not just a non-zero floor.
3. **`warmup_ura_floor_m` and `warmup_epochs` are UNCONDITIONALLY in the resume
   fingerprint** (`NetworkRunContext.cpp:218`), and a fingerprint mismatch **refuses
   the warm resume** outright (`NetworkStateSnapshot.cpp:456`, `warmResumeAllowed`).
   Making that append conditional on `floor > 0` (the `cbexc=` / `shl2=` /
   `carrycpl*` pattern at `:256-314`) keeps every **unarmed** fingerprint
   byte-identical — but it does **not** let an armed run resume an unarmed seed:
   unarmed snapshot has no tag, armed run's live fingerprint has one ⇒ still a
   mismatch ⇒ still a cold start. **Conditional append is hygiene, not an arming
   path.** See §1 MUST-7 for the consequence of D-G4.
4. **The floor covers the per-satellite clock URA only** (`NetworkSolPrep.cpp:440-441`
   sets `s.clock_ura_m`). The M4 contract-v2 combined σ channels — `sigc` (phase) and
   `sigcc` (code) + `rho` — are computed separately at `NetworkSolPrep.cpp:834-900`
   and are **not** floored. A consumer weighting by `sigcc` (which is exactly what M4
   built it for) would keep trusting a degraded era at full weight.
5. **The datum-pinned pivot satellite is skipped entirely** (`NetworkSolPrep.cpp:425`,
   correctly — its variance is the artificial pin). The pivot is G01. G01's code bias
   is the state that ratchets. So the one satellite whose corrections carry the fault
   is the one satellite with no URA to degrade.

### 0.6 Policy counterfactuals (measured on both runs)

Harness: §3.4. `rms_ho` = RMS over degrees ≥ 1, i.e. a₀₀ algebraically removed from
`rms_all`. Dwell `n/m` = engage after n consecutive out-of-band, release after m
consecutive in-band.

**On the complete leg (600 epochs)** and **on the reference day (2160 epochs)**:

| policy | leg withheld | leg engagements | ref withheld | ref engagements | ref: catches 20:47? |
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

Four results drive §1–§3:

* **Lowering the low edge to 0 or −5 does not stop the chatter** (5 engagements → 5, and
  → 3 only once time hysteresis is added). a₀₀ passes through −8.7 TECU in that era, so
  any edge in [−5, +2] is crossed. The chatter only dies at **≈ −30**.
* **Widening to −30 costs the 20:47 detection on the reference day.** The a₀₀ band's
  apparent grip on the damaging event is fragile: it depends on `low = 2` *and*
  `dwell = 1`. This is a hard argument against tuning a₀₀ as the load-bearing statistic.
* **A trailing-window robust z is inert on this leg — 0 engagements.** The window
  adapted to the diverged level it was initialized inside. The MW-thresholds directive's
  self-calibration pattern **cannot** be applied to a trailing window here; the
  calibration must be *frozen* from a converged reference window.
* **A correctly calibrated field-shape gate withholds ~80–100 % of this leg.** That is
  the honest verdict on the leg's product stream, and it is the reason §1 is about
  *degrading* the era rather than shortening it.

---

## 1. Policy P1 — resume grace: **degrade, do not withhold** — RULED (D-G1)

**Intent.** During a bounded post-resume recovery, ship the epoch with an honest
DEGRADED quality advertisement instead of shipping nothing. §0.2 forbids shipping it as
good; §0.6 shows no threshold choice makes the era short.

### MUSTs

1. **Grace changes the DISPOSITION, never the VERDICT.** The gate statistic, its
   out-of-band determination, the `sh_gate_oob_*` counters and the `SH_GATE` events MUST
   be computed and recorded identically whether or not grace is active. Grace may only
   change what the epoch *does* about an engaged verdict. A grace that suppresses the
   verdict destroys the record that the era was bad — and would let a real divergence
   ride out of the grace window unnoticed.
2. **A third disposition.** The epoch-status disposition MUST distinguish
   `sh_gate_degraded` from both `emitted` and `sh_gate_withheld`, and the product H/S
   line MUST carry a machine-readable degraded marker (not only an inflated σ). A
   consumer that ignores σ must still be able to see it. Grace epochs MUST be counted
   in their own run-end counter, separate from `n_sh_gate_withheld` (invariant #4).
3. **Degrade every quality channel.** Per §0.5(4) the URA floor alone leaves
   `sigc`/`sigcc`/`rho` at full weight — and `sigcc` is exactly what M4 built for a
   σ-weighting consumer. A degraded epoch MUST floor **all** of `clock_ura_m`,
   `sigc_m`, `sigcc_m` (`NetworkSolPrep.cpp:440-441` and `:834-900` respectively). Any
   channel left unfloored MUST be named in the report as a known honesty gap.
   **D-G6 = SHIP THE PIVOT WITH A NOTE (ruled).** The datum-pinned pivot is skipped by
   the URA path by design (`NetworkSolPrep.cpp:425` — its variance is the artificial
   pin), so its corrections ship during grace with **no per-satellite quality signal**.
   Withholding them was rejected: a consumer that loses the pivot loses the datum.
   Binding consequences: (a) the report MUST state this explicitly, naming the pivot
   (G01 on this chain) and noting that it is *the satellite whose code bias ratchets*
   (`project_day129_pivot_cb_ratchet`) — i.e. the one correction a grace consumer cannot
   de-weight; (b) the grace marker of MUST-2 MUST be an **epoch-level** flag, not only
   per-satellite σ inflation, so a consumer has *some* signal covering the pivot;
   (c) the run-end line MUST report how many grace epochs shipped pivot corrections.
4. **Resolve the deadlock structurally.** The grace decision MUST be evaluated where
   both `NetworkSolPrep` and `NetworkEmitStore` can see it and *before* the
   `NetworkSolPrep.cpp:140` early-return, so that `n_emitted` (`:367`) and the ramp-lift
   rule (`:398`) advance on grace epochs. A grace that leaves `n_emitted` frozen
   re-creates §0.5(1) in a new place.
5. **Grace is BOUNDED at 120 epochs and its exit is data-driven — D-G2 = CAP 120
   (ruled).** Measured recovery was 239 epochs by a₀₀ and 400–542 by field shape
   (§0.6/§1.1), so the cap is deliberately *shorter* than the observed recovery: a cap
   long enough to cover it is indistinguishable from disarming the gate. Grace MUST end
   at whichever comes first: (a) the gate statistic releases (normal operation resumes),
   or (b) **120 processed epochs (1 h at 30 s) from the seam**. On reaching the cap
   while still engaged the epoch MUST revert to **hard withhold**, and that transition
   MUST be logged un-throttled, evented, and counted. **The cap-reversion is a
   first-class deliverable of this policy, not an error path** — "this resume did not
   re-converge within an hour" is the single most important thing the mechanism can tell
   an operator, and on this leg it is what would have fired (§1.1: 118 degraded, 424
   withheld). A run that reverts MUST NOT be graded as a successful warm resume.
6. **Grace arms only at a resume seam.** The trigger MUST be the warm-resume event, not
   "the gate engaged". A gate engagement in steady state is a real divergence and MUST
   go straight to hard withhold with no grace. (Cold starts are already covered by the
   existing RTB-7(b) ramp, which is *not* restored-as-lifted on a cold run.)
7. **Default OFF, byte-identical when off** (invariant #6). **D-G4 is RULED:
   conditional append.** Two consequences the ruling forces, both MUST:
   * `warmup_ura_floor_m` / `warmup_epochs` move to a conditional append gated on
     `warmup_ura_floor_m > 0` (`NetworkRunContext.cpp:218` → the `:256-314` pattern).
     This is **back-compat hygiene**: every unarmed run's fingerprint stays
     byte-identical, including against snapshots written before the knobs existed.
   * It does **not** create an arming path (§0.5(3)): an armed run still cannot resume
     an unarmed seed. Therefore **grace MUST NOT be implemented by re-arming the
     RTB-7(b) ramp.** Grace gets its **own emission-only knobs, deliberately NOT in the
     fingerprint** — the same class decision the SH gate itself took
     (`NetworkRunContext.hpp:515-521`: "the raim / clock_runaway_check class, not the
     cb_excursion class"), and legitimate for the same reason: grace changes what is
     emitted and with what advertised quality, never what a carried state means. That
     is what lets grace arm on the **existing** `runs/day129_guards` chain with no new
     seed. It also MUST NOT read or write `ramp_lifted` / `ura_ewma_updates`, which are
     snapshotted and fingerprint-coupled — grace applies its own floor directly.

### CANDIDATE mechanism

* `network.resume_grace_epochs` (cap, **default 120**, 0 = off) +
  `network.resume_grace_ura_floor_m` (+ the `sigc`/`sigcc` floors of MUST-3), all
  outside the fingerprint per MUST-7.
  Grace state lives in `NetworkRunContext` (`NetworkCarryState`), seeded at the warm
  resume, decremented per processed epoch, and is **not** snapshotted (a resume of a
  resume re-seeds it, which is the conservative direction).
* Floor value: reuse the RTB-7(b) *semantics* (a per-satellite URA floor in m) but not
  its state. No numeric recommendation is made here — the floor must be large enough
  that a σ-weighting consumer effectively de-weights the era; deriving it belongs with
  the consumer-side replay (`ApplicationConsumerReplay`, `REPORT …phase01.md` §1)
  which can measure position error vs floor directly. **MUST: derive it, do not guess
  it**, and until it is derived the report MUST call grace un-calibrated.
* Grace MUST NOT extend the `phase_leveling_max_gap_s` (900 s) exposure the SH gate was
  deliberately designed to avoid (`REPORT …phase01.md` §6): since grace *emits*, it
  strictly reduces that exposure. Worth one line in the report confirming it.

### 1.1 What grace does to tonight's leg (D-G2 decision table)

Under the ruled statistic (D-G3, 4.0/2.5, dwell 3/10) the engaged era is **542 epochs,
releasing at ep 544**. Grace degrades the first `cap` epochs of it and hard-withholds
the rest:

| cap | degraded | hard-withheld | emitted | cap-reversion fires? |
|---|---|---|---|---|
| 60 | 58 | 484 | 58 | YES |
| **120 — RULED (D-G2)** | **118** | **424** | **58** | **YES** |
| 240 | 238 | 304 | 58 | YES |
| 400 | 398 | 144 | 58 | YES |
| 600 (= no cap) | 542 | 0 | 58 | no |

At the looser 6.0/3.0 pair the era is 400 epochs and 200 epochs emit normally; the
same cap sweep gives 58/118/238/398 degraded with 342/282/162/2 withheld.

**This is the D-G2 evidence.** Any cap ≤ 400 reverts to hard withhold on this leg, i.e.
the run reports "recovery did not complete" — which is the truth. A cap of 600 (no
reversion) ships 542 degraded epochs from a field that was wrong by up to 1150 TECU for
the first two hours; the degraded advertisement would have to be extreme for that to be
honest. *Recommendation unchanged: cap 120 with reversion, because the cap-reversion
event is the operator-visible statement that a resume failed to re-converge.*

---

## 2. Policy P2 — statistic, asymmetry, hysteresis — RULED (D-G3: `rms_ho` ratio)

### MUSTs

1. **The load-bearing statistic MUST be a field-SHAPE statistic, not a₀₀ alone.**
   Measured basis: §0.3 (a₀₀ is datum-coupled ⇒ invariant #5) and §0.6 (relaxing the
   a₀₀ low edge far enough to stop the chatter loses the 20:47 event; the shape
   statistic catches it as an 86–107-epoch era instead of a 1-epoch blip).
   `rms_ho` (degrees ≥ 1) is the CANDIDATE; `rms_all` is acceptable with the noted a₀₀
   contamination (a₀₀ = 35 TECU contributes 2.2 TECU of a 3.8 TECU quiet `rms_all`).
   **All the ingredients already exist in `network_vtec_sh_health.csv`** (`rms_all_tecu`,
   `rms_deg0..15_tecu`, `kaula_ratio_deg*`) — no new statistic needs deriving, only
   selecting. The gate's own `sh_gate_rms_max_tecu` knob
   (`GnssConfigTypes.hpp:639`, default 0 = inert) is the existing hook.
2. **Thresholds MUST be asymmetric (engage ≠ release) AND time-hysteretic (two dwell
   counters).** Today `sh_gate_min_epochs` (default 1) gates engagement only; release is
   instantaneous on the first in-band epoch (`NetworkSolveGlue.cpp:1276-1278` resets
   `sh_gate_oob_run` to 0). Both halves are needed: threshold hysteresis alone leaves the
   4 chatter engagements, dwell alone leaves 3 (§0.6).
   Mirror `NetworkIonoDeweightGate`'s `sigma_hi_`/`sigma_lo_` naming and its
   forced-`lo < hi` load validation (`NetworkIonoDeweightGate.hpp:92-93`).
3. **The a₀₀ band is DEMOTED to a wide catastrophic rail, not deleted.** It is the only
   statistic that catches a pure DC runaway (a₀₀ → −91 TECU with a plausible field
   shape). MUST become asymmetric and wide enough not to fire on a realization offset.
4. **`sh_gate_min_epochs` = 1 MUST NOT remain the default once release dwell exists** —
   a 1/1 gate on a shape statistic will chatter on the release edge the same way.
5. **Counting stays un-throttled and epoch-based** (`REPORT …phase01.md` §6: the old
   throttled WARN under-read the reference run 23.8×). Engage/release/degrade
   transitions all MUST event.

### CANDIDATE numbers

Derived in §3, stated here for review:

| knob | CANDIDATE | basis |
|---|---|---|
| statistic | `rms_ho` (deg ≥ 1 RMS) | §0.6; scale-free under §3 normalization |
| engage | ratio > **4.0** × calibrated median | ref: catches all 3 events; leg: 1 engagement |
| release | ratio < **2.5** × calibrated median | ≈ 1.5× the converged spread |
| engage dwell | **3** epochs | kills 1-epoch blips; costs 3 epochs of latency |
| release dwell | **10** epochs (5 min) | merges the 4 chatter eras; < the 15–30 ep RS transient |
| a₀₀ rail | **[−60, +200]** TECU | leg healthy-depressed min −8.7; ref divergent −302/+2151 |

The a₀₀ rail is deliberately ~7× wider on the low side than the 0/−5 the card's framing
proposed: at −5 it still fires 5 times on this leg (§0.6), and the epochs it fires on
have a quiet `rms_all` of 5.8–8.6, i.e. it is firing on a level offset, not a defect.

### 2.1 Knob semantics — three traps the ruling creates

**MUST, all three.** D-G3 selects a statistic the existing knobs cannot express, so a
"reuse what's there" implementation would silently ship a different policy.

1. **`sh_gate_rms_max_tecu` is NOT the ruled statistic and MUST NOT be reused for it.**
   It is an **absolute TECU ceiling on `rms_all`** — the RMS over all 256 coefficients
   *including* a₀₀ (`NetworkSolveGlue.cpp:1245-1270`). The ruled statistic is a
   **dimensionless ratio** on the **degree ≥ 1** RMS. Ship new knobs
   (`sh_gate_ratio_engage` / `sh_gate_ratio_release` / `sh_gate_ratio_median_tecu` /
   engage+release dwell) and either retire `sh_gate_rms_max_tecu` or keep it as an
   independent absolute rail alongside the a₀₀ rail — but do not overload it.
2. **The calibration median is MANDATORY when the gate is armed, with no default.**
   The threshold's meaning is `ratio × median`; a wrong or defaulted median silently
   rescales the gate. The loader MUST refuse to arm (hard error, gate disarmed with an
   explicit message — the pattern already used for `cb_excursion_median_m ≤ 0` at
   `GnssConfigLoadNetwork.cpp:1584-1590`) if `sh_gate_ratio_median_tecu` is unset or
   ≤ 0. It MUST also be echoed into `network_run_manifest.json` and the run-end line
   (§3.2).
3. **The loader's arm/disarm validation inverts.** Today `sh_health_gate` is silently
   **disarmed** when the a₀₀ band is silenced (`low ≥ high`) *and*
   `sh_gate_rms_max_tecu == 0` (`GnssConfigLoadNetwork.cpp:1620-1639`), because
   band-only is the shipped design. Under D-G3 the ratio is primary and the a₀₀ band is
   an optional backstop, so a band-silenced config is **legitimate** and must arm. The
   new rule: arm iff the ratio pair + calibration median are valid; the a₀₀ rail and any
   absolute rms rail are optional; warn (do not disarm) if all backstops are silenced.
4. **`rms_ho` computation detail.** The gate's existing loop already accumulates
   `Σ val²` and picks out a₀₀ (`NetworkSolveGlue.cpp:1245-1254`), so
   `rms_ho = √((Σval² − a₀₀²)/(n−1))` is free. Under Iono-2 dual-layer the a₀₀ that is
   subtracted MUST be the layer **sum** (as `a00_total` already is, `:1257-1260`) and
   the coefficient count MUST span both layers, or the two shells' degree-1+ content is
   mis-normalized.

---

## 3. Policy P3 — empirical derivation, per the MW-thresholds directive

The directive (`feedback_mw_thresholds_empirical`): thresholds come from the data via
MAD/Welford/EWMA over a peer population; never from a stochastic-model σ. Applied here
with one measured amendment.

### 3.1 The requested derivation: the reference day's a₀₀ distribution

`runs/day129_cont/seg01/logs/network_vtec_sh_health.csv`, 2160 epochs (06:00–24:00):

| statistic | a₀₀ (TECU) |
|---|---|
| min / q0.001 / q0.005 / q0.01 | −302.61 / −133.88 / −23.69 / −19.79 |
| q0.02 / q0.05 / q0.25 | −5.57 / 20.99 / 31.50 |
| **median** | **37.25** |
| q0.75 / q0.95 / q0.99 / max | 43.51 / 373.46 / 1538.22 / 2150.98 |
| robust σ (1.4826·MAD) | **9.06** |
| fraction < 2.0 (current low bound) | **2.41 %** |
| fraction < 0 / > 150 | 2.36 % / 7.50 % |

Quiet-hours-only subset (hours 10–18, 1080 epochs, no known event):
min **21.88**, q0.05 29.34, median **37.49**, q0.95 44.85, max **51.21**,
robust σ **4.27**.

**Result, and the amendment it forces.** A band derived from this distribution —
e.g. median ± 6·robust σ = **[11.9, 63.1]** — would have withheld **~100 %** of the
guarded leg's late epochs, whose a₀₀ runs −8.7…22.7 with a *quiet* rms_all. The
reference day's a₀₀ distribution is **not transferable to another realization of the
same day**: the two runs' a₀₀ levels differ by ~25 TECU because a₀₀ trades against the
code-bias common mode. **An absolute a₀₀ band cannot be empirically derived in a
transferable way. This is a property of the quantity, not of the derivation.**

### 3.2 What IS derivable: a scale-free field-shape ratio

`rms_ho` calibration sets, all measured:

| calibration window | n | median | 1.4826·MAD | min … max |
|---|---|---|---|---|
| **guarded seed, converged tail (last 60 ep)** | 60 | **2.376** | 0.0532 | 2.251 … 2.483 |
| guarded seed, whole 6 h run | 720 | 2.277 | 0.4016 | 1.553 … 3.409 |
| reference day, quiet hours 10–18 | 1080 | 2.896 | 0.3151 | 1.841 … 5.774 |

The two independent converged runs agree on the **median** to 22 % (2.38 vs 2.90) but
their robust σ differs 6× (0.053 vs 0.315) — the 60-epoch tail measures *within-run
epoch-to-epoch stability*, not realization spread. Hence:

* **MUST: normalize by the calibrated MEDIAN (a ratio), not by a robust σ (a z).** The
  median transfers; the σ does not. A ratio threshold of 4× is 4× on either
  calibration; a z threshold of 20 means two different things on them (measured: the
  leg's healthy-looking tail is z = 64–84 in seed-tail units, z = 10–12 in ref-quiet
  units).
* **MUST: the calibration window is a CONVERGED, MULTI-HOUR window of the same chain**
  (the seed's tail, or a prior converged leg) — **never a trailing window of the run
  being gated** (§0.6: trailing robust z = 0 engagements on a run that begins diverged).
* **MUST: the calibration median is recorded in `network_run_manifest.json`** and in the
  run-end line. A gate whose threshold depends on an off-line calibration constant is
  unauditable otherwise.
* **MUST: re-derive per chain, and re-state in every report** that the constant is
  chain-specific (this is the same discipline the CB-excursion threshold got in
  `REPORT …phase01.md` §3.1).

Ratio values at the three reference events, in ref-quiet units (median 2.896):
07:32 = **77.8×**, 20:47 = **34.2×**, 19:21 = **4.8×**. Converged runs sit at 1.0–2.0×;
the leg's end-of-leg tail sits at **2.4–2.9×**. A 4.0× engage / 2.5× release pair
separates all three events from both converged runs, and correctly classifies the leg's
tail as *not yet converged but no longer divergent* — the exact state P1's grace exists
to express.

### 3.3 The a₀₀ rail, derived

Rail bounds must exclude every *healthy* realization and include every *divergent*
epoch. Healthy evidence: ref quiet [21.9, 51.2]; leg healthy-tail min **−8.7**.
Divergent evidence: ref min/max **−302.6 / +2151.0**; leg min **−1147.2**.
Widest healthy → nearest divergent margin: −8.7 → −19.8 (ref q0.01). CANDIDATE rail
**[−60, +200]** takes ~7× margin below the healthy minimum and still catches every
divergent epoch measured in either run by ≥ 5×. **MUST: the rail is the backstop; if
the rail is ever the only thing that fires, the report says so** (it means the shape
statistic missed a DC-only runaway and P2's statistic choice needs revisiting).

### 3.4 Harness

The counterfactual table in §0.6 is reproducible and MUST be regenerated against the
completed leg before §7 is ruled on:

```bash
python C:/Users/weaverb/AppData/Local/Temp/claude/C--GnssProcessor/82d12a18-5a6d-4df1-babf-3dc786c739ac/scratchpad/gate_policy_sim.py
```

**MUST: promote it into `scripts/` (extending `scripts/p2_gate_verdict.py`, per the
guard card's "extend rather than new one-offs") as part of the implementing card, and
make the chosen policy's counterfactual a required table in that card's report.** A
gate policy shipped without its counterfactual on both a converged run and a
seam-bearing run is a hand-tuned constant with extra steps.

---

## 4. Policy P4 — CB-excursion detector resume window — RULED (D-G5: state-coupled)

### MUSTs

1. **The window is derived from the shock, not fixed at 1 — and the completed leg says
   the shock does not decay on a 10-epoch scale.** Measured from the finished run
   (steady state = median over the last 120 epochs: `clock_std_m` 12.15 m,
   `worst_dev_m` 30.78 m):

   | from seam | `clock_std_m` | × steady | `worst_dev_m` |
   |---|---|---|---|
   | ep 0 | 371.1 | 30.5× | 1264 |
   | ep 10 | 398.1 | 32.8× | 1767 |
   | ep 20 | 469.4 | 38.6× | 2110 |
   | ep 38 | **586.2** | **48.3×** | 2680 |

   The clock spread **grows** for the first ~40 epochs and only stays below 5× steady
   from **ep 177** (tow 332910), below 2× from **ep 329**. So a 20-epoch window
   suppresses the ep-1/+2/+10 trips (172 / 54 / 69 m) but **not** the ep-69/+70 trips
   (115 / 87 m at tow 329670/329700) — which fired while the field was still 45× steady
   and a₀₀ ≈ −1000 TECU, i.e. those are shock artifacts too. **A fixed epoch window
   cannot separate them without being ~70 epochs long, which is too blind for a guard.**

   **D-G5 = STATE-COUPLED (ruled).** The window is `post_resume AND (grace active OR SH
   gate engaged OR epochs_since_seam < floor)`, floor = **20 epochs**. Rationale: the
   run already maintains an authoritative "not converged" state (P1/P2); a second,
   independent epoch counter would inevitably disagree with it. The floor covers the
   case where the SH gate is off or releases early.

   **Ordering fact that makes this clean (verified):** within one epoch the driver runs
   `networkSolveGlue` (`ApplicationNetwork.cpp:1488`, where the SH-gate verdict is
   computed) → `networkInstallCarry` (:1522) → `networkSolPrep` (:1535) →
   `networkEmitStore` (:1546) → **`networkClockQc` (:1585)** → `networkStashStatus`
   (:1603). So the CB detector reads `ctx.carry.sh_gate_engaged` **already set for the
   current epoch** — no lag, no stale read. **MUST NOT** add a defensive one-epoch lag;
   **MUST** state this ordering dependency in a comment, because it silently breaks if a
   future decomposition moves `networkClockQc` above `networkSolveGlue`.

   **MUST: the suppression window MUST end when the state clears** — it is not latched.
   Once grace has exited and the gate has released, the detector is live again even if
   `epochs_since_seam` is still small (the floor is a minimum, not a maximum).
2. **Suppression means OBSERVE AND COUNT, NOT ACT.** Inside the window the detector MUST
   still compute its statistic, update `cb_exc_max_median`/`_tow`, and emit its event
   with a distinct action (e.g. `cb_excursion_suppressed_resume`), while performing **no**
   CB reinit and queueing **no** disc bump. A silent suppression window would hide a real
   excursion that lands during it and would make invariant #4's telemetry lie.
3. **A distinct run-end counter** (`cb_exc_suppressed_resume`) with the same
   armed-but-never-tripped WARN discipline the detector already has. The report MUST
   state how many trips the window suppressed and what their statistics were, so the
   window's cost is visible and re-derivable.
4. **The window MUST NOT be open-ended, and MUST NOT be re-armed by anything but a
   resume.** A real excursion at seam+21 must trip.
5. **The pending-bump queue's behaviour across a withheld/graced era MUST be re-checked
   under P1**, since P1 changes which epoch is "the next emit". Today three reinits
   correctly collapsed to one bump landing 2 h later (§0.4); under grace the first
   graced epoch becomes the landing site, which is *better* (the bump lands nearer the
   value step) but MUST be confirmed, not assumed.
6. **Consider, and rule on, whether the window should also gate the CB reinit's σ.**
   The reinit uses `clock_monitor_reinit_sigma_m` (30 m). Three reinits inside 10 epochs
   is not "reset once"; it is a ratchet of its own kind. Out of scope for this card to
   decide, but the report MUST state whether repeated reinits inside a shock are a
   distinct defect. *(Recommendation: yes — record it, defer the fix.)*

---

## 5. Validation gates

A policy card's gates, all MUST:

1. **OFF byte-identity**, same-build, on the established 20-station × 11-epoch cold leg
   (`runs/bytecheck/`): `products.txt`, `network_solution.csv`, `network_trace.csv`,
   `network_epoch_status.csv`, `network_events.csv` SHA-256 identical against the
   pre-change binary. New disposition/columns are appended-only (T2d precedent).
2. **Counterfactual agreement**: for the *shipped* policy, the §3.4 harness's predicted
   engage/release epochs MUST match the live run's `SH_GATE` events exactly on a replay
   of one converged window and one seam window. A policy whose off-line simulation and
   on-line implementation disagree is not validated, whatever the counters say.
3. **ON-path counters non-zero** for every new mechanism: grace epochs, cap-reversion
   events, suppressed CB trips (invariant #4 — 0 fired = inert = FAIL, and each
   already-specified inert-case WARN is retained).
4. **A re-run of the seam** (a fresh leg resuming the same `runs/day129_guards` seed
   snapshot — armable with no new seed per §1 MUST-7) MUST show, and the report MUST
   tabulate: grace engaged at epoch 1; `n_emitted` advancing through the grace era;
   floored `clock_ura_m` / `sigc` / `sigcc` on the wire (read back from
   `products.txt`, not from the log); **the cap-reversion firing at epoch 121** — since
   §1.1 predicts 118 degraded + 424 withheld, a re-run that does *not* revert means the
   cap is not wired; and the SH statistic's engaged era *identical* to tonight's
   (P1 MUST-1: grace changes the disposition, never the verdict). The CB detector MUST
   report ≥ 3 suppressed trips (the seam+1/+2/+10 set) and MUST NOT suppress any trip
   after grace exits and the gate releases (P4 MUST-1's non-latching rule).
5. **Grading stays datum-invariant** (invariant #5): grade the degraded era's product
   with the between-satellite total-IF deviation methodology (handoff §3), not raw a₀₀.
6. **No hard exclusion**: nothing in this card may drop a measurement
   (`feedback_prefer_fallback_over_drop`). Grace *adds* an emission path; the gate's
   withhold remains emission-only (solve/carry/QC continue, `REPORT …phase01.md` §6).

---

## 6. Standing-invariant pre-flight

1. **Stiff/zero-q event response.** This card touches two stiff carries. The SH carry is
   *the* stiff quantity in §0.2's mechanism: a garbage value entered it at the seam and
   ratcheted for 239 epochs, which is invariant #1 in its purest form — a stiff carry
   with no event response for a *resume* event. **This card does not fix that** (§8); it
   makes the consequence honest on the wire. The CB carry's response (Phase 2.1) is
   narrowed by P4 so it does not fire on the shock itself. ✔ (with §8's deferral stated)
2. **Release-only, renamed exe** for every run; name-scoped WER-zombie pre-kill; LNK1104
   ⇒ retry. The chain currently running is `gnss_processor_p2chain.exe`. ✔
3. **Deferrals triaged.** *Field-blocking:* the seam shock itself (§8) — the SH gate is
   not field-runnable on a warm-resumed chain until either P1 ships or the shock is
   fixed, because 58 % of a leg is withheld. *Field-inert:* the pivot's missing URA
   channel (§0.5(5)) — inert only if grace excludes the pivot or the report states the
   gap; the repeated-reinit question (P4 MUST-6) — record-and-defer. ✔
4. **Run-end counters** for grace, cap reversion, suppressed CB trips, plus retention of
   the existing armed-but-never-tripped WARNs. ✔
5. **Datum-invariant grading only** — and this card's central technical finding is an
   invariant-#5 violation in the *shipped* gate (a₀₀ is not datum-invariant). ✔
6. **Default-OFF + byte-identity**, conditional fingerprint appends. ✔

---

## 7. Decisions — **ALL RULED 2026-07-28. This card is ready to implement.**

| # | ruling | binding in |
|---|---|---|
| D-G1 | degrade, don't withhold | §1 (7 MUSTs) |
| D-G2 | grace cap **120 epochs**, reversion to hard withhold | §1 MUST-5, §1.1 |
| D-G3 | `rms_ho` **ratio** to a calibrated median; a₀₀ → wide rail | §2, §2.1, §3.2 |
| D-G4 | conditional fingerprint append (+ grace outside the fingerprint) | §0.5(3), §1 MUST-7 |
| D-G5 | CB suppression **state-coupled**, 20-epoch floor | §4 MUST-1 |
| D-G6 | ship the pivot during grace, with an explicit report note | §1 MUST-3 |

One **prerequisite** remains, and it is a measurement, not a decision: the grace floor
VALUE (`resume_grace_ura_floor_m`, and the `sigc`/`sigcc` floors) is un-derived. It MUST
come from the consumer replay (`ApplicationConsumerReplay`; ~33 s/arm per
`project_consumer_replay_driver`) measuring consumer position error against floor value
over a degraded era — not from taste. Until it is derived the report MUST call grace
un-calibrated, and the §5 gates MUST NOT be read as validating the floor magnitude.

### Ruling details

* **D-G1 = DEGRADE.** Grace emits with a degraded quality advertisement instead of
  withholding. Binding: §1 (all 7 MUSTs), with the §1.1 cap table feeding D-G2.
* **D-G3 = `rms_ho` RATIO.** Field-shape statistic normalized by a calibrated converged
  median, a₀₀ demoted to a wide rail. Binding: §2 + §2.1's three knob traps + §3.2's
  ratio-not-z derivation rule.
* **D-G4 = CONDITIONAL APPEND.** `warmup_ura_floor_m` / `warmup_epochs` append only when
  armed. **Correction folded in:** this is back-compat hygiene and does **not** create an
  arming path across an existing seed (§0.5(3)) — so grace gets its own emission-only,
  non-fingerprinted knobs and must not re-arm the RTB-7(b) ramp (§1 MUST-7). That is
  what lets grace arm on the existing `runs/day129_guards` chain with no new seed.

* **D-G2 = cap 120, with reversion.** §1.1's table is the basis: on this leg the cap
  fires (118 degraded / 424 withheld), which is the correct report — a cap of 600 would
  ship 542 degraded epochs from a field wrong by up to 1150 TECU for its first two hours.
* **D-G5 = state-coupled, 20-epoch floor.** The completed leg changed this answer: the
  clock spread *grows* to 48× steady by ep 38 and only falls below 5× at ep 177, so a
  fixed 20-epoch window leaves the ep-69/+70 trips unsuppressed. Coupling to the
  recovery state P1/P2 already maintain covers them; the same-epoch ordering that makes
  this lag-free is recorded in §4 MUST-1.
* **D-G6 = ship the pivot, with a note.** Matches current behaviour
  (`NetworkSolPrep.cpp:425`); withholding was rejected because a consumer losing the
  pivot loses the datum. The cost — G01, the satellite whose CB ratchets, is the one
  correction a grace consumer cannot de-weight — is now a binding reporting requirement
  plus an epoch-level grace marker (§1 MUST-3).

---

## 9. Implementation order for the owning session

Not a design; a sequencing aid, since all six rulings are in. Each step is
independently gate-able, and steps 1–3 are byte-identity-safe when their knobs are off.

1. **§2.1 knobs + loader** — new ratio knobs, mandatory calibration median with a hard
   disarm, inverted arm/disarm rule, `rms_ho` from the existing coefficient loop.
   Gate: OFF byte-identity (§5.1) + the §3.4 counterfactual agreement on a converged
   window (§5.2). *This step alone fixes defect B and can ship without grace.*
2. **§0.5(3)/§1 MUST-7 fingerprint hygiene** — conditional append for
   `warmup_ura_floor_m` / `warmup_epochs`. Gate: an unarmed run still resumes the
   existing `runs/day129_guards` seed (fingerprint unchanged).
3. **§4 CB suppression** — state-coupled window + `cb_excursion_suppressed_resume`
   event/counter + the ordering comment. Gate: the seam re-run's ≥ 3 suppressed trips
   and the non-latching check (§5.4).
4. **§1 grace** — the emission-only path (own knobs, outside the fingerprint, never
   touching `ramp_lifted`), evaluated upstream of `NetworkSolPrep.cpp:140`, with the
   third disposition, all three σ floors, the epoch-level marker, and the cap-reversion
   telemetry. Gate: the full §5 set, notably the cap firing at epoch 121.
5. **Floor calibration** (the §7 prerequisite) — consumer replay sweep to derive the
   floor magnitude. Until this lands, grace is wired but un-calibrated, and the report
   must say so.
6. **Re-run the arming chain** for Phase 2.1 gate (iii), which is still unmet (§8). With
   grace armed the 20:47 event is still inside a withheld era on the current seed
   placement — so **either** move the seed so the first event is ≥ 400 epochs after the
   seam (the zero-code mitigation) **or** accept that the validation event needs its own
   leg. Do not read gate (iii) off a run whose event sits inside a graced or withheld
   era.

---

## 8. What this card explicitly does NOT fix

**The seam shock.** §0.2's mechanism — the D5 innovation gate re-tapping 92 % of a
restored DD carry at the first post-resume epoch, collapsing the phase constraint and
letting the SH field slide 1400 TECU — is an *estimation* defect, and it is the load-
bearing one. Every number in §0 says so: no emission policy makes tonight's leg's
product good, because the field was genuinely wrong for ~3.5 of its 5 hours and had not
re-converged at the end. That defect belongs to `docs/SPEC_warm_resume_event_card.md`
(the RS card), whose §"snap_wwork audit" is still open, and it should be re-scoped with
this measurement in hand: **the RS card's D5-warmup mechanism now has a measured SH
consequence and a measured re-tap fraction at 305 stations.**

**Tonight's leg's validation value.** The 20:47 excursion — Phase 2.1's validation
event — fell inside the withheld seam era, and the leg does not reproduce the reference
run's 19:21 event either (§0.4). Whatever this leg reports, **Phase 2.1 gate (iii)
remains unmet**, and the arming chain needs a re-run whose seam is either graced (P1) or
placed far enough ahead of the events to have recovered. Placing the seed so that the
leg's first event is ≥ 400 epochs (3.5 h) after the seam is the cheap mitigation
available today with no code at all.
