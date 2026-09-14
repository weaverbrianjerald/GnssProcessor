# SPEC — the ionosphere-level information budget (LV-1/LV-2/LV-3/LV-4/LV-5, S2H) — SPLIT

`docs/SPEC_level_information_budget.md` was split on 2026-09-14 — the original is [archive/SPEC_level_information_budget.md](archive/SPEC_level_information_budget.md); sections map as follows.

The archived copy is **frozen**: it is kept byte-for-byte as proof and is never appended to. New results go in the card file named below. See [INDEX.md](INDEX.md) for the whole tree and the rules, and [topics/ionosphere.md](topics/ionosphere.md) for this subject.

| section | card id | file |
| --- | --- | --- |
| (title + preamble) | LV | [cards/ionosphere/LV-REF.md](cards/ionosphere/LV-REF.md) |
| Knob ledger | LV | [cards/ionosphere/LV-REF.md](cards/ionosphere/LV-REF.md) |
| Verdict ledger | LV | [cards/ionosphere/LV-REF.md](cards/ionosphere/LV-REF.md) |
| Section map (numeric order) | LV | [cards/ionosphere/LV-REF.md](cards/ionosphere/LV-REF.md) |
| Grading protocol (binding for anything touching this subject) | LV | [cards/ionosphere/LV-REF.md](cards/ionosphere/LV-REF.md) |
| 0. Rigor level for THIS initiative (user directive, 2026-08-04) | LV | [cards/ionosphere/LV-REF.md](cards/ionosphere/LV-REF.md) |
| 1. The problem, in one paragraph | LV | [cards/ionosphere/LV-REF.md](cards/ionosphere/LV-REF.md) |
| 2. Design principle (applies to all three cards) | LV | [cards/ionosphere/LV-REF.md](cards/ionosphere/LV-REF.md) |
| 3. Card LV-1 — bias-free per-LOS iono seeding  (PRIMARY) | LV-1 | [cards/ionosphere/LV-1-BIAS-FREE-SEEDING.md](cards/ionosphere/LV-1-BIAS-FREE-SEEDING.md) |
| Current behaviour | LV-1 | [cards/ionosphere/LV-1-BIAS-FREE-SEEDING.md](cards/ionosphere/LV-1-BIAS-FREE-SEEDING.md) |
| Change | LV-1 | [cards/ionosphere/LV-1-BIAS-FREE-SEEDING.md](cards/ionosphere/LV-1-BIAS-FREE-SEEDING.md) |
| Events (invariant #1) | LV-1 | [cards/ionosphere/LV-1-BIAS-FREE-SEEDING.md](cards/ionosphere/LV-1-BIAS-FREE-SEEDING.md) |
| Knob, counters, trace | LV-1 | [cards/ionosphere/LV-1-BIAS-FREE-SEEDING.md](cards/ionosphere/LV-1-BIAS-FREE-SEEDING.md) |
| Validation arms + predictions (write these into the configs before running) | LV-1 | [cards/ionosphere/LV-1-BIAS-FREE-SEEDING.md](cards/ionosphere/LV-1-BIAS-FREE-SEEDING.md) |
| 4. Card LV-2 — rx-CB carry common-mode de-mean | LV-2 | [cards/ionosphere/LV-2-RXCB-DEMEAN.md](cards/ionosphere/LV-2-RXCB-DEMEAN.md) |
| Current behaviour (anchors) | LV-2 | [cards/ionosphere/LV-2-RXCB-DEMEAN.md](cards/ionosphere/LV-2-RXCB-DEMEAN.md) |
| Change | LV-2 | [cards/ionosphere/LV-2-RXCB-DEMEAN.md](cards/ionosphere/LV-2-RXCB-DEMEAN.md) |
| Events (invariant #7) | LV-2 | [cards/ionosphere/LV-2-RXCB-DEMEAN.md](cards/ionosphere/LV-2-RXCB-DEMEAN.md) |
| Knob | LV-2 | [cards/ionosphere/LV-2-RXCB-DEMEAN.md](cards/ionosphere/LV-2-RXCB-DEMEAN.md) |
| Validation arms + predictions | LV-2 | [cards/ionosphere/LV-2-RXCB-DEMEAN.md](cards/ionosphere/LV-2-RXCB-DEMEAN.md) |
| 5. Card LV-3 — the level authority, explicit  (mostly documentation; one OPTIONAL knob) | LV-3 | [cards/ionosphere/LV-3-LEVEL-AUTHORITY.md](cards/ionosphere/LV-3-LEVEL-AUTHORITY.md) |
| 6. Build / run mechanics for the implementing session | LV | [cards/ionosphere/LV-REF.md](cards/ionosphere/LV-REF.md) |
| 7. Do NOT (falsified or blocked — do not rediscover) | LV | [cards/ionosphere/LV-REF.md](cards/ionosphere/LV-REF.md) |
| 8. File-touch map | LV | [cards/ionosphere/LV-REF.md](cards/ionosphere/LV-REF.md) |
| 9. Implementation record (2026-08-05) | LV impl | [cards/ionosphere/LV-IMPL-RECORD.md](cards/ionosphere/LV-IMPL-RECORD.md) |
| 10. Measured outcomes — and the mechanism correction (2026-08-05) | LV impl | [cards/ionosphere/LV-IMPL-RECORD.md](cards/ionosphere/LV-IMPL-RECORD.md) |
| 10.1 §1's gradient model is FALSIFIED — the seed cannot pull the level | LV impl | [cards/ionosphere/LV-IMPL-RECORD.md](cards/ionosphere/LV-IMPL-RECORD.md) |
| 10.2 …but LV-1 helps anyway, through a DIFFERENT and defective channel | LV impl | [cards/ionosphere/LV-IMPL-RECORD.md](cards/ionosphere/LV-IMPL-RECORD.md) |
| 10.3 LV-2 works as designed, and the 1 h window is where it shows | LV impl | [cards/ionosphere/LV-IMPL-RECORD.md](cards/ionosphere/LV-IMPL-RECORD.md) |
| 10.4 Predictions that MISSED (stated before the runs, recorded honestly) | LV impl | [cards/ionosphere/LV-IMPL-RECORD.md](cards/ionosphere/LV-IMPL-RECORD.md) |
| 10.5 What is now open | LV impl | [cards/ionosphere/LV-IMPL-RECORD.md](cards/ionosphere/LV-IMPL-RECORD.md) |
| 11. Card LV-4 — the carry screen, made frame-invariant (2026-08-05) | LV-4 | [cards/ionosphere/LV-4-FRAME-INVARIANT-SCREEN.md](cards/ionosphere/LV-4-FRAME-INVARIANT-SCREEN.md) |
| 11.1 The defect | LV-4 | [cards/ionosphere/LV-4-FRAME-INVARIANT-SCREEN.md](cards/ionosphere/LV-4-FRAME-INVARIANT-SCREEN.md) |
| 11.2 The fix | LV-4 | [cards/ionosphere/LV-4-FRAME-INVARIANT-SCREEN.md](cards/ionosphere/LV-4-FRAME-INVARIANT-SCREEN.md) |
| 11.3 Measured — the screen is fixed, and it exposes a second defect | LV-4 | [cards/ionosphere/LV-4-FRAME-INVARIANT-SCREEN.md](cards/ionosphere/LV-4-FRAME-INVARIANT-SCREEN.md) |
| 11.4 Verdict and the next card | LV-4 | [cards/ionosphere/LV-4-FRAME-INVARIANT-SCREEN.md](cards/ionosphere/LV-4-FRAME-INVARIANT-SCREEN.md) |
| 12. Card LV-5 — μ computed within the epoch (2026-08-05) | LV-5 | [cards/ionosphere/LV-5-MU-WITHIN-EPOCH.md](cards/ionosphere/LV-5-MU-WITHIN-EPOCH.md) |
| 12.1 The defect | LV-5 | [cards/ionosphere/LV-5-MU-WITHIN-EPOCH.md](cards/ionosphere/LV-5-MU-WITHIN-EPOCH.md) |
| 12.2 The change | LV-5 | [cards/ionosphere/LV-5-MU-WITHIN-EPOCH.md](cards/ionosphere/LV-5-MU-WITHIN-EPOCH.md) |
| 12.3 Measured | LV-5 | [cards/ionosphere/LV-5-MU-WITHIN-EPOCH.md](cards/ionosphere/LV-5-MU-WITHIN-EPOCH.md) |
| 12.4 Predictions that missed | LV-5 | [cards/ionosphere/LV-5-MU-WITHIN-EPOCH.md](cards/ionosphere/LV-5-MU-WITHIN-EPOCH.md) |
| 12.5 Recommended posture | LV-5 | [cards/ionosphere/LV-5-MU-WITHIN-EPOCH.md](cards/ionosphere/LV-5-MU-WITHIN-EPOCH.md) |
| 13. CORRECTION — what LV-2 actually does, and what LV-1 is really for (2026-08-05) | LV correction | [cards/ionosphere/LV-CORRECTION-AND-12H.md](cards/ionosphere/LV-CORRECTION-AND-12H.md) |
| 13.1 The measurement that broke the story | LV correction | [cards/ionosphere/LV-CORRECTION-AND-12H.md](cards/ionosphere/LV-CORRECTION-AND-12H.md) |
| 13.2 The channel is LV-2 itself: the de-mean is NOT frame-invariant | LV correction | [cards/ionosphere/LV-CORRECTION-AND-12H.md](cards/ionosphere/LV-CORRECTION-AND-12H.md) |
| 13.3 So what IS LV-1 for? | LV correction | [cards/ionosphere/LV-CORRECTION-AND-12H.md](cards/ionosphere/LV-CORRECTION-AND-12H.md) |
| 13.4 What a principled version would need | LV correction | [cards/ionosphere/LV-CORRECTION-AND-12H.md](cards/ionosphere/LV-CORRECTION-AND-12H.md) |
| 13.5 What these arms did NOT measure — the over-retention question is OPEN | LV correction | [cards/ionosphere/LV-CORRECTION-AND-12H.md](cards/ionosphere/LV-CORRECTION-AND-12H.md) |
| 13.6 Status of the claims in this document (see also §14) | LV correction | [cards/ionosphere/LV-CORRECTION-AND-12H.md](cards/ionosphere/LV-CORRECTION-AND-12H.md) |
| 14. THE 12 h VERDICT — the 1 h result was an ignition transient (2026-08-05) | LV correction | [cards/ionosphere/LV-CORRECTION-AND-12H.md](cards/ionosphere/LV-CORRECTION-AND-12H.md) |
| 14.1 The grading changed first | LV correction | [cards/ionosphere/LV-CORRECTION-AND-12H.md](cards/ionosphere/LV-CORRECTION-AND-12H.md) |
| 14.2 What the pair says | LV correction | [cards/ionosphere/LV-CORRECTION-AND-12H.md](cards/ionosphere/LV-CORRECTION-AND-12H.md) |
| 14.3 Why the drift is the expected consequence of §13 | LV correction | [cards/ionosphere/LV-CORRECTION-AND-12H.md](cards/ionosphere/LV-CORRECTION-AND-12H.md) |
| 14.4 Recommendation (supersedes §12.5) | LV correction | [cards/ionosphere/LV-CORRECTION-AND-12H.md](cards/ionosphere/LV-CORRECTION-AND-12H.md) |
| 15. The tracking-pin fix, tested and FALSIFIED (2026-08-05) | LV tracking pin | [cards/ionosphere/LV-TRACKING-PIN.md](cards/ionosphere/LV-TRACKING-PIN.md) |
| 15.1 What that rules out, and what it leaves | LV tracking pin | [cards/ionosphere/LV-TRACKING-PIN.md](cards/ionosphere/LV-TRACKING-PIN.md) |
| 15.2 The next card, now with four arms behind it | LV tracking pin | [cards/ionosphere/LV-TRACKING-PIN.md](cards/ionosphere/LV-TRACKING-PIN.md) |
| 16. LV-5 requirements (review addendum, 2026-08-05 — was mis-numbered §12; kept for the record) | LV-5 | [cards/ionosphere/LV-5-MU-WITHIN-EPOCH.md](cards/ionosphere/LV-5-MU-WITHIN-EPOCH.md) |
| 12.1 The μ vote MUST be robust — LV-5 opens a contamination window LV-4 cannot close | LV-5 | [cards/ionosphere/LV-5-MU-WITHIN-EPOCH.md](cards/ionosphere/LV-5-MU-WITHIN-EPOCH.md) |
| 12.2 Ordering + consistency invariants | LV-5 | [cards/ionosphere/LV-5-MU-WITHIN-EPOCH.md](cards/ionosphere/LV-5-MU-WITHIN-EPOCH.md) |
| 12.3 Theory conformance (answers "is this ad hoc" — it is not) | LV-5 | [cards/ionosphere/LV-5-MU-WITHIN-EPOCH.md](cards/ionosphere/LV-5-MU-WITHIN-EPOCH.md) |
| 12.4 Cold start / born-wrong-static (the residual exposure, named) | LV-5 | [cards/ionosphere/LV-5-MU-WITHIN-EPOCH.md](cards/ionosphere/LV-5-MU-WITHIN-EPOCH.md) |
| 12.5 Grades for the LV-4+LV-5 validation arms (write into the configs) | LV-5 | [cards/ionosphere/LV-5-MU-WITHIN-EPOCH.md](cards/ionosphere/LV-5-MU-WITHIN-EPOCH.md) |
| 17. ROADMAP for the continuing session (2026-08-05, written after §15) | LV roadmap | [cards/ionosphere/LV-ROADMAP.md](cards/ionosphere/LV-ROADMAP.md) |
| 18. Is the residual an EXTRAPOLATION artifact?  No — measured (2026-08-05) | LV residual | [cards/ionosphere/LV-RESIDUAL-EXTRAPOLATION.md](cards/ionosphere/LV-RESIDUAL-EXTRAPOLATION.md) |
| 18.1 And is `rx_cb_carry_q` simply too tight? | LV residual | [cards/ionosphere/LV-RESIDUAL-EXTRAPOLATION.md](cards/ionosphere/LV-RESIDUAL-EXTRAPOLATION.md) |
| 19. Roadmap item 1 FALSIFIED at stage 1, and §13.5's audit re-attributes the whole ladder (20… | LV item 1 | [cards/ionosphere/LV-ITEM1-FALSIFIED.md](cards/ionosphere/LV-ITEM1-FALSIFIED.md) |
| 19.1 The 1 h stability gate | LV item 1 | [cards/ionosphere/LV-ITEM1-FALSIFIED.md](cards/ionosphere/LV-ITEM1-FALSIFIED.md) |
| 19.2 The §13.5 audit — and it does not say what §13.5 predicted | LV item 1 | [cards/ionosphere/LV-ITEM1-FALSIFIED.md](cards/ionosphere/LV-ITEM1-FALSIFIED.md) |
| 19.3 The cost attributes to LV-4, and to LV-4 alone | LV item 1 | [cards/ionosphere/LV-ITEM1-FALSIFIED.md](cards/ionosphere/LV-ITEM1-FALSIFIED.md) |
| 19.4 Two invariant-#4 holes in the audit instrument itself (verified, not inferred) | LV item 1 | [cards/ionosphere/LV-ITEM1-FALSIFIED.md](cards/ionosphere/LV-ITEM1-FALSIFIED.md) |
| 19.5 What this changes | LV item 1 | [cards/ionosphere/LV-ITEM1-FALSIFIED.md](cards/ionosphere/LV-ITEM1-FALSIFIED.md) |
| 19.6 Reconciliation with §17 item 0 (the invariant-#5 course-correction) | LV item 1 | [cards/ionosphere/LV-ITEM1-FALSIFIED.md](cards/ionosphere/LV-ITEM1-FALSIFIED.md) |
| 20. Arm 30 (LV-1+2, no LV-4) and the 303-station EXT-CB arm (2026-08-05) | EXT-CB defect 2 | [cards/ionosphere/LV-EXTCB-DEFECT2.md](cards/ionosphere/LV-EXTCB-DEFECT2.md) |
| 20.1 Arm 30 — the §19 thesis holds | EXT-CB defect 2 | [cards/ionosphere/LV-EXTCB-DEFECT2.md](cards/ionosphere/LV-EXTCB-DEFECT2.md) |
| 20.2 ★ DEFECT 2 is largely a STATION-SUPPORT artifact — measured at 303 stations | EXT-CB defect 2 | [cards/ionosphere/LV-EXTCB-DEFECT2.md](cards/ionosphere/LV-EXTCB-DEFECT2.md) |
| 20.3 Mask posture changed after these runs (user directive, 2026-08-05) | EXT-CB defect 2 | [cards/ionosphere/LV-EXTCB-DEFECT2.md](cards/ionosphere/LV-EXTCB-DEFECT2.md) |
| 21. The mask 2×2 — the level gain and the residual cost come from DIFFERENT knobs (2026-08-05) | LV mask study | [cards/ionosphere/LV-MASK-STUDY.md](cards/ionosphere/LV-MASK-STUDY.md) |
| 21.1 Why this is physically coherent | LV mask study | [cards/ionosphere/LV-MASK-STUDY.md](cards/ionosphere/LV-MASK-STUDY.md) |
| 21.2 Recommendation | LV mask study | [cards/ionosphere/LV-MASK-STUDY.md](cards/ionosphere/LV-MASK-STUDY.md) |
| 22. ★ The mask trade is REGIME-DEPENDENT — it flips sign between 61 and 303 stations (2026-08… | LV mask study | [cards/ionosphere/LV-MASK-STUDY.md](cards/ionosphere/LV-MASK-STUDY.md) |
| 22.1 Why the sign flips — and why it was predictable from the original finding | LV mask study | [cards/ionosphere/LV-MASK-STUDY.md](cards/ionosphere/LV-MASK-STUDY.md) |
| 22.2 Standing recommendation | LV mask study | [cards/ionosphere/LV-MASK-STUDY.md](cards/ionosphere/LV-MASK-STUDY.md) |
| 23. Roadmap item 1, re-run on the CURRENT denominator: §19's FAIL does not reproduce, and LV-… | LV mask study | [cards/ionosphere/LV-MASK-STUDY.md](cards/ionosphere/LV-MASK-STUDY.md) |
| 23.1 Stage 1 at 61 stations, obs 15 / iono 25 — arm 34 | LV mask study | [cards/ionosphere/LV-MASK-STUDY.md](cards/ionosphere/LV-MASK-STUDY.md) |
| 23.2 The mask/scale extreme at 303 stations — arms 35 / 36 | LV mask study | [cards/ionosphere/LV-MASK-STUDY.md](cards/ionosphere/LV-MASK-STUDY.md) |
| 23.3 The finding: LV-4's data-side cost is a function of the observation mask | LV mask study | [cards/ionosphere/LV-MASK-STUDY.md](cards/ionosphere/LV-MASK-STUDY.md) |
| 23.4 The §13.5 / §17-item-2 audit does not resolve | LV mask study | [cards/ionosphere/LV-MASK-STUDY.md](cards/ionosphere/LV-MASK-STUDY.md) |
| 23.5 A third cell for §22's mask axis, and what it implies for §22.2's open question | LV mask study | [cards/ionosphere/LV-MASK-STUDY.md](cards/ionosphere/LV-MASK-STUDY.md) |
| 23.6 Predictions, scored | LV mask study | [cards/ionosphere/LV-MASK-STUDY.md](cards/ionosphere/LV-MASK-STUDY.md) |
| 23.7 Provenance | LV mask study | [cards/ionosphere/LV-MASK-STUDY.md](cards/ionosphere/LV-MASK-STUDY.md) |
| 24. Item-1 STAGE 2 passes every gate — and sharpens the trade to its final form (2026-08-05) | LV mask study | [cards/ionosphere/LV-MASK-STUDY.md](cards/ionosphere/LV-MASK-STUDY.md) |
| 24.1 The stage-2 gate, measured | LV mask study | [cards/ionosphere/LV-MASK-STUDY.md](cards/ionosphere/LV-MASK-STUDY.md) |
| 24.2 The level channel: the best tracker any 61-station posture has shown | LV mask study | [cards/ionosphere/LV-MASK-STUDY.md](cards/ionosphere/LV-MASK-STUDY.md) |
| 24.3 The data side: +32 % persists settled, and every loud instrument agrees | LV mask study | [cards/ionosphere/LV-MASK-STUDY.md](cards/ionosphere/LV-MASK-STUDY.md) |
| 24.4 Predictions, scored | LV mask study | [cards/ionosphere/LV-MASK-STUDY.md](cards/ionosphere/LV-MASK-STUDY.md) |
| 24.5 Standing after stage 2 | LV mask study | [cards/ionosphere/LV-MASK-STUDY.md](cards/ionosphere/LV-MASK-STUDY.md) |
| 25. The excursion attribution: the product wire is MARK-FREE, the churn is posture-invariant,… | LV mask study | [cards/ionosphere/LV-MASK-STUDY.md](cards/ionosphere/LV-MASK-STUDY.md) |
| 25.1 There ARE no R/disc events — the wire carries values and nothing else | LV mask study | [cards/ionosphere/LV-MASK-STUDY.md](cards/ionosphere/LV-MASK-STUDY.md) |
| 25.2 The step census (the disc record, computed after the fact) | LV mask study | [cards/ionosphere/LV-MASK-STUDY.md](cards/ionosphere/LV-MASK-STUDY.md) |
| 25.3 Excursion onsets align with RE-REFERENCING EVENTS, not with value steps | LV mask study | [cards/ionosphere/LV-MASK-STUDY.md](cards/ionosphere/LV-MASK-STUDY.md) |
| 25.4 The h9.5–11.5 storm: common stressor, differential response | LV mask study | [cards/ionosphere/LV-MASK-STUDY.md](cards/ionosphere/LV-MASK-STUDY.md) |
| 25.5 What this changes (and does not) | LV mask study | [cards/ionosphere/LV-MASK-STUDY.md](cards/ionosphere/LV-MASK-STUDY.md) |
| 25.6 Item 0's diagnostic rider, discharged: the fleet-bias mean does NOT drift diurnally | LV mask study | [cards/ionosphere/LV-MASK-STUDY.md](cards/ionosphere/LV-MASK-STUDY.md) |
| 25.7 CORRECTION (2026-08-05, same day): §25.5's central claim was FALSE — the reaction path e… | LV mask study | [cards/ionosphere/LV-MASK-STUDY.md](cards/ionosphere/LV-MASK-STUDY.md) |
| 26. The 305 @ 15/25 cell: a data-side TIE, and the footprint INVERTS the level verdict — the … | LV mask study | [cards/ionosphere/LV-MASK-STUDY.md](cards/ionosphere/LV-MASK-STUDY.md) |
| 26.1 The completed 305 factorial | LV mask study | [cards/ionosphere/LV-MASK-STUDY.md](cards/ionosphere/LV-MASK-STUDY.md) |
| 26.2 The mask study CLOSES: one posture serves both scales | LV mask study | [cards/ionosphere/LV-MASK-STUDY.md](cards/ionosphere/LV-MASK-STUDY.md) |
| 26.3 Predictions, scored | LV mask study | [cards/ionosphere/LV-MASK-STUDY.md](cards/ionosphere/LV-MASK-STUDY.md) |
| 26.4 Instrument note (invariant #4, near-miss) | LV mask study | [cards/ionosphere/LV-MASK-STUDY.md](cards/ionosphere/LV-MASK-STUDY.md) |
| 27. Roadmap refresh — state on hand-back (2026-08-05, end of session) | LV roadmap | [cards/ionosphere/LV-ROADMAP.md](cards/ionosphere/LV-ROADMAP.md) |
| 28. Item 3 BUILT and validated through 3 h — card S2H, the S-consistent harvest (2026-08-07, … | S2H | [cards/code-bias/S2H-HARVEST.md](cards/code-bias/S2H-HARVEST.md) |
| 29. The EXT-CB campaign record, arms 1–36 (absorbed from `HANDOFF_ext_cb_iono_level.md`, 2026… | EXT-CB arms 1–36 | [cards/ionosphere/LV-EXTCB-CAMPAIGN.md](cards/ionosphere/LV-EXTCB-CAMPAIGN.md) |
| 29.1 The goal, and the fixture design rationale | EXT-CB arms 1–36 | [cards/ionosphere/LV-EXTCB-CAMPAIGN.md](cards/ionosphere/LV-EXTCB-CAMPAIGN.md) |
| 29.2 Already built — do not rebuild | EXT-CB arms 1–36 | [cards/ionosphere/LV-EXTCB-CAMPAIGN.md](cards/ionosphere/LV-EXTCB-CAMPAIGN.md) |
| 29.3 Measured — do not re-derive | EXT-CB arms 1–36 | [cards/ionosphere/LV-EXTCB-CAMPAIGN.md](cards/ionosphere/LV-EXTCB-CAMPAIGN.md) |
| 29.4 ELIMINATED — do NOT re-test | EXT-CB arms 1–36 | [cards/ionosphere/LV-EXTCB-CAMPAIGN.md](cards/ionosphere/LV-EXTCB-CAMPAIGN.md) |
| 29.5 ROOT CAUSE (2026-08-04, measured) | EXT-CB arms 1–36 | [cards/ionosphere/LV-EXTCB-CAMPAIGN.md](cards/ionosphere/LV-EXTCB-CAMPAIGN.md) |
| 29.6 The fix and its validation, arms 9–13 — including the L2-ESCAPE | EXT-CB arms 1–36 | [cards/ionosphere/LV-EXTCB-CAMPAIGN.md](cards/ionosphere/LV-EXTCB-CAMPAIGN.md) |
| 29.7 Arm 14 — the level's information budget is ACCIDENTAL | EXT-CB arms 1–36 | [cards/ionosphere/LV-EXTCB-CAMPAIGN.md](cards/ionosphere/LV-EXTCB-CAMPAIGN.md) |
| 29.8 Known-latent, out of scope: the 4-char receiver-OSB card | EXT-CB arms 1–36 | [cards/ionosphere/LV-EXTCB-CAMPAIGN.md](cards/ionosphere/LV-EXTCB-CAMPAIGN.md) |
| 30. CONTRACT — the ionosphere LEVEL and receiver-DCB frame of the emitted products (card LV-3) | LV-3 contract | [cards/wire-contract/LEVEL-CONTRACT.md](cards/wire-contract/LEVEL-CONTRACT.md) |
| 30.1 Why a level needs a declared owner | LV-3 contract | [cards/wire-contract/LEVEL-CONTRACT.md](cards/wire-contract/LEVEL-CONTRACT.md) |
| 30.2 The contract, per posture | LV-3 contract | [cards/wire-contract/LEVEL-CONTRACT.md](cards/wire-contract/LEVEL-CONTRACT.md) |
| 30.3 What is NOT a level authority (paid for, do not retry) | LV-3 contract | [cards/wire-contract/LEVEL-CONTRACT.md](cards/wire-contract/LEVEL-CONTRACT.md) |
| 30.4 The seed is a FRAME, not an authority (LV-1's real scope) | LV-3 contract | [cards/wire-contract/LEVEL-CONTRACT.md](cards/wire-contract/LEVEL-CONTRACT.md) |
| 30.5 What a consumer should be told — MOVED | LV-3 contract | [cards/wire-contract/LEVEL-CONTRACT.md](cards/wire-contract/LEVEL-CONTRACT.md) |
| 30.6 Optional, DEFERRED: an internal a₀₀ climatology from the broadcast stream | LV-3 contract | [cards/wire-contract/LEVEL-CONTRACT.md](cards/wire-contract/LEVEL-CONTRACT.md) |
| 30.7 How to grade a level claim (tooling, 2026-08-05) | LV-3 contract | [cards/wire-contract/LEVEL-CONTRACT.md](cards/wire-contract/LEVEL-CONTRACT.md) |
| Related | LV-3 contract | [cards/wire-contract/LEVEL-CONTRACT.md](cards/wire-contract/LEVEL-CONTRACT.md) |
| §31 The S2H 12 h tracking verdict (2026-08-21) — §28's deferred questions ANSWERED; the level… | S2H | [cards/code-bias/S2H-HARVEST.md](cards/code-bias/S2H-HARVEST.md) |
