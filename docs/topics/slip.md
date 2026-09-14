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
| [slip-QC impl](../cards/slip/SLIPQC-IMPL-2026-07-21.md) | the 2026-07-21 implementation record — what landed, the five findings that contradicted… | closed | 2026-08-12 |
| [D5-A](../cards/slip/D5A-RETAP-ATTRIBUTION.md) | D5 re-tap attribution as built, the cross-experiment protocol, and the 2026-07-22 valid… | closed | 2026-08-15 |
| [slip-QC follow-ups](../cards/slip/SLIPQC-FOLLOWUPS.md) | the 2026-07-22 follow-up cards D5-A / W / OP / H, and why the D5 lead outranked everyth… | closed | 2026-08-15 |
| [lane slip response](../cards/slip/LANE-SLIP-RESPONSE.md) | the headline decomposition — the lane-integer slip cost is 100 % response-inflicted; th… | closed | 2026-08-22 |
| [T4 lane restore](../cards/slip/T4-LANE-RESTORE-SPEC.md) | pend-and-restore v1 (value-only), `network.cycle_slip.relevel_t4_lane_restore` — spec o… | open | 2026-08-23 |
| [rung 3](../cards/slip/LANE-SNAP-RUNG3.md) | the lane-preserving snap — v1/v2 falsified, the SEED SITE deleted as an exact solve no-… | open | 2026-08-23 |
| [wind-up census](../cards/slip/WINDUP-FRACTIONAL-CENSUS.md) | the fractional-mode census — the two continuity horizons are INVERTED, `trace_windup_ce… | open | 2026-08-23 |
| [IF-TD ref gate](../cards/slip/IFTD-REF-GATE.md) | the IF-TD reference gate is 100 % false on the sim bed, and the fix was already in the … | closed | 2026-09-08 |
| [Card G response](../cards/slip/CARD-G-GAP-REENTRY-RELEVEL.md) | `gap_reentry_relevel` — round 1 was inert on the live-state walk, round 2's verdict, an… | closed | 2026-09-09 |
| [IF-TD ref gate (v2)](../cards/slip/IFTD-REF-GATE-FOLLOWUP.md) | abstain while cold, normalize the bar by the reference's elevation, and close the gatin… | closed | 2026-09-09 |
| [slip-QC](../cards/slip/SLIPQC-REF.md) | the slip-QC hardening spec — evidence base, the binding ground rules, cards R / L / E /… | reference | 2026-09-09 |
| [Card ORACLE](../cards/slip/CARD-ORACLE-RELEVEL.md) | `oracle_relevel`, the sim-only CEILING of a level-preserving re-level — the mechanism i… | closed | 2026-09-10 |
| [Study 3](../cards/slip/STUDY-3-band-scoped-responses.md) | band-scoped slip responses, the production knobs — the combined arm FAILS at storm scale | closed | 2026-09-10 |
| [IF-TD pairs](../cards/slip/IFTD-PAIRS.md) | the IF-TD detector watched ONE lane per satellite — `if_phase_td_pairs` | closed | 2026-09-11 |
| [Study 4](../cards/slip/STUDY-4-slip-dose.md) | the slip DOSE — the anchor-slip handover cure, validated on the harness and on the stor… | closed | 2026-09-11 |
| [Study 5](../cards/slip/STUDY-5-detector-coverage-l5.md) | detector coverage — the unflagged GPS L5 slip and `mw_evidence_refloat` | closed | 2026-09-11 |
| [Study 6](../cards/slip/STUDY-6-common-band-scope.md) | the COMMON-BAND scope — free only the band the evidence names | closed | 2026-09-11 |
| [Study 7](../cards/slip/STUDY-7-common-band-narrowing.md) | the common-band scope's 3 cm is ONE wrong narrowing at 4.99 h | closed | 2026-09-11 |
| [REF-ARC](../cards/slip/REF-ARC-CLAUSE.md) | a chain's reference-sat pair is UNTESTED and cannot exonerate — `common_band_ref_arc`, … | closed | 2026-09-13 |
| [REF-FAN](../cards/slip/REF-FAN.md) | the reference-slip fan-out under `if_phase_td_pairs = all` — RECOMMEND_SCOPED, the fiel… | open | 2026-09-13 |
| [RULE F2](../cards/slip/IFTD-F2-lli-shadow.md) | the LLI shadow — a lane that did not see the flagged epoch is not silent | closed | — |
| [RULE F3](../cards/slip/IFTD-F3-lli-epoch.md) | the LLI-EPOCH verdict, and the ANCHOR CLAUSE inside F3 | closed | — |

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
