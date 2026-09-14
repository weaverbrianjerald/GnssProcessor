# SPEC — the AMBIGUITY / IONO SPLIT OVER-COUNT — SPLIT

`docs/SPEC_amb_iono_split_overcount.md` was split on 2026-09-14 — the original is [archive/SPEC_amb_iono_split_overcount.md](archive/SPEC_amb_iono_split_overcount.md); sections map as follows.

The archived copy is **frozen**: it is kept byte-for-byte as proof and is never appended to. New results go in the card file named below. See [INDEX.md](INDEX.md) for the whole tree and the rules, and [topics/ionosphere.md](topics/ionosphere.md) for this subject.

| section | card id | file |
| --- | --- | --- |
| (title + preamble) | AMB-IONO | [cards/ionosphere/AMB-IONO-SPLIT.md](cards/ionosphere/AMB-IONO-SPLIT.md) |
| (why the network producer never reaches per-band integers on the QUIET simulated bed: B indep… | AMB-IONO | [cards/ionosphere/AMB-IONO-SPLIT.md](cards/ionosphere/AMB-IONO-SPLIT.md) |
| §0 READ THIS FIRST — status, and the verdict | AMB-IONO | [cards/ionosphere/AMB-IONO-SPLIT.md](cards/ionosphere/AMB-IONO-SPLIT.md) |
| §1 The bed and the fresh 24 h grade | AMB-IONO | [cards/ionosphere/AMB-IONO-SPLIT.md](cards/ionosphere/AMB-IONO-SPLIT.md) |
| §2 The five measurements | AMB-IONO | [cards/ionosphere/AMB-IONO-SPLIT.md](cards/ionosphere/AMB-IONO-SPLIT.md) |
| 2.1 The DD error IS the per-LOS ambiguity/iono split (MEASURED) | AMB-IONO | [cards/ionosphere/AMB-IONO-SPLIT.md](cards/ionosphere/AMB-IONO-SPLIT.md) |
| 2.2 Time structure: a level frozen at birth, not noise, not a walk (MEASURED) | AMB-IONO | [cards/ionosphere/AMB-IONO-SPLIT.md](cards/ionosphere/AMB-IONO-SPLIT.md) |
| 2.3 Responses are exonerated (MEASURED) | AMB-IONO | [cards/ionosphere/AMB-IONO-SPLIT.md](cards/ionosphere/AMB-IONO-SPLIT.md) |
| 2.4 The formal σ collapses geometrically while the value is frozen (MEASURED) | AMB-IONO | [cards/ionosphere/AMB-IONO-SPLIT.md](cards/ionosphere/AMB-IONO-SPLIT.md) |
| 2.5 CRLB vs measured: K_eff ≈ 2–3 epochs (MEASURED / DERIVED) | AMB-IONO | [cards/ionosphere/AMB-IONO-SPLIT.md](cards/ionosphere/AMB-IONO-SPLIT.md) |
| 2.6 The truth-iono channel: the LOS's own code never reaches the level (MEASURED) | AMB-IONO | [cards/ionosphere/AMB-IONO-SPLIT.md](cards/ionosphere/AMB-IONO-SPLIT.md) |
| 2.7 Round-2 amendments to §2 (adversarial verification, 2026-09-04) | AMB-IONO | [cards/ionosphere/AMB-IONO-SPLIT.md](cards/ionosphere/AMB-IONO-SPLIT.md) |
| §3 The mechanism (DERIVED; READ citations from the code lens) | AMB-IONO | [cards/ionosphere/AMB-IONO-SPLIT.md](cards/ionosphere/AMB-IONO-SPLIT.md) |
| 3.1 The station-common channel (round 2, MEASURED) — the residue F1 leaves | AMB-IONO | [cards/ionosphere/AMB-IONO-SPLIT.md](cards/ionosphere/AMB-IONO-SPLIT.md) |
| §4 Falsified — do not retry (each with the closing measurement) | AMB-IONO | [cards/ionosphere/AMB-IONO-SPLIT.md](cards/ionosphere/AMB-IONO-SPLIT.md) |
| §5 The fix ladder, with the invariant #1 / #7 edges | AMB-IONO | [cards/ionosphere/AMB-IONO-SPLIT.md](cards/ionosphere/AMB-IONO-SPLIT.md) |
| 5.1 F3 specification — `network.phase_leveling_local_block` (bool, default false) | AMB-IONO | [cards/ionosphere/AMB-IONO-SPLIT.md](cards/ionosphere/AMB-IONO-SPLIT.md) |
| §6 PRE-REGISTERED predictions (written 2026-09-04 BEFORE any round-2 result was read) | F3 (pre-reg) | [cards/ionosphere/AMB-IONO-F3-PREDICTIONS.md](cards/ionosphere/AMB-IONO-F3-PREDICTIONS.md) |
| 6.1 The window expectation must come from the CRLB curve, not the 24 h mixture | F3 (pre-reg) | [cards/ionosphere/AMB-IONO-F3-PREDICTIONS.md](cards/ionosphere/AMB-IONO-F3-PREDICTIONS.md) |
| 6.2 The var-floor arithmetic (M6 with a cap) | F3 (pre-reg) | [cards/ionosphere/AMB-IONO-F3-PREDICTIONS.md](cards/ionosphere/AMB-IONO-F3-PREDICTIONS.md) |
| 6.3 The prediction table (L1C unless stated; "settled" = last 25 % of the window) | F3 (pre-reg) | [cards/ionosphere/AMB-IONO-F3-PREDICTIONS.md](cards/ionosphere/AMB-IONO-F3-PREDICTIONS.md) |
| 6.4 Falsifiers — what refutes the mechanism | F3 (pre-reg) | [cards/ionosphere/AMB-IONO-F3-PREDICTIONS.md](cards/ionosphere/AMB-IONO-F3-PREDICTIONS.md) |
| 6.5 What the arms cannot decide | F3 (pre-reg) | [cards/ionosphere/AMB-IONO-F3-PREDICTIONS.md](cards/ionosphere/AMB-IONO-F3-PREDICTIONS.md) |
| 6.6 Provenance of the numbers above | F3 (pre-reg) | [cards/ionosphere/AMB-IONO-F3-PREDICTIONS.md](cards/ionosphere/AMB-IONO-F3-PREDICTIONS.md) |
| 6.7 Results — AMENDMENT (2026-09-04, round 2; §6.3 untouched) | F3 (pre-reg) | [cards/ionosphere/AMB-IONO-F3-PREDICTIONS.md](cards/ionosphere/AMB-IONO-F3-PREDICTIONS.md) |
| 6.8 F3 results — AMENDMENT (2026-09-04, round 4; §6.3 and §6.7 untouched) | F3 (pre-reg) | [cards/ionosphere/AMB-IONO-F3-PREDICTIONS.md](cards/ionosphere/AMB-IONO-F3-PREDICTIONS.md) |
| §6.9 The 24 h F3 arm (2026-09-05, `runs/nonint_losblock24h_hv`: F3 + handover ceiling 1e-2, 2… | F3 24 h | [cards/ionosphere/AMB-IONO-F3-24H.md](cards/ionosphere/AMB-IONO-F3-24H.md) |
| §6.10 The RE-ADMISSION EDGE card — BUILT 2026-09-05 (E2 seed-first re-admission + the block r… | READMIT | [cards/network-vehicle/READMIT-EDGE.md](cards/network-vehicle/READMIT-EDGE.md) |
| §6.11 Results of the §6.10 arms (filled as they land; nothing in §6.10 is edited) | READMIT | [cards/network-vehicle/READMIT-EDGE.md](cards/network-vehicle/READMIT-EDGE.md) |
| §6.12 The "post-eclipse network-wide DD episode" was the INSTRUMENT'S REFERENCE SATELLITE (20… | DD-REF | [cards/sim-bed/DD-TRACE-REF-SAT.md](cards/sim-bed/DD-TRACE-REF-SAT.md) |
| 6.12.1 The instrument | DD-REF | [cards/sim-bed/DD-TRACE-REF-SAT.md](cards/sim-bed/DD-TRACE-REF-SAT.md) |
| 6.12.2 The decisive test — re-referencing in post-processing | DD-REF | [cards/sim-bed/DD-TRACE-REF-SAT.md](cards/sim-bed/DD-TRACE-REF-SAT.md) |
| 6.12.3 Corrected readings of the §6.11 arms (ref-free, `simnet_dd_reref.py`, stride 8) | DD-REF | [cards/sim-bed/DD-TRACE-REF-SAT.md](cards/sim-bed/DD-TRACE-REF-SAT.md) |
| 6.12.4 What the field inherits | DD-REF | [cards/sim-bed/DD-TRACE-REF-SAT.md](cards/sim-bed/DD-TRACE-REF-SAT.md) |
| 6.12.5 Fixes | DD-REF | [cards/sim-bed/DD-TRACE-REF-SAT.md](cards/sim-bed/DD-TRACE-REF-SAT.md) |
| 6.13 The FIELD re-grade, and the producer knob (2026-09-06) | DD-REF | [cards/sim-bed/DD-TRACE-REF-SAT.md](cards/sim-bed/DD-TRACE-REF-SAT.md) |
| §6.14 The IF-TD reference gate: 100 % false, and the fix was already in the goldens (2026-09-08) | IF-TD ref gate | [cards/slip/IFTD-REF-GATE.md](cards/slip/IFTD-REF-GATE.md) |
| 6.14.1 Ground truth: there is nothing to detect | IF-TD ref gate | [cards/slip/IFTD-REF-GATE.md](cards/slip/IFTD-REF-GATE.md) |
| 6.14.2 The statistic is pure common mode, and it is the reference satellite's own noise | IF-TD ref gate | [cards/slip/IFTD-REF-GATE.md](cards/slip/IFTD-REF-GATE.md) |
| 6.14.3 It is a FIXTURE-REALISM artifact — the field is 5× quieter | IF-TD ref gate | [cards/slip/IFTD-REF-GATE.md](cards/slip/IFTD-REF-GATE.md) |
| 6.14.4 The A/B — Card R takes the acting refloats to ZERO | IF-TD ref gate | [cards/slip/IFTD-REF-GATE.md](cards/slip/IFTD-REF-GATE.md) |
| 6.14.5 What the pool arm can and cannot detect | IF-TD ref gate | [cards/slip/IFTD-REF-GATE.md](cards/slip/IFTD-REF-GATE.md) |
| 6.14.6 Both fixed, and the two-arm A/B that says so (2026-09-08) | IF-TD ref gate | [cards/slip/IFTD-REF-GATE.md](cards/slip/IFTD-REF-GATE.md) |
| §6.15 The FIELD F3 arm — PRE-REGISTERED 2026-09-09 (§7 item 21; campaign queue item 3's linea… | F3 field | [cards/network-vehicle/F3-FIELD-ARM.md](cards/network-vehicle/F3-FIELD-ARM.md) |
| 6.15.1 Bed, arms, provenance | F3 field | [cards/network-vehicle/F3-FIELD-ARM.md](cards/network-vehicle/F3-FIELD-ARM.md) |
| 6.15.2 Instruments (all truth-free; the field has no `truth_ambiguities.csv`) | F3 field | [cards/network-vehicle/F3-FIELD-ARM.md](cards/network-vehicle/F3-FIELD-ARM.md) |
| 6.15.3 Pre-registered predictions (written before any 12 h arm ran; smokes only) | F3 field | [cards/network-vehicle/F3-FIELD-ARM.md](cards/network-vehicle/F3-FIELD-ARM.md) |
| 6.15.3a Pre-data amendments (2026-09-09 17:45–18:00, written while arm 0 was still running an… | F3 field | [cards/network-vehicle/F3-FIELD-ARM.md](cards/network-vehicle/F3-FIELD-ARM.md) |
| 6.15.4 Results — filled as they land (nothing above is edited) | F3 field (results) | [cards/network-vehicle/F3-FIELD-ARM-RESULTS.md](cards/network-vehicle/F3-FIELD-ARM-RESULTS.md) |
| §6.14.6 The follow-up build (2026-09-09) — abstain while cold, normalize the bar, close the g… | IF-TD ref gate (v2) | [cards/slip/IFTD-REF-GATE-FOLLOWUP.md](cards/slip/IFTD-REF-GATE-FOLLOWUP.md) |
| 6.14.6.1 Item 1 — ABSTAIN while the empirical arm is cold | IF-TD ref gate (v2) | [cards/slip/IFTD-REF-GATE-FOLLOWUP.md](cards/slip/IFTD-REF-GATE-FOLLOWUP.md) |
| 6.14.6.2 Item 2 — the bar has to track the REFERENCE's elevation | IF-TD ref gate (v2) | [cards/slip/IFTD-REF-GATE-FOLLOWUP.md](cards/slip/IFTD-REF-GATE-FOLLOWUP.md) |
| 6.14.6.3 Item 3 — the nuisance gate, and the leak it exposed | IF-TD ref gate (v2) | [cards/slip/IFTD-REF-GATE-FOLLOWUP.md](cards/slip/IFTD-REF-GATE-FOLLOWUP.md) |
| 6.14.6.4 Item 4 — the bench mostly EXISTS; the gap was elevation | IF-TD ref gate (v2) | [cards/slip/IFTD-REF-GATE-FOLLOWUP.md](cards/slip/IFTD-REF-GATE-FOLLOWUP.md) |
| 6.14.6.5 The three-arm A/B — item 1 confirmed, item 2 COSTS on this bed | IF-TD ref gate (v2) | [cards/slip/IFTD-REF-GATE-FOLLOWUP.md](cards/slip/IFTD-REF-GATE-FOLLOWUP.md) |
| 6.14.6.6 What is NOT validated here | IF-TD ref gate (v2) | [cards/slip/IFTD-REF-GATE-FOLLOWUP.md](cards/slip/IFTD-REF-GATE-FOLLOWUP.md) |
| §6.16 The ZWD sink census, the slipped-bed golden, and where the integer information is (2026… | ZWD sink | [cards/sim-bed/ZWD-SINK-CENSUS.md](cards/sim-bed/ZWD-SINK-CENSUS.md) |
| 6.16.1 The instrument — settled per-station ZWD span | ZWD sink | [cards/sim-bed/ZWD-SINK-CENSUS.md](cards/sim-bed/ZWD-SINK-CENSUS.md) |
| 6.16.2 BSHM00ISR is a phase-side receiver fault — and roster edits are not the lever | ZWD sink | [cards/sim-bed/ZWD-SINK-CENSUS.md](cards/sim-bed/ZWD-SINK-CENSUS.md) |
| 6.16.3 The golden posture on the SLIPPED sim bed — FAIL on all three gates | ZWD sink | [cards/sim-bed/ZWD-SINK-CENSUS.md](cards/sim-bed/ZWD-SINK-CENSUS.md) |
| 6.16.4 E1 / E2 — two separable halves, and neither knob is the fix | ZWD sink | [cards/sim-bed/ZWD-SINK-CENSUS.md](cards/sim-bed/ZWD-SINK-CENSUS.md) |
| 6.16.5 The HEAD baseline on the quiet bed (PARTIAL, 18.6 h) — the ~5 cm KPI holds | ZWD sink | [cards/sim-bed/ZWD-SINK-CENSUS.md](cards/sim-bed/ZWD-SINK-CENSUS.md) |
| 6.16.6 Where the integer information is — the P records carry none, and that is gauge-ambiguous | ZWD sink | [cards/sim-bed/ZWD-SINK-CENSUS.md](cards/sim-bed/ZWD-SINK-CENSUS.md) |
| §6.16.7 Late results (2026-09-10 00:00–02:30) | ZWD sink | [cards/sim-bed/ZWD-SINK-CENSUS.md](cards/sim-bed/ZWD-SINK-CENSUS.md) |
| §6.17 The 2026-09-10 morning — the same-build control, three truth censuses, two falsified ca… | 0910 morning | [cards/ionosphere/AMB-IONO-0910-MORNING.md](cards/ionosphere/AMB-IONO-0910-MORNING.md) |
| 6.17a The F2 same-build control — the A/B is same-build by proof | 0910 morning | [cards/ionosphere/AMB-IONO-0910-MORNING.md](cards/ionosphere/AMB-IONO-0910-MORNING.md) |
| 6.17b The T4 erase census, truth-graded — T4 re-erases the damage its own previous re-level c… | 0910 morning | [cards/ionosphere/AMB-IONO-0910-MORNING.md](cards/ionosphere/AMB-IONO-0910-MORNING.md) |
| 6.17c The consumer's float, joined to truth — the lanes are PRECISE and the offset is a BIAS | 0910 morning | [cards/ionosphere/AMB-IONO-0910-MORNING.md](cards/ionosphere/AMB-IONO-0910-MORNING.md) |
| 6.17d The wire, joined to truth directly — every channel is at the null, and the gauge is the… | 0910 morning | [cards/ionosphere/AMB-IONO-0910-MORNING.md](cards/ionosphere/AMB-IONO-0910-MORNING.md) |
| 6.17e The ORACLE slip response — the mechanism is exact, the plateau does not move, and that … | 0910 morning | [cards/ionosphere/AMB-IONO-0910-MORNING.md](cards/ionosphere/AMB-IONO-0910-MORNING.md) |
| 6.17f PB-gauge LEVELLING — FALSIFIED as built (branch `worktree-agent-a7e2586003a605548`, com… | 0910 morning | [cards/ionosphere/AMB-IONO-0910-MORNING.md](cards/ionosphere/AMB-IONO-0910-MORNING.md) |
| 6.17g The FIELD after the ZWD fix — one wandering direction, and phase biases that step by ME… | 0910 morning | [cards/ionosphere/AMB-IONO-0910-MORNING.md](cards/ionosphere/AMB-IONO-0910-MORNING.md) |
| 6.17h What is in flight (pre-registered; results append here, nothing above is edited) | 0910 morning | [cards/ionosphere/AMB-IONO-0910-MORNING.md](cards/ionosphere/AMB-IONO-0910-MORNING.md) |
| §6.18 The 2026-09-10 afternoon — the level-memory and T4-persistence ladders, the wire wander… | 0910 afternoon | [cards/ionosphere/AMB-IONO-0910-AFTERNOON.md](cards/ionosphere/AMB-IONO-0910-AFTERNOON.md) |
| 6.18a The level-memory ladder — softening the levels does not release the split (FAIL ×4) | 0910 afternoon | [cards/ionosphere/AMB-IONO-0910-AFTERNOON.md](cards/ionosphere/AMB-IONO-0910-AFTERNOON.md) |
| 6.18b The T4 persistence ladder — fewer erases buy the producer and cost the consumer | 0910 afternoon | [cards/ionosphere/AMB-IONO-0910-AFTERNOON.md](cards/ionosphere/AMB-IONO-0910-AFTERNOON.md) |
| 6.18c The wire grade of the ladder — the wander rises with K, the jitter does not | 0910 afternoon | [cards/ionosphere/AMB-IONO-0910-AFTERNOON.md](cards/ionosphere/AMB-IONO-0910-AFTERNOON.md) |
| 6.18d The field continuity card — validated as a mechanism, KEEP_OFF twice, and the defect is… | 0910 afternoon | [cards/ionosphere/AMB-IONO-0910-AFTERNOON.md](cards/ionosphere/AMB-IONO-0910-AFTERNOON.md) |
| 6.18e Addendum to §6.17g's field wire census — what drives the steps, and what does not | 0910 afternoon | [cards/ionosphere/AMB-IONO-0910-AFTERNOON.md](cards/ionosphere/AMB-IONO-0910-AFTERNOON.md) |
| §6.19 The 2026-09-10 second half — the slipped-bed probe ledger, the continuity closure, the … | 0910 second half | [cards/ionosphere/AMB-IONO-0910-SECOND-HALF.md](cards/ionosphere/AMB-IONO-0910-SECOND-HALF.md) |
| 6.19a The slipped-bed probe ledger — eleven probes, one shape | 0910 second half | [cards/ionosphere/AMB-IONO-0910-SECOND-HALF.md](cards/ionosphere/AMB-IONO-0910-SECOND-HALF.md) |
| 6.19b The false-alarm isolation — the re-levels the detectors get WRONG are load-bearing | 0910 second half | [cards/ionosphere/AMB-IONO-0910-SECOND-HALF.md](cards/ionosphere/AMB-IONO-0910-SECOND-HALF.md) |
| 6.19c EMISSION-SIDE CONTINUITY — CLOSED, falsified as a CLASS | 0910 second half | [cards/ionosphere/AMB-IONO-0910-SECOND-HALF.md](cards/ionosphere/AMB-IONO-0910-SECOND-HALF.md) |
| 6.19d The receiver clock has NO temporal prior on the network producer — and the sim's truth … | 0910 second half | [cards/ionosphere/AMB-IONO-0910-SECOND-HALF.md](cards/ionosphere/AMB-IONO-0910-SECOND-HALF.md) |
| 6.19e The Phase-2 producer-AR diagnostic, graded against truth | 0910 second half | [cards/ionosphere/AMB-IONO-0910-SECOND-HALF.md](cards/ionosphere/AMB-IONO-0910-SECOND-HALF.md) |
| 6.19f THE EVENT HARNESS — the development loop changes | 0910 second half | [cards/ionosphere/AMB-IONO-0910-SECOND-HALF.md](cards/ionosphere/AMB-IONO-0910-SECOND-HALF.md) |
| §7 Open questions (re-cut 2026-09-04, round 2; round-4 closures and items 15–21, §6.8) | AMB-IONO open | [cards/ionosphere/AMB-IONO-OPEN-QUESTIONS.md](cards/ionosphere/AMB-IONO-OPEN-QUESTIONS.md) |
| §8 Reproduction | AMB-IONO | [cards/ionosphere/AMB-IONO-SPLIT.md](cards/ionosphere/AMB-IONO-SPLIT.md) |
