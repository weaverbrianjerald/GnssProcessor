# GnssProcessor — documentation index

The single entry point for `docs/`. Every markdown file under `docs/` is reachable
from this page. If you are looking for a result, find its **topic** below, then its
**card**; if you are adding a result, read [the rules](#the-rules) first — they exist
because this corpus had grown into four files holding 39 % of the record.

---

## The current program — the network re-founding (2026-09-14)

**[refound/NETWORK_REFOUNDING_2026-09-14.md](refound/NETWORK_REFOUNDING_2026-09-14.md)
— the author's decision that replaces the card-by-card patching of the network
producer. It is the entry point for the network from this date on.**

- **The frozen posture (RF-0):** the leveling vehicle + gauge projection + phase-datum
  family, free slant iono, the physical clock time-link, the clock step test; scope cut
  to GPS-only dual-frequency, no lanes, no F3, no seams.
- **The only two bars:** the cold 6 h consumer, and the wire-vs-CODE census. Everything
  else is diagnostic.
- **The card order:** RF-0 (freeze + baseline) → RF-1 (ambiguities as float states) →
  RF-2 (fixed S-basis with declared S-transformations) → RF-3 (QC on code rows + a wire
  contract that declares every step); add-backs one at a time.
- **The workflow rule:** *one posture, two beds* — sim truth bed first, then field; the
  field golden never diverges from the sim's vehicle again.

Further RF-N card files land in `docs/refound/`. That directory is the program's own
space and is **not** split or reorganised by the topic tree; everything below is the
record the program is re-founding **on**.

- [ALC-ARC](refound/ALC-ARC_reset_generation_2026-09-16.md) — closed, 2026-09-16:
  reset-gap fix; 82 focused tests pass and ALC-OFF products are byte-identical;
  longer accuracy campaigns deferred.
- [PBT-AUDIT](refound/PBT-AUDIT_translation_premise_2026-09-16.md) — closed,
  2026-09-16: blanket translation premise falsified; seven controlled witnesses;
  frame-transfer contract recommended, runtime unchanged.
- [ALC-STAT](refound/ALC-STAT_statistics_audit_2026-09-17.md) — closed,
  2026-09-17: statistical interpretation audited; 40 focused tests pass;
  correlation and nuisance bias prevent a calibrated 4-sigma claim; no tuning.
- [ALC-BAND](refound/ALC-BAND_window_scope_review_2026-09-17.md) — closed,
  2026-09-17: retain paired restarts; per-band dual-code levels need a separate
  statistical design; 44 focused tests pass, runtime unchanged.
- [ALC2](refound/ALC2_restart_policy_adoption_2026-09-19.md): closed,
  2026-09-19: the level-check restart policy refused on both beds and its
  branch left unmerged; translate + level check + consumer-sat trace adopted
  into the RF-0hv working bases; the pair re-based on post-ALC-ARC main.
- [PBH](refound/PBH_phase_bias_hold_2026-09-19.md): open, 2026-09-19: the wire's
  phase-bias movement is harmful error for a float consumer (sim 3.0 mm clock alone
  against 19.9 mm with the phase bias) and 0.2 % of the translated mass is a known
  frame move; the config-only dead-band ladder PASSED (zero dead band: field hours
  3 to 6 at 0.11 m with 167 sub-dm epochs, sim 0.06 m with 448); the emit-side
  phase-bias hold is now BUILT as `network.emit_pb_hold` (default OFF,
  identity-passing `gen=` wire contract) and all five PBH-1 bars PASS on both beds:
  the solve, the clock records and the code-bias records are byte-identical, every
  emitted phase bias is constant within its generation, the phase-bias channel's
  contribution to the within-arc error of the phase correction goes to exactly
  0.0000 m so the product's phase quality IS its clock quality (field 0.0693 to
  0.0274 m, sim 0.0199 to 0.0030 m), and the consumer reproduces the zero dead band
  to 0.3 mm, which measures the two as the same operation. ADOPTED into the
  RF-0hv working bases 2026-09-20 (baseline pair `runs/_rf0hv3_{field,sim}6`); no
  loader default, golden or ledger row changed.
- [AB1](refound/AB1_sh_consumer_product_2026-09-20.md): parked, 2026-09-20: add-back 1,
  the SH VTEC field as a consumer product; the in-solve field is loader-refused, and
  the trace-only prototype fails the frozen rule (a causal global product beats the
  dual-frequency consumer's own ionosphere for 2 epochs only, sim with truth): the
  consumer constraint is KEEP_OFF, the field is specified as a product (degree 6,
  25 degree mask, per-satellite bias terms) and not built.
- [AB2](refound/AB2_l5_third_frequency_2026-09-20.md): open (re-opened 2026-09-20,
  section 7: the L5 rows fit at the typical station, the damage reads as station-local,
  the station-scope arms T3s / T3sp are pre-registered); first verdict 2026-09-20: add-back 2,
  GPS L5 as a third frequency, KEEP_OFF on the field: the triple NETWORK damages the
  L1/L2 wire on real data (consumer hours 4 to 6 at 0.6 to 1.1 m against 0.11; a dual
  consumer on the triple wire reproduces it; the (L1, L5) IF-TD pairs and doubled
  post-fit refloats), the sim is neutral to better; an L5 phase-term model and a
  band-5 slip-tier calibration are owed.
- [GRS](refound/GRS_gap_reentry_per_signal_2026-09-20.md): open, 2026-09-20: build card,
  the gap re-entry response per SIGNAL; the satellite-level bookkeeping misses a
  primary-pair loss of lock while band 5 or a fallback signal tracks through, which
  is how L5 damages the L1/L2 wire (AB2 sections 10 to 12) and a defect of the dual
  posture too; default OFF rider of `gap_reentry_relevel`, gates and bars frozen.
- [CPA](refound/CPA_canonical_pair_admission_2026-09-20.md): closed, 2026-09-20, KEEP_OFF:
  canonical-pair admission, built exactly and falsified as the cause; the RINEX emulation
  that motivated it (203 to 627 epochs below a decimetre) had also moved the producer's
  first-epoch canonical-signal latch at 13 stations, and the gain belongs to that latch
  (every station on its priority-first signal: 601 epochs with no rule).
- [CSL](refound/CSL_canonical_signal_latch_2026-09-20.md): open (built and ARMED 2026-09-20, baseline
  pair `runs/_rf0hv5_*`; the mechanism read is open), 2026-09-20: the canonical-signal
  latch; the first-epoch satellite count prefers a civil signal, the dual field base latches
  L1C at 47 stations although 18 carry L1W; every station on its priority-first signal takes
  the consumer from 162 to 601 epochs below a decimetre (275 to 983 of 1440 in 12 h); mechanism
  unread; the decision rule is built behind a knob, bars frozen.
- [TBI](refound/TBI_three_band_ignition_2026-09-20.md): closed, 2026-09-20: the three-band
  ignition; the network closes band 5's code datum by asserting a zero inter-signal correction
  per satellite where the truth is metres (raw triple-code combination against the wire's:
  correlation 0.947 field, 0.962 sim); the misfit is pushed into band 2 and the WIRE carries the
  damage; band-5 code removed at the stations alone: 0.27 0.10 0.05 0.04 0.06 0.02 m, 535 epochs
  below a decimetre (314 with it, 603 dual); no config-only cure; L5 adoption bars frozen.
- [B5D](refound/B5D_band5_code_datum_2026-09-20.md): open (built 2026-09-20, NOT armed), build
  card, the band-5 code datum (`network.sat_gf_extra_band_free`, default OFF): the zero
  inter-signal assertion is gone, the wire carries 2.78 of the 2.97 m truth, the band-2
  contamination is cured; triple field 314 to 390 epochs below a decimetre, which fails the
  L5 adoption bars (dual pair 603, emulation 535); the emulation had also removed the band-5
  wide-lane detector pair, an unread lead.
- [L5P](refound/L5P_l5_phase_posture_2026-09-21.md): open (built 2026-09-21, NOT armed), the L5
  phase posture; two default-OFF knobs (`network.cycle_slip.mw_detect_pairs`,
  `network.extra_band_code_sigma_scale`); the gap is the band-5 CODE ROWS (scale 10: 597 epochs
  below a decimetre at WTZZ00DEU, every hour better than the dual pair), not the detector
  pairs (391); the scale fails the clock-wire bar and does not transfer across the 16 census
  sites; L5 not yet adopted and SEQUENCED, never abandoned (the author's directive; return
  plan R-1 to R-4 in section 4).
- [MSC](refound/MSC_multi_site_consumer_2026-09-21.md): open (MSC-1 result 2026-09-21), the
  multi-site consumer census; WTZZ00DEU is not representative (603 epochs below a decimetre
  against a median of 145 at 16 independent sites); by the frozen count rule no adoption of
  2026-09-20 holds across sites, the three together improve 13 of 16; the CSL support reading
  is refuted; the priority latch thinned the wire's C1W code biases and a consumer diverges
  on the dropouts; the census fails its own bar as a standing instrument at 16 sites.
- [RXP](refound/RXP_rinex3_blank_prn_2026-09-21.md): open, 2026-09-21: DEFECT, the RINEX 3 reader
  drops every satellite numbered below 10 in a file that pads the number with a blank (`G 7`);
  16 of 310 files of the day, four in the 59-station roster (CAS100ATA, GOPE00CZE, LCK400IND,
  LMMF00MTQ); found by the multi-site census; fix, gates and the re-baseline reading frozen.
- [AB3](refound/AB3_lanes_2026-09-20.md): open, 2026-09-20: add-back 3, lanes; the
  lane layer as built is loader-refused on the posture (it needs the retired
  invariant carry and the L5 extra-wide lane), so lanes are a new build on the float
  states; the readiness read AB3-0 PASSED: wide-lane integers exist on the FIELD under
  the float vehicle (concentration gap +0.283 against a bar of 0.10, 57 % of fractions
  within 0.15 cycle against 30 % uniform, bands still uniform); a lane design
  decision is owed, observatory first.

---

## Start here

| | |
| --- | --- |
| **New contributor?** Start at [onboarding/README.md](onboarding/README.md) | [1 Setup](onboarding/01_setup.md) · [2 Code tour](onboarding/02_code_tour.md) · [3 Worked example](onboarding/03_worked_example.md) · [4 Git workflow](onboarding/04_git_workflow.md) · [5 Debugging](onboarding/05_debugging.md) · [6 First tasks](onboarding/06_first_tasks.md) · `CONTRIBUTING.md` (repo root) |
| Architecture, capabilities, build targets, config sections (the pre-2026-09-19 root README) | [OVERVIEW.md](OVERVIEW.md) |
| Build, standing invariants, the standard armed set, the anti-regrowth trees | `CLAUDE.md` (repo root) |
| The goldens — the armed set made executable | `config/golden/` + `python scripts/make_golden_config.py --verify` |
| Every knob's verdict and regime | [CONFIG_promotion_ledger.md](CONFIG_promotion_ledger.md) — a **living register**, kept whole; `CONFIG_promotion_ledger.json` is its machine-read twin |
| What is owed, everywhere | [status/OPEN_ITEMS.md](status/OPEN_ITEMS.md) |
| Configuration reference | [CONFIG.md](CONFIG.md) |
| The real-data truth-graded harness | [test_suite.md](test_suite.md) |
| The older docs/ index (superseded by this page) | [README.md](README.md) |
| How this tree was built, and the proof it is verbatim | [REORG_PLAN.md](REORG_PLAN.md) · [REORG_MAPPING.md](REORG_MAPPING.md) |

---

## The rules

<a id="the-rules"></a>

1. **One card = one file** under `docs/cards/<topic>/<CARD-ID>.md`. A card is one
   pre-registered piece of work plus its results. Pre-register it as a file, do not
   append it to something else.
2. **Results are appended to the CARD's file only.** Not to a topic page, not to a
   monolith, never to `docs/archive/`.
3. **The INDEX line is updated twice**: when the card is pre-registered (status `open`),
   and when it closes (`closed` / `parked` / `superseded`, with the date).
4. **No non-archive file over ~60 KB.** When one approaches it, split it by topic —
   the card id stays, the file gains a suffix, and both are listed here. The one
   exemption is a **living register** that is appended to one row at a time and read
   by a tool at a fixed path — today that is `CONFIG_promotion_ledger.md` alone, named
   in `SIZE_EXEMPT` in `scripts/docs_index_check.py` with its reason.
5. **Every card file starts with the STATUS block** — `id`, `topic`, `status`,
   `summary`, `source`, plus the knobs / hashes / run dirs / date span. `status` is one
   of `open`, `closed`, `parked`, `superseded`, `reference`; the project's own verdict
   vocabulary (PROMOTED, KEEP_OFF, FALSIFIED, RECOMMEND_SCOPED …) belongs in the summary
   and the text.
6. **The monoliths are frozen.** `docs/archive/` is read-only and kept byte-for-byte as
   proof; the file at the old path is a routing stub, not a document. Never edit either.
7. **`python scripts/docs_index_check.py` must pass before a docs commit.** It checks
   reachability from this page, dangling links, the STATUS blocks, the 60 KB bar, and
   the archive banners.

Two corollaries worth stating, because both have already cost time here:

- **Never rewrite, shorten or reflow existing section text.** The docs are the
  scientific record; a result that was superseded is annotated, not deleted. The split
  that built this tree is byte-exact and `python scripts/docs_split.py --verify` proves
  it.
- **A card file carries verbatim history below its `<!-- BEGIN VERBATIM: … -->` marker.**
  Append new results *below* that text with a dated heading. The links inside the
  verbatim body were written against the old location and are deliberately not
  link-checked — do not "fix" them.

---

## The topic tree

Each topic has a short living page (what it is, the conventions and invariants that
govern it, the current posture) and a set of cards. "Standing documents" are the
pre-existing `docs/*.md` that were under 60 KB and were indexed rather than moved —
they are cited by path from `src/`, `include/`, `scripts/`, arm configs and the
author's memory files, so relocating them is a later pass.

### `harness` — [topics/harness.md](topics/harness.md)

| card | one line | status | last dated |
| --- | --- | --- | --- |
| [Cards A+B](cards/harness/CARD-AB-require-and-rx-clock-step.md) | the `require` hard-fail and the rx_clock_step event, plus the jump gate in metres | closed | 2026-09-10 |
| [HARNESS](cards/harness/HARNESS-REF.md) | what the harness is, the six injectable events, the files, the leg recipe, the grader, … | reference | 2026-09-10 |
| [Study 1](cards/harness/STUDY-1-erase-vs-refloat.md) | erase vs refloat — what actually costs the 0.45-cycle floor | closed | 2026-09-10 |
| [Study 2](cards/harness/STUDY-2-rx-clock-carry.md) | the receiver-clock carry under single events | closed | 2026-09-10 |
| [ORACLE PIN (stub)](cards/harness/ORACLE-PIN-stub.md) | the ORACLE PIN pre-registration stub — superseded by the card record in SPEC_oracle_pin | superseded | 2026-09-11 |
| [harness seeds](cards/harness/HARNESS-SEEDS.md) | seed hygiene and the fingerprint rule — every seed re-write and why the old snapshots s… | reference | 2026-09-12 |
| [FIELD harness](cards/harness/FIELD-HARNESS.md) | the 6 h field seed, the owed field legs for the sim-validated slip cards, and FP-1 / FP… | closed | 2026-09-13 |
| [DEC-field-vehicle](decisions/DEC-field-vehicle-posture.md) | the field vehicle posture decision and the closing legs | open | 2026-09-14 |

Standing documents (not split this pass):

- [SPEC_oracle_pin.md](SPEC_oracle_pin.md)

### `network-vehicle` — [topics/network-vehicle.md](topics/network-vehicle.md)

| card | one line | status | last dated |
| --- | --- | --- | --- |
| [Card 1b](cards/network-vehicle/CARD-1B-JOINT-WHITENING.md) | coupling-row JOINT whitening — PREMISE FALSIFIED, the toggle is a no-op, and what the f… | closed | 2026-07-10 |
| [datum wander](cards/network-vehicle/DATUM-WANDER.md) | the common level FOLLOWS the datum station — both remedies measured | closed | 2026-08-11 |
| [chain-pin audit](cards/network-vehicle/CHAIN-PIN-AUDIT.md) | rungs 2/6 re-attributed and the field's emission explained — the chain is COMPLETE | closed | 2026-08-12 |
| [R0 + 1(d)](cards/network-vehicle/PIVOT-CARRY-REREF-R0-1D.md) | `network.phase_anchor_carry_reref` — the carry re-reference closes the churn, with the … | closed | 2026-08-14 |
| [Card 1d](cards/network-vehicle/CARD-1D-COUPLING-CARRY.md) | the border time-update defect, verified four ways — magnitude in the T7 lineage, the ba… | closed | 2026-08-15 |
| [S-basis conformance](cards/network-vehicle/SBASIS-CONFORMANCE-F.md) | the per-deficiency conformance table, the implicit S-basis the code adopts per mode, th… | reference | 2026-08-15 |
| [S-basis contract](cards/network-vehicle/SBASIS-CONTRACT.md) | the realized basis, what the wire records mean, the realization-transfer contract, and … | reference | 2026-08-15 |
| [S-basis theory](cards/network-vehicle/SBASIS-THEORY-E.md) | the estimability spine, the datum-invariance theorem, the rank deficiencies, CC-R vs CC… | reference | 2026-08-15 |
| [chain-slant](cards/network-vehicle/CHAIN-SLANT-ODIJK.md) | Odijk rank arithmetic, the global-network chain caveat, the three self-consistent postu… | closed | 2026-08-15 |
| [pivot gauge defect](cards/network-vehicle/PIVOT-GAUGE-DEFECT.md) | the single-pivot phase-bias gauge defect — the causal proof, the field evidence, the tw… | closed | 2026-08-15 |
| [queue 1(b)](cards/network-vehicle/HANDOVER-STRANSFORM-SPEC.md) | the handover S-transform spec — the transform, its rank-1 specialization to a pin swap,… | closed | 2026-08-15 |
| [eq34 D4](cards/network-vehicle/EQ34-D4-S-TRANSFORM-FIX.md) | the eq34 D4 S-transform now re-bases `coupling[].s_prev` | closed | 2026-08-17 |
| [Cards 1f + 1g](cards/network-vehicle/CARD-1F-1G-TRUNCATED-ROW-GAUGE.md) | the TRUNCATED-ROW gauge defect and the reinit-source widening — the positive control, t… | closed | 2026-08-19 |
| [queue item 2](cards/network-vehicle/PIVOT-ELECTION-QUEUE2.md) | the datum-station election formalized as plumbing, and item 1(b)'s precondition priced | closed | 2026-08-21 |
| [wind-up sign](cards/network-vehicle/WINDUP-SIGN-DEFECT.md) | the carrier-phase wind-up sign defect — why nothing caught it, why the fix is both line… | closed | 2026-08-21 |
| [wind-up field](cards/network-vehicle/WINDUP-FIELD-LADDER.md) | the three-arm field ladder — the slope lands on 2, never on 0; the 5-station matrix; th… | closed | 2026-08-22 |
| [R2 sticky](cards/network-vehicle/PIVOT-R2-STICKY.md) | the sticky continuous target, composed with the pairing — churn dead, consumer 2×, and … | closed | 2026-08-31 |
| [READMIT](cards/network-vehicle/READMIT-EDGE.md) | the re-admission edge — `readmit_seed_horizon_s` promoted, the coupling-marginalize and… | closed | 2026-09-06 |
| [F3 field](cards/network-vehicle/F3-FIELD-ARM.md) | the field F3 arm — bed, arms, truth-free instruments, and the pre-registered prediction… | closed | 2026-09-09 |
| [F3 field (results)](cards/network-vehicle/F3-FIELD-ARM-RESULTS.md) | the field F3 arm's results, filled as they landed | closed | 2026-09-09 |
| [COLD-FB (iter 2–4)](cards/network-vehicle/COLD-FB-iterations-2-4.md) | the SATELLITE clause, the datum-invariant consumer census, and the new-golden arm — KEE… | parked | 2026-09-11 |
| [COLD-FB](cards/network-vehicle/COLD-FB-anchor-fallbacks.md) | the cold phase-datum anchor fallbacks (`phase_datum_candidacy_usable`) — pre-registrati… | parked | 2026-09-12 |
| [PIVOT DEPARTURE](cards/network-vehicle/PIVOT-DEPARTURE.md) | the consumer shock at a pivot handover is producer-internal carry free-fall — `scoped_h… | closed | 2026-09-12 |
| [D5 v5](cards/network-vehicle/D5-V5-carried-only.md) | `carry_invariant_innov_carried_only` on the field — KEEP_OFF, the same-realization pair… | parked | 2026-09-13 |
| [CE](cards/network-vehicle/CE-chain-era-bisect.md) | the CHAIN-ERA bisect on the leveling vehicle | open | 2026-09-14 |
| [CM](cards/network-vehicle/CM-code-model-consistency.md) | CODE-MODEL CONSISTENCY — eleven cold arms clear every estimation knob, and the station … | closed | 2026-09-14 |
| [CM2](cards/network-vehicle/CM2-leveling-vehicle.md) | the second defect — the CM knob arms on the LEVELING vehicle | open | 2026-09-14 |
| [PV](cards/network-vehicle/PV-vehicle-test.md) | the VEHICLE test — carry_invariant vs leveling, pre-registered | open | 2026-09-14 |
| [ZP](cards/network-vehicle/ZP-station-block-parameterization.md) | the station block's parameterization at ignition — pre-registered | open | — |

Standing documents (not split this pass):

- [SPEC_card4_invariant_carry.md](SPEC_card4_invariant_carry.md)
- [SPEC_carry_aware_covariance.md](SPEC_carry_aware_covariance.md)
- [SPEC_carry_vce_apply.md](SPEC_carry_vce_apply.md)
- [SPEC_d5_churn_limit_cycle.md](SPEC_d5_churn_limit_cycle.md)
- [SPEC_datum_graph_census.md](SPEC_datum_graph_census.md)
- [SPEC_eq34_reattach_hygiene.md](SPEC_eq34_reattach_hygiene.md)
- [SPEC_eq34_seed_round.md](SPEC_eq34_seed_round.md)
- [SPEC_family_gf_datum.md](SPEC_family_gf_datum.md)
- [SPEC_o2_carry_invariant_accumulator.md](SPEC_o2_carry_invariant_accumulator.md)
- [SPEC_pd_safe_dd_fold.md](SPEC_pd_safe_dd_fold.md)
- [network_ambiguity_resolution_design.md](network_ambiguity_resolution_design.md)

### `resume-seam` — [topics/resume-seam.md](topics/resume-seam.md)

| card | one line | status | last dated |
| --- | --- | --- | --- |
| [§18.10/§18.11](cards/resume-seam/RESUME-FRAME-RESTORE.md) | the seam is a FRAME loss — `resume_frame_restore`, built and graded | closed | 2026-09-03 |
| [§18.12](cards/resume-seam/RESUME-COUPLING-RESTORE.md) | the coupling-regressor restore verdict — the mechanism works, the wire is 10× better | closed | 2026-09-05 |
| [§18.13/§18.14](cards/resume-seam/RESUME-DENSE-BORDER-E2.md) | E2 and the dense border across the seam — pre-registered, then its verdicts | closed | 2026-09-05 |
| [RS-2](cards/resume-seam/RS-2-writer-frame-rebase.md) | the restored invariant DD branch is folded in the WRITER's linearization frame — Phase … | closed | 2026-09-12 |
| [RS-3 … RS-5](cards/resume-seam/RS-3-to-RS-5-datum-and-emit-layer.md) | both datums continue at the seam, the phase-bias level is the EMIT layer, and the seam … | closed | 2026-09-13 |
| [RS-6a](cards/resume-seam/RS-6a-station-layer-memories.md) | the station layer's own memory across the seam — the rx-CB carry and the T5 iono deviat… | closed | 2026-09-13 |
| [RS-7 … RS-9](cards/resume-seam/RS-7-to-RS-9-border-and-rxcb-frame.md) | the dense border restores exactly and is NOT the residual; the flat 26 % is the rx-CB i… | closed | 2026-09-13 |
| [RS-8 … RS-10b](cards/resume-seam/RS-8-to-RS-10b-latch-and-close-out.md) | the canonical-signal latch flips on 23 of 61 stations at a resume; the ZWD step is `zwd… | closed | 2026-09-14 |

Standing documents (not split this pass):

- [HANDOFF_seam_frame_restore_2026-09-03.md](HANDOFF_seam_frame_restore_2026-09-03.md)
- [SPEC_warm_resume_event_card.md](SPEC_warm_resume_event_card.md)

### `clock` — [topics/clock.md](topics/clock.md)

| card | one line | status | last dated |
| --- | --- | --- | --- |
| [Q3 ladder](cards/clock/Q3-FLOOR-GOVERNED-LADDER.md) | the floor-governed ladder — floors derived not tuned, the guard's denominator bisected,… | closed | 2026-08-10 |
| [Q3 results](cards/clock/Q3-ANCHOR-RESULTS.md) | G0–G3, the binding knee, the FC-1 shadow-hour table, and the relationship to the surrou… | closed | 2026-08-10 |
| [Q3 floor](cards/clock/Q3-PHYSICAL-FLOOR.md) | the physical floor measured — trap 5 resolved, the FC-1 gate retired, the ladder re-sco… | closed | 2026-08-11 |
| [field wander probe](cards/clock/FIELD-WANDER-PROBE.md) | the field wire wander is NOT receiver-code-driven; the a-priori CLK common-drift probe … | closed | 2026-08-11 |
| [Q3 design](cards/clock/Q3-ANCHOR-DESIGN.md) | the guarded temporal clock anchor — the measured basis, stage A/B design, the empirical… | closed | 2026-08-15 |
| [Q3 G4](cards/clock/Q3-G4-SIM-HALF.md) | G4's sim half executed — the anchor truth-graded on the 12 h quiet flagship, and the si… | closed | 2026-09-01 |
| [CS](cards/clock/CS-clock-step-signalling.md) | clock step signalling — 0 unsignalled steps, the 55.8 m consumer excursion gone | closed | 2026-09-14 |
| [Q3-F](cards/clock/Q3-F-self-contained-time-link.md) | the self-contained clock time-link on the leveling vehicle | open | 2026-09-14 |

Standing documents (not split this pass):

- [CLOCK_HADAMARD_CALIB_2026-07-14.md](CLOCK_HADAMARD_CALIB_2026-07-14.md)
- [SPEC_c1_clock_rate_validation.md](SPEC_c1_clock_rate_validation.md)
- [SPEC_clock_emit_rebase.md](SPEC_clock_emit_rebase.md)
- [SPEC_fast_clock_split_cadence.md](SPEC_fast_clock_split_cadence.md)

### `ionosphere` — [topics/ionosphere.md](topics/ionosphere.md)

| card | one line | status | last dated |
| --- | --- | --- | --- |
| [EXT-CB defect 2](cards/ionosphere/LV-EXTCB-DEFECT2.md) | arm 30 and the 303-station EXT-CB arm — DEFECT 2 is largely a station-support artifact | closed | 2026-08-05 |
| [LV correction](cards/ionosphere/LV-CORRECTION-AND-12H.md) | what LV-2 actually does and what LV-1 is really for — and the 12 h verdict that made th… | closed | 2026-08-05 |
| [LV item 1](cards/ionosphere/LV-ITEM1-FALSIFIED.md) | roadmap item 1 falsified at stage 1 — the cost attributes to LV-4 alone, plus two invar… | closed | 2026-08-05 |
| [LV mask study](cards/ionosphere/LV-MASK-STUDY.md) | the mask study, end to end — the trade is regime-dependent, it flips sign between 61 an… | closed | 2026-08-05 |
| [LV residual](cards/ionosphere/LV-RESIDUAL-EXTRAPOLATION.md) | the residual is not an extrapolation artifact, and `rx_cb_carry_q` is not simply too ti… | closed | 2026-08-05 |
| [LV tracking pin](cards/ionosphere/LV-TRACKING-PIN.md) | the tracking-pin fix, tested and FALSIFIED | closed | 2026-08-05 |
| [LV-4](cards/ionosphere/LV-4-FRAME-INVARIANT-SCREEN.md) | the carry screen made frame-invariant — fixed, and it exposed a second defect | closed | 2026-08-05 |
| [LV-5](cards/ionosphere/LV-5-MU-WITHIN-EPOCH.md) | μ computed within the epoch, with the review addendum's requirements | closed | 2026-08-05 |
| [LV roadmap](cards/ionosphere/LV-ROADMAP.md) | the continuing session's roadmap, and its refresh at hand-back | open | 2026-08-07 |
| [LV impl](cards/ionosphere/LV-IMPL-RECORD.md) | the 2026-08-05 implementation record, and the measured outcomes that corrected the mech… | closed | 2026-08-12 |
| [elev-cutoff A/B](cards/ionosphere/ELEV-CUTOFF-AB.md) | the elevation-cutoff A/B WITH TRUTH — the field verdict INVERTS on a coherent field | closed | 2026-08-12 |
| [iono-field agreement](cards/ionosphere/IONO-FIELD-AGREEMENT.md) | cross-station iono-field agreement — coherent where mapped, BROKEN at low–high, plus th… | closed | 2026-08-12 |
| [EXT-CB arms 1–36](cards/ionosphere/LV-EXTCB-CAMPAIGN.md) | the EXT-CB campaign record, arms 1–36 — root cause, the fix, and the L2-ESCAPE | closed | 2026-08-15 |
| [LV](cards/ionosphere/LV-REF.md) | the ionosphere-level initiative's own reference page — knob ledger, verdict ledger, sec… | reference | 2026-08-21 |
| [AMB-IONO](cards/ionosphere/AMB-IONO-SPLIT.md) | why the network producer never reaches per-band integers on the quiet sim bed — the fiv… | open | 2026-09-04 |
| [F3 (pre-reg)](cards/ionosphere/AMB-IONO-F3-PREDICTIONS.md) | the pre-registered predictions for the F3 station block, and the round-2 / round-4 resu… | closed | 2026-09-04 |
| [F3 24 h](cards/ionosphere/AMB-IONO-F3-24H.md) | the 24 h F3 arm — the settled number is NOT met, and the reason is an EDGE, not the mec… | closed | 2026-09-05 |
| [0910 afternoon](cards/ionosphere/AMB-IONO-0910-AFTERNOON.md) | the level-memory and T4-persistence ladders, the wire wander, and the continuity card | closed | 2026-09-10 |
| [0910 morning](cards/ionosphere/AMB-IONO-0910-MORNING.md) | the same-build control, three truth censuses, two falsified cards, and the field's phas… | closed | 2026-09-10 |
| [0910 second half](cards/ionosphere/AMB-IONO-0910-SECOND-HALF.md) | the slipped-bed probe ledger, the continuity closure, the false-alarm isolation, and th… | closed | 2026-09-10 |
| [AMB-IONO open](cards/ionosphere/AMB-IONO-OPEN-QUESTIONS.md) | the open-question register — re-cut round 2, with the round-4 closures and items 15–21 | open | 2026-09-10 |
| [LV-SEED](cards/ionosphere/LV-SEED-per-los-level.md) | where the remaining per-LOS level is born — R1–R9 pre-registered reads | open | 2026-09-14 |
| [LV-1](cards/ionosphere/LV-1-BIAS-FREE-SEEDING.md) | bias-free per-LOS iono seeding — the seed is a FRAME, not an authority | closed | — |
| [LV-2](cards/ionosphere/LV-2-RXCB-DEMEAN.md) | rx-CB carry common-mode de-mean — works as designed, but the de-mean is NOT frame-invar… | closed | — |
| [LV-3](cards/ionosphere/LV-3-LEVEL-AUTHORITY.md) | the level authority, made explicit — mostly documentation, one optional knob | closed | — |

Standing documents (not split this pass):

- [SPEC_deweight_release_hygiene.md](SPEC_deweight_release_hygiene.md)
- [SPEC_iono2_mechB_v3.md](SPEC_iono2_mechB_v3.md)
- [SPEC_iono_carry_8b.md](SPEC_iono_carry_8b.md)
- [SPEC_level_authority_internal.md](SPEC_level_authority_internal.md)
- [network_global_sh_iono_design_2026-06-29.md](network_global_sh_iono_design_2026-06-29.md)

### `code-bias` — [topics/code-bias.md](topics/code-bias.md)

| card | one line | status | last dated |
| --- | --- | --- | --- |
| [S2H](cards/code-bias/S2H-HARVEST.md) | the S-consistent rx-CB harvest — built, validated through 3 h, and its 12 h tracking ve… | closed | 2026-08-21 |

Standing documents (not split this pass):

- [SPEC_beidou_w6_datum.md](SPEC_beidou_w6_datum.md)
- [SPEC_cb_admission_card.md](SPEC_cb_admission_card.md)
- [SPEC_cb_carry_physical_q.md](SPEC_cb_carry_physical_q.md)
- [SPEC_rx_cb_carry.md](SPEC_rx_cb_carry.md)
- [glonass_enablement_plan.md](glonass_enablement_plan.md)

### `slip` — [topics/slip.md](topics/slip.md)

| card | one line | status | last dated |
| --- | --- | --- | --- |
| [slip-QC impl](cards/slip/SLIPQC-IMPL-2026-07-21.md) | the 2026-07-21 implementation record — what landed, the five findings that contradicted… | closed | 2026-08-12 |
| [D5-A](cards/slip/D5A-RETAP-ATTRIBUTION.md) | D5 re-tap attribution as built, the cross-experiment protocol, and the 2026-07-22 valid… | closed | 2026-08-15 |
| [slip-QC follow-ups](cards/slip/SLIPQC-FOLLOWUPS.md) | the 2026-07-22 follow-up cards D5-A / W / OP / H, and why the D5 lead outranked everyth… | closed | 2026-08-15 |
| [lane slip response](cards/slip/LANE-SLIP-RESPONSE.md) | the headline decomposition — the lane-integer slip cost is 100 % response-inflicted; th… | closed | 2026-08-22 |
| [T4 lane restore](cards/slip/T4-LANE-RESTORE-SPEC.md) | pend-and-restore v1 (value-only), `network.cycle_slip.relevel_t4_lane_restore` — spec o… | open | 2026-08-23 |
| [rung 3](cards/slip/LANE-SNAP-RUNG3.md) | the lane-preserving snap — v1/v2 falsified, the SEED SITE deleted as an exact solve no-… | open | 2026-08-23 |
| [wind-up census](cards/slip/WINDUP-FRACTIONAL-CENSUS.md) | the fractional-mode census — the two continuity horizons are INVERTED, `trace_windup_ce… | open | 2026-08-23 |
| [IF-TD ref gate](cards/slip/IFTD-REF-GATE.md) | the IF-TD reference gate is 100 % false on the sim bed, and the fix was already in the … | closed | 2026-09-08 |
| [Card G response](cards/slip/CARD-G-GAP-REENTRY-RELEVEL.md) | `gap_reentry_relevel` — round 1 was inert on the live-state walk, round 2's verdict, an… | closed | 2026-09-09 |
| [IF-TD ref gate (v2)](cards/slip/IFTD-REF-GATE-FOLLOWUP.md) | abstain while cold, normalize the bar by the reference's elevation, and close the gatin… | closed | 2026-09-09 |
| [slip-QC](cards/slip/SLIPQC-REF.md) | the slip-QC hardening spec — evidence base, the binding ground rules, cards R / L / E /… | reference | 2026-09-09 |
| [Card ORACLE](cards/slip/CARD-ORACLE-RELEVEL.md) | `oracle_relevel`, the sim-only CEILING of a level-preserving re-level — the mechanism i… | closed | 2026-09-10 |
| [Study 3](cards/slip/STUDY-3-band-scoped-responses.md) | band-scoped slip responses, the production knobs — the combined arm FAILS at storm scale | closed | 2026-09-10 |
| [IF-TD pairs](cards/slip/IFTD-PAIRS.md) | the IF-TD detector watched ONE lane per satellite — `if_phase_td_pairs` | closed | 2026-09-11 |
| [Study 4](cards/slip/STUDY-4-slip-dose.md) | the slip DOSE — the anchor-slip handover cure, validated on the harness and on the stor… | closed | 2026-09-11 |
| [Study 5](cards/slip/STUDY-5-detector-coverage-l5.md) | detector coverage — the unflagged GPS L5 slip and `mw_evidence_refloat` | closed | 2026-09-11 |
| [Study 6](cards/slip/STUDY-6-common-band-scope.md) | the COMMON-BAND scope — free only the band the evidence names | closed | 2026-09-11 |
| [Study 7](cards/slip/STUDY-7-common-band-narrowing.md) | the common-band scope's 3 cm is ONE wrong narrowing at 4.99 h | closed | 2026-09-11 |
| [REF-ARC](cards/slip/REF-ARC-CLAUSE.md) | a chain's reference-sat pair is UNTESTED and cannot exonerate — `common_band_ref_arc`, … | closed | 2026-09-13 |
| [REF-FAN](cards/slip/REF-FAN.md) | the reference-slip fan-out under `if_phase_td_pairs = all` — RECOMMEND_SCOPED, the fiel… | open | 2026-09-13 |
| [RULE F2](cards/slip/IFTD-F2-lli-shadow.md) | the LLI shadow — a lane that did not see the flagged epoch is not silent | closed | — |
| [RULE F3](cards/slip/IFTD-F3-lli-epoch.md) | the LLI-EPOCH verdict, and the ANCHOR CLAUSE inside F3 | closed | — |

Standing documents (not split this pass):

- [SPEC_ewl_wl_cascade_card.md](SPEC_ewl_wl_cascade_card.md)
- [SPEC_lane_carry_card.md](SPEC_lane_carry_card.md)
- [SPEC_lane_spectrometer.md](SPEC_lane_spectrometer.md)
- [SPEC_mw_wl_card.md](SPEC_mw_wl_card.md)
- [SPEC_noncausal_slip_bridge.md](SPEC_noncausal_slip_bridge.md)
- [SPEC_sd_ewl_prereduce.md](SPEC_sd_ewl_prereduce.md)
- [SPEC_slip_relevel_walk.md](SPEC_slip_relevel_walk.md)
- [SPEC_uwl_card.md](SPEC_uwl_card.md)
- [slip_comparison_report.md](slip_comparison_report.md)
- [turbo_edit_lli_bridge_design.md](turbo_edit_lli_bridge_design.md)

### `raim-qc` — [topics/raim-qc.md](topics/raim-qc.md)

| card | one line | status | last dated |
| --- | --- | --- | --- |
| [DOY-129 defects](cards/raim-qc/DAY129-DEFECTS-ABC.md) | defects A/B/C from the instrumented leg, the five wiring facts, and the measured policy… | closed | 2026-07-28 |
| [DOY-129 phases](cards/raim-qc/DAY129-PHASES.md) | phases 0–2: the closed-loop replay, the trigger instrumentation, and the gated guards | closed | 2026-07-28 |
| [INC-4](cards/raim-qc/MEAS-ADMISSION-INC4-REENTRY.md) | the arc RE-ENTRY vet — there were no mid-arc onsets; `adm_revet_hold` erases the event | closed | 2026-08-02 |
| [Rider A](cards/raim-qc/MDB-RIDER-A.md) | Baarda reliability numbers (MDB) on the network path — `network.mdb` | closed | 2026-08-04 |
| [Rider B](cards/raim-qc/BLOCK-BAARDA-RIDER-B.md) | BlockFormal — the (station, sat) LOS block outlier census, `network.block_baarda` | closed | 2026-08-04 |
| [meas admission](cards/raim-qc/MEAS-ADMISSION.md) | the product-referenced admission screen — the fault class no differential screen can se… | closed | 2026-08-12 |
| [meas admission impl](cards/raim-qc/MEAS-ADMISSION-IMPL.md) | the 2026-08-01 implementation record — every deviation from the spec, the gates as run,… | closed | 2026-08-12 |
| [DOY-129 anatomy](cards/raim-qc/DAY129-FRAME-ANATOMY.md) | the answered questions — the pinned pivot's code bias, the a₀₀ collapse anatomy, the qu… | closed | 2026-08-15 |
| [Rider C](cards/raim-qc/CONGRUENCE-RIDER-C.md) | the station congruence screen, `network.congruence_screen` | closed | 2026-08-15 |
| [T3 null](cards/raim-qc/T3-NULL-DERIVATION.md) | the statistic as actually computed, the invariance theorem, the falsified redundancy st… | closed | 2026-08-15 |
| [global σ̂₀ rider](cards/raim-qc/GLOBAL-SIGMA0-RIDER.md) | `network.prior_residual_global_sigma0` — three sums, one ratio, nothing acted on | closed | 2026-08-15 |
| [prires census](cards/raim-qc/PRIOR-RESIDUAL-CENSUS.md) | the prior-residual census — the binding precedent, INC-0..3, the wiring sites, the buil… | closed | 2026-08-15 |
| [redundancy census](cards/raim-qc/REDUNDANCY-CENSUS.md) | the row-redundancy and unobservable-local censuses — design, cost model, wiring, build … | closed | 2026-08-15 |
| [A4 withhold](cards/raim-qc/WITHHOLD-ERA-RELEASE.md) | the withhold-era release policy — the defect named exactly, the v2 design, the pre-regi… | closed | 2026-08-21 |
| [T3 field](cards/raim-qc/T3-FIELD-AND-COLLATERALS.md) | the T3 class and its two collateral defects, the field check, the detection-power gate,… | closed | 2026-08-21 |
| [withhold cliff](cards/raim-qc/WITHHOLD-CLIFF-RETIREMENT.md) | the retirement census — every G3 number was on a retired binary; the cliff does not rep… | closed | 2026-08-23 |
| [RNEG](cards/raim-qc/RNEG-REPARTITION.md) | the RNEG re-partition — what a `post_var > fold_var` row is, and the numerator card it … | closed | 2026-09-09 |
| [GL](cards/raim-qc/GL-gross-los-screen.md) | the gross line-of-sight screen | open | 2026-09-14 |
| [GL-2 + R8](cards/raim-qc/GL-2-whole-satellite-standdown.md) | the whole-satellite stand-down, and R8 — the bulk drift's channel | open | 2026-09-14 |
| [DEC-day129-gate-policy](decisions/DEC-day129-gate-policy.md) | the gate-policy ruling — DEGRADE, DO NOT WITHHOLD; the statistic; the CB-excursion resu… | open | — |
| [T3 mechanism](cards/raim-qc/T3-IONO-MECHANISM.md) | the per-row census, the falsification ledger, and the MECHANISM FOUND — per-LOS ionosph… | closed | — |

Standing documents (not split this pass):

- [SPEC_flicker_gap_integrity.md](SPEC_flicker_gap_integrity.md)
- [SPEC_prefit_census_observability.md](SPEC_prefit_census_observability.md)
- [SPEC_raim_pool_rehost.md](SPEC_raim_pool_rehost.md)

### `wire-contract` — [topics/wire-contract.md](topics/wire-contract.md)

| card | one line | status | last dated |
| --- | --- | --- | --- |
| [LV-3 contract](cards/wire-contract/LEVEL-CONTRACT.md) | the normative contract for the ionosphere LEVEL and receiver-DCB frame of the emitted p… | closed | 2026-08-15 |
| [level health](cards/wire-contract/LEVEL-HEALTH-CHANNEL.md) | `network.emit_level_health` — the contract gap, the one-channel three-class mechanism, … | open | 2026-08-23 |

Standing documents (not split this pass):

- [SPEC_product_cbpin_contract.md](SPEC_product_cbpin_contract.md)

### `consumer` — [topics/consumer.md](topics/consumer.md)

| card | one line | status | last dated |
| --- | --- | --- | --- |
| [CC](cards/consumer/CC-declared-step-carry.md) | the continuity half — the consumer's declared-step carry and warm start, landed default… | closed | 2026-09-14 |
| [CI](cards/consumer/CI-consumer-instrument.md) | the closed-loop consumer as an INSTRUMENT — what the consumer bars have actually been r… | open | 2026-09-14 |
| [CI census](cards/consumer/CI-census-wire-vs-code.md) | the wire-vs-CODE consistency census and the CODE-RESIDUAL per-LOS attribution | closed | 2026-09-14 |
| [consumer AR](cards/consumer/CONSUMER-AR-BLUNDER.md) | the consumer AR blunder diagnosed and fixed — the innovation screen is the gate that ca… | closed | — |

### `sim-bed` — [topics/sim-bed.md](topics/sim-bed.md)

| card | one line | status | last dated |
| --- | --- | --- | --- |
| [G-A](cards/sim-bed/SIMNET-GA-NULL-TEST.md) | the NULL test — the first absolute grade, measured 2026-08-11 | closed | 2026-08-11 |
| [G-A 50-sta](cards/sim-bed/SIMNET-50STA-QC-TIERS.md) | the 50-station global arm — the vf trend, the QC tiers, and the MW null space | closed | 2026-08-11 |
| [W6+SH arm](cards/sim-bed/SIMNET-W6SH-ARM.md) | the W6+SH arm and its minimum-n ladder — the two-band wall is n-invariant, posture is t… | closed | 2026-08-12 |
| [W6+SH brief](cards/sim-bed/SIMNET-W6SH-BRIEF.md) | the design brief for the W6+SH synthetic arm — superseded by the arm itself | superseded | 2026-08-12 |
| [G-B](cards/sim-bed/SIMNET-GB-CLOCK-STEP.md) | event injection, satellite-clock step — first class only | closed | 2026-08-15 |
| [SIMNET](cards/sim-bed/SIMNET-REF.md) | the truth-graded 3/5/50-station fixture — architecture, truth channels, the estimable-f… | reference | 2026-08-15 |
| [injection bench](cards/sim-bed/SIM-INJECTION-BENCH.md) | the G-B event-injection bench — the ambiguity N-regime, the pre-flight, the arms, and t… | closed | 2026-08-15 |
| [60-sta campaign](cards/sim-bed/SIMNET-60STA-CAMPAIGN.md) | the first long / multi-GNSS / realistic-slip grade — full record in the slip re-level w… | closed | 2026-08-20 |
| [dashboard](cards/sim-bed/SIMNET-DASHBOARD.md) | one-command HTML report over a graded simnet run | closed | 2026-09-01 |
| [stability](cards/sim-bed/STABILITY-CAMPAIGN.md) | the settled state must stop moving — targets 1–4, the eclipse-exclusion valve, sticky p… | closed | 2026-09-02 |
| [DD-REF](cards/sim-bed/DD-TRACE-REF-SAT.md) | the post-eclipse network-wide DD episode was the INSTRUMENT'S reference satellite, and … | closed | 2026-09-06 |
| [slant + triple](cards/sim-bed/SIMNET-SLANT-TRIPLE.md) | the third band and the slant-ionosphere posture — the ambiguity-integer grade, the sett… | closed | 2026-09-09 |
| [ZWD sink](cards/sim-bed/ZWD-SINK-CENSUS.md) | the ZWD sink census, the slipped-bed golden FAIL, and where the integer information is | closed | 2026-09-10 |

### `perf` — [topics/perf.md](topics/perf.md)

_No split cards yet — the topic page and the standing documents below are the whole of it._

Standing documents (not split this pass):

- [SPEC_carry_factored_pz_C2.md](SPEC_carry_factored_pz_C2.md)
- [SPEC_dev_tier_perf_cards.md](SPEC_dev_tier_perf_cards.md)
- [SPEC_inc8b_election_split.md](SPEC_inc8b_election_split.md)
- [network_decomposition_plan.md](network_decomposition_plan.md)

### `realtime` — [topics/realtime.md](topics/realtime.md)

_No split cards yet — the topic page and the standing documents below are the whole of it._

Standing documents (not split this pass):

- [rt_seam_decomposition.md](rt_seam_decomposition.md)
- [rt_service_conventions.md](rt_service_conventions.md)
- [rt_service_operations.md](rt_service_operations.md)

### `config-ledger` — [topics/config-ledger.md](topics/config-ledger.md)

_No split cards yet — the topic page and the standing documents below are the whole of it._

Standing documents (not split this pass):

- [CONFIG_promotion_ledger.md](CONFIG_promotion_ledger.md)

### `queues` — [topics/queues.md](topics/queues.md)

| card | one line | status | last dated |
| --- | --- | --- | --- |
| [QPR blocked](cards/queues/QPR-BLOCKED-AND-CLOSED.md) | blocked on root cause, leave-alone (falsified or decided), and the queue's maintenance … | open | 2026-08-14 |
| [QPR](cards/queues/QPR-00-CHARTER.md) | the charter — why this queue exists, how an item enters and leaves it, and the rank ord… | open | 2026-08-21 |
| [QPR 0–3](cards/queues/QPR-ITEMS-0-3.md) | items 0–3: the pin bridge, the S-transform re-reference family, datum-station election … | open | 2026-08-21 |
| [QPR 4](cards/queues/QPR-ITEM-4-T3-NULLS.md) | item 4: T3-class test statistics — correct nulls, not recalibrated constants | open | 2026-08-21 |
| [QPR 5–8](cards/queues/QPR-ITEMS-5-8.md) | items 5–8: the level contract, the half-epoch back-off ritual, the SH health-gate relea… | open | 2026-08-21 |

Standing documents (not split this pass):

- [HANDOFF_edge_response_queue.md](HANDOFF_edge_response_queue.md)

### `build-infra` — [topics/build-infra.md](topics/build-infra.md)

_No split cards yet — the topic page and the standing documents below are the whole of it._

Standing documents (not split this pass):

- [CONFIG.md](CONFIG.md)
- [README.md](README.md)
- [SPEC_latex_doc_refresh_2026-07-17.md](SPEC_latex_doc_refresh_2026-07-17.md)
- [SPEC_rinex_zero_sentinel.md](SPEC_rinex_zero_sentinel.md)
- [aiding_source_abstraction_design.md](aiding_source_abstraction_design.md)
- [datum_transformation_reference_2026-08-07.md](datum_transformation_reference_2026-08-07.md)
- [test_suite.md](test_suite.md)

### `history` — [topics/history.md](topics/history.md)

_No split cards yet — the topic page and the standing documents below are the whole of it._

Standing documents (not split this pass):

- [RUNQUEUE_pdfix_reask.md](RUNQUEUE_pdfix_reask.md)
---

## Decisions

Posture rulings whose text is self-contained live in `docs/decisions/`; every other
posture call stays in the verdict section of the card that reached it.

- [decisions/DEC-field-vehicle-posture.md](decisions/DEC-field-vehicle-posture.md) — the
  field vehicle posture decision and its closing legs (now superseded in direction by
  the re-founding above, kept as the decision that led to it).
- [decisions/DEC-day129-gate-policy.md](decisions/DEC-day129-gate-policy.md) — the
  DOY-129 gate-policy ruling: degrade, do not withhold.

---

## Status

- [status/OPEN_ITEMS.md](status/OPEN_ITEMS.md) — every owed / open item the corpus
  names, each with the card and section that is its evidence.

---

## The frozen monoliths

Eighteen files over 60 KB were split on 2026-09-14. The original of each is kept
byte-for-byte in `docs/archive/` and is never appended to; the old path now holds a
routing stub whose table resolves any `§N.N` citation in one hop, which is what keeps
the several hundred `// … (SPEC_x §N.N): …` comments in `src/` and `include/` working.

**A nineteenth file over 60 KB was deliberately NOT split:**
[CONFIG_promotion_ledger.md](CONFIG_promotion_ledger.md). It is a **living register** —
one row per knob, appended to in place by the `scripts/_*_ledger_oneoff.py` family and
by every future promotion, with `CONFIG_promotion_ledger.json` as its machine-read
twin. It stays whole at its path and carries a named size exemption in
`scripts/docs_index_check.py` (`SIZE_EXEMPT`). See
[topics/config-ledger.md](topics/config-ledger.md).

| the monolith (frozen) | routing stub at the old path | cards under |
| --- | --- | --- |
| [archive/QUEUE_principled_replacements.md](archive/QUEUE_principled_replacements.md) | [QUEUE_principled_replacements.md](QUEUE_principled_replacements.md) | `queues` |
| [archive/SPEC_amb_iono_split_overcount.md](archive/SPEC_amb_iono_split_overcount.md) | [SPEC_amb_iono_split_overcount.md](SPEC_amb_iono_split_overcount.md) | `ionosphere`, `network-vehicle`, `sim-bed`, `slip` |
| [archive/SPEC_card1_coupling_carry.md](archive/SPEC_card1_coupling_carry.md) | [SPEC_card1_coupling_carry.md](SPEC_card1_coupling_carry.md) | `network-vehicle` |
| [archive/SPEC_clock_temporal_anchor.md](archive/SPEC_clock_temporal_anchor.md) | [SPEC_clock_temporal_anchor.md](SPEC_clock_temporal_anchor.md) | `clock`, `consumer` |
| [archive/SPEC_day129_frame_guards.md](archive/SPEC_day129_frame_guards.md) | [SPEC_day129_frame_guards.md](SPEC_day129_frame_guards.md) | `raim-qc` |
| [archive/SPEC_event_harness.md](archive/SPEC_event_harness.md) | [SPEC_event_harness.md](SPEC_event_harness.md) | `clock`, `consumer`, `harness`, `ionosphere`, `network-vehicle`, `raim-qc`, `resume-seam`, `slip` |
| [archive/SPEC_lane_slip_response.md](archive/SPEC_lane_slip_response.md) | [SPEC_lane_slip_response.md](SPEC_lane_slip_response.md) | `slip` |
| [archive/SPEC_level_information_budget.md](archive/SPEC_level_information_budget.md) | [SPEC_level_information_budget.md](SPEC_level_information_budget.md) | `code-bias`, `ionosphere`, `wire-contract` |
| [archive/SPEC_measurement_admission.md](archive/SPEC_measurement_admission.md) | [SPEC_measurement_admission.md](SPEC_measurement_admission.md) | `raim-qc` |
| [archive/SPEC_phase_windup_sign.md](archive/SPEC_phase_windup_sign.md) | [SPEC_phase_windup_sign.md](SPEC_phase_windup_sign.md) | `network-vehicle`, `slip` |
| [archive/SPEC_pivot_visibility_gauge_defect.md](archive/SPEC_pivot_visibility_gauge_defect.md) | [SPEC_pivot_visibility_gauge_defect.md](SPEC_pivot_visibility_gauge_defect.md) | `network-vehicle` |
| [archive/SPEC_prior_residual_census.md](archive/SPEC_prior_residual_census.md) | [SPEC_prior_residual_census.md](SPEC_prior_residual_census.md) | `raim-qc` |
| [archive/SPEC_redundancy_census.md](archive/SPEC_redundancy_census.md) | [SPEC_redundancy_census.md](SPEC_redundancy_census.md) | `raim-qc` |
| [archive/SPEC_sbasis_realization_contract.md](archive/SPEC_sbasis_realization_contract.md) | [SPEC_sbasis_realization_contract.md](SPEC_sbasis_realization_contract.md) | `network-vehicle` |
| [archive/SPEC_slip_qc_hardening.md](archive/SPEC_slip_qc_hardening.md) | [SPEC_slip_qc_hardening.md](SPEC_slip_qc_hardening.md) | `slip` |
| [archive/SPEC_synthetic_minimum_network.md](archive/SPEC_synthetic_minimum_network.md) | [SPEC_synthetic_minimum_network.md](SPEC_synthetic_minimum_network.md) | `clock`, `ionosphere`, `network-vehicle`, `resume-seam`, `sim-bed` |
| [archive/SPEC_t3_null_derivation.md](archive/SPEC_t3_null_derivation.md) | [SPEC_t3_null_derivation.md](SPEC_t3_null_derivation.md) | `raim-qc` |
| [archive/SPEC_withhold_era_release.md](archive/SPEC_withhold_era_release.md) | [SPEC_withhold_era_release.md](SPEC_withhold_era_release.md) | `raim-qc`, `wire-contract` |
---

## Older material

- [topics/history.md](topics/history.md) — the 2026-08-15 attic (43 handoffs, session
  reports and superseded specs, kept for provenance) and the closed campaign records.
- [report/README.md](report/README.md) — the LaTeX deliverable under `docs/report/`.
