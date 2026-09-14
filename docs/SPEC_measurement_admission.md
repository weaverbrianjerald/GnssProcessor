# SPEC — product-referenced measurement admission (the corrupt-channel screen) — SPLIT

`docs/SPEC_measurement_admission.md` was split on 2026-09-14 — the original is [archive/SPEC_measurement_admission.md](archive/SPEC_measurement_admission.md); sections map as follows.

The archived copy is **frozen**: it is kept byte-for-byte as proof and is never appended to. New results go in the card file named below. See [INDEX.md](INDEX.md) for the whole tree and the rules, and [topics/raim-qc.md](topics/raim-qc.md) for this subject.

| section | card id | file |
| --- | --- | --- |
| (title + preamble) | meas admission | [cards/raim-qc/MEAS-ADMISSION.md](cards/raim-qc/MEAS-ADMISSION.md) |
| 1. Problem — the class no differential screen can see | meas admission | [cards/raim-qc/MEAS-ADMISSION.md](cards/raim-qc/MEAS-ADMISSION.md) |
| 2. The screen statistic — absolute, coarse, clock-free | meas admission | [cards/raim-qc/MEAS-ADMISSION.md](cards/raim-qc/MEAS-ADMISSION.md) |
| 3. Design — three increments | meas admission | [cards/raim-qc/MEAS-ADMISSION.md](cards/raim-qc/MEAS-ADMISSION.md) |
| INC-1: admission screen at arc birth and warm-resume restore | meas admission | [cards/raim-qc/MEAS-ADMISSION.md](cards/raim-qc/MEAS-ADMISSION.md) |
| INC-2: mid-arc onset + heal | meas admission | [cards/raim-qc/MEAS-ADMISSION.md](cards/raim-qc/MEAS-ADMISSION.md) |
| INC-3: absolute-residual census (the missing scan modality, made permanent) | meas admission | [cards/raim-qc/MEAS-ADMISSION.md](cards/raim-qc/MEAS-ADMISSION.md) |
| 4. Wiring sites (verify at implementation) | meas admission | [cards/raim-qc/MEAS-ADMISSION.md](cards/raim-qc/MEAS-ADMISSION.md) |
| 5. Knobs | meas admission | [cards/raim-qc/MEAS-ADMISSION.md](cards/raim-qc/MEAS-ADMISSION.md) |
| 6. Unit tests | meas admission | [cards/raim-qc/MEAS-ADMISSION.md](cards/raim-qc/MEAS-ADMISSION.md) |
| 7. Field validation — KNOWN-ANSWER gates (fixtures on disk) | meas admission | [cards/raim-qc/MEAS-ADMISSION.md](cards/raim-qc/MEAS-ADMISSION.md) |
| 8. Deferral triage (pre-declared) | meas admission | [cards/raim-qc/MEAS-ADMISSION.md](cards/raim-qc/MEAS-ADMISSION.md) |
| 9. Implementation record (2026-08-01, Opus session) | meas admission impl | [cards/raim-qc/MEAS-ADMISSION-IMPL.md](cards/raim-qc/MEAS-ADMISSION-IMPL.md) |
| 9.0b The centre is a median over SATELLITES, not over observables | meas admission impl | [cards/raim-qc/MEAS-ADMISSION-IMPL.md](cards/raim-qc/MEAS-ADMISSION-IMPL.md) |
| 9.1 The statistic — implemented as `observed − modeled`, not as a product re-query | meas admission impl | [cards/raim-qc/MEAS-ADMISSION-IMPL.md](cards/raim-qc/MEAS-ADMISSION-IMPL.md) |
| 9.2 Seam placement — AFTER `networkSignalIntegrity`, not before | meas admission impl | [cards/raim-qc/MEAS-ADMISSION-IMPL.md](cards/raim-qc/MEAS-ADMISSION-IMPL.md) |
| 9.3 Wider orphan-state prune than §4 specifies | meas admission impl | [cards/raim-qc/MEAS-ADMISSION-IMPL.md](cards/raim-qc/MEAS-ADMISSION-IMPL.md) |
| 9.4 Station-guard reading of "hold all" | meas admission impl | [cards/raim-qc/MEAS-ADMISSION-IMPL.md](cards/raim-qc/MEAS-ADMISSION-IMPL.md) |
| 9.5 Refloat scope: UNCONDITIONAL, and the resume latch is per station | meas admission impl | [cards/raim-qc/MEAS-ADMISSION-IMPL.md](cards/raim-qc/MEAS-ADMISSION-IMPL.md) |
| 9.5b The station guard counts SATELLITES, not bands | meas admission impl | [cards/raim-qc/MEAS-ADMISSION-IMPL.md](cards/raim-qc/MEAS-ADMISSION-IMPL.md) |
| 9.6 INC-3's two columns | meas admission impl | [cards/raim-qc/MEAS-ADMISSION-IMPL.md](cards/raim-qc/MEAS-ADMISSION-IMPL.md) |
| 9.7 Byte-identity: the two census columns are CONDITIONAL | meas admission impl | [cards/raim-qc/MEAS-ADMISSION-IMPL.md](cards/raim-qc/MEAS-ADMISSION-IMPL.md) |
| 9.8 Not fingerprinted | meas admission impl | [cards/raim-qc/MEAS-ADMISSION-IMPL.md](cards/raim-qc/MEAS-ADMISSION-IMPL.md) |
| 9.9 Also landed | meas admission impl | [cards/raim-qc/MEAS-ADMISSION-IMPL.md](cards/raim-qc/MEAS-ADMISSION-IMPL.md) |
| 9.10 Known gaps in this implementation | meas admission impl | [cards/raim-qc/MEAS-ADMISSION-IMPL.md](cards/raim-qc/MEAS-ADMISSION-IMPL.md) |
| 9.11 §7 gate 1 — smoke, AS RUN | meas admission impl | [cards/raim-qc/MEAS-ADMISSION-IMPL.md](cards/raim-qc/MEAS-ADMISSION-IMPL.md) |
| 9.12 §7 gate 1b — the OFF byte-identity gate CANNOT PASS in this tree | meas admission impl | [cards/raim-qc/MEAS-ADMISSION-IMPL.md](cards/raim-qc/MEAS-ADMISSION-IMPL.md) |
| 9.13 §7 gate 2 — the KNOWN-ANSWER birth gate, AS RUN: **PASS** | meas admission impl | [cards/raim-qc/MEAS-ADMISSION-IMPL.md](cards/raim-qc/MEAS-ADMISSION-IMPL.md) |
| 9.13b §7 gate 3 — THE BREAK GATE: **PASS**, and it closes the `dwrat` hunt | meas admission impl | [cards/raim-qc/MEAS-ADMISSION-IMPL.md](cards/raim-qc/MEAS-ADMISSION-IMPL.md) |
| 9.13c §7 gate 4 — the bound sweep: **PASS** | meas admission impl | [cards/raim-qc/MEAS-ADMISSION-IMPL.md](cards/raim-qc/MEAS-ADMISSION-IMPL.md) |
| 9.14 §7 gates 5–7 — status at hand-off | meas admission impl | [cards/raim-qc/MEAS-ADMISSION-IMPL.md](cards/raim-qc/MEAS-ADMISSION-IMPL.md) |
| 9.15 Adversarial review record (2026-08-01) | meas admission impl | [cards/raim-qc/MEAS-ADMISSION-IMPL.md](cards/raim-qc/MEAS-ADMISSION-IMPL.md) |
| 10. INC-4 — the arc RE-ENTRY vet (2026-08-02, Opus session) | INC-4 | [cards/raim-qc/MEAS-ADMISSION-INC4-REENTRY.md](cards/raim-qc/MEAS-ADMISSION-INC4-REENTRY.md) |
| 10.1 The finding — there were no mid-arc onsets | INC-4 | [cards/raim-qc/MEAS-ADMISSION-INC4-REENTRY.md](cards/raim-qc/MEAS-ADMISSION-INC4-REENTRY.md) |
| 10.2 Design | INC-4 | [cards/raim-qc/MEAS-ADMISSION-INC4-REENTRY.md](cards/raim-qc/MEAS-ADMISSION-INC4-REENTRY.md) |
| 10.3 Deviations and things worth knowing | INC-4 | [cards/raim-qc/MEAS-ADMISSION-INC4-REENTRY.md](cards/raim-qc/MEAS-ADMISSION-INC4-REENTRY.md) |
| 10.4 What this increment does NOT do — stated, not glossed | INC-4 | [cards/raim-qc/MEAS-ADMISSION-INC4-REENTRY.md](cards/raim-qc/MEAS-ADMISSION-INC4-REENTRY.md) |
| 10.5 Gate 1 — smoke + OFF byte-identity, AS RUN: **PASS** | INC-4 | [cards/raim-qc/MEAS-ADMISSION-INC4-REENTRY.md](cards/raim-qc/MEAS-ADMISSION-INC4-REENTRY.md) |
| 10.6 Gate 2 — the class-B known-answer legs | INC-4 | [cards/raim-qc/MEAS-ADMISSION-INC4-REENTRY.md](cards/raim-qc/MEAS-ADMISSION-INC4-REENTRY.md) |
| 10.7 Gate 2 result — the census trigger fires, and it fires UNDER the bound | INC-4 | [cards/raim-qc/MEAS-ADMISSION-INC4-REENTRY.md](cards/raim-qc/MEAS-ADMISSION-INC4-REENTRY.md) |
| 10.7b …but it does NOT close the class where the bound ties | INC-4 | [cards/raim-qc/MEAS-ADMISSION-INC4-REENTRY.md](cards/raim-qc/MEAS-ADMISSION-INC4-REENTRY.md) |
| 10.8 The decisive arm — `adm_revet_hold` erases the event: **PASS** | INC-4 | [cards/raim-qc/MEAS-ADMISSION-INC4-REENTRY.md](cards/raim-qc/MEAS-ADMISSION-INC4-REENTRY.md) |
| 10.11 Gate 3 — the tighter vet bound, AS RUN: **PASS** | INC-4 | [cards/raim-qc/MEAS-ADMISSION-INC4-REENTRY.md](cards/raim-qc/MEAS-ADMISSION-INC4-REENTRY.md) |
| 10.9 Status at hand-off | INC-4 | [cards/raim-qc/MEAS-ADMISSION-INC4-REENTRY.md](cards/raim-qc/MEAS-ADMISSION-INC4-REENTRY.md) |
| 10.10 The re-entry \|z\| measurement (§10.4's owed run): **DONE** | INC-4 | [cards/raim-qc/MEAS-ADMISSION-INC4-REENTRY.md](cards/raim-qc/MEAS-ADMISSION-INC4-REENTRY.md) |
