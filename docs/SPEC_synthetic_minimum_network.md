# SPEC — the SYNTHETIC MINIMUM NETWORK (the sim half of G4) — SPLIT

`docs/SPEC_synthetic_minimum_network.md` was split on 2026-09-14 — the original is [archive/SPEC_synthetic_minimum_network.md](archive/SPEC_synthetic_minimum_network.md); sections map as follows.

The archived copy is **frozen**: it is kept byte-for-byte as proof and is never appended to. New results go in the card file named below. See [INDEX.md](INDEX.md) for the whole tree and the rules, and [topics/sim-bed.md](topics/sim-bed.md) for this subject.

| section | card id | file |
| --- | --- | --- |
| (title + preamble) | SIMNET | [cards/sim-bed/SIMNET-REF.md](cards/sim-bed/SIMNET-REF.md) |
| (a truth-graded 3/5-station fixture: the first ABSOLUTE grade the network producer has ever h… | SIMNET | [cards/sim-bed/SIMNET-REF.md](cards/sim-bed/SIMNET-REF.md) |
| §0 READ THIS FIRST — document status, and the error class it exposed | SIMNET | [cards/sim-bed/SIMNET-REF.md](cards/sim-bed/SIMNET-REF.md) |
| The error class, stated generally | SIMNET | [cards/sim-bed/SIMNET-REF.md](cards/sim-bed/SIMNET-REF.md) |
| §1 The architecture decision — N processes, one per station | SIMNET | [cards/sim-bed/SIMNET-REF.md](cards/sim-bed/SIMNET-REF.md) |
| 1.1 The two failure modes this decision creates, and their fixes | SIMNET | [cards/sim-bed/SIMNET-REF.md](cards/sim-bed/SIMNET-REF.md) |
| §2 The fixture | SIMNET | [cards/sim-bed/SIMNET-REF.md](cards/sim-bed/SIMNET-REF.md) |
| 2.1 Geometry must be CERTIFIED, not assumed | SIMNET | [cards/sim-bed/SIMNET-REF.md](cards/sim-bed/SIMNET-REF.md) |
| 2.4 The 50-station globally-distributed variant | SIMNET | [cards/sim-bed/SIMNET-REF.md](cards/sim-bed/SIMNET-REF.md) |
| 2.2 The two bias postures are separate arms, one knob apart | SIMNET | [cards/sim-bed/SIMNET-REF.md](cards/sim-bed/SIMNET-REF.md) |
| 2.3 Mandatory config hygiene (each of these silently produces a null run) | SIMNET | [cards/sim-bed/SIMNET-REF.md](cards/sim-bed/SIMNET-REF.md) |
| §3 Truth channels | SIMNET | [cards/sim-bed/SIMNET-REF.md](cards/sim-bed/SIMNET-REF.md) |
| 3.1 Where the satellite clock error may and may not be injected | SIMNET | [cards/sim-bed/SIMNET-REF.md](cards/sim-bed/SIMNET-REF.md) |
| §4 The estimable-function grading map | SIMNET | [cards/sim-bed/SIMNET-REF.md](cards/sim-bed/SIMNET-REF.md) |
| 4.1 The method correction that makes the χ²/vf audit valid | SIMNET | [cards/sim-bed/SIMNET-REF.md](cards/sim-bed/SIMNET-REF.md) |
| 4.2 C record — satellite clock | SIMNET | [cards/sim-bed/SIMNET-REF.md](cards/sim-bed/SIMNET-REF.md) |
| 4.3 P record — satellite phase bias | SIMNET | [cards/sim-bed/SIMNET-REF.md](cards/sim-bed/SIMNET-REF.md) |
| 4.4 B record — satellite code bias (W6 arm only) | SIMNET | [cards/sim-bed/SIMNET-REF.md](cards/sim-bed/SIMNET-REF.md) |
| 4.5 Ionosphere — the Table-8 identity, and why rx-CB injection is mandatory | SIMNET | [cards/sim-bed/SIMNET-REF.md](cards/sim-bed/SIMNET-REF.md) |
| 4.6 ZWD — the one clean vf channel available today | SIMNET | [cards/sim-bed/SIMNET-REF.md](cards/sim-bed/SIMNET-REF.md) |
| 4.7 Consumer position — the only fully datum-free absolute output | SIMNET | [cards/sim-bed/SIMNET-REF.md](cards/sim-bed/SIMNET-REF.md) |
| 4.8 The one export gap, and how it is closed WITHOUT touching the wire | SIMNET | [cards/sim-bed/SIMNET-REF.md](cards/sim-bed/SIMNET-REF.md) |
| §5 The rank assertion — and why the obvious one is worthless | SIMNET | [cards/sim-bed/SIMNET-REF.md](cards/sim-bed/SIMNET-REF.md) |
| 5.1 What to actually assert | SIMNET | [cards/sim-bed/SIMNET-REF.md](cards/sim-bed/SIMNET-REF.md) |
| §6 The gates | SIMNET | [cards/sim-bed/SIMNET-REF.md](cards/sim-bed/SIMNET-REF.md) |
| G-A — the NULL TEST (no events) | SIMNET | [cards/sim-bed/SIMNET-REF.md](cards/sim-bed/SIMNET-REF.md) |
| G-B — event injection, one class at a time, magnitude sweeps at known epochs | SIMNET | [cards/sim-bed/SIMNET-REF.md](cards/sim-bed/SIMNET-REF.md) |
| G-C — the G4 bench | SIMNET | [cards/sim-bed/SIMNET-REF.md](cards/sim-bed/SIMNET-REF.md) |
| §7 What this fixture CANNOT certify (read before quoting it) | SIMNET | [cards/sim-bed/SIMNET-REF.md](cards/sim-bed/SIMNET-REF.md) |
| §8 Trap register (grader must defend against each) | SIMNET | [cards/sim-bed/SIMNET-REF.md](cards/sim-bed/SIMNET-REF.md) |
| §9 Build manifest | SIMNET | [cards/sim-bed/SIMNET-REF.md](cards/sim-bed/SIMNET-REF.md) |
| §9.5 G-A RESULTS — measured 2026-08-11 (the first absolute grade) | G-A | [cards/sim-bed/SIMNET-GA-NULL-TEST.md](cards/sim-bed/SIMNET-GA-NULL-TEST.md) |
| The finding | G-A | [cards/sim-bed/SIMNET-GA-NULL-TEST.md](cards/sim-bed/SIMNET-GA-NULL-TEST.md) |
| What this does NOT establish | G-A | [cards/sim-bed/SIMNET-GA-NULL-TEST.md](cards/sim-bed/SIMNET-GA-NULL-TEST.md) |
| §9.7 The 50-STATION GLOBAL arm — the vf trend and the QC tiers (2026-08-11) | G-A 50-sta | [cards/sim-bed/SIMNET-50STA-QC-TIERS.md](cards/sim-bed/SIMNET-50STA-QC-TIERS.md) |
| 9.7.1 The vf trend — and the two channels diverge | G-A 50-sta | [cards/sim-bed/SIMNET-50STA-QC-TIERS.md](cards/sim-bed/SIMNET-50STA-QC-TIERS.md) |
| 9.7.2 The QC tiers — 0/7 live at n = 3, 4/7 at n = 50 | G-A 50-sta | [cards/sim-bed/SIMNET-50STA-QC-TIERS.md](cards/sim-bed/SIMNET-50STA-QC-TIERS.md) |
| 9.7.3 QC DETECTION — quiet vs fault, one knob apart | G-A 50-sta | [cards/sim-bed/SIMNET-50STA-QC-TIERS.md](cards/sim-bed/SIMNET-50STA-QC-TIERS.md) |
| 9.7.4 CHASING THE mw_detect MISS — one defect PROVEN, one still open | G-A 50-sta | [cards/sim-bed/SIMNET-50STA-QC-TIERS.md](cards/sim-bed/SIMNET-50STA-QC-TIERS.md) |
| 9.7.5 RESOLVED — the detector SAW the jump and formed no candidate | G-A 50-sta | [cards/sim-bed/SIMNET-50STA-QC-TIERS.md](cards/sim-bed/SIMNET-50STA-QC-TIERS.md) |
| 9.7.6 The MW NULL SPACE — measured, and RAIM does not currently cover it | G-A 50-sta | [cards/sim-bed/SIMNET-50STA-QC-TIERS.md](cards/sim-bed/SIMNET-50STA-QC-TIERS.md) |
| 9.7.7 The remedy is real but NOT free — do not make it standard yet | G-A 50-sta | [cards/sim-bed/SIMNET-50STA-QC-TIERS.md](cards/sim-bed/SIMNET-50STA-QC-TIERS.md) |
| 9.7.8 ⚠️ CORRECTION — RAIM was NOT blind.  The consensus rule vetoed it. | G-A 50-sta | [cards/sim-bed/SIMNET-50STA-QC-TIERS.md](cards/sim-bed/SIMNET-50STA-QC-TIERS.md) |
| 9.7.9 HOW MUCH DO SLIPS CONTAMINATE?  Measured — and they are ORTHOGONAL to the level problem | G-A 50-sta | [cards/sim-bed/SIMNET-50STA-QC-TIERS.md](cards/sim-bed/SIMNET-50STA-QC-TIERS.md) |
| 9.7.10 The §9.7.8 trail CLOSED — corrections, fixes LANDED, and their measurements (2026-08-11) | G-A 50-sta | [cards/sim-bed/SIMNET-50STA-QC-TIERS.md](cards/sim-bed/SIMNET-50STA-QC-TIERS.md) |
| §9.8 The DATUM-WANDER arm — the common level FOLLOWS THE DATUM STATION, and both remedies mea… | datum wander | [cards/network-vehicle/DATUM-WANDER.md](cards/network-vehicle/DATUM-WANDER.md) |
| Findings | datum wander | [cards/network-vehicle/DATUM-WANDER.md](cards/network-vehicle/DATUM-WANDER.md) |
| What this does and does not say about the FIELD wander | datum wander | [cards/network-vehicle/DATUM-WANDER.md](cards/network-vehicle/DATUM-WANDER.md) |
| 9.8.1 The 25° mask arms — the mechanism CONFIRMED, the remedy REJECTED | datum wander | [cards/network-vehicle/DATUM-WANDER.md](cards/network-vehicle/DATUM-WANDER.md) |
| §9.9 The FIELD probe — the wire wander is NOT receiver-code-driven (2026-08-11) | field wander probe | [cards/clock/FIELD-WANDER-PROBE.md](cards/clock/FIELD-WANDER-PROBE.md) |
| 9.9.1 The a-priori CLK common-drift probe — the PRODUCT FRAME EXONERATED | field wander probe | [cards/clock/FIELD-WANDER-PROBE.md](cards/clock/FIELD-WANDER-PROBE.md) |
| §9.10 Cross-station IONO-FIELD agreement — coherent where mapped, BROKEN at low–high (2026-08… | iono-field agreement | [cards/ionosphere/IONO-FIELD-AGREEMENT.md](cards/ionosphere/IONO-FIELD-AGREEMENT.md) |
| §9.6 G-B — satellite-clock step, FIRST CLASS ONLY (measured 2026-08-11) | G-B | [cards/sim-bed/SIMNET-GB-CLOCK-STEP.md](cards/sim-bed/SIMNET-GB-CLOCK-STEP.md) |
| §11 NEXT — the W6+SH synthetic arm (design brief for the next session) | W6+SH brief | [cards/sim-bed/SIMNET-W6SH-BRIEF.md](cards/sim-bed/SIMNET-W6SH-BRIEF.md) |
| 9.10.1 The elevation-cutoff FIELD A/B — direction CONFIRMED, magnitude small (2026-08-12) | iono-field agreement | [cards/ionosphere/IONO-FIELD-AGREEMENT.md](cards/ionosphere/IONO-FIELD-AGREEMENT.md) |
| §12 The W6+SH ARM — built, and its minimum-n ladder measured (2026-08-12) | W6+SH arm | [cards/sim-bed/SIMNET-W6SH-ARM.md](cards/sim-bed/SIMNET-W6SH-ARM.md) |
| 12.1 What was built | W6+SH arm | [cards/sim-bed/SIMNET-W6SH-ARM.md](cards/sim-bed/SIMNET-W6SH-ARM.md) |
| 12.2 The ladder — four rungs at n = 50, 20 epochs each (~90 s per rung) | W6+SH arm | [cards/sim-bed/SIMNET-W6SH-ARM.md](cards/sim-bed/SIMNET-W6SH-ARM.md) |
| 12.3 What the ladder does and does not settle | W6+SH arm | [cards/sim-bed/SIMNET-W6SH-ARM.md](cards/sim-bed/SIMNET-W6SH-ARM.md) |
| 12.4 The §4.8 export gap is CLOSED (both halves) | W6+SH arm | [cards/sim-bed/SIMNET-W6SH-ARM.md](cards/sim-bed/SIMNET-W6SH-ARM.md) |
| 12.5 Pre-registered predictions for the settled arm | W6+SH arm | [cards/sim-bed/SIMNET-W6SH-ARM.md](cards/sim-bed/SIMNET-W6SH-ARM.md) |
| 12.7 THE GRADED ARM — 466/480 epochs, and all three predictions resolved | W6+SH arm | [cards/sim-bed/SIMNET-W6SH-ARM.md](cards/sim-bed/SIMNET-W6SH-ARM.md) |
| 12.6 What is still owed | W6+SH arm | [cards/sim-bed/SIMNET-W6SH-ARM.md](cards/sim-bed/SIMNET-W6SH-ARM.md) |
| §12.8 The CHAIN-PIN AUDIT — rungs 2/6 re-attributed, and the field's emission | chain-pin audit | [cards/network-vehicle/CHAIN-PIN-AUDIT.md](cards/network-vehicle/CHAIN-PIN-AUDIT.md) |
| explained (2026-08-12, HANDOFF_wsh_chain_slant T1/T2) | chain-pin audit | [cards/network-vehicle/CHAIN-PIN-AUDIT.md](cards/network-vehicle/CHAIN-PIN-AUDIT.md) |
| 12.8.1 The audit answer: the chain is COMPLETE — and the decision point flips | chain-pin audit | [cards/network-vehicle/CHAIN-PIN-AUDIT.md](cards/network-vehicle/CHAIN-PIN-AUDIT.md) |
| 12.8.2 Rungs 2/6 were NEVER rank-gate withholds — §12.2's attribution corrected | chain-pin audit | [cards/network-vehicle/CHAIN-PIN-AUDIT.md](cards/network-vehicle/CHAIN-PIN-AUDIT.md) |
| 12.8.3 The discharge is a SIM-ONLY phase-alignment artifact — and it answers | chain-pin audit | [cards/network-vehicle/CHAIN-PIN-AUDIT.md](cards/network-vehicle/CHAIN-PIN-AUDIT.md) |
| "why does the field emit at 303" | chain-pin audit | [cards/network-vehicle/CHAIN-PIN-AUDIT.md](cards/network-vehicle/CHAIN-PIN-AUDIT.md) |
| 12.8.4 Consequences | chain-pin audit | [cards/network-vehicle/CHAIN-PIN-AUDIT.md](cards/network-vehicle/CHAIN-PIN-AUDIT.md) |
| §12.9 The elevation-cutoff A/B WITH TRUTH — the field verdict INVERTS on a | elev-cutoff A/B | [cards/ionosphere/ELEV-CUTOFF-AB.md](cards/ionosphere/ELEV-CUTOFF-AB.md) |
| coherent field (2026-08-12, HANDOFF_wsh_chain_slant T3) | elev-cutoff A/B | [cards/ionosphere/ELEV-CUTOFF-AB.md](cards/ionosphere/ELEV-CUTOFF-AB.md) |
| §13 The THIRD BAND, and the slant-ionosphere posture (2026-08-12) | slant + triple | [cards/sim-bed/SIMNET-SLANT-TRIPLE.md](cards/sim-bed/SIMNET-SLANT-TRIPLE.md) |
| 13.1 What was built (sim side, done) | slant + triple | [cards/sim-bed/SIMNET-SLANT-TRIPLE.md](cards/sim-bed/SIMNET-SLANT-TRIPLE.md) |
| 13.2 The slant posture is NOT reachable in the producer today — two changes | slant + triple | [cards/sim-bed/SIMNET-SLANT-TRIPLE.md](cards/sim-bed/SIMNET-SLANT-TRIPLE.md) |
| 13.3 What can be measured before that work | slant + triple | [cards/sim-bed/SIMNET-SLANT-TRIPLE.md](cards/sim-bed/SIMNET-SLANT-TRIPLE.md) |
| 13.3.1 MEASURED — the third band closes the observability wall (outcome 1) | slant + triple | [cards/sim-bed/SIMNET-SLANT-TRIPLE.md](cards/sim-bed/SIMNET-SLANT-TRIPLE.md) |
| 13.3.2 BUILT AND MEASURED — the slant posture runs, and its code bias is CALIBRATED | slant + triple | [cards/sim-bed/SIMNET-SLANT-TRIPLE.md](cards/sim-bed/SIMNET-SLANT-TRIPLE.md) |
| 13.5 THE MATCHED MATRIX — settled, and it corrects §13.3.2 | slant + triple | [cards/sim-bed/SIMNET-SLANT-TRIPLE.md](cards/sim-bed/SIMNET-SLANT-TRIPLE.md) |
| 13.4 Pre-registered, for when the posture runs | slant + triple | [cards/sim-bed/SIMNET-SLANT-TRIPLE.md](cards/sim-bed/SIMNET-SLANT-TRIPLE.md) |
| 13.6 THE AMBIGUITY-INTEGER GRADE — the first absolute integer test, and the | slant + triple | [cards/sim-bed/SIMNET-SLANT-TRIPLE.md](cards/sim-bed/SIMNET-SLANT-TRIPLE.md) |
| lane ladder measured (2026-08-12, HANDOFF_slant_triple_next §3) | slant + triple | [cards/sim-bed/SIMNET-SLANT-TRIPLE.md](cards/sim-bed/SIMNET-SLANT-TRIPLE.md) |
| 13.7 THE SETTLED TRIO ARM — unblocked, and it DIVERGES (2026-08-12) | slant + triple | [cards/sim-bed/SIMNET-SLANT-TRIPLE.md](cards/sim-bed/SIMNET-SLANT-TRIPLE.md) |
| 13.8 GRADING RULES AND TRAPS for the triple/slant posture | slant + triple | [cards/sim-bed/SIMNET-SLANT-TRIPLE.md](cards/sim-bed/SIMNET-SLANT-TRIPLE.md) |
| (absorbed 2026-08-15 from `docs/HANDOFF_slant_triple_next.md`, now attic'd) | slant + triple | [cards/sim-bed/SIMNET-SLANT-TRIPLE.md](cards/sim-bed/SIMNET-SLANT-TRIPLE.md) |
| §10 Verification status | SIMNET | [cards/sim-bed/SIMNET-REF.md](cards/sim-bed/SIMNET-REF.md) |
| §14 THE CHAIN-SLANT BRIEF — Odijk theory, and the T1–T4 disposition | chain-slant | [cards/network-vehicle/CHAIN-SLANT-ODIJK.md](cards/network-vehicle/CHAIN-SLANT-ODIJK.md) |
| (absorbed 2026-08-15 from `docs/HANDOFF_wsh_chain_slant.md`, now attic'd) | chain-slant | [cards/network-vehicle/CHAIN-SLANT-ODIJK.md](cards/network-vehicle/CHAIN-SLANT-ODIJK.md) |
| 14.0 T-LABEL MAP — the anchor to re-point to | chain-slant | [cards/network-vehicle/CHAIN-SLANT-ODIJK.md](cards/network-vehicle/CHAIN-SLANT-ODIJK.md) |
| 14.1 Odijk rank arithmetic — why the near-null is n-invariant BY CONSTRUCTION | chain-slant | [cards/network-vehicle/CHAIN-SLANT-ODIJK.md](cards/network-vehicle/CHAIN-SLANT-ODIJK.md) |
| 14.2 The global-network chain caveat, and the eq34 implementation it warrants | chain-slant | [cards/network-vehicle/CHAIN-SLANT-ODIJK.md](cards/network-vehicle/CHAIN-SLANT-ODIJK.md) |
| 14.3 "From the second epoch onwards" — the epoch-1 datum, and its inverse | chain-slant | [cards/network-vehicle/CHAIN-SLANT-ODIJK.md](cards/network-vehicle/CHAIN-SLANT-ODIJK.md) |
| 14.4 The three self-consistent postures, and the f ≥ 3 epoch-1 caveat | chain-slant | [cards/network-vehicle/CHAIN-SLANT-ODIJK.md](cards/network-vehicle/CHAIN-SLANT-ODIJK.md) |
| 14.5 Reproduction, and the exe-snapshot rule | chain-slant | [cards/network-vehicle/CHAIN-SLANT-ODIJK.md](cards/network-vehicle/CHAIN-SLANT-ODIJK.md) |
| 14.6 Do NOT re-litigate (measured, in §12) | chain-slant | [cards/network-vehicle/CHAIN-SLANT-ODIJK.md](cards/network-vehicle/CHAIN-SLANT-ODIJK.md) |
| §15 THE EVENT-INJECTION BENCH — the G-B harness §6 called for | injection bench | [cards/sim-bed/SIM-INJECTION-BENCH.md](cards/sim-bed/SIM-INJECTION-BENCH.md) |
| (absorbed 2026-08-15 from `docs/SPEC_sim_injection_bench.md`, now attic'd) | injection bench | [cards/sim-bed/SIM-INJECTION-BENCH.md](cards/sim-bed/SIM-INJECTION-BENCH.md) |
| 15.1 The ambiguity N-REGIME — a fixture property that changes what an experiment can conclude | injection bench | [cards/sim-bed/SIM-INJECTION-BENCH.md](cards/sim-bed/SIM-INJECTION-BENCH.md) |
| 15.2 The PRE-FLIGHT — do not inject at a dead mechanism | injection bench | [cards/sim-bed/SIM-INJECTION-BENCH.md](cards/sim-bed/SIM-INJECTION-BENCH.md) |
| 15.3 The ARMS | injection bench | [cards/sim-bed/SIM-INJECTION-BENCH.md](cards/sim-bed/SIM-INJECTION-BENCH.md) |
| 15.4 The GRADE, and the mandatory negative control | injection bench | [cards/sim-bed/SIM-INJECTION-BENCH.md](cards/sim-bed/SIM-INJECTION-BENCH.md) |
| 15.5 Grader hardening — the EMPTY-PRODUCT trap | injection bench | [cards/sim-bed/SIM-INJECTION-BENCH.md](cards/sim-bed/SIM-INJECTION-BENCH.md) |
| 15.6 MEASURED — the first bench run | injection bench | [cards/sim-bed/SIM-INJECTION-BENCH.md](cards/sim-bed/SIM-INJECTION-BENCH.md) |
| 15.7 What this bench does NOT discharge — THE single owed-list for G-B | injection bench | [cards/sim-bed/SIM-INJECTION-BENCH.md](cards/sim-bed/SIM-INJECTION-BENCH.md) |
| 15.8 Reproduction | injection bench | [cards/sim-bed/SIM-INJECTION-BENCH.md](cards/sim-bed/SIM-INJECTION-BENCH.md) |
| §16 THE 60-STA CAMPAIGN — the first long / multi-GNSS / realistic-slip grade | 60-sta campaign | [cards/sim-bed/SIMNET-60STA-CAMPAIGN.md](cards/sim-bed/SIMNET-60STA-CAMPAIGN.md) |
| (2026-08-20; full record `docs/SPEC_slip_relevel_walk.md`) | 60-sta campaign | [cards/sim-bed/SIMNET-60STA-CAMPAIGN.md](cards/sim-bed/SIMNET-60STA-CAMPAIGN.md) |
| §17 THE DASHBOARD — one-command HTML report over a graded simnet run (2026-08-31) | dashboard | [cards/sim-bed/SIMNET-DASHBOARD.md](cards/sim-bed/SIMNET-DASHBOARD.md) |
| §18 THE STABILITY CAMPAIGN — the settled state must stop moving (2026-09-01 →) | stability | [cards/sim-bed/STABILITY-CAMPAIGN.md](cards/sim-bed/STABILITY-CAMPAIGN.md) |
| 18.1 TARGET 3 — per-sat metre-class settled clock tails: ROOT-CAUSED | stability | [cards/sim-bed/STABILITY-CAMPAIGN.md](cards/sim-bed/STABILITY-CAMPAIGN.md) |
| (2026-09-01, census instrument on the 12 h bed; fix cards derived, not built) | stability | [cards/sim-bed/STABILITY-CAMPAIGN.md](cards/sim-bed/STABILITY-CAMPAIGN.md) |
| 18.2 TARGET 1 — settled late drift: instrument + 12 h re-analysis | stability | [cards/sim-bed/STABILITY-CAMPAIGN.md](cards/sim-bed/STABILITY-CAMPAIGN.md) |
| (verdict in §18.3) | stability | [cards/sim-bed/STABILITY-CAMPAIGN.md](cards/sim-bed/STABILITY-CAMPAIGN.md) |
| 18.3 TARGET 1 VERDICT (2026-09-01, the 24 h arm `runs/camp60wu_quiet24h_r2q3`): | stability | [cards/sim-bed/STABILITY-CAMPAIGN.md](cards/sim-bed/STABILITY-CAMPAIGN.md) |
| carry aging FALSIFIED; the "late drift" is the transient response to the | stability | [cards/sim-bed/STABILITY-CAMPAIGN.md](cards/sim-bed/STABILITY-CAMPAIGN.md) |
| ECLIPSE-EXCLUSION valve's edge events, dominated by the clock PIVOTS' own transits | stability | [cards/sim-bed/STABILITY-CAMPAIGN.md](cards/sim-bed/STABILITY-CAMPAIGN.md) |
| 18.4 E2 DESIGN SKETCH — realization continuity at re-admission (not built) | stability | [cards/sim-bed/STABILITY-CAMPAIGN.md](cards/sim-bed/STABILITY-CAMPAIGN.md) |
| 18.5 E3 VERDICT — `network.sticky_pivot` (RTA-1) removes the pivot-eclipse | stability | [cards/sim-bed/STABILITY-CAMPAIGN.md](cards/sim-bed/STABILITY-CAMPAIGN.md) |
| consumer cost: settled 24 h consumer 3D rms 0.303 → 0.078 m (−74 %) | stability | [cards/sim-bed/STABILITY-CAMPAIGN.md](cards/sim-bed/STABILITY-CAMPAIGN.md) |
| (2026-09-01, arm `runs/camp60wu_quiet24h_sticky`, single knob vs §18.3's baseline) | stability | [cards/sim-bed/STABILITY-CAMPAIGN.md](cards/sim-bed/STABILITY-CAMPAIGN.md) |
| 18.6 TARGET 4 VERDICT — the injection bench re-verified on the r2q3 posture: | stability | [cards/sim-bed/STABILITY-CAMPAIGN.md](cards/sim-bed/STABILITY-CAMPAIGN.md) |
| MDB unchanged at 2 cycles, 22/24 attributable (2026-09-01) | stability | [cards/sim-bed/STABILITY-CAMPAIGN.md](cards/sim-bed/STABILITY-CAMPAIGN.md) |
| 18.7 TARGET 2 VERDICT — the post-resume transient is a ~4 h WIRE-REALIZATION | stability | [cards/sim-bed/STABILITY-CAMPAIGN.md](cards/sim-bed/STABILITY-CAMPAIGN.md) |
| re-convergence; carry-side seam responses are already native (2026-09-01) | stability | [cards/sim-bed/STABILITY-CAMPAIGN.md](cards/sim-bed/STABILITY-CAMPAIGN.md) |
| 18.8 E3 FIELD TRANSFER (2026-09-02, `runs/fld63_stk_{ctl,on}`): | stability | [cards/sim-bed/STABILITY-CAMPAIGN.md](cards/sim-bed/STABILITY-CAMPAIGN.md) |
| mechanism transfers EXACTLY; the sim bed's harm is ABSENT on the GPS-dual | stability | [cards/sim-bed/STABILITY-CAMPAIGN.md](cards/sim-bed/STABILITY-CAMPAIGN.md) |
| lineage, so the recommendation stays SCOPED | stability | [cards/sim-bed/STABILITY-CAMPAIGN.md](cards/sim-bed/STABILITY-CAMPAIGN.md) |
| 18.9 QUEUE-1 DATA VERDICT — the post-resume transient is the WIRE's phase-bias | stability | [cards/sim-bed/STABILITY-CAMPAIGN.md](cards/sim-bed/STABILITY-CAMPAIGN.md) |
| RE-CONVERGENCE, not the consumer's cold start and not a one-time realization | stability | [cards/sim-bed/STABILITY-CAMPAIGN.md](cards/sim-bed/STABILITY-CAMPAIGN.md) |
| step (2026-09-02, consumer-only replays on the h12 warm leg) | stability | [cards/sim-bed/STABILITY-CAMPAIGN.md](cards/sim-bed/STABILITY-CAMPAIGN.md) |
| 18.10 RESUME FRAME RESTORE — the seam is a FRAME loss; the card restores the | §18.10/§18.11 | [cards/resume-seam/RESUME-FRAME-RESTORE.md](cards/resume-seam/RESUME-FRAME-RESTORE.md) |
| frame state the snapshot omitted (built 2026-09-02; verdict in §18.11) | §18.10/§18.11 | [cards/resume-seam/RESUME-FRAME-RESTORE.md](cards/resume-seam/RESUME-FRAME-RESTORE.md) |
| 18.11 RESUME FRAME RESTORE — VERDICT (2026-09-03, legs from the v2 .ep1440 of | §18.10/§18.11 | [cards/resume-seam/RESUME-FRAME-RESTORE.md](cards/resume-seam/RESUME-FRAME-RESTORE.md) |
| `runs/camp60wu_quiet12h_frwriter`; ON `runs/camp60wu_q12warm_fron`, OFF control | §18.10/§18.11 | [cards/resume-seam/RESUME-FRAME-RESTORE.md](cards/resume-seam/RESUME-FRAME-RESTORE.md) |
| `runs/camp60wu_q12warm_frctl`, rung 3 `runs/camp60wu_q12warm_fron_ari`) | §18.10/§18.11 | [cards/resume-seam/RESUME-FRAME-RESTORE.md](cards/resume-seam/RESUME-FRAME-RESTORE.md) |
| 18.12 COUPLING-REGRESSOR RESTORE — VERDICT (2026-09-03, legs from the v3 | §18.12 | [cards/resume-seam/RESUME-COUPLING-RESTORE.md](cards/resume-seam/RESUME-COUPLING-RESTORE.md) |
| 18.13 E2 + THE DENSE BORDER ACROSS THE SEAM — BUILT 2026-09-05, PRE-REGISTERED | §18.13/§18.14 | [cards/resume-seam/RESUME-DENSE-BORDER-E2.md](cards/resume-seam/RESUME-DENSE-BORDER-E2.md) |
| 18.14 VERDICTS for §18.13 (2026-09-05/06) | §18.13/§18.14 | [cards/resume-seam/RESUME-DENSE-BORDER-E2.md](cards/resume-seam/RESUME-DENSE-BORDER-E2.md) |
