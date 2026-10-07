# `slip` — Cycle slips — detection, response, and the lanes

Detection (MW, GF, LLI, IF-TD time difference), the RESPONSE to a detection (erase,
refloat, re-tap, re-level, band scope), and the lane cascade (EWL / WL / UWL, the lane
carry and the lane spectrometer). The headline result of the topic: the lane-integer slip
cost is **100 % response-inflicted** — response-virgin chords sit at 98.9–100 % of their
own true integer.

**Conventions and invariants**

- Detector thresholds are EMPIRICAL only, and they are set by data rate: 30 / 60 s needs
  relaxed MW / LLI / IF-TD.
- A signal with no satellite bias gets a KF-triggered reset, not an MW repair.
- Sub-cycle suppression HURTS position — do not extend it.
- Zero-q phase leveling REQUIRES a wired slip detector (standing invariant #1); the
  re-level's own re-coupling edge is what random-walks the level channel by metres.
- `relevel_retap_statistical` and `relevel_retap_corroborate` are KEEP_OFF — both
  falsified on the lane-integer instrument. The open build is the LANE-PRESERVING refloat.
- The seed-site lane snap is DELETED and must not be rebuilt: its only write was an
  `x_lin` shift, an exact solve no-op. The lane lives in `LocalCarry::value_abs`.

**Current posture (2026-09-14)** — lanes OFF under the re-founding's scope cut; the slip
response becomes "drop the prior (refloat)" once ambiguities are float states with
covariance (RF-1). See [the network re-founding](../refound/NETWORK_REFOUNDING_2026-09-14.md).

## Cards, oldest first

Status is one of `open`, `closed`, `parked`, `superseded`, `reference`; the verdict a card reached is in its summary and its text. Results are appended to the card file, never here.

| card | one line | status | last dated |
| --- | --- | --- | --- |
| [slip-QC impl](../cards/slip/SLIPQC-IMPL-2026-07-21.md) | slip-QC build of cards E/R/L/G: Card E MET, Card R PARTIAL (14 to 4), Card L premise false | closed | 2026-08-12 |
| [D5-A](../cards/slip/D5A-RETAP-ATTRIBUTION.md) | the D5 re-tap instrument: 3.0 % raw-step coincidence, re-tap count a third overstated | closed | 2026-08-15 |
| [slip-QC follow-ups](../cards/slip/SLIPQC-FOLLOWUPS.md) | slip-QC follow-ups: D5-A, H1/H2 landed; Card W never launched, OP untriggered, H3 deferred | closed | 2026-08-15 |
| [lane slip response](../cards/slip/LANE-SLIP-RESPONSE.md) | lane-integer slip cost 100 % response-inflicted; both retap keeps FALSIFIED, KEEP_OFF | closed | 2026-08-22 |
| [T4 lane restore](../cards/slip/T4-LANE-RESTORE-SPEC.md) | pend-and-restore of the pre-erase carry after a T4 erase: specified, not built | open | 2026-08-23 |
| [rung 3](../cards/slip/LANE-SNAP-RUNG3.md) | seed-site snap deleted (solve no-op); observatory solve-inert; apply leg re-scoped to T4 | open | 2026-08-23 |
| [wind-up census](../cards/slip/WINDUP-FRACTIONAL-CENSUS.md) | 20 gaps in 4 h between the wind-up and carry horizons; restart response ARMED by card WUP | closed | 2026-08-23 |
| [IF-TD ref gate](../cards/slip/IFTD-REF-GATE.md) | the IF-TD ref gate is 100 % false on the sim bed; Card R takes acting refloats 546 to 0 | closed | 2026-09-08 |
| [Card G response](../cards/slip/CARD-G-GAP-REENTRY-RELEVEL.md) | gap re-entry re-level: bridged arcs 88 to 0, consumer +56 %; KEEP_OFF; armed on float | closed | 2026-09-09 |
| [IF-TD ref gate (v2)](../cards/slip/IFTD-REF-GATE-FOLLOWUP.md) | warm-up abstain removes 118 cold fires, elev_norm adds 2; both UNVALIDATED, fault arm owed | closed | 2026-09-09 |
| [slip-QC](../cards/slip/SLIPQC-REF.md) | slip-QC hardening spec and status ledger: cards R/L/E/G, D5 lead closed, Card W never run | reference | 2026-09-09 |
| [Card ORACLE](../cards/slip/CARD-ORACLE-RELEVEL.md) | sim-only oracle re-level: exact repair, every gate FAILS (T4 +77 %); KEEP_OFF diagnostic | closed | 2026-09-10 |
| [Study 3](../cards/slip/STUDY-3-band-scoped-responses.md) | band-scoped slip responses: both knobs KEEP_OFF, the T4 erase scope alone collapses the WL | closed | 2026-09-10 |
| [IF-TD pairs](../cards/slip/IFTD-PAIRS.md) | all IF-TD pairs per satellite: equal-count trap closed, F2 and F3 finish it; RECOMMEND_ON | closed | 2026-09-11 |
| [Study 4](../cards/slip/STUDY-4-slip-dose.md) | slip dose: anchor-arc slips drive the cascade; anchor-slip handover RECOMMEND_ON | closed | 2026-09-11 |
| [Study 5](../cards/slip/STUDY-5-detector-coverage-l5.md) | unflagged L5 slips dropped by the repair gate; `mw_evidence_refloat` RECOMMEND_ON | closed | 2026-09-11 |
| [Study 6](../cards/slip/STUDY-6-common-band-scope.md) | common-band scope KEEP_OFF (consumer +3.3 cm); superseded by Study 7's RECOMMEND_ON | superseded | 2026-09-11 |
| [Study 7](../cards/slip/STUDY-7-common-band-narrowing.md) | common-band scope + IF-TD on every pair RECOMMEND_ON together, sim p50 0.174 to 0.120 m | closed | 2026-09-11 |
| [REF-ARC](../cards/slip/REF-ARC-CLAUSE.md) | ref-arc clause PROMOTED: an untested reference pair cannot exonerate; sim storm neutral | closed | 2026-09-13 |
| [REF-FAN](../cards/slip/REF-FAN.md) | reference-slip fan-out: RECOMMEND_SCOPED, collateral refloats 6 to 0; field pin still owed | open | 2026-09-13 |
| [RULE F2](../cards/slip/IFTD-F2-lli-shadow.md) | the LLI shadow: a pair holding a band flagged last epoch cannot exonerate; S1 storm closed | closed | — |
| [RULE F3](../cards/slip/IFTD-F3-lli-epoch.md) | the LLI-epoch verdict: S3 residual 0.335 to 0.005 cyc; its anchor clause keeps references | closed | — |

**Standing documents** (still at their original paths, indexed not moved):

- [SPEC_ewl_wl_cascade_card.md](../SPEC_ewl_wl_cascade_card.md)
- [SPEC_lane_carry_card.md](../SPEC_lane_carry_card.md)
- [SPEC_lane_spectrometer.md](../SPEC_lane_spectrometer.md)
- [SPEC_mw_wl_card.md](../SPEC_mw_wl_card.md)
- [SPEC_noncausal_slip_bridge.md](../SPEC_noncausal_slip_bridge.md)
- [SPEC_sd_ewl_prereduce.md](../SPEC_sd_ewl_prereduce.md)
- [SPEC_slip_relevel_walk.md](../SPEC_slip_relevel_walk.md)
- [SPEC_uwl_card.md](../SPEC_uwl_card.md)
- [slip_comparison_report.md](../slip_comparison_report.md)
- [turbo_edit_lli_bridge_design.md](../turbo_edit_lli_bridge_design.md)

---

Back to [INDEX.md](../INDEX.md).
