# HANDOFF — 305-station ALL-ON golden candidate (1h, deg-15 SH, GPS+GAL)

**Executor:** Opus session (config + runs only — ZERO source edits).
**Author/validator:** Fable (Jul 13 2026). Report results back; Fable grades the verdict.
**Independence:** does NOT depend on the decomposition survey; do not touch any
decomposition goldens/masks/docs.

## Objective

First-ever run of the full Card-4/M-queue stack together at network scale: a
candidate "all-on golden" arm. User accepts cross-contamination risk; the payoff
is the small chance the combined stack performs well at 305 stations, plus this
becomes the ON-arm candidate for the decomposition INC-0 byte-identity matrix.
Science question is STEADY-STATE quality at scale (1h = pre-event window; this
does NOT re-litigate the hr-4 event, where 305/deg-15 already has a NEGATIVE
verdict as a cure).

## Config recipe

Create `config/test_suite/_net305_allon_1h.json`:

1. **Base = `config/test_suite/_net305_gpsgal_sh_n15_1h.json`** (305 stations,
   GPS+Galileo, SH max_degree 15 / cutoff 25°, elevation_mask_deg 25,
   max_duration_s 3600, native 30 s — do NOT add gnss_rate_s decimation).
2. **Merge INTO its `network` section** (toggle source =
   `config/test_suite/_eq34_carryinv_em10_d5v2_3h.json` lines ~620-642):
   ```json
   "code_datum_time_linked": true,
   "phase_datum_eq34": true,
   "carry_invariant": true,
   "carry_invariant_qz": 0.0001,
   "carry_invariant_innov_gate": true,
   "carry_invariant_innov_k": 4.0,
   "carry_invariant_innov_calibrate": true,
   "carry_invariant_innov_calibrate_window": 60.0,
   "carry_vce": true,
   "trace_dd_fractional": true,
   "trace_dd_fractional_max_pairs": 2000,
   "trace_dd_fractional_epoch_stride": 1
   ```
   (`carry_invariant_emit_rebase` defaults true under carry — no key needed.)
3. **Do NOT set** (mutex / broken / excluded):
   - phase-datum ensemble (load-reject vs eq34), sum-zero gauge (throws vs DD
     priors), `carry_invariant_innov_refold` (unimplemented, warns),
     `carry_vce_apply` (v2, throws), pin_round / carry_shift / rpb_q
     (warn-inert under carry — leave absent), GLONASS/BDS (keep
     `estimate_glonass: false`, GPS+GAL only), any consumer FULL clock-consume
     mode.
   - `carry_invariant_honesty` stays OFF this run (monitor-only; un-measured
     cost at 305 — arm it in a rerun if the first pass is cheap).
   - AR: leave producer defaults (phase-2 fixability is log-only; user wants
     AR off — fraction distribution is read from the trace instead).

## Run discipline (standing directives)

- **Release build ONLY** (`build/x64-Release`); run a **RENAMED exe copy**
  (e.g. `gnss_processor_allon_run.exe`) — Fable/Opus sessions may rebuild
  concurrently and the linker must never hit a locked exe.
- Pre-kill any zombie `gnss_processor*` processes before launching (WER wedge).
- Expected safe horizon: the known QC-armed 305 crash fires >~1.5 h; a 1h run
  is under it. If it crashes anyway, capture the full signature (last epochs,
  exception text, which station/matrix) and STOP — that is a result, not a
  retry loop. carry_invariant has never run at 305; degenerate/non-PD fold
  warnings are exactly the kind of thing to report verbatim.

## Acceptance telemetry (check COUNTERS, not absence of crashes)

- Load banner: eq34 + carry_invariant + D5 gate + v2 calibration all announced;
  zero unexpected warn lines (a `carry_shift`-style warn-inert line is fine).
- Run-end lines, all nonzero where applicable: carry counters
  (born/carried/marginalized/slip_kills/dropped), D5 `innov_flags`/`retaps`,
  the v2 σ̂₀ line (expect σ̂₀ > 1; 50-sta regimes gave 2.5-4.1), emit-rebase
  counters, M2 VCE run-end summary. The D5 reverse-map-miss WARN must be
  ABSENT (0 misses).
- The config-echo JSON line must contain the new toggles (proof the config
  parsed, not defaulted).

## Grading (datum-invariant quantities ONLY)

```
python scripts/grade_ddfrac.py <run_dir>
python scripts/grade_ddfrac_perhour.py <run_dir> 600     # 10-min buckets
```
- Primary: tail DD-step median (cyc/ep). Reference points (50-sta, 3h):
  em25 OFF 1.50 / em10+D5v2 2.35; carry-live golden lineage 0.085 (different
  config class — not directly comparable). Anything ≤~1 at 305 all-on is a
  headline result → escalate to 3h/6h ladder per staged cadence.
- Fraction distribution (the AR-off integer check): med|f| and the |frac|
  histogram from network_dd_fractional.csv. ≈0.25 uniform ⇒ no integer
  structure (AR futile here, matches T4-era verdict); mass piled near 0
  ⇒ integers exist in the all-on frame — flag LOUDLY (it advances M5).
- a00 sanity from the producer log: physical band (tens of TECU), not
  hundreds/thousands (deg-15 obs-per-DOF blowup mode).

## Report back

One table: {tail DD-step, per-bucket trend, med|f| + histogram shape, σ̂₀,
innov flags/retaps, slip_kills, a00 range, wall time}; plus the run-end counter
block verbatim, plus any warn/crash text. No commits — leave the new config
untracked; Fable folds it into the pre-freeze config decision.
