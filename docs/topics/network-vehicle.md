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
| [Card 1b](../cards/network-vehicle/CARD-1B-JOINT-WHITENING.md) | coupling-row JOINT whitening — PREMISE FALSIFIED, the toggle is a no-op, and what the f… | closed | 2026-07-10 |
| [datum wander](../cards/network-vehicle/DATUM-WANDER.md) | the common level FOLLOWS the datum station — both remedies measured | closed | 2026-08-11 |
| [chain-pin audit](../cards/network-vehicle/CHAIN-PIN-AUDIT.md) | rungs 2/6 re-attributed and the field's emission explained — the chain is COMPLETE | closed | 2026-08-12 |
| [R0 + 1(d)](../cards/network-vehicle/PIVOT-CARRY-REREF-R0-1D.md) | `network.phase_anchor_carry_reref` — the carry re-reference closes the churn, with the … | closed | 2026-08-14 |
| [Card 1d](../cards/network-vehicle/CARD-1D-COUPLING-CARRY.md) | the border time-update defect, verified four ways — magnitude in the T7 lineage, the ba… | closed | 2026-08-15 |
| [S-basis conformance](../cards/network-vehicle/SBASIS-CONFORMANCE-F.md) | the per-deficiency conformance table, the implicit S-basis the code adopts per mode, th… | reference | 2026-08-15 |
| [S-basis contract](../cards/network-vehicle/SBASIS-CONTRACT.md) | the realized basis, what the wire records mean, the realization-transfer contract, and … | reference | 2026-08-15 |
| [S-basis theory](../cards/network-vehicle/SBASIS-THEORY-E.md) | the estimability spine, the datum-invariance theorem, the rank deficiencies, CC-R vs CC… | reference | 2026-08-15 |
| [chain-slant](../cards/network-vehicle/CHAIN-SLANT-ODIJK.md) | Odijk rank arithmetic, the global-network chain caveat, the three self-consistent postu… | closed | 2026-08-15 |
| [pivot gauge defect](../cards/network-vehicle/PIVOT-GAUGE-DEFECT.md) | the single-pivot phase-bias gauge defect — the causal proof, the field evidence, the tw… | closed | 2026-08-15 |
| [queue 1(b)](../cards/network-vehicle/HANDOVER-STRANSFORM-SPEC.md) | the handover S-transform spec — the transform, its rank-1 specialization to a pin swap,… | closed | 2026-08-15 |
| [eq34 D4](../cards/network-vehicle/EQ34-D4-S-TRANSFORM-FIX.md) | the eq34 D4 S-transform now re-bases `coupling[].s_prev` | closed | 2026-08-17 |
| [Cards 1f + 1g](../cards/network-vehicle/CARD-1F-1G-TRUNCATED-ROW-GAUGE.md) | the TRUNCATED-ROW gauge defect and the reinit-source widening — the positive control, t… | closed | 2026-08-19 |
| [queue item 2](../cards/network-vehicle/PIVOT-ELECTION-QUEUE2.md) | the datum-station election formalized as plumbing, and item 1(b)'s precondition priced | closed | 2026-08-21 |
| [wind-up sign](../cards/network-vehicle/WINDUP-SIGN-DEFECT.md) | the carrier-phase wind-up sign defect — why nothing caught it, why the fix is both line… | closed | 2026-08-21 |
| [wind-up field](../cards/network-vehicle/WINDUP-FIELD-LADDER.md) | the three-arm field ladder — the slope lands on 2, never on 0; the 5-station matrix; th… | closed | 2026-08-22 |
| [R2 sticky](../cards/network-vehicle/PIVOT-R2-STICKY.md) | the sticky continuous target, composed with the pairing — churn dead, consumer 2×, and … | closed | 2026-08-31 |
| [READMIT](../cards/network-vehicle/READMIT-EDGE.md) | the re-admission edge — `readmit_seed_horizon_s` promoted, the coupling-marginalize and… | closed | 2026-09-06 |
| [F3 field](../cards/network-vehicle/F3-FIELD-ARM.md) | the field F3 arm — bed, arms, truth-free instruments, and the pre-registered prediction… | closed | 2026-09-09 |
| [F3 field (results)](../cards/network-vehicle/F3-FIELD-ARM-RESULTS.md) | the field F3 arm's results, filled as they landed | closed | 2026-09-09 |
| [COLD-FB (iter 2–4)](../cards/network-vehicle/COLD-FB-iterations-2-4.md) | the SATELLITE clause, the datum-invariant consumer census, and the new-golden arm — KEE… | parked | 2026-09-11 |
| [COLD-FB](../cards/network-vehicle/COLD-FB-anchor-fallbacks.md) | the cold phase-datum anchor fallbacks (`phase_datum_candidacy_usable`) — pre-registrati… | parked | 2026-09-12 |
| [PIVOT DEPARTURE](../cards/network-vehicle/PIVOT-DEPARTURE.md) | the consumer shock at a pivot handover is producer-internal carry free-fall — `scoped_h… | closed | 2026-09-12 |
| [D5 v5](../cards/network-vehicle/D5-V5-carried-only.md) | `carry_invariant_innov_carried_only` on the field — KEEP_OFF, the same-realization pair… | parked | 2026-09-13 |
| [CE](../cards/network-vehicle/CE-chain-era-bisect.md) | the CHAIN-ERA bisect on the leveling vehicle | open | 2026-09-14 |
| [CM](../cards/network-vehicle/CM-code-model-consistency.md) | CODE-MODEL CONSISTENCY — eleven cold arms clear every estimation knob, and the station … | closed | 2026-09-14 |
| [CM2](../cards/network-vehicle/CM2-leveling-vehicle.md) | the second defect — the CM knob arms on the LEVELING vehicle | open | 2026-09-14 |
| [PV](../cards/network-vehicle/PV-vehicle-test.md) | the VEHICLE test — carry_invariant vs leveling, pre-registered | open | 2026-09-14 |
| [ZP](../cards/network-vehicle/ZP-station-block-parameterization.md) | the station block's parameterization at ignition — pre-registered | open | — |

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
