# RF-1-steps345: the float-vehicle mechanism in three armed steps (2026-09-15)

**STATUS**

- **id:** RF-1-steps345
- **topic:** network-vehicle
- **status:** closed
- **summary:** The float vehicle is CORRECT (D1 b 1.001 field, 1.028 sim) and NOT SUFFICIENT on the field, where the phase datum's churn dominates; `float_kf` stayed default OFF pending RF-2.
- **dates in the text:** 2026-09-15
- **knobs / posture:** cumulative arms on the RF-0 configs: A3 `network.ambiguity_vehicle: float_kf` (the joint local block structural), A4 + `amb_float_birth_var_m2: 0.25` with drop semantics (`filter/NetworkAmbFloat.cpp`), A5 + `amb_float_declared_handover: true`; interlocks: `carry_conditional_coupling` required, `carry_coupling_joint_whiten` exclusive
- **hashes named in the text:** commits `7ccced8` (step 3), `c3a2bf8` (step 4), `ee16ddc` (step 5) on worker branch `worktree-agent-a2915f195d8518bf6`, merged; combined tree `f291dcc`; OFF smoke `87a33bf0…`
- **run dirs named in the text:** `_rf1_ctl_field6` (the like-for-like control)
- **source:** [RF-1-steps12](RF-1_steps12_read_2026-09-15.md) (the pre-registration, bars and frozen readings); [RF-1 design](RF-1_float_ambiguities_design.md) section 7.6

**Outcome.** The card arms the mechanism in three cumulative steps (joint block, floored birth prior with a bounded drop, declared anchor S-transform) on both beds. On the sim the DD ambiguity error falls 14× and the consumer settles at 0.30 m; on the field neither I1 nor I2 crosses and PB marks above 2 m rise from 75 to 350, past the storm bar. The frozen reading "correct and not sufficient" applies: the live float state makes the phase datum churn, which [RF-2](RF-2_held_basis_2026-09-15.md) was sharpened to hold. After a KEEP OFF on the RF-0g posture ([RF-3-prep](RF-3-prep_golden_rebase_2026-09-15.md)), the vehicle became the goldens' posture on 2026-09-22 ([GRB](GRB_golden_rebase_2026-09-22.md)).

**Status as recorded (2026-09-15):** closed — **the vehicle is CORRECT (S4 / S3 cross decisively on both beds; the sim's consumer and code wire cross) and NOT
SUFFICIENT on the field, where the phase DATUM's churn dominates**; `float_kf` stays the posture candidate, default OFF, un-armed until RF-2
makes the phase datum a held S-basis.  Worker branch `worktree-agent-a2915f195d8518bf6` (`7ccced8` step 3, `c3a2bf8` step 4, `ee16ddc` step 5),
merged.  Suite 2157 / 289 (0 failed; `AmbFloatVehicleTest` 34); the OFF smoke `87a33bf0…` after every step; `--verify` PASS; the combined tree
certified first (fast-forward to f291dcc, suite 2145 / 289, smoke unchanged).

**Step 3** — the joint local block structural under `float_kf` (`pl_local_block` = the F3 knob OR the vehicle; interlocks: `carry_conditional_
coupling` required, `carry_coupling_joint_whiten` exclusive); the PD fallback is a **run-end failure above 1 %** — measured 0 of 43 410 (field)
and 42 626 (sim) station-epochs.  The §6.2 test at the real seam: the joint fold gives `var(n)·n / var(1) = 1` to 1e-6 at all 30 folds (`b` =
1.000); the legacy fold on the same fixture `b > 1.15` — the over-count reproduced at the seam (a collapsed variance is only dishonest against
something that MOVES; the D4 fixture carries an un-modelled phase-only drift).  **Step 4** — `filter/NetworkAmbFloat.cpp`: the drop restores
`max(var_birth, var_first, floor)` (the floor elevation-mapped at the code weights' power), clears coupling, keeps `value_abs` and the birth
stamps; at the five slip latch sites, the inject-site gap cliff, and T4 / T3 (drop in place of erase); the gap prune stays an erase.  **Step 5** —
the declared anchor S-transform moves all three references together (per-station levels, the shared PB carried prior, every coupling `s_prev`)
with a per-family `STRANSFORM/sts_applied` event and a self-check (0 failures); scope stated: only a CONTINUOUS handover has a Δ — a cold
election keeps the erase, counted (**55 of 81 field handovers are cold**).

## The internals — the vehicle does what it claims (the like-for-like control `_rf1_ctl_field6` = the RF-0 base + the trace)

| field | control (level) | A3 | A4 | A5 |
|---|---|---|---|---|
| fold route | coupling-scalar | block | block | block |
| **D1 `b` p50** | **3.73** (fold_var 0.37 → 1.4e-5 over 30 folds, 26 000×) | **1.001** (15.6×) | 1.045 | 1.042 |
| D4 mature prior σ̂₀ (robust) / |t| p90 | **2.98 / 12.2** | **0.096 / 0.47** | 0.114 / 0.53 | 0.266 / 1.97 |
| prires `pl` σ̂₀ p50 (the production census) | **129.5** | **1.45** | 1.77 | 3.07 |
| sim D1 `b` / prires σ̂₀ | — | 1.028 / 1.045 | 1.028 / 1.047 | 1.025 / 1.24 |

**S3 (sim, DD ambiguity error vs truth, ref-free, settled):** L1C med |e| **1.28 → 0.092 cyc (14×)**, L2W 1.64 → 0.114, WL(1,2) 0.355 → **0.030**;
**WL at the correct integer 59.5 % → 99.0 %**; the arc-age slope −0.12 (a frozen level) → **−0.55 (1/√K)** — by age 3.97 → 0.14 cyc.  This is the
first integer-capable network ambiguity on the sim in the project's record; the "amb / iono split over-count" (the B-band priors asserting one
datum B× per epoch) was the scalar fold, and the joint block removes it.

## The two instruments — split by bed, and the split is the finding

| bed · hour | consumer p50 RF-0 → A3 → A4 → A5 (m) | D_phase declared RF-0 → A3 → A5 | D_code RF-0 → A3 |
|---|---|---|---|
| SIM 1 / 3 / 5 / 6 | 3.31 → **1.48** / 3.57 → **0.77** / 1.05 → **0.31** / 0.57 → **0.30** (A4 = A3; A5 0.35) | 5.01 → 5.66 → 5.61 / 1.18 → 2.08 → 2.55 / 0.97 → 1.93 → 2.81 / 0.39 → 1.66 → 4.84 | 0.81 → 0.28 / 0.20 → 0.07 / 0.18 → 0.06 / 0.02 → — (2.5–3× every hour) |
| FIELD 1 / 3 / 5 / 6 | 3.02 → 5.80 → 6.20 → 6.20 / 1.54 → 1.87 → 1.86 → 7.56 / 0.98 → 4.53 → 5.37 → 2.60 / 3.29 → 4.47 → 6.03 → 7.16 | 3.21 → 9.31 → 9.47 / 4.79 → 7.85 → 32.5 / 4.31 → **115** → 89 / 1.48 → 14.9 → 62.6 | 0.40 → — / 0.12 → — / 0.22 → 0.14 (improves) |

Availability 720 / 720, 0 withheld, every arm; unsignalled clock steps > 2 m **0** everywhere.  **Marks blow the storm bar**: CLK > 2 m 1 → 8 → 5 →
42; PB > 2 m 75 → 212 → 252 → 350 (A5).  Field response population per hour: ~180 T4 refloats, ~209 gap prunes, ~98 anchor re-references (the
sim: 12 latches and 0 T4 in 6 h — it cannot grade step 4).  A5 doubles the field's T4 drops (1 348 → 2 698): the transform keeps carries alive
that T4 then flags — a treadmill at a new level.

## Grade and reading

S4 passes on both beds after A3 (`b` 1.001 / 1.028); S3 crosses 14×; the sim's consumer crosses (settled 0.57 → 0.30 m, every hour better) and its
code wire crosses (2.5–3×); its phase wire does NOT (D_phase 1.7–4.8× worse by hour 6).  The field crosses nothing on the two instruments and
worsens on both, while its code wire improves and its internal instruments improve by two orders of magnitude.  **Reading:** the float vehicle
is right — and once the ambiguity is a live state, **the satellite phase bias's datum, which is one station's ambiguity, becomes a live state
too**: it converges, it drops, it is re-flagged by T4 at 454 / h, and every move is a declared PB re-realization the consumer must re-converge
through.  The level vehicle's frozen zero-q anchor was, accidentally, a held S-basis; `float_kf` removed the accident.  The textbook makes the
S-basis ambiguities CONSTANTS, not states — that is RF-2, sharpened below — and step 5's transform cannot reach it because 55 of 81 field
handovers are cold and because a T4 hit on the anchor is a datum change nobody declares.  §7.6's "correct and not sufficient" branch applies;
the pre-registered "level's content" card is deferred behind RF-2, since the phase datum's churn is the larger term by an order of magnitude.

## RF-2 (pre-registered, sharpened by this card): the phase datum as a HELD S-basis with declared transformations at EVERY handover

Build, default OFF, byte-identical, under `float_kf`:
1. **Held basis:** the anchor station's ambiguity for each pinned (satellite, signal) is the S-basis — HELD at its value at election (removed from
   the float update: its column pinned with the gauge weight, not carried as a state), the satellite phase bias then estimable as (PB + that
   constant); all other ambiguities are float states as RF-1 has them.  A basis ambiguity that must move (a slip / T4 / gross flag on the anchor
   station, the arc's death) is never re-realized in place: it is a **handover**.
2. **Every handover declared:** continuous AND cold — a cold election is a new basis value, so the wire's PB steps by exactly the difference and
   the step is DECLARED (`markSatDiscontinuity` + the transform's `sts_applied` event); the network-wide erase is retired (counter must read 0).
3. **Election:** the successor is the tracking station with the highest elevation among arcs older than `phase_anchor_min_age_ep` (10) and not
   flagged by any screen in the last 5 epochs (R5 / R6: never a newborn low arc, never a split-referencing family); an incumbent is not deposed
   while it qualifies (sticky).
4. **T4 on the anchor:** a T4 / slip / gross flag on the basis ambiguity triggers the handover BEFORE the response acts on that column (the
   response then applies to the ex-anchor as an ordinary float state).
Gates: unit tests (the held column's variance does not change under the fold; a declared cold handover shifts the PB by exactly Δ and the
consumer's counter moves; the election refuses a newborn / low / flagged candidate); the Debug suite; the OFF smoke; `--verify`.  **Arms:**
A3 + RF-2 on both beds, 6 h, on the RF-0 configs (and on RF-0d's posture once it is the baseline).  **Bars:** field and sim D_phase declared
within-arc rms ≤ RF-0's in every hour (the PB wire no longer churns); PB marks ≤ 1.5× RF-0's count; the field consumer per-hour p50 no hour
worse than RF-0 and settled ≤ 0.6×; the sim's S3 preserved (WL ≥ 98 % at the integer) and S4 `b` within 1.00 ± 0.15; unsignalled steps 0;
availability within 1 %.  **Frozen reading:** all bars ⇒ RF-1 + RF-2 is the vehicle and the goldens re-base on it; D_phase fixed but the field
consumer still worse ⇒ the remaining term is the response population itself (T4 at 180 / h, gap prunes at 209 / h) — RF-3 with GL's screen
in front of T4; the sim's integers lost ⇒ the held basis is mis-specified (which column, how many per satellite) — stop and read.
