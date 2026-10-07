# `network-vehicle` — The producer's estimation vehicle — carry, gauge, datum, S-basis

The "vehicle" is the choice of how the network producer carries information between
epochs and how it fixes the rank deficiencies: the `carry_invariant` DD branch versus
phase leveling, the S-basis realization contract, the phase-datum / pivot family, eq34,
the coupling-row carry, and the station block itself.

This is the topic that the 2026-09-14 re-founding is about: the field golden's carry
(`carry_invariant`, July) had diverged from the sim golden's (leveling) and was never
re-validated as a whole, and the field golden's station block turned out to be SINGULAR
(null dim 7 at 58 of 61 stations).

**Conventions and invariants**

- `StateManager` is the single source of truth for state layout; do not add sibling
  classes that own parallel state.
- Standing invariant #1: any zero-q / stiff time-linked quantity needs a wired, tested
  slip / jump / event response — and the response's own RE-COUPLING edge must bound or
  re-seed what it reconnects.
- Standing invariant #7: anything that changes which information flows into which states
  is an EVENT at BOTH edges, with counters and identities.
- Standing invariant #5: grade datum-invariant / SD quantities only. An `x_lin` shift is
  an EXACT solve no-op — any screen or fix that reads `x_lin` is measuring an arbitrary
  frame.
- New work is default-OFF with structural byte-identity (invariant #6); shared-path edits
  need a same-build golden comparison.

**Current posture (2026-09-14)** — frozen at the leveling vehicle + gauge projection +
phase-datum family; the `carry_invariant` family is off. See [the network re-founding](../refound/NETWORK_REFOUNDING_2026-09-14.md), RF-1
(ambiguities as float states with covariance) and RF-2 (a fixed S-basis with declared
S-transformations).

## Cards, oldest first

Status is one of `open`, `closed`, `parked`, `superseded`, `reference`; the verdict a card reached is in its summary and its text. Results are appended to the card file, never here.

| card | one line | status | last dated |
| --- | --- | --- | --- |
| [Card 1b](../cards/network-vehicle/CARD-1B-JOINT-WHITENING.md) | coupling-row joint whitening: premise FALSIFIED, the toggle byte-identical, knob retired | closed | 2026-07-10 |
| [datum wander](../cards/network-vehicle/DATUM-WANDER.md) | the common clock level follows the datum station; sum-to-zero PASSES, a 25° mask REJECTED | closed | 2026-08-11 |
| [chain-pin audit](../cards/network-vehicle/CHAIN-PIN-AUDIT.md) | eq34 chain-pin audit: COMPLETE, rungs 2/6 were SH-gate withholds (sim-only artifact) | closed | 2026-08-12 |
| [R0 + 1(d)](../cards/network-vehicle/PIVOT-CARRY-REREF-R0-1D.md) | phase-anchor carry reref VALIDATED: churn 160.5 to 1.5 m (n = 1); armed in the goldens | closed | 2026-08-14 |
| [Card 1d](../cards/network-vehicle/CARD-1D-COUPLING-CARRY.md) | scalar κ border time update: BUILT, armed; card CTU's exact update now builds the rows | closed | 2026-08-15 |
| [S-basis conformance](../cards/network-vehicle/SBASIS-CONFORMANCE-F.md) | user-side Odijk audit: invariant via differencing; network charter BUILT, P0/P1 still open | reference | 2026-08-15 |
| [S-basis contract](../cards/network-vehicle/SBASIS-CONTRACT.md) | realized S-basis, wire meaning, hold-or-mark contract (2026-08); GF datum residues open | reference | 2026-08-15 |
| [S-basis theory](../cards/network-vehicle/SBASIS-THEORY-E.md) | S-system canon: re-datuming moves no estimable wander; the Eq-34 gate stays binding | reference | 2026-08-15 |
| [chain-slant](../cards/network-vehicle/CHAIN-SLANT-ODIJK.md) | Odijk rank arithmetic and the chain caveat: n-invariant near-null, posture C the goal | closed | 2026-08-15 |
| [pivot gauge defect](../cards/network-vehicle/PIVOT-GAUGE-DEFECT.md) | single-pivot phase-bias gauge defect PROVEN; clean election fixes it, third root named | closed | 2026-08-15 |
| [queue 1(b)](../cards/network-vehicle/HANDOVER-STRANSFORM-SPEC.md) | clock-pivot handover S-transform spec; built and PROMOTED 2026-09-12 under PIVOT DEPARTURE | closed | 2026-08-15 |
| [eq34 D4](../cards/network-vehicle/EQ34-D4-S-TRANSFORM-FIX.md) | eq34 D4 carry shift now re-bases coupling s_prev: FIXED 2026-08-17, no live lineage hit | closed | 2026-08-17 |
| [Cards 1f + 1g](../cards/network-vehicle/CARD-1F-1G-TRUNCATED-ROW-GAUGE.md) | truncated-row gauge + reinit widening: PASS to 305 sta, withholds to 0, wire −29 % | closed | 2026-08-19 |
| [queue item 2](../cards/network-vehicle/PIVOT-ELECTION-QUEUE2.md) | datum election landed as tooling; handover S-transform deferred, target near-empty | closed | 2026-08-21 |
| [wind-up sign](../cards/network-vehicle/WINDUP-SIGN-DEFECT.md) | wind-up sign defect FIXED at four sites; a half-fix is worse, all sim validations blind | closed | 2026-08-21 |
| [wind-up field](../cards/network-vehicle/WINDUP-FIELD-LADDER.md) | wind-up field ladder: slope near 2 in 9/9 components, null rejected; network arm consistent | closed | 2026-08-22 |
| [R2 sticky](../cards/network-vehicle/PIVOT-R2-STICKY.md) | sticky continuous target VALIDATED: churn dead, consumer 2x; superseded by RF-2 held basis | superseded | 2026-08-31 |
| [READMIT](../cards/network-vehicle/READMIT-EDGE.md) | re-admission edge: seed horizon PROMOTED (T3 tails cut); window, ratio, age gates KEEP_OFF | closed | 2026-09-06 |
| [F3 field](../cards/network-vehicle/F3-FIELD-ARM.md) | field F3 ladder pre-registration: control DD ambiguities wander ~10 cyc/arc; P2 FALSIFIED | closed | 2026-09-09 |
| [F3 field (results)](../cards/network-vehicle/F3-FIELD-ARM-RESULTS.md) | field F3 KEEP OFF (P2 FALSIFIED, WL12 PARTIAL); the converging leveling posture found | closed | 2026-09-09 |
| [COLD-FB (iter 2–4)](../cards/network-vehicle/COLD-FB-iterations-2-4.md) | satellite clause removes every split; KEEP_OFF, de-biased consumer misses golden by 4 mm | superseded | 2026-09-11 |
| [COLD-FB](../cards/network-vehicle/COLD-FB-anchor-fallbacks.md) | candidacy screen KEEP_OFF: settled cold handovers 51 → 19, consumer p50 +49 % vs cbscope5 | superseded | 2026-09-12 |
| [PIVOT DEPARTURE](../cards/network-vehicle/PIVOT-DEPARTURE.md) | pivot-handover shock is carry free-fall: scoped response and clock S-transform PROMOTED | closed | 2026-09-12 |
| [D5 v5](../cards/network-vehicle/D5-V5-carried-only.md) | carried-only D5 gate KEEP_OFF: born-chord flags gone, same-realization consumer +39 % | superseded | 2026-09-13 |
| [CE](../cards/network-vehicle/CE-chain-era-bisect.md) | chain-era bisect: c0 = PV-A byte-identical, only the a priori clock pin (c3) moves code | closed | 2026-09-14 |
| [CM](../cards/network-vehicle/CM-code-model-consistency.md) | eleven cold arms clear every estimation knob; the station block is near-rank-deficient | closed | 2026-09-14 |
| [CM2](../cards/network-vehicle/CM2-leveling-vehicle.md) | the second defect is not a knob: no arm crosses or halves a bar on the leveling vehicle | closed | 2026-09-14 |
| [PV](../cards/network-vehicle/PV-vehicle-test.md) | vehicle test: leveling beats carry_invariant on every read, all three bars MISSED | closed | 2026-09-14 |
| [ZP](../cards/network-vehicle/ZP-station-block-parameterization.md) | station block singular under carry_invariant (58/61); ZWD prior RETIRED; leveling vehicle | closed | — |

**Standing documents** (still at their original paths, indexed not moved):

- [SPEC_card4_invariant_carry.md](../SPEC_card4_invariant_carry.md)
- [SPEC_carry_aware_covariance.md](../SPEC_carry_aware_covariance.md)
- [SPEC_carry_vce_apply.md](../SPEC_carry_vce_apply.md)
- [SPEC_d5_churn_limit_cycle.md](../SPEC_d5_churn_limit_cycle.md)
- [SPEC_datum_graph_census.md](../SPEC_datum_graph_census.md)
- [SPEC_eq34_reattach_hygiene.md](../SPEC_eq34_reattach_hygiene.md)
- [SPEC_eq34_seed_round.md](../SPEC_eq34_seed_round.md)
- [SPEC_family_gf_datum.md](../SPEC_family_gf_datum.md)
- [SPEC_o2_carry_invariant_accumulator.md](../SPEC_o2_carry_invariant_accumulator.md)
- [SPEC_pd_safe_dd_fold.md](../SPEC_pd_safe_dd_fold.md)
- [network_ambiguity_resolution_design.md](../network_ambiguity_resolution_design.md)

---

Back to [INDEX.md](../INDEX.md).
