# `queues` — Work registers — the open cross-family queues

Rank-ordered registers of work that spans several topics: the principled-replacement
queue (replace symptom-level patches with the correct mechanism) and the edge-response
queue (invariant #7's open valves).

**Conventions and invariants**

- A queue is a rank order, not a plan. An item leaves it by being built and graded, by
  being falsified, or by being re-filed as a card — never by being quietly dropped.
- "Blocked on root cause" is a real state: do not build the "correct" fix before the
  defect is located.
- The "leave alone" list is binding — those were decided or falsified; do not
  re-litigate them.

**Current posture (2026-09-14)** — [the network re-founding](../refound/NETWORK_REFOUNDING_2026-09-14.md) supersedes the queue's ordering for
the network producer. The queue's items remain valid statements of what is wrong; RF-1 …
RF-3 are the order they are now attacked in.

## Cards, oldest first

Status is one of `open`, `closed`, `parked`, `superseded`, `reference`; the verdict a card reached is in its summary and its text. Results are appended to the card file, never here.

| card | one line | status | last dated |
| --- | --- | --- | --- |
| [QPR blocked](../cards/queues/QPR-BLOCKED-AND-CLOSED.md) | blocked/leave-alone: churn fixed by 1(d), D5 retired by RF-1, trio runaway to L5B; verdicts stand | superseded | 2026-08-14 |
| [QPR](../cards/queues/QPR-00-CHARTER.md) | queue charter: SUPERSEDED by the 2026-09-14 re-founding; two leftovers re-filed to OPEN_ITEMS | superseded | 2026-08-21 |
| [QPR 0–3](../cards/queues/QPR-ITEMS-0-3.md) | items 0–3: 1(a), 1(b), 1(d), 1f, 1g, clock marking armed in goldens; item 2 tooling; rest moot | closed | 2026-08-21 |
| [QPR 4](../cards/queues/QPR-ITEM-4-T3-NULLS.md) | T3 null: excess is per-LOS iono error; R2/R4 built measure-only; rest superseded by RF-3 code-row QC | superseded | 2026-08-21 |
| [QPR 5–8](../cards/queues/QPR-ITEMS-5-8.md) | items 5–8: 8(b) falsified; 5, 7, 8 superseded by RF-0c, RF-1, no SH; item 6 re-filed as owed | superseded | 2026-08-21 |
| [CAF](../cards/queues/CAF-COMMENT-AUDIT-FINDINGS.md) | comment audit: 5 defects fixed; Card-4, lane and stand-down rows moot; 20 owed lines re-filed to OPEN_ITEMS | closed | 2026-09-28 |

**Standing documents** (still at their original paths, indexed not moved):

- [HANDOFF_edge_response_queue.md](../HANDOFF_edge_response_queue.md)

---

Back to [INDEX.md](../INDEX.md).
