# `clock` — The satellite and receiver clock — datum, time-link, emission

The clock is the worst re-realizer the producer has measured, and the free satellite
clock random-walks at ~0.17 m per step. This topic holds the clock datum (sum-to-zero,
broadcast-anchored, pivot election), the Q3 temporal anchor that gives the clock's
estimable content stiffness, the CLK-R emission rebase, the fast-clock split cadence,
the C1 rate card, and clock-step signalling.

**Conventions and invariants**

- The clock datum is a **product-quality decision, not a label**: the datum-free contrast
  moves 2.2× with the pivot's geometry.
- A clock step that reaches the wire without being declared is a contract violation —
  the step test must be armed, or the disc counter is never populated and the steps are
  silent to the consumer.
- ZOH is MMSE for the clock; the rate lives in the a-priori product (C1 is net-harmful).
- Grade the clock on datum-invariant contrasts, not on the raw state.

**Current posture (2026-09-14)** — the physical clock time-link (`clock_anchor_q: 2.9e-8`)
and `clock_disc_jump_test: true` are both in the frozen posture of [the network re-founding](../refound/NETWORK_REFOUNDING_2026-09-14.md).

## Cards, oldest first

Status is one of `open`, `closed`, `parked`, `superseded`, `reference`; the verdict a card reached is in its summary and its text. Results are appended to the card file, never here.

| card | one line | status | last dated |
| --- | --- | --- | --- |
| [Q3 ladder](../cards/clock/Q3-FLOOR-GOVERNED-LADDER.md) | q ladder to 1e-7: producer gates monotone, trip collapse is the denominator, no promotion | closed | 2026-08-10 |
| [Q3 results](../cards/clock/Q3-ANCHOR-RESULTS.md) | G0 to G2 PASS (σ_w −40 %), knee at q 1.0 to 1.5e-5; FC-1 correlation gate NOT met | closed | 2026-08-10 |
| [Q3 floor](../cards/clock/Q3-PHYSICAL-FLOOR.md) | physical clock floor: q_phys 2.9e-8 m2/s under a 2.7 mm floor, FC-1 gate retired | closed | 2026-08-11 |
| [field wander probe](../cards/clock/FIELD-WANDER-PROBE.md) | field wire wander is estimator-internal: not receiver code, not the a-priori clock frame | closed | 2026-08-11 |
| [Q3 design](../cards/clock/Q3-ANCHOR-DESIGN.md) | guarded temporal clock anchor design: binding carry, residual guard, gates G0 to G4 | closed | 2026-08-15 |
| [Q3 G4](../cards/clock/Q3-G4-SIM-HALF.md) | sim G4 half: anchor NET-POSITIVE (consumer -23 %), cb error real; q 3e-6 CLOSED on sim | closed | 2026-09-01 |
| [CS](../cards/clock/CS-clock-step-signalling.md) | clock step test VALIDATED: 0 unsignalled steps > 2 m, consumer 55.8 → 2.7 m at the event | closed | 2026-09-14 |
| [Q3-F](../cards/clock/Q3-F-self-contained-time-link.md) | anchor at the physical floor (q 2.9e-8) PASSES the code bars; in the re-founding posture | closed | 2026-09-14 |

**Standing documents** (still at their original paths, indexed not moved):

- [CLOCK_HADAMARD_CALIB_2026-07-14.md](../CLOCK_HADAMARD_CALIB_2026-07-14.md)
- [SPEC_c1_clock_rate_validation.md](../SPEC_c1_clock_rate_validation.md)
- [SPEC_clock_emit_rebase.md](../SPEC_clock_emit_rebase.md)
- [SPEC_fast_clock_split_cadence.md](../SPEC_fast_clock_split_cadence.md)

---

Back to [INDEX.md](../INDEX.md).
