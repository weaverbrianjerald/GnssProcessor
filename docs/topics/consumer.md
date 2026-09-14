# `consumer` — The closed-loop consumer — the instrument that decides

The consumer is a held-out station run against the producer's own wire. Since card CI it
is treated as an INSTRUMENT rather than a bar of last resort — and the instrument said
the field consumer wanders by metres, resets on every declared step it observes, and
starts cold at every seam.

**Conventions and invariants**

- Grade the consumer COLD unless the card is about the warm start; a warm consumer hides
  the producer's own transient.
- A consumer bar is only readable once the wire it consumes is understood — several
  consumer-side bars were suspended in September 2026 when the wire-vs-CODE census showed
  the wire itself was metres off CODE within arcs.
- The consumer's AR is fail-unsafe on the user side; never trust a fix count as evidence.

**Current posture (2026-09-14)** — the cold 6 h consumer is one of the only two bars in
[the network re-founding](../refound/NETWORK_REFOUNDING_2026-09-14.md); target sub-decimetre settled.

## Cards, oldest first

Status is one of `open`, `closed`, `parked`, `superseded`, `reference`; the verdict a card reached is in its summary and its text. Results are appended to the card file, never here.

| card | one line | status | last dated |
| --- | --- | --- | --- |
| [CC](../cards/consumer/CC-declared-step-carry.md) | the continuity half — the consumer's declared-step carry and warm start, landed default… | closed | 2026-09-14 |
| [CI](../cards/consumer/CI-consumer-instrument.md) | the closed-loop consumer as an INSTRUMENT — what the consumer bars have actually been r… | open | 2026-09-14 |
| [CI census](../cards/consumer/CI-census-wire-vs-code.md) | the wire-vs-CODE consistency census and the CODE-RESIDUAL per-LOS attribution | closed | 2026-09-14 |
| [consumer AR](../cards/consumer/CONSUMER-AR-BLUNDER.md) | the consumer AR blunder diagnosed and fixed — the innovation screen is the gate that ca… | closed | — |

---

Back to [INDEX.md](../INDEX.md).
