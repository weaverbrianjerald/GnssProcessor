# `wire-contract` — The product wire — what it promises the consumer

What the emitted product means and what it must declare: the M4 contract, discontinuity
declaration on every channel (clock, phase bias, code bias), the cbpin / product legend
schema, the ionosphere level contract, and the level-health channel for withhold eras.

**Conventions and invariants**

- **A consumer SUPERSEDES, never adds.** The M4 contract v2 is the binding statement.
- Never gate a DECLARATION on a QC or trace knob — declarations drift out of the product
  the moment they depend on a diagnostic being armed.
- Every step the producer takes must be declared. A satellite clock step that reaches the
  wire unsignalled is the defect, not the consumer's reaction to it.
- A level needs a declared owner; the seed is a FRAME, not an authority.

**Current posture (2026-09-14)** — RF-3 is "a wire contract that declares every step".
See [the network re-founding](../refound/NETWORK_REFOUNDING_2026-09-14.md).

## Cards, oldest first

Status is one of `open`, `closed`, `parked`, `superseded`, `reference`; the verdict a card reached is in its summary and its text. Results are appended to the card file, never here.

| card | one line | status | last dated |
| --- | --- | --- | --- |
| [LV-3 contract](../cards/wire-contract/LEVEL-CONTRACT.md) | level contract: W6 sum-to-zero or a00 GIM anchor owns the level; LV-1/2/4/5 SUPERSEDED | closed | 2026-08-15 |
| [level health](../cards/wire-contract/LEVEL-HEALTH-CHANNEL.md) | level health: Phase A PASS on era-3 (7/7), causal limb misses slides; Phase B build owed | open | 2026-08-23 |

**Standing documents** (still at their original paths, indexed not moved):

- [SPEC_product_cbpin_contract.md](../SPEC_product_cbpin_contract.md)

---

Back to [INDEX.md](../INDEX.md).
