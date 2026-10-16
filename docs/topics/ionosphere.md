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
| [EXT-CB defect 2](../cards/ionosphere/LV-EXTCB-DEFECT2.md) | station-support artifact: ignition a₀₀ − GIM −1.98 TECU at 303 stations vs −15.83 at 61 | closed | 2026-08-05 |
| [LV correction](../cards/ionosphere/LV-CORRECTION-AND-12H.md) | de-mean pins a frozen epoch-1 frame; 12 h LV stack ramps +15 to +21 TECU: do NOT arm | closed | 2026-08-05 |
| [LV item 1](../cards/ionosphere/LV-ITEM1-FALSIFIED.md) | lv-4 alone failed stage 1 at obs mask 10; the FALSIFIED verdict retracted at obs mask 15 | superseded | 2026-08-05 |
| [LV mask study](../cards/ionosphere/LV-MASK-STUDY.md) | mask study closes on obs mask 15 / iono cutoff 25 at both scales; lv-4 passes stage 2 | closed | 2026-08-05 |
| [LV residual](../cards/ionosphere/LV-RESIDUAL-EXTRAPOLATION.md) | level residual not an extrapolation artifact; the rx_cb_carry_q axis is exhausted | closed | 2026-08-05 |
| [LV tracking pin](../cards/ionosphere/LV-TRACKING-PIN.md) | tracking-pin fix FALSIFIED: rx_cb_carry_q 1e-9 to 1e-6 made the level ramp worse, sooner | closed | 2026-08-05 |
| [LV-4](../cards/ionosphere/LV-4-FRAME-INVARIANT-SCREEN.md) | frame-invariant rx-CB step screen: correct fix; needs LV-5, costs +16 to +64 % phase rms | closed | 2026-08-05 |
| [LV-5](../cards/ionosphere/LV-5-MU-WITHIN-EPOCH.md) | within-epoch μ: limit cycle gone (sd 15.53 → 4.69 TECU); max \|μ\| prediction MISSED | closed | 2026-08-05 |
| [LV roadmap](../cards/ionosphere/LV-ROADMAP.md) | lv action list superseded: item 3 built as S2H, level authority moved to LV-6 | superseded | 2026-08-07 |
| [LV impl](../cards/ionosphere/LV-IMPL-RECORD.md) | built LV-1 and LV-2; seed-gradient model FALSIFIED, LV-1 acts only via the rx-CB carry | closed | 2026-08-12 |
| [elev-cutoff A/B](../cards/ionosphere/ELEV-CUTOFF-AB.md) | iono cutoff with truth: 15° beats 25° on every clock channel; the field verdict inverts | closed | 2026-08-12 |
| [iono-field agreement](../cards/ionosphere/IONO-FIELD-AGREEMENT.md) | iono input coherent both-high (corr 0.981), broken low-high; 25° cutoff trims wander 8 % | closed | 2026-08-12 |
| [EXT-CB arms 1–36](../cards/ionosphere/LV-EXTCB-CAMPAIGN.md) | level defect = receiver GF-bias convention offset + ignition transient; L2-ESCAPE closed | closed | 2026-08-15 |
| [LV](../cards/ionosphere/LV-REF.md) | reference: lv ladder knob and verdict ledgers, section map, binding grading protocol | reference | 2026-08-21 |
| [AMB-IONO](../cards/ionosphere/AMB-IONO-SPLIT.md) | closed: diagonal carry over-counts each LOS's iono per band (B 3.00); F3's block is now float_kf's structure | closed | 2026-09-04 |
| [F3 (pre-reg)](../cards/ionosphere/AMB-IONO-F3-PREDICTIONS.md) | f3 pre-registration: F3 beat its 3 h line 2.7×, F1 intermediate, var floor FALSIFIED | closed | 2026-09-04 |
| [F3 24 h](../cards/ionosphere/AMB-IONO-F3-24H.md) | settled quarter FALSIFIED as traced; the episodes were the DD trace's reference satellite | superseded | 2026-09-05 |
| [0910 afternoon](../cards/ionosphere/AMB-IONO-0910-AFTERNOON.md) | level memory FAILS x4; fewer T4 erases FAIL the consumer at every K; continuity KEEP_OFF | closed | 2026-09-10 |
| [0910 morning](../cards/ionosphere/AMB-IONO-0910-MORNING.md) | oracle repair FAILS, PB-gauge levelling FALSIFIED; the phase bias is a receiver's float | closed | 2026-09-10 |
| [0910 second half](../cards/ionosphere/AMB-IONO-0910-SECOND-HALF.md) | eleven probes FAIL with one shape; emission continuity FALSIFIED; producer AR NO-GO | closed | 2026-09-10 |
| [AMB-IONO open](../cards/ionosphere/AMB-IONO-OPEN-QUESTIONS.md) | closed: 45 items reviewed: 13 done, 10 falsified, 12 superseded, 7 moot, 2 dup; 1 owed (unread config keys) | closed | 2026-09-10 |
| [LV-SEED](../cards/ionosphere/LV-SEED-per-los-level.md) | all three hypotheses refuted (arc start, iono, geometry); wander is the satellite clock | closed | 2026-09-14 |
| [LV-1](../cards/ionosphere/LV-1-BIAS-FREE-SEEDING.md) | bias-free iono seeding: stated mechanism FALSIFIED (a solve no-op); kept as a diagnostic | closed | — |
| [LV-2](../cards/ionosphere/LV-2-RXCB-DEMEAN.md) | rx-CB de-mean: do NOT arm, it re-pins the common mode to a frozen frame (+15 to +21 TECU) | closed | — |
| [LV-3](../cards/ionosphere/LV-3-LEVEL-AUTHORITY.md) | level authority made explicit: contract is §30; broadcast a00 prior deliberately not built | closed | — |

**Standing documents** (still at their original paths, indexed not moved):

- [SPEC_deweight_release_hygiene.md](../SPEC_deweight_release_hygiene.md)
- [SPEC_iono2_mechB_v3.md](../SPEC_iono2_mechB_v3.md)
- [SPEC_iono_carry_8b.md](../SPEC_iono_carry_8b.md)
- [SPEC_level_authority_internal.md](../SPEC_level_authority_internal.md)
- [network_global_sh_iono_design_2026-06-29.md](../network_global_sh_iono_design_2026-06-29.md)

---

Back to [INDEX.md](../INDEX.md).
