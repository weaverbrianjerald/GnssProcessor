# CARD 4 diagnostic ladder — the hr-4 event under the invariant carry (Opus runs, Fable adjudicates)

*Fable-specced 2026-07-12. Context: memory `project-m1-card4-phasec3c` (cliff
section). The ON arm (eq34 + carry_invariant, q_z=1e-4) is best-ever on 3h
(1.50 cyc/ep, hr 0-3 converging 5.4→1.29) but detonates at content-hour 4
(hr4=54, hr5=118, a00 diverges) — the SAME event hour that kills the legacy
stack. PB/amb carries are fully bypassed under ON, and the CB-floor causality
probe came back NEGATIVE (honest 4.0 m² floor: hr4=61/hr5=115, unchanged) —
so the carrier is NOT the phase carries and NOT CB-carry stiffness. Find it.*

## Ground rules
- **Release only** (build/x64-Release, renamed copy `gnss_processor_opus_run.exe`).
- One arm at a time from the SAME build; never run concurrently with gnss_tests
  or another producer from the same cwd (logs\ file locks).
- Grade every arm two ways: `python scripts/grade_ddfrac.py .` (final-hour tail)
  AND the per-hour profile (script below). The verdict metric is the per-hour
  median DD step: "CURED" = hr 4-5 stay within ~2× of hr 3; "UNCHANGED" =
  hr 5 ≥ ~50; anything between = PARTIAL (report, don't interpret).
- Per-hour script (adjacent-epoch steps, 60 s):
  see scripts/grade_ddfrac.py for the step convention; group |Δdd_cyc| of
  epoch-adjacent samples by hour((tow−t0)/3600) per
  (cons,signal,sat_i,ref_sat,sta_a,sta_b) series; report the per-hour median.
- Base config for every arm: `config/test_suite/_eq34_carryinv_6h.json`
  (carry_invariant + q_z=1e-4). 6h window covers the event (hr ~3.9) + 2h tail;
  ~10 min per run. Copy to a new config per arm; NEVER edit the base in place.

## The ladder (run in this order; STOP EARLY on the first CURED)
1. **D1 — Card-2 code time-link off**: `code_datum_time_linked: false`.
   Hypothesis: pinning the code-datum CHANGE across the satellite-set
   transition re-levels against a re-realized W6 frame.
2. **D2 — clock carry loosened**: `carry_coupling_time_update: false` (the 1d
   κ time-update off; D2 clock rows go stale-raw). If that alone is ambiguous,
   a second sub-arm with the shared clock carry weakened via its floor knob
   (`shared_carry_var_floor_pb_m2` is PHASE — do NOT touch; use the clock-kind
   knob if present, else report "no clock-kind lever" and move on).
3. **D3 — SH-VTEC time-link loosened**: raise the SH treated-pin σ_rep (find
   the knob in the config's iono section; report name + old/new). Hypothesis:
   the SH coefficient carry is the stiff channel the event poisons (a00
   diverges in EVERY failing arm — cause vs symptom undecided).
4. **D4 — observability-gate masking**: the ON arm withholds 0 epochs where
   the control withholds 5 (the DD priors make deficient-geometry epochs pass
   the gate; those may be exactly the event epochs). Probe: raise the gate
   strictness knob (report name) so the event epochs withhold again under ON.
5. **D5 — combination of the first CURED arm + all-else-default**, 12h window
   (`_eq34_carryinv_12h.json` variant), to confirm the cure holds to hr 12.

## Stop-and-report conditions (do NOT improvise past these)
- An arm fails to run (crash/exit≠0): capture the log tail, STOP the ladder.
- Two arms both come back PARTIAL: STOP — interaction effects need Fable.
- All of D1-D4 UNCHANGED: STOP — the carrier is outside the probed channels
  (report; Fable takes over with trace-level forensics).
- Any arm's a00 goes physical while dd-step stays ≥50: report as its own
  category (iono cured, steps not — decouples the two symptoms).

## Report format (one table + one paragraph per arm)
| arm | knob (old→new) | hr3 | hr4 | hr5 | a00 tail | verdict |
Plus the run-end Card-4 counter line per arm. No interpretation beyond the
verdict labels — the adjudication is Fable's.
