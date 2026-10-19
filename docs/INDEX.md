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
  to GPS-only dual-frequency, no lanes, no F3, no seams (Galileo added back on the goldens 2026-10-13, card
  [GAD](refound/GAD_galileo_adoption_2026-10-13.md)).
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

The program's cards, one entry per card in pre-registration order from ALC-ARC
(2026-09-16) on, are listed in **[refound/CARDS.md](refound/CARDS.md)**, moved there
verbatim on 2026-09-25 under the 60 KB bar. A program card's index line under rule 3
is its entry there.

---

## Start here

| | |
| --- | --- |
| **New contributor?** Start at [onboarding/README.md](onboarding/README.md) | [1 Setup](onboarding/01_setup.md) · [2 Code tour](onboarding/02_code_tour.md) · [3 Worked example](onboarding/03_worked_example.md) · [4 Git workflow](onboarding/04_git_workflow.md) · [5 Debugging](onboarding/05_debugging.md) · [6 First tasks](onboarding/06_first_tasks.md) · [7 Network tour](onboarding/07_network_tour.md) · [8 Adding a network parameter](onboarding/08_adding_a_network_parameter.md) · `CONTRIBUTING.md` (repo root) |
| Architecture, capabilities, build targets, config sections (the pre-2026-09-19 root README) | [OVERVIEW.md](OVERVIEW.md) |
| Build, standing invariants, the standard armed set, the anti-regrowth trees | `CLAUDE.md` (repo root) |
| The goldens — the armed set made executable | `config/golden/` + `python scripts/make_golden_config.py --verify` |
| Every config key's verdict and regime | [CONFIG_promotion_ledger.md](CONFIG_promotion_ledger.md), the view generated from `CONFIG_promotion_ledger.json` (the ledger itself) |
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
   exemption is the generated ledger view `CONFIG_promotion_ledger.md`, named in
   `SIZE_EXEMPT` in `scripts/docs_index_check.py` with its reason.
5. **Every card file starts with the STATUS block** — `id`, `topic`, `status`,
   `summary`, `source`, plus the knobs / hashes / run dirs / date span. `status` is one
   of `open`, `closed`, `parked`, `superseded`, `reference`; the project's own verdict
   vocabulary (PROMOTED, KEEP_OFF, FALSIFIED, RECOMMEND_SCOPED …) belongs in the summary
   and the text. Since 2026-10-07 an **Outcome** paragraph follows the block: the
   question, the decisive result, the verdict (missed bars included) and the current
   standing with a link to any later card, in about 90 words. The summary is one sentence.
   Both are revised when a result lands; the text below them never is.
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
| [Cards A+B](cards/harness/CARD-AB-require-and-rx-clock-step.md) | require hard-fail fixed (error line, exit 1); rx_clock_step built; rx-clock carry KEEP_OFF | closed | 2026-09-10 |
| [HARNESS](cards/harness/HARNESS-REF.md) | event harness reference: injectable events, leg recipe, event-minus-null grader, gaps | reference | 2026-09-10 |
| [Study 1](cards/harness/STUDY-1-erase-vs-refloat.md) | erase-path premise FALSIFIED: band scope buys the 0.45-cycle floor (0.445 to 0.050 scoped) | closed | 2026-09-10 |
| [Study 2](cards/harness/STUDY-2-rx-clock-carry.md) | rx-clock carry INERT on slip, erase and handover (at the null floor); KEEP_OFF (Study 2b) | closed | 2026-09-10 |
| [ORACLE PIN (stub)](cards/harness/ORACLE-PIN-stub.md) | oracle pin stub, superseded by SPEC_oracle_pin: a complete pin cuts DD-invariant iono 10x | superseded | 2026-09-11 |
| [harness seeds](cards/harness/HARNESS-SEEDS.md) | seed hygiene: legs arm zwd_seam_reseed; fingerprinted promotions force _v2, then _v3 seeds | reference | 2026-09-12 |
| [FIELD harness](cards/harness/FIELD-HARNESS.md) | field harness: evidence refloat FIELD PIN; scope + pairs all stay exceptions, twin FAILS | closed | 2026-09-13 |
| [DEC-field-vehicle](decisions/DEC-field-vehicle-posture.md) | field posture c8b: 6 h twin DRIFTS, G4 INCONCLUSIVE; superseded by the float vehicle | superseded | 2026-09-14 |

Standing documents (not split this pass):

- [SPEC_oracle_pin.md](SPEC_oracle_pin.md)

### `network-vehicle` — [topics/network-vehicle.md](topics/network-vehicle.md)

| card | one line | status | last dated |
| --- | --- | --- | --- |
| [Card 1b](cards/network-vehicle/CARD-1B-JOINT-WHITENING.md) | coupling-row joint whitening: premise FALSIFIED, the toggle byte-identical, knob retired | closed | 2026-07-10 |
| [datum wander](cards/network-vehicle/DATUM-WANDER.md) | the common clock level follows the datum station; sum-to-zero PASSES, a 25° mask REJECTED | closed | 2026-08-11 |
| [chain-pin audit](cards/network-vehicle/CHAIN-PIN-AUDIT.md) | eq34 chain-pin audit: COMPLETE, rungs 2/6 were SH-gate withholds (sim-only artifact) | closed | 2026-08-12 |
| [R0 + 1(d)](cards/network-vehicle/PIVOT-CARRY-REREF-R0-1D.md) | phase-anchor carry reref VALIDATED: churn 160.5 to 1.5 m (n = 1); armed in the goldens | closed | 2026-08-14 |
| [Card 1d](cards/network-vehicle/CARD-1D-COUPLING-CARRY.md) | scalar κ border time update: BUILT, armed; card CTU's exact update now builds the rows | closed | 2026-08-15 |
| [S-basis conformance](cards/network-vehicle/SBASIS-CONFORMANCE-F.md) | user-side Odijk audit: invariant via differencing; network charter BUILT, P0/P1 still open | reference | 2026-08-15 |
| [S-basis contract](cards/network-vehicle/SBASIS-CONTRACT.md) | realized S-basis, wire meaning, hold-or-mark contract (2026-08); GF datum residues open | reference | 2026-08-15 |
| [S-basis theory](cards/network-vehicle/SBASIS-THEORY-E.md) | S-system canon: re-datuming moves no estimable wander; the Eq-34 gate stays binding | reference | 2026-08-15 |
| [chain-slant](cards/network-vehicle/CHAIN-SLANT-ODIJK.md) | Odijk rank arithmetic and the chain caveat: n-invariant near-null, posture C the goal | closed | 2026-08-15 |
| [pivot gauge defect](cards/network-vehicle/PIVOT-GAUGE-DEFECT.md) | single-pivot phase-bias gauge defect PROVEN; clean election fixes it, third root named | closed | 2026-08-15 |
| [queue 1(b)](cards/network-vehicle/HANDOVER-STRANSFORM-SPEC.md) | clock-pivot handover S-transform spec; built and PROMOTED 2026-09-12 under PIVOT DEPARTURE | closed | 2026-08-15 |
| [eq34 D4](cards/network-vehicle/EQ34-D4-S-TRANSFORM-FIX.md) | eq34 D4 carry shift now re-bases coupling s_prev: FIXED 2026-08-17, no live lineage hit | closed | 2026-08-17 |
| [Cards 1f + 1g](cards/network-vehicle/CARD-1F-1G-TRUNCATED-ROW-GAUGE.md) | truncated-row gauge + reinit widening: PASS to 305 sta, withholds to 0, wire −29 % | closed | 2026-08-19 |
| [queue item 2](cards/network-vehicle/PIVOT-ELECTION-QUEUE2.md) | datum election landed as tooling; handover S-transform deferred, target near-empty | closed | 2026-08-21 |
| [wind-up sign](cards/network-vehicle/WINDUP-SIGN-DEFECT.md) | wind-up sign defect FIXED at four sites; a half-fix is worse, all sim validations blind | closed | 2026-08-21 |
| [wind-up field](cards/network-vehicle/WINDUP-FIELD-LADDER.md) | wind-up field ladder: slope near 2 in 9/9 components, null rejected; network arm consistent | closed | 2026-08-22 |
| [R2 sticky](cards/network-vehicle/PIVOT-R2-STICKY.md) | sticky continuous target VALIDATED: churn dead, consumer 2x; superseded by RF-2 held basis | superseded | 2026-08-31 |
| [READMIT](cards/network-vehicle/READMIT-EDGE.md) | re-admission edge: seed horizon PROMOTED (T3 tails cut); window, ratio, age gates KEEP_OFF | closed | 2026-09-06 |
| [F3 field](cards/network-vehicle/F3-FIELD-ARM.md) | field F3 ladder pre-registration: control DD ambiguities wander ~10 cyc/arc; P2 FALSIFIED | closed | 2026-09-09 |
| [F3 field (results)](cards/network-vehicle/F3-FIELD-ARM-RESULTS.md) | field F3 KEEP OFF (P2 FALSIFIED, WL12 PARTIAL); the converging leveling posture found | closed | 2026-09-09 |
| [COLD-FB (iter 2–4)](cards/network-vehicle/COLD-FB-iterations-2-4.md) | satellite clause removes every split; KEEP_OFF, de-biased consumer misses golden by 4 mm | superseded | 2026-09-11 |
| [COLD-FB](cards/network-vehicle/COLD-FB-anchor-fallbacks.md) | candidacy screen KEEP_OFF: settled cold handovers 51 → 19, consumer p50 +49 % vs cbscope5 | superseded | 2026-09-12 |
| [PIVOT DEPARTURE](cards/network-vehicle/PIVOT-DEPARTURE.md) | pivot-handover shock is carry free-fall: scoped response and clock S-transform PROMOTED | closed | 2026-09-12 |
| [D5 v5](cards/network-vehicle/D5-V5-carried-only.md) | carried-only D5 gate KEEP_OFF: born-chord flags gone, same-realization consumer +39 % | superseded | 2026-09-13 |
| [CE](cards/network-vehicle/CE-chain-era-bisect.md) | chain-era bisect: c0 = PV-A byte-identical, only the a priori clock pin (c3) moves code | closed | 2026-09-14 |
| [CM](cards/network-vehicle/CM-code-model-consistency.md) | eleven cold arms clear every estimation knob; the station block is near-rank-deficient | closed | 2026-09-14 |
| [CM2](cards/network-vehicle/CM2-leveling-vehicle.md) | the second defect is not a knob: no arm crosses or halves a bar on the leveling vehicle | closed | 2026-09-14 |
| [PV](cards/network-vehicle/PV-vehicle-test.md) | vehicle test: leveling beats carry_invariant on every read, all three bars MISSED | closed | 2026-09-14 |
| [ZP](cards/network-vehicle/ZP-station-block-parameterization.md) | station block singular under carry_invariant (58/61); ZWD prior RETIRED; leveling vehicle | closed | — |

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

Program card on this topic (indexed in [refound/CARDS.md](refound/CARDS.md)):
[OTL](refound/OTL_ocean_tide_loading_source_2026-10-16.md), ocean tide loading from the SIF registry alone and the
nodal corrections, gated against IERS HARDISP (open, 2026-10-17: built default off and checked, identical off, consumer +6.5 mm, census median -2.0 mm; diagnosed, no sign defect, diurnal net translation removable behind a default-off key, consumer +2.0 mm; arming open).

Program card on this topic, also indexed in [refound/CARDS.md](refound/CARDS.md):
[NSD](refound/NSD_network_small_defects_2026-10-17.md), three small network defects (open, 2026-10-17: the hard clock refloat counted by three response sites; the stash-status mirrors' skipped readmit seeds confirmed, counted and taken behind the default-off `network.readmit_seed_mirrors`; the coupled-mark census median per constellation; same-build check 2026-10-18: declared lines only; the 6 h seed counts owed).

[WGB](refound/WGB_windup_gap_bridge_2026-10-17.md), the phase wind-up accumulator through data gaps inside the leveling-carry horizon (open, 2026-10-18: built default off, UNVALIDATED, never armed; the simulator cannot grade it; checked: knob off identical to main, field golden 6 h restarts 139 to 91 and arcs refloated 116 to 23, consumer and census within 0.3 mm, no benefit shown).

[PIV](refound/PIV_pivot_election_coverage_2026-10-17.md), the clock pivot elected by predicted station coverage, broadcast
health and shadow, at ignition and at a forced handover (open, 2026-10-18: built default off, never armed; unit gate and
Release smoke in; checked 2026-10-18 against same-exe controls on the field and sim goldens: knob absent identical except the declared lines, no handover in 6 h against two in each control, field hour-2 common drift 0.051 against 0.192 m/h, consumer and census unchanged at the median, clock product within 2.4 mm).

[BDS](refound/BDS_beidou_third_constellation_2026-10-18.md), BeiDou on B1I and B3I as the third constellation of the field
golden, against GPS only and GPS + Galileo on one exe (open, 2026-10-18: read, healthy, no failure mode A or B, no piece needed;
single-band stations dropped whole, removed in arm GEC-s; census hours 2 to 6 -0.0038 m against GPS + Galileo, -0.0267 m against
GPS only; nothing armed; the single-band rule is built behind `network.second_band_admission`, UNVALIDATED; 2026-10-19: key off identical,
key on at 6 h it reproduces GEC-s).
[BDSB](refound/BDSB_beidou_continued_2026-10-19.md), the BDS card's continuation (open, 2026-10-19: the identity gate and the 6 h grade repeated
from fresh arms, key off 24 IDENTICAL, key on reproduces GEC-s, census median change against GEC-s 0.0000 m).
[SNX](refound/SNX_sinex_station_coordinates_2026-10-19.md), the IGS weekly SINEX of week 2313 against the coordinates the field runs hold (open, 2026-10-19: median 3D 6.0 mm over 71 stations, 20 above 1 cm, 4 above 5 cm, MIZU00JPN 1.68 m, WTZZ00DEU 4.5 mm; frame and marker reduction not the cause; the Asian Up bias, MAJU00MHL and POHN00FSM not explained by it; a default-off coordinate source planned, nothing built).
[PLANT](refound/PLANT_planted_coordinate_offsets_2026-10-19.md), the held coordinate of one sim station planted wrong by 1 cm to 1 km, East and Up (closed, 2026-10-19: measurement only; up to 10 m the wire moves 28.5 mm of satellite-clock RMS per metre East and 6.2 per metre Up and the consumer up to 3.4 mm per metre, no tier acts below 1 km; the congruence screen names the station at 10 m from the first sweep and at 1 m only after hours; at 1 km Up the gross-LOS screen declares the station alone at epoch 1, at 1 km East it declares 47 other stations and the consumer ends 335 m off; the existing station tier clears it only after the 120-epoch horizon; nothing built, nothing armed).

### `resume-seam` — [topics/resume-seam.md](topics/resume-seam.md)

| card | one line | status | last dated |
| --- | --- | --- | --- |
| [§18.10/§18.11](cards/resume-seam/RESUME-FRAME-RESTORE.md) | frame restore closes the phase side; 33 of 52 clocks trip, KEEP OFF until §18.14 | closed | 2026-09-03 |
| [§18.12](cards/resume-seam/RESUME-COUPLING-RESTORE.md) | coupling-regressor restore: wire about 10× better, consumer misses the h12 stop rule | closed | 2026-09-05 |
| [§18.13/§18.14](cards/resume-seam/RESUME-DENSE-BORDER-E2.md) | leg D misses the consumer bar; leg E (dense border) PASSES, resume knobs RECOMMEND_SCOPED | closed | 2026-09-05 |
| [RS-2](cards/resume-seam/RS-2-writer-frame-rebase.md) | the DD blob is the field seam defect; re-base exact, NECESSARY NOT SUFFICIENT; D5 hold K=1 | closed | 2026-09-12 |
| [RS-3 … RS-5](cards/resume-seam/RS-3-to-RS-5-datum-and-emit-layer.md) | both datums continue; the 17.7 m PB step is the cold emit layer; RS-5 closes the PB wire | closed | 2026-09-13 |
| [RS-6a](cards/resume-seam/RS-6a-station-layer-memories.md) | station-layer restores: T5 removes the a₀₀ seam transient, rx-CB modest; most bars FAIL | closed | 2026-09-13 |
| [RS-7 … RS-9](cards/resume-seam/RS-7-to-RS-9-border-and-rxcb-frame.md) | dense border KEEP_OFF (not the residual); rx-CB frame restored, withheld 26.3 % to 9.8 % | closed | 2026-09-13 |
| [RS-8 … RS-10b](cards/resume-seam/RS-8-to-RS-10b-latch-and-close-out.md) | latch restore: seam chord births 64 to 1; ZWD reseed OFF without a ZWD carry; v8 seeds | closed | 2026-09-14 |

Standing documents (not split this pass):

- [HANDOFF_seam_frame_restore_2026-09-03.md](HANDOFF_seam_frame_restore_2026-09-03.md)
- [SPEC_warm_resume_event_card.md](SPEC_warm_resume_event_card.md)

### `clock` — [topics/clock.md](topics/clock.md)

| card | one line | status | last dated |
| --- | --- | --- | --- |
| [Q3 ladder](cards/clock/Q3-FLOOR-GOVERNED-LADDER.md) | q ladder to 1e-7: producer gates monotone, trip collapse is the denominator, no promotion | closed | 2026-08-10 |
| [Q3 results](cards/clock/Q3-ANCHOR-RESULTS.md) | G0 to G2 PASS (σ_w −40 %), knee at q 1.0 to 1.5e-5; FC-1 correlation gate NOT met | closed | 2026-08-10 |
| [Q3 floor](cards/clock/Q3-PHYSICAL-FLOOR.md) | physical clock floor: q_phys 2.9e-8 m2/s under a 2.7 mm floor, FC-1 gate retired | closed | 2026-08-11 |
| [field wander probe](cards/clock/FIELD-WANDER-PROBE.md) | field wire wander is estimator-internal: not receiver code, not the a-priori clock frame | closed | 2026-08-11 |
| [Q3 design](cards/clock/Q3-ANCHOR-DESIGN.md) | guarded temporal clock anchor design: binding carry, residual guard, gates G0 to G4 | closed | 2026-08-15 |
| [Q3 G4](cards/clock/Q3-G4-SIM-HALF.md) | sim G4 half: anchor NET-POSITIVE (consumer -23 %), cb error real; q 3e-6 CLOSED on sim | closed | 2026-09-01 |
| [CS](cards/clock/CS-clock-step-signalling.md) | clock step test VALIDATED: 0 unsignalled steps > 2 m, consumer 55.8 → 2.7 m at the event | closed | 2026-09-14 |
| [Q3-F](cards/clock/Q3-F-self-contained-time-link.md) | anchor at the physical floor (q 2.9e-8) PASSES the code bars; in the re-founding posture | closed | 2026-09-14 |

Standing documents (not split this pass):

- [CLOCK_HADAMARD_CALIB_2026-07-14.md](CLOCK_HADAMARD_CALIB_2026-07-14.md)
- [SPEC_c1_clock_rate_validation.md](SPEC_c1_clock_rate_validation.md)
- [SPEC_clock_emit_rebase.md](SPEC_clock_emit_rebase.md)
- [SPEC_fast_clock_split_cadence.md](SPEC_fast_clock_split_cadence.md)

### `ionosphere` — [topics/ionosphere.md](topics/ionosphere.md)

| card | one line | status | last dated |
| --- | --- | --- | --- |
| [EXT-CB defect 2](cards/ionosphere/LV-EXTCB-DEFECT2.md) | station-support artifact: ignition a₀₀ − GIM −1.98 TECU at 303 stations vs −15.83 at 61 | closed | 2026-08-05 |
| [LV correction](cards/ionosphere/LV-CORRECTION-AND-12H.md) | de-mean pins a frozen epoch-1 frame; 12 h LV stack ramps +15 to +21 TECU: do NOT arm | closed | 2026-08-05 |
| [LV item 1](cards/ionosphere/LV-ITEM1-FALSIFIED.md) | lv-4 alone failed stage 1 at obs mask 10; the FALSIFIED verdict retracted at obs mask 15 | superseded | 2026-08-05 |
| [LV mask study](cards/ionosphere/LV-MASK-STUDY.md) | mask study closes on obs mask 15 / iono cutoff 25 at both scales; lv-4 passes stage 2 | closed | 2026-08-05 |
| [LV residual](cards/ionosphere/LV-RESIDUAL-EXTRAPOLATION.md) | level residual not an extrapolation artifact; the rx_cb_carry_q axis is exhausted | closed | 2026-08-05 |
| [LV tracking pin](cards/ionosphere/LV-TRACKING-PIN.md) | tracking-pin fix FALSIFIED: rx_cb_carry_q 1e-9 to 1e-6 made the level ramp worse, sooner | closed | 2026-08-05 |
| [LV-4](cards/ionosphere/LV-4-FRAME-INVARIANT-SCREEN.md) | frame-invariant rx-CB step screen: correct fix; needs LV-5, costs +16 to +64 % phase rms | closed | 2026-08-05 |
| [LV-5](cards/ionosphere/LV-5-MU-WITHIN-EPOCH.md) | within-epoch μ: limit cycle gone (sd 15.53 → 4.69 TECU); max \|μ\| prediction MISSED | closed | 2026-08-05 |
| [LV roadmap](cards/ionosphere/LV-ROADMAP.md) | lv action list superseded: item 3 built as S2H, level authority moved to LV-6 | superseded | 2026-08-07 |
| [LV impl](cards/ionosphere/LV-IMPL-RECORD.md) | built LV-1 and LV-2; seed-gradient model FALSIFIED, LV-1 acts only via the rx-CB carry | closed | 2026-08-12 |
| [elev-cutoff A/B](cards/ionosphere/ELEV-CUTOFF-AB.md) | iono cutoff with truth: 15° beats 25° on every clock channel; the field verdict inverts | closed | 2026-08-12 |
| [iono-field agreement](cards/ionosphere/IONO-FIELD-AGREEMENT.md) | iono input coherent both-high (corr 0.981), broken low-high; 25° cutoff trims wander 8 % | closed | 2026-08-12 |
| [EXT-CB arms 1–36](cards/ionosphere/LV-EXTCB-CAMPAIGN.md) | level defect = receiver GF-bias convention offset + ignition transient; L2-ESCAPE closed | closed | 2026-08-15 |
| [LV](cards/ionosphere/LV-REF.md) | reference: lv ladder knob and verdict ledgers, section map, binding grading protocol | reference | 2026-08-21 |
| [AMB-IONO](cards/ionosphere/AMB-IONO-SPLIT.md) | closed: diagonal carry over-counts each LOS's iono per band (B 3.00); F3's block is now float_kf's structure | closed | 2026-09-04 |
| [F3 (pre-reg)](cards/ionosphere/AMB-IONO-F3-PREDICTIONS.md) | f3 pre-registration: F3 beat its 3 h line 2.7×, F1 intermediate, var floor FALSIFIED | closed | 2026-09-04 |
| [F3 24 h](cards/ionosphere/AMB-IONO-F3-24H.md) | settled quarter FALSIFIED as traced; the episodes were the DD trace's reference satellite | superseded | 2026-09-05 |
| [0910 afternoon](cards/ionosphere/AMB-IONO-0910-AFTERNOON.md) | level memory FAILS x4; fewer T4 erases FAIL the consumer at every K; continuity KEEP_OFF | closed | 2026-09-10 |
| [0910 morning](cards/ionosphere/AMB-IONO-0910-MORNING.md) | oracle repair FAILS, PB-gauge levelling FALSIFIED; the phase bias is a receiver's float | closed | 2026-09-10 |
| [0910 second half](cards/ionosphere/AMB-IONO-0910-SECOND-HALF.md) | eleven probes FAIL with one shape; emission continuity FALSIFIED; producer AR NO-GO | closed | 2026-09-10 |
| [AMB-IONO open](cards/ionosphere/AMB-IONO-OPEN-QUESTIONS.md) | closed: 45 items reviewed: 13 done, 10 falsified, 12 superseded, 7 moot, 2 dup; 1 owed (unread config keys) | closed | 2026-09-10 |
| [LV-SEED](cards/ionosphere/LV-SEED-per-los-level.md) | all three hypotheses refuted (arc start, iono, geometry); wander is the satellite clock | closed | 2026-09-14 |
| [LV-1](cards/ionosphere/LV-1-BIAS-FREE-SEEDING.md) | bias-free iono seeding: stated mechanism FALSIFIED (a solve no-op); kept as a diagnostic | closed | — |
| [LV-2](cards/ionosphere/LV-2-RXCB-DEMEAN.md) | rx-CB de-mean: do NOT arm, it re-pins the common mode to a frozen frame (+15 to +21 TECU) | closed | — |
| [LV-3](cards/ionosphere/LV-3-LEVEL-AUTHORITY.md) | level authority made explicit: contract is §30; broadcast a00 prior deliberately not built | closed | — |

Standing documents (not split this pass):

- [SPEC_deweight_release_hygiene.md](SPEC_deweight_release_hygiene.md)
- [SPEC_iono2_mechB_v3.md](SPEC_iono2_mechB_v3.md)
- [SPEC_iono_carry_8b.md](SPEC_iono_carry_8b.md)
- [SPEC_level_authority_internal.md](SPEC_level_authority_internal.md)
- [network_global_sh_iono_design_2026-06-29.md](network_global_sh_iono_design_2026-06-29.md)

### `code-bias` — [topics/code-bias.md](topics/code-bias.md)

| card | one line | status | last dated |
| --- | --- | --- | --- |
| [S2H](cards/code-bias/S2H-HARVEST.md) | rx-CB harvest de-mean: frame hygiene validated to 3 h, 12 h level bar MISSED, LV-6 next | closed | 2026-08-21 |

Standing documents (not split this pass):

- [SPEC_beidou_w6_datum.md](SPEC_beidou_w6_datum.md)
- [SPEC_cb_admission_card.md](SPEC_cb_admission_card.md)
- [SPEC_cb_carry_physical_q.md](SPEC_cb_carry_physical_q.md)
- [SPEC_rx_cb_carry.md](SPEC_rx_cb_carry.md)
- [glonass_enablement_plan.md](glonass_enablement_plan.md)

### `slip` — [topics/slip.md](topics/slip.md)

| card | one line | status | last dated |
| --- | --- | --- | --- |
| [slip-QC impl](cards/slip/SLIPQC-IMPL-2026-07-21.md) | slip-QC build of cards E/R/L/G: Card E MET, Card R PARTIAL (14 to 4), Card L premise false | closed | 2026-08-12 |
| [D5-A](cards/slip/D5A-RETAP-ATTRIBUTION.md) | the D5 re-tap instrument: 3.0 % raw-step coincidence, re-tap count a third overstated | closed | 2026-08-15 |
| [slip-QC follow-ups](cards/slip/SLIPQC-FOLLOWUPS.md) | slip-QC follow-ups: D5-A, H1/H2 landed; Card W never launched, OP untriggered, H3 deferred | closed | 2026-08-15 |
| [lane slip response](cards/slip/LANE-SLIP-RESPONSE.md) | lane-integer slip cost 100 % response-inflicted; both retap keeps FALSIFIED, KEEP_OFF | closed | 2026-08-22 |
| [T4 lane restore](cards/slip/T4-LANE-RESTORE-SPEC.md) | pend-and-restore after a T4 erase: specified, never built; superseded by RF-1's bounded drop | superseded | 2026-08-23 |
| [rung 3](cards/slip/LANE-SNAP-RUNG3.md) | seed-site lane snap deleted (exact solve no-op); observatory solve-inert; apply leg moot under RF-1; closed | closed | 2026-08-23 |
| [wind-up census](cards/slip/WINDUP-FRACTIONAL-CENSUS.md) | 20 gaps in 4 h between the wind-up and carry horizons; restart response ARMED by card WUP | closed | 2026-08-23 |
| [IF-TD ref gate](cards/slip/IFTD-REF-GATE.md) | the IF-TD ref gate is 100 % false on the sim bed; Card R takes acting refloats 546 to 0 | closed | 2026-09-08 |
| [Card G response](cards/slip/CARD-G-GAP-REENTRY-RELEVEL.md) | gap re-entry re-level: bridged arcs 88 to 0, consumer +56 %; KEEP_OFF; armed on float | closed | 2026-09-09 |
| [IF-TD ref gate (v2)](cards/slip/IFTD-REF-GATE-FOLLOWUP.md) | warm-up abstain removes 118 cold fires, elev_norm adds 2; both UNVALIDATED, fault arm owed | closed | 2026-09-09 |
| [slip-QC](cards/slip/SLIPQC-REF.md) | slip-QC hardening spec and status ledger: cards R/L/E/G, D5 lead closed, Card W never run | reference | 2026-09-09 |
| [Card ORACLE](cards/slip/CARD-ORACLE-RELEVEL.md) | sim-only oracle re-level: exact repair, every gate FAILS (T4 +77 %); KEEP_OFF diagnostic | closed | 2026-09-10 |
| [Study 3](cards/slip/STUDY-3-band-scoped-responses.md) | band-scoped slip responses: both knobs KEEP_OFF, the T4 erase scope alone collapses the WL | closed | 2026-09-10 |
| [IF-TD pairs](cards/slip/IFTD-PAIRS.md) | all IF-TD pairs per satellite: equal-count trap closed, F2 and F3 finish it; RECOMMEND_ON | closed | 2026-09-11 |
| [Study 4](cards/slip/STUDY-4-slip-dose.md) | slip dose: anchor-arc slips drive the cascade; anchor-slip handover RECOMMEND_ON | closed | 2026-09-11 |
| [Study 5](cards/slip/STUDY-5-detector-coverage-l5.md) | unflagged L5 slips dropped by the repair gate; `mw_evidence_refloat` RECOMMEND_ON | closed | 2026-09-11 |
| [Study 6](cards/slip/STUDY-6-common-band-scope.md) | common-band scope KEEP_OFF (consumer +3.3 cm); superseded by Study 7's RECOMMEND_ON | superseded | 2026-09-11 |
| [Study 7](cards/slip/STUDY-7-common-band-narrowing.md) | common-band scope + IF-TD on every pair RECOMMEND_ON together, sim p50 0.174 to 0.120 m | closed | 2026-09-11 |
| [REF-ARC](cards/slip/REF-ARC-CLAUSE.md) | ref-arc clause PROMOTED: an untested reference pair cannot exonerate; sim storm neutral | closed | 2026-09-13 |
| [REF-FAN](cards/slip/REF-FAN.md) | reference-slip fan-out: collateral refloats 6 to 0; armed on both goldens, inert at dual frequency; closed | closed | 2026-09-13 |
| [RULE F2](cards/slip/IFTD-F2-lli-shadow.md) | the LLI shadow: a pair holding a band flagged last epoch cannot exonerate; S1 storm closed | closed | — |
| [RULE F3](cards/slip/IFTD-F3-lli-epoch.md) | the LLI-epoch verdict: S3 residual 0.335 to 0.005 cyc; its anchor clause keeps references | closed | — |

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
| [DOY-129 defects](cards/raim-qc/DAY129-DEFECTS-ABC.md) | guarded leg: seam withhold CORRECT, a₀₀ the wrong statistic, one-epoch CB resume window | closed | 2026-07-28 |
| [DOY-129 phases](cards/raim-qc/DAY129-PHASES.md) | phase 0/1 results unrecorded; CB guard KEEP_OFF (pivot-only FALSIFIED), gate (iii) UNMET | closed | 2026-07-28 |
| [INC-4](cards/raim-qc/MEAS-ADMISSION-INC4-REENTRY.md) | re-entry vet: 16 of 17 onsets were re-entries; 200 m vet bound closes class B; default OFF | closed | 2026-08-02 |
| [Rider A](cards/raim-qc/MDB-RIDER-A.md) | network MDB: byte-identical; faults under 9.4 m hide in the median code row; gates OWED | closed | 2026-08-04 |
| [Rider B](cards/raim-qc/BLOCK-BAARDA-RIDER-B.md) | block-Baarda LOS census: BUILT, unit-gated, measure-only; every §12.8 field gate owed | closed | 2026-08-04 |
| [meas admission](cards/raim-qc/MEAS-ADMISSION.md) | admission screen: corrupt channels refused at birth, era-2 withhold gone; ARMED on goldens | closed | 2026-08-12 |
| [meas admission impl](cards/raim-qc/MEAS-ADMISSION-IMPL.md) | admission build: gates 1 to 4 PASS, BSHM births refused; 1b cannot pass; 5 to 7 ungraded | closed | 2026-08-12 |
| [DOY-129 anatomy](cards/raim-qc/DAY129-FRAME-ANATOMY.md) | pivot CB ratchet (+49 m, no event response), clock wander, datum-only disc counter | closed | 2026-08-15 |
| [Rider C](cards/raim-qc/CONGRUENCE-RIDER-C.md) | congruence screen: smoke PASS; detects held-coordinate errors, cannot measure them | closed | 2026-08-15 |
| [T3 null](cards/raim-qc/T3-NULL-DERIVATION.md) | scale-blind statistic; station strata cut 4.46 % to 0.97 %; redundancy fix FALSIFIED (sim) | closed | 2026-08-15 |
| [global σ̂₀ rider](cards/raim-qc/GLOBAL-SIGMA0-RIDER.md) | global σ̂₀ rider: measure-only, OFF identity and wiring PASS; triangle and cliff OWED | closed | 2026-08-15 |
| [prires census](cards/raim-qc/PRIOR-RESIDUAL-CENSUS.md) | prior-residual census: smoke gates PASS, power gates 3 to 6 ungraded; armed on goldens | closed | 2026-08-15 |
| [redundancy census](cards/raim-qc/REDUNDANCY-CENSUS.md) | testability census built and smoke-gated; the leg-19 known-answer gates stay OWED (§15) | closed | 2026-08-15 |
| [A4 withhold](cards/raim-qc/WITHHOLD-ERA-RELEASE.md) | withhold-era escalation: era 29 to 1, VALIDATED; G3 FAIL later a cross-build artifact | closed | 2026-08-21 |
| [T3 field](cards/raim-qc/T3-FIELD-AND-COLLATERALS.md) | field check: σ̂₀ floor-pinned, stratification 1.5×; detection gate PASSED; R2 + R4 BUILT | closed | 2026-08-21 |
| [withhold cliff](cards/raim-qc/WITHHOLD-CLIFF-RETIREMENT.md) | cliff is a timer, not therapy; G3 FAIL did NOT REPRODUCE; promotion waits on level health | closed | 2026-08-23 |
| [RNEG](cards/raim-qc/RNEG-REPARTITION.md) | r-negative re-partition: worst \|t\| 2.48e8 was a floor artifact; RNEG read in metres | closed | 2026-09-09 |
| [GL](cards/raim-qc/GL-gross-los-screen.md) | gross-LOS screen: tail 13 to 16× smaller but bars FAILED; completed and ARMED by RF-3 | closed | 2026-09-14 |
| [GL-2 + R8](cards/raim-qc/GL-2-whole-satellite-standdown.md) | stand-down FAILED (consumer 22.25 m in hour 4), left OFF; rx-CB carry exonerated | closed | 2026-09-14 |
| [DEC-day129-gate-policy](decisions/DEC-day129-gate-policy.md) | gate rulings (degrade, 120-epoch cap, rms_ho ratio) unbuilt; superseded, SH and seams out of the re-founding | superseded | — |
| [T3 mechanism](cards/raim-qc/T3-IONO-MECHANISM.md) | per-LOS ionosphere error drives the T3 heterogeneity (corr +0.926) and the DD integers | closed | — |

Program card on this topic (indexed in [refound/CARDS.md](refound/CARDS.md)):

- [BBE](refound/BBE_block_baarda_estimable_2026-10-18.md), the block Baarda test on the float vehicle (open, 2026-10-18: the whole-block test refused every block because a line of sight's free slant ionosphere absorbs one direction exactly; `network.block_baarda_estimable` built default off and never armed; knob off identical on the smokes, the 2 h arms and the same-build gates; the 2 h check found no flagged block block-only; a positive control is owed).

Standing documents (not split this pass):

- [SPEC_flicker_gap_integrity.md](SPEC_flicker_gap_integrity.md)
- [SPEC_prefit_census_observability.md](SPEC_prefit_census_observability.md)
- [SPEC_raim_pool_rehost.md](SPEC_raim_pool_rehost.md)

### `wire-contract` — [topics/wire-contract.md](topics/wire-contract.md)

| card | one line | status | last dated |
| --- | --- | --- | --- |
| [LV-3 contract](cards/wire-contract/LEVEL-CONTRACT.md) | level contract: W6 sum-to-zero or a00 GIM anchor owns the level; LV-1/2/4/5 SUPERSEDED | closed | 2026-08-15 |
| [level health](cards/wire-contract/LEVEL-HEALTH-CHANNEL.md) | level health: Phase A PASS, slides need lvlretro, Phase B unbuilt; superseded, SH dropped from the producer | superseded | 2026-08-23 |

Standing documents (not split this pass):

- [SPEC_product_cbpin_contract.md](SPEC_product_cbpin_contract.md)

### `consumer` — [topics/consumer.md](topics/consumer.md)

| card | one line | status | last dated |
| --- | --- | --- | --- |
| [CC](cards/consumer/CC-declared-step-carry.md) | declared-step carry exact, default OFF (re-grade fails 2 of 5 bars); warm start LANDED | closed | 2026-09-14 |
| [CI](cards/consumer/CI-consumer-instrument.md) | consumer sound (0.25 m on CODE), old wire 6 to 8 m; suspended bars moot; closed, re-founding grades cold 6 h | closed | 2026-09-14 |
| [CI census](cards/consumer/CI-census-wire-vs-code.md) | wire-vs-CODE census: W1 and W3 FAIL; the clock carries a metre-level code-model error | closed | 2026-09-14 |
| [consumer AR](cards/consumer/CONSUMER-AR-BLUNDER.md) | consumer WL-only blunder: a frame mismatch FFR misses, caught by the innovation screen | closed | — |

Program cards on this topic (indexed in [refound/CARDS.md](refound/CARDS.md)):

- [SEA](refound/SEA_seasonal_term_outside_window_2026-10-16.md), the seasonal term of a SIF coordinate
  outside its block windows (open, 2026-10-17: built without a knob; the same-build golden identity
  check passed; the merge is owed).
- [NIS](refound/NIS_consumer_innovation_screen_2026-10-17.md), the consumer's pre-update innovation screen (open, 2026-10-17: built default off, never armed; checked 2026-10-17: key absent identical on every gate, armed no output moved, no benefit shown).

### `sim-bed` — [topics/sim-bed.md](topics/sim-bed.md)

| card | one line | status | last dated |
| --- | --- | --- | --- |
| [G-A](cards/sim-bed/SIMNET-GA-NULL-TEST.md) | first absolute grade: n = 3 not determinable (ZWD +4.31 m), all internal instruments clean | closed | 2026-08-11 |
| [G-A 50-sta](cards/sim-bed/SIMNET-50STA-QC-TIERS.md) | 50-sta QC tiers: 4 of 7 live, datum-station slip blind spot fixed, MW mid-arc silence open | closed | 2026-08-11 |
| [W6+SH arm](cards/sim-bed/SIMNET-W6SH-ARM.md) | the two-band wall is one n-invariant L2W phase-bias near-null; code-bias vf ~4.9e+05 | closed | 2026-08-12 |
| [W6+SH brief](cards/sim-bed/SIMNET-W6SH-BRIEF.md) | design brief for the W6+SH sim arm: item 1 FALSIFIED, item 3 MOOT, the rest implemented | superseded | 2026-08-12 |
| [G-B](cards/sim-bed/SIMNET-GB-CLOCK-STEP.md) | clock-step floor RETRACTED: 92.4 to 92.8 % recovered against the null arm, no floor | closed | 2026-08-15 |
| [SIMNET](cards/sim-bed/SIMNET-REF.md) | sim truth-bed design: per-station processes, truth in the producer basis, trap register | reference | 2026-08-15 |
| [injection bench](cards/sim-bed/SIM-INJECTION-BENCH.md) | slip-injection bench: 22/24 attributed, MDB 2 cycles; null arm found the gauge defect | closed | 2026-08-15 |
| [60-sta campaign](cards/sim-bed/SIMNET-60STA-CAMPAIGN.md) | 60-sta slipped grade: detection 94.1 %, but refloat re-levels walk the levels to metres | closed | 2026-08-20 |
| [dashboard](cards/sim-bed/SIMNET-DASHBOARD.md) | simnet dashboard built; 12 h quiet arms: S0 / BCA not promoted, carry_zwd KEEP_OFF | closed | 2026-09-01 |
| [stability](cards/sim-bed/STABILITY-CAMPAIGN.md) | late drift is eclipse-valve edge response, not aging; sticky_pivot cuts sim consumer 74 % | closed | 2026-09-02 |
| [DD-REF](cards/sim-bed/DD-TRACE-REF-SAT.md) | post-eclipse DD episode was the trace's reference satellite; the field histogram is not | closed | 2026-09-06 |
| [slant + triple](cards/sim-bed/SIMNET-SLANT-TRIPLE.md) | third band closes the wall; slant cuts cb vf ~16× uncalibrated; settled EWL trio DIVERGES | closed | 2026-09-09 |
| [ZWD sink](cards/sim-bed/ZWD-SINK-CENSUS.md) | slipped-bed golden FAIL x3; carry_zwd_in_block RECOMMEND_SCOPED; consumer AR 0/2879 fixes | closed | 2026-09-10 |

### `perf` — [topics/perf.md](topics/perf.md)

_No split cards yet — the topic page and the standing documents below are the whole of it._

Standing documents (not split this pass):

- [SPEC_carry_factored_pz_C2.md](SPEC_carry_factored_pz_C2.md)
- [SPEC_dev_tier_perf_cards.md](SPEC_dev_tier_perf_cards.md)
- [SPEC_inc8b_election_split.md](SPEC_inc8b_election_split.md)
- [network_decomposition_plan.md](network_decomposition_plan.md)

Program card on this topic (indexed in [refound/CARDS.md](refound/CARDS.md)):
[NETCOST](refound/NETCOST_gps_galileo_cost_attribution_2026-10-18.md), where the GPS + Galileo epoch time goes and the exact sparse station
normals (open, 2026-10-18: loop 5.3 and 3.7 times the GPS-only loop; the sparse formation behind a default-off key
shortens the field loop 13.3 % and the sim loop 10.6 % at identical output; arming and two further optimizations owed).

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

Program card on this topic (indexed in [refound/CARDS.md](refound/CARDS.md)):
[RKD](refound/RKD_retired_key_deletion_2026-10-14.md), the 144 retired network keys deleted with
their code (closed, 2026-10-14: landed; the same-build golden comparison passed).

### `queues` — [topics/queues.md](topics/queues.md)

| card | one line | status | last dated |
| --- | --- | --- | --- |
| [QPR blocked](cards/queues/QPR-BLOCKED-AND-CLOSED.md) | blocked/leave-alone: churn fixed by 1(d), D5 retired by RF-1, trio runaway to L5B; verdicts stand | superseded | 2026-08-14 |
| [QPR](cards/queues/QPR-00-CHARTER.md) | queue charter: SUPERSEDED by the 2026-09-14 re-founding; two leftovers re-filed to OPEN_ITEMS | superseded | 2026-08-21 |
| [QPR 0–3](cards/queues/QPR-ITEMS-0-3.md) | items 0–3: 1(a), 1(b), 1(d), 1f, 1g, clock marking armed in goldens; item 2 tooling; rest moot | closed | 2026-08-21 |
| [QPR 4](cards/queues/QPR-ITEM-4-T3-NULLS.md) | T3 null: excess is per-LOS iono error; R2/R4 built measure-only; rest superseded by RF-3 code-row QC | superseded | 2026-08-21 |
| [QPR 5–8](cards/queues/QPR-ITEMS-5-8.md) | items 5–8: 8(b) falsified; 5, 7, 8 superseded by RF-0c, RF-1, no SH; item 6 built and checked (millisecond-grid start-offset seek, leap table; 2 h golden arms byte-identical) | superseded | 2026-10-17 |
| [CAF](cards/queues/CAF-COMMENT-AUDIT-FINDINGS.md) | comment audit: 5 defects fixed; Card-4, lane and stand-down rows moot; 20 owed lines re-filed to OPEN_ITEMS | closed | 2026-09-28 |

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
- [status/OPEN_ITEMS_CODE_DEFECTS.md](status/OPEN_ITEMS_CODE_DEFECTS.md) — the code-defect
  section of the register (verified defects, their fixes, the gate records), split out for size.

---

## The frozen monoliths

Eighteen files over 60 KB were split on 2026-09-14. The original of each is kept
byte-for-byte in `docs/archive/` and is never appended to; the old path now holds a
routing stub whose table resolves any `§N.N` citation in one hop, which is what keeps
the several hundred `// … (SPEC_x §N.N): …` comments in `src/` and `include/` working.

**A nineteenth file over 60 KB was not split:**
[CONFIG_promotion_ledger.md](CONFIG_promotion_ledger.md). Since 2026-10-13 it is the
view of `CONFIG_promotion_ledger.json` (one verdict per config key), rendered by
`scripts/ledger_render.py`, with a named size exemption in
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
