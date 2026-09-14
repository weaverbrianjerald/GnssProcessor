# SPEC — Card Q3: the guarded temporal clock anchor (CTA) — SPLIT

`docs/SPEC_clock_temporal_anchor.md` was split on 2026-09-14 — the original is [archive/SPEC_clock_temporal_anchor.md](archive/SPEC_clock_temporal_anchor.md); sections map as follows.

The archived copy is **frozen**: it is kept byte-for-byte as proof and is never appended to. New results go in the card file named below. See [INDEX.md](INDEX.md) for the whole tree and the rules, and [topics/clock.md](topics/clock.md) for this subject.

| section | card id | file |
| --- | --- | --- |
| (title + preamble) | Q3 design | [cards/clock/Q3-ANCHOR-DESIGN.md](cards/clock/Q3-ANCHOR-DESIGN.md) |
| (give the per-satellite clock ESTIMABLE CONTENT temporal stiffness — the estimator-side half … | Q3 design | [cards/clock/Q3-ANCHOR-DESIGN.md](cards/clock/Q3-ANCHOR-DESIGN.md) |
| §1 Measured basis — what the walk actually is | Q3 design | [cards/clock/Q3-ANCHOR-DESIGN.md](cards/clock/Q3-ANCHOR-DESIGN.md) |
| 1.1 Why this is NOT the Jul-31 `sat_clock_q` sweep repeated | Q3 design | [cards/clock/Q3-ANCHOR-DESIGN.md](cards/clock/Q3-ANCHOR-DESIGN.md) |
| §2 Design | Q3 design | [cards/clock/Q3-ANCHOR-DESIGN.md](cards/clock/Q3-ANCHOR-DESIGN.md) |
| 2.1 Stage A — the anchor (what changes in the fold) | Q3 design | [cards/clock/Q3-ANCHOR-DESIGN.md](cards/clock/Q3-ANCHOR-DESIGN.md) |
| 2.2 Per-satellite elapsed time — the latent defect a binding q would detonate | Q3 design | [cards/clock/Q3-ANCHOR-DESIGN.md](cards/clock/Q3-ANCHOR-DESIGN.md) |
| 2.3 Stage B — the guard (the statistic) | Q3 design | [cards/clock/Q3-ANCHOR-DESIGN.md](cards/clock/Q3-ANCHOR-DESIGN.md) |
| 2.4 Stage B — the response (adapt-and-EMIT, both edges marked) | Q3 design | [cards/clock/Q3-ANCHOR-DESIGN.md](cards/clock/Q3-ANCHOR-DESIGN.md) |
| 2.5 Telemetry (standing invariant #4) | Q3 design | [cards/clock/Q3-ANCHOR-DESIGN.md](cards/clock/Q3-ANCHOR-DESIGN.md) |
| 2.6 Fingerprint class | Q3 design | [cards/clock/Q3-ANCHOR-DESIGN.md](cards/clock/Q3-ANCHOR-DESIGN.md) |
| §3 The empirical derivation (two-pass, pre-registered) | Q3 design | [cards/clock/Q3-ANCHOR-DESIGN.md](cards/clock/Q3-ANCHOR-DESIGN.md) |
| §4 Pre-registered gates | Q3 design | [cards/clock/Q3-ANCHOR-DESIGN.md](cards/clock/Q3-ANCHOR-DESIGN.md) |
| §4.1 Read the dev census PIVOT-AWARE — the ceiling row is a gauge | Q3 design | [cards/clock/Q3-ANCHOR-DESIGN.md](cards/clock/Q3-ANCHOR-DESIGN.md) |
| §5 Results | Q3 results | [cards/clock/Q3-ANCHOR-RESULTS.md](cards/clock/Q3-ANCHOR-RESULTS.md) |
| G0 — OFF-path byte-identity: **PASS, maximally** | Q3 results | [cards/clock/Q3-ANCHOR-RESULTS.md](cards/clock/Q3-ANCHOR-RESULTS.md) |
| G1 — 20-epoch ON smoke (`runs/cta_20ep_{ctl,on}`): **PASS** | Q3 results | [cards/clock/Q3-ANCHOR-RESULTS.md](cards/clock/Q3-ANCHOR-RESULTS.md) |
| G2 — 1 h A/B (`runs/cta_1h_{ctl,on}`, one knob + the anchor block): **PASS** | Q3 results | [cards/clock/Q3-ANCHOR-RESULTS.md](cards/clock/Q3-ANCHOR-RESULTS.md) |
| §3 pass 1 — the binding knee, MEASURED | Q3 results | [cards/clock/Q3-ANCHOR-RESULTS.md](cards/clock/Q3-ANCHOR-RESULTS.md) |
| Pre-registered prediction that did NOT verify | Q3 results | [cards/clock/Q3-ANCHOR-RESULTS.md](cards/clock/Q3-ANCHOR-RESULTS.md) |
| G3 — the 3-arm 3 h ladder (2026-08-10) | Q3 results | [cards/clock/Q3-ANCHOR-RESULTS.md](cards/clock/Q3-ANCHOR-RESULTS.md) |
| The FC-1 shadow hour table — the score, and it says NO | Q3 results | [cards/clock/Q3-ANCHOR-RESULTS.md](cards/clock/Q3-ANCHOR-RESULTS.md) |
| Not yet run | Q3 results | [cards/clock/Q3-ANCHOR-RESULTS.md](cards/clock/Q3-ANCHOR-RESULTS.md) |
| §6 Relationship to the surrounding cards | Q3 results | [cards/clock/Q3-ANCHOR-RESULTS.md](cards/clock/Q3-ANCHOR-RESULTS.md) |
| §7 (2026-08-10, Fable) THE PHYSICAL FLOOR MEASURED — trap 5 resolved, the FC-1 gate RETIRED, … | Q3 floor | [cards/clock/Q3-PHYSICAL-FLOOR.md](cards/clock/Q3-PHYSICAL-FLOOR.md) |
| §8 (2026-08-10/11) THE FLOOR-GOVERNED LADDER — floors derived, the guard's denominator bisect… | Q3 ladder | [cards/clock/Q3-FLOOR-GOVERNED-LADDER.md](cards/clock/Q3-FLOOR-GOVERNED-LADDER.md) |
| §8.1 The floors, DERIVED — not tuned | Q3 ladder | [cards/clock/Q3-FLOOR-GOVERNED-LADDER.md](cards/clock/Q3-FLOOR-GOVERNED-LADDER.md) |
| §8.2 The owed bisection — `network.clock_anchor_gauge_free_stat` | Q3 ladder | [cards/clock/Q3-FLOOR-GOVERNED-LADDER.md](cards/clock/Q3-FLOOR-GOVERNED-LADDER.md) |
| §8.3 The gauge-aware σ̂₀_clk, and the guard's own ladder | Q3 ladder | [cards/clock/Q3-FLOOR-GOVERNED-LADDER.md](cards/clock/Q3-FLOOR-GOVERNED-LADDER.md) |
| §8.4 Pre-registered gates for this leg (written before any 3 h arm was read) | Q3 ladder | [cards/clock/Q3-FLOOR-GOVERNED-LADDER.md](cards/clock/Q3-FLOOR-GOVERNED-LADDER.md) |
| §8.5 Provenance — all four identity gates PASS | Q3 ladder | [cards/clock/Q3-FLOOR-GOVERNED-LADDER.md](cards/clock/Q3-FLOOR-GOVERNED-LADDER.md) |
| §8.6 The bisection — RESOLVED, and it inverts §5's reading | Q3 ladder | [cards/clock/Q3-FLOOR-GOVERNED-LADDER.md](cards/clock/Q3-FLOOR-GOVERNED-LADDER.md) |
| §8.7 Rung 0 — the derived floors are a MEASURED no-op at the top rung | Q3 ladder | [cards/clock/Q3-FLOOR-GOVERNED-LADDER.md](cards/clock/Q3-FLOOR-GOVERNED-LADDER.md) |
| §8.8 The descent — every PRODUCER gate improves monotonically to 1e-7; the boundary is in the… | Q3 ladder | [cards/clock/Q3-FLOOR-GOVERNED-LADDER.md](cards/clock/Q3-FLOOR-GOVERNED-LADDER.md) |
| §8.9 k re-derived per rung, on the UN-TRUNCATED null — and the 2 m rail is INERT | Q3 ladder | [cards/clock/Q3-FLOOR-GOVERNED-LADDER.md](cards/clock/Q3-FLOOR-GOVERNED-LADDER.md) |
| §8.10 The replay instrument — the boundary is a CLIFF, and both consumers are sitting on it | Q3 ladder | [cards/clock/Q3-FLOOR-GOVERNED-LADDER.md](cards/clock/Q3-FLOOR-GOVERNED-LADDER.md) |
| §8.11 The consumer AR blunder — DIAGNOSED and FIXED, and the first two diagnoses were wrong | consumer AR | [cards/consumer/CONSUMER-AR-BLUNDER.md](cards/consumer/CONSUMER-AR-BLUNDER.md) |
| §8.11.1 What `ratio = inf` actually is — §8.8's reading was WRONG | consumer AR | [cards/consumer/CONSUMER-AR-BLUNDER.md](cards/consumer/CONSUMER-AR-BLUNDER.md) |
| §8.11.2 The FFR gate — necessary, correct, and it does NOT catch this | consumer AR | [cards/consumer/CONSUMER-AR-BLUNDER.md](cards/consumer/CONSUMER-AR-BLUNDER.md) |
| §8.11.3 The mechanism, by elimination | consumer AR | [cards/consumer/CONSUMER-AR-BLUNDER.md](cards/consumer/CONSUMER-AR-BLUNDER.md) |
| §8.11.4 The gate that does catch it — the innovation screen | consumer AR | [cards/consumer/CONSUMER-AR-BLUNDER.md](cards/consumer/CONSUMER-AR-BLUNDER.md) |
| §8.11.5 ⇒ §8.8's boundary is DISSOLVED | consumer AR | [cards/consumer/CONSUMER-AR-BLUNDER.md](cards/consumer/CONSUMER-AR-BLUNDER.md) |
| §8.11.6 Remaining gaps, named not hidden | consumer AR | [cards/consumer/CONSUMER-AR-BLUNDER.md](cards/consumer/CONSUMER-AR-BLUNDER.md) |
| §8.11.7 The NlWl sibling — both guards applied | consumer AR | [cards/consumer/CONSUMER-AR-BLUNDER.md](cards/consumer/CONSUMER-AR-BLUNDER.md) |
| §8.11.8 The NlWl guards, FIELD-EXERCISED — and the path was destroying the solution | consumer AR | [cards/consumer/CONSUMER-AR-BLUNDER.md](cards/consumer/CONSUMER-AR-BLUNDER.md) |
| §8.11.9 The consumer POSTURE — measured, and the WL-only choice was a drag all along | consumer AR | [cards/consumer/CONSUMER-AR-BLUNDER.md](cards/consumer/CONSUMER-AR-BLUNDER.md) |
| §8.11.10 The "broken consumer logger" — claim WITHDRAWN, cause NOT established | consumer AR | [cards/consumer/CONSUMER-AR-BLUNDER.md](cards/consumer/CONSUMER-AR-BLUNDER.md) |
| §9 (2026-08-31) G4's SIM HALF EXECUTED — the anchor truth-graded on the 12 h quiet flagship, … | Q3 G4 | [cards/clock/Q3-G4-SIM-HALF.md](cards/clock/Q3-G4-SIM-HALF.md) |
| §9.1 (2026-09-01) THE SIM q-LADDER — 3e-6 confirmed a LOCAL OPTIMUM on truth, from both sides | Q3 G4 | [cards/clock/Q3-G4-SIM-HALF.md](cards/clock/Q3-G4-SIM-HALF.md) |
