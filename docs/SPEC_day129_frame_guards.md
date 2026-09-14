# SPEC — DOY-129 frame guards & trigger instrumentation (Opus card) — SPLIT

`docs/SPEC_day129_frame_guards.md` was split on 2026-09-14 — the original is [archive/SPEC_day129_frame_guards.md](archive/SPEC_day129_frame_guards.md); sections map as follows.

The archived copy is **frozen**: it is kept byte-for-byte as proof and is never appended to. New results go in the card file named below. See [INDEX.md](INDEX.md) for the whole tree and the rules, and [topics/raim-qc.md](topics/raim-qc.md) for this subject.

| section | card id | file |
| --- | --- | --- |
| (title + preamble) | DOY-129 anatomy | [cards/raim-qc/DAY129-FRAME-ANATOMY.md](cards/raim-qc/DAY129-FRAME-ANATOMY.md) |
| A1 Configuration facts the mechanisms hang on | DOY-129 anatomy | [cards/raim-qc/DAY129-FRAME-ANATOMY.md](cards/raim-qc/DAY129-FRAME-ANATOMY.md) |
| A2 Q2 — why the pinned pivot's code bias absorbed the damage. ANSWERED | DOY-129 anatomy | [cards/raim-qc/DAY129-FRAME-ANATOMY.md](cards/raim-qc/DAY129-FRAME-ANATOMY.md) |
| A2.1 What actually stepped at tow 334020 [M] | DOY-129 anatomy | [cards/raim-qc/DAY129-FRAME-ANATOMY.md](cards/raim-qc/DAY129-FRAME-ANATOMY.md) |
| A2.2 The mechanism | DOY-129 anatomy | [cards/raim-qc/DAY129-FRAME-ANATOMY.md](cards/raim-qc/DAY129-FRAME-ANATOMY.md) |
| A2.3 The arithmetic closes on both sides [M] | DOY-129 anatomy | [cards/raim-qc/DAY129-FRAME-ANATOMY.md](cards/raim-qc/DAY129-FRAME-ANATOMY.md) |
| A3 Q1 — the a00 collapse. Anatomy established; trigger needs one trace | DOY-129 anatomy | [cards/raim-qc/DAY129-FRAME-ANATOMY.md](cards/raim-qc/DAY129-FRAME-ANATOMY.md) |
| A3.1 What the artifacts rule in and out [M] | DOY-129 anatomy | [cards/raim-qc/DAY129-FRAME-ANATOMY.md](cards/raim-qc/DAY129-FRAME-ANATOMY.md) |
| A3.2 The enabler, and the trigger hypothesis [H] | DOY-129 anatomy | [cards/raim-qc/DAY129-FRAME-ANATOMY.md](cards/raim-qc/DAY129-FRAME-ANATOMY.md) |
| A4 Q3 — the quiet-hour wander IS satellite-clock wander. ANSWERED | DOY-129 anatomy | [cards/raim-qc/DAY129-FRAME-ANATOMY.md](cards/raim-qc/DAY129-FRAME-ANATOMY.md) |
| A5 Q4 — the disc counter is a DATUM channel, not a fault channel. ANSWERED | DOY-129 anatomy | [cards/raim-qc/DAY129-FRAME-ANATOMY.md](cards/raim-qc/DAY129-FRAME-ANATOMY.md) |
| A6 The datum-design finding — the IF pin is on the wrong pair (D3) | DOY-129 anatomy | [cards/raim-qc/DAY129-FRAME-ANATOMY.md](cards/raim-qc/DAY129-FRAME-ANATOMY.md) |
| A7 The investigation's delta over the handoff — now Part B | DOY-129 anatomy | [cards/raim-qc/DAY129-FRAME-ANATOMY.md](cards/raim-qc/DAY129-FRAME-ANATOMY.md) |
| Phase 0 — Q5 closed-loop replay (no new code, run-only, ~minutes/arm) | DOY-129 phases | [cards/raim-qc/DAY129-PHASES.md](cards/raim-qc/DAY129-PHASES.md) |
| Phase 1 — trigger instrumentation (code; trace-only; fingerprint-SAFE; then one 3–4 h leg) | DOY-129 phases | [cards/raim-qc/DAY129-PHASES.md](cards/raim-qc/DAY129-PHASES.md) |
| Phase 2 — guards (code; default-OFF; needs a FRESH convergence to arm) — GATED | DOY-129 phases | [cards/raim-qc/DAY129-PHASES.md](cards/raim-qc/DAY129-PHASES.md) |
| Standing-invariant pre-flight (this card) | DOY-129 phases | [cards/raim-qc/DAY129-PHASES.md](cards/raim-qc/DAY129-PHASES.md) |
| Decisions needed from the user before Phase 2 (Phases 0–1 need none) | DOY-129 phases | [cards/raim-qc/DAY129-PHASES.md](cards/raim-qc/DAY129-PHASES.md) |
| Practicalities | DOY-129 phases | [cards/raim-qc/DAY129-PHASES.md](cards/raim-qc/DAY129-PHASES.md) |
| C1 What the leg shipped — the disposition census | DOY-129 defects | [cards/raim-qc/DAY129-DEFECTS-ABC.md](cards/raim-qc/DAY129-DEFECTS-ABC.md) |
| C2 DEFECT A — the seam engagement was CORRECT. The field really was diverged. | DOY-129 defects | [cards/raim-qc/DAY129-DEFECTS-ABC.md](cards/raim-qc/DAY129-DEFECTS-ABC.md) |
| C3 DEFECT B — a₀₀ is the WRONG STATISTIC; the late chatter is that transient's tail | DOY-129 defects | [cards/raim-qc/DAY129-DEFECTS-ABC.md](cards/raim-qc/DAY129-DEFECTS-ABC.md) |
| C4 DEFECT C — the CB detector's resume suppression is ONE epoch; the shock is ~10+ | DOY-129 defects | [cards/raim-qc/DAY129-DEFECTS-ABC.md](cards/raim-qc/DAY129-DEFECTS-ABC.md) |
| C5 The FIVE wiring facts any policy must respect | DOY-129 defects | [cards/raim-qc/DAY129-DEFECTS-ABC.md](cards/raim-qc/DAY129-DEFECTS-ABC.md) |
| C6 Policy counterfactuals, measured on BOTH runs | DOY-129 defects | [cards/raim-qc/DAY129-DEFECTS-ABC.md](cards/raim-qc/DAY129-DEFECTS-ABC.md) |
| D-G1 — resume grace: DEGRADE, DO NOT WITHHOLD | DEC-day129-gate-policy | [decisions/DEC-day129-gate-policy.md](decisions/DEC-day129-gate-policy.md) |
| The seven MUSTs | DEC-day129-gate-policy | [decisions/DEC-day129-gate-policy.md](decisions/DEC-day129-gate-policy.md) |
| The D-G2 cap decision table | DEC-day129-gate-policy | [decisions/DEC-day129-gate-policy.md](decisions/DEC-day129-gate-policy.md) |
| D-G3 — the statistic: `rms_ho` RATIO, asymmetry, hysteresis | DEC-day129-gate-policy | [decisions/DEC-day129-gate-policy.md](decisions/DEC-day129-gate-policy.md) |
| The five MUSTs | DEC-day129-gate-policy | [decisions/DEC-day129-gate-policy.md](decisions/DEC-day129-gate-policy.md) |
| CANDIDATE numbers (derived below) | DEC-day129-gate-policy | [decisions/DEC-day129-gate-policy.md](decisions/DEC-day129-gate-policy.md) |
| ⚠ Three knob traps the ruling creates — MUST, all three | DEC-day129-gate-policy | [decisions/DEC-day129-gate-policy.md](decisions/DEC-day129-gate-policy.md) |
| The empirical derivation — and the amendment it forces | DEC-day129-gate-policy | [decisions/DEC-day129-gate-policy.md](decisions/DEC-day129-gate-policy.md) |
| D-G5 — the CB-excursion detector's resume window: STATE-COUPLED | DEC-day129-gate-policy | [decisions/DEC-day129-gate-policy.md](decisions/DEC-day129-gate-policy.md) |
| The six MUSTs | DEC-day129-gate-policy | [decisions/DEC-day129-gate-policy.md](decisions/DEC-day129-gate-policy.md) |
| E1 Validation gates — all MUST | DEC-day129-gate-policy | [decisions/DEC-day129-gate-policy.md](decisions/DEC-day129-gate-policy.md) |
| E2 Standing-invariant pre-flight (the policy) | DEC-day129-gate-policy | [decisions/DEC-day129-gate-policy.md](decisions/DEC-day129-gate-policy.md) |
| E3 Implementation order for the owning session | DEC-day129-gate-policy | [decisions/DEC-day129-gate-policy.md](decisions/DEC-day129-gate-policy.md) |
| E4 ⚠ The UN-DERIVED prerequisite — the grace floor MAGNITUDE | DEC-day129-gate-policy | [decisions/DEC-day129-gate-policy.md](decisions/DEC-day129-gate-policy.md) |
