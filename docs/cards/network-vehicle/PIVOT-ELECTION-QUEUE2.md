# queue item 2: datum-station election by criterion, and item 1(b) priced

**STATUS**

- **id:** queue item 2
- **topic:** network-vehicle
- **status:** closed
- **summary:** Datum-station election by criterion landed as tooling (generator election, config preflight, no producer change); the handover S-transform, item 1(b), stays deferred because its measured target population is near-empty.
- **dates in the text:** 2026-08-21
- **knobs / posture:** `network.sticky_pivot` (named as the first response to handover flux); tools `scripts/simnet_make_producer_config.py` (election) and `scripts/datum_gauge_audit.py` (`--config` preflight, `--run` gate)
- **hashes named in the text** (commit / exe / byte-cert): none
- **run dirs named in the text:** `runs/camp60_i3`, `runs/fld63_relevel4h_on`
- **source:** `docs/archive/SPEC_pivot_visibility_gauge_defect.md` §10, §11

**Outcome.** The questions: can tooling enforce the gauge-defect card's clean datum election, and is item 1(b)'s one-epoch refloat cost worth a build. The generator elects by fewest mid-arc losses, then the most equatorial station; the run gate reads 0 avoidable removals on the elected sim datum and 12, all unmarked, over 4 h on the field. The pricing found 0 handover edges in 4 h on the sticky field lineage and 3 one-epoch fold-skips in 6 h on the sim. Item 1(b) was later built and PROMOTED as `network.handover_stransform` ([PIVOT DEPARTURE](../network-vehicle/PIVOT-DEPARTURE.md)).

> Results are appended TO THIS FILE, below the verbatim text. The archived monolith is frozen; never append there.

---
<!-- BEGIN VERBATIM: SPEC_pivot_visibility_gauge_defect.md -->
## §10 QUEUE item 2 EXECUTED (2026-08-21) — the election is formalized as plumbing; first standard-gate readings

The §4-measured remedy (clean election: 5.9× datum-free contrast, 40 % T3
cut) is now enforced by tooling, not luck.  No producer code changed — every
producer-side alternative in this space is falsified (§ "What this card is
NOT") and stays that way.

- **Generator election** (`scripts/simnet_make_producer_config.py`): with no
  `--datum-station`, the datum is ELECTED — fewest mid-arc losses over the
  post-stride roster (`datum_gauge_audit.compute_rank`), most equatorial
  among ties (§9.8's 8× enforced, not advised), name for determinism (the
  elected value is warm-resume-fingerprinted).  An explicit station absent
  from the roster is a hard error (exit 2, ranking printed) — the old silent
  fall-back to the most-polar `stations[0]` is gone.  `--constellations`
  matches the producer's active systems (a satellite the producer does not
  estimate cannot cost a gauge event).
- **Field-config preflight** (`datum_gauge_audit.py --config <json>`):
  flattens the extends chain (`gnss_config_chain.load_config`) and gates the
  declaration — set (exit 1 on the `stations.front()` default), in-roster,
  not polar (≥55°) while an equatorial (≤30°) alternative exists
  (`--allow-polar` downgrades to a warning), marking channel armed
  (`emit_clock_disc` + `clock_disc_jump_test`), datum-station slip response
  armed (invariant #1).  Continuity is documented as NOT statically
  checkable — `--run` is the post-hoc, truth-free gate (exit 1 on any
  avoidable removal, both modes sharing the harness-gate exit).
- **Validation against this SPEC's own record**: the 24-sta GPS-only
  election returns **S06000SIM (0 losses)** — arm B, reproduced; the 60-sta
  GPS+GAL election returns **S30000SIM (10 losses, −0.9°)**, winning the
  equatorial tiebreak over S26000SIM (10 losses, −8.1°) — the camp60
  campaign's hand-elected datum, retro-validated by the formalized
  criterion.  All fail paths verified (unset ⇒ exit 1; absent ⇒ exit 2, no
  config written).
- **First standard-gate readings**: `--run runs/camp60_i3` (elected datum)
  ⇒ **0 avoidable removals, exit 0**.  `--run runs/fld63_relevel4h_on`
  (field, MBAR00UGA) ⇒ **12 avoidable pivot-only removals over 4 h, ALL
  unmarked** — exit 1.  The preflight predicts the silence exactly: the
  ext63 lineage arms `emit_clock_disc` but NOT `clock_disc_jump_test`, so
  every avoidable re-realization is silent to the consumer; it also runs
  without `datum_station_detect`.  MBAR00UGA's geometry is fine (−0.6°);
  its §4.2 "arrived at by luck" continuity did not survive a 4 h window.
  Filed for the ext63 lineage owners: arm the marking channel
  (fingerprint-SAFE trace-class per the armed-set rules) and re-read; the
  12 events are the datum-election exposure item 1(b)'s S-transform would
  close producer-internally.

## §11 Item 1(b) precondition PRICED (2026-08-21) — the refloat cost measures near-zero; the build stays deferred with a concrete trigger

§8.9's standing verdict ("build the transform when a bigger fixture shows the
one-epoch refloat cost") was written when the only fixture had ONE roster
event in 1 h.  The bigger fixtures now exist, and the cost is measured from
data on disk:

| lineage | window | sticky_pivot | PIVOT/handover edges | amputation |
|---|---|---|---|---|
| fld63_relevel4h (63-sta field, GPS dual) | 4 h | **true** | **0** | zero |
| camp60_i3 / _f4 (60-sta sim, GPS+GAL, slipped) | 6 h | off | **3** (G×2, E×1, all `adjacent`) | 3 single-epoch fold-skips per run vs 720 epochs |

The §5.08.5 "30 family-epochs" headroom figure belongs to the PB-anchor edge
on the pdp posture — not this edge.  On every current lineage the clock-pivot
handover flux is either zero (sticky election, RTA-1, doing exactly its job)
or ~1 edge / 2 h with a one-epoch cost the prires census prices as noise.
The mechanism's target population is measured near-empty — the
`emit_pb_filter_gate_v2` lesson ("its advertised win has an empty target
population here"), applied BEFORE the build this time.

**Disposition: 1(b) remains deferred-conditional, precondition now PRICED
rather than assumed.**  The §8 design (rank-1 transform along the structural
null, INC-0..3, four knobs) is fully derived and stays ready.  Concrete build
triggers, any of: (a) a lineage that must run non-sticky election with
measured handover flux ≳ 1/h; (b) a vanish-return-heavy roster (arm
`pivot_vanish_return_signal` and read the PIVOT arms mix first); (c) a
measured per-edge consumer cost at an `adjacent` handover (the A2-marked
epoch class) that the one-epoch free-fall demonstrably owns.  First response
on any lineage showing flux is `network.sticky_pivot`, not the transform.
Census note for the next pricing: the camp60 arms ran
`prior_residual_families: "iono"` only, so FRAME/mech=handover rows were not
censused there — a future pricing arm should widen the families to see the
per-edge amputation directly.

---

