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

## RESULTS (Opus, 2026-07-12; Release `gnss_processor_opus_run.exe`, base `_eq34_carryinv_6h.json`)

Per-hour median DD step (cyc/ep), datum-invariant (`scripts/grade_ddfrac.py` +
per-hour bucketing). `a00@hr4` = the SH degree-0 spike at the event (the
diagnostic quantity; the `grade_ddfrac` final-hour "a00 tail" is post-event
noise, ±hundreds, so the hr4 spike is reported instead). Reference (the
known-failing ON arm) reproduced by every inert arm: hr3≈1.29, hr4≈54, hr5≈118.

| arm | knob (old→new) | hr3 | hr4 | hr5 | a00@hr4 (TECU) | verdict |
|---|---|---|---|---|---|---|
| D1 | `code_datum_time_linked` (true→false) | 1.29 | 54.3 | 120.4 | 4582 | UNCHANGED |
| D2 | `carry_coupling_time_update` (true→false) | 1.29 | 54.3 | 118.3 | 4598 | UNCHANGED |
| D3 | `global_vtec_sh.representation_sigma_tecu` (5→50) | — | — | — | — | MALFORMED — 0 epochs (broke iono observability) |
| D3b | `global_vtec_sh.q_per_degree` (0.01→1.0, SH coeff carry ×100) | 1.29 | 58.3 | 118.5 | 4143 | UNCHANGED |
| D4 | `raim_within_epoch`=true + `raim_withhold_sigma0`=3.0 | 1.29 | 54.3 | 118.3 | 4598 | UNCHANGED — σ̂0 ceiling withheld 0 epochs |
| D3r10 | `representation_sigma_tecu` (5→10) | 1.32 | 51.0 | 119.3 | 4585 | UNCHANGED |
| D3r20 | `representation_sigma_tecu` (5→20) | 1.33 | 50.3 | 120.0 | 3681 | UNCHANGED — 1 epoch withheld (approaching the σ_rep=50 obs break) |

Run-end Card-4 counter line — **IDENTICAL for every emitting arm** (D3r20: 359
ep/1 withheld, else identical): `360 epoch(s) driven, 944 carried DD(s) at run
end; 4672 chord birth(s), 7 marginalization(s), 997 slip refloat(s), 0
unresolvable-cycle drop(s), 989 tree swap(s), 1 component merge(s), 1 split(s);
q_z=1e-4`. Emit re-base (phase D, D1 arm): `360 emit ep, 7628 component re-base(s),
267324647 integer cycle(s) / 111675 key(s), 74 reference-path slip step(s)
preserved, mean|δ|=0.039 m`.

### Per-arm (facts only)
- **D1** UNCHANGED. hr4/hr5 = 54.3/120.4, identical to the reference to 2 dp; the
  code-datum time-link is inert at the cliff.
- **D2** UNCHANGED. 54.3/118.3, identical to reference; the 1d κ clock-carry
  time-update is inert at the cliff. (No clock-kind carry-floor knob exists to run
  the contemplated second sub-arm — reported per §2.)
- **D3 (σ_rep=50)** MALFORMED. 0 epochs emitted — every epoch withheld by the
  NetworkSbasis null-space gate (raising `representation_sigma_tecu` 10× = 100×
  variance dropped the SH↔slant iono constraint below the observability floor).
  Retried valid at 10/20 (below).
- **D3b (SH carry ×100)** UNCHANGED. 58.3/118.5; **a00 still diverges to 4143 TECU
  at hr4 with the SH coefficient process noise loosened 100×** ⇒ the a00 blow-up is
  a SYMPTOM, not a stiff-carry cause.
- **D4 (σ̂0 ceiling)** UNCHANGED. 54.3/118.3, and the σ̂0 ceiling (3.0) withheld
  **0 epochs** — the event's within-epoch σ̂0 never exceeds 3 because the misfit is
  absorbed by the ambiguities, keeping residuals low. NOTE: the null-space geometry
  gate tolerance is a compile constant (`kNullSpaceTol`, no config lever), and the
  ON trace shows FULL geometry at hr4 (n_present=50, n_phase_bias≈317,
  declared_dim=2, gate_ok=100%, solved=100%) — the D4 geometry-deficiency premise is
  not visible under ON, so the available epoch-withhold lever (the σ̂0 contamination
  ceiling) was probed instead; it is structurally blind to this event.
- **D3r10 / D3r20 (valid σ_rep 10 / 20)** UNCHANGED. hr4 = 51.0 / 50.3 — the σ_rep
  lever moves the cliff only ~7% across its whole usable range (obs breaks above
  ~20), never approaching CURED.

### Verdict + stop
**All D1–D4 UNCHANGED ⇒ stop condition "the carrier is outside the probed channels"
fires.** STOP the ladder; Fable takes over with trace-level forensics. Probed and
exonerated: code time-link, clock κ-carry, SH coefficient carry, σ̂0 contamination
gate, SH representation weight. The Card-4 carry itself is exonerated too — its
counters are byte-identical across every arm and healthy (944 carried DDs, 997 slip
refloats); the corruption is in the per-epoch iono↔ambiguity SOLVE, not the carry.

**Strongest lead for Fable (mechanism marker, producer's own log, TOW 273780 /
04:03 UTC):** `Network (SH-iono): a₀₀ = -13.7 TECU OUTSIDE physical band [2.0,
150.0] — global VTEC field diverging; the SH↔slant misfit is being squeezed into
the ambiguities.` The DD detonation and the a00 divergence are the SAME hr4 event:
at 04:00 UTC the degree-4 SH VTEC field cannot represent the true ionosphere, and
the misfit routes into the float ambiguities (hence the datum-invariant DD steps
blow up). It resists BOTH loosening the SH (carry q ×100, representation σ ×2–4)
and gating (σ̂0, observability) because it lives in the iono↔ambiguity separation
subspace those levers do not reach. Forensic next steps outside this ladder's
scope: SH `max_degree` sensitivity (representation resolution, not weight); the
IPP-sampling / satellite-set change at 04:00 that shifts iono observability;
per-station slant-iono residuals at the event (a single bad injector); and the
iono↔ambiguity conditioning (code-weight / GIM constraint) through hr4.
