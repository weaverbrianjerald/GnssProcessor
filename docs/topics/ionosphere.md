# `ionosphere` — The ionosphere — the SH field, the per-LOS representation, the level

Two separable problems live here: the **representation** (a spherical-harmonic VTEC field
versus free per-LOS slant iono, and the ties between them) and the **level** (who owns the
absolute offset of the estimated ionosphere and the receiver-DCB frame that goes with it).
The ambiguity/iono split over-count — why the producer never reaches per-band integers on
a quiet simulated bed — is the deepest card in the topic.

**Conventions and invariants**

- **Never grade the level against a single GIM number, and never on a 1 h window.** The
  GIM's own global mean moves through the day, and the level channel's convergence budget
  is ~2 h. Use `scripts/gim_global_mean_series.py`.
- External data is DIAGNOSTIC only — a GIM may explain a delta, never gate a decision.
- `network_ionosphere.csv` is a CORRECTION, not absolute slant iono; synthesize
  `network_vtec_sh.csv` with `scripts/sh_vtec.py` when the estimated field is wanted.
- Do not "fix" the level by tuning the rx-CB carry's memory model in either direction —
  freezing it drifts +20 TECU, loosening it +25. It needs a per-epoch data-derived
  authority instead.

**Current posture (2026-09-14)** — free slant iono; `network.estimate_iono_sh: false`,
the SH field returns later as a CONSUMER product and never as a producer constraint.
See [the network re-founding](../refound/NETWORK_REFOUNDING_2026-09-14.md).

## Cards, oldest first

Status is one of `open`, `closed`, `parked`, `superseded`, `reference`; the verdict a card reached is in its summary and its text. Results are appended to the card file, never here.

| card | one line | status | last dated |
| --- | --- | --- | --- |
| [EXT-CB defect 2](../cards/ionosphere/LV-EXTCB-DEFECT2.md) | arm 30 and the 303-station EXT-CB arm — DEFECT 2 is largely a station-support artifact | closed | 2026-08-05 |
| [LV correction](../cards/ionosphere/LV-CORRECTION-AND-12H.md) | what LV-2 actually does and what LV-1 is really for — and the 12 h verdict that made th… | closed | 2026-08-05 |
| [LV item 1](../cards/ionosphere/LV-ITEM1-FALSIFIED.md) | roadmap item 1 falsified at stage 1 — the cost attributes to LV-4 alone, plus two invar… | closed | 2026-08-05 |
| [LV mask study](../cards/ionosphere/LV-MASK-STUDY.md) | the mask study, end to end — the trade is regime-dependent, it flips sign between 61 an… | closed | 2026-08-05 |
| [LV residual](../cards/ionosphere/LV-RESIDUAL-EXTRAPOLATION.md) | the residual is not an extrapolation artifact, and `rx_cb_carry_q` is not simply too ti… | closed | 2026-08-05 |
| [LV tracking pin](../cards/ionosphere/LV-TRACKING-PIN.md) | the tracking-pin fix, tested and FALSIFIED | closed | 2026-08-05 |
| [LV-4](../cards/ionosphere/LV-4-FRAME-INVARIANT-SCREEN.md) | the carry screen made frame-invariant — fixed, and it exposed a second defect | closed | 2026-08-05 |
| [LV-5](../cards/ionosphere/LV-5-MU-WITHIN-EPOCH.md) | μ computed within the epoch, with the review addendum's requirements | closed | 2026-08-05 |
| [LV roadmap](../cards/ionosphere/LV-ROADMAP.md) | the continuing session's roadmap, and its refresh at hand-back | open | 2026-08-07 |
| [LV impl](../cards/ionosphere/LV-IMPL-RECORD.md) | the 2026-08-05 implementation record, and the measured outcomes that corrected the mech… | closed | 2026-08-12 |
| [elev-cutoff A/B](../cards/ionosphere/ELEV-CUTOFF-AB.md) | the elevation-cutoff A/B WITH TRUTH — the field verdict INVERTS on a coherent field | closed | 2026-08-12 |
| [iono-field agreement](../cards/ionosphere/IONO-FIELD-AGREEMENT.md) | cross-station iono-field agreement — coherent where mapped, BROKEN at low–high, plus th… | closed | 2026-08-12 |
| [EXT-CB arms 1–36](../cards/ionosphere/LV-EXTCB-CAMPAIGN.md) | the EXT-CB campaign record, arms 1–36 — root cause, the fix, and the L2-ESCAPE | closed | 2026-08-15 |
| [LV](../cards/ionosphere/LV-REF.md) | the ionosphere-level initiative's own reference page — knob ledger, verdict ledger, sec… | reference | 2026-08-21 |
| [AMB-IONO](../cards/ionosphere/AMB-IONO-SPLIT.md) | why the network producer never reaches per-band integers on the quiet sim bed — the fiv… | open | 2026-09-04 |
| [F3 (pre-reg)](../cards/ionosphere/AMB-IONO-F3-PREDICTIONS.md) | the pre-registered predictions for the F3 station block, and the round-2 / round-4 resu… | closed | 2026-09-04 |
| [F3 24 h](../cards/ionosphere/AMB-IONO-F3-24H.md) | the 24 h F3 arm — the settled number is NOT met, and the reason is an EDGE, not the mec… | closed | 2026-09-05 |
| [0910 afternoon](../cards/ionosphere/AMB-IONO-0910-AFTERNOON.md) | the level-memory and T4-persistence ladders, the wire wander, and the continuity card | closed | 2026-09-10 |
| [0910 morning](../cards/ionosphere/AMB-IONO-0910-MORNING.md) | the same-build control, three truth censuses, two falsified cards, and the field's phas… | closed | 2026-09-10 |
| [0910 second half](../cards/ionosphere/AMB-IONO-0910-SECOND-HALF.md) | the slipped-bed probe ledger, the continuity closure, the false-alarm isolation, and th… | closed | 2026-09-10 |
| [AMB-IONO open](../cards/ionosphere/AMB-IONO-OPEN-QUESTIONS.md) | the open-question register — re-cut round 2, with the round-4 closures and items 15–21 | open | 2026-09-10 |
| [LV-SEED](../cards/ionosphere/LV-SEED-per-los-level.md) | where the remaining per-LOS level is born — R1–R9 pre-registered reads | open | 2026-09-14 |
| [LV-1](../cards/ionosphere/LV-1-BIAS-FREE-SEEDING.md) | bias-free per-LOS iono seeding — the seed is a FRAME, not an authority | closed | — |
| [LV-2](../cards/ionosphere/LV-2-RXCB-DEMEAN.md) | rx-CB carry common-mode de-mean — works as designed, but the de-mean is NOT frame-invar… | closed | — |
| [LV-3](../cards/ionosphere/LV-3-LEVEL-AUTHORITY.md) | the level authority, made explicit — mostly documentation, one optional knob | closed | — |

**Standing documents** (still at their original paths, indexed not moved):

- [SPEC_deweight_release_hygiene.md](../SPEC_deweight_release_hygiene.md)
- [SPEC_iono2_mechB_v3.md](../SPEC_iono2_mechB_v3.md)
- [SPEC_iono_carry_8b.md](../SPEC_iono_carry_8b.md)
- [SPEC_level_authority_internal.md](../SPEC_level_authority_internal.md)
- [network_global_sh_iono_design_2026-06-29.md](../network_global_sh_iono_design_2026-06-29.md)

---

Back to [INDEX.md](../INDEX.md).
