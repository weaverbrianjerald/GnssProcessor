# SPEC — DOY-129 frame guards & trigger instrumentation (Opus card)

**Source analysis:** `docs/REPORT_night305_day129_investigation.md` (read it first;
this spec only restates what is needed to act). Lineage:
`docs/HANDOFF_night305_day129.md`. Artifacts: `runs/day129_cont/seg01/`,
`val_out/night305_*`, `leg.json`.

**Mechanism being guarded (one paragraph).** The producer's between-satellite
frame has no guarded stiff anchor: phase memory leaks 13.5 %/epoch (D5
re-taps), satellite clocks are effectively white (q_clock:q_cb = 10⁸), and the
only stiff anchors are the near-static CB states (q_cb = 1e-10 m²/s) and the
two hard pins (pivot sat clock G01, datum receiver MBAR00UGA). Common-mode
(one-sided) excursions ratchet into the pivot's CB at gain K ≈ 2.2e-3/ep and
bleed at τ ≈ 3.8 h; the SH-VTEC collapse is a follower symptom; the emitted
disc counters carry no magnitude integrity. Guard the CB carry (invariant #1),
instrument the trigger, measure the consumer impact.

---

## Phase 0 — Q5 closed-loop replay (no new code, run-only, ~minutes/arm)

Replay `runs/day129_cont/seg01/products.txt` through the closed-loop consumer
(`NetworkProductReader::replay`; config parse `GnssConfigLoadNetwork.cpp:3218-3262`;
`realtime.network_products_path` + `processing.network_consumer.enable`;
`closed_loop` is NOT in the snapshot fingerprint — no producer implications).
Consumer station: any station with DOY-129 obs (`data/2024-129/obs`, 311
available); prefer one NOT in the 305 roster if convenient, else any.

Arms (full-day each, grade `pos_err_3d_m,east_m,north_m,up_m` from the
consumer trace, `NetworkRunTraces.cpp:115-116`):

- **A:** consumer using C1W/C2W pseudoranges (convention default).
- **B:** identical but C1C/C2L.

**Predicted signature (falsifiable):** arm A sees the 20:47→EOD era as a
G01-specific pseudorange fault (tens of m on one PRN, decaying τ≈3.8 h); arm B
sees almost nothing. Also record both arms across the 07:32 event (expect
little damage in either — transient only). Report hourly position-error bands
per handoff methodology (no slopes).

Cadence: smoke the replay on a 10-epoch window first (standing directive),
then full day. Release exe only, renamed copy (`gnss_processor_opus_run.exe`).

## Phase 1 — trigger instrumentation (code; trace-only; fingerprint-SAFE; then one 3–4 h leg)

All items are trace/diagnostic: **byte-identical when their toggle is off**
(invariant #6), and every toggle is fingerprint-SAFE (verify each against
`NetworkRunContext.cpp:172-307` before running — nothing here may enter the
fingerprint). Trace-stream ownership stays in `logging/NetworkRunTraces`
(seam decision tree in CLAUDE.md). Appended CSV columns only — positions
preserved (T2d precedent).

1. **Per-epoch carry-churn counters** → append to `network_epoch_status.csv`:
   `n_d5_flags, n_d5_retaps, n_chord_births, n_slip_refloats` (per-epoch values
   of the run-end totals at `main_log` Card-4 lines). Source: the carry driver
   epoch counters already accumulated in `ctx.carry` / driver returns
   (`NetworkInvariantCarryDriver`, `NetworkInstallCarry.cpp`).
2. **Per-epoch iono-deweight state** → append `n_dw_engaged, n_dw_zero,
   dw_min_w, dw_mean_w` (gate already exposes per-station state:
   `NetworkIonoDeweightGate` — add cheap aggregate accessors) AND emit
   engage/release transitions as `IONO_DW` events into `network_events.csv`
   (station, z, w). Serial harvest site only (threading contract in the gate
   header).
3. **SH estimability spectrum** → per-epoch λ_min/λ_max (+ smallest-mode
   participation vector at a configurable stride) of the SH block via
   `NetworkEstimator::dumpEstimabilitySpectrum`, behind
   `network.trace_sh_spectrum` (new, default false). New diag seam or
   `NetworkShHealth` — NOT the driver.
4. **Emit-path post-fit σ0** → the `sigma0` column is 0.0000 on all 2160 rows
   of the reference run: find why the statistic is not computed on this path
   and make it computed + traced (compute-only; the A4 ceiling stays at its
   current setting — behavior change belongs to Phase 2).
5. `network.trace_dd_fractional: true` for the leg (existing knob, known SAFE;
   closes handoff Q6 measurability).

**Run:** warm-resume from `val_out/night305_conv_immutable.txt` (copy it to a
leg-owned snapshot path — never point `state_snapshot_path` at the immutable;
it is read AND written), `start_offset_hours` per the resume arithmetic in
handoff §5 (DAY_ANCHOR for DOY-129; assert `snapshot age 30.0 s` in the log),
duration ≈ 3.5 h GPS time to cover the 07:32:00 collapse (tow 286320) with
full traces. Wall cost ≈ 4 h at 41 s/ep. 10-ep smoke first. Do NOT try to
reproduce the 20:47 event in Phase 1 (needs a ~15 h leg; the 07:32 event
answers the trigger question — Q1 needs *any* onset under instrumentation).

**Deliverable:** the trigger verdict — at tow 286320 (and the ramp before it):
does λ_min crater? does the re-tap count spike? did a deweight release wave
land? Any one confirmed ⇒ Q1 closed; all quiet ⇒ report that honestly
(invariant: counters, not absence of crashes).

## Phase 2 — guards (code; default-OFF; needs a FRESH convergence to arm) — GATED, see decisions

Do not start Phase 2 until Phase 0/1 results are in AND the user has ruled on
D1/D2 below. Fingerprinted knobs (`code_bias_disc_jump_test`, `emit_ura`,
`phase_leveling` — handoff §5) require a fresh 6 h seed + re-run to arm:
budget ≈ 8 h seed + whatever leg length the validation needs.

1. **CB carry event response (the invariant-#1 fix; the core of this card).**
   Recommended shape (decision D1): a network-wide common-mode excursion
   detector — per-epoch median |Δclock| across satellites against a
   robust threshold (the excursion epochs sit at 3–4 orders of magnitude
   above baseline; threshold is not delicate — derive it empirically from the
   reference run per the MW-thresholds directive, no stochastic-model σ) —
   which, when tripped: (a) marks the epoch's CB updates for the PIVOT
   satellite (minimally; optionally all satellites, D1b) as reinit-not-update
   via the existing `CarryPrior.inflate_q` path
   (`NetworkRunTypes.hpp:108`, fold at `ApplicationNetworkAssembly.cpp:1247`),
   and (b) bumps the affected sats' CB disc counters at the next emit
   (a value step must coincide with a signalled discontinuity — Card-2 rule).
   Reset-not-repair; never drop measurements. New logic goes in
   `NetworkClockQc` (post-fit QC seam), state in `NetworkRunContext`.
   Default-OFF toggle `network.cb_excursion_reinit` (+ threshold knob);
   OFF-path byte-identity gate mandatory.
2. **Arm-together for the fresh-convergence run** (handoff §7 list, unchanged):
   `code_bias_disc_jump_test`, `emit_ura`, `phase_leveling`, `raim`,
   `clock_runaway_check`, A4 σ̂0 ceiling; plus the SH health gate reworked to
   band-check the low-degree coefficient VECTOR (or total-field RMS), counting
   epochs, gating not warning, un-throttled (`NetworkShHealth.cpp:75-97`).
3. **Explicitly OUT of this card:** re-pointing `selectIfPinPair` at the
   consumer pair (D3, deferred unless ruled otherwise); any change to q_cb
   itself (the stiffness is correct for OSB physics — the fix is the event
   response, not a softer carry); anything touching the consumer.

**Validation gates (Phase 2):** (i) OFF = byte-identical vs a same-build
control on a 10-ep smoke; (ii) ON over an excursion-bearing window: the
detector fires on the known excursion epochs and `n_reinit` run-end counters
are non-zero (counters are the ON-path telemetry — 0 fired = inert = FAIL);
(iii) the pivot-CB step at a collapse is bounded (re-tracked within ~10 epochs
instead of τ=3.8 h) and IF(C1W,C2W) of G01 shows no multi-hour offset;
(iv) grade only datum-invariant/SD quantities (invariant #5) — use the
between-sat total-IF methodology from handoff §3.

## Standing-invariant pre-flight (this card)

1. zero-q/stiff response — Phase 2.1 IS this item, for the CB carry. ✔
2. Release-only runs; renamed exe (concurrent sessions). ✔ noted per phase.
3. Deferrals triaged: 20:47-event reproduction (field-inert for the trigger
   question — 07:32 serves); D3 pin re-point (field-blocking only for the
   consumer-pair stability goal, deferred by decision); q_cb retune (rejected
   with rationale). ✔
4. Run-end counters for every new mechanism (detector fires, reinits, trace
   row counts). Phase-2 gate (ii). ✔
5. Datum-invariant grading only. Phase-0/2 gates. ✔
6. Default-OFF + byte-identity; same-build comparisons only. ✔

## Decisions needed from the user before Phase 2 (Phases 0–1 need none)

- **D1:** CB event-response scope — pivot-only reinit (minimal, targets the
  measured mechanism) vs all-sat CB reinit on a common-mode trip (safer if a
  future pivot handover moves the vulnerable satellite; slightly more
  disruptive to consumers via mass disc bumps). *Recommendation: pivot-only
  first, counter for how often non-pivot CBs would have tripped.*
- **D2:** whether Phase 2 rides this card at all, or waits for Phase-0/1
  results to size it. *Recommendation: implement 2.1 concurrently (it is
  self-contained), but do not schedule the fresh-convergence arming run until
  Phase 1's trigger verdict is in.*
- **D3 (deferred by default):** re-point the IF pin at the consumer pair —
  datum re-reference, fingerprint-breaking, touches every downstream
  comparison. Not recommended now.

## Practicalities

- Concurrent-session hygiene: renamed exe; pre-kill name-scoped WER zombies
  before each launch; LNK1104 = lock → retry.
- `pcarry=true/preduce=false` stays as in `leg.json` (all-parallel directive;
  the pair is mutex — loader silently forces pcarry off if preduce is set).
- Every new `.cpp` needs a manual `CMakeLists.txt` entry.
- Smoke-first cadence on every run, including replays.
- Grading scripts: extend `scripts/chain_stability.py` rather than new
  one-offs where possible.
