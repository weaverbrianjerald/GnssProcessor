# F3 field: pre-registration of the field F3 ladder, with pre-data amendments

**STATUS**

- **id:** F3 field
- **topic:** network-vehicle
- **status:** closed
- **summary:** Pre-registered field F3 ladder (shipped control, leveling vehicle, leveling plus F3) with truth-free instruments; the amendments find the control's DD ambiguities wandering ~10 cycles (arc MAD), and headline P2 was later FALSIFIED.
- **dates in the text:** 2024-05-08 (the DOY-129 bed) → 2026-09-09 (pre-registration and amendments)
- **knobs / posture:** `network.phase_leveling_local_block` (F3, arm 2); arm 1 = `carry_invariant: false` with its sub-family off, the EWL/UWL lanes off, `cycle_slip.relevel_free_epoch: true`; `network.raim_carry_invariant` (left off on arm 0); `processing.max_duration_s` (the smoke clip; `network.max_duration_s` only clips the RINEX parse); `processing.frequency_mode` (dual, so no L5 readout)
- **hashes named in the text** (commit / exe / byte-cert): `00d5fdf` (source of the one arm exe, `gnss_processor_f3field.exe`)
- **run dirs named in the text:** `runs/fld63_stk_on` (the field control baseline); arms `fldf3_ci`, `fldf3_pl`, `fldf3_on`
- **source:** `docs/archive/SPEC_amb_iono_split_overcount.md` §6.15, 6.15.1, 6.15.2, 6.15.3, 6.15.3a

**Outcome.** The card fixes the bed (DOY-129 00 to 12 UT, 61 GPS L1/L2 stations), three arms that switch the vehicle first and arm F3 second, seven instruments and predictions P1 to P7. Its amendments, read on the control, find the per-band DD estimates moving 1:1 in cycles across bands (slope 1.004) and with the estimated DD ionosphere (slope +1.68): a uniform DD fraction here reads as wander. The [results card](../network-vehicle/F3-FIELD-ARM-RESULTS.md) graded P1 PASS, P2 FALSIFIED on this chain, PARTIAL on the converging leveling posture; F3 is KEEP OFF on the field and off in the [re-founding](../../refound/NETWORK_REFOUNDING_2026-09-14.md) posture.

> Results are appended TO THIS FILE, below the verbatim text. The archived monolith is frozen; never append there.

---
<!-- BEGIN VERBATIM: SPEC_amb_iono_split_overcount.md -->
## §6.15 The FIELD F3 arm — PRE-REGISTERED 2026-09-09 (§7 item 21; campaign queue item 3's lineage question deferred to a triple-frequency chain)

F3 (`network.phase_leveling_local_block`) is sim-validated only: 3 h (§6.8), 24 h (§6.9, ref-free re-grade §6.12
— L1C 0.089 cyc / 86.8 % at the true integer over the day), and it is the vehicle of `golden_sim_tri_w6`.  The
field has never run it: every field lineage (golden_field_gps_dual, FT63/EXT63/FLD63) carries ambiguities on the
Card-4 `carry_invariant` vehicle, under which the phase-leveling inject is skipped outright
(`ApplicationNetworkAssembly.cpp` `do_amb_carry = (…) && !carry_invariant`), so F3 is structurally inert there.
A field F3 arm is therefore a VEHICLE SWITCH first and F3 second, and the ladder is built that way.

### 6.15.1 Bed, arms, provenance

Bed: DOY-129 (2024-05-08) 00–12 UT, the E3 field posture `_fld63_stk_on` = `_ext63_12h_evt_pl_clkpin` (61 real
GPS L1/L2 stations, SH-VTEC deg-8 co-estimated iono, external COD orbit/code-bias, precise-clock soft hold,
pivot clock datum MBAR00UGA, `phase_leveling: true`, `carry_invariant: true`, `carry_conditional_coupling: true`,
`phase_leveling_max_gap_s` 900, Card R armed, `emit_clock_disc`, the standard QC/observability set) +
`sticky_pivot: true`.  WTZZ00DEU is the colocated hold-out consumer.  The chain sits at the loader's 8-level
`extends` limit, so the arms restate the posture on `_ext63_12h_evt_pl_clkpin` rather than extending
`_fld63_stk_on`; smokes and replays are siblings, not children.

| arm | config | knobs beyond the posture | single mechanism |
|---|---|---|---|
| 0 `fldf3_ci` | `_fldf3_ci.json` | `trace_rx_states`, `trace_zwd` (trace-class) | the shipped posture, same-build control |
| 1 `fldf3_pl` | `_fldf3_pl.json` | + `carry_invariant: false` (with its sub-family `carry_invariant_innov_gate / _innov_calibrate / _innov_corroborate / _honesty / _pd_safe` set false explicitly — inert without the vehicle, and the loader hard-errors on `innov_gate` without `carry_invariant`), and the EWL/UWL lane layer off (`mw_wl.ewl_enable / uwl_enable / uwl_cold_fix / wl_inject_enable: false` — the lanes ride the invariant carry's chord basis and the loader hard-errors without it), `cycle_slip.relevel_free_epoch: true` | the VEHICLE (leveling carry + its invariant-#1 slip response; the lanes are structurally unavailable on it) |
| 2 `fldf3_on` | `_fldf3_on.json` | + `phase_leveling_local_block: true` | F3 |

`relevel_free_epoch` is part of the leveling vehicle, not a fourth arm: standing invariant #1 requires the zero-q
leveling carry's wired slip response, the re-level-walk fix IS that response (SPEC_slip_relevel_walk, 720/720 on
wu93; pinned on the sim golden) and `mw_detect` is already armed on the chain.  Everything else stays at the
chain's value in every arm, two of which matter for reading the result: `phase_datum_handover_max_var_m2` is the
loader default **1.0 m²** on this chain (not the 1e-4 of the FT/camp60wu sticky lineages), so the honest-σ
successor interlock of §6.8.3 cannot bind here; and `phase_datum_handover_continuous` is **false** (cold PB-gauge
handovers, as the chain has always run).

⚠ **A named asymmetry, kept on purpose.**  On the `carry_invariant` vehicle the post-fit network RAIM pool is
inert unless `network.raim_carry_invariant` (default false) re-hosts it (`ApplicationNetwork.cpp` calls it under
`raim_on && (carry_warm || raim_ci_on)`, and `carry_warm` needs the leveling carry) — arm 0 reports RAIM armed and
never runs it, while arms 1 and 2 run it live once warm, with refloats.  Arm 0 is left as the SHIPPED posture
(comparability with `runs/fld63_stk_on` and the field golden) rather than re-hosted off-posture; the card's
graded delta is **arm 2 ↔ arm 1** (same vehicle, same live pool, same refloat response), and the vehicle-switch
reading (arm 1 ↔ arm 0) is graded on the instruments that exist on BOTH sides — the prires / block-Baarda /
station-congruence run-end counters and the truth-free DD instrument — with this asymmetry stated.

Provenance: one exe for every arm, built from HEAD `00d5fdf` in a detached worktree
(`.claude/worktrees/f3field`, VS 2026 Release), snapshot `gnss_processor_f3field.exe` beside its DLLs, sha256
`4c6aa137f67888de32afb851a2d72dbb29dcb85954f8d73739a71a6d4c1d69b4`; runs sequential, each from its own
`runs/<arm>` CWD (the traces are CWD-relative); 20-epoch smokes of all three arms first (`processing.max_duration_s`
600 — the `network.max_duration_s` key only clips the RINEX parse; a first smoke that set the wrong key ran
the full window and was killed).  `mw_wl.enable` and `lane_carry.enable` are OFF on the chain already; the chain's
EWL/UWL lanes (`mw_wl.ewl_enable`, `uwl_enable`, `uwl_cold_fix`) are the one further thing the vehicle switch
must drop, so the vehicle-switch reading (arm 1 ↔ arm 0) also carries "no EWL/UWL lanes" — the lanes touch the
invariant carry's chords and the consumer's lane products, not the phase-leveling carry, so the F3 delta
(arm 2 ↔ arm 1) is unaffected.

Baseline of the truth-free instrument on the field control (`runs/fld63_stk_on`, 2026-09-02 exe, stride 4, G):
L1C as-traced med|f| 0.249 (null 0.246), near 19.4 %, R 0.461 (null 0.472), ref-free gap +0.012; and the
synthesized **WL(1,2)** — 37,913 paired chords of 66,087 L1C / 202,411 L2W rows (the unpaired rows are
mixed-reference or single-band chords) — reads R 0.487 (null 0.482), near 21.4 %, ref-free gap +0.005: ALSO at
the null.  So the control's L1 DD errors are multi-cycle even at the WL scale (|e₁| ≳ 2 cyc), consistent with
§13.8.2's "~5 cycles off with a ±20-cycle tail".  That is the reading arm 2 has to move.

### 6.15.2 Instruments (all truth-free; the field has no `truth_ambiguities.csv`)

1. **DD integer structure** — `scripts/dd_frac_reref.py <run> --signal {L1C,L2W} --cons G` (§6.13.1).  ⚠ Its
   null is the UNIFORM case = no integer structure, so "at the null" is the WORST reading, not a floor: the field
   control reads R 0.468 vs null 0.473 / ref-free gap +0.008 (§6.13.2) and improvement is leaving the null UPWARD
   (R − null > 0, near-integer share > 20 %, ref-free GAP < 0 — the sim F3 base reads R 0.610 vs 0.430, gap
   −0.047).  Quote the GAP to the size-matched null, never the raw median; closure against its DD-structure null.
   ⚠ An at-null reading is AMBIGUOUS on a truth-free bed: multi-cycle DD errors wrap to uniform and read exactly
   like "no integer content" (§6.13.2's run-selection lesson).  So the falsified branch is paired with three
   SCALE-BEARING readouts before it is called a falsification: (i) the **WL(1,2) DD fraction** synthesized from
   the same trace — `dd_cyc(L1C) − dd_cyc(L2W)` per (epoch, sat, ref, sta_a, sta_b) chord, joined on the SAME
   reference satellite in both bands (the reference is elected per signal, so mixed-reference rows are dropped
   and counted), λ_WL = 86 cm.  An n-cycle L1 error is n/3.5 cycles at WL if iono-driven ((f₁−f₂)/f₂ = 0.283 —
   the applicable factor, since the sim root cause is 99 % iono) and n/4.5 if range-common (λ₁/λ_WL = 0.221), so
   a sub-half-cycle WL corresponds to |e₁| ≲ 1.8 cycles: multi-cycle-but-bounded L1 errors show WL structure
   while L1/L2 stay uniform (`scripts/dd_frac_wl.py` writes the synthesized trace in the same schema; the same
   tool grades it AS-TRACED and REF-FREE with the same nulls — quote the ref-free gap); (ii) the trace's own
   `dd_sigma_cyc` median per arm, filtered to rows with `dd_sigma_cyc ≥ 0` and `sigma_carry_aware = 1` (the σ is
   −1 when the covariance is unavailable and the carry-aware flag says whether it could see the carried state —
   the retained fraction is reported per arm so arms are compared on the same population; an honest σ ≥ 0.1 cyc
   on the leveling arms, a collapsed sub-0.02 cyc σ beside a uniform histogram is the over-count's signature);
   (iii) the prires / σ̂₀ families of instrument 2.  Per-station and per-hour splits off this trace compare ROTATING populations
   (the field reference switches 8–11 times/day and a chord needs both stations to see it) and are quoted with
   that caveat.
2. **The σ̂₀ families** present on both vehicles: prires census (worst |t|, rows > 4σ̂₀), block-Baarda census
   (blocks flagged / block-only), station congruence, the constraint-aware global σ̂₀ per family; T3/T4/T1r counts
   from `network_flag_rates.csv`; the post-fit σ̂₀ where the pool is live.
3. **F3's own counters** (invariant #4): the `Network (leveling local-block F3)` run-end line — blocks whitened,
   rows emitted, columns marginalized at inject, pairs zeroed, columns refused, factorization fallbacks, block-dim
   histogram, within-epoch refloat re-whiten ok/failed.
4. **Continuity censuses**: PB-gauge handovers (`network_events.csv` PHASE_DATUM / PBGAUGE tiers), the
   leveling-carry retirement census [A]/[C], the rx-CB carry line, the D0/INC-C0 carry census.
5. **ZWD plausibility** (`network_zwd.csv`, `trace_zwd`): settled per-station |ΔZWD| per epoch (p50/p95),
   hourly |rate| p95, formal σ, per-station span.  No ZTD truth on this bed; the reading is physical
   plausibility (a ZWD that moves decimetres per hour is the over-count wandering, §6.8.4's field analogue).
6. **Consumer**: the in-process WTZZ00DEU consumer (metre-class and non-attributing on this chain — §18.8 item 3;
   reported, not leaned on) and a float-only cold-consumer REPLAY of each wire
   (`_fldf3_{ci,pl,on}_replay.json`, `ar_mode 0`, `Application::runConsumerReplay`) — hourly 3D rms and p50,
   settled ≥ h6.
7. **Cost**: D0 total ms/epoch median after epoch 30 (sim: +7.4 % under F3).

### 6.15.3 Pre-registered predictions (written before any 12 h arm ran; smokes only)

- **P1 (F3 fires and is well-conditioned).** Arm 2's F3 line shows blocks whitened every warm epoch (≈ 61 per
  epoch → ~8×10⁴ over 12 h), 0 factorization fallbacks, 0 columns refused; the block-dim histogram sits in the
  10–49 range (GPS dual: ~2 bands × ~8–12 sats per station).  Falsifier: fallbacks > 0 (the F1 diagonal fallback
  re-asserts the station-common directions, §6.11 arm 5) or refused columns.
- **P2 (the headline).** Arm 2 lifts the field DD fractions OFF uniform on L1C and L2W: R − null ≥ +0.05,
  near-integer share ≥ 25 % (uniform 20 %), ref-free GAP ≤ −0.02, settled half (h6–h12).  Arms 0 and 1 stay at
  the null (|R − null| ≤ 0.02, |gap| ≤ 0.015, as `fld63_stk_on`).  Falsifier: arm 2 at the null on L1C, L2W AND WL(1,2) with an honest `dd_sigma_cyc` → the field's
  non-integer DDs are NOT the leveling over-count (the field residual is then multipath / real slant-iono
  mis-modelling under the SH posture / the §13.8.2 multi-cycle tail), and the AGU-26 "uniform 0.25" stands with
  F3 exonerated as the cure.  A partial lift (R − null in (0.02, 0.05)) is a real but insufficient effect and is
  reported as such, not rounded either way.  Intermediate branch: L1C/L2W at the null but WL(1,2) lifted → the
  errors are multi-cycle-but-bounded at L1 (1–4 cycles) and F3's effect is graded on the WL readout; PARTIAL.
- **P3 (station-common wander).** Arm 1's ZWD wanders (hourly |rate| p95 and per-station span markedly above
  arm 0's — the over-count's field signature, §6.8.4: sim F1 rms 0.19 m) and arm 2 brings it back to arm 0's
  scale or below (sim F3: 0.009 m rms, |err|/σ 0.94); arm 2's formal ZWD σ is HONEST-scale (≥ 3 mm), not the
  collapsed sub-mm of a diagonal carry.
- **P4 (σ̂₀ families).** Arm 2 ≤ arm 1 on prires rows > 4σ̂₀, block-Baarda blocks flagged and worst |t|; T4
  attribution flags on arm 2 ≈ 0 where arm 1 shows the over-confident coupling rows' own residual (sim: 288 → 0).
  Against arm 0 no direction is pre-registered (different live QC).
- **P5 (consumer replay, float).** Settled (h6–h12) 3D rms: arm 2 ≤ arm 1; arm 2 vs arm 0 not pre-registered
  (vehicle + RAIM asymmetry), reported.  The in-process AR consumer is reported only.
- **P6 (continuity).** PB-gauge handovers on arm 2 are not more often cold than on arm 1 (same ceiling, same
  policy); leveling carries erased on arm 2 within 2× of arm 1.
- **P7 (cost).** Arm 2 D0 median within +15 % of arm 1.
- **Stop rules.** Any arm exit ≠ 0, an F3 fallback count > 0, or a withhold era > 60 min ends the ladder before
  grading; a smoke that shows loader warnings naming any ladder knob as INERT stops it before the 12 h arms.

### 6.15.3a Pre-data amendments (2026-09-09 17:45–18:00, written while arm 0 was still running and before any 12 h arm was graded; on peer review)

1. **L5 is absent on this chain** (`processing.frequency_mode: dual`; the control trace carries only G/L1C, L1W,
   L2W, L2X), so the EWL(2,5) readout that would discriminate at the ~5-cycle scale (÷17.9 iono-driven) is
   unavailable here.  The wrapped scale check on this ladder tops out at WL(1,2)'s ÷3.5.
2. **The falsifier of P2 is sharpened.**  The WL(1,2) instrument is at the null on the CONTROL too, so an
   at-null reading on arm 2 bounds the RESIDUAL DD error at ≳ 3 L1 cycles (a uniform WL fraction needs
   |e_WL| ≳ ~1 cyc) and falsifies "F3 brings the field to the integer regime" — it is NOT by itself a proof
   that the leveling over-count is absent (F3 may remove it while another multi-cycle error dominates).
   Conversely the control's WL null is a FINDING, not a baseline: a truth-free lower bound of ≳ 3 cycles on
   the field's DD error scale, corroborating the multi-cycle figure that only the truth side had before.
3. **A third, UNWRAPPED scale readout — per-chord DD ARC STABILITY** (`dd_arc_stability.py`, v3; promoted to
   `scripts/` with the results).  For the SAME chords on the SAME arcs in both bands (paired epochs, arcs keyed on
   `ref_sat` so a reference switch starts a new arc, first 10 epochs of every arc dropped, ≥ 20 epochs, settled
   ≥ h6): MAD of `dd_cyc` about the arc median, drift = |median(last 10) − median(first 10)|, the cross-band
   slope of the deviations (1.283 iono-driven, 0.779 range-common, 1.000 cycle-common), and a REF-FREE pass
   (group median over satellites of the arc-relative deviation removed) quoted beside a size-matched
   iid-Gaussian null of the same subtraction (the estimator shrinks the statistic by itself — the wrapped
   grade's trap 1 transposed).  ⚠ STABILITY IS NOT QUALITY: it cannot see a constant bias (a frozen wrong level
   is perfectly stable — the diagonal-carry sim arm is the MOST stable thing measured here and carries 1.3–2.7 cyc
   of real ref-free error), so an arc-stability number is never quoted alone; every one is paired with an
   accuracy readout (truth-graded DD on the sim; the wrapped fraction and the σ̂₀ families on the field), and
   arms are never ranked by stability.  With the fraction test it splits a multi-cycle DD error into BIAS vs
   WANDER.  Semantics on the sim (v3, trimmed, settled ≥ h6, 6,893 arcs): the F3 24 h arm reads raw MAD p50
   0.148 cyc L1C / 0.189 L2W / 0.047 WL (ref-free 0.063 / 0.080 / 0.021; ref-free/raw REAL 0.43 vs NULL 0.68 —
   the fixed G01 reference's own wander is genuinely in every chord, §6.12) with cross-band slope 1.262 (p10 1.16,
   p90 1.35, corr 0.995) — its residual wander is IONO-SHAPED, the honest per-LOS accumulation floor; the diagonal-carry sticky arm 0.020 / 0.018 / 0.020 (frozen-but-biased: its DD
   errors are 2–20 cycles, §6.13.2), slope uninformative (noise, corr 0.39).  Validation criterion (amended
   17:55, before any field arm was graded — an absolute "≪ 0.1" gate first stated in review was mis-specified,
   since the F3 arm's honest iono-shaped wander is itself 0.1–0.2 cyc): the instrument must show SEPARATION,
   and it does — the sim F3 arm sits ~70× below the field control and the frozen diagonal arm ~7× below F3.
   **Field control (`runs/fld63_stk_on`, L1C+L1W / L2W, 361 chords, 372 arcs):** raw MAD p50 e1 **10.4** / e2
   **10.4** / wl **0.57** cyc (p90 36 / 36 / 1.6), drift p50 25 / 26 / 0.8 cyc per ~65-min arc, 100 % of
   per-band arcs with MAD > 1 cyc; cross-band slope p50 **1.004** (p10 0.962, p90 1.048; corr 0.999); ref-free
   MAD ratio REAL 0.65 vs NULL 0.77 (gap −0.12 on e1, −0.15 on wl).  (The equal MADs, the slope and the small
   WL are ONE measurement — the joint (σ₁, σ₂, ρ) = (10.4, 10.4, 0.999) structure — presented three ways: the
   OLS slope is ρσ₂/σ₁ and the difference's spread σ√(2(1−ρ)) ≈ 0.47, not three independent agreements.)
   So on the shipped field posture the
   per-band DD ambiguity ESTIMATES are not frozen biases: they WANDER by ~10 cycles (MAD) / ~25 cycles per hour,
   in lock-step across L1 and L2 — 1:1 in CYCLES, which is neither the iono direction nor a range-like error —
   while the WL DD is ~20× stiffer; about a third of it is the reference satellite's own wander, the rest
   per-satellite.  ⚠ The mechanism is NOT guessed into the record (candidates, unranked: a cycle-domain model
   term; an estimator constraint holding N₁−N₂ stiff on this posture, e.g. the armed EWL/UWL lanes; a trace
   artifact).  One structural DEDUCTION from the slope alone (not a mechanism claim): a wander that is 1:1 in
   CYCLES across two bands cannot originate in a metric quantity — range-like and iono errors each impose a
   wavelength ratio (0.779 / 1.283) and neither is measured — so it lives in a channel COUNTED IN CYCLES, i.e.
   the ambiguity/level side of the estimator or a cycle-domain model term.  The discriminators are already in
   the ladder: the leveling arms carry no lanes and no invariant carry, and the sim arms' slopes say what the
   estimator does without field effects (F3 1.271 = iono-shaped; diagonal carry: noise).
5. **Independent leg — the estimated slant-iono states of the same chords** (`scripts/iono_arc_wander.py`,
   `network_ionosphere.csv` `iono_total_m` = x_lin + x_r, the full state, so the x_r-only frame caveat does not
   apply).  Field control, settled ≥ h6, same arc rules: single-LOS iono_total_m arc MAD p50 **3.9 m** (p90 13.3),
   drift 8.7 m (p90 29.7); the DD of the iono states over the DD trace's chords (646 chords, 584 arcs) MAD p50
   **1.25 m = 6.6 L1 cycles** (p90 5.4 m), drift 3.5 m = 18 cyc.  So the iono states wander by metres per arc
   too.  Joint per-arc regression of the DD-ambiguity deviation e1 (cycles) on the DD-iono deviation (L1 cycles):
   slope p50 **+1.72** (p10 +0.32, p90 +2.50), corr **+0.93**.  The two wanders are COHERENT and co-directed —
   not the compensation an iono-state error would produce (slope −1 at L1) and not independence (slope 0) —
   yet the ambiguity part is cycle-common across bands (item 3), which the model's iono direction is not.  So
   the (I, N₁, N₂) triple of a field chord moves together along a direction that is NOT the estimator's iono
   direction; recorded as a measurement, mechanism still not guessed.  (Real ionospheric change on DOY-129 is
   metres per hour at low elevation, so the iono states' own wander is not by itself anomalous; the coherent
   cycle-common ambiguity motion riding on it is the anomaly.)
   Sharper form (peer review): if the ambiguities were absorbing an iono-state error δI, the cross-band ratio
   would be δN₂/δN₁ = (f₁²/f₂²)(λ₁/λ₂) = f₁/f₂ = 1.283 — convention-free, identical to the iono fingerprint
   — and 1.004 was measured; so the +0.93 coherence is NOT the ambiguity eating an iono-state error.  The +1.72
   is regressed on the raw per-band e₁, which the wind-up card warns DILUTES a cycle-domain quantity (§13.8.6:
   regress the extracted A).  **Pre-registered before running (18:00):** decompose each chord-epoch exactly
   into e_i = A + B·(λ_i/λ₁) (two bands ⇒ B = (e₂−e₁)/0.2833 = −WL/0.2833, A = e₁ − B) and regress the arc
   deviations of A and of B separately on the DD-iono deviation (L1 cycles).  Prediction: B carries whatever
   iono relation exists (|slope| of order 1, the sign fixing the convention) and A does not (slope ≈ 0); an A
   that still regresses near +1 on the iono deviation is the finding, since a cycle-common channel tracking
   the iono states 1:1 is nothing the measurement model can produce.  Same test on the sim F3 arm as the
   semantic control.  ⚠ Wind-up is the one physical term that is band-common in cycles (ΔWL = 0, MW-blind) and
   it is ALREADY FALSIFIED as the band-common DD error at 0.08 % of the variance, 144× too small
   (SPEC_synthetic §13.8.6, 2026-09-08) — do not re-derive it; the open question is what else is
   cycle-common at a TEN-cycle amplitude, two orders above wind-up.
   **Result (18:05, field control, 372 paired arcs):** A (cycle-common) on the DD-iono deviation: slope p50
   **+1.68** (p10 +0.26, p90 +2.68), corr **+0.90**; B (iono-shaped): slope **+0.09** (p10 −0.14, p90 +0.42),
   corr +0.33.  The pre-registered "finding" branch: the cycle-common channel co-moves with the iono states
   ~1.7:1 and the iono-shaped channel does not.  **Semantic control, sim F3 arm** (`nonint_losblock24h_hv`,
   6,893 arcs): its DD iono states wander by a comparable amount — MAD p50 0.88 m = 4.6 L1 cyc per arc (the
   simulated ionosphere's real change; single-LOS 1.5 m) — and the joint regression of e₁ on the DD-iono
   deviation reads slope **+0.001** (p10 −0.055, p90 +0.111), corr +0.075: on a healthy arm the iono state
   tracks the real ionosphere and the ambiguity does not move with it.  So the field's iono-state wander is
   physical-scale and not itself the anomaly; the anomaly is the ambiguities' cycle-common co-motion with it.
   **How this is argued (peer review, 18:10, before the sim A/B split was read):** (i) A ≈ e₁ by algebra —
   with e₁ = A + B and e₂ = A + 1.283·B, B = −3.53·WL, so with WL MAD 0.57 the B channel holds only ~2 cyc
   against e₁'s 10.4 and A's +1.68 restates the raw +1.72, not a second measurement.  (ii) The load-bearing
   statement is a MAGNITUDE one, convention-free: an ambiguity absorbing the iono-state wander does so
   iono-shaped by construction, i.e. entirely in B, so a 6.6-cycle DD-iono wander would put ~6.6 cyc of MAD into
   B; B carries ~2 — 3.3× too small — and its regression on the iono deviation is +0.09.  (iii) B is
   noise-amplified (a 0.999-correlated difference × 3.53), so "B ≈ 0" needs a power check.  ⚠ The state-total
   regression is NOT that check: on the sim F3 arm the ambiguity error is iono-shaped (cross-band 1.262) yet e₁
   does not follow the iono STATE's total (slope +0.001), because the state total is dominated by the real
   ionosphere the state tracks and the absorbed quantity is the state's ERROR — so the pending sim A/B split is
   pre-registered to read A ≈ 0 AND B ≈ 0 on the state total, and that is expected, not a failure of B.  The
   power check that exists is the cross-band STRUCTURE itself: on the sim F3 arm the error is B-dominant
   (§13.8.6: med|B| 0.206 vs med|A| 0.108, ratio ≈ 2) — the B channel demonstrably carries an iono-shaped error
   when one exists — while the field control is A-dominant (10.4 vs ~2, ratio ≈ 0.2).  A truth-iono join
   (regress B on the state ERROR, sim only) would make the positive control explicit; not run here.
   **Sim A/B split (read 18:03, as pre-registered):** A slope −0.000 (corr −0.005), B +0.002 (corr +0.074) on
   the state total — both ≈ 0, expected.  **Truth-join positive control (regress B on the sim state ERROR):
   OWED**, and it is the leg that decides the load-bearing reading — the field's +1.68 is a regression on the
   iono state TOTAL, which on a healthy arm is dominated by the real ionosphere the state tracks, so it says
   "the cycle-common ambiguity channel co-moves with the estimated DD ionosphere", NOT "with the state's
   error".  **Truth-free discriminator, baseline length** (real DD ionosphere grows with station separation;
   `iono_arc_wander.py` split, field control): DD-iono MAD p50 0.74 m (< 500 km, 63 arcs) → 0.92 (500–1500,
   16) → 1.56 (1500–4000, 88) → 1.57 m (> 4000 km, 205) — it grows like real ionosphere; A's MAD grows with it
   (8.8 → 7.1 → 11.2 → 11.3 cyc) and the A-on-iono slope stays flat (+1.87 / +1.93 / +1.66 / +1.51).  So the
   cycle-common ambiguity channel follows the ESTIMATED DD ionosphere proportionally at every baseline
   length, i.e. it follows what the state tracks — the real ionosphere — not a baseline-independent error.
   **Trace semantics verified in source:** `network_dd_fractional.csv` forms dd_m from the plain ambiguity
   states, `x_lin + x_r` per (station, sat, signal) in metres, differenced between satellites then stations
   (`NetworkDdFrac.cpp:143-158, 245-248`); `iono_total_m` is likewise the plain per-LOS iono state
   `x_lin + x_r` (`NetworkIonoDiag.cpp:98-110`).  Neither is a derived quantity, so the (I, N₁, N₂) co-motion
   is a motion of the ESTIMATED STATES.  ⚠ Consistency question this raises, to be read before any
   mechanism: in metres the co-motion is ΔN₁ ≈ +1.7 ΔI, ΔN₂ ≈ +2.2 ΔI against phase iono terms of −ΔI / −1.65 ΔI,
   leaving ~+0.6 ΔI of range-like model change on BOTH bands that no DD of clocks / geometry can absorb — so
   either the phase post-fit σ̂₀ of the control carries it (check `network_epoch_status.csv`), or a fourth
   per-LOS term of the field posture (the SH↔slant pin with its GM deviation is the candidate) closes the
   budget and the co-motion is a legal, weakly-observed direction of THAT model.  Not resolved here.
   **Read (18:05, the 09-02 control):** the model FITS — `network_epoch_status.csv` settled `sigma0_pf_phase`
   median 0.0105 m (p90 0.012; `sigma0_pf_code` 0 = the code pool is empty on this vehicle, `sigma0_global`
   3.1), and `network_pf_keyed.csv` per-station phase rms p50 1.6 cm (p90 5.4, p99 13 cm; max p50 4 cm) over
   703k settled phase rows.  A 0.75 m range-like misfit per cycle of iono change is nowhere in the residuals,
   **and that FORKS the reading rather than settling it** (peer review, 18:10): the unexplained band-common
   range is ~0.6·ΔI ≈ **0.75 m per arc** (ΔI MAD 1.25 m), and nothing in a double difference is that size —
   receiver and satellite clocks cancel EXACTLY, DD troposphere is centimetres (2 cm ZWD through a 3.8 mapping
   across four terms ≲ 15 cm), DD orbit error centimetres, and a 0.75 m SH↔slant pin deviation would be
   conspicuous elsewhere.  Attenuation makes it worse, not better: the regression is on the iono STATE, which
   carries its own error, so errors-in-variables bias the slope toward zero and +1.7 is a LOWER bound on the
   structural coefficient.  So with the fit normal, no physical term closes the budget, and the two readings
   left are: (a) the regression slope is not a structural coefficient of the phase model at all (corr 0.90 ⇒
   ~19 % of A's variance is not ΔI; the budget line assumes all of A is 1.7·ΔI), or (b) the traced states do
   not enter the phase equation with the assumed coefficients/frame — the TRACE-ARTIFACT branch is alive, not
   dead: the DD trace and the iono trace are both `x_lin + x_r`, but whether that sum is the solve's state at
   the SAME stage of the epoch (before/after re-linearization, harvest, relevel) is not verified.  The
   definitive check is direct: reconstruct the phase residual of a few chords from the TRACED I and N and the
   run's own model terms and compare with the run's post-fit residual (cm) — metres of reconstructed residual
   ⇒ (b).  OWED, with the truth-join.  Until then the field co-motion is a measurement about the TRACES, and
   "the DD ambiguity is not estimable-invariant on this vehicle" is a candidate reading, not a finding.
   **(b) as stated — a cross-sweep STAGE mismatch — is ELIMINATED by a source read (peer, 18:15):** the two
   sweeps are back-to-back in the driver (`networkIonoDiag` at `ApplicationNetwork.cpp:2127`, `networkDdFrac`
   at `:2131`, both after `networkSolveGlue` `:2041` / `networkInstallCarry` `:2075` and before
   `networkLeveling` `:2154` / `networkPostFitRaim` `:2165`, same `est`/`x_s`/`plan`/`asm_res`), and both form
   `x_lin + x_r` identically (`getStateVector()` + `asm_res[i].h2local`).  The traces are consistent with each
   other; what is unverified is only that this `x_lin + x_r` is what the phase rows were fit with (a
   re-linearization/relevel between the fit and the sweep would move both traces together).  Weight moves to
   (a).  Reconstruction scoping: per chord-epoch the four LOS iono totals and four ambiguity totals form the
   modelled band-1/band-2 phase to compare with the run's post-fit ROW residual — but on the ci arm the per-row
   phase residual stream rides the inert RAIM pool (`trace_station_residuals` writes 0 rows on carry_invariant;
   `network_pf_keyed.csv` is per-key aggregates), so the check runs fully only on the pl arm (pool live), i.e.
   it validates the traces on a different vehicle than the one the 0.75 m was measured on; on ci it needs a
   per-row residual dump first.
   **The last unverified step is closed too (peer source read, 18:12, two tools):** the only writer of a
   station's state vector on the network path is `NetworkIonoDwRelease.cpp:438`, invoked at
   `ApplicationNetwork.cpp:2150` — AFTER both sweeps — and the four seams between install-carry and the sweeps
   (producerAr, solPrep, emitStore, fastClockHarvest) contain no mutating `sm->` call; `NetworkInstallCarry.cpp`
   `:422-423` / `:460-461` form absolute WL values with the same `x_lin + x_r` idiom (the wu93 lane observatory was
   validated on it).  **And NO coefficient closes the budget, so (a) does not rescue it either:** zero phase
   residual needs A = 1.000·ΔI on band 1 but 1.2833·A = 1.6469·ΔI ⇒ A = 1.283·ΔI on band 2 — a cycle-common
   channel scales 1.283 between the bands, an iono change 1.647, so a cycle-common channel STRUCTURALLY cannot
   absorb an iono deviation; the best-balanced A still leaves ±0.18·ΔI ≈ 0.22 m band-DIFFERENTIAL residual
   against 1.6 cm measured (A = 1.7 leaves +0.70/+0.54·ΔI).  Attenuation cannot produce an over-estimate.
   **Reconstruction outcome PRE-REGISTERED (18:13, before it runs and before the leveling arms were read):**
   if the traced `x_lin + x_r` pairs are what the phase rows were fit with, the reconstruction MUST show
   decimetre-to-metre residuals the run does not have; either that gap appears (the estimability reading gets
   its evidence, and the run's residual bookkeeping is then the question) or it does not — and then the traced
   pairs are NOT the fitted quantities despite clean staging, which sends the search to the DD CONSTRUCTION
   itself (column mapping `asm_res[i].h2local`, state units cycles-vs-metres, per-station re-estimation of
   ambiguities under the invariant carry making `x_r` large on the field where the sim's frozen carries keep
   it near zero).  Both outcomes are informative; the assumption both the budget and the check share is that
   the DD of states maps linearly onto the DD of modelled phase with no other state moving.
   **Five-minute check of the mis-pairing candidate on data already on disk (18:16):** `network_ionosphere.csv`
   emits `x_lin_m` and `iono_m` (= x_r) separately.  On BOTH beds the iono `x_lin` is a constant seed within
   every arc (arc MAD 0.000) and x_r carries the entire time variation — field control |x_r| p50 11.0 m, arc
   MAD 3.9 m; sim F3 arm |x_r| p50 5.9 m, arc MAD 1.5 m.  So the premise "x_r ≈ 0 on the sim" is FALSE for
   the iono state: the sim's x_r wanders metres per arc, and the sim's DD-ambiguity trace nevertheless reads
   clean (MAD 0.15 cyc).  An ambiguity↔iono column mis-pairing in the DD trace is therefore FALSIFIED as a
   universal bug (it would print the sim's 1.5 m iono x_r into the sim's ambiguities); what survives of that
   candidate is only a vehicle-specific mapping difference (carry_invariant field vs leveling sim), which the
   leveling arms test directly.  ⚠ Correction (peer): this check measured the IONO column's x_r, not the
   AMBIGUITY column's — the original reason to expect a small sim x_r was frozen CARRIES, which govern
   ambiguities — so the mis-pairing is NARROWED, not killed; the DD-trace column split stays the decisive test.  The ambiguity states' own x_lin/x_r split is not traced (only the sum) — the
   one-line `x_lin_m`/`x_r_m` column addition to `NetworkDdFrac.cpp:318-327` (trace-only, byte-identical) is
   the follow-up instrument for the reconstruction.
4. **What the field's "uniform 0.25" now means.**  A DD estimate that wanders tens of cycles within an arc
   wraps to uniform trivially, and no leveling-carry fix addresses WANDER of that scale (the over-count freezes
   a WRONG but STABLE level).  P2's success branch therefore requires arm 2 to ALSO read stable on this
   instrument (per-band MAD ≲ 0.3 cyc, as the sim F3 arm does); an arm 2 that reads at the fraction null but
   with per-band MAD of cycles is a WANDER verdict (the field's DD problem is dynamic, upstream of the
   ambiguity/iono split), not an over-count verdict.

