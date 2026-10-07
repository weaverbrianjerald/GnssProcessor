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
| [§18.10/§18.11](../cards/resume-seam/RESUME-FRAME-RESTORE.md) | frame restore closes the phase side; 33 of 52 clocks trip, KEEP OFF until §18.14 | closed | 2026-09-03 |
| [§18.12](../cards/resume-seam/RESUME-COUPLING-RESTORE.md) | coupling-regressor restore: wire about 10× better, consumer misses the h12 stop rule | closed | 2026-09-05 |
| [§18.13/§18.14](../cards/resume-seam/RESUME-DENSE-BORDER-E2.md) | leg D misses the consumer bar; leg E (dense border) PASSES, resume knobs RECOMMEND_SCOPED | closed | 2026-09-05 |
| [RS-2](../cards/resume-seam/RS-2-writer-frame-rebase.md) | the DD blob is the field seam defect; re-base exact, NECESSARY NOT SUFFICIENT; D5 hold K=1 | closed | 2026-09-12 |
| [RS-3 … RS-5](../cards/resume-seam/RS-3-to-RS-5-datum-and-emit-layer.md) | both datums continue; the 17.7 m PB step is the cold emit layer; RS-5 closes the PB wire | closed | 2026-09-13 |
| [RS-6a](../cards/resume-seam/RS-6a-station-layer-memories.md) | station-layer restores: T5 removes the a₀₀ seam transient, rx-CB modest; most bars FAIL | closed | 2026-09-13 |
| [RS-7 … RS-9](../cards/resume-seam/RS-7-to-RS-9-border-and-rxcb-frame.md) | dense border KEEP_OFF (not the residual); rx-CB frame restored, withheld 26.3 % to 9.8 % | closed | 2026-09-13 |
| [RS-8 … RS-10b](../cards/resume-seam/RS-8-to-RS-10b-latch-and-close-out.md) | latch restore: seam chord births 64 to 1; ZWD reseed OFF without a ZWD carry; v8 seeds | closed | 2026-09-14 |

**Standing documents** (still at their original paths, indexed not moved):

- [HANDOFF_seam_frame_restore_2026-09-03.md](../HANDOFF_seam_frame_restore_2026-09-03.md)
- [SPEC_warm_resume_event_card.md](../SPEC_warm_resume_event_card.md)

---

Back to [INDEX.md](../INDEX.md).
