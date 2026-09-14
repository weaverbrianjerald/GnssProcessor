# OPEN ITEMS — what is owed, and where the evidence is

Every entry names the card (or standing document) that is its evidence. An item leaves
this page by being built and graded, by being falsified, or by being re-filed as a card —
never by being quietly dropped.

This page is a **register**, not a plan. The order of work for the network producer is
[the network re-founding](../refound/NETWORK_REFOUNDING_2026-09-14.md): RF-0 → RF-1 →
RF-2 → RF-3, graded on the cold 6 h consumer and the wire-vs-CODE census and nothing
else. Items below that the re-founding supersedes are marked as such rather than deleted,
because each is still a true statement about a defect.

Harvested 2026-09-14 from the STATUS blocks of every card (`status: open`) and from the
open registers the texts carry. Regenerate the card half with:

```bash
python scripts/docs_index_check.py          # gate
grep -l '^- \*\*status:\*\* open' -r docs/cards docs/decisions
```

---

## A. The current program

| item | where |
| --- | --- |
| RF-0 — the frozen posture on both beds, 6 h cold, the baseline numbers | [refound/NETWORK_REFOUNDING_2026-09-14.md](../refound/NETWORK_REFOUNDING_2026-09-14.md) |
| RF-1 — ambiguities as float STATES with covariance in the local block | same |
| RF-2 — a fixed S-basis with DECLARED S-transformations at every handover | same |
| RF-3 — QC that acts on CODE rows, and a wire contract that declares every step | same |
| Add-backs, one at a time: the SH field as a consumer product; L5; lanes; seams / warm resume | same |

The first finding RF-0 owes: the config that makes GPS **dual** solvable under W6 —
`frequency_mode: dual` alone leaves W6's L5 code-bias columns registered but unobserved.

---

## B. Open cards

Every card whose STATUS block says `status: open`. Twenty-four at the time of writing.

| topic | card | what is open | source section |
| --- | --- | --- | --- |
| `clock` | [Q3-F](../cards/clock/Q3-F-self-contained-time-link.md) | the self-contained clock time-link on the leveling vehicle | `SPEC_event_harness.md` — 24.16 |
| `consumer` | [CI](../cards/consumer/CI-consumer-instrument.md) | the closed-loop consumer as an INSTRUMENT — what the consumer bars have actually been reading, and the attribution legs | `SPEC_event_harness.md` — 24, 24.1, 24.2, 24.3 |
| `harness` | [DEC-field-vehicle](../decisions/DEC-field-vehicle-posture.md) | the field vehicle posture decision and the closing legs | `SPEC_event_harness.md` — 24.17 |
| `ionosphere` | [AMB-IONO](../cards/ionosphere/AMB-IONO-SPLIT.md) | why the network producer never reaches per-band integers on the quiet sim bed — the five measurements, the mechanism, the falsified list and the fix ladder | `SPEC_amb_iono_split_overcount.md` — (preamble), §0, §1, §2, 2.1, 2.2, 2.3, 2.4, 2.5, 2.6, 2.7, §3, 3.1, §4, §5, 5.1, §8 |
| `ionosphere` | [AMB-IONO open](../cards/ionosphere/AMB-IONO-OPEN-QUESTIONS.md) | the open-question register — re-cut round 2, with the round-4 closures and items 15–21 | `SPEC_amb_iono_split_overcount.md` — §7 |
| `ionosphere` | [LV roadmap](../cards/ionosphere/LV-ROADMAP.md) | the continuing session's roadmap, and its refresh at hand-back | `SPEC_level_information_budget.md` — 17, 27 |
| `ionosphere` | [LV-SEED](../cards/ionosphere/LV-SEED-per-los-level.md) | where the remaining per-LOS level is born — R1–R9 pre-registered reads | `SPEC_event_harness.md` — 24.13, 24.15 |
| `network-vehicle` | [CE](../cards/network-vehicle/CE-chain-era-bisect.md) | the CHAIN-ERA bisect on the leveling vehicle | `SPEC_event_harness.md` — 24.14 |
| `network-vehicle` | [CM2](../cards/network-vehicle/CM2-leveling-vehicle.md) | the second defect — the CM knob arms on the LEVELING vehicle | `SPEC_event_harness.md` — 24.12 |
| `network-vehicle` | [PV](../cards/network-vehicle/PV-vehicle-test.md) | the VEHICLE test — carry_invariant vs leveling, pre-registered | `SPEC_event_harness.md` — 24.11 |
| `network-vehicle` | [ZP](../cards/network-vehicle/ZP-station-block-parameterization.md) | the station block's parameterization at ignition — pre-registered | `SPEC_event_harness.md` — 24.10 |
| `queues` | [QPR](../cards/queues/QPR-00-CHARTER.md) | the charter — why this queue exists, how an item enters and leaves it, and the rank order | `QUEUE_principled_replacements.md` — (preamble) |
| `queues` | [QPR 0–3](../cards/queues/QPR-ITEMS-0-3.md) | items 0–3: the pin bridge, the S-transform re-reference family, datum-station election by criterion, and the clock's re-realization-free posture | `QUEUE_principled_replacements.md` — 0, 1, 2, 3 |
| `queues` | [QPR 4](../cards/queues/QPR-ITEM-4-T3-NULLS.md) | item 4: T3-class test statistics — correct nulls, not recalibrated constants | `QUEUE_principled_replacements.md` — 4 |
| `queues` | [QPR 5–8](../cards/queues/QPR-ITEMS-5-8.md) | items 5–8: the level contract, the half-epoch back-off ritual, the SH health-gate release policy, and the per-LOS ionosphere representation | `QUEUE_principled_replacements.md` — 5, 6, 7, 8 |
| `queues` | [QPR blocked](../cards/queues/QPR-BLOCKED-AND-CLOSED.md) | blocked on root cause, leave-alone (falsified or decided), and the queue's maintenance rule | `QUEUE_principled_replacements.md` — (see body) |
| `raim-qc` | [DEC-day129-gate-policy](../decisions/DEC-day129-gate-policy.md) | the gate-policy ruling — DEGRADE, DO NOT WITHHOLD; the statistic; the CB-excursion resume window; the validation gates; and the un-derived grace-floor magnitude | `SPEC_day129_frame_guards.md` — D, E1, E2, E3, E4 |
| `raim-qc` | [GL](../cards/raim-qc/GL-gross-los-screen.md) | the gross line-of-sight screen | `SPEC_event_harness.md` — 24.18 |
| `raim-qc` | [GL-2 + R8](../cards/raim-qc/GL-2-whole-satellite-standdown.md) | the whole-satellite stand-down, and R8 — the bulk drift's channel | `SPEC_event_harness.md` — 24.19 |
| `slip` | [REF-FAN](../cards/slip/REF-FAN.md) | the reference-slip fan-out under `if_phase_td_pairs = all` — RECOMMEND_SCOPED, the field pin still owed | `SPEC_event_harness.md` — 22.6, 22.7 |
| `slip` | [T4 lane restore](../cards/slip/T4-LANE-RESTORE-SPEC.md) | pend-and-restore v1 (value-only), `network.cycle_slip.relevel_t4_lane_restore` — spec only, not built | `SPEC_lane_slip_response.md` — §8 |
| `slip` | [rung 3](../cards/slip/LANE-SNAP-RUNG3.md) | the lane-preserving snap — v1/v2 falsified, the SEED SITE deleted as an exact solve no-op, and the v3 carry-domain observatory through its field arm | `SPEC_lane_slip_response.md` — §6, §7, §7.1, §7.2, §7.3, §7.4, §7.5, §7.6, §7.7, §7.8 |
| `slip` | [wind-up census](../cards/slip/WINDUP-FRACTIONAL-CENSUS.md) | the fractional-mode census — the two continuity horizons are INVERTED, `trace_windup_census`, and the restart response built default-OFF | `SPEC_phase_windup_sign.md` — §10, §10.1, §10.2, §10.3, §10.4, §10.5, §11 |
| `wire-contract` | [level health](../cards/wire-contract/LEVEL-HEALTH-CHANNEL.md) | `network.emit_level_health` — the contract gap, the one-channel three-class mechanism, and the normative consumer contract | `SPEC_withhold_era_release.md` — §12, §12.1, §12.2, §12.3, §12.4, §12.5, §12.6, §12.7, §12.8, §12.9 |

---

## C. Registers that hold more than a card row can

| register | what is in it | where |
| --- | --- | --- |
| The ambiguity/iono open questions | the re-cut round-2 register with the round-4 closures and items 15–21 — the largest single list of owed work in the corpus | [cards/ionosphere/AMB-IONO-OPEN-QUESTIONS.md](../cards/ionosphere/AMB-IONO-OPEN-QUESTIONS.md) (§7) |
| The principled-replacement queue | items 0–8 plus "blocked on root cause" and "leave alone" | [cards/queues/QPR-00-CHARTER.md](../cards/queues/QPR-00-CHARTER.md) and the four item cards |
| The edge-response queue | invariant #7's open valves, per family | [HANDOFF_edge_response_queue.md](../HANDOFF_edge_response_queue.md) |
| The open valve inventory | which valves still have a bare decoupling or re-coupling edge | [SPEC_prefit_census_observability.md](../SPEC_prefit_census_observability.md) §13 |
| Tier 3 of the config ledger | **every** row there is "plausible, never measured on field data" — the tier IS an owed list | [CONFIG_promotion_ledger.md](../CONFIG_promotion_ledger.md), `## Tier 3 — UNVALIDATED` |
| The consolidated OWED / gate ledger | the redundancy-census family's own gate list | [cards/raim-qc/REDUNDANCY-CENSUS.md](../cards/raim-qc/REDUNDANCY-CENSUS.md) (§15) |

---

## D. Named owed validations, from cards that are otherwise closed

A closed card can still owe a run. These are the ones the corpus names explicitly.

| owed | why it is open | evidence |
| --- | --- | --- |
| **REF-FAN's field pin (FP-2)** | RECOMMEND_SCOPED, never pinned: the 60-epoch matrix is clean but the 6 h twin refuses the pin on the slip-RESPONSE cost (+107 declared PB re-realizations). The lane-preserving refloat is the lever. | [cards/slip/REF-FAN.md](../cards/slip/REF-FAN.md) (§22.6–§22.7), [cards/harness/FIELD-HARNESS.md](../cards/harness/FIELD-HARNESS.md) (§20.3) |
| **`network.clock_datum_sum_to_zero` on the field** | first validation was sim-only (50-sta fixture, PASS); the field arm is still owed. The same matrix showed the bigger consumer lever is PIVOT ELECTION. | [cards/network-vehicle/DATUM-WANDER.md](../cards/network-vehicle/DATUM-WANDER.md) (§9.8) |
| **`network.clock_datum_broadcast_anchored`** | never validated; `declared_dimension = 0` with "no pivot, no handover seam", against a clock that is the worst re-realizer measured. | [CONFIG_promotion_ledger.md](../CONFIG_promotion_ledger.md), Tier 2 row `network.clock_datum_broadcast_anchored` |
| **`network.emit_clock_disc`** | without it the per-satellite clock disc counter is never populated, so product-wide clock jumps are silent to the consumer. | [CONFIG_promotion_ledger.md](../CONFIG_promotion_ledger.md), Tier 2 row `network.emit_clock_disc`; RF-3 subsumes it |
| **`network.raim_we_code_iono`** (WE-CI) | joins the fingerprint-SAFE armed set only once its first ON validation passes. | [CONFIG_promotion_ledger.md](../CONFIG_promotion_ledger.md), Tier 3 row `network.raim_we_code_iono` |
| **The within-epoch RAIM tier is `carry_warm`-gated** | so it is inert without `phase_leveling`. Decoupling it is a sanctioned open fix. | [CONFIG_promotion_ledger.md](../CONFIG_promotion_ledger.md), Tier 2 row `network.raim_within_epoch` |
| **`network.trace_dd_fractional_ref: oldest`** | BUILT and UNVALIDATED; the goldens stay on `lowest`. An `oldest` trace is quieter, never reference-FREE. | [cards/sim-bed/DD-TRACE-REF-SAT.md](../cards/sim-bed/DD-TRACE-REF-SAT.md) (§6.13) |
| **The wind-up accumulator's RESTART response** | the flip is blocked on an inverted horizon (accumulator 300 s vs `phase_leveling_max_gap_s: 900`); the census measured ~306 in-horizon wraps/day traded for ~120 uncompensated restarts/day. Build the response, not the flip. | [cards/slip/WINDUP-FRACTIONAL-CENSUS.md](../cards/slip/WINDUP-FRACTIONAL-CENSUS.md) (§10, §11) |
| **The SH health gate has no release / degrade policy** | a sick-field era withholds the wire for its full duration — 95+ min measured on DOY-129. | [cards/queues/QPR-ITEMS-5-8.md](../cards/queues/QPR-ITEMS-5-8.md) (item 7), [cards/wire-contract/LEVEL-HEALTH-CHANNEL.md](../cards/wire-contract/LEVEL-HEALTH-CHANNEL.md) |
| **The rx-CB inject gate is FRAME-BLIND** | it compares `\|value_abs − x_lin\|` against a frozen first-epoch seed, bar 5.13 m; 7 of 61 field stations were refused all day. RS-9 fixed continuity only — the gate itself is a solve-affecting card. | [cards/resume-seam/RS-7-to-RS-9-border-and-rxcb-frame.md](../cards/resume-seam/RS-7-to-RS-9-border-and-rxcb-frame.md) (§21.18) |
| **The DOY-129 grace-floor MAGNITUDE is un-derived** | the ruling's own named prerequisite; every number in the policy hangs off it. | [decisions/DEC-day129-gate-policy.md](../decisions/DEC-day129-gate-policy.md) (E4) |
| **`total_IF` was never graded in the lane A/B** | 1 h cannot settle the LEVEL channel; the lane verdict rests on everything except that channel. Grade it on ≥ 2 h before quoting it. | `CLAUDE.md`, "Amendments from the AGU-26 campaign"; [SPEC_lane_carry_card.md](../SPEC_lane_carry_card.md) |
| **The T4 lane-restore apply leg** | specified, not built — pend-and-restore v1 (value-only). | [cards/slip/T4-LANE-RESTORE-SPEC.md](../cards/slip/T4-LANE-RESTORE-SPEC.md) (§8) |

---

## E. Follow-ups this documentation pass created

| item | detail |
| --- | --- |
| **The ledger was deliberately left whole** | `docs/CONFIG_promotion_ledger.md` is a LIVING REGISTER, not a pile of closed cards: the `scripts/_*_ledger_oneoff.py` family (`_coldfb_`, `_fieldharness_`, `_rs2_refarc_`, `_scopedpromo_`, `_stspromo_`, `_study7_`) and every future promotion append a row at that exact path, in step with `CONFIG_promotion_ledger.json`. It is NOT split, NOT archived and NOT stubbed, and it carries a named size exemption in `scripts/docs_index_check.py` (`SIZE_EXEMPT`). Nothing to do — recorded so a later pass does not "finish the job". |
| **The 58 standing documents were indexed, not moved** | Each is referenced by path from `src/`, `include/`, `scripts/`, arm configs and the read-only memory directory. Relocating them into `topics/` or `cards/` is a later pass, and it should happen together with the memory re-pointing below so the two do not strand each other. |
| **123 memory lines across 67 memory files cite a split doc** | None break — the routing stub resolves every `§N.N` in one hop — but the author's own directive says memory files should point at the card files once the tree exists. The per-doc list is in [REORG_PLAN.md](../REORG_PLAN.md) §7. The memory directory is read-only for automated work; this is the author's call. |
| **The largest card is 59.0 KB and cannot be split** | `cards/ionosphere/AMB-IONO-OPEN-QUESTIONS.md` — `SPEC_amb_iono` §7 has no internal heading, so cutting it would mean editing text. When its items are re-filed as cards (register C above), the file shrinks by construction. |
| **`scripts/` was not reorganised** | Paths are cited everywhere; [scripts/README.md](../../scripts/README.md) groups every script by topic and marks the ones nothing references as archive candidates for a later pass. |
