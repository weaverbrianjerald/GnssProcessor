# `resume-seam` — Warm resume, state snapshots and the seam

Everything about restarting a converged producer from a snapshot: what the snapshot
carries, what it silently does not, and what the wire does at the seam epoch. The RS-2
… RS-10b chain closed the field warm-resume seam in September 2026.

**Conventions and invariants**

- A seam is invariant #7's two edges. Grade it at the **seam epoch and at the consumer**,
  never on pooled leg-minus-seed bars — those measure realization divergence, and they
  were retired as seam instruments at RS-9.
- Grade every seam card with a **cold-consumer replay** (`splice_wire.py`); the
  seam-epoch diff alone cannot see an integrated, unsignalled drift.
- A value that must survive to the next epoch is a member, not an epoch context — putting
  it in the context silently cold-starts it every epoch.
- Restore knobs are fingerprint-sensitive: the snapshot and the resuming run must agree.

**Current posture (2026-09-14)** — seams, snapshots and warm resumes are **off** under
the re-founding's scope cut; they are an add-back, one at a time, after the cold consumer
is sub-decimetre. See [the network re-founding](../refound/NETWORK_REFOUNDING_2026-09-14.md).

## Cards, oldest first

Status is one of `open`, `closed`, `parked`, `superseded`, `reference`; the verdict a card reached is in its summary and its text. Results are appended to the card file, never here.

| card | one line | status | last dated |
| --- | --- | --- | --- |
| [§18.10/§18.11](../cards/resume-seam/RESUME-FRAME-RESTORE.md) | the seam is a FRAME loss — `resume_frame_restore`, built and graded | closed | 2026-09-03 |
| [§18.12](../cards/resume-seam/RESUME-COUPLING-RESTORE.md) | the coupling-regressor restore verdict — the mechanism works, the wire is 10× better | closed | 2026-09-05 |
| [§18.13/§18.14](../cards/resume-seam/RESUME-DENSE-BORDER-E2.md) | E2 and the dense border across the seam — pre-registered, then its verdicts | closed | 2026-09-05 |
| [RS-2](../cards/resume-seam/RS-2-writer-frame-rebase.md) | the restored invariant DD branch is folded in the WRITER's linearization frame — Phase … | closed | 2026-09-12 |
| [RS-3 … RS-5](../cards/resume-seam/RS-3-to-RS-5-datum-and-emit-layer.md) | both datums continue at the seam, the phase-bias level is the EMIT layer, and the seam … | closed | 2026-09-13 |
| [RS-6a](../cards/resume-seam/RS-6a-station-layer-memories.md) | the station layer's own memory across the seam — the rx-CB carry and the T5 iono deviat… | closed | 2026-09-13 |
| [RS-7 … RS-9](../cards/resume-seam/RS-7-to-RS-9-border-and-rxcb-frame.md) | the dense border restores exactly and is NOT the residual; the flat 26 % is the rx-CB i… | closed | 2026-09-13 |
| [RS-8 … RS-10b](../cards/resume-seam/RS-8-to-RS-10b-latch-and-close-out.md) | the canonical-signal latch flips on 23 of 61 stations at a resume; the ZWD step is `zwd… | closed | 2026-09-14 |

**Standing documents** (still at their original paths, indexed not moved):

- [HANDOFF_seam_frame_restore_2026-09-03.md](../HANDOFF_seam_frame_restore_2026-09-03.md)
- [SPEC_warm_resume_event_card.md](../SPEC_warm_resume_event_card.md)

---

Back to [INDEX.md](../INDEX.md).
