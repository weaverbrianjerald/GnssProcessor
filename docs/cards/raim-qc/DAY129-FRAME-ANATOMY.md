# DOY-129 anatomy — the answered questions — the pinned pivot's code bias, the a₀₀ collapse anatomy, the quiet-hour wander, and the disc counter as a DATUM channel

**STATUS**

- **id:** DOY-129 anatomy
- **topic:** raim-qc
- **status:** closed
- **summary:** the answered questions — the pinned pivot's code bias, the a₀₀ collapse anatomy, the quiet-hour wander, and the disc counter as a DATUM channel
- **dates in the text:** 2026-08-12 → 2026-08-15
- **knobs / posture:** `network.code_datum_time_linked`, `network.cb_kaula_prior`, `estimation.global_vtec_sh`, `estimation.ionosphere_weighting`
- **hashes named in the text** (commit / exe / byte-cert): —
- **run dirs named in the text:** `runs/day129_cont`, `runs/day129_guards`, `runs/bytecheck`
- **source:** `docs/archive/SPEC_day129_frame_guards.md` — (preamble), A1, A2, A2.1, A2.2, A2.3, A3, A3.1, A3.2, A4, A5, A6, A7

> Results are appended TO THIS FILE, below the verbatim text. The archived monolith is frozen; never append there.

---
<!-- BEGIN VERBATIM: SPEC_day129_frame_guards.md -->
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

