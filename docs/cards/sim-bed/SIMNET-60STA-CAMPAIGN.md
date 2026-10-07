# 60-sta campaign: the first long, multi-GNSS, realistic-slip grade

**STATUS**

- **id:** 60-sta campaign
- **topic:** sim-bed
- **status:** closed
- **summary:** Slip detection passes at 60 stations (94.1 % of 2,486 slips), but each refloat re-levels from one epoch of code and the level channels walk to metres unseen.
- **dates in the text:** 2026-08-20
- **knobs / posture:** `raim_arc_persist_epochs`; 60 sta + consumer, GPS+GAL triple+triple, field-N, 720 epochs
- **hashes named in the text** (commit / exe / byte-cert): `1b9e157`
- **run dirs named in the text:** `runs/simnet60_campaign`, `runs/simnet60_quiet`
- **source:** `docs/archive/SPEC_synthetic_minimum_network.md` §16; full record `docs/SPEC_slip_relevel_walk.md`

**Outcome.** The card holds the headlines of the first 6 h, multi-GNSS, field-N truth grade with a natural slip population; the full record is [SPEC_slip_relevel_walk](../../SPEC_slip_relevel_walk.md). The levels random-walk √t to ~6.5 m ZWD and 14 m clock contrast while σ̂₀, the rank gate, A4 and R stay silent; the quiet twin converges (ZWD 0.15 m). The `raim_arc_persist_epochs` spike premise is falsified on this error class. That record's §8 kills the walk under `network.cycle_slip.relevel_free_epoch`, which CLAUDE.md (wu93 amendment 2) arms on slipped sim postures.

> Results are appended TO THIS FILE, below the verbatim text. The archived monolith is frozen; never append there.

---
<!-- BEGIN VERBATIM: SPEC_synthetic_minimum_network.md -->
## §16 THE 60-STA CAMPAIGN — the first long / multi-GNSS / realistic-slip grade
## (2026-08-20; full record `docs/SPEC_slip_relevel_walk.md`)

Fixtures `runs/simnet60_campaign` (2 486 natural slips) + `runs/simnet60_quiet`
(quiet twin), 60 sta + consumer, GPS+GAL **triple+triple**, field-N, 720 ep,
post-`1b9e157`.  Headlines, recorded here so §-local readers see them:

- **The §9.5 "confidently wrong" class now has a MECHANISM**: at a realistic
  slip rate every refloat re-levels from one epoch of code and freezes the
  result — the level channels random-walk √t to ~6.5 m ZWD / 14 m clock
  contrast over 6 h while σ̂₀, the rank gate, A4, and invariant-#7's R all stay
  silent.  Six-arm elimination ladder + the quiet twin (ZWD 0.15 m,
  CONVERGING) pin the cause to slips; iono posture / arc persistence /
  coupling stack / Galileo exonerated.  QUEUE item 9 (ranks first).
- **Slip DETECTION passes at scale**: 94.1 % detected + 3.5 % responded of
  2 486 (new `scripts/simnet_slip_census.py`; silent 2.4 % are mask-edge
  arc-ends).  The response's re-coupling edge is the defect, not detection.
- **`raim_arc_persist_epochs` degenerates to ÷K** on this error class (the
  per-LOS iono error is a LEVEL, so arcs re-trip) — its spike premise is
  falsified here.
- **QUEUE 8(a) verdict (PERSISTS)** and its Table-8 split collateral live in
  `QUEUE_principled_replacements.md` §8; grader
  `scripts/simnet_iono_interaction.py` (the §7.8 decomposition, promoted).
- Posture facts at 60 sta: GF pin holds the code channel at p50 4.9 cm;
  GPS-only is WORSE than GPS+GAL; the coupling stack ATTENUATES refloat churn.
- Wire hygiene: 720/720 with 720 H lines (the RTA-10 pre-open H-buffer fix
  landed this session); Card-1f orphan attribution live (88 orphans, all PB).

---

