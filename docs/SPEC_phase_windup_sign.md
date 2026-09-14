# SPEC — the carrier-phase wind-up SIGN defect (and the wrapped-mode wrap) — SPLIT

`docs/SPEC_phase_windup_sign.md` was split on 2026-09-14 — the original is [archive/SPEC_phase_windup_sign.md](archive/SPEC_phase_windup_sign.md); sections map as follows.

The archived copy is **frozen**: it is kept byte-for-byte as proof and is never appended to. New results go in the card file named below. See [INDEX.md](INDEX.md) for the whole tree and the rules, and [topics/network-vehicle.md](topics/network-vehicle.md) for this subject.

| section | card id | file |
| --- | --- | --- |
| (title + preamble) | wind-up sign | [cards/network-vehicle/WINDUP-SIGN-DEFECT.md](cards/network-vehicle/WINDUP-SIGN-DEFECT.md) |
| §1 The defect | wind-up sign | [cards/network-vehicle/WINDUP-SIGN-DEFECT.md](cards/network-vehicle/WINDUP-SIGN-DEFECT.md) |
| Three independent confirmations | wind-up sign | [cards/network-vehicle/WINDUP-SIGN-DEFECT.md](cards/network-vehicle/WINDUP-SIGN-DEFECT.md) |
| §1a ⚠ "exactly `−φ_std`" is NOT exact — the prose overstated the table | wind-up sign | [cards/network-vehicle/WINDUP-SIGN-DEFECT.md](cards/network-vehicle/WINDUP-SIGN-DEFECT.md) |
| §1b Correction — `ModelGeometry.cpp:742,748` are NOT live wind-up sites | wind-up sign | [cards/network-vehicle/WINDUP-SIGN-DEFECT.md](cards/network-vehicle/WINDUP-SIGN-DEFECT.md) |
| §1c Two further dead ends found while auditing the consumers | wind-up sign | [cards/network-vehicle/WINDUP-SIGN-DEFECT.md](cards/network-vehicle/WINDUP-SIGN-DEFECT.md) |
| §2 Why nothing caught it | wind-up sign | [cards/network-vehicle/WINDUP-SIGN-DEFECT.md](cards/network-vehicle/WINDUP-SIGN-DEFECT.md) |
| §3 ⚠ THE FIX IS BOTH LINES, TOGETHER — a half-fix is worse than the defect | wind-up sign | [cards/network-vehicle/WINDUP-SIGN-DEFECT.md](cards/network-vehicle/WINDUP-SIGN-DEFECT.md) |
| §3a ⚠ THE THIRD SITE — the velocity-fallback branch, where the old `+` was RIGHT | wind-up sign | [cards/network-vehicle/WINDUP-SIGN-DEFECT.md](cards/network-vehicle/WINDUP-SIGN-DEFECT.md) |
| §3b ⚠ THE YAW-TURN RESPONSE PASSES A HALF-FIX — it cannot be the acceptance gate | wind-up sign | [cards/network-vehicle/WINDUP-SIGN-DEFECT.md](cards/network-vehicle/WINDUP-SIGN-DEFECT.md) |
| §3c The 0.2512 / 0.2522 figures are NOT constants | wind-up sign | [cards/network-vehicle/WINDUP-SIGN-DEFECT.md](cards/network-vehicle/WINDUP-SIGN-DEFECT.md) |
| §4 Impact — where it lands, and where it does not | wind-up sign | [cards/network-vehicle/WINDUP-SIGN-DEFECT.md](cards/network-vehicle/WINDUP-SIGN-DEFECT.md) |
| §4a ⚠ The 79 mm figure is right for the WIRE and wrong for the ESTIMATOR | wind-up sign | [cards/network-vehicle/WINDUP-SIGN-DEFECT.md](cards/network-vehicle/WINDUP-SIGN-DEFECT.md) |
| §5 It is NOT the §7 common-cycle carrier | wind-up sign | [cards/network-vehicle/WINDUP-SIGN-DEFECT.md](cards/network-vehicle/WINDUP-SIGN-DEFECT.md) |
| §6 The §7 lead does not survive — "equal in cycles" is not a separate direction | wind-up sign | [cards/network-vehicle/WINDUP-SIGN-DEFECT.md](cards/network-vehicle/WINDUP-SIGN-DEFECT.md) |
| §7 The second, separate defect — fractional-mode wrapping on every field lineage | wind-up sign | [cards/network-vehicle/WINDUP-SIGN-DEFECT.md](cards/network-vehicle/WINDUP-SIGN-DEFECT.md) |
| §8 What was landed | wind-up sign | [cards/network-vehicle/WINDUP-SIGN-DEFECT.md](cards/network-vehicle/WINDUP-SIGN-DEFECT.md) |
| §8.1 The code — FOUR sites, one commit | wind-up sign | [cards/network-vehicle/WINDUP-SIGN-DEFECT.md](cards/network-vehicle/WINDUP-SIGN-DEFECT.md) |
| §8.2 Invariant #6 — this is a product-frame change and CANNOT be byte-identical | wind-up sign | [cards/network-vehicle/WINDUP-SIGN-DEFECT.md](cards/network-vehicle/WINDUP-SIGN-DEFECT.md) |
| §8.3 The product contract — no new legend field; the existing one was INERT | wind-up sign | [cards/network-vehicle/WINDUP-SIGN-DEFECT.md](cards/network-vehicle/WINDUP-SIGN-DEFECT.md) |
| §8.4 Warm resume across the fix — invariant #1 + #7, and it was SILENT | wind-up sign | [cards/network-vehicle/WINDUP-SIGN-DEFECT.md](cards/network-vehicle/WINDUP-SIGN-DEFECT.md) |
| §8.5 Re-grading — what must be annotated | wind-up sign | [cards/network-vehicle/WINDUP-SIGN-DEFECT.md](cards/network-vehicle/WINDUP-SIGN-DEFECT.md) |
| §9 THE FIELD RESULT — the three-arm ladder, 2026-08-21 | wind-up field | [cards/network-vehicle/WINDUP-FIELD-LADDER.md](cards/network-vehicle/WINDUP-FIELD-LADDER.md) |
| §9.1 Why the ladder, and not "ON must be worse than OFF" | wind-up field | [cards/network-vehicle/WINDUP-FIELD-LADDER.md](cards/network-vehicle/WINDUP-FIELD-LADDER.md) |
| §9.2 Controls — both pass | wind-up field | [cards/network-vehicle/WINDUP-FIELD-LADDER.md](cards/network-vehicle/WINDUP-FIELD-LADDER.md) |
| §9.3 Result — slope lands on 2, never on 0 | wind-up field | [cards/network-vehicle/WINDUP-FIELD-LADDER.md](cards/network-vehicle/WINDUP-FIELD-LADDER.md) |
| §9.4 ⚠ Why the second difference is NOT at the numerical floor — arm-dependent editing | wind-up field | [cards/network-vehicle/WINDUP-FIELD-LADDER.md](cards/network-vehicle/WINDUP-FIELD-LADDER.md) |
| §9.5 ⚠ The sign fix and the §7 WRAP are coupled in the deliverable, though independent in mec… | wind-up field | [cards/network-vehicle/WINDUP-FIELD-LADDER.md](cards/network-vehicle/WINDUP-FIELD-LADDER.md) |
| §9.6 What this run does and does NOT certify | wind-up field | [cards/network-vehicle/WINDUP-FIELD-LADDER.md](cards/network-vehicle/WINDUP-FIELD-LADDER.md) |
| §9.7 Replication + extension — the 5-station 4-arm matrix (2026-08-21, second session) | wind-up field | [cards/network-vehicle/WINDUP-FIELD-LADDER.md](cards/network-vehicle/WINDUP-FIELD-LADDER.md) |
| §9.8 §8 item 2 CLOSED — the contract now covers every consumer path | wind-up field | [cards/network-vehicle/WINDUP-FIELD-LADDER.md](cards/network-vehicle/WINDUP-FIELD-LADDER.md) |
| §9.9 The sim is frame-consistent again — fixtures regenerated | wind-up field | [cards/network-vehicle/WINDUP-FIELD-LADDER.md](cards/network-vehicle/WINDUP-FIELD-LADDER.md) |
| §9.10 The NETWORK-producer arm (§9.6 item 1) — EXECUTED 2026-08-22 | wind-up field | [cards/network-vehicle/WINDUP-FIELD-LADDER.md](cards/network-vehicle/WINDUP-FIELD-LADDER.md) |
| §10 The fractional-mode CENSUS — §7's premise gets an instrument (2026-08-22) | wind-up census | [cards/slip/WINDUP-FRACTIONAL-CENSUS.md](cards/slip/WINDUP-FRACTIONAL-CENSUS.md) |
| §10.1 The accumulator is NOT structurally broken by its key — verified | wind-up census | [cards/slip/WINDUP-FRACTIONAL-CENSUS.md](cards/slip/WINDUP-FRACTIONAL-CENSUS.md) |
| §10.2 The real blocker: the two continuity horizons are INVERTED | wind-up census | [cards/slip/WINDUP-FRACTIONAL-CENSUS.md](cards/slip/WINDUP-FRACTIONAL-CENSUS.md) |
| §10.3 What landed — `estimation.trace_windup_census` | wind-up census | [cards/slip/WINDUP-FRACTIONAL-CENSUS.md](cards/slip/WINDUP-FRACTIONAL-CENSUS.md) |
| §10.4 Pre-registered reading of the first armed run | wind-up census | [cards/slip/WINDUP-FRACTIONAL-CENSUS.md](cards/slip/WINDUP-FRACTIONAL-CENSUS.md) |
| §10.5 The census EXECUTED on the field lineage (2026-08-22) — both numbers measured | wind-up census | [cards/slip/WINDUP-FRACTIONAL-CENSUS.md](cards/slip/WINDUP-FRACTIONAL-CENSUS.md) |
| §11 The §10.4 RESTART RESPONSE — BUILT (2026-08-23, default-OFF) | wind-up census | [cards/slip/WINDUP-FRACTIONAL-CENSUS.md](cards/slip/WINDUP-FRACTIONAL-CENSUS.md) |
