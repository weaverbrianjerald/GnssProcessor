# DOY-129 phases: the closed-loop replay, the trigger traces and the CB-carry guard

**STATUS**

- **id:** DOY-129 phases
- **topic:** raim-qc
- **status:** closed
- **summary:** Phase 0 and 1 results are not on record; Phase 2.1's guard `network.cb_excursion_reinit` passed gate (ii) mechanically, but D1's pivot-only scope was FALSIFIED and gate (iii) is UNMET: KEEP_OFF.
- **dates in the text:** 2026-07-28
- **knobs / posture:** `realtime.network_products_path`, `processing.network_consumer.enable`, `network.trace_sh_spectrum`, `network.trace_dd_fractional`, `network.cb_excursion_reinit`
- **hashes named in the text** (commit / exe / byte-cert): none
- **run dirs named in the text:** `runs/day129_cont`, `runs/day129_guards`
- **source:** `docs/archive/SPEC_day129_frame_guards.md`: Phase 0, Phase 1, Phase 2, the pre-flight, the decisions D1 to D3, the practicalities, and the Part C preamble

**Outcome.** The plan: replay the reference products through the closed-loop consumer (Phase 0), trace the a₀₀-collapse trigger (Phase 1), then guard the code-bias carry with a common-mode excursion detector armed on a fresh convergence (Phase 2). The Phase 0 and 1 results are not on record: their report no longer exists. Per the [ledger](../../CONFIG_promotion_ledger.md), the guard fired 5 times with 5 G01 reinits, all seam-coupled, while 130 non-pivot cases refuted the pivot-only scope. The guarded leg's SH gate withheld the 20:47 event ([DOY-129 defects](../raim-qc/DAY129-DEFECTS-ABC.md)); gate (iii) is owed in [DEC-day129-gate-policy](../../decisions/DEC-day129-gate-policy.md) Part F.

> Results are appended TO THIS FILE, below the verbatim text. The archived monolith is frozen; never append there.

---
<!-- BEGIN VERBATIM: SPEC_day129_frame_guards.md -->
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

