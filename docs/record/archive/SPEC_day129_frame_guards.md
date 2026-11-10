<!-- FROZEN / READ-ONLY: the pre-split monolith, kept byte-for-byte. Never append here — new results go in the card file (see docs/INDEX.md). -->
# SPEC — DOY-129 frame guards & trigger instrumentation (Opus card)

> ### ⚠ HEADING DISCIPLINE — read before editing
> **This file must never contain a heading numbered `§4.3` or `§6`.** Three code comments
> cite anchors that were always targets in *other* files:
> `include/config/GnssConfigTypes.hpp:1454` and `include/logging/NetworkRunTraces.hpp:517`
> cite "SPEC_day129_frame_guards §4.3" (really `REPORT_day129_frame_guards_phase01.md` §4.3,
> the Phase-1 verdict), and `src/core/app/ApplicationConsumerReplay.cpp:404` cites
> "SPEC_day129_frame_guards §6" (really `HANDOFF_night305_day129.md` §6, the
> `start_offset_hours` residue gotcha — now the **Appendix**). A bare `§4.3` or `§6` here
> would silently give those comments a WRONG target. All sections are **Part-lettered**
> (`Part A`…`Part F`, `A1`, `D-G1`, …) or carry a **frozen LABEL** (`Phase 0`, `Phase 1`,
> `P1-1`…`P1-5`, `Phase 2.1`, `Phase 2.2`, `D1`/`D2`/`D3`) — ~30 source sites cite those
> labels by name. **Never renumber, retitle or reorder them.**

**What this file is.** The single surviving record of the DOY-129 frame-guard lineage. It
absorbed, 2026-08-15: `docs/REPORT_night305_day129_investigation.md` → **Part A** (the
disease, measured); `docs/SPEC_day129_gate_policy.md` → **Parts C–F** (what the guarded leg
measured, the six gate-policy rulings, the validation gates, the open items). Two lineage
members **no longer exist anywhere** and this file is their only trace:
`docs/HANDOFF_night305_day129.md` (its §3 total-IF grading methodology, §5 resume arithmetic
and §7 arm-together list survive only as restated here and in the Appendix) and
`docs/REPORT_day129_frame_guards_phase01.md` (the Phase-1 arming pass that built the SH gate
and launched the chain). `docs/REPORT_warm_resume_event.md`, cited by the absorbed gate policy
as the estimation-side companion, also does not exist; the live companion is
`docs/SPEC_warm_resume_event_card.md`.

**Run artifacts.** `runs/day129_cont/seg01/` (the 2160-epoch reference day),
`runs/day129_guards/{seed_immutable,leg01}/` (the guarded chain; run under the renamed exe
`gnss_processor_p2chain.exe`, per the concurrent-build-lock directive), `runs/bytecheck/`,
`val_out/night305_*`, `leg.json`. ⚠ **`runs/` was archived off `C:` on 2026-08-12 to
`D:\GnssProcessor_archive\2026-08-12\runs\`** — every `runs/<name>` path here resolves there.

**Reading order.** Part A is the *disease*; the card body (Phase 0/1/2 — **Part B**) is the
*response*; Parts C–F are what happened when the response was armed.

**Mechanism being guarded.** The producer's between-satellite frame has no guarded stiff
anchor: phase memory leaks 13.5 %/epoch (D5 re-taps), satellite clocks are effectively white
(q_clock:q_cb = 10⁸), and the only stiff anchors are the near-static CB states
(q_cb = 1e-10 m²/s) and the two hard pins (pivot sat clock G01, datum receiver MBAR00UGA).
Common-mode (one-sided) excursions ratchet into the pivot's CB at gain K ≈ 2.2e-3/ep and bleed
at τ ≈ 3.8 h; the SH-VTEC collapse is a follower symptom; the emitted disc counters carry no
magnitude integrity. Guard the CB carry (invariant #1), instrument the trigger, measure the
consumer impact.

---

# Part A — the disease, measured

*Absorbed from `docs/REPORT_night305_day129_investigation.md` (now `docs/attic/`). It answered
the handoff's Q1–Q4 **from existing artifacts plus code tracing — NO re-run was needed** (every
number rebuilt from `runs/day129_cont/seg01/{logs,products.txt}` + `leg.json`). **[M]** =
measured; **[H]** = hypothesis consistent with all evidence but needing the Phase-1 re-run.*

**One-line synthesis: the four findings are ONE disease.** Phase memory is porous (13.5 % of
carried DDs re-tapped fresh *every* epoch [M]), satellite clocks are effectively per-epoch
(q ratio clock:CB = 10⁸), and the only stiff cross-epoch anchors are the near-static code-bias
states and the two hard datum pins. In quiet hours the frame wanders metres/hour on the clocks
(A4). Under common-mode excitation the system slides along a near-null global mode that only
the pins resist; what lands on the pinned satellite ratchets into its near-static CB states and
bleeds out on their Kalman time constant (A2, τ = 3.79 h *derived from q*). The SH field is a
follower with weak smooth-mode observability **by design** (A3). The emitted disc counters
signal datum bookkeeping, not faults (A5).

## A1 Configuration facts the mechanisms hang on

From `runs/day129_cont/seg01/leg.json` [M]:

| knob | value | consequence |
|---|---|---|
| `filter.process_noise.sat_code_bias_q` | **1e-10 m²/s** (3e-9 m²/ep) | CB states near-static → K ≈ 2.2e-3/ep gain |
| `filter.process_noise.sat_clock_q` | **1e-2 m²/s** (0.3 m²/ep) | clocks effectively white; **10⁸ softer than CB** |
| `filter.process_noise.sat_phase_bias_q` | 1e-12 m²/s | PB stiffest — moved ≤ 4 mm at the event [M] |
| `network.code_datum_time_linked` | true | IF-pin + s2z rows carry previous values (Card 2) |
| `network.cb_kaula_prior` | absent → **false** (`GnssConfigLoadNetwork.cpp:2046`) | no zero-pull on CBs; decay must come from data |
| clock pivot | auto-lexical (no config pin) | G01 when present, else G02 — matches 2051/109 epochs [M] |
| `estimation.global_vtec_sh` | deg 15, Kaula 10/n TECU, a00 15±30, q 0.01 TECU²/s, rep-σ 5 TECU | |
| `estimation.ionosphere_weighting` | σ_c = **50 m** common-mode, floor 0.05 m, elev 25° | SH sees only between-sat structure |
| `network.iono_constraint_deweight_*` | adaptive, 3σ↑/2σ↓, **floor 0.0**, rate 0.25 | w_s ≤ 0 drops a station's whole SH block |

Satellite-clock datum = the hard pin `clock(G01) = 0` (`NetworkSbasis.cpp:184-185`); code-bias
datum = one IF pin per satellite plus per-(constellation,signal) sum-to-zero, folded at
`kCodeBiasGaugeWeight = 1e3` (`NetworkSbasis.hpp:226`, `NetworkSolveGlue.cpp:204`).

## A2 Q2 — why the pinned pivot's code bias absorbed the damage. ANSWERED

### A2.1 What actually stepped at tow 334020 [M]

Product records 333990 → 334020 (`products.txt`):

| quantity | step |
|---|---|
| G01 CB C1C / C1W / C2L / C2W / C2X | +9.66 / **+30.53** / +16.52 / **+18.54** / +15.12 m |
| G01 IF(C1C,C2L) — the **datum-pinned** combination | **−0.95 m** (held) |
| G01 IF(C1W,C2W) — the **graded / consumer** combination | **+49.06 m** |
| G01 phase biases (all 5 signals) | ≤ 4 mm |
| all 31 non-pivot clocks | −4175 … −5636 m (transient; G01 pinned 0) |
| per-channel ΣCB over all sats | held to < 0.25 m on every channel |
| each non-pivot sat per channel | −Δ_G01/31 exactly: predicted −0.985 / −0.598 / −0.312, measured −0.978 / −0.598 / −0.319 (C1W/C2W/C1C) |

The Card-2 time-linked datum **held everything it actually pins** (pinned-pair IF and channel
sums) — and the sum-to-zero carry made the *other 31 satellites pay* for G01's step (~0.3–1.0 m
each, uniform per channel, invisible to pairwise spans). The +49 m consumer-visible damage lives
entirely in combinations the datum does **not** pin, i.e. data-determined directions.

### A2.2 The mechanism

At the excursion epoch the solve moves along the cheap global common mode {all satellite clocks
−δ, all receiver clocks −δ}: phase and code residuals are invariant along it — **except at the
two hard pins** (G01 satellite clock, MBAR00UGA receiver clock), which cannot move. **The pin
converts G01's clock↔code split from a GAUGE direction into a MEASURED quantity**, so the
excursion pressure every other satellite dumps into its soft clock (q = 0.3 m²/ep, recovers in
~2–4 epochs) lands on G01's **near-static** CB states instead. This *derives* the discriminator
the handoff had only observed: **damage ∝ the excursion's common-mode (ONE-SIDED) projection.**
The two-sided morning event (common/diff = **0.05**) had almost no projection onto the pins and
cost nothing; the one-sided evening event (ratio **5.4**) ratcheted the pivot.

### A2.3 The arithmetic closes on both sides [M]

Steady-state Kalman gain of a CB state: **K = √(q/R_eff)**.

- **Decay:** observed τ = **3.79 h = 455 ep** ⇒ K = **2.2e-3** ⇒ R_eff = **6.2e-4 m²** ⇒
  σ_eff ≈ **25 mm/epoch** of network-wide code information on one satellite's bias ≈
  0.3 m/√(≈250 stations). Self-consistent with the configured q = 3e-9 m²/ep **to ~15 %**.
- **Injection:** K × (~4600 m clock-space excursion) ≈ **10 m/epoch** × the few out-of-band
  epochs ≈ the observed **30–49 m** step.

The persistence is **NOT** the Card-2 datum rows (they held); it is the CB states' own
near-static random walk with **no jump/event response** — a standing-invariant-#1 violation of
exactly the Card-4-slip-hole class, one level down: the C3c/D5 machinery guards the *ambiguity*
carry; **nothing guards the *code-bias* carry**. The `CarryPrior.inflate_q` reinit machinery
already exists for clock-monitor-flagged satellites (`NetworkRunTypes.hpp:108`,
`ApplicationNetworkAssembly.cpp:1247-1257`) but `clock_runaway_check` was off. **Recurrence:
GUARANTEED** for any one-sided excursion while (pivot hard pin + near-static CB + no event
response) coexist. Refined hypothesis: not "sole free parameter" but **"the only satellite whose
split is data-exposed *and* stiff-carried"** (memory `project_day129_pivot_cb_ratchet`).

## A3 Q1 — the a00 collapse. Anatomy established; trigger needs one trace

### A3.1 What the artifacts rule in and out [M]

- **No precursor** — `slant_resid_{mean,rms,max}`, `n_los_gt3sigma`, `n_stations_gt3sigma` are
  exactly baseline at **t−1 for all three events**.
- **The three events are DIFFERENT directions** in coefficient space (cosines
  **−0.05 / −0.48 / +0.04**) — *not* one recurring null mode. Power sits at degrees 0–3, and
  **n=1,m=1 out-steps a00 in two of three events** (**+2018 TECU vs +1236** at 07:32) — the
  a00-only band check watches the **wrong scalar**, even before its 23.8× edge-trigger under-count.
- **Formal σ is FROZEN**: `a00_std` 0.622 → 0.623 TECU across a **+1236 TECU** step
  (all-coefficient mean 0.15 TECU) — a **~1500–2000σ** step against the folded prior. The prior
  injection (`networkInjectShVtecPrior`, `NetworkRunContext.cpp:800-901`) is healthy (Kaula +
  RW-toward-previous, always on), and a weighted solve cannot move 2000σ against its own prior
  unless the *data side* moves — the SH rows' innovation is literally `−x(iono_idx)`
  (`ApplicationNetworkAssembly.cpp:374`). **The field follows the per-LOS iono states: the SH
  collapse is a FOLLOWER, not the disease.**
- **Smooth-mode observability is weak BY CONSTRUCTION**: per-constellation
  `R = diag(var) + (50 m)²·11ᵀ` deliberately kills each station's common mode (receiver-DCB
  freedom), so a00-like modes are seen only through the mapping-function *spread*
  (`ApplicationNetworkAssembly.cpp:304-308` says exactly this).
- **Chronic saturation:** **69 % of LOS exceed 3σ** against the SH constraint in the *quietest*
  hours (expected 70–90 %, `NetworkIonoDeweightGate.hpp:21-23`); slant-residual mean ≈ 10 m all day.
- **Deweight gate:** very active — **174 stations ever, peak 72 simultaneous, 326 engage / 307
  release** (`main_log.log:7539`) — but network-relative (median-centred ⇒ **structurally blind to
  common-mode excursions**) and **NOT the proximate trigger at onset** (formal σ unchanged ⇒ weight
  pattern unchanged that epoch). With floor 0.0 an engaged station's whole SH block drops
  (`ApplicationNetworkAssembly.cpp:330-331`) and its per-LOS iono floats.

### A3.2 The enabler, and the trigger hypothesis [H]

The phase frame that should forbid a joint clock/iono/field slide is **porous**: **2,421,756 D5
innovation re-taps over the run = 1,121/epoch ≈ 13.5 % of the ~8.3 k carried DDs re-tapped fresh
EVERY epoch**, with **LS-VCE σ̂₀ = 4.74** (`main_log.log` run-end) [M]. Working hypothesis for the
single-epoch onset: the re-tap/release pattern (a D5 avalanche and/or a deweight release wave
re-coupling stations whose iono floated for hours) momentarily releases the weakly-resisted joint
mode; the solve slides until secondary stiffness catches it; sign and mixing differ per event
(hence the uncorrelated step directions). **Untestable from this run's artifacts** — per-epoch
re-tap count, deweight state and SH spectrum are not traced. That is what Phase 1 adds.

## A4 Q3 — the quiet-hour wander IS satellite-clock wander. ANSWERED

Hour-to-hour delta of per-sat deviation from the constellation median, quiet hours 06–18 [M]:

| component | median \|Δ\|/hr | median hr-pair span |
|---|---|---|
| total IF (CLK + IF_W(CB)) | 2.68 m | 16.1 m (handoff's ~18 m ✓) |
| **clock alone** | **2.83 m** | **16.5 m** |
| IF(C1W,C2W) of CB | 0.18 m | 3.2 m |
| IF(C1C,C2L) of CB — the pinned pair | 0.009 m | 0.14 m |

**The wander IS the clocks.** G01 is among the *steadiest* satellites (**1.98 m/hr** vs 5.6 max)
because its total-IF rides the stiff CB carry. The 0.18 m/hr on the consumer pair's CB-IF is the
slow integral of the same frame wander leaking into the near-static CB states at gain K
(order-consistent with A2.3). **Structural reading: nothing carries absolute per-sat clock
information across epochs** — the emitted clock is re-derived every epoch against a frame whose
only stiffness is the CB carry plus a phase graph that leaks 13.5 %/ep. The fix is not tuning
code weights; it is "give the clock product a guarded temporal anchor" (clock model / C1
clock-rate product with event response), or accept the wander and emit honest URA.

## A5 Q4 — the disc counter is a DATUM channel, not a fault channel. ANSWERED

From `network_solution.csv` counter diffs [M]: **5,126 CB bumps concentrated in 128 epochs**,
arriving as **mass waves of ~150–204 keys** (a channel-membership change bumps *every* satellite
in that channel, `NetworkProductEmitter.cpp:345-349`); the two pivot handovers (tow **319260**,
**322530** — the handoff's two `datum_changed` epochs) contribute **402 (7.8 %)**. **CLK bumps:
0 all day** (`emit_clock_disc` off). **PB bumps: 3,711.** Triggers in code — forced datum
change, per-channel set change, per-sat band-tuple change, Card-2 reseed — are all
**structural**; the only magnitude-sensitive trigger is `value_jump`
(`NetworkProductEmitter.cpp:336`), gated by `code_bias_disc_jump_test = false`. **At 334020 G01's
CB bumps = 0** [M]. **Conclusion:** the emitted stream carries **NO usable magnitude integrity on
B lines**, and worse, a counter-following consumer would reset ~200 satellites' CB state ~128
times per day for bookkeeping reasons **while integrating straight across the one real 49 m
fault**.

## A6 The datum-design finding — the IF pin is on the wrong pair (D3)

`selectIfPinPair` sorts by (band digit, name), so with full signal sets the pinned pair is
**(C1C, C2L)** — ***not*** the **(C1W, C2W)** pair the grading formula and RTKLIB-convention
consumers use. The stable combination is therefore not the one consumers apply. Re-pointing the
pin at the consumer pair puts the carry's stability where users look, but it **is a datum
re-reference**: fingerprint-breaking, needs a fresh convergence, touches every downstream
comparison. Recorded as decision **D3** and deferred.

## A7 The investigation's delta over the handoff — now Part B

The five traces it asked for are Part B's **P1-1…P1-5** verbatim. Two facts that live nowhere
else: `n_adapt` in `network_epoch_status.csv` is a **different** counter from the deweight state
and reads **0 all day** [M]; and the `sigma0` emit column reads **0.0000 on all 2160 rows** [M]
because **no post-fit residual statistic exists on that path at all**.

Three recommendations, all folded into Part B: **wire an event response for the CB carry**
(invariant #1 — the existing `inflate_q` reinit path extended to the pivot's CB under a
clock-monitor flag is the minimal shape; `code_bias_disc_jump_test` alone only *signals*, and
only helps a consumer that resets, at ~0.6 m/√ep re-tracking cost); **arm the σ̂0/A4 ceiling** so
a −4500 m clock epoch cannot ship `gate_ok = 1`; and **rework the health gate to band-check the
low-degree coefficient VECTOR** (or total-field RMS), not a00 alone (A3.1) — counting epochs,
gating rather than warning.

Predicted signature for the Q5 replay, from A2: **a consumer using C1W/C2W sees the +43 m era as
a G01-specific pseudorange fault its RAIM should exclude, while a C1C/C2L consumer sees almost
nothing.** That delta is worth one replay each — Phase 0.

---

# Part B — the guard card

*The card body, unchanged. **Every label below is frozen** — ~30 source sites cite `Phase 0` /
`Phase 1` / `P1-1`…`P1-5` / `Phase 2.1` / `Phase 2.2` / `D1`/`D2`/`D3` by name.*

## Phase 0 — Q5 closed-loop replay (no new code, run-only, ~minutes/arm)

Replay `runs/day129_cont/seg01/products.txt` through the closed-loop consumer
(`NetworkProductReader::replay`; config parse `GnssConfigLoadNetwork.cpp:3218-3262`;
`realtime.network_products_path` + `processing.network_consumer.enable`; `closed_loop` is NOT in
the snapshot fingerprint — no producer implications). Consumer station: any with DOY-129 obs
(`data/2024-129/obs`, 311 available); prefer one NOT in the 305 roster if convenient.

Arms (full-day each, grade `pos_err_3d_m,east_m,north_m,up_m` from the consumer trace,
`NetworkRunTraces.cpp:115-116`): **A** = consumer using C1W/C2W pseudoranges (convention
default); **B** = identical but C1C/C2L.

**Predicted signature (falsifiable):** arm A sees the 20:47→EOD era as a G01-specific
pseudorange fault (tens of m on one PRN, decaying τ≈3.8 h); arm B sees almost nothing. Also
record both arms across the 07:32 event (expect little damage in either — transient only).
Report **hourly position-error bands** (no slopes) — the handoff's grading methodology, restated
in the Appendix. Cadence: smoke the replay on a 10-epoch window first (standing directive), then
full day. Release exe only, renamed copy (`gnss_processor_opus_run.exe`).

## Phase 1 — trigger instrumentation (code; trace-only; fingerprint-SAFE; then one 3–4 h leg)

All items are trace/diagnostic: **byte-identical when their toggle is off** (invariant #6), and
every toggle is fingerprint-SAFE (verify each against `NetworkRunContext.cpp:172-307` before
running — nothing here may enter the fingerprint). Trace-stream ownership stays in
`logging/NetworkRunTraces`. Appended CSV columns only — positions preserved (T2d precedent).

1. **P1-1 — per-epoch carry-churn counters** → append `n_d5_flags, n_d5_retaps, n_chord_births,
   n_slip_refloats` to `network_epoch_status.csv` (per-epoch values of the run-end totals at
   `main_log` Card-4 lines), from the carry-driver epoch counters already accumulated in
   `ctx.carry` / driver returns (`NetworkInvariantCarryDriver`, `NetworkInstallCarry.cpp`).
2. **P1-2 — per-epoch iono-deweight state** → append `n_dw_engaged, n_dw_zero, dw_min_w,
   dw_mean_w` (the gate exposes per-station state; add cheap aggregate accessors) AND emit
   engage/release transitions as `IONO_DW` events (station, z, w). Serial harvest site only
   (threading contract in the gate header).
3. **P1-3 — SH estimability spectrum** → per-epoch λ_min/λ_max (+ smallest-mode participation
   vector at a configurable stride) of the SH block via
   `NetworkEstimator::dumpEstimabilitySpectrum`, behind `network.trace_sh_spectrum` (new, default
   false, fingerprint-SAFE). New diag seam or `NetworkShHealth` — NOT the driver.
4. **P1-4 — emit-path post-fit σ0** → the `sigma0` column is 0.0000 on all 2160 rows of the
   reference run: find why the statistic is not computed on this path and make it computed +
   traced (compute-only; the A4 ceiling stays put — behavior change belongs to Phase 2).
5. **P1-5** — `network.trace_dd_fractional: true` for the leg (existing knob, known SAFE; closes
   handoff Q6 measurability).

**Run:** warm-resume from `val_out/night305_conv_immutable.txt` (copy it to a leg-owned snapshot
path — never point `state_snapshot_path` at the immutable; it is read AND written),
`start_offset_hours` per the resume arithmetic in the **Appendix** (DAY_ANCHOR for DOY-129;
assert `snapshot age 30.0 s` in the log), duration ≈ 3.5 h GPS time to cover the 07:32:00
collapse (tow 286320) with full traces. Wall cost ≈ 4 h at 41 s/ep. 10-ep smoke first. Do NOT
try to reproduce the 20:47 event in Phase 1 (needs a ~15 h leg; the 07:32 event answers the
trigger question — Q1 needs *any* onset under instrumentation).

**Deliverable:** the trigger verdict — at tow 286320 (and the ramp before it): does λ_min crater?
does the re-tap count spike? did a deweight release wave land? Any one confirmed ⇒ Q1 closed; all
quiet ⇒ report that honestly (counters, not absence of crashes).

## Phase 2 — guards (code; default-OFF; needs a FRESH convergence to arm) — GATED

Do not start Phase 2 until Phase 0/1 results are in AND the user has ruled on D1/D2 below.
Fingerprinted knobs (`code_bias_disc_jump_test`, `emit_ura`, `phase_leveling`) require a fresh
6 h seed + re-run to arm: budget ≈ 8 h seed + whatever leg length the validation needs.

1. **Phase 2.1 — CB carry event response (the invariant-#1 fix; the core of this card).**
   Recommended shape (decision D1): a network-wide common-mode excursion detector — per-epoch
   median |Δclock| across satellites against a robust threshold (the excursion epochs sit 3–4
   orders of magnitude above baseline, so the threshold is not delicate — derive it empirically
   from the reference run per the MW-thresholds directive, memory
   `feedback_mw_thresholds_empirical`, no stochastic-model σ) — which, when tripped: (a) marks
   the epoch's CB updates for the PIVOT satellite (minimally; optionally all satellites, D1b) as
   reinit-not-update via the existing `CarryPrior.inflate_q` path (`NetworkRunTypes.hpp:108`,
   fold at `ApplicationNetworkAssembly.cpp:1247`), and (b) bumps the affected sats' CB disc
   counters at the next emit (a value step must coincide with a signalled discontinuity —
   Card-2 rule). Reset-not-repair; never drop measurements. New logic in `NetworkClockQc`
   (post-fit QC seam), state in `NetworkRunContext`. Default-OFF toggle
   `network.cb_excursion_reinit` (+ threshold knob `cb_excursion_median_m`);
   **conditional-fingerprinted** — seed and legs must ALL carry it, never flip mid-chain.
   OFF-path byte-identity gate mandatory.
2. **Phase 2.2 — arm-together for the fresh-convergence run** (the handoff's §7 list,
   unchanged): `code_bias_disc_jump_test`, `emit_ura`, `phase_leveling`, `raim`,
   `clock_runaway_check`, A4 σ̂0 ceiling; **plus the SH health gate reworked** to band-check the
   low-degree coefficient VECTOR (or total-field RMS), counting epochs, **gating not warning**,
   un-throttled (`NetworkShHealth.cpp:75-97`). *(Parts C–D are what happened when this gate was
   armed — read them before implementing it.)*
3. **Phase 2.3 — explicitly OUT of this card:** re-pointing `selectIfPinPair` at the consumer
   pair (**D3**, deferred unless ruled otherwise); any change to q_cb itself (the stiffness is
   correct for OSB physics — the fix is the event response, not a softer carry); anything
   touching the consumer.

**Validation gates (Phase 2):** (i) OFF = byte-identical vs a same-build control on a 10-ep
smoke; (ii) ON over an excursion-bearing window: the detector fires on the known excursion
epochs and `n_reinit` run-end counters are non-zero (0 fired = inert = FAIL); (iii) the pivot-CB
step at a collapse is bounded (re-tracked within ~10 epochs instead of τ=3.8 h) and IF(C1W,C2W)
of G01 shows no multi-hour offset; (iv) grade only datum-invariant/SD quantities (invariant #5)
— the between-sat total-IF methodology restated in the Appendix.
⚠ **Gate (iii) is still UNMET** — Part F.

## Standing-invariant pre-flight (this card)

1. zero-q/stiff response — Phase 2.1 IS this item, for the CB carry. ✔ 2. Release-only runs;
renamed exe (concurrent sessions). ✔ 3. Deferrals triaged: 20:47-event reproduction
(field-inert for the trigger question — 07:32 serves); D3 pin re-point (field-blocking only for
the consumer-pair stability goal, deferred by decision); q_cb retune (rejected with rationale). ✔
4. Run-end counters for every new mechanism (detector fires, reinits, trace row counts) —
Phase-2 gate (ii). ✔ 5. Datum-invariant grading only — Phase-0/2 gates. ✔ 6. Default-OFF +
byte-identity; same-build comparisons only. ✔

## Decisions needed from the user before Phase 2 (Phases 0–1 need none)

- **D1:** CB event-response scope — pivot-only reinit (minimal, targets the measured mechanism)
  vs all-sat CB reinit on a common-mode trip (safer if a future pivot handover moves the
  vulnerable satellite; more disruptive to consumers via mass disc bumps). *Recommendation:
  pivot-only first, with a counter for how often non-pivot CBs would have tripped.*
- **D2:** whether Phase 2 rides this card at all, or waits for Phase-0/1 results to size it.
  *Recommendation: implement 2.1 concurrently (self-contained), but do not schedule the
  fresh-convergence arming run until Phase 1's trigger verdict is in.*
- **D3 (deferred by default):** re-point the IF pin at the consumer pair (A6) — datum
  re-reference, fingerprint-breaking, touches every downstream comparison. Not recommended now.

## Practicalities

Renamed exe; pre-kill name-scoped WER zombies before each launch; LNK1104 = lock → retry.
`pcarry=true/preduce=false` stays as in `leg.json` (all-parallel directive; the pair is mutex —
the loader silently forces pcarry off if preduce is set). Every new `.cpp` needs a manual
`CMakeLists.txt` entry. Smoke-first cadence on every run, including replays. Extend
`scripts/chain_stability.py` rather than writing new one-offs where possible.

---

# Part C — what the guarded leg actually measured

*Absorbed from `docs/SPEC_day129_gate_policy.md` §0 (now `docs/attic/`). Every number was
measured from `runs/day129_guards/` and `runs/day129_cont/seg01/`. The guarded leg
`runs/day129_guards/leg01/` **COMPLETED** 2026-07-28 00:45:46 — final tow **345570** (23:59:30),
**600 epochs**, `n_emitted` **1042** cumulative, chain STATUS COMPLETE. All figures are the
**complete** leg.*

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

## D-G1 — resume grace: DEGRADE, DO NOT WITHHOLD

**Intent.** During a bounded post-resume recovery, ship the epoch with an honest DEGRADED quality
advertisement instead of shipping nothing. C2 forbids shipping it as good; C6 shows **no
threshold choice makes the era short**.

### The seven MUSTs

1. **Grace changes the DISPOSITION, never the VERDICT.** The gate statistic, its out-of-band
   determination, the `sh_gate_oob_*` counters and the `SH_GATE` events MUST be computed and
   recorded identically whether or not grace is active — a grace that suppresses the verdict
   destroys the record that the era was bad and lets a real divergence ride out of the window
   unnoticed.
2. **A third disposition.** `sh_gate_degraded` MUST be distinguishable from both `emitted` and
   `sh_gate_withheld`, and the product H/S line MUST carry a **machine-readable degraded marker**
   (not only an inflated σ) — a consumer that ignores σ must still see it. Grace epochs MUST have
   their own run-end counter, separate from `n_sh_gate_withheld` (invariant #4).
3. **Degrade EVERY quality channel.** Per C5(4) the URA floor alone leaves `sigc`/`sigcc`/`rho` at
   full weight, and `sigcc` is exactly what M4 built for a σ-weighting consumer: a degraded epoch
   MUST floor **all** of `clock_ura_m`, `sigc_m`, `sigcc_m` (`NetworkSolPrep.cpp:440-441` and
   `:834-900`), and any channel left unfloored MUST be named in the report as a known honesty gap.
   **D-G6 = SHIP THE PIVOT WITH A NOTE (ruled).** The datum-pinned pivot is skipped by the URA path
   by design (`NetworkSolPrep.cpp:425` — its variance is the artificial pin), so **its corrections
   ship during grace with NO per-satellite quality signal**; withholding them was rejected (a
   consumer that loses the pivot loses the datum). Binding: (a) the report MUST name the pivot
   (**G01**) and note it is *the satellite whose code bias ratchets* (A2) — the one correction a
   grace consumer cannot de-weight; (b) MUST-2's marker MUST be **epoch-level**, not only per-sat σ
   inflation, so some signal covers the pivot; (c) the run-end line MUST report how many grace
   epochs shipped pivot corrections.
4. **Resolve the deadlock STRUCTURALLY.** The grace decision MUST be evaluated where both
   `NetworkSolPrep` and `NetworkEmitStore` can see it and **before** the `NetworkSolPrep.cpp:140`
   early-return, so `n_emitted` (`:367`) and the ramp-lift rule (`:398`) advance on grace epochs. A
   grace that leaves `n_emitted` frozen re-creates C5(1) elsewhere.
5. **Grace is BOUNDED at 120 epochs and its exit is data-driven — D-G2 = CAP 120.** Measured
   recovery was **239 epochs by a₀₀ and 400–542 by field shape**, so the cap is deliberately
   *shorter* than the observed recovery: **a cap long enough to cover it is indistinguishable from
   disarming the gate.** Grace MUST end at whichever comes first: the gate statistic releases, or
   **120 processed epochs (1 h at 30 s) from the seam**. On reaching the cap while still engaged
   the epoch MUST revert to **hard withhold**, logged un-throttled, evented and counted. **The
   cap-reversion is a FIRST-CLASS DELIVERABLE, not an error path** — *"this resume did not
   re-converge within an hour"* is the most important thing the mechanism can tell an operator,
   and on this leg it is what would have fired. **A run that reverts MUST NOT be graded as a
   successful warm resume.**
6. **Grace arms ONLY at a resume seam** — the warm-resume event, not "the gate engaged". A gate
   engagement in steady state is a real divergence and MUST go straight to hard withhold. (Cold
   starts are covered by the existing RTB-7(b) ramp, which is *not* restored-as-lifted cold.)
7. **Default OFF, byte-identical when off** (invariant #6). **D-G4 = CONDITIONAL APPEND.** (a)
   `warmup_ura_floor_m` / `warmup_epochs` move to a conditional append gated on
   `warmup_ura_floor_m > 0` (today's unconditional site `src/core/app/NetworkRunContext.cpp:589`,
   the policy card's `NetworkRunContext.cpp:218`; copy the `:256-314`-class block) —
   **back-compat HYGIENE**, keeping every unarmed run's fingerprint byte-identical, including
   against snapshots written before the knobs existed. (b) It does **NOT** create an arming path
   (C5(3)), so **grace MUST NOT be implemented by re-arming the RTB-7(b) ramp**: grace gets its
   **own emission-only knobs, deliberately NOT in the fingerprint** — the same class decision the
   SH gate took (`NetworkRunContext.hpp:515-521`: "the raim / clock_runaway_check class, not the
   cb_excursion class"), legitimate because grace changes what is emitted and with what advertised
   quality, **never what a carried state means**. That is what lets grace arm on the **existing**
   `runs/day129_guards` chain with **no new seed**. It MUST NOT read or write `ramp_lifted` /
   `ura_ewma_updates` (snapshotted and fingerprint-coupled) — grace applies its own floor directly.

**CANDIDATE mechanism.** `network.resume_grace_epochs` (cap, **default 120**, 0 = off) +
`network.resume_grace_ura_floor_m` (+ MUST-3's `sigc`/`sigcc` floors), all outside the fingerprint.
Grace state lives in `NetworkRunContext` (`NetworkCarryState`), seeded at the warm resume,
decremented per processed epoch, **not snapshotted** (a resume of a resume re-seeds it — the
conservative direction). Floor value: reuse RTB-7(b) *semantics* (a per-satellite URA floor in m)
but not its state; **MUST derive it, do not guess it** (Part E). Grace MUST NOT extend the
`phase_leveling_max_gap_s` (900 s) exposure the SH gate was designed to avoid — since grace
*emits* it strictly **reduces** that exposure; one line in the report confirms it.

### The D-G2 cap decision table

Under the ruled statistic (D-G3, 4.0/2.5, dwell 3/10) the engaged era is **542 epochs, releasing
at ep 544**. Grace degrades the first `cap` epochs and hard-withholds the rest:

| cap | degraded | hard-withheld | emitted | cap-reversion fires? |
|---|---|---|---|---|
| 60 | 58 | 484 | 58 | YES |
| **120 — RULED (D-G2)** | **118** | **424** | **58** | **YES** |
| 240 | 238 | 304 | 58 | YES |
| 400 | 398 | 144 | 58 | YES |
| 600 (= no cap) | 542 | 0 | 58 | no |

At the looser 6.0/3.0 pair the era is **400 epochs and 200 epochs emit normally**; the same cap
sweep gives 58/118/238/398 degraded with 342/282/162/2 withheld. **This is the D-G2 evidence.**
Any cap ≤ 400 reverts to hard withhold on this leg, i.e. the run reports "recovery did not
complete" — the truth. A cap of 600 (no reversion) ships 542 degraded epochs from a field wrong by
up to 1150 TECU for its first two hours; the degraded advertisement would have to be extreme for
that to be honest.

## D-G3 — the statistic: `rms_ho` RATIO, asymmetry, hysteresis

### The five MUSTs

1. **The load-bearing statistic MUST be a field-SHAPE statistic, not a₀₀ alone.** Measured basis:
   C3 (a₀₀ is datum-coupled ⇒ invariant #5) and C6 (relaxing the a₀₀ low edge far enough to stop
   the chatter loses the 20:47 event; the shape statistic catches it as an **86–107-epoch era**
   instead of a 1-epoch blip). `rms_ho` (degrees ≥ 1) is the CANDIDATE; `rms_all` is acceptable
   with the noted a₀₀ contamination (a₀₀ = 35 TECU contributes **2.2 of a 3.8 TECU** quiet
   `rms_all`). **All ingredients already exist in `network_vtec_sh_health.csv`**
   (`rms_all_tecu`, `rms_deg0..15_tecu`, `kaula_ratio_deg*`) — no new statistic needs deriving,
   only selecting. The gate's own `sh_gate_rms_max_tecu` knob (`GnssConfigTypes.hpp:639`,
   **default 0 = inert**) is the existing hook.
2. **Thresholds MUST be ASYMMETRIC (engage ≠ release) AND TIME-HYSTERETIC (two dwell counters).**
   Today `sh_gate_min_epochs` (**default 1**) gates engagement only; release is instantaneous on
   the first in-band epoch (`NetworkSolveGlue.cpp:1276-1278` resets `sh_gate_oob_run` to 0). Both
   halves are needed: threshold hysteresis alone leaves the 4 chatter engagements, dwell alone
   leaves 3 (C6). Mirror `NetworkIonoDeweightGate`'s `sigma_hi_`/`sigma_lo_` naming and its
   forced-`lo < hi` load validation (`NetworkIonoDeweightGate.hpp:92-93`).
3. **The a₀₀ band is DEMOTED to a wide catastrophic rail, not deleted.** It is the only statistic
   that catches a **pure DC runaway** (a₀₀ → −91 TECU with a plausible field shape). MUST become
   asymmetric and wide enough not to fire on a realization offset.
4. **`sh_gate_min_epochs = 1` MUST NOT remain the default once release dwell exists** — a 1/1
   gate on a shape statistic will chatter on the release edge the same way.
5. **Counting stays UN-THROTTLED and epoch-based** (the deleted Phase-1 report measured the old
   throttled WARN under-reading the reference run **23.8×**). Engage / release / degrade
   transitions all MUST event.

### CANDIDATE numbers (derived below)

| knob | CANDIDATE | basis |
|---|---|---|
| statistic | `rms_ho` (deg ≥ 1 RMS) | C6; scale-free under the normalization below |
| engage | ratio > **4.0** × calibrated median | ref: catches all 3 events; leg: 1 engagement |
| release | ratio < **2.5** × calibrated median | ≈ 1.5× the converged spread |
| engage dwell | **3** epochs | kills 1-epoch blips; costs 3 epochs of latency |
| release dwell | **10** epochs (5 min) | merges the 4 chatter eras; < the 15–30 ep RS transient |
| a₀₀ rail | **[−60, +200]** TECU | leg healthy-depressed min −8.7; ref divergent −302/+2151 |

The a₀₀ rail is deliberately **~7× wider on the low side** than the 0/−5 the card's framing
proposed: at −5 it still fires 5 times on this leg (C6), and those epochs have a quiet `rms_all`
of 5.8–8.6, i.e. **it is firing on a level offset, not a defect.**

### ⚠ Three knob traps the ruling creates — MUST, all three

D-G3 selects a statistic the existing knobs **cannot express**, so a "reuse what's there"
implementation would silently ship a different policy.

1. **`sh_gate_rms_max_tecu` is NOT the ruled statistic and MUST NOT be reused for it.** It is an
   **absolute TECU ceiling on `rms_all`** — the RMS over all 256 coefficients *including* a₀₀
   (`NetworkSolveGlue.cpp:1245-1270`). The ruled statistic is a **DIMENSIONLESS RATIO** on the
   **degree ≥ 1** RMS. Ship new knobs (`sh_gate_ratio_engage` / `sh_gate_ratio_release` /
   `sh_gate_ratio_median_tecu` / engage+release dwell) and either retire `sh_gate_rms_max_tecu` or
   keep it as an independent absolute rail alongside the a₀₀ rail — **but do not overload it.**
2. **The calibration median is MANDATORY when the gate is armed, with NO default.** The
   threshold's meaning is `ratio × median`; a wrong or defaulted median **silently rescales the
   gate**. The loader MUST refuse to arm (hard error, gate disarmed with an explicit message —
   the pattern already used for `cb_excursion_median_m ≤ 0` at
   `GnssConfigLoadNetwork.cpp:1584-1590`) if `sh_gate_ratio_median_tecu` is unset or ≤ 0. It MUST
   also be echoed into `network_run_manifest.json` and the run-end line.
3. **The loader's arm/disarm rule INVERTS.** Today `sh_health_gate` is silently **disarmed** when
   the a₀₀ band is silenced (`low ≥ high`) *and* `sh_gate_rms_max_tecu == 0`
   (`GnssConfigLoadNetwork.cpp:1620-1639`), because band-only is the shipped design. Under D-G3
   the ratio is primary and the a₀₀ band is an optional backstop, so a **band-silenced config is
   LEGITIMATE and must arm**. New rule: arm iff the ratio pair + calibration median are valid; the
   a₀₀ rail and any absolute rms rail are optional; **warn (do not disarm) if all backstops are
   silenced.**

*(Computation detail, also MUST.)* The gate's existing loop already accumulates `Σ val²` and
picks out a₀₀ (`NetworkSolveGlue.cpp:1245-1254`), so **`rms_ho = √((Σval² − a₀₀²)/(n−1))` is
free**. ⚠ **Under Iono-2 dual-layer the subtracted a₀₀ MUST be the layer SUM** (as `a00_total`
already is, `:1257-1260`) and the coefficient count MUST span **both** layers, or the two shells'
degree-1+ content is mis-normalized.

### The empirical derivation — and the amendment it forces

*Per the MW-thresholds directive (memory `feedback_mw_thresholds_empirical`): thresholds come from
the data via MAD / Welford / EWMA over a peer population, never from a stochastic-model σ.*

**The reference day's a₀₀ distribution**
(`runs/day129_cont/seg01/logs/network_vtec_sh_health.csv`, 2160 epochs, 06:00–24:00):

| statistic | a₀₀ (TECU) |
|---|---|
| min / q0.001 / q0.005 / q0.01 | −302.61 / −133.88 / −23.69 / −19.79 |
| q0.02 / q0.05 / q0.25 | −5.57 / 20.99 / 31.50 |
| **median** | **37.25** |
| q0.75 / q0.95 / q0.99 / max | 43.51 / 373.46 / 1538.22 / 2150.98 |
| robust σ (1.4826·MAD) | **9.06** |
| fraction < 2.0 (current low bound) | **2.41 %** |
| fraction < 0 / > 150 | 2.36 % / 7.50 % |

Quiet-hours-only subset (hours 10–18, 1080 epochs, no known event): min **21.88**, q0.05 29.34,
median **37.49**, q0.95 44.85, max **51.21**, robust σ **4.27**.

**Result, and the amendment.** A band derived from this distribution — e.g. median ± 6·robust σ =
**[11.9, 63.1]** — would have withheld **~100 %** of the guarded leg's late epochs, whose a₀₀ runs
−8.7…22.7 with a *quiet* rms_all. The reference day's a₀₀ distribution is **not transferable to
another realization of the same day**: the two runs' a₀₀ levels differ by ~25 TECU because a₀₀
trades against the code-bias common mode. **An absolute a₀₀ band CANNOT be empirically derived in
a transferable way. That is a property of the QUANTITY, not of the derivation.**

**What IS derivable: a scale-free field-shape RATIO.** `rms_ho` calibration sets, all measured:

| calibration window | n | median | 1.4826·MAD | min … max |
|---|---|---|---|---|
| **guarded seed, converged tail (last 60 ep)** | 60 | **2.376** | 0.0532 | 2.251 … 2.483 |
| guarded seed, whole 6 h run | 720 | 2.277 | 0.4016 | 1.553 … 3.409 |
| reference day, quiet hours 10–18 | 1080 | 2.896 | 0.3151 | 1.841 … 5.774 |

The two independent converged runs agree on the **median to 22 %** (2.38 vs 2.90) but their
**robust σ differs 6×** (0.053 vs 0.315) — the 60-epoch tail measures *within-run
epoch-to-epoch stability*, not realization spread. Hence:

* **MUST: normalize by the calibrated MEDIAN (a ratio), NEVER by a robust σ (a z).** The median
  transfers; the σ does not. A ratio threshold of 4× is 4× on either calibration; a z threshold of
  20 means two different things (measured: the leg's healthy-looking tail is **z = 64–84** in
  seed-tail units, **z = 10–12** in ref-quiet units).
* **MUST: the calibration window is a CONVERGED, MULTI-HOUR window of the SAME CHAIN** (the seed's
  tail, or a prior converged leg) — **never a trailing window of the run being gated** (C6:
  trailing robust z = 0 engagements on a run that begins diverged).
* **MUST: the calibration median is recorded in `network_run_manifest.json`** and in the run-end
  line. A gate whose threshold depends on an off-line calibration constant is otherwise unauditable.
* **MUST: re-derive per chain, and re-state in every report that the constant is CHAIN-SPECIFIC**
  (the same discipline the CB-excursion threshold got).

**Ratio values at the three reference events, in ref-quiet units (median 2.896): 07:32 = 77.8×,
20:47 = 34.2×, 19:21 = 4.8×.** Converged runs sit at **1.0–2.0×**; the leg's end-of-leg tail sits
at **2.4–2.9×**. A **4.0× engage / 2.5× release** pair separates all three events from both
converged runs, and correctly classifies the leg's tail as *not yet converged but no longer
divergent* — the exact state grace exists to express.

**The a₀₀ rail, derived.** Rail bounds must exclude every *healthy* realization and include every
*divergent* epoch. Healthy: ref quiet **[21.9, 51.2]**, leg healthy-tail min **−8.7**. Divergent:
ref min/max **−302.6 / +2151.0**, leg min **−1147.2**. Widest healthy → nearest divergent margin:
−8.7 → −19.8 (ref q0.01). CANDIDATE rail **[−60, +200]** takes **~7× margin below the healthy
minimum** and still catches every divergent epoch measured in either run by **≥ 5×**. **MUST: the
rail is the BACKSTOP; if the rail is ever the only thing that fires, the report says so** — it
means the shape statistic missed a DC-only runaway and the statistic choice needs revisiting.

## D-G5 — the CB-excursion detector's resume window: STATE-COUPLED

### The six MUSTs

1. **The window is derived from the shock, not fixed at 1 — and the completed leg says the shock
   does not decay on a 10-epoch scale.** Measured from the FINISHED run (steady state = median over
   the last 120 epochs: `clock_std_m` **12.15 m**, `worst_dev_m` **30.78 m**): from-seam ep 0
   **371.1** m (30.5× steady, worst_dev 1264); ep 10 **398.1** (32.8×, 1767); ep 20 **469.4**
   (38.6×, 2110); ep 38 **586.2** (**48.3×**, 2680). The clock spread **GROWS** for ~40 epochs and
   only stays below 5× steady from **ep 177** (tow 332910), below 2× from **ep 329**. So a 20-epoch
   window suppresses the ep-1/+2/+10 trips (172 / 54 / 69 m) but **NOT** the ep-69/+70 trips
   (115 / 87 m at tow 329670/329700) — which fired while the field was still 45× steady and
   a₀₀ ≈ −1000 TECU, i.e. shock artifacts too. **A fixed epoch window cannot separate them without
   being ~70 epochs long, which is too blind for a guard.**

   **D-G5 = STATE-COUPLED (ruled).** The window is
   `post_resume AND (grace active OR SH gate engaged OR epochs_since_seam < floor)`, **floor = 20
   epochs**: the run already maintains an authoritative "not converged" state (D-G1/D-G3) and a
   second, independent epoch counter would inevitably disagree with it; the floor covers the case
   where the SH gate is off or releases early.

   **⚠ THE SAME-EPOCH ORDERING FACT that makes this lag-free (verified).** Within one epoch the
   driver runs `networkSolveGlue` (`ApplicationNetwork.cpp:1488`, where the SH-gate verdict is
   computed) → `networkInstallCarry` (`:1522`) → `networkSolPrep` (`:1535`) → `networkEmitStore`
   (`:1546`) → **`networkClockQc` (`:1585`)** → `networkStashStatus` (`:1603`), so the CB detector
   reads `ctx.carry.sh_gate_engaged` **already set for the current epoch** — no lag, no stale read.
   **MUST NOT** add a defensive one-epoch lag; **MUST** state this ordering dependency in a
   comment, because it **silently breaks if a future decomposition moves `networkClockQc` above
   `networkSolveGlue`.** **MUST: the window ENDS WHEN THE STATE CLEARS — it is NOT latched**; once
   grace has exited and the gate released, the detector is live again even if `epochs_since_seam`
   is still small (the floor is a minimum, not a maximum).
2. **Suppression means OBSERVE AND COUNT, NOT ACT.** Inside the window the detector MUST still
   compute its statistic, update `cb_exc_max_median`/`_tow`, and emit its event with a **distinct
   action** (e.g. `cb_excursion_suppressed_resume`), while performing **no** CB reinit and queueing
   **no** disc bump. A silent window would hide a real excursion landing inside it and make
   invariant #4's telemetry lie.
3. **A distinct run-end counter** (`cb_exc_suppressed_resume`) with the same
   armed-but-never-tripped WARN discipline the detector already has; the report MUST state how many
   trips were suppressed and what their statistics were, so the window's cost is re-derivable.
4. **The window MUST NOT be open-ended, and MUST NOT be re-armed by anything but a resume.** A real
   excursion at seam+21 must trip.
5. **The pending-bump queue's behaviour across a withheld/graced era MUST be re-checked under
   D-G1**, since grace changes which epoch is "the next emit". Today three reinits correctly
   collapsed to one bump landing 2 h later (C4); under grace the first graced epoch becomes the
   landing site, which is *better* (the bump lands nearer the value step) but MUST be confirmed.
6. **Rule on whether the window should also gate the CB reinit's σ** (`clock_monitor_reinit_sigma_m`,
   30 m). **Three reinits inside 10 epochs is not "reset once"; it is a ratchet of its own kind.**
   Out of scope to decide here, but the report MUST state whether repeated reinits inside a shock
   are a distinct defect. *(Recommendation: yes — record it, defer the fix.)*

---

# Part E — validation gates, implementation order, and the UN-DERIVED prerequisite

## E1 Validation gates — all MUST

1. **OFF byte-identity**, same-build, on the established **20-station × 11-epoch cold leg**
   (`runs/bytecheck/`): `products.txt`, `network_solution.csv`, `network_trace.csv`,
   `network_epoch_status.csv`, `network_events.csv` **SHA-256 identical** against the pre-change
   binary. New disposition/columns are appended-only (T2d precedent).
2. **Counterfactual agreement**: for the *shipped* policy, the off-line harness's predicted
   engage/release epochs MUST match the live run's `SH_GATE` events **exactly** on a replay of one
   converged window and one seam window. **A policy whose off-line simulation and on-line
   implementation disagree is not validated, whatever the counters say.**
3. **ON-path counters non-zero** for every new mechanism: grace epochs, cap-reversion events,
   suppressed CB trips (invariant #4 — 0 fired = inert = FAIL; the inert-case WARNs are retained).
4. **A re-run of the seam** (a fresh leg resuming the same `runs/day129_guards` seed snapshot —
   armable with **no new seed** per D-G1 MUST-7) MUST show, and the report MUST tabulate: grace
   engaged at **epoch 1**; `n_emitted` advancing through the grace era; floored `clock_ura_m` /
   `sigc` / `sigcc` **on the wire** (read back from `products.txt`, **not** the log); **the
   cap-reversion firing at epoch 121** — the cap table predicts 118 degraded + 424 withheld, so a
   re-run that does *not* revert means the cap is not wired; and the SH statistic's engaged era
   **identical** to the original leg's (D-G1 MUST-1). The CB detector MUST report **≥ 3 suppressed
   trips** (the seam+1/+2/+10 set) and MUST NOT suppress any trip after grace exits and the gate
   releases (D-G5 MUST-1's non-latching rule).
5. **Grading stays datum-invariant** (invariant #5): grade the degraded era's product with the
   **between-satellite total-IF deviation** methodology (Appendix), not raw a₀₀.
6. **No hard exclusion**: nothing here may drop a measurement (memory
   `feedback_prefer_fallback_over_drop`). Grace *adds* an emission path; the gate's withhold stays
   **emission-only** (solve / carry / QC continue).

**Harness.** C6's counterfactual tables and the D-G2 cap table are reproducible from an off-line
simulator reading `network_vtec_sh_health.csv` + `network_epoch_status.csv`. **MUST: promote it
into `scripts/`** (extending `scripts/p2_gate_verdict.py`) as part of the implementing card, and
**make the chosen policy's counterfactual a required table in that card's report.** *(The original
lived only in a session scratchpad and is presumed gone — regenerate it against the archived leg.)*
**A gate policy shipped without its counterfactual on BOTH a converged run and a seam-bearing run
is a hand-tuned constant with extra steps.**

## E2 Standing-invariant pre-flight (the policy)

1. **Stiff/zero-q event response.** Two stiff carries are touched. The SH carry is *the* stiff
   quantity in C2's mechanism — a garbage value entered it at the seam and ratcheted for 239 epochs,
   **invariant #1 in its purest form, a stiff carry with no event response for a RESUME event. This
   card does NOT fix that** (Part F); it makes the consequence honest on the wire. The CB carry's
   response (Phase 2.1) is narrowed by D-G5 so it does not fire on the shock itself. ✔
2. Release-only, renamed exe; name-scoped WER-zombie pre-kill; LNK1104 ⇒ retry. ✔
3. **Deferrals triaged.** *Field-blocking:* the seam shock itself (Part F) — the SH gate is **not
   field-runnable on a warm-resumed chain** until either grace ships or the shock is fixed, because
   ~58 % of a leg is withheld. *Field-inert:* the pivot's missing URA channel (C5(5)) — inert only
   if grace excludes the pivot or the report states the gap; the repeated-reinit question (D-G5
   MUST-6) — record-and-defer. ✔
4. Run-end counters for grace, cap reversion, suppressed CB trips, plus the existing
   armed-but-never-tripped WARNs. ✔ 5. Datum-invariant grading only — **and this card's central
   technical finding is an invariant-#5 violation in the *shipped* gate (a₀₀ is not
   datum-invariant).** ✔ 6. Default-OFF + byte-identity, conditional fingerprint appends. ✔

## E3 Implementation order for the owning session

A sequencing aid, not a design. Each step is independently gate-able; steps 1–3 are
byte-identity-safe when their knobs are off.

1. **The D-G3 knobs + loader** — ratio knobs, mandatory calibration median with a hard disarm, the
   inverted arm/disarm rule, `rms_ho` from the existing coefficient loop. Gate: OFF byte-identity
   (E1.1) + counterfactual agreement on a converged window (E1.2). *This step alone fixes defect B
   and can ship without grace.*
2. **Fingerprint hygiene** — conditional append for `warmup_ura_floor_m` / `warmup_epochs` (C5(3),
   D-G1 MUST-7). Gate: an unarmed run still resumes the existing `runs/day129_guards` seed.
3. **The D-G5 CB suppression** — state-coupled window + `cb_excursion_suppressed_resume`
   event/counter + the ordering comment. Gate: the seam re-run's ≥ 3 suppressed trips and the
   non-latching check (E1.4).
4. **D-G1 grace** — the emission-only path (own knobs, outside the fingerprint, never touching
   `ramp_lifted`), evaluated upstream of `NetworkSolPrep.cpp:140`, with the third disposition, all
   three σ floors, the epoch-level marker and the cap-reversion telemetry. Gate: the full E1 set,
   notably the cap firing at epoch 121.
5. **Floor calibration** (the E4 prerequisite). 6. **Re-run the arming chain** for Phase 2.1 gate
   (iii), still unmet (Part F).

## E4 ⚠ The UN-DERIVED prerequisite — the grace floor MAGNITUDE

**Not a decision; a measurement.** `resume_grace_ura_floor_m` (and the `sigc`/`sigcc` floors of
D-G1 MUST-3) is **un-derived**. It MUST come from an `ApplicationConsumerReplay` sweep (**~33
s/arm**, the cost figure from memory `project_consumer_replay_driver`) measuring **consumer
position error against floor value** over a degraded era — not from taste. The floor must be large
enough that a σ-weighting consumer effectively de-weights the era. **Until it is derived the
report MUST call grace "wired but UN-CALIBRATED", and the E1 gates MUST NOT be read as validating
the floor magnitude.**

---

# Part F — open / owed

1. **Phase 2.1 validation gate (iii) is STILL UNMET.** The 20:47 excursion fell inside the
   withheld seam era, and the leg does not reproduce the reference run's 19:21 event either (C4).
   The arming chain needs a re-run whose seam is either graced (D-G1) or placed far enough ahead
   of the events to have recovered. **Do not read gate (iii) off a run whose event sits inside a
   graced or withheld era.**
2. **⭑ The cheap zero-code mitigation: place the seed so the leg's first event is ≥ 400 epochs
   (3.5 h) after the seam.** Available today with no code at all.
3. **THE SEAM SHOCK ITSELF IS NOT FIXED BY ANY OF THIS.** C2's mechanism — the D5 innovation gate
   re-tapping **92 % of a restored DD carry** at the first post-resume epoch, collapsing the phase
   constraint and letting the SH field slide 1400 TECU — is an **ESTIMATION defect**, and it is
   the load-bearing one. Every number in Part C says so: **no emission policy makes that leg's
   product good**, because the field was genuinely wrong for ~3.5 of its 5 hours and had not
   re-converged at the end. That defect belongs to `docs/SPEC_warm_resume_event_card.md` (the RS
   card), whose "snap_wwork audit" is still open, and should be re-scoped with this measurement in
   hand. *(The gate policy also cited a `docs/REPORT_warm_resume_event.md` companion — that file
   does not exist.)*
4. **D-G4's conditional fingerprint append is NOT implemented** — `warmup_ura_floor_m` /
   `warmup_epochs` are still appended unconditionally at `NetworkRunContext.cpp:589`.
5. **The D3 IF-pin re-point** (A6) remains deferred: `selectIfPinPair` pins (C1C, C2L) while
   consumers apply (C1W, C2W). Fingerprint-breaking datum re-reference.
6. **Phase 0 / Phase 1 results are not recorded in this file** — the report that held them
   (`REPORT_day129_frame_guards_phase01.md`) no longer exists. What survives: the throttled-WARN
   23.8× under-count (D-G3 MUST-5), the `phase_leveling_max_gap_s` 900 s exposure argument (D-G1
   CANDIDATE), the chain-specific-threshold discipline (D-G3 derivation), and the "the gate's
   withhold is emission-only; solve/carry/QC continue" fact (E1.6).

---

# Appendix — traps this chain paid for

**⭑ The `start_offset_hours` half-epoch residue gotcha.** *(This is the target of
`src/core/app/ApplicationConsumerReplay.cpp:404`, whose "§6" pointed at the deleted
`HANDOFF_night305_day129.md`.)* ⚠ **RECONSTRUCTED, not quoted** — the source file is gone, so
what follows is re-derived from the rule as it survives in CLAUDE.md's "Cold-start hygiene" line
plus the chain's own practice; the *mechanism* sentence is inference, not a measured claim.
**The rule (supported): always take a half-epoch back-off on every `start_offset_hours`** (e.g.
`13.0` → `13.0 − 15 s` in hours), because bare hour values sit on the double-residue boundary.
**The mechanism (inferred, unverified): a whole-hour offset lands exactly on an epoch boundary
in a way that is ambiguous to both the resume arithmetic and the consumer replay's epoch
alignment, and resolves inconsistently.** Either way, assert `snapshot age 30.0 s` in the log
after a warm resume; any other value means the offset arithmetic missed.

**Grade only datum-invariant / between-satellite quantities.** The handoff's methodology, restated
because its file is gone: grade the **between-satellite total-IF deviation** (CLK + IF_W(CB) per
satellite, referenced to the constellation median), reported as **hourly bands — never slopes**.
Raw wire steps confound benign frame drift with real defects (invariant #5); A4 is the worked
example.

**Never point `state_snapshot_path` at an immutable seed** — it is read AND written. Copy the
immutable to a leg-owned snapshot path first.

Three traps stated in full elsewhere, listed here so a reader of the Appendix alone meets them:
**an empty counter is a wiring verdict, not a clean bill of health** (A7 — `n_adapt`,
`es.n_refloat_sats` and the `sigma0` emit column all read 0 for structural reasons); **threshold
constants are CHAIN-SPECIFIC** and must be re-derived and re-stated per chain (D-G3 derivation);
**the two hard pins are not symmetric with the other satellites** — G01 and MBAR00UGA cannot move
along the common mode, which is why the pivot absorbs one-sided excursions (A2.2) and why it has no
URA to degrade (C5(5)).
