# The EVENT HARNESS — SPLIT

`docs/SPEC_event_harness.md` was split on 2026-09-14 — the original is [archive/SPEC_event_harness.md](archive/SPEC_event_harness.md); sections map as follows.

The archived copy is **frozen**: it is kept byte-for-byte as proof and is never appended to. New results go in the card file named below. See [INDEX.md](INDEX.md) for the whole tree and the rules, and [topics/harness.md](topics/harness.md) for this subject.

| section | card id | file |
| --- | --- | --- |
| (title + preamble) | HARNESS | [cards/harness/HARNESS-REF.md](cards/harness/HARNESS-REF.md) |
| 1. What it is | HARNESS | [cards/harness/HARNESS-REF.md](cards/harness/HARNESS-REF.md) |
| 2. The six injectable events | HARNESS | [cards/harness/HARNESS-REF.md](cards/harness/HARNESS-REF.md) |
| 2.1 Where the injection acts, and why it is one site | HARNESS | [cards/harness/HARNESS-REF.md](cards/harness/HARNESS-REF.md) |
| 2.2 Never-fired is a HARD error | HARNESS | [cards/harness/HARNESS-REF.md](cards/harness/HARNESS-REF.md) |
| 3. Files | HARNESS | [cards/harness/HARNESS-REF.md](cards/harness/HARNESS-REF.md) |
| 4. The leg recipe | HARNESS | [cards/harness/HARNESS-REF.md](cards/harness/HARNESS-REF.md) |
| 5. Reading the grader | HARNESS | [cards/harness/HARNESS-REF.md](cards/harness/HARNESS-REF.md) |
| 6. Validation, 2026-09-10 | HARNESS | [cards/harness/HARNESS-REF.md](cards/harness/HARNESS-REF.md) |
| (1) The seam transient — and why grading is event-minus-null | HARNESS | [cards/harness/HARNESS-REF.md](cards/harness/HARNESS-REF.md) |
| (2) `slip`, 10 cycles on S21000SIM/G10/L1C | HARNESS | [cards/harness/HARNESS-REF.md](cards/harness/HARNESS-REF.md) |
| (3) `arc_erase` on the SAME arc — the re-level treadmill, priced | HARNESS | [cards/harness/HARNESS-REF.md](cards/harness/HARNESS-REF.md) |
| (4) `anchor_handover` on G10 — cold vs continuous, 50x | HARNESS | [cards/harness/HARNESS-REF.md](cards/harness/HARNESS-REF.md) |
| (5) `outage`, 20 epochs on S21000SIM/G10 | HARNESS | [cards/harness/HARNESS-REF.md](cards/harness/HARNESS-REF.md) |
| (6) `sat_clock_step`, +0.5 m on G10 | HARNESS | [cards/harness/HARNESS-REF.md](cards/harness/HARNESS-REF.md) |
| Byte-identity and the suite | HARNESS | [cards/harness/HARNESS-REF.md](cards/harness/HARNESS-REF.md) |
| 6b. A defect the harness found on its first day — FIXED, §11.1 | HARNESS | [cards/harness/HARNESS-REF.md](cards/harness/HARNESS-REF.md) |
| 7. Byte-identity | HARNESS | [cards/harness/HARNESS-REF.md](cards/harness/HARNESS-REF.md) |
| 8. Known gaps — what is NOT injectable | HARNESS | [cards/harness/HARNESS-REF.md](cards/harness/HARNESS-REF.md) |
| 9. Study 1 — erase vs refloat: what actually costs the 0.45-cycle floor | Study 1 | [cards/harness/STUDY-1-erase-vs-refloat.md](cards/harness/STUDY-1-erase-vs-refloat.md) |
| 9.1 The question | Study 1 | [cards/harness/STUDY-1-erase-vs-refloat.md](cards/harness/STUDY-1-erase-vs-refloat.md) |
| 9.2 The exact difference list (read from the source, not inferred) | Study 1 | [cards/harness/STUDY-1-erase-vs-refloat.md](cards/harness/STUDY-1-erase-vs-refloat.md) |
| 9.3 The diagnostic sub-knobs (harness-only) | Study 1 | [cards/harness/STUDY-1-erase-vs-refloat.md](cards/harness/STUDY-1-erase-vs-refloat.md) |
| 9.4 The legs | Study 1 | [cards/harness/STUDY-1-erase-vs-refloat.md](cards/harness/STUDY-1-erase-vs-refloat.md) |
| 9.5 The table | Study 1 | [cards/harness/STUDY-1-erase-vs-refloat.md](cards/harness/STUDY-1-erase-vs-refloat.md) |
| 9.6 Verdict | Study 1 | [cards/harness/STUDY-1-erase-vs-refloat.md](cards/harness/STUDY-1-erase-vs-refloat.md) |
| 9.7 Reproducing | Study 1 | [cards/harness/STUDY-1-erase-vs-refloat.md](cards/harness/STUDY-1-erase-vs-refloat.md) |
| 10. Study 2 — the receiver-clock carry under single events | Study 2 | [cards/harness/STUDY-2-rx-clock-carry.md](cards/harness/STUDY-2-rx-clock-carry.md) |
| 10.1 The knob is FINGERPRINTED, and that decided the design | Study 2 | [cards/harness/STUDY-2-rx-clock-carry.md](cards/harness/STUDY-2-rx-clock-carry.md) |
| 10.2 Design | Study 2 | [cards/harness/STUDY-2-rx-clock-carry.md](cards/harness/STUDY-2-rx-clock-carry.md) |
| 10.3 The OFF arm first — the events are NOT alike, and the big one is not a ρ error | Study 2 | [cards/harness/STUDY-2-rx-clock-carry.md](cards/harness/STUDY-2-rx-clock-carry.md) |
| 10.4 The table — {event} × {OFF, ON} | Study 2 | [cards/harness/STUDY-2-rx-clock-carry.md](cards/harness/STUDY-2-rx-clock-carry.md) |
| 10.5 The mechanism, and where the card would actually bite | Study 2 | [cards/harness/STUDY-2-rx-clock-carry.md](cards/harness/STUDY-2-rx-clock-carry.md) |
| 10.6 The extra rung — `phase_leveling_young_inflate_var_m2` on one erase | Study 2 | [cards/harness/STUDY-2-rx-clock-carry.md](cards/harness/STUDY-2-rx-clock-carry.md) |
| 10.7 Byte-identity and the suite (branch `worktree-ionoblk`) | Study 2 | [cards/harness/STUDY-2-rx-clock-carry.md](cards/harness/STUDY-2-rx-clock-carry.md) |
| 10.8 Reproducing | Study 2 | [cards/harness/STUDY-2-rx-clock-carry.md](cards/harness/STUDY-2-rx-clock-carry.md) |
| 11. Cards A and B — the `require` hard-fail, and the receiver-clock step | Cards A+B | [cards/harness/CARD-AB-require-and-rx-clock-step.md](cards/harness/CARD-AB-require-and-rx-clock-step.md) |
| 11.1 Card A — `network.state_snapshot_require` now MEANS refuse | Cards A+B | [cards/harness/CARD-AB-require-and-rx-clock-step.md](cards/harness/CARD-AB-require-and-rx-clock-step.md) |
| 11.2 Card B — `rx_clock_step`, the dual of `sat_clock_step` | Cards A+B | [cards/harness/CARD-AB-require-and-rx-clock-step.md](cards/harness/CARD-AB-require-and-rx-clock-step.md) |
| 11.3 A new instrument: the jump gate in metres | Cards A+B | [cards/harness/CARD-AB-require-and-rx-clock-step.md](cards/harness/CARD-AB-require-and-rx-clock-step.md) |
| 11.4 Study 2b — PRE-REGISTRATION (written before any ON leg was run) | Cards A+B | [cards/harness/CARD-AB-require-and-rx-clock-step.md](cards/harness/CARD-AB-require-and-rx-clock-step.md) |
| 11.5 The gate census refuted half the pre-registration before the ON legs were graded | Cards A+B | [cards/harness/CARD-AB-require-and-rx-clock-step.md](cards/harness/CARD-AB-require-and-rx-clock-step.md) |
| 11.6 The table | Cards A+B | [cards/harness/CARD-AB-require-and-rx-clock-step.md](cards/harness/CARD-AB-require-and-rx-clock-step.md) |
| 11.7 Verdict | Cards A+B | [cards/harness/CARD-AB-require-and-rx-clock-step.md](cards/harness/CARD-AB-require-and-rx-clock-step.md) |
| 11.8 Byte identity, the suite, and reproducing | Cards A+B | [cards/harness/CARD-AB-require-and-rx-clock-step.md](cards/harness/CARD-AB-require-and-rx-clock-step.md) |
| Z. Study 3 — band-scoped responses, the production knobs | Study 3 | [cards/slip/STUDY-3-band-scoped-responses.md](cards/slip/STUDY-3-band-scoped-responses.md) |
| Z.1 What was built | Study 3 | [cards/slip/STUDY-3-band-scoped-responses.md](cards/slip/STUDY-3-band-scoped-responses.md) |
| Z.2 The defect the harness found on its first ON leg: the invariant is per EPOCH | Study 3 | [cards/slip/STUDY-3-band-scoped-responses.md](cards/slip/STUDY-3-band-scoped-responses.md) |
| Z.3 The legs | Study 3 | [cards/slip/STUDY-3-band-scoped-responses.md](cards/slip/STUDY-3-band-scoped-responses.md) |
| Z.4 Verdict | Study 3 | [cards/slip/STUDY-3-band-scoped-responses.md](cards/slip/STUDY-3-band-scoped-responses.md) |
| Z.6 Storm-bed validation — the combined arm FAILS, and Z.4 said which half | Study 3 | [cards/slip/STUDY-3-band-scoped-responses.md](cards/slip/STUDY-3-band-scoped-responses.md) |
| Z.5 Reproducing | Study 3 | [cards/slip/STUDY-3-band-scoped-responses.md](cards/slip/STUDY-3-band-scoped-responses.md) |
| Z.7 The T4-only storm arm — the cure is falsified at storm scale, the mechanism is not | Study 3 | [cards/slip/STUDY-3-band-scoped-responses.md](cards/slip/STUDY-3-band-scoped-responses.md) |
| 12. Study 4 — the slip DOSE: does a CONVERGED network survive the campaign's slip rate? | Study 4 | [cards/slip/STUDY-4-slip-dose.md](cards/slip/STUDY-4-slip-dose.md) |
| 12.0 Why this study replaced "lane-evidence band choice" | Study 4 | [cards/slip/STUDY-4-slip-dose.md](cards/slip/STUDY-4-slip-dose.md) |
| 12.1 Mechanics | Study 4 | [cards/slip/STUDY-4-slip-dose.md](cards/slip/STUDY-4-slip-dose.md) |
| 12.2 Hypotheses and gates (frozen) | Study 4 | [cards/slip/STUDY-4-slip-dose.md](cards/slip/STUDY-4-slip-dose.md) |
| 12.3 First pass (2026-09-10 evening, hand-graded with the trace-complete channels; the dose g… | Study 4 | [cards/slip/STUDY-4-slip-dose.md](cards/slip/STUDY-4-slip-dose.md) |
| 12.4 The twins and the density ladder (2026-09-10 evening, `scripts/event_dose_grade.py` + th… | Study 4 | [cards/slip/STUDY-4-slip-dose.md](cards/slip/STUDY-4-slip-dose.md) |
| 12.5 The band-scope twin FAILS, and the datum-station timeline names the engine (2026-09-10, … | Study 4 | [cards/slip/STUDY-4-slip-dose.md](cards/slip/STUDY-4-slip-dose.md) |
| 12.6 The gauge is the engine — both gauges (2026-09-10, evening local) | Study 4 | [cards/slip/STUDY-4-slip-dose.md](cards/slip/STUDY-4-slip-dose.md) |
| 12.7 The cure, validated on the harness before it exists in production (2026-09-10, evening l… | Study 4 | [cards/slip/STUDY-4-slip-dose.md](cards/slip/STUDY-4-slip-dose.md) |
| 12.8 Two more twins (2026-09-10, evening local) | Study 4 | [cards/slip/STUDY-4-slip-dose.md](cards/slip/STUDY-4-slip-dose.md) |
| 12.9 Pre-registration — the datum half of the cure (written before any leg on the new seed wa… | Study 4 | [cards/slip/STUDY-4-slip-dose.md](cards/slip/STUDY-4-slip-dose.md) |
| 12.10 The datum half is FALSIFIED — there is one gauge, and it is the phase-bias anchor (2026… | Study 4 | [cards/slip/STUDY-4-slip-dose.md](cards/slip/STUDY-4-slip-dose.md) |
| 12.11 The card on the datum station's schedule — PASS (2026-09-10, 23:15 local; worker's exe … | Study 4 | [cards/slip/STUDY-4-slip-dose.md](cards/slip/STUDY-4-slip-dose.md) |
| 12.12 The card's gates (worker report, branch `worktree-anchorslip` @ ec26de1; 2026-09-10 ~23… | Study 4 | [cards/slip/STUDY-4-slip-dose.md](cards/slip/STUDY-4-slip-dose.md) |
| 12.13 STORM-BED VALIDATION — the plateau is gone (2026-09-10, 23:30 local; `runs/golden_sim_c… | Study 4 | [cards/slip/STUDY-4-slip-dose.md](cards/slip/STUDY-4-slip-dose.md) |
| 13. Study 5 — detector coverage: the unflagged GPS L5 slip (2026-09-11) | Study 5 | [cards/slip/STUDY-5-detector-coverage-l5.md](cards/slip/STUDY-5-detector-coverage-l5.md) |
| 13.1 The question, and what was established before any new leg | Study 5 | [cards/slip/STUDY-5-detector-coverage-l5.md](cards/slip/STUDY-5-detector-coverage-l5.md) |
| 13.2 Hypotheses — frozen before the reproduction legs are read | Study 5 | [cards/slip/STUDY-5-detector-coverage-l5.md](cards/slip/STUDY-5-detector-coverage-l5.md) |
| 13.3 Pre-registered gates for the production change (whatever H is true) | Study 5 | [cards/slip/STUDY-5-detector-coverage-l5.md](cards/slip/STUDY-5-detector-coverage-l5.md) |
| 13.4 RESULT — H1, with the exact gate named (2026-09-11; legs `l5miss_s36`, `s36only_mwlog`, … | Study 5 | [cards/slip/STUDY-5-detector-coverage-l5.md](cards/slip/STUDY-5-detector-coverage-l5.md) |
| 13.5 The card's gates (worker build 0550b58 on its own worktree, merged to main as 2490087; m… | Study 5 | [cards/slip/STUDY-5-detector-coverage-l5.md](cards/slip/STUDY-5-detector-coverage-l5.md) |
| 13.6 STORM-BED VALIDATION — PASS on every bar (2026-09-11; `runs/golden_sim_campaign_zwdblk_m… | Study 5 | [cards/slip/STUDY-5-detector-coverage-l5.md](cards/slip/STUDY-5-detector-coverage-l5.md) |
| 14. Study 6 — the COMMON-BAND scope: free only the band the evidence names (2026-09-11) | Study 6 | [cards/slip/STUDY-6-common-band-scope.md](cards/slip/STUDY-6-common-band-scope.md) |
| 14.1 The question, the population, what is already known | Study 6 | [cards/slip/STUDY-6-common-band-scope.md](cards/slip/STUDY-6-common-band-scope.md) |
| 14.2 The rule (pre-registered) | Study 6 | [cards/slip/STUDY-6-common-band-scope.md](cards/slip/STUDY-6-common-band-scope.md) |
| 14.3 Pre-registered legs and gates (knob `network.cycle_slip.common_band_scope`, default OFF) | Study 6 | [cards/slip/STUDY-6-common-band-scope.md](cards/slip/STUDY-6-common-band-scope.md) |
| 15. The COLD anchor fallbacks — KEEP_OFF after TWO iterations: the rule works, the per-family… | COLD-FB | [cards/network-vehicle/COLD-FB-anchor-fallbacks.md](cards/network-vehicle/COLD-FB-anchor-fallbacks.md) |
| 15.1 The card as built — pre-registration, frozen before any leg (2026-09-11 evening; worktre… | COLD-FB | [cards/network-vehicle/COLD-FB-anchor-fallbacks.md](cards/network-vehicle/COLD-FB-anchor-fallbacks.md) |
| 15.2 Step 1 — the family census, from the seed's own traces (no runs) | COLD-FB | [cards/network-vehicle/COLD-FB-anchor-fallbacks.md](cards/network-vehicle/COLD-FB-anchor-fallbacks.md) |
| 15.3 G1 — OFF byte-identity: PASS, on both halves | COLD-FB | [cards/network-vehicle/COLD-FB-anchor-fallbacks.md](cards/network-vehicle/COLD-FB-anchor-fallbacks.md) |
| 15.4 The first finding, and it is about the HARNESS: the maturity bar is structurally inert o… | COLD-FB | [cards/network-vehicle/COLD-FB-anchor-fallbacks.md](cards/network-vehicle/COLD-FB-anchor-fallbacks.md) |
| 15.5 G2 — the census proxy named the wrong families, and the production path answered anyway | COLD-FB | [cards/network-vehicle/COLD-FB-anchor-fallbacks.md](cards/network-vehicle/COLD-FB-anchor-fallbacks.md) |
| 15.6 G3 — the storm arm: every producer channel the card targets improves, and the USER'S PRO… | COLD-FB | [cards/network-vehicle/COLD-FB-anchor-fallbacks.md](cards/network-vehicle/COLD-FB-anchor-fallbacks.md) |
| 15.7 Verdict and what is owed | COLD-FB | [cards/network-vehicle/COLD-FB-anchor-fallbacks.md](cards/network-vehicle/COLD-FB-anchor-fallbacks.md) |
| 15.8 ITERATION 2 — the SATELLITE clause: it removes every split, it fixes the slip leg, and o… | COLD-FB (iter 2–4) | [cards/network-vehicle/COLD-FB-iterations-2-4.md](cards/network-vehicle/COLD-FB-iterations-2-4.md) |
| 15.9 Verdict on iteration 2, and what iteration 3 must test | COLD-FB (iter 2–4) | [cards/network-vehicle/COLD-FB-iterations-2-4.md](cards/network-vehicle/COLD-FB-iterations-2-4.md) |
| 15.10 ITERATION 3 — the DATUM-INVARIANT consumer census: the raw median INVERTS the ranking, … | COLD-FB (iter 2–4) | [cards/network-vehicle/COLD-FB-iterations-2-4.md](cards/network-vehicle/COLD-FB-iterations-2-4.md) |
| 15.11 ITERATION 4 — the card on the NEW golden, graded on iteration 3's invariant instruments… | COLD-FB (iter 2–4) | [cards/network-vehicle/COLD-FB-iterations-2-4.md](cards/network-vehicle/COLD-FB-iterations-2-4.md) |
| 14.4 Iteration 1 — the rule is right, the evidence source was wrong (worker build 5fe64c7 on … | Study 6 | [cards/slip/STUDY-6-common-band-scope.md](cards/slip/STUDY-6-common-band-scope.md) |
| 14.5 Iteration-1 storm arm — producer better, consumer worse, and no producer-side fingerprin… | Study 6 | [cards/slip/STUDY-6-common-band-scope.md](cards/slip/STUDY-6-common-band-scope.md) |
| 14.6 Iteration 2 — silence from the detector's own decision: the rule engages (worker commit … | Study 6 | [cards/slip/STUDY-6-common-band-scope.md](cards/slip/STUDY-6-common-band-scope.md) |
| 14.7 Iteration-2 storm arm, the consumer instrument calibrated, and the outlier rise located … | Study 6 | [cards/slip/STUDY-6-common-band-scope.md](cards/slip/STUDY-6-common-band-scope.md) |
| 14.8 Iteration 3 — the anchor clause puts the references back together (worker commit 5223dc6… | Study 6 | [cards/slip/STUDY-6-common-band-scope.md](cards/slip/STUDY-6-common-band-scope.md) |
| 14.9 FINAL storm arm (iteration 3) — split removed, producer better, the user's median still … | Study 6 | [cards/slip/STUDY-6-common-band-scope.md](cards/slip/STUDY-6-common-band-scope.md) |
| 14.10 The IF-TD detector watched ONE lane per satellite — `network.cycle_slip.if_phase_td_pai… | IF-TD pairs | [cards/slip/IFTD-PAIRS.md](cards/slip/IFTD-PAIRS.md) |
| 14.10.1 The defect | IF-TD pairs | [cards/slip/IFTD-PAIRS.md](cards/slip/IFTD-PAIRS.md) |
| 14.10.2 The knob | IF-TD pairs | [cards/slip/IFTD-PAIRS.md](cards/slip/IFTD-PAIRS.md) |
| 14.10.3 ⚠ 14.10.4 first — ONE reference arm per (station, constellation) | IF-TD pairs | [cards/slip/IFTD-PAIRS.md](cards/slip/IFTD-PAIRS.md) |
| 14.10.4 Harness legs (quiet seed ep0665, S21000SIM / G08, event at +10, 70 epochs) | IF-TD pairs | [cards/slip/IFTD-PAIRS.md](cards/slip/IFTD-PAIRS.md) |
| 14.11 RULE F2 — the LLI shadow: a lane that did not see the flagged epoch is not silent | RULE F2 | [cards/slip/IFTD-F2-lli-shadow.md](cards/slip/IFTD-F2-lli-shadow.md) |
| 14.11.1 The mechanism, from the detector's own decision lines | RULE F2 | [cards/slip/IFTD-F2-lli-shadow.md](cards/slip/IFTD-F2-lli-shadow.md) |
| 14.11.2 The rule | RULE F2 | [cards/slip/IFTD-F2-lli-shadow.md](cards/slip/IFTD-F2-lli-shadow.md) |
| 14.11.3 Detector readout | RULE F2 | [cards/slip/IFTD-F2-lli-shadow.md](cards/slip/IFTD-F2-lli-shadow.md) |
| 14.11.4 Gates | RULE F2 | [cards/slip/IFTD-F2-lli-shadow.md](cards/slip/IFTD-F2-lli-shadow.md) |
| 14.12 RULE F3 — the LLI-EPOCH verdict: the epoch the rule could not see | RULE F3 | [cards/slip/IFTD-F3-lli-epoch.md](cards/slip/IFTD-F3-lli-epoch.md) |
| 14.12.1 The gap §14.10.4 found | RULE F3 | [cards/slip/IFTD-F3-lli-epoch.md](cards/slip/IFTD-F3-lli-epoch.md) |
| 14.12.2 The rule | RULE F3 | [cards/slip/IFTD-F3-lli-epoch.md](cards/slip/IFTD-F3-lli-epoch.md) |
| 14.12.3 Legs and gates | RULE F3 | [cards/slip/IFTD-F3-lli-epoch.md](cards/slip/IFTD-F3-lli-epoch.md) |
| 14.12.4 RESULT — the gap closes, and `all` becomes a pure addition on every leg | RULE F3 | [cards/slip/IFTD-F3-lli-epoch.md](cards/slip/IFTD-F3-lli-epoch.md) |
| 14.12.5 The ANCHOR CLAUSE inside F3 — iteration 2's defect, re-entered through the LLI door | RULE F3 | [cards/slip/IFTD-F3-lli-epoch.md](cards/slip/IFTD-F3-lli-epoch.md) |
| 14.12.6 RESULT — the anchor-arc leg (S36000SIM / G04, L5Q −31 with LLI at +10) | RULE F3 | [cards/slip/IFTD-F3-lli-epoch.md](cards/slip/IFTD-F3-lli-epoch.md) |
| 16. Study 7 — the common-band scope's 3 cm is ONE wrong narrowing at 4.99 h (2026-09-11, afte… | Study 7 | [cards/slip/STUDY-7-common-band-narrowing.md](cards/slip/STUDY-7-common-band-narrowing.md) |
| 16.1 What the two storm arms' own logs established (no runs; `scratchpad/cb3_census.py`) | Study 7 | [cards/slip/STUDY-7-common-band-narrowing.md](cards/slip/STUDY-7-common-band-narrowing.md) |
| 16.2 The mechanism (the reading, frozen before the workers report) | Study 7 | [cards/slip/STUDY-7-common-band-narrowing.md](cards/slip/STUDY-7-common-band-narrowing.md) |
| 16.3 The fix, pre-registered — IF-TD on EVERY pair (`network.cycle_slip.if_phase_td_pairs`: `… | Study 7 | [cards/slip/STUDY-7-common-band-narrowing.md](cards/slip/STUDY-7-common-band-narrowing.md) |
| 16.4 Pre-registered gates | Study 7 | [cards/slip/STUDY-7-common-band-narrowing.md](cards/slip/STUDY-7-common-band-narrowing.md) |
| 16.5 The census returns — the reading corrected and the storm gates REVISED before any FIX ar… | Study 7 | [cards/slip/STUDY-7-common-band-narrowing.md](cards/slip/STUDY-7-common-band-narrowing.md) |
| 16.6 The follow-up census — the LLI shadow, a second rule under the same knob, pre-registered… | Study 7 | [cards/slip/STUDY-7-common-band-narrowing.md](cards/slip/STUDY-7-common-band-narrowing.md) |
| 17. Next card, pre-registration stub — the ORACLE PIN: hold the ambiguities at truth and read… | ORACLE PIN (stub) | [cards/harness/ORACLE-PIN-stub.md](cards/harness/ORACLE-PIN-stub.md) |
| 16.7 Storm arm cbscope4 — the fallout is gone, and the arm found the one clause F3 lacked (20… | Study 7 | [cards/slip/STUDY-7-common-band-narrowing.md](cards/slip/STUDY-7-common-band-narrowing.md) |
| 16.8 FINAL storm arm cbscope5 — split references gone, the user's median 0.174 → 0.120 m: bot… | Study 7 | [cards/slip/STUDY-7-common-band-narrowing.md](cards/slip/STUDY-7-common-band-narrowing.md) |
| 18. Harness hygiene — every leg before 2026-09-11 (evening) RESTARTED the ZWD channel at the … | harness seeds | [cards/harness/HARNESS-SEEDS.md](cards/harness/HARNESS-SEEDS.md) |
| 19. The PIVOT DEPARTURE card — the 1.30 m consumer shock is producer-internal carry free-fall… | PIVOT DEPARTURE | [cards/network-vehicle/PIVOT-DEPARTURE.md](cards/network-vehicle/PIVOT-DEPARTURE.md) |
| 19.1 What the census established (cbscope5 `products.txt`, step across 319230 → 319260) | PIVOT DEPARTURE | [cards/network-vehicle/PIVOT-DEPARTURE.md](cards/network-vehicle/PIVOT-DEPARTURE.md) |
| 19.2 The harness leg — the departure with NO injection | PIVOT DEPARTURE | [cards/network-vehicle/PIVOT-DEPARTURE.md](cards/network-vehicle/PIVOT-DEPARTURE.md) |
| 19.3 Pre-registered increments and gates | PIVOT DEPARTURE | [cards/network-vehicle/PIVOT-DEPARTURE.md](cards/network-vehicle/PIVOT-DEPARTURE.md) |
| 19.4 Increment 0 RESULT — `network.scoped_handover_response` removes the collateral constella… | PIVOT DEPARTURE | [cards/network-vehicle/PIVOT-DEPARTURE.md](cards/network-vehicle/PIVOT-DEPARTURE.md) |
| 19.5 Storm arm + PROMOTION — `network.scoped_handover_response` is RECOMMEND_ON and pinned in… | PIVOT DEPARTURE | [cards/network-vehicle/PIVOT-DEPARTURE.md](cards/network-vehicle/PIVOT-DEPARTURE.md) |
| 18.1 Seeds re-written at the promoted golden, and the fingerprint rule (2026-09-12 ~00:50 local) | harness seeds | [cards/harness/HARNESS-SEEDS.md](cards/harness/HARNESS-SEEDS.md) |
| 18.2 Seeds re-written AGAIN at the increment-1 golden — `_v3`, and why `_v2` snapshots are no… | harness seeds | [cards/harness/HARNESS-SEEDS.md](cards/harness/HARNESS-SEEDS.md) |
| 19.6 Increment 1 BUILT + PROMOTED — the clock-carry S-transform (`network.handover_stransform… | PIVOT DEPARTURE | [cards/network-vehicle/PIVOT-DEPARTURE.md](cards/network-vehicle/PIVOT-DEPARTURE.md) |
| 20. The FIELD harness — a 6 h seed on the field golden and the owed field legs for the three … | FIELD harness | [cards/harness/FIELD-HARNESS.md](cards/harness/FIELD-HARNESS.md) |
| 20.1 RESULTS — the field seed, the legs, the twin, and the two defects the harness found (202… | FIELD harness | [cards/harness/FIELD-HARNESS.md](cards/harness/FIELD-HARNESS.md) |
| 20.2 RESULTS — FP-1, the §20 matrix on the warm-resuming seed: defect 1 is cured, every ident… | FIELD harness | [cards/harness/FIELD-HARNESS.md](cards/harness/FIELD-HARNESS.md) |
| 20.3 RESULTS — FP-2, the matrix on the FIXED posture: the 60-epoch matrix is a clean sweep, t… | FIELD harness | [cards/harness/FIELD-HARNESS.md](cards/harness/FIELD-HARNESS.md) |
| 21. THE FIELD WARM-RESUME SEAM (card RS-2) — the restored invariant DD branch is folded in th… | RS-2 | [cards/resume-seam/RS-2-writer-frame-rebase.md](cards/resume-seam/RS-2-writer-frame-rebase.md) |
| 21.1 What §20.1 called DEFECT 1, re-read from the instruments the legs already wrote | RS-2 | [cards/resume-seam/RS-2-writer-frame-rebase.md](cards/resume-seam/RS-2-writer-frame-rebase.md) |
| 21.2 The mechanism (read from the code; the pre-registered legs test it) | RS-2 | [cards/resume-seam/RS-2-writer-frame-rebase.md](cards/resume-seam/RS-2-writer-frame-rebase.md) |
| 21.3 Pre-registered legs — Phase 1, on the EXISTING seed (no new seed, no code change) | RS-2 | [cards/resume-seam/RS-2-writer-frame-rebase.md](cards/resume-seam/RS-2-writer-frame-rebase.md) |
| 21.4 Phase 2 — the fix, pre-registered (built only after Phase 1 is graded by the manager) | RS-2 | [cards/resume-seam/RS-2-writer-frame-rebase.md](cards/resume-seam/RS-2-writer-frame-rebase.md) |
| 21.5 RESULTS — Phase 1 (the blob is the seam), the Phase 2 build merged, and the P2/P3 bars r… | RS-2 | [cards/resume-seam/RS-2-writer-frame-rebase.md](cards/resume-seam/RS-2-writer-frame-rebase.md) |
| 21.6 RESULTS — Phase 2: the re-base is exact and total, and it is NECESSARY BUT NOT SUFFICIEN… | RS-2 | [cards/resume-seam/RS-2-writer-frame-rebase.md](cards/resume-seam/RS-2-writer-frame-rebase.md) |
| 21.7 RESULTS — RS-2b: the branch was right; the wire's realization is set by two datum memori… | RS-2 | [cards/resume-seam/RS-2-writer-frame-rebase.md](cards/resume-seam/RS-2-writer-frame-rebase.md) |
| 21.8 MAIN CERTIFICATION after the day's merges (2026-09-12 ~14:20 local; worker on main's own… | RS-2 | [cards/resume-seam/RS-2-writer-frame-rebase.md](cards/resume-seam/RS-2-writer-frame-rebase.md) |
| 21.9 RESULTS — RS-3: both datums continue at the seam; the wire still re-realizes ~10 m per p… | RS-3 … RS-5 | [cards/resume-seam/RS-3-to-RS-5-datum-and-emit-layer.md](cards/resume-seam/RS-3-to-RS-5-datum-and-emit-layer.md) |
| 21.10 RESULTS — RS-4: the seam's phase-bias level is the EMIT LAYER, and it closes exactly; t… | RS-3 … RS-5 | [cards/resume-seam/RS-3-to-RS-5-datum-and-emit-layer.md](cards/resume-seam/RS-3-to-RS-5-datum-and-emit-layer.md) |
| 21.11 RESULTS — RS-5 closes the seam on the phase-bias wire; the day's close-out; RS-6 pre-re… | RS-3 … RS-5 | [cards/resume-seam/RS-3-to-RS-5-datum-and-emit-layer.md](cards/resume-seam/RS-3-to-RS-5-datum-and-emit-layer.md) |
| 21.12 CLOSE-OUT CERTIFICATION (2026-09-12 ~22:30 local; worker on main's own tree at `17f854f`) | RS-3 … RS-5 | [cards/resume-seam/RS-3-to-RS-5-datum-and-emit-layer.md](cards/resume-seam/RS-3-to-RS-5-datum-and-emit-layer.md) |
| 21.13 RS-6a pre-registered — the station layer's own memory across the seam: the rx-CB carry … | RS-6a | [cards/resume-seam/RS-6a-station-layer-memories.md](cards/resume-seam/RS-6a-station-layer-memories.md) |
| 21.14 RESULTS — RS-6a: the two station-layer memories partition cleanly, the T5 half removes … | RS-6a | [cards/resume-seam/RS-6a-station-layer-memories.md](cards/resume-seam/RS-6a-station-layer-memories.md) |
| 21.15 FINAL CERTIFICATION of the day's tree (2026-09-13 ~04:30 local; worker on main at `cb4f… | RS-6a | [cards/resume-seam/RS-6a-station-layer-memories.md](cards/resume-seam/RS-6a-station-layer-memories.md) |
| 21.16 RS-8 pre-registered — the six L2X families' "datum drop" is the per-station CANONICAL-S… | RS-7 … RS-9 | [cards/resume-seam/RS-7-to-RS-9-border-and-rxcb-frame.md](cards/resume-seam/RS-7-to-RS-9-border-and-rxcb-frame.md) |
| 21.17 RESULTS — RS-7: the dense border restores exactly, and it is NOT the residual (KEEP_OFF… | RS-7 … RS-9 | [cards/resume-seam/RS-7-to-RS-9-border-and-rxcb-frame.md](cards/resume-seam/RS-7-to-RS-9-border-and-rxcb-frame.md) |
| 21.18 RESULTS — RS-7b census: the flat 26 % is the rx-CB inject gate reading a FROZEN, un-sna… | RS-7 … RS-9 | [cards/resume-seam/RS-7-to-RS-9-border-and-rxcb-frame.md](cards/resume-seam/RS-7-to-RS-9-border-and-rxcb-frame.md) |
| 21.19 RESULTS — RS-9 closes the rx-CB frame (withheld share 26.3 % → 9.8 %, the seed's 11.0 %… | RS-7 … RS-9 | [cards/resume-seam/RS-7-to-RS-9-border-and-rxcb-frame.md](cards/resume-seam/RS-7-to-RS-9-border-and-rxcb-frame.md) |
| 21.20 RESULTS — RS-8: the canonical-signal latch flips on 23 of 61 stations at a resume, not … | RS-8 … RS-10b | [cards/resume-seam/RS-8-to-RS-10b-latch-and-close-out.md](cards/resume-seam/RS-8-to-RS-10b-latch-and-close-out.md) |
| 21.21 RESULTS — RS-10 census: the ZWD "step" is `zwd_seam_reseed` itself on a lineage that ca… | RS-8 … RS-10b | [cards/resume-seam/RS-8-to-RS-10b-latch-and-close-out.md](cards/resume-seam/RS-8-to-RS-10b-latch-and-close-out.md) |
| 21.22 CERTIFICATION — the night's merges on main's own binaries; the v8 harness seeds; the cl… | RS-8 … RS-10b | [cards/resume-seam/RS-8-to-RS-10b-latch-and-close-out.md](cards/resume-seam/RS-8-to-RS-10b-latch-and-close-out.md) |
| 22. THE REFERENCE-ARC CLAUSE for the common-band scope (`network.cycle_slip.common_band_ref_a… | REF-ARC | [cards/slip/REF-ARC-CLAUSE.md](cards/slip/REF-ARC-CLAUSE.md) |
| 22.1 The defect, restated from the code | REF-ARC | [cards/slip/REF-ARC-CLAUSE.md](cards/slip/REF-ARC-CLAUSE.md) |
| 22.2 The clause (built default-OFF behind `network.cycle_slip.common_band_ref_arc`) | REF-ARC | [cards/slip/REF-ARC-CLAUSE.md](cards/slip/REF-ARC-CLAUSE.md) |
| 22.3 Pre-registered legs and gates | REF-ARC | [cards/slip/REF-ARC-CLAUSE.md](cards/slip/REF-ARC-CLAUSE.md) |
| 22.4 RESULTS — B1–B3 PASS on the field seed; an unregistered effect; B4 running (2026-09-12 ~… | REF-ARC | [cards/slip/REF-ARC-CLAUSE.md](cards/slip/REF-ARC-CLAUSE.md) |
| 22.5 B4 PASS — the sim storm bed is neutral on every bar; the clause is PROMOTED (RECOMMEND_O… | REF-ARC | [cards/slip/REF-ARC-CLAUSE.md](cards/slip/REF-ARC-CLAUSE.md) |
| 22.6 THE REFERENCE-SLIP FAN-OUT under `if_phase_td_pairs = all` — found by FP-1 on the warm-r… | REF-FAN | [cards/slip/REF-FAN.md](cards/slip/REF-FAN.md) |
| 22.7 RESULTS — REF-FAN: the reference arm on the additional chains holds the flood and remove… | REF-FAN | [cards/slip/REF-FAN.md](cards/slip/REF-FAN.md) |
| 23. D5 v5 CARRIED-ONLY on the field (`network.carry_invariant_innov_carried_only`) — the born… | D5 v5 | [cards/network-vehicle/D5-V5-carried-only.md](cards/network-vehicle/D5-V5-carried-only.md) |
| 23.2 RESULTS — the same-realization pair refuses the pin: KEEP_OFF on the field, re-test name… | D5 v5 | [cards/network-vehicle/D5-V5-carried-only.md](cards/network-vehicle/D5-V5-carried-only.md) |
| 23.3 The v8 re-test (2026-09-13 ~21:20; the pair §23.2 named, run in the certification, §21.22) | D5 v5 | [cards/network-vehicle/D5-V5-carried-only.md](cards/network-vehicle/D5-V5-carried-only.md) |
| 24. THE CLOSED-LOOP CONSUMER AS AN INSTRUMENT (card CI) — the field consumer wanders by metre… | CI | [cards/consumer/CI-consumer-instrument.md](cards/consumer/CI-consumer-instrument.md) |
| 24.1 What the consumer bars have actually been reading | CI | [cards/consumer/CI-consumer-instrument.md](cards/consumer/CI-consumer-instrument.md) |
| 24.2 The reset channel, measured — the four slip knobs did not reach the consumer through it | CI | [cards/consumer/CI-consumer-instrument.md](cards/consumer/CI-consumer-instrument.md) |
| 24.3 Pre-registered attribution legs (launched ~00:20, before this section; none read) | CI | [cards/consumer/CI-consumer-instrument.md](cards/consumer/CI-consumer-instrument.md) |
| 24.4 Card CC — the continuity half, built regardless of §24.3's answer (worker brief `brief_c… | CC | [cards/consumer/CC-declared-step-carry.md](cards/consumer/CC-declared-step-carry.md) |
| 24.5 The wire-vs-CODE consistency census — truth-referenced, datum-invariant, consumer-free, … | CI census | [cards/consumer/CI-census-wire-vs-code.md](cards/consumer/CI-census-wire-vs-code.md) |
| 24.6 The CODE-RESIDUAL attribution — per-LOS, from the producer's own post-fit residuals (pre… | CI census | [cards/consumer/CI-census-wire-vs-code.md](cards/consumer/CI-census-wire-vs-code.md) |
| 24.8 Card CM — CODE-MODEL CONSISTENCY: single-knob COLD arms that name the mechanism (pre-reg… | CM | [cards/network-vehicle/CM-code-model-consistency.md](cards/network-vehicle/CM-code-model-consistency.md) |
| 24.9 The row-level read (2026-09-14 ~14:30, manager): the a-priori model is right, the code r… | CM | [cards/network-vehicle/CM-code-model-consistency.md](cards/network-vehicle/CM-code-model-consistency.md) |
| 24.10 Card ZP — the station block's parameterization at ignition (pre-registered; the phase-d… | ZP | [cards/network-vehicle/ZP-station-block-parameterization.md](cards/network-vehicle/ZP-station-block-parameterization.md) |
| 24.11 Card PV — the VEHICLE test (pre-registered 2026-09-14 before any arm is read; the autho… | PV | [cards/network-vehicle/PV-vehicle-test.md](cards/network-vehicle/PV-vehicle-test.md) |
| 24.12 Card CM2 — the second defect: the §24.8 knob arms on the LEVELING vehicle (pre-register… | CM2 | [cards/network-vehicle/CM2-leveling-vehicle.md](cards/network-vehicle/CM2-leveling-vehicle.md) |
| 24.13 Card LV-SEED — where the remaining per-LOS level is born (pre-registered reads on the E… | LV-SEED | [cards/ionosphere/LV-SEED-per-los-level.md](cards/ionosphere/LV-SEED-per-los-level.md) |
| 24.14 Card CE — the CHAIN-ERA bisect on the leveling vehicle (pre-registered before any arm i… | CE | [cards/network-vehicle/CE-chain-era-bisect.md](cards/network-vehicle/CE-chain-era-bisect.md) |
| 24.15 Additions pre-registered from §24.13 (before any is read) | LV-SEED | [cards/ionosphere/LV-SEED-per-los-level.md](cards/ionosphere/LV-SEED-per-los-level.md) |
| 24.16 Card Q3-F — the self-contained clock time-link on the leveling vehicle (pre-registered … | Q3-F | [cards/clock/Q3-F-self-contained-time-link.md](cards/clock/Q3-F-self-contained-time-link.md) |
| 24.17 The posture decision, and the closing legs (pre-registered before any is read) | DEC-field-vehicle | [decisions/DEC-field-vehicle-posture.md](decisions/DEC-field-vehicle-posture.md) |
| 24.18 Card GL — the gross line-of-sight screen (pre-registered before any arm is read) | GL | [cards/raim-qc/GL-gross-los-screen.md](cards/raim-qc/GL-gross-los-screen.md) |
| 24.19 GL-2 (the whole-satellite stand-down) and R8 (the bulk drift's channel) — pre-registere… | GL-2 + R8 | [cards/raim-qc/GL-2-whole-satellite-standdown.md](cards/raim-qc/GL-2-whole-satellite-standdown.md) |
| 24.20 Card CS — clock step signalling (pre-registered before any arm is read) | CS | [cards/clock/CS-clock-step-signalling.md](cards/clock/CS-clock-step-signalling.md) |
