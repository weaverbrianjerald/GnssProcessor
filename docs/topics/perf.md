# `perf` — Performance — timing, parallelism, and the anti-regrowth decomposition

Runtime and scaling of the network producer, the deterministic-parallel seams, and the
decomposition that keeps `ApplicationNetwork.cpp` and `RealTimeEngine.cpp` thin.

**Conventions and invariants**

- **Never verdict inside ignition.** A 30-epoch arm measures the cold start; the lane
  layer's verdict flipped when the same arms were re-measured settled.
- `parallel_carry` and `parallel_reduce` are MUTEX; the loader forces `parallel_carry` off
  on a warn-only line.
- Byte-identity is the parallel seam's acceptance test, and a fused accumulate is not
  byte-identical to a sequential one.
- New producer code goes in the seam matching its concern, never inline in the driver —
  the decision trees are in `CLAUDE.md`.
- Debug perf numbers are artifacts (standing invariant #2).

**Current posture (2026-09-14)** — the lane layer is OFF at both scales, on speed and on
science; the scope cut in [the network re-founding](../refound/NETWORK_REFOUNDING_2026-09-14.md) removes the lanes entirely for now.

## Cards, oldest first

Status is one of `open`, `closed`, `parked`, `superseded`, `reference`; the verdict a card reached is in its summary and its text. Results are appended to the card file, never here.

_No split cards in this topic._

**Standing documents** (still at their original paths, indexed not moved):

- [SPEC_carry_factored_pz_C2.md](../SPEC_carry_factored_pz_C2.md)
- [SPEC_dev_tier_perf_cards.md](../SPEC_dev_tier_perf_cards.md)
- [SPEC_inc8b_election_split.md](../SPEC_inc8b_election_split.md)
- [network_decomposition_plan.md](../network_decomposition_plan.md)

---

Back to [INDEX.md](../INDEX.md).
