# ORACLE PIN (stub): pre-registration for holding the ambiguities at truth

**STATUS**

- **id:** ORACLE PIN (stub)
- **topic:** harness
- **status:** superseded
- **summary:** SUPERSEDED by the card record in SPEC_oracle_pin: under a complete truth pin the DD-invariant ionosphere collapses 10×, confirming the 99.3 %-iono root cause.
- **dates in the text:** 2026-09-11
- **knobs / posture:** `network.inject.events` (new kind `oracle_pin`, sim-only, never a golden knob), `network.cycle_slip.oracle_relevel`
- **hashes named in the text** (commit / exe / byte-cert): none
- **run dirs named in the text:** none
- **source:** `docs/archive/SPEC_event_harness.md` §17

**Outcome.** The stub pre-registered the question (hold the sim's ambiguities at their true integers and grade where the remaining error goes), the gauge-free design and gates G1 to G4. The card was built and graded the same day in [SPEC_oracle_pin](../../SPEC_oracle_pin.md): G1 and G4 PASS, G2 at 100 % of pinned chords on every band but Galileo L7Q (57.7 %), and the DD-invariant slant ionosphere falls 0.423 → 0.042 m. Its section 12 later WITHDREW the ZWD reading quoted below (0.067 → 0.013 m): the null's ZWD was the harness seam restart, and on a reseeded leg the pin buys the ZWD nothing ([harness seeds](../harness/HARNESS-SEEDS.md)).

> Results are appended TO THIS FILE, below the verbatim text. The archived monolith is frozen; never append there.

---
<!-- BEGIN VERBATIM: SPEC_event_harness.md -->
## 17. Next card, pre-registration stub — the ORACLE PIN: hold the ambiguities at truth and read where the error goes (stubbed 2026-09-11; opens after Study 7's storm arm)

**→ DONE 2026-09-11 (evening): the card is built, graded and merged — results in `docs/SPEC_oracle_pin.md`** (`network.inject.events[] kind "oracle_pin"`, `filter/NetworkOraclePin`, sim-only, never a golden knob).  Headline, quiet seed ep0665, 60 ep, pin +5: G1 byte-identical, G2 100 % of pinned chords integer to < 0.01 cyc with unpinned chords unmoved, G4 the pin follows 109 truth slips.  The DATUM-INVARIANT (two-way de-meaned) per-LOS ionosphere collapses **0.423 → 0.042 m (2.61 → 0.26 TECU)** under the complete pin — the 99.3 %-iono root cause CONFIRMED and the integer-information deficit measured; ZWD rms 0.067 → 0.013 m; a PARTIAL band pin (L1C+L2W) makes the DD-invariant iono 1.3× WORSE than no pin.  ⚠ Instrument lesson: the raw per-LOS iono error vs truth is basis-dependent (station/satellite DCB absorption, ~97 % of its 2.7 m) and cannot test the root cause.


**The question (user's, 2026-09-11 pm).**  The sim knows every true integer (`<station>/truth_ambiguities.csv` — one integer per satellite-frequency arc — carried through `truth_slips.csv`), every satellite and receiver bias, the slant ionosphere, ZWD, clocks and positions.  Hold some or all ambiguities at truth inside the estimator and grade the remaining parameters against truth: with the ambiguity/ionosphere degeneracy removed, the residual error budget is the model's own, and partial pins show which parameter absorbs the error a pinned band or station no longer carries.

**What exists.**  `network.cycle_slip.oracle_relevel` (`include/slip/NetworkSlipOracle.hpp`) is a truth-fed slip REPAIR: it shifts `LocalCarry::value_abs` by the true jump at every detector actuation site; it carries the truth-root loader, the field-inert guard (loader hard-errors without `oracle_truth_root`; never in a golden) and access to the carried values.  The pin is that mechanism turned from a delta into a level.  Truth graders: `simnet_grade.py` (`grade_clock`, `grade_zwd`, `grade_rx_states`, `grade_iono_table8`, `grade_phase`, `grade_code_bias`, `grade_consumer`), `simnet_amb_integer.py --ref-free`, `simnet_dd_reref.py`, `simnet_iono_interaction.py`.

**The one subtlety — the gauge.**  A raw pin `value_abs = λ·N_true` fights the datum: the network fixes one arc per (satellite, band) family at whatever level it carried when it became the anchor, and station-level phase biases ride inside the ambiguities.  The truth must enter as gauge-free integer DIFFERENCES (double differences), or as absolute values computed in the run's own frame from the anchors: for a non-anchor arc (r, s, j), `value_abs = value_abs(anchor(s, j)) + λ_j·[N_true(r,s,j) − N_true(a,s,j)] + (station term for (r, j))`, the station term taken from the run's own station reference arc so that every pinned DD equals its true integer.  ⚠ The x_lin trap (SPEC_lane_slip_response §7.2): a pin that only shifts `x_lin` is an exact solve no-op — set `value_abs` AND stiffen the prior (zero-q, Λ large), then verify actuation on the DD instrument (`network_dd_fractional.csv`, ref-free).

**Design (to pre-register in full when the card opens).**  Injector kind `oracle_pin` in `network.inject.events[]` with `scope` ∈ {all, band:<sig>, station:<id>, sat:<id>, arc:<sta>/<sat>/<sig>} and `at` (epoch); truth root as `oracle_relevel`; the pinned set follows the truth integer through subsequent truth slips (`N_true(t) = N_int + Σ deltas`), so the pin holds on the slipped bed too; counters `oracle_pins_set / held / released`, an event row per pinned arc, run-end clause; field-inert by construction (the same hard errors as `oracle_relevel`); NOT a golden knob, ever.

**Pre-registered legs (quiet seed ep0665 first; slipped seed second):** 60 epochs, pin at +5, arms: none / all / L1C+L2W only / L5Q only / one station (S21000SIM) / one satellite (G08).  Grade at +30 and +55 against truth: slant iono per LOS (rms, and the per-LOS distribution), ZWD per station, station clocks, satellite clocks (⚠ this bed's satellite clocks are NOT null — use the truth-differenced grade, not `simnet_sat_clock_census.py`), code biases, phase biases (ref-free lane grade), consumer position; plus the DD fractional (ref-free) as the actuation check (pinned chords must read exactly integer).

**What the columns decide.**  `all` = the ceiling of what integer information buys on this bed — prices user-side AR and atmosphere constraints in one leg, and tests the standing root-cause (`project_why_non_integer_rootcause`: 99.3 % ionosphere): iono snapping to the phase floor CONFIRMS it and points the fix at integer information; iono staying wrong FALSIFIES it and names another sink.  The partial columns are the coupling map: where a pinned band's or station's former error lands.

**Gates:** G1 OFF byte-identical (the knob absent → no code path touched); G2 actuation (pinned DDs read integer to < 0.01 cyc at +1); G3 the six-column table with every cell from a truth grader, never a raw wire step (Psychas); G4 the slipped-seed repeat holds the pin through ≥ 1 truth slip on a pinned arc.

