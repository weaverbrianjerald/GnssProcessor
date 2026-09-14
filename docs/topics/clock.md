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
| [Q3 ladder](../cards/clock/Q3-FLOOR-GOVERNED-LADDER.md) | the floor-governed ladder — floors derived not tuned, the guard's denominator bisected,… | closed | 2026-08-10 |
| [Q3 results](../cards/clock/Q3-ANCHOR-RESULTS.md) | G0–G3, the binding knee, the FC-1 shadow-hour table, and the relationship to the surrou… | closed | 2026-08-10 |
| [Q3 floor](../cards/clock/Q3-PHYSICAL-FLOOR.md) | the physical floor measured — trap 5 resolved, the FC-1 gate retired, the ladder re-sco… | closed | 2026-08-11 |
| [field wander probe](../cards/clock/FIELD-WANDER-PROBE.md) | the field wire wander is NOT receiver-code-driven; the a-priori CLK common-drift probe … | closed | 2026-08-11 |
| [Q3 design](../cards/clock/Q3-ANCHOR-DESIGN.md) | the guarded temporal clock anchor — the measured basis, stage A/B design, the empirical… | closed | 2026-08-15 |
| [Q3 G4](../cards/clock/Q3-G4-SIM-HALF.md) | G4's sim half executed — the anchor truth-graded on the 12 h quiet flagship, and the si… | closed | 2026-09-01 |
| [CS](../cards/clock/CS-clock-step-signalling.md) | clock step signalling — 0 unsignalled steps, the 55.8 m consumer excursion gone | closed | 2026-09-14 |
| [Q3-F](../cards/clock/Q3-F-self-contained-time-link.md) | the self-contained clock time-link on the leveling vehicle | open | 2026-09-14 |

**Standing documents** (still at their original paths, indexed not moved):

- [CLOCK_HADAMARD_CALIB_2026-07-14.md](../CLOCK_HADAMARD_CALIB_2026-07-14.md)
- [SPEC_c1_clock_rate_validation.md](../SPEC_c1_clock_rate_validation.md)
- [SPEC_clock_emit_rebase.md](../SPEC_clock_emit_rebase.md)
- [SPEC_fast_clock_split_cadence.md](../SPEC_fast_clock_split_cadence.md)

---

Back to [INDEX.md](../INDEX.md).
