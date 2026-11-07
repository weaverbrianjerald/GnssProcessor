# SRN: a scientific review of the network processing engine

**STATUS**

- **id:** SRN
- **topic:** network-vehicle
- **status:** open
- **summary:** Pre-registered 2026-11-06: an independent scientific review of the network producer at main `adef08b` on the golden posture (GPS L1/L2 + Galileo E1/E5a, float vehicle, free slant ionosphere, Q3 clock anchor), judged against estimation theory and the PPP-RTK literature along nine dimensions (functional model, stochastic model, parameterization and S-basis, temporal recursion, atmosphere, quality control, cycle slips, clock level and product, methodology). Every finding carries file and line evidence, its theoretical basis, a materiality grade, its prior coverage in the record, and a falsification route. Results land below the pre-registration.
- **source:** main `adef08b` (worktree `D:/GnssProcessorSupport/worktrees/scirev`, branch `review/network-engine`); the baseline pair `runs/_rf0hv17_{field,sim}6` (card BOF, exe `gnss_processor_bofarm.exe`, sha256[:16] `91143c81de9a9547`); the goldens `config/golden/golden_{field,sim}_refound.json`; the ledger `docs/CONFIG_promotion_ledger.json`
- **dates in the text:** 2026-11-06
- **knobs / posture:** the goldens as generated; nothing armed or changed by this card
- **hashes / run dirs:** main `adef08b`; `runs/_rf0hv17_field6`, `runs/_rf0hv17_sim6`

**Outcome.** Question: where does the network producer, as armed on the goldens, depart from estimation theory and from the PPP-RTK literature, which departures are material to the product or to the user, and which experiments would settle the open ones? The review reads the code at one commit and the baseline telemetry, not a new run, and it does not re-litigate the verdicts of the record (the do-not-retry list of the datum line, the falsified ionosphere levers, the retired carries). Its deliverable is a ranked table of findings with evidence, materiality and a falsification route each, and a short list of builds the findings justify. Pending: the results are appended below as they land.

## 1. Pre-registration (2026-11-06)

### 1.1 The object under review

The network producer: `runNetwork` and the eight stages of one epoch (`docs/onboarding/07_network_tour.md`), the shared observation equation (`include/measurement/ObservationEquation.hpp`), the station parameter set and its declared dispositions (`include/network/assembly/ParamInventory.hpp`), the realized S-basis (`docs/cards/network-vehicle/SBASIS-CONTRACT.md`), the carry (`network/carry/`, `network/solve/NormalEquations`), the quality-control tiers (`network/qc/`), the slip layer (`network/slip/`), the clock anchor and the emission (`network/solve/ClockAnchor`, `network/emit/`), and the product grammar (`products/NetworkProductIO`). The posture is the pair of goldens as generated at `adef08b`; the telemetry is the baseline pair of card BOF.

### 1.2 The nine dimensions

| # | dimension | the questions |
| --- | --- | --- |
| D1 | functional model and physical corrections | what the equation models, which standard corrections are absent or approximated at the millimetre level, which conventions are verified by tests |
| D2 | stochastic model and variance components | row weights, the a priori variance factor against the post-fit sigma0, the carried covariance's consistency, the URA |
| D3 | parameterization and the S-basis | every rank deficiency of the two-constellation uncombined model against its realized constraint; minimality; the held basis and its handovers; what the products mean |
| D4 | temporal recursion and the carry | the exact information-form recursion against the code's border prior, conditional station prior, exact time update, births, departures and pins; what the tests prove |
| D5 | atmosphere | the white wet delay and the white slant ionosphere: cost, the correct time link, the absent ionosphere product |
| D6 | quality control | which residual cofactor each test uses, whether anything adapts on the goldens, the identification logic, the standing of the hard bounds |
| D7 | cycle slips and arc management | detection power and false-alarm rate at 30 s, the response cost, the gap rule, the mass-slip of a clock step |
| D8 | clock level, anchor and product | what the clock product is under the a priori anchor, the hold's standing, the covariance information on the wire, the contract against SSR practice |
| D9 | methodology and evidence | the user error and the sim truth errors by hour with formal sigmas, what the two instruments cannot see, what a scientific validation adds |

### 1.3 Method

Wave 1: one independent reviewer per dimension reads the worktree and the baseline telemetry and returns findings in a fixed schema (claim, evidence as file and line, theory, materiality, prior coverage, falsification route, confidence). The findings are read, deduplicated across dimensions and ranked by hand. Wave 2: every kept finding is attacked by two refuters with distinct lenses (the code truth of the claim; its scientific materiality and prior coverage). A finding survives only when the code claim is confirmed and the materiality is not refuted. Open questions whose answer is cheap are settled by a run: a 2 h arm per bed, a golden copy with one change, on a renamed Release executable, graded against a same-executable control on the held-out user and on datum-invariant wire quantities.

### 1.4 What counts, and what is not re-litigated

A finding is a departure from theory or practice with a mechanism and an expected size. Materiality: field-blocking (the posture cannot be trusted with it), material (a measurable effect on the user or on the product), inert (true but without effect on this posture), unknown (an experiment is owed). Not re-litigated: the do-not-retry list of the datum line (ensemble phase datum, `clock_datum_sum_to_zero`, the broadcast-anchored clock datum, lever B and lever C of card SBASIS-THEORY-E), the falsified ionosphere levers (adaptive iono q, SH as a producer constraint, GIM rows), the retired carries (`carry_invariant`, the lane layer), producer-side integer search as a product mechanism (card AB3's wide-lane exception stands). A finding that re-encounters one of these is reported as a re-encounter in one line.

### 1.5 Deliverables

Section 2: the findings table, ranked. Section 3: the experiments run and their results. Section 4: the builds the surviving findings justify, each with its owner card or a new pre-registration. Section 5: the verdict in plain words.
